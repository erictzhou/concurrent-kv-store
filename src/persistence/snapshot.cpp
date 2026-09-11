#include "persistence/snapshot.h"

#include "persistence/binary_io.h"

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <system_error>
#include <unistd.h>

namespace kv {
namespace persistence {

namespace {

using CountType = std::uint32_t;
using SizeType = binary_io::SizeType;

constexpr std::uint32_t kSnapshotMagic = 0x3153564BU;  // "KVS1"
constexpr std::uint32_t kSnapshotVersion = 1;
constexpr std::size_t kMaxSnapshotFieldLength = 64U * 1024U * 1024U;

void sync_file(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "open snapshot for sync");
#ifdef __linux__
  const int result = ::fdatasync(fd);
#else
  const int result = ::fsync(fd);
#endif
  const int error = errno;
  ::close(fd);
  if (result < 0) throw std::system_error(error, std::generic_category(), "sync snapshot");
}

void sync_parent_directory(const std::string& path) {
  const auto parent = std::filesystem::path(path).parent_path();
  const std::string directory = parent.empty() ? "." : parent.string();
#ifdef O_DIRECTORY
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
  const int fd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
#endif
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "open snapshot directory");
  const int result = ::fsync(fd);
  const int error = errno;
  ::close(fd);
  if (result < 0) throw std::system_error(error, std::generic_category(), "sync snapshot directory");
}

void remove_if_exists(const std::string& path, const char* description) {
  errno = 0;
  if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
    throw std::runtime_error(std::string("failed to remove ") + description +
                             ": " + path);
  }
}

CountType checked_entry_count(std::size_t count) {
  if (count > std::numeric_limits<CountType>::max()) {
    throw std::runtime_error("too many snapshot entries");
  }
  return static_cast<CountType>(count);
}

void write_entry(std::ofstream& output, const std::string& key,
                 const std::string& value) {
  const SizeType key_size = binary_io::CheckedSize(key, "Snapshot key");
  const SizeType value_size = binary_io::CheckedSize(value, "Snapshot value");

  binary_io::WritePrimitive(output, key_size, "Snapshot key size");
  binary_io::WriteBytes(output, key, "Snapshot key");
  binary_io::WritePrimitive(output, value_size, "Snapshot value size");
  binary_io::WriteBytes(output, value, "Snapshot value");
}

bool read_entry(std::ifstream& input, std::string& key, std::string& value) {
  SizeType key_size = 0;
  if (!binary_io::ReadPrimitive(input, key_size)) {
    return false;
  }

  if (key_size > kMaxSnapshotFieldLength) {
    throw std::runtime_error("snapshot key is too large");
  }

  std::string loaded_key(key_size, '\0');
  input.read(loaded_key.data(), static_cast<std::streamsize>(loaded_key.size()));
  if (!input) {
    return false;
  }

  SizeType value_size = 0;
  if (!binary_io::ReadPrimitive(input, value_size)) {
    return false;
  }

  if (value_size > kMaxSnapshotFieldLength) {
    throw std::runtime_error("snapshot value is too large");
  }

  std::string loaded_value(value_size, '\0');
  input.read(loaded_value.data(),
             static_cast<std::streamsize>(loaded_value.size()));
  if (!input) {
    return false;
  }

  key = std::move(loaded_key);
  value = std::move(loaded_value);
  return true;
}

}  // namespace

Snapshot::Snapshot(std::string path) : path_(std::move(path)) {}

void Snapshot::Save(
    const std::unordered_map<std::string, std::string>& store,
    std::uint64_t wal_offset) const {
  const std::string temp_path = path_ + ".tmp";
  const CountType entry_count = checked_entry_count(store.size());

  {
    // Keep the stream scoped so the temp file is closed before rename.
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      throw std::runtime_error("failed to open temp snapshot file for write: " +
                               temp_path);
    }

    binary_io::WritePrimitive(output, kSnapshotMagic, "Snapshot magic");
    binary_io::WritePrimitive(output, kSnapshotVersion, "Snapshot version");
    binary_io::WritePrimitive(output, wal_offset, "Snapshot WAL offset");
    binary_io::WritePrimitive(output, entry_count, "Snapshot entry count");
    for (const auto& entry : store) {
      write_entry(output, entry.first, entry.second);
    }

    output.flush();
    if (!output) {
      throw std::runtime_error("failed to write temp snapshot file");
    }
  }

  sync_file(temp_path);

  if (std::rename(temp_path.c_str(), path_.c_str()) != 0) {
    throw std::runtime_error("failed to replace snapshot file: " + path_);
  }
  sync_parent_directory(path_);
}

void Snapshot::SaveVerified(
    const std::unordered_map<std::string, std::string>& store,
    std::uint64_t wal_offset) const {
  Save(store, wal_offset);
  if (!Verify(store, wal_offset)) {
    throw std::runtime_error("snapshot verification failed: " + path_);
  }
}

bool Snapshot::Verify(
    const std::unordered_map<std::string, std::string>& expected_store,
    std::uint64_t expected_wal_offset) const {
  std::unordered_map<std::string, std::string> loaded;
  const SnapshotLoadResult result = Load(loaded);
  return result.loaded && result.entry_count == expected_store.size() &&
         result.wal_offset == expected_wal_offset &&
         loaded == expected_store;
}

void Snapshot::Clear() const {
  // Clear both the committed snapshot and any abandoned temp file from an
  // interrupted save. The in-memory store decides separately whether data
  // should also be cleared.
  remove_if_exists(path_, "snapshot file");
  remove_if_exists(path_ + ".tmp", "temp snapshot file");
  sync_parent_directory(path_);
}

SnapshotLoadResult Snapshot::Load(
    std::unordered_map<std::string, std::string>& store) const {
  std::ifstream input(path_, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  std::uint64_t wal_offset = 0;
  CountType entry_count = 0;
  std::uint32_t first_field = 0;
  if (!binary_io::ReadPrimitive(input, first_field)) {
    return {};
  }

  if (first_field == kSnapshotMagic) {
    std::uint32_t version = 0;
    if (!binary_io::ReadPrimitive(input, version) ||
        version != kSnapshotVersion ||
        !binary_io::ReadPrimitive(input, wal_offset) ||
        !binary_io::ReadPrimitive(input, entry_count)) {
      throw std::runtime_error("failed to load snapshot metadata");
    }
  } else {
    // Legacy snapshots started directly with the entry count. Treat them as
    // covering no WAL bytes so recovery safely falls back to full WAL replay.
    entry_count = first_field;
  }

  std::unordered_map<std::string, std::string> loaded;
  for (CountType counter = 0; counter < entry_count; ++counter) {
    std::string key;
    std::string value;
    if (!read_entry(input, key, value)) {
      throw std::runtime_error("failed to load snapshot");
    }
    loaded[key] = value;
  }

  store = std::move(loaded);
  return SnapshotLoadResult{true, entry_count, wal_offset};
}

}  // namespace persistence
}  // namespace kv
