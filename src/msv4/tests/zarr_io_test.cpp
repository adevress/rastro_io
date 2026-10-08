// Self-contained unit test for the from-scratch Zarr reader/writer.

#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "msv4/zarr_io.hpp"
#include "xtensor/containers/xarray.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

nlohmann::json codec_configuration(rastro::ZarrCodec codec) {
  switch (codec) {
  case rastro::ZarrCodec::Blosc:
    return {{"cname", "lz4"}, {"clevel", 5}, {"shuffle", "shuffle"}, {"typesize", 8}, {"blocksize", 0}};
  case rastro::ZarrCodec::Zstd:
    return {{"level", 3}};
  case rastro::ZarrCodec::Gzip:
  case rastro::ZarrCodec::Zlib:
    return {{"level", 6}};
  case rastro::ZarrCodec::Lz4:
  case rastro::ZarrCodec::None:
    return nlohmann::json::object();
  }
  return nlohmann::json::object();
}

void test_double_roundtrip(const std::string& root, rastro::ZarrCodec codec, bool crc32c) {
  rastro::ZarrWriter writer(root);
  writer.create_group("");

  rastro::ZarrArrayInfo info;
  info.path = "d";
  info.shape = {10, 7};
  info.chunks = {4, 3};
  info.dtype = rastro::ZarrDtype::Float64;
  info.element_bytes = 8;
  info.codec = codec;
  info.codec_configuration = codec_configuration(codec);
  info.crc32c = crc32c;
  info.fill_value = 0.0;
  info.dimension_names = {"row", "column"};
  writer.create_array(info);

  xt::xarray<double> data = xt::xarray<double>::from_shape({10, 7});
  for (std::size_t i = 0; i < 10; ++i) {
    for (std::size_t j = 0; j < 7; ++j) {
      data(i, j) = static_cast<double>(i * 7 + j) + 0.5;
    }
  }
  writer.write_array(info, data);

  rastro::ZarrStore store(root);
  const auto full = rastro::zarr_read<double>(store, info);
  check(full.shape()[0] == 10 && full.shape()[1] == 7, "full read shape");
  for (std::size_t i = 0; i < 10; ++i) {
    for (std::size_t j = 0; j < 7; ++j) {
      check(full(i, j) == data(i, j), "full read value");
    }
  }

  const auto sub = rastro::zarr_read<double>(store, info, {rastro::ZarrSlice{3, 2}, rastro::ZarrSlice{1, 4}});
  check(sub.shape()[0] == 2 && sub.shape()[1] == 4, "sliced read shape");
  for (std::size_t i = 0; i < 2; ++i) {
    for (std::size_t j = 0; j < 4; ++j) {
      check(sub(i, j) == data(i + 3, j + 1), "sliced read value");
    }
  }
}

void test_complex_and_bool(const std::string& root) {
  rastro::ZarrWriter writer(root);
  rastro::ZarrStore store(root);

  rastro::ZarrArrayInfo vis;
  vis.path = "vis";
  vis.shape = {5, 3};
  vis.chunks = {2, 2};
  vis.dtype = rastro::ZarrDtype::Complex64;
  vis.element_bytes = 8;
  vis.codec = rastro::ZarrCodec::Blosc;
  vis.codec_configuration = codec_configuration(rastro::ZarrCodec::Blosc);
  vis.fill_value = nlohmann::json::array({0.0, 0.0});
  writer.create_array(vis);

  xt::xarray<std::complex<float>> values = xt::xarray<std::complex<float>>::from_shape({5, 3});
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t j = 0; j < 3; ++j) {
      values(i, j) = std::complex<float>(static_cast<float>(i), static_cast<float>(j));
    }
  }
  writer.write_array(vis, values);
  const auto read = rastro::zarr_read<std::complex<float>>(store, vis);
  check(read(4, 2) == values(4, 2), "complex read value");

  rastro::ZarrArrayInfo flags;
  flags.path = "flags";
  flags.shape = {5, 3};
  flags.chunks = {2, 2};
  flags.dtype = rastro::ZarrDtype::Boolean;
  flags.element_bytes = 1;
  flags.codec = rastro::ZarrCodec::None;
  flags.fill_value = false;
  writer.create_array(flags);

  xt::xarray<bool> mask = xt::xarray<bool>::from_shape({5, 3});
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t j = 0; j < 3; ++j) {
      mask(i, j) = (i + j) % 2 == 0;
    }
  }
  writer.write_array(flags, mask);
  const auto read_mask = rastro::zarr_read<bool>(store, flags);
  check(read_mask(0, 0) == true && read_mask(1, 0) == false, "bool read value");
}

void test_big_endian(const std::string& root) {
  rastro::ZarrWriter writer(root);
  writer.create_group("");

  // A big-endian complex64 array. The writer must byte-swap each component
  // (real and imaginary separately); the reader must swap them back.
  rastro::ZarrArrayInfo info;
  info.path = "be_c8";
  info.shape = {4};
  info.chunks = {2};
  info.dtype = rastro::ZarrDtype::Complex64;
  info.element_bytes = 8;
  info.endianness = std::endian::big;
  info.codec = rastro::ZarrCodec::None;
  info.fill_value = nlohmann::json::array({0.0, 0.0});
  writer.create_array(info);

  xt::xarray<std::complex<float>> data = xt::xarray<std::complex<float>>::from_shape({4});
  for (std::size_t i = 0; i < 4; ++i) {
    data(i) = std::complex<float>(static_cast<float>(i) + 1.0F, static_cast<float>(i) + 2.0F);
  }
  writer.write_array(info, data);

  rastro::ZarrStore store(root);
  const auto read = rastro::zarr_read<std::complex<float>>(store, info);
  for (std::size_t i = 0; i < 4; ++i) {
    check(read(i) == data(i), "big-endian complex64 round-trip value");
  }

  // The on-disk bytes must really be big-endian: complex64 (1.0, 2.0) is
  // 3F 80 00 00 40 00 00 00 in big-endian (a whole-object byte reversal
  // would store 40 00 00 00 3F 80 00 00, swapping real and imaginary).
  const std::vector<std::byte> raw = rastro::zarr_decompress(info, store.read_chunk_raw(info, {0}));
  const auto* bytes = reinterpret_cast<const unsigned char*>(raw.data());
  check(raw.size() >= 8 && bytes[0] == 0x3F && bytes[1] == 0x80 && bytes[2] == 0x00 && bytes[3] == 0x00 &&
            bytes[4] == 0x40 && bytes[5] == 0x00 && bytes[6] == 0x00 && bytes[7] == 0x00,
        "big-endian complex64 byte order on disk");

  // A compressed big-endian float64 array round-trips too.
  rastro::ZarrArrayInfo dbl;
  dbl.path = "be_f8";
  dbl.shape = {3};
  dbl.chunks = {3};
  dbl.dtype = rastro::ZarrDtype::Float64;
  dbl.element_bytes = 8;
  dbl.endianness = std::endian::big;
  dbl.codec = rastro::ZarrCodec::Zstd;
  dbl.codec_configuration = codec_configuration(rastro::ZarrCodec::Zstd);
  dbl.fill_value = 0.0;
  writer.create_array(dbl);

  xt::xarray<double> values = xt::xarray<double>::from_shape({3});
  values(0) = 1.0;
  values(1) = -2.5;
  values(2) = 3.14159265358979;
  writer.write_array(dbl, values);

  const auto read_dbl = rastro::zarr_read<double>(store, dbl);
  for (std::size_t i = 0; i < 3; ++i) {
    check(read_dbl(i) == values(i), "big-endian float64 round-trip value");
  }
}

void test_dtype_from_name() {
  std::size_t bytes = 0;
  check(rastro::zarr_dtype_from_name("float64", bytes) == rastro::ZarrDtype::Float64 && bytes == 8, "dtype float64");
  check(rastro::zarr_dtype_from_name("complex64", bytes) == rastro::ZarrDtype::Complex64 && bytes == 8,
        "dtype complex64");
  check(rastro::zarr_dtype_from_name("bool", bytes) == rastro::ZarrDtype::Boolean && bytes == 1, "dtype bool");
  // Raw bit types r<N> exercise the from_chars parsing.
  check(rastro::zarr_dtype_from_name("r8", bytes) == rastro::ZarrDtype::Raw && bytes == 1, "dtype r8");
  check(rastro::zarr_dtype_from_name("r16", bytes) == rastro::ZarrDtype::Raw && bytes == 2, "dtype r16");
  check(rastro::zarr_dtype_from_name("r128", bytes) == rastro::ZarrDtype::Raw && bytes == 16, "dtype r128");
  check(rastro::zarr_dtype_from_name("not-a-type", bytes) == rastro::ZarrDtype::Unknown && bytes == 0, "dtype unknown");
}

} // namespace

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "rastro_zarr_io_test.zarr";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  for (const auto codec : {rastro::ZarrCodec::None, rastro::ZarrCodec::Blosc, rastro::ZarrCodec::Zstd,
                           rastro::ZarrCodec::Gzip, rastro::ZarrCodec::Lz4}) {
    test_double_roundtrip(root.string(), codec, false);
  }
  test_double_roundtrip(root.string(), rastro::ZarrCodec::Blosc, true); // with crc32c
  test_complex_and_bool(root.string());
  test_big_endian(root.string());
  test_dtype_from_name();

  std::filesystem::remove_all(root);

  if (g_failures == 0) {
    std::cout << "zarr_io test passed\n";
    return 0;
  }
  std::cerr << g_failures << " checks failed\n";
  return 1;
}
