#include "persistence/binary_io.h"
#include "persistence/wal.h"

#include <array>
#include <cstdint>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
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
constexpr char kWalMagic[] = {'K', 'V', 'W', '3'};
constexpr std::uint32_t kWalVersion = 3;
constexpr std::uint32_t kEndianMarker = 0x01020304U;
constexpr std::size_t kHeaderLength = 24;

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

void append_le32(std::string& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<char>(value >> shift));
  }
}

void append_le64(std::string& bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    bytes.push_back(static_cast<char>(value >> shift));
  }
}

template <typename T>
bool consume_le(const std::string& bytes, std::size_t& offset, T& value) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
  value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<unsigned char>(bytes[offset + i]))
             << (8 * i);
  }
  offset += sizeof(T);
  return true;
}

constexpr std::array<std::uint32_t, 256> make_crc32_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    std::uint32_t entry = i;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (entry & 1U);
      entry = (entry >> 1U) ^ (0xEDB88320U & mask);
    }
    table[i] = entry;
  }
  return table;
}

constexpr auto kCrc32Table = make_crc32_table();

std::uint32_t crc32(std::string_view bytes) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const unsigned char byte : bytes) {
    crc = (crc >> 8U) ^ kCrc32Table[(crc ^ byte) & 0xFFU];
  }
  return ~crc;
}

std::string make_header(std::uint64_t generation) {
  std::string header(kWalMagic, sizeof(kWalMagic));
  append_le32(header, kWalVersion);
  append_le32(header, kEndianMarker);
  append_le64(header, generation);
  append_le32(header, crc32(header));
  return header;
}

bool parse_header(std::ifstream& input, std::uint64_t& generation) {
  input.clear();
  input.seekg(0, std::ios::beg);
  std::string header(kHeaderLength, '\0');
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  if (!input || std::memcmp(header.data(), kWalMagic, sizeof(kWalMagic)) != 0) {
    return false;
  }
  std::size_t offset = sizeof(kWalMagic);
  std::uint32_t version = 0, marker = 0, checksum = 0;
  if (!consume_le(header, offset, version) ||
      !consume_le(header, offset, marker) ||
      !consume_le(header, offset, generation) ||
      !consume_le(header, offset, checksum)) {
    return false;
  }
  return version == kWalVersion && marker == kEndianMarker &&
         generation != 0 &&
         checksum == crc32(header.substr(0, kHeaderLength - sizeof(checksum)));
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

bool read_framing_u32(std::ifstream& input,
                      std::uint64_t& cursor,
                      std::uint32_t& value,
                      WalReplayResult& result,
                      bool little_endian) {
  char bytes[sizeof(value)]{};
  input.read(bytes, sizeof(bytes));
  const std::streamsize bytes_read = input.gcount();
  if (bytes_read == 0 && input.eof()) {
    result.status = WalReplayStatus::CleanEof;
    result.stop_offset = cursor;
    return false;
  }

  if (bytes_read != static_cast<std::streamsize>(sizeof(bytes))) {
    result.status = WalReplayStatus::PartialRecord;
    result.stop_offset = cursor;
    return false;
  }

  if (little_endian) {
    value = 0;
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
      value |= static_cast<std::uint32_t>(
                   static_cast<unsigned char>(bytes[i])) << (8 * i);
    }
  } else {
    std::memcpy(&value, bytes, sizeof(value));
  }
  cursor += sizeof(bytes);
  return true;
}

WalReplayStatus parse_record(const std::string& record,
                             ParsedRecord& parsed,
                             bool portable,
                             std::uint64_t& sequence) {
  std::size_t offset = 0;

  if (portable && !consume_le(record, offset, sequence)) {
    return WalReplayStatus::PartialPayload;
  }

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
  if (!(portable ? consume_le(record, offset, key_size)
                 : binary_io::ConsumePrimitive(record, offset, key_size))) {
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
    if (!(portable ? consume_le(record, offset, value_size)
                   : binary_io::ConsumePrimitive(record, offset, value_size))) {
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
                             std::size_t max_batch, std::uint32_t max_delay_us,
                             WalFaultHook fault_hook)
    : path_(std::move(path)),
      output_(path_, std::ios::binary | std::ios::app),
      policy_(policy), max_batch_(max_batch), max_delay_us_(max_delay_us),
      fault_hook_(std::move(fault_hook)) {
  if (!output_.is_open()) {
    throw std::runtime_error("failed to open WAL file: " + path_);
  }
  if (max_batch_ == 0 || max_batch_ > 4096) {
    throw std::invalid_argument("WAL max_batch must be in [1, 4096]");
  }
  std::error_code size_error;
  const auto file_size = std::filesystem::file_size(path_, size_error);
  if (size_error) {
    throw std::runtime_error("failed to inspect WAL file: " + path_);
  }
  if (file_size == 0) {
    header_pending_ = true;
  } else {
    std::ifstream input(path_, std::ios::binary);
    char prefix[sizeof(kWalMagic)]{};
    input.read(prefix, sizeof(prefix));
    const auto prefix_size = static_cast<std::size_t>(input.gcount());
    const bool magic_prefix =
        prefix_size != 0 &&
        std::memcmp(prefix, kWalMagic, prefix_size) == 0;
    if (magic_prefix) {
      format_valid_ = prefix_size == sizeof(kWalMagic) &&
                      parse_header(input, generation_);
      if (format_valid_) {
        std::unordered_map<std::string, std::string> ignored;
        const auto replay = ReplayFromDetailed(0, ignored);
        needs_recovery_ = replay.status != WalReplayStatus::CleanEof;
        if (replay.last_sequence == std::numeric_limits<std::uint64_t>::max()) {
          throw std::runtime_error("WAL sequence is exhausted");
        }
        next_sequence_ = replay.last_sequence + 1;
      } else {
        generation_ = 0;
      }
    } else {
      legacy_v2_ = true;
      generation_ = 0;
      std::unordered_map<std::string, std::string> ignored;
      needs_recovery_ =
          ReplayFromDetailed(0, ignored).status != WalReplayStatus::CleanEof;
    }
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

void WriteAheadLog::EnsureHeaderLocked() {
  if (legacy_v2_ || !header_pending_) return;
  const std::string header = make_header(generation_);
  output_.write(header.data(), static_cast<std::streamsize>(header.size()));
  output_.flush();
  if (!output_) throw std::runtime_error("failed to write WAL header");
  header_pending_ = false;
}

std::string WriteAheadLog::PrepareFrame(const std::string& payload) const {
  const LengthType length =
      checked_record_length(payload.size() + (legacy_v2_ ? 0 : sizeof(std::uint64_t)));
  std::string frame;
  frame.reserve(payload.size() + (legacy_v2_ ? 8 : 16));
  if (legacy_v2_) {
    append_primitive(frame, length);
    append_primitive(frame, crc32(payload));
  } else {
    append_le32(frame, length);
    append_le32(frame, 0);
    append_le64(frame, 0);
  }
  frame.append(payload);
  return frame;
}

void WriteAheadLog::FinalizeFrameLocked(std::string& frame) {
  if (legacy_v2_) return;
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("WAL sequence is exhausted");
  }
  const std::uint64_t sequence = next_sequence_++;
  for (std::size_t i = 0; i < sizeof(sequence); ++i) {
    frame[8 + i] = static_cast<char>(sequence >> (8 * i));
  }
  const ChecksumType checksum = crc32(std::string_view(frame).substr(8));
  for (std::size_t i = 0; i < sizeof(checksum); ++i) {
    frame[4 + i] = static_cast<char>(checksum >> (8 * i));
  }
}

void WriteAheadLog::WriteFrameLocked(std::string& frame) {
  EnsureHeaderLocked();
  FinalizeFrameLocked(frame);
  output_.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  output_.flush();
  if (!output_) throw std::runtime_error("failed to write WAL frame");
  if (fault_hook_) fault_hook_(WalFaultPoint::AfterWriteBeforeSync);
  ++stats_.records;
}

void WriteAheadLog::AppendPayload(std::string payload) {
  std::string frame = PrepareFrame(payload);
  if (policy_ != DurabilityPolicy::GroupCommit) {
    std::lock_guard lock(io_mutex_);
    if (failure_) std::rethrow_exception(failure_);
    if (!format_valid_ || needs_recovery_) {
      throw std::runtime_error("WAL requires recovery before append");
    }
    try {
      WriteFrameLocked(frame);
      if (policy_ == DurabilityPolicy::Sync) {
        SyncLocked();
        if (fault_hook_) fault_hook_(WalFaultPoint::AfterSync);
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
    if (!format_valid_ || needs_recovery_) {
      throw std::runtime_error("WAL requires recovery before append");
    }
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
      EnsureHeaderLocked();
      std::size_t bytes = 0;
      for (const auto& request : batch) {
        bytes += request->frame.size();
      }
      std::string frames;
      frames.reserve(bytes);
      for (const auto& request : batch) {
        FinalizeFrameLocked(request->frame);
        frames.append(request->frame);
      }
      output_.write(frames.data(), static_cast<std::streamsize>(frames.size()));
      output_.flush();
      if (!output_) throw std::runtime_error("failed to write WAL batch");
      if (fault_hook_) fault_hook_(WalFaultPoint::AfterWriteBeforeSync);
      stats_.records += batch.size();
      SyncLocked();
      if (fault_hook_) fault_hook_(WalFaultPoint::AfterSync);
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
  checked_record_length(sizeof(op) + sizeof(key_size) + key_size +
                        sizeof(value_size) + value_size +
                        (legacy_v2_ ? 0 : sizeof(std::uint64_t)));

  std::string payload;
  append_primitive(payload, op);
  if (legacy_v2_) append_primitive(payload, key_size);
  else append_le32(payload, key_size);
  payload.append(key);
  if (legacy_v2_) append_primitive(payload, value_size);
  else append_le32(payload, value_size);
  payload.append(value);
  AppendPayload(std::move(payload));
}

void WriteAheadLog::AppendDelete(const std::string& key) {
  const OpType op = static_cast<OpType>(WalOp::Delete);
  const SizeType key_size = binary_io::CheckedSize(key, "WAL key");
  checked_record_length(sizeof(op) + sizeof(key_size) + key_size +
                        (legacy_v2_ ? 0 : sizeof(std::uint64_t)));

  std::string payload;
  append_primitive(payload, op);
  if (legacy_v2_) append_primitive(payload, key_size);
  else append_le32(payload, key_size);
  payload.append(key);
  AppendPayload(std::move(payload));
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
  if (fault_hook_) fault_hook_(WalFaultPoint::BeforeTruncate);
  if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("WAL generation is exhausted");
  }
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
  ++generation_;
  next_sequence_ = 1;
  legacy_v2_ = false;
  header_pending_ = true;
  format_valid_ = true;
  needs_recovery_ = false;
  if (policy_ != DurabilityPolicy::Buffered) SyncLocked();
}

void WriteAheadLog::Rotate() {
  Clear();
}

void WriteAheadLog::TruncateTo(std::uint64_t offset) {
  std::lock_guard lock(io_mutex_);
  if (fault_hook_) fault_hook_(WalFaultPoint::BeforeTruncate);
  if (!legacy_v2_ && offset == 0 &&
      generation_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("WAL generation is exhausted");
  }
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
  if (!legacy_v2_) {
    if (offset == 0) {
      ++generation_;
      next_sequence_ = 1;
      header_pending_ = true;
      format_valid_ = true;
    } else {
      std::unordered_map<std::string, std::string> ignored;
      const auto replay = ReplayFromDetailed(0, ignored);
      if (replay.status != WalReplayStatus::CleanEof ||
          replay.last_sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("WAL truncation did not leave a valid prefix");
      }
      next_sequence_ = replay.last_sequence + 1;
    }
  }
  needs_recovery_ = false;
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

  char prefix[sizeof(kWalMagic)]{};
  input.read(prefix, sizeof(prefix));
  const auto prefix_size = static_cast<std::size_t>(input.gcount());
  if (prefix_size == 0) return result;
  const bool magic_prefix =
      std::memcmp(prefix, kWalMagic, prefix_size) == 0;
  const bool portable = magic_prefix;
  if (portable) {
    if (prefix_size != sizeof(kWalMagic) ||
        !parse_header(input, result.generation) ||
        (offset != 0 && offset < kHeaderLength)) {
      result.status = WalReplayStatus::InvalidHeader;
      result.last_good_offset = 0;
      result.stop_offset = 0;
      return result;
    }
    if (offset == 0) {
      offset = kHeaderLength;
      result.last_good_offset = offset;
      result.stop_offset = offset;
    }
  }

  input.clear();
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input) {
    return result;
  }

  std::uint64_t cursor = offset;

  while (true) {
    // Each iteration validates one complete v2 or v3 frame.
    // A record mutates the store only after the full payload parses and its
    // checksum matches.
    const std::uint64_t record_start = cursor;
    LengthType record_length = 0;
    if (!read_framing_u32(input, cursor, record_length, result, portable)) {
      break;
    }

    if (record_length == 0 || record_length > kMaxRecordLength) {
      result.status = WalReplayStatus::InvalidLength;
      result.stop_offset = record_start;
      break;
    }

    ChecksumType expected_checksum = 0;
    if (!read_framing_u32(input, cursor, expected_checksum, result, portable)) {
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
    std::uint64_t sequence = 0;
    const WalReplayStatus parse_status =
        parse_record(record, parsed, portable, sequence);
    if (parse_status != WalReplayStatus::CleanEof) {
      result.status = parse_status;
      result.stop_offset = record_start;
      break;
    }
    if (portable &&
        (sequence == 0 ||
         (result.applied_operations == 0 &&
          offset == kHeaderLength && sequence != 1) ||
         (result.applied_operations != 0 &&
          sequence != result.last_sequence + 1))) {
      result.status = WalReplayStatus::InvalidSequence;
      result.stop_offset = record_start;
      break;
    }

    apply_record(parsed, store);
    ++result.applied_operations;
    if (portable) result.last_sequence = sequence;
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
