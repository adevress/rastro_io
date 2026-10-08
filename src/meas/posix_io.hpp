#pragma once

// Small positional-I/O layer used by the on-disk writers and readers.
//
// Everything goes through `preadv`/`pwritev` (with `pread`/`pwrite` fallbacks)
// on raw file descriptors: there is no iostream in this layer. Positional I/O
// is also naturally thread-safe because a file offset is passed per call
// instead of being kept in the descriptor.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rastro {

/// Owning, movable wrapper around a file descriptor with positional I/O.
class PosixFile {
public:
  PosixFile() = default;
  PosixFile(PosixFile&& other) noexcept;
  PosixFile& operator=(PosixFile&& other) noexcept;
  ~PosixFile();

  PosixFile(const PosixFile&) = delete;
  PosixFile& operator=(const PosixFile&) = delete;

  /// Open an existing file for reading.
  static PosixFile open_read(const std::string& path);
  /// Open (create or truncate) a file for writing.
  static PosixFile open_write(const std::string& path);
  /// Open (create) a file for reading and writing without truncating.
  static PosixFile open_read_write(const std::string& path);

  bool valid() const { return m_fd >= 0; }
  int descriptor() const { return m_fd; }

  /// Size of the underlying file in bytes.
  std::uint64_t size() const;

  /// Read up to `buffer.size()` bytes at `offset`, returning the byte count.
  std::size_t read_at(std::span<std::byte> buffer, std::uint64_t offset) const;
  /// Read exactly `buffer.size()` bytes at `offset` or throw.
  void read_exact_at(std::span<std::byte> buffer, std::uint64_t offset) const;
  /// Scatter-read the buffers sequentially starting at `offset`.
  std::size_t readv_at(const std::vector<std::span<std::byte>>& buffers, std::uint64_t offset) const;

  /// Write up to `buffer.size()` bytes at `offset`, returning the byte count.
  std::size_t write_at(std::span<const std::byte> buffer, std::uint64_t offset) const;
  /// Write exactly `buffer.size()` bytes at `offset` or throw.
  void write_exact_at(std::span<const std::byte> buffer, std::uint64_t offset) const;
  /// Gather-write the buffers sequentially starting at `offset`.
  std::size_t writev_at(const std::vector<std::span<const std::byte>>& buffers, std::uint64_t offset) const;

  void close();

private:
  explicit PosixFile(int fd) : m_fd(fd) {}
  int m_fd = -1;
};

/// True when `path` exists and is a regular file.
bool file_is_regular(const std::string& path);
/// True when `path` exists (file or directory).
bool path_exists(const std::string& path);
/// Create a directory and all its missing ancestors.
void create_directories(const std::string& path);

/// Read a whole file into an owning byte buffer using `preadv`.
std::vector<std::byte> read_file(const std::string& path);
/// Write a whole file from a byte buffer using `pwritev`.
void write_file(const std::string& path, std::span<const std::byte> bytes);
/// Atomically replace `path` with a byte buffer (write to a temporary then rename).
void write_file_atomic(const std::string& path, std::span<const std::byte> bytes);

} // namespace rastro
