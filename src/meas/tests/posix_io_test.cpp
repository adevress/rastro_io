// Unit test for the positional-I/O layer (preadv/pwritev based).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "meas/posix_io.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

std::vector<std::byte> make_bytes(std::size_t count, std::uint8_t seed) {
  std::vector<std::byte> bytes(count);
  for (std::size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<std::byte>((i + seed) % 256);
  }
  return bytes;
}

void test_whole_file(const std::filesystem::path& root) {
  const std::string path = (root / "whole.bin").string();
  const std::vector<std::byte> payload = make_bytes(4096, 7);
  rastro::write_file(path, payload);
  check(rastro::file_is_regular(path), "write_file creates a regular file");

  const std::vector<std::byte> read = rastro::read_file(path);
  check(read.size() == payload.size(), "read_file round-trip size");
  check(read == payload, "read_file round-trip content");

  rastro::PosixFile file = rastro::PosixFile::open_read(path);
  check(file.size() == payload.size(), "PosixFile::size");
}

void test_positional(const std::filesystem::path& root) {
  const std::string path = (root / "positional.bin").string();
  rastro::PosixFile file = rastro::PosixFile::open_read_write(path);

  const std::vector<std::byte> first = make_bytes(100, 1);
  const std::vector<std::byte> second = make_bytes(50, 2);
  file.write_exact_at(first, 0);
  file.write_exact_at(second, 1000);
  check(file.size() == 1050, "size after two positional writes");

  std::vector<std::byte> buffer(100);
  file.read_exact_at(buffer, 0);
  check(buffer == first, "positional read at offset 0");

  buffer.assign(50, std::byte{0});
  file.read_exact_at(buffer, 1000);
  check(buffer == second, "positional read at offset 1000");

  // A read crossing the gap must return zeros.
  std::vector<std::byte> gap(100);
  file.read_exact_at(gap, 100);
  check(std::all_of(gap.begin(), gap.end(), [](std::byte b) { return b == std::byte{0}; }), "sparse gap is zero");
}

void test_scatter_gather(const std::filesystem::path& root) {
  const std::string path = (root / "scatter.bin").string();
  rastro::PosixFile file = rastro::PosixFile::open_read_write(path);

  std::vector<std::byte> a = make_bytes(32, 10);
  std::vector<std::byte> b = make_bytes(64, 20);
  std::vector<std::byte> c = make_bytes(16, 30);
  const std::vector<std::span<const std::byte>> out = {a, b, c};
  const std::size_t written = file.writev_at(out, 128);
  check(written == a.size() + b.size() + c.size(), "writev_at byte count");

  std::vector<std::byte> ra(32);
  std::vector<std::byte> rb(64);
  std::vector<std::byte> rc(16);
  const std::vector<std::span<std::byte>> in = {ra, rb, rc};
  const std::size_t got = file.readv_at(in, 128);
  check(got == written, "readv_at byte count");
  check(ra == a && rb == b && rc == c, "readv_at content");
}

void test_atomic(const std::filesystem::path& root) {
  const std::string path = (root / "nested" / "atomic.bin").string();
  const std::vector<std::byte> payload = make_bytes(777, 42);
  rastro::write_file_atomic(path, payload);
  check(rastro::file_is_regular(path), "write_file_atomic creates nested file");
  check(rastro::read_file(path) == payload, "write_file_atomic content");
}

void test_missing_file(const std::filesystem::path& root) {
  bool threw = false;
  try {
    rastro::read_file((root / "does-not-exist.bin").string());
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "read_file throws on a missing file");
}

} // namespace

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "rastro_posix_io_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  try {
    test_whole_file(root);
    test_positional(root);
    test_scatter_gather(root);
    test_atomic(root);
    test_missing_file(root);
  } catch (const std::exception& error) {
    std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
    ++g_failures;
  }

  std::filesystem::remove_all(root);

  if (g_failures == 0) {
    std::cout << "posix_io test passed\n";
    return 0;
  }
  std::cerr << g_failures << " checks failed\n";
  return 1;
}
