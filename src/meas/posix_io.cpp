#include "posix_io.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace rastro {
namespace {

[[noreturn]] void throw_errno(const std::string& what) { throw std::runtime_error(what + ": " + std::strerror(errno)); }

/// Build the iovec array describing a set of writable byte spans.
std::vector<iovec> make_iovec(const std::vector<std::span<std::byte>>& buffers) {
  std::vector<iovec> vec;
  vec.reserve(buffers.size());
  for (const std::span<std::byte>& buffer : buffers) {
    vec.push_back(iovec{const_cast<std::byte*>(buffer.data()), buffer.size()});
  }
  return vec;
}

/// Build the iovec array describing a set of read-only byte spans.
std::vector<iovec> make_iovec(const std::vector<std::span<const std::byte>>& buffers) {
  std::vector<iovec> vec;
  vec.reserve(buffers.size());
  for (const std::span<const std::byte>& buffer : buffers) {
    vec.push_back(iovec{const_cast<std::byte*>(buffer.data()), buffer.size()});
  }
  return vec;
}

/// Number of bytes described by an iovec array.
std::size_t iovec_bytes(const std::vector<iovec>& vec) {
  std::size_t total = 0;
  for (const iovec& entry : vec) {
    total += entry.iov_len;
  }
  return total;
}

/// Advance the iovec array past the first `consumed` bytes.
void advance_iovec(std::vector<iovec>& vec, std::size_t consumed) {
  std::size_t index = 0;
  while (index < vec.size() && consumed >= vec[index].iov_len) {
    consumed -= vec[index].iov_len;
    ++index;
  }
  if (index > 0) {
    vec.erase(vec.begin(), vec.begin() + static_cast<std::ptrdiff_t>(index));
  }
  if (consumed > 0 && !vec.empty()) {
    auto* base = static_cast<std::byte*>(vec.front().iov_base) + consumed;
    vec.front().iov_base = base;
    vec.front().iov_len -= consumed;
  }
}

std::string temporary_suffix() {
  return ".tmp." + std::to_string(static_cast<unsigned long>(::getpid())) + "." +
         std::to_string(static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
}

} // namespace

PosixFile::PosixFile(PosixFile&& other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; }

PosixFile& PosixFile::operator=(PosixFile&& other) noexcept {
  if (this != &other) {
    close();
    m_fd = other.m_fd;
    other.m_fd = -1;
  }
  return *this;
}

PosixFile::~PosixFile() { close(); }

PosixFile PosixFile::open_read(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throw_errno("cannot open '" + path + "' for reading");
  }
  return PosixFile(fd);
}

PosixFile PosixFile::open_write(const std::string& path) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    throw_errno("cannot open '" + path + "' for writing");
  }
  return PosixFile(fd);
}

PosixFile PosixFile::open_read_write(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    throw_errno("cannot open '" + path + "' for reading and writing");
  }
  return PosixFile(fd);
}

void PosixFile::close() {
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
}

std::uint64_t PosixFile::size() const {
  struct stat status{};
  if (::fstat(m_fd, &status) != 0) {
    throw_errno("cannot stat file descriptor");
  }
  return static_cast<std::uint64_t>(status.st_size);
}

std::size_t PosixFile::read_at(std::span<std::byte> buffer, std::uint64_t offset) const {
  if (buffer.empty()) {
    return 0;
  }
  std::size_t total = 0;
  while (total < buffer.size()) {
    const ssize_t result =
        ::pread(m_fd, buffer.data() + total, buffer.size() - total, static_cast<off_t>(offset + total));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("pread failed");
    }
    if (result == 0) {
      break; // end of file
    }
    total += static_cast<std::size_t>(result);
  }
  return total;
}

void PosixFile::read_exact_at(std::span<std::byte> buffer, std::uint64_t offset) const {
  const std::size_t read = read_at(buffer, offset);
  if (read != buffer.size()) {
    throw std::runtime_error("short read: expected " + std::to_string(buffer.size()) + " bytes, got " +
                             std::to_string(read));
  }
}

std::size_t PosixFile::readv_at(const std::vector<std::span<std::byte>>& buffers, std::uint64_t offset) const {
  std::vector<iovec> vec = make_iovec(buffers);
  const std::size_t expected = iovec_bytes(vec);
  std::size_t total = 0;
  std::uint64_t position = offset;
  while (!vec.empty()) {
    const ssize_t result = ::preadv(m_fd, vec.data(), static_cast<int>(vec.size()), static_cast<off_t>(position));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("preadv failed");
    }
    if (result == 0) {
      break;
    }
    total += static_cast<std::size_t>(result);
    position += static_cast<std::uint64_t>(result);
    advance_iovec(vec, static_cast<std::size_t>(result));
  }
  (void)expected;
  return total;
}

std::size_t PosixFile::write_at(std::span<const std::byte> buffer, std::uint64_t offset) const {
  if (buffer.empty()) {
    return 0;
  }
  std::size_t total = 0;
  while (total < buffer.size()) {
    const ssize_t result =
        ::pwrite(m_fd, buffer.data() + total, buffer.size() - total, static_cast<off_t>(offset + total));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("pwrite failed");
    }
    total += static_cast<std::size_t>(result);
  }
  return total;
}

void PosixFile::write_exact_at(std::span<const std::byte> buffer, std::uint64_t offset) const {
  const std::size_t written = write_at(buffer, offset);
  if (written != buffer.size()) {
    throw std::runtime_error("short write: expected " + std::to_string(buffer.size()) + " bytes, got " +
                             std::to_string(written));
  }
}

std::size_t PosixFile::writev_at(const std::vector<std::span<const std::byte>>& buffers, std::uint64_t offset) const {
  std::vector<iovec> vec = make_iovec(buffers);
  std::size_t total = 0;
  std::uint64_t position = offset;
  while (!vec.empty()) {
    const ssize_t result = ::pwritev(m_fd, vec.data(), static_cast<int>(vec.size()), static_cast<off_t>(position));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("pwritev failed");
    }
    if (result == 0) {
      throw std::runtime_error("pwritev wrote zero bytes");
    }
    total += static_cast<std::size_t>(result);
    position += static_cast<std::uint64_t>(result);
    advance_iovec(vec, static_cast<std::size_t>(result));
  }
  return total;
}

bool file_is_regular(const std::string& path) {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) {
    return false;
  }
  return S_ISREG(status.st_mode);
}

bool path_exists(const std::string& path) {
  struct stat status{};
  return ::stat(path.c_str(), &status) == 0;
}

void create_directories(const std::string& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) {
    throw std::runtime_error("cannot create directory '" + path + "': " + error.message());
  }
}

std::vector<std::byte> read_file(const std::string& path) {
  PosixFile file = PosixFile::open_read(path);
  const std::uint64_t size = file.size();
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    file.read_exact_at(bytes, 0);
  }
  return bytes;
}

void write_file(const std::string& path, std::span<const std::byte> bytes) {
  PosixFile file = PosixFile::open_write(path);
  file.write_exact_at(bytes, 0);
}

void write_file_atomic(const std::string& path, std::span<const std::byte> bytes) {
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    create_directories(parent.string());
  }
  const std::string temporary = path + temporary_suffix();
  write_file(temporary, bytes);
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    const int saved = errno;
    ::remove(temporary.c_str());
    errno = saved;
    throw_errno("cannot rename '" + temporary + "' to '" + path + "'");
  }
}

} // namespace rastro
