#include "persistence/binary_io.h"
#include "persistence/wal.h"

#include <cstdint>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace kv {
namespace persistence {

namespace {

using LengthType = std::uint32_t;
using ChecksumType = std::uint32_t;
using SizeType = binary_io::SizeType;
using OpType = std::uint8_t;

// Bound individual records so corrupt lengths cannot force unbounded memory
// allocation during replay.
constexpr std::size_t kMaxRecordLength = 64U * 1024U * 1024U;
constexpr std::size_t kMaxBatchBytes = kMaxRecordLength + 8;

void sync_parent_directory(const std::string& path) {
  const auto parent = std::filesystem::path(path).parent_path();
  const std::string directory = parent.empty() ? "." : parent.string();
#ifdef O_DIRECTORY
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
  const int fd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
#endif
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "open WAL directory");
  }
  const int result = ::fsync(fd);
  const int error = errno;
  ::close(fd);
  if (result < 0) {
    throw std::system_error(error, std::generic_category(), "sync WAL directory");
  }
}

enum class WalOp : OpType {
  Set = 1,
  Delete = 2,
};

struct ParsedRecord {
  WalOp op = WalOp::Set;
  std::string key;
  std::string value;
};

template <typename T>
void append_primitive(std::string& bytes, T value) {
  const char* raw = reinterpret_cast<const char*>(&value);
  bytes.append(raw, sizeof(T));
}

std::uint32_t crc32(const std::string& bytes) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

LengthType checked_record_length(std::size_t record_length) {
  // Keep writer and reader limits aligned: if we will not replay it, do not
  // write it.
  if (record_length > kMaxRecordLength) {
    throw std::runtime_error("WAL record is too large");
  }
  return static_cast<LengthType>(record_length);
}

bool read_exact(std::ifstream& input, char* data, std::size_t size) {
  input.read(data, static_cast<std::streamsize>(size));
  return static_cast<bool>(input);
}

template <typename T>
bool read_framing_primitive(std::ifstream& input,
                            std::uint64_t& cursor,
                            T& value,
                            WalReplayResult& result) {
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  const std::streamsize bytes_read = input.gcount();
  if (bytes_read == 0 && input.eof()) {
    result.status = WalReplayStatus::CleanEof;
    result.stop_offset = cursor;
    return false;
  }

  if (bytes_read != static_cast<std::streamsize>(sizeof(value))) {
    result.status = WalReplayStatus::PartialRecord;
    result.stop_offset = cursor;
    return false;
  }

  cursor += sizeof(value);
  return true;
}

WalReplayStatus parse_record(const std::string& record,
                             ParsedRecord& parsed) {
  std::size_t offset = 0;

  // All record variants start with an opcode and a key.

  // Operation
  OpType op = 0;
  if (!binary_io::ConsumePrimitive(record, offset, op)) {
    return WalReplayStatus::PartialPayload;
  }

  if (op != static_cast<OpType>(WalOp::Set) &&
      op != static_cast<OpType>(WalOp::Delete)) {
    return WalReplayStatus::InvalidOpcode;
  }

  // Key Size
  SizeType key_size = 0;
  if (!binary_io::ConsumePrimitive(record, offset, key_size)) {
    return WalReplayStatus::PartialPayload;
  }

  // Key itself
  std::string key;
  if (!binary_io::ConsumeBytes(record, offset, key_size, key)) {
    return WalReplayStatus::PartialPayload;
  }

  if (op == static_cast<OpType>(WalOp::Set)) {
    // SET records carry exactly one value after the key. Extra trailing bytes
    // make the record malformed.
    SizeType value_size = 0;
    if (!binary_io::ConsumePrimitive(record, offset, value_size)) {
      return WalReplayStatus::PartialPayload;
    }

    std::string value;
    if (!binary_io::ConsumeBytes(record, offset, value_size, value)) {
      return WalReplayStatus::PartialPayload;
    }

    if (offset != record.size()) {
      return WalReplayStatus::InvalidPayload;
    }

    parsed.op = WalOp::Set;
    parsed.key = std::move(key);
    parsed.value = std::move(value);
    return WalReplayStatus::CleanEof;
  }

  // DELETE records end immediately after the key.
  if (offset != record.size()) {
    return WalReplayStatus::InvalidPayload;
  }

  parsed.op = WalOp::Delete;
  parsed.key = std::move(key);
  return WalReplayStatus::CleanEof;
}

void apply_record(const ParsedRecord& record,
                  std::unordered_map<std::string, std::string>& store) {
  if (record.op == WalOp::Set) {
    store[record.key] = record.value;
    return;
  }

  store.erase(record.key);
}

}  // namespace

WriteAheadLog::WriteAheadLog(std::string path, DurabilityPolicy policy,
                             std::size_t max_batch, std::uint32_t max_delay_us)
    : path_(std::move(path)),
      output_(path_, std::ios::binary | std::ios::app),
      policy_(policy), max_batch_(max_batch), max_delay_us_(max_delay_us) {
  if (!output_.is_open()) {
    throw std::runtime_error("failed to open WAL file: " + path_);
  }
  if (max_batch_ == 0 || max_batch_ > 4096) {
    throw std::invalid_argument("WAL max_batch must be in [1, 4096]");
  }
  stats_.batch_histogram.resize(max_batch_ + 1);
  if (policy_ != DurabilityPolicy::Buffered) {
    sync_fd_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
    if (sync_fd_ < 0) {
      throw std::system_error(errno, std::generic_category(), "open WAL sync fd");
    }
    try {
      sync_parent_directory(path_);
    } catch (...) {
      ::close(sync_fd_);
      sync_fd_ = -1;
      throw;
    }
  }
  if (policy_ == DurabilityPolicy::GroupCommit) {
    writer_ = std::thread(&WriteAheadLog::WriterLoop, this);
  }
}

WriteAheadLog::~WriteAheadLog() {
  if (writer_.joinable()) {
    {
      std::lock_guard lock(queue_mutex_);
      stopping_ = true;
    }
    queue_ready_.notify_one();
    writer_.join();
  }
  if (sync_fd_ >= 0) ::close(sync_fd_);
}

void WriteAheadLog::SyncLocked() {
  if (sync_fd_ < 0) return;
  int result;
  do {
#ifdef __linux__
    result = ::fdatasync(sync_fd_);
#else
    result = ::fsync(sync_fd_);
#endif
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    throw std::system_error(errno, std::generic_category(), "fdatasync WAL");
  }
  ++stats_.sync_calls;
}

void WriteAheadLog::WriteFrameLocked(const std::string& frame) {
  output_.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  output_.flush();
  if (!output_) throw std::runtime_error("failed to write WAL frame");
  ++stats_.records;
}

void WriteAheadLog::AppendFrame(std::string frame) {
  if (policy_ != DurabilityPolicy::GroupCommit) {
    std::lock_guard lock(io_mutex_);
    if (failure_) std::rethrow_exception(failure_);
    try {
      WriteFrameLocked(frame);
      if (policy_ == DurabilityPolicy::Sync) {
        SyncLocked();
        ++stats_.batch_histogram[1];
      }
    } catch (...) {
      failure_ = std::current_exception();
      throw;
    }
    return;
  }

  auto pending = std::make_shared<Pending>();
  pending->frame = std::move(frame);
  {
    std::lock_guard lock(queue_mutex_);
    if (failure_) std::rethrow_exception(failure_);
    queue_.push_back(pending);
  }
  queue_ready_.notify_one();
  std::unique_lock lock(pending->mutex);
  pending->done.wait(lock, [&] { return pending->complete; });
  if (pending->error) std::rethrow_exception(pending->error);
}

void WriteAheadLog::WriterLoop() {
  while (true) {
    std::vector<std::shared_ptr<Pending>> batch;
    {
      std::unique_lock lock(queue_mutex_);
      queue_ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (queue_.empty() && stopping_) return;
      if (queue_.size() < max_batch_ && max_delay_us_ > 0 && !stopping_) {
        queue_ready_.wait_for(lock, std::chrono::microseconds(max_delay_us_),
                              [&] { return stopping_ || queue_.size() >= max_batch_; });
      }
      std::size_t batch_bytes = 0;
      while (!queue_.empty() && batch.size() < max_batch_) {
        const std::size_t next_bytes = queue_.front()->frame.size();
        if (!batch.empty() && batch_bytes + next_bytes > kMaxBatchBytes) {
          break;
        }
        batch_bytes += next_bytes;
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
      }
    }

    std::exception_ptr error;
    try {
      std::lock_guard lock(io_mutex_);
      std::size_t bytes = 0;
      for (const auto& request : batch) bytes += request->frame.size();
      std::string frames;
      frames.reserve(bytes);
      for (const auto& request : batch) frames.append(request->frame);
      output_.write(frames.data(), static_cast<std::streamsize>(frames.size()));
      output_.flush();
      if (!output_) throw std::runtime_error("failed to write WAL batch");
      stats_.records += batch.size();
      SyncLocked();
      ++stats_.batch_histogram[batch.size()];
    } catch (...) {
      error = std::current_exception();
      {
        std::lock_guard lock(queue_mutex_);
        failure_ = error;
        while (!queue_.empty()) {
          batch.push_back(std::move(queue_.front()));
          queue_.pop_front();
        }
      }
    }
    for (const auto& request : batch) {
      {
        std::lock_guard lock(request->mutex);
        request->error = error;
        request->complete = true;
      }
      request->done.notify_one();
    }
    if (error) return;
  }
}

WalStats WriteAheadLog::GetStats() const {
  std::lock_guard lock(io_mutex_);
  return stats_;
}

void WriteAheadLog::ResetStats() {
  std::lock_guard lock(io_mutex_);
  stats_ = {};
  stats_.batch_histogram.resize(max_batch_ + 1);
}

void WriteAheadLog::PinWriterToCpu(int cpu) {
  if (policy_ != DurabilityPolicy::GroupCommit) return;
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  const int error = pthread_setaffinity_np(writer_.native_handle(), sizeof(set), &set);
  if (error != 0) {
    throw std::system_error(error, std::generic_category(), "pin WAL writer");
  }
#else
  (void)cpu;
  throw std::runtime_error("WAL writer affinity requires Linux");
#endif
}

void WriteAheadLog::AppendSet(const std::string& key, const std::string& value) {
  const OpType op = static_cast<OpType>(WalOp::Set);
  const SizeType key_size = binary_io::CheckedSize(key, "WAL key");
  const SizeType value_size = binary_io::CheckedSize(value, "WAL value");
  // Length covers the payload after the length field itself:
  // [op][key_size][key][value_size][value].
  const LengthType record_length =
      checked_record_length(sizeof(op) + sizeof(key_size) + key_size +
                            sizeof(value_size) + value_size);

  std::string payload;
  append_primitive(payload, op);
  append_primitive(payload, key_size);
  payload.append(key);
  append_primitive(payload, value_size);
  payload.append(value);
  const ChecksumType checksum = crc32(payload);

  std::string frame;
  append_primitive(frame, record_length);
  append_primitive(frame, checksum);
  frame.append(payload);
  AppendFrame(std::move(frame));
}

void WriteAheadLog::AppendDelete(const std::string& key) {
  const OpType op = static_cast<OpType>(WalOp::Delete);
  const SizeType key_size = binary_io::CheckedSize(key, "WAL key");
  // Length covers the payload after the length field itself:
  // [op][key_size][key].
  const LengthType record_length =
      checked_record_length(sizeof(op) + sizeof(key_size) + key_size);

  std::string payload;
  append_primitive(payload, op);
  append_primitive(payload, key_size);
  payload.append(key);
  const ChecksumType checksum = crc32(payload);

  std::string frame;
  append_primitive(frame, record_length);
  append_primitive(frame, checksum);
  frame.append(payload);
  AppendFrame(std::move(frame));
}

std::uint64_t WriteAheadLog::CurrentOffset() {
  std::lock_guard lock(io_mutex_);
  output_.flush();
  if (!output_) {
    throw std::runtime_error("failed to flush WAL before reading offset");
  }

  output_.seekp(0, std::ios::end);
  if (!output_) {
    throw std::runtime_error("failed to seek WAL output stream");
  }

  const std::streampos position = output_.tellp();
  if (position == std::streampos(-1)) {
    throw std::runtime_error("failed to read WAL offset");
  }

  return static_cast<std::uint64_t>(position);
}

void WriteAheadLog::Clear() {
  std::lock_guard lock(io_mutex_);
  // The WAL keeps an append stream open for normal writes. Close and reopen it
  // around truncation so future SET/DELETE records continue using the same WAL
  // object after persistence has been cleared.
  output_.close();
  output_.clear();

  {
    std::ofstream truncated(path_, std::ios::binary | std::ios::trunc);
    if (!truncated.is_open()) {
      throw std::runtime_error("failed to truncate WAL file: " + path_);
    }

    truncated.flush();
    if (!truncated) {
      throw std::runtime_error("failed to clear WAL file: " + path_);
    }
  }

  output_.open(path_, std::ios::binary | std::ios::app);
  if (!output_.is_open()) {
    throw std::runtime_error("failed to reopen WAL file: " + path_);
  }
  if (policy_ != DurabilityPolicy::Buffered) SyncLocked();
}

void WriteAheadLog::Rotate() {
  Clear();
}

void WriteAheadLog::TruncateTo(std::uint64_t offset) {
  std::lock_guard lock(io_mutex_);
  output_.close();
  output_.clear();

  std::error_code error;
  const std::uintmax_t file_size = std::filesystem::exists(path_, error)
                                       ? std::filesystem::file_size(path_, error)
                                       : 0;
  if (error) {
    throw std::runtime_error("failed to inspect WAL file before truncation: " +
                             path_);
  }
  if (offset > file_size) {
    throw std::runtime_error("WAL truncation offset is past EOF");
  }

  std::filesystem::resize_file(path_, offset, error);
  if (error) {
    throw std::runtime_error("failed to truncate WAL file: " + path_);
  }

  output_.open(path_, std::ios::binary | std::ios::app);
  if (!output_.is_open()) {
    throw std::runtime_error("failed to reopen WAL file: " + path_);
  }
  if (policy_ != DurabilityPolicy::Buffered) SyncLocked();
}

std::size_t WriteAheadLog::Replay(
    std::unordered_map<std::string, std::string>& store) const {
  return ReplayFrom(0, store);
}

WalReplayResult WriteAheadLog::ReplayDetailed(
    std::unordered_map<std::string, std::string>& store) const {
  return ReplayFromDetailed(0, store);
}

std::size_t WriteAheadLog::ReplayFrom(
    std::uint64_t offset,
    std::unordered_map<std::string, std::string>& store) const {
  return ReplayFromDetailed(offset, store).applied_operations;
}

WalReplayResult WriteAheadLog::ReplayFromDetailed(
    std::uint64_t offset,
    std::unordered_map<std::string, std::string>& store) const {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max())) {
    throw std::runtime_error("WAL replay offset is too large");
  }

  WalReplayResult result;
  result.start_offset = offset;
  result.last_good_offset = offset;
  result.stop_offset = offset;

  std::ifstream input(path_, std::ios::binary);
  if (!input.is_open()) {
    return result;
  }

  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input) {
    return result;
  }

  std::uint64_t cursor = offset;

  while (true) {
    // Each iteration validates one WAL v2 frame:
    // [uint32 payload_length][uint32 crc32(payload)][payload].
    // A record mutates the store only after the full payload parses and its
    // checksum matches.
    const std::uint64_t record_start = cursor;
    LengthType record_length = 0;
    if (!read_framing_primitive(input, cursor, record_length, result)) {
      break;
    }

    if (record_length == 0 || record_length > kMaxRecordLength) {
      result.status = WalReplayStatus::InvalidLength;
      result.stop_offset = record_start;
      break;
    }

    ChecksumType expected_checksum = 0;
    if (!read_framing_primitive(input, cursor, expected_checksum, result)) {
      result.status = WalReplayStatus::PartialRecord;
      result.stop_offset = record_start;
      break;
    }

    std::string record(record_length, '\0');
    if (!read_exact(input, record.data(), record.size())) {
      result.status = WalReplayStatus::PartialRecord;
      result.stop_offset = record_start;
      break;
    }
    cursor += record.size();

    if (crc32(record) != expected_checksum) {
      result.status = WalReplayStatus::ChecksumMismatch;
      result.stop_offset = record_start;
      break;
    }

    ParsedRecord parsed;
    const WalReplayStatus parse_status = parse_record(record, parsed);
    if (parse_status != WalReplayStatus::CleanEof) {
      result.status = parse_status;
      result.stop_offset = record_start;
      break;
    }

    apply_record(parsed, store);
    ++result.applied_operations;
    result.last_good_offset = cursor;
    result.stop_offset = cursor;
  }

  return result;
}

WalReplayResult WriteAheadLog::ReplayFromAndTruncate(
    std::uint64_t offset,
    std::unordered_map<std::string, std::string>& store) {
  WalReplayResult result = ReplayFromDetailed(offset, store);
  if (result.status != WalReplayStatus::CleanEof) {
    TruncateTo(result.last_good_offset);
    result.truncated = true;
  }
  return result;
}

}  // namespace persistence
}  // namespace kv
