#include "zarr_io.hpp"

#include "meas/posix_io.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <charconv>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <unistd.h>

#include <blosc.h>
#include <lz4.h>
#include <zlib.h>
#include <zstd.h>

namespace rastro {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

template <class T> void swap_endianness(T& value) {
  auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
  std::reverse(bytes, bytes + sizeof(T));
}

template <class T> struct is_complex : std::false_type {};
template <class U> struct is_complex<std::complex<U>> : std::true_type {};

/// Byte-swap one scalar value. Complex values are swapped per component:
/// reversing the whole object would exchange the real and imaginary parts.
template <class T> void swap_scalar_endianness(T& value) {
  if constexpr (is_complex<T>::value) {
    // std::complex<T> is layout-compatible with T[2] (array-oriented access).
    auto& parts = reinterpret_cast<typename T::value_type(&)[2]>(value);
    swap_endianness(parts[0]);
    swap_endianness(parts[1]);
  } else {
    swap_endianness(value);
  }
}

/// True when elements of an array declared with `endianness` must be
/// byte-swapped to be usable on this host.
constexpr bool needs_byte_swap(std::endian endianness) { return endianness != std::endian::native; }

/// Join the store root with a store-relative path.
fs::path join_path(const std::string& root, const fs::path& key) {
  if (key.empty()) {
    return root;
  }
  return fs::path(root) / key;
}

bool file_exists(const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error);
}

/// Read a whole file into an owning byte buffer (preadv-based).
std::vector<std::byte> read_file(const fs::path& path) { return ::rastro::read_file(path.string()); }

/// Parse a JSON metadata document from raw bytes.
nlohmann::json parse_json(std::span<const std::byte> bytes) {
  const auto* first = reinterpret_cast<const char*>(bytes.data());
  return nlohmann::json::parse(first, first + bytes.size());
}

/// Non-owning byte view over character data.
std::span<const std::byte> as_byte_span(std::string_view text) { return std::as_bytes(std::span(text)); }

void write_file_atomic(const fs::path& path, std::span<const std::byte> bytes) {
  ::rastro::write_file_atomic(path.string(), bytes);
}

std::size_t product(const std::vector<std::size_t>& values) {
  return std::accumulate(values.begin(), values.end(), std::size_t{1}, std::multiplies<>{});
}

/// Row-major (C) strides for an extent.
std::vector<std::size_t> c_strides(const std::vector<std::size_t>& extent) {
  std::vector<std::size_t> strides(extent.size(), 1);
  for (std::size_t i = extent.size(); i-- > 1;) {
    strides[i - 1] = strides[i] * extent[i];
  }
  return strides;
}

// ---------------------------------------------------------------------------
// CRC32C (Castagnoli), used by the Zarr v3 `crc32c` codec
// ---------------------------------------------------------------------------

std::uint32_t crc32c(std::span<const std::byte> data) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) ? (0x82F63B78U ^ (value >> 1)) : (value >> 1);
      }
      values[i] = value;
    }
    return values;
  }();

  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::byte byte : data) {
    crc = table[(crc ^ static_cast<std::uint8_t>(byte)) & 0xFFU] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFU;
}

// ---------------------------------------------------------------------------
// Compression codecs
// ---------------------------------------------------------------------------

int blosc_shuffle_from_json(const nlohmann::json& configuration) {
  if (!configuration.contains("shuffle")) {
    return BLOSC_SHUFFLE;
  }
  const nlohmann::json& value = configuration.at("shuffle");
  if (value.is_number()) {
    return value.get<int>();
  }
  const std::string_view name = value.get<std::string_view>();
  if (name == "noshuffle" || name == "NOSHUFFLE") {
    return BLOSC_NOSHUFFLE;
  }
  if (name == "bitshuffle" || name == "BITSHUFFLE") {
    return BLOSC_BITSHUFFLE;
  }
  return BLOSC_SHUFFLE;
}

void ensure_blosc_initialized() {
  static std::once_flag flag;
  std::call_once(flag, [] { blosc_init(); });
}

std::vector<std::byte> blosc_decompress_bytes(std::span<const std::byte> payload) {
  ensure_blosc_initialized();
  std::size_t uncompressed_size = 0;
  if (blosc_cbuffer_validate(payload.data(), payload.size(), &uncompressed_size) != 0) {
    throw std::runtime_error("zarr: invalid Blosc buffer");
  }
  std::vector<std::byte> output(uncompressed_size);
  const int result = blosc_decompress(payload.data(), output.data(), uncompressed_size);
  if (result <= 0) {
    throw std::runtime_error("zarr: Blosc decompression failed");
  }
  return output;
}

std::vector<std::byte> blosc_compress_bytes(std::span<const std::byte> raw, const nlohmann::json& configuration,
                                            std::size_t typesize) {
  ensure_blosc_initialized();
  const int clevel = configuration.value("clevel", 5);
  const int shuffle = blosc_shuffle_from_json(configuration);
  const std::string cname = configuration.value("cname", std::string("lz4"));
  const std::size_t blocksize = configuration.value("blocksize", std::size_t{0});

  std::vector<std::byte> output(raw.size() + BLOSC_MAX_OVERHEAD);
  const int result = blosc_compress_ctx(clevel, shuffle, typesize, raw.size(), raw.data(), output.data(), output.size(),
                                        cname.c_str(), blocksize, 1);
  if (result <= 0) {
    throw std::runtime_error("zarr: Blosc compression failed");
  }
  output.resize(static_cast<std::size_t>(result));
  return output;
}

std::vector<std::byte> zstd_decompress_bytes(std::span<const std::byte> payload) {
  const unsigned long long size = ZSTD_getFrameContentSize(payload.data(), payload.size());
  if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
    throw std::runtime_error("zarr: invalid or unsized Zstandard frame");
  }
  std::vector<std::byte> output(static_cast<std::size_t>(size));
  const std::size_t result = ZSTD_decompress(output.data(), output.size(), payload.data(), payload.size());
  if (ZSTD_isError(result)) {
    throw std::runtime_error(std::string("zarr: Zstandard decompression failed: ") + ZSTD_getErrorName(result));
  }
  return output;
}

std::vector<std::byte> zstd_compress_bytes(std::span<const std::byte> raw, int level) {
  std::vector<std::byte> output(ZSTD_compressBound(raw.size()));
  const std::size_t result = ZSTD_compress(output.data(), output.size(), raw.data(), raw.size(), level);
  if (ZSTD_isError(result)) {
    throw std::runtime_error(std::string("zarr: Zstandard compression failed: ") + ZSTD_getErrorName(result));
  }
  output.resize(result);
  return output;
}

std::vector<std::byte> inflate_bytes(std::span<const std::byte> payload) {
  z_stream stream{};
  if (inflateInit2(&stream, 15 + 32) != Z_OK) { // auto-detect gzip / zlib wrapper
    throw std::runtime_error("zarr: inflateInit2 failed");
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(payload.data()));
  stream.avail_in = static_cast<uInt>(payload.size());

  std::vector<std::byte> output;
  std::array<std::byte, 1 << 16> buffer{};
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    result = inflate(&stream, Z_NO_FLUSH);
    if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
      inflateEnd(&stream);
      throw std::runtime_error("zarr: inflate failed");
    }
    output.insert(output.end(), buffer.data(), buffer.data() + (buffer.size() - stream.avail_out));
  } while (result != Z_STREAM_END);
  inflateEnd(&stream);
  return output;
}

std::vector<std::byte> deflate_bytes(std::span<const std::byte> raw, int level, bool gzip_wrapper) {
  z_stream stream{};
  const int window_bits = gzip_wrapper ? (15 + 16) : 15;
  if (deflateInit2(&stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    throw std::runtime_error("zarr: deflateInit2 failed");
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(raw.data()));
  stream.avail_in = static_cast<uInt>(raw.size());

  std::vector<std::byte> output;
  std::array<std::byte, 1 << 16> buffer{};
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    result = deflate(&stream, Z_FINISH);
    if (result == Z_STREAM_ERROR) {
      deflateEnd(&stream);
      throw std::runtime_error("zarr: deflate failed");
    }
    output.insert(output.end(), buffer.data(), buffer.data() + (buffer.size() - stream.avail_out));
  } while (result != Z_STREAM_END);
  deflateEnd(&stream);
  return output;
}

std::vector<std::byte> lz4_decompress_bytes(std::span<const std::byte> payload, std::size_t expected_size) {
  const std::byte* data = payload.data();
  std::size_t size = payload.size();
  if (size >= sizeof(std::uint32_t)) {
    std::uint32_t prefix = 0;
    std::memcpy(&prefix, data, sizeof(prefix));
    if (prefix == expected_size) { // numcodecs `store_size=True` framing
      data += sizeof(prefix);
      size -= sizeof(prefix);
    }
  }
  if (expected_size == 0) {
    return {};
  }
  std::vector<std::byte> output(expected_size);
  const int result = LZ4_decompress_safe(reinterpret_cast<const char*>(data), reinterpret_cast<char*>(output.data()),
                                         static_cast<int>(size), static_cast<int>(expected_size));
  if (result < 0) {
    throw std::runtime_error("zarr: LZ4 decompression failed");
  }
  output.resize(static_cast<std::size_t>(result));
  return output;
}

std::vector<std::byte> lz4_compress_bytes(std::span<const std::byte> raw) {
  const int bound = LZ4_compressBound(static_cast<int>(raw.size()));
  if (bound <= 0) {
    throw std::runtime_error("zarr: LZ4 input too large");
  }
  std::vector<std::byte> output(sizeof(std::uint32_t) + static_cast<std::size_t>(bound));
  const std::uint32_t size = static_cast<std::uint32_t>(raw.size());
  std::memcpy(output.data(), &size, sizeof(size));
  const int result =
      LZ4_compress_default(reinterpret_cast<const char*>(raw.data()),
                           reinterpret_cast<char*>(output.data() + sizeof(size)), static_cast<int>(raw.size()), bound);
  if (result <= 0) {
    throw std::runtime_error("zarr: LZ4 compression failed");
  }
  output.resize(sizeof(size) + static_cast<std::size_t>(result));
  return output;
}

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

ZarrDtype integer_dtype(std::size_t bytes) {
  switch (bytes) {
  case 1:
    return ZarrDtype::Int8;
  case 2:
    return ZarrDtype::Int16;
  case 4:
    return ZarrDtype::Int32;
  case 8:
    return ZarrDtype::Int64;
  default:
    return ZarrDtype::Unknown;
  }
}

ZarrDtype unsigned_dtype(std::size_t bytes) {
  switch (bytes) {
  case 1:
    return ZarrDtype::UInt8;
  case 2:
    return ZarrDtype::UInt16;
  case 4:
    return ZarrDtype::UInt32;
  case 8:
    return ZarrDtype::UInt64;
  default:
    return ZarrDtype::Unknown;
  }
}

ZarrDtype float_dtype(std::size_t bytes) {
  switch (bytes) {
  case 2:
    return ZarrDtype::Float16;
  case 4:
    return ZarrDtype::Float32;
  case 8:
    return ZarrDtype::Float64;
  default:
    return ZarrDtype::Unknown;
  }
}

ZarrDtype complex_dtype(std::size_t bytes) {
  switch (bytes) {
  case 8:
    return ZarrDtype::Complex64;
  case 16:
    return ZarrDtype::Complex128;
  default:
    return ZarrDtype::Unknown;
  }
}

/// Parse a NumPy-style v2 type string (`<f8`, `|b1`, `|S12`, `|U4`, ...).
ZarrDtype parse_v2_dtype(const std::string& text, std::endian& endianness, std::size_t& element_bytes) {
  if (text.empty()) {
    return ZarrDtype::Unknown;
  }
  std::size_t position = 0;
  const char order = text[0];
  if (order == '<' || order == '>' || order == '|') {
    if (order == '>') {
      endianness = std::endian::big;
    } else if (order == '<') {
      endianness = std::endian::little;
    } else {
      // '|' means "not relevant" (single-byte types): treat as native order.
      endianness = std::endian::native;
    }
    position = 1;
  }
  if (position >= text.size()) {
    return ZarrDtype::Unknown;
  }
  const char kind = text[position++];
  std::string digits = text.substr(position);
  const std::size_t bracket = digits.find('[');
  if (bracket != std::string::npos) {
    digits = digits.substr(0, bracket);
  }
  std::size_t size = 0;
  if (!digits.empty()) {
    const std::from_chars_result result = std::from_chars(digits.data(), digits.data() + digits.size(), size);
    if (result.ec != std::errc{}) {
      throw std::runtime_error("zarr: malformed v2 dtype '" + text + "'");
    }
  }

  switch (kind) {
  case 'b':
    element_bytes = size;
    return ZarrDtype::Boolean;
  case 'i':
    element_bytes = size;
    return integer_dtype(size);
  case 'u':
    element_bytes = size;
    return unsigned_dtype(size);
  case 'f':
    element_bytes = size;
    return float_dtype(size);
  case 'c':
    element_bytes = size;
    return complex_dtype(size);
  case 'S':
    element_bytes = size;
    return ZarrDtype::Raw;
  case 'U':
    // Fixed-length UTF-32 (4 bytes per code point).
    element_bytes = size * 4;
    return ZarrDtype::Utf32Fixed;
  default:
    return ZarrDtype::Unknown;
  }
}

// ---------------------------------------------------------------------------
// Metadata parsing
// ---------------------------------------------------------------------------

std::vector<std::size_t> json_sizes(const nlohmann::json& value) {
  std::vector<std::size_t> sizes;
  sizes.reserve(value.size());
  for (const auto& entry : value) {
    sizes.push_back(entry.get<std::size_t>());
  }
  return sizes;
}

/// Reject zero-length chunk dimensions: they would cause a division by zero
/// when mapping elements to chunks (and an endless loop in the string
/// reader).
void validate_chunks(std::string_view path, const std::vector<std::size_t>& chunks) {
  for (const std::size_t chunk : chunks) {
    if (chunk == 0) {
      throw std::runtime_error("zarr: zero-length chunk dimension in array '" + std::string(path) + "'");
    }
  }
}

void parse_v3_codecs(const nlohmann::json& array, ZarrArrayInfo& info) {
  if (!array.contains("codecs")) {
    return;
  }
  for (const auto& codec : array.at("codecs")) {
    const std::string_view name = codec.at("name").get<std::string_view>();
    const nlohmann::json configuration = codec.value("configuration", nlohmann::json::object());
    if (name == "bytes") {
      info.endianness =
          configuration.value("endian", std::string("little")) == "big" ? std::endian::big : std::endian::little;
    } else if (name == "transpose") {
      // Only the identity permutation (the default) is supported; anything
      // else changes the in-chunk memory layout.
      const std::vector<std::size_t> order =
          configuration.contains("order") ? json_sizes(configuration.at("order")) : std::vector<std::size_t>{};
      for (std::size_t d = 0; d < order.size(); ++d) {
        if (order[d] != d) {
          throw std::runtime_error("zarr: non-identity transpose codec is not supported");
        }
      }
    } else if (name == "blosc") {
      info.codec = ZarrCodec::Blosc;
      info.codec_configuration = configuration;
    } else if (name == "zstd") {
      info.codec = ZarrCodec::Zstd;
      info.codec_configuration = configuration;
    } else if (name == "gzip") {
      info.codec = ZarrCodec::Gzip;
      info.codec_configuration = configuration;
    } else if (name == "lz4") {
      info.codec = ZarrCodec::Lz4;
      info.codec_configuration = configuration;
    } else if (name == "crc32c") {
      info.crc32c = true;
    } else if (name == "sharding_indexed" || name == "sharding") {
      throw std::runtime_error("zarr: sharding codec is not supported");
    } else {
      throw std::runtime_error("zarr: unsupported codec '" + std::string(name) + "'");
    }
  }
}

void parse_v2_compressor(const nlohmann::json& array, ZarrArrayInfo& info) {
  if (!array.contains("compressor") || array.at("compressor").is_null()) {
    return;
  }
  const nlohmann::json& compressor = array.at("compressor");
  const std::string_view id = compressor.at("id").get<std::string_view>();
  info.codec_configuration = compressor;
  if (id == "blosc") {
    info.codec = ZarrCodec::Blosc;
  } else if (id == "zstd") {
    info.codec = ZarrCodec::Zstd;
  } else if (id == "gzip") {
    info.codec = ZarrCodec::Gzip;
  } else if (id == "zlib") {
    info.codec = ZarrCodec::Zlib;
  } else if (id == "lz4") {
    info.codec = ZarrCodec::Lz4;
  } else {
    throw std::runtime_error("zarr: unsupported v2 compressor '" + std::string(id) + "'");
  }
}

ZarrArrayInfo parse_v3_array(std::string_view path, const nlohmann::json& array) {
  ZarrArrayInfo info;
  info.path = path;
  info.zarr_format = 3;
  info.shape = json_sizes(array.at("shape"));
  info.chunks = json_sizes(array.at("chunk_grid").at("configuration").at("chunk_shape"));
  validate_chunks(path, info.chunks);
  if (array.contains("dimension_names") && array.at("dimension_names").is_array()) {
    for (const auto& name : array.at("dimension_names")) {
      info.dimension_names.push_back(name.is_string() ? name.get<std::string>() : std::string{});
    }
  }
  info.attributes = array.value("attributes", nlohmann::json::object());
  info.fill_value = array.value("fill_value", nlohmann::json(nullptr));

  const nlohmann::json& data_type = array.at("data_type");
  if (data_type.is_string()) {
    info.dtype = zarr_dtype_from_name(data_type.get<std::string_view>(), info.element_bytes);
  } else if (data_type.is_object()) {
    const std::string_view name = data_type.at("name").get<std::string_view>();
    if (name == "fixed_length_utf32") {
      info.dtype = ZarrDtype::Utf32Fixed;
      info.element_bytes = data_type.at("configuration").at("length_bytes").get<std::size_t>();
    } else if (name == "vlen_utf8" || name == "vlen-utf8") {
      info.dtype = ZarrDtype::Utf8Variable;
    } else {
      info.dtype = zarr_dtype_from_name(name, info.element_bytes);
    }
  }
  parse_v3_codecs(array, info);
  return info;
}

ZarrArrayInfo parse_v2_array(std::string_view path, const nlohmann::json& array) {
  ZarrArrayInfo info;
  info.path = path;
  info.zarr_format = 2;
  info.shape = json_sizes(array.at("shape"));
  info.chunks = json_sizes(array.at("chunks"));
  validate_chunks(path, info.chunks);
  info.fill_value = array.value("fill_value", nlohmann::json(nullptr));
  info.fortran_order = array.value("order", std::string("C")) == "F";
  info.dimension_separator = array.value("dimension_separator", std::string(".")) == "/" ? '/' : '.';

  const nlohmann::json& dtype = array.at("dtype");
  if (!dtype.is_string()) {
    // Structured (list-of-lists) dtypes are not supported.
    throw std::runtime_error("zarr: unsupported structured v2 dtype in array '" + std::string(path) + "'");
  }
  info.dtype = parse_v2_dtype(dtype.get<std::string>(), info.endianness, info.element_bytes);

  if (array.contains("filters") && !array.at("filters").is_null()) {
    for (const auto& filter : array.at("filters")) {
      if (!filter.is_null()) {
        throw std::runtime_error("zarr: v2 filters are not supported (id '" + filter.value("id", std::string("?")) +
                                 "')");
      }
    }
  }
  parse_v2_compressor(array, info);
  return info;
}

// ---------------------------------------------------------------------------
// Chunk payload (de)compression
// ---------------------------------------------------------------------------

std::vector<std::byte> codec_decode(const ZarrArrayInfo& info, std::span<const std::byte> payload) {
  switch (info.codec) {
  case ZarrCodec::None:
    return {payload.begin(), payload.end()};
  case ZarrCodec::Blosc:
    return blosc_decompress_bytes(payload);
  case ZarrCodec::Zstd:
    return zstd_decompress_bytes(payload);
  case ZarrCodec::Gzip:
  case ZarrCodec::Zlib:
    return inflate_bytes(payload);
  case ZarrCodec::Lz4:
    return lz4_decompress_bytes(payload, product(info.chunks) * info.element_bytes);
  }
  throw std::runtime_error("zarr: unknown codec");
}

std::vector<std::byte> codec_encode(const ZarrArrayInfo& info, std::span<const std::byte> raw, std::size_t typesize) {
  switch (info.codec) {
  case ZarrCodec::None:
    return {raw.begin(), raw.end()};
  case ZarrCodec::Blosc:
    return blosc_compress_bytes(raw, info.codec_configuration, typesize);
  case ZarrCodec::Zstd:
    return zstd_compress_bytes(raw, info.codec_configuration.value("level", 3));
  case ZarrCodec::Gzip:
    return deflate_bytes(raw, info.codec_configuration.value("level", 6), true);
  case ZarrCodec::Zlib:
    return deflate_bytes(raw, info.codec_configuration.value("level", 6), false);
  case ZarrCodec::Lz4:
    return lz4_compress_bytes(raw);
  }
  throw std::runtime_error("zarr: unknown codec");
}

// ---------------------------------------------------------------------------
// Fill values
// ---------------------------------------------------------------------------

template <class T> T fill_value_as(const nlohmann::json& json) {
  if (json.is_null()) {
    return T{};
  }
  if (json.is_array()) {
    const double real = json.size() > 0 ? json[0].get<double>() : 0.0;
    const double imag = json.size() > 1 ? json[1].get<double>() : 0.0;
    if constexpr (is_complex<T>::value) {
      return T(static_cast<typename T::value_type>(real), static_cast<typename T::value_type>(imag));
    } else {
      return static_cast<T>(real);
    }
  }
  if (json.is_boolean()) {
    return static_cast<T>(json.get<bool>());
  }
  if (json.is_number()) {
    return static_cast<T>(json.get<double>());
  }
  if (json.is_string() && std::is_floating_point_v<T>) {
    const std::string text = json.get<std::string>();
    if (text == "NaN") {
      return std::numeric_limits<T>::quiet_NaN();
    }
    if (text == "Infinity") {
      return std::numeric_limits<T>::infinity();
    }
    if (text == "-Infinity") {
      return -std::numeric_limits<T>::infinity();
    }
  }
  return T{};
}

// ---------------------------------------------------------------------------
// Chunk copy helpers
// ---------------------------------------------------------------------------

/// Copy the intersection [begin, end) of a C-order chunk into a C-order output.
template <class T>
void copy_c_box(T* out, const std::vector<std::size_t>& out_strides, const T* chunk,
                const std::vector<std::size_t>& chunk_strides, const std::vector<std::size_t>& begin,
                const std::vector<std::size_t>& end, const std::vector<std::size_t>& region_start,
                const std::vector<std::size_t>& chunk_origin) {
  const std::size_t rank = begin.size();
  const std::size_t run = end[rank - 1] - begin[rank - 1];

  std::vector<std::size_t> index(rank);
  for (std::size_t d = 0; d + 1 < rank; ++d) {
    index[d] = begin[d];
  }

  while (true) {
    std::size_t out_offset = (begin[rank - 1] - region_start[rank - 1]) * out_strides[rank - 1];
    std::size_t chunk_offset = (begin[rank - 1] - chunk_origin[rank - 1]) * chunk_strides[rank - 1];
    for (std::size_t d = 0; d + 1 < rank; ++d) {
      out_offset += (index[d] - region_start[d]) * out_strides[d];
      chunk_offset += (index[d] - chunk_origin[d]) * chunk_strides[d];
    }
    std::memcpy(out + out_offset, chunk + chunk_offset, run * sizeof(T));

    if (rank == 1) {
      break;
    }
    std::size_t d = 0;
    for (; d + 1 < rank; ++d) {
      if (++index[d] < end[d]) {
        break;
      }
      index[d] = begin[d];
    }
    if (d + 1 == rank) {
      break;
    }
  }
}

/// Reorder a Fortran-order buffer into C order (rare; used only for v2
/// `order: "F"` arrays).
std::vector<std::byte> fortran_to_c(std::span<const std::byte> buffer, const std::vector<std::size_t>& extent,
                                    std::size_t element_bytes) {
  const std::size_t total = product(extent);
  std::vector<std::byte> output(buffer.size());
  const std::vector<std::size_t> c = c_strides(extent);
  std::vector<std::size_t> f_strides(extent.size(), 1);
  for (std::size_t d = 1; d < extent.size(); ++d) {
    f_strides[d] = f_strides[d - 1] * extent[d - 1];
  }
  std::vector<std::size_t> index(extent.size(), 0);
  for (std::size_t i = 0; i < total; ++i) {
    std::size_t c_offset = 0;
    std::size_t f_offset = 0;
    for (std::size_t d = 0; d < extent.size(); ++d) {
      c_offset += index[d] * c[d];
      f_offset += index[d] * f_strides[d];
    }
    std::memcpy(output.data() + c_offset * element_bytes, buffer.data() + f_offset * element_bytes, element_bytes);
    for (std::size_t d = extent.size(); d-- > 0;) {
      if (++index[d] < extent[d]) {
        break;
      }
      index[d] = 0;
    }
  }
  return output;
}

// ---------------------------------------------------------------------------
// String decoding
// ---------------------------------------------------------------------------

std::string utf32le_to_utf8(std::span<const std::byte> data, std::size_t width) {
  std::string result;
  const std::size_t count = width / sizeof(std::uint32_t);
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t code = 0;
    std::memcpy(&code, data.data() + i * sizeof(std::uint32_t), sizeof(code));
    if (code == 0) {
      break;
    }
    if (code < 0x80) {
      result.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      result.push_back(static_cast<char>(0xC0 | (code >> 6)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      result.push_back(static_cast<char>(0xE0 | (code >> 12)));
      result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      result.push_back(static_cast<char>(0xF0 | (code >> 18)));
      result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }
  return result;
}

/// Decode a UTF-8 string into a little-endian UTF-32 element buffer of
/// `width` bytes, zero-padded. Code points that do not fit are dropped.
void utf8_to_utf32le(const std::string& text, std::span<std::byte> element) {
  std::fill(element.begin(), element.end(), std::byte{0});
  const std::size_t capacity = element.size() / sizeof(std::uint32_t);
  std::size_t written = 0;
  for (std::size_t i = 0; i < text.size() && written < capacity;) {
    std::uint32_t code = 0;
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::size_t length = 1;
    if (lead < 0x80) {
      code = lead;
    } else if ((lead & 0xE0) == 0xC0 && i + 1 < text.size()) {
      code = lead & 0x1F;
      length = 2;
    } else if ((lead & 0xF0) == 0xE0 && i + 2 < text.size()) {
      code = lead & 0x0F;
      length = 3;
    } else if ((lead & 0xF8) == 0xF0 && i + 3 < text.size()) {
      code = lead & 0x07;
      length = 4;
    } else {
      ++i;
      continue;
    }
    bool valid = true;
    for (std::size_t k = 1; k < length; ++k) {
      const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
      if ((continuation & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      code = (code << 6) | (continuation & 0x3F);
    }
    if (!valid) {
      ++i;
      continue;
    }
    i += length;
    std::memcpy(element.data() + written * sizeof(std::uint32_t), &code, sizeof(code));
    ++written;
  }
}

} // namespace
// ---------------------------------------------------------------------------
// Public data-type helpers
// ---------------------------------------------------------------------------

std::size_t zarr_element_size(ZarrDtype type) {
  switch (type) {
  case ZarrDtype::Boolean:
  case ZarrDtype::Int8:
  case ZarrDtype::UInt8:
    return 1;
  case ZarrDtype::Int16:
  case ZarrDtype::UInt16:
  case ZarrDtype::Float16:
    return 2;
  case ZarrDtype::Int32:
  case ZarrDtype::UInt32:
  case ZarrDtype::Float32:
    return 4;
  case ZarrDtype::Int64:
  case ZarrDtype::UInt64:
  case ZarrDtype::Float64:
  case ZarrDtype::Complex64:
    return 8;
  case ZarrDtype::Complex128:
    return 16;
  case ZarrDtype::Raw:
  case ZarrDtype::Utf32Fixed:
  case ZarrDtype::Utf8Variable:
  case ZarrDtype::Unknown:
    return 0;
  }
  return 0;
}

std::string zarr_dtype_name(ZarrDtype type) {
  switch (type) {
  case ZarrDtype::Boolean:
    return "bool";
  case ZarrDtype::Int8:
    return "int8";
  case ZarrDtype::Int16:
    return "int16";
  case ZarrDtype::Int32:
    return "int32";
  case ZarrDtype::Int64:
    return "int64";
  case ZarrDtype::UInt8:
    return "uint8";
  case ZarrDtype::UInt16:
    return "uint16";
  case ZarrDtype::UInt32:
    return "uint32";
  case ZarrDtype::UInt64:
    return "uint64";
  case ZarrDtype::Float16:
    return "float16";
  case ZarrDtype::Float32:
    return "float32";
  case ZarrDtype::Float64:
    return "float64";
  case ZarrDtype::Complex64:
    return "complex64";
  case ZarrDtype::Complex128:
    return "complex128";
  case ZarrDtype::Raw:
    return "r8";
  case ZarrDtype::Utf32Fixed:
    return "fixed_length_utf32";
  case ZarrDtype::Utf8Variable:
    return "vlen_utf8";
  case ZarrDtype::Unknown:
    return "unknown";
  }
  return "unknown";
}

ZarrDtype zarr_dtype_from_name(std::string_view name, std::size_t& element_bytes) {
  static const std::vector<std::pair<std::string_view, ZarrDtype>> table = {
      {"bool", ZarrDtype::Boolean},
      {"i1", ZarrDtype::Int8},
      {"int8", ZarrDtype::Int8},
      {"i2", ZarrDtype::Int16},
      {"int16", ZarrDtype::Int16},
      {"i4", ZarrDtype::Int32},
      {"int32", ZarrDtype::Int32},
      {"i8", ZarrDtype::Int64},
      {"int64", ZarrDtype::Int64},
      {"u1", ZarrDtype::UInt8},
      {"uint8", ZarrDtype::UInt8},
      {"u2", ZarrDtype::UInt16},
      {"uint16", ZarrDtype::UInt16},
      {"u4", ZarrDtype::UInt32},
      {"uint32", ZarrDtype::UInt32},
      {"u8", ZarrDtype::UInt64},
      {"uint64", ZarrDtype::UInt64},
      {"f2", ZarrDtype::Float16},
      {"float16", ZarrDtype::Float16},
      {"f4", ZarrDtype::Float32},
      {"float32", ZarrDtype::Float32},
      {"f8", ZarrDtype::Float64},
      {"float64", ZarrDtype::Float64},
      {"c8", ZarrDtype::Complex64},
      {"complex64", ZarrDtype::Complex64},
      {"c16", ZarrDtype::Complex128},
      {"complex128", ZarrDtype::Complex128},
  };
  for (const auto& [key, value] : table) {
    if (name == key) {
      element_bytes = zarr_element_size(value);
      return value;
    }
  }
  // Raw bit types: r<N>.
  if (name.size() >= 2 && name[0] == 'r' && std::isdigit(static_cast<unsigned char>(name[1]))) {
    std::size_t bits = 0;
    const std::from_chars_result result = std::from_chars(name.data() + 1, name.data() + name.size(), bits);
    if (result.ec == std::errc{}) {
      element_bytes = bits / 8;
      return ZarrDtype::Raw;
    }
  }
  element_bytes = 0;
  return ZarrDtype::Unknown;
}

std::size_t ZarrArrayInfo::size() const { return product(shape); }

std::string zarr_chunk_key(const ZarrArrayInfo& info, const std::vector<std::size_t>& index) {
  fs::path key = info.path;
  if (info.zarr_format == 3) {
    key /= "c";
    for (const std::size_t i : index) {
      key /= std::to_string(i);
    }
  } else {
    // v2: the indices form a single component joined by the dimension
    // separator ('.' gives "0.0", '/' gives nested components "0/0").
    std::string indices;
    for (std::size_t d = 0; d < index.size(); ++d) {
      if (d != 0) {
        indices.push_back(info.dimension_separator);
      }
      indices += std::to_string(index[d]);
    }
    key /= indices;
  }
  return key.string();
}

std::vector<std::byte> zarr_decompress(const ZarrArrayInfo& info, std::span<const std::byte> compressed) {
  std::span<const std::byte> payload = compressed;
  if (info.crc32c) {
    if (payload.size() < sizeof(std::uint32_t)) {
      throw std::runtime_error("zarr: crc32c payload too short");
    }
    std::uint32_t stored = 0;
    std::memcpy(&stored, payload.data() + payload.size() - sizeof(stored), sizeof(stored));
    const std::uint32_t actual = crc32c(payload.subspan(0, payload.size() - sizeof(stored)));
    if (stored != actual) {
      throw std::runtime_error("zarr: crc32c checksum mismatch");
    }
    payload = payload.subspan(0, payload.size() - sizeof(stored));
  }
  return codec_decode(info, payload);
}

std::vector<std::byte> zarr_compress(const ZarrArrayInfo& info, std::span<const std::byte> raw) {
  std::vector<std::byte> payload = codec_encode(info, raw, std::max<std::size_t>(info.element_bytes, 1));
  if (info.crc32c) {
    const std::uint32_t checksum = crc32c(payload);
    const auto* bytes = reinterpret_cast<const std::byte*>(&checksum);
    payload.insert(payload.end(), bytes, bytes + sizeof(checksum));
  }
  return payload;
}

// ---------------------------------------------------------------------------
// ZarrStore
// ---------------------------------------------------------------------------

ZarrStore::ZarrStore(std::string root) : m_root(std::move(root)) {
  while (m_root.size() > 1 && m_root.back() == '/') {
    m_root.pop_back();
  }
}

bool ZarrStore::has_node(std::string_view path) const {
  const fs::path base = join_path(m_root, path);
  return file_exists(base / "zarr.json") || file_exists(base / ".zarray") || file_exists(base / ".zgroup");
}

bool ZarrStore::is_array(std::string_view path) const {
  const fs::path base = join_path(m_root, path);
  const fs::path v3 = base / "zarr.json";
  if (file_exists(v3)) {
    return parse_json(read_file(v3)).value("node_type", "") == "array";
  }
  return file_exists(base / ".zarray");
}

bool ZarrStore::is_group(std::string_view path) const {
  const fs::path base = join_path(m_root, path);
  const fs::path v3 = base / "zarr.json";
  if (file_exists(v3)) {
    return parse_json(read_file(v3)).value("node_type", "") == "group";
  }
  return file_exists(base / ".zgroup");
}

ZarrArrayInfo ZarrStore::read_array_info(std::string_view path) const {
  const fs::path base = join_path(m_root, path);
  const fs::path v3 = base / "zarr.json";
  if (file_exists(v3)) {
    const nlohmann::json json = parse_json(read_file(v3));
    return parse_v3_array(path, json);
  }
  const fs::path v2 = base / ".zarray";
  if (file_exists(v2)) {
    const nlohmann::json json = parse_json(read_file(v2));
    return parse_v2_array(path, json);
  }
  throw std::runtime_error("zarr: no array metadata at '" + std::string(path) + "'");
}

nlohmann::json ZarrStore::read_attributes(std::string_view path) const {
  const fs::path base = join_path(m_root, path);
  const fs::path v3 = base / "zarr.json";
  if (file_exists(v3)) {
    return parse_json(read_file(v3)).value("attributes", nlohmann::json::object());
  }
  const fs::path v2 = base / ".zattrs";
  if (file_exists(v2)) {
    return parse_json(read_file(v2));
  }
  return nlohmann::json::object();
}

std::vector<std::string> ZarrStore::list_children(std::string_view path) const {
  const fs::path directory = join_path(m_root, path);
  std::vector<std::string> children;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(directory, error)) {
    if (entry.is_directory(error)) {
      const std::string name = entry.path().filename().string();
      if (!name.empty() && name.front() != '.') {
        children.push_back(name);
      }
    }
  }
  std::sort(children.begin(), children.end());
  return children;
}

bool ZarrStore::has_chunk(const ZarrArrayInfo& info, const std::vector<std::size_t>& index) const {
  return file_exists(join_path(m_root, zarr_chunk_key(info, index)));
}

std::vector<std::byte> ZarrStore::read_chunk_raw(const ZarrArrayInfo& info,
                                                 const std::vector<std::size_t>& index) const {
  return read_file(join_path(m_root, zarr_chunk_key(info, index)));
}

// ---------------------------------------------------------------------------
// zarr_read
// ---------------------------------------------------------------------------

template <class T>
xt::xarray<T> zarr_read(const ZarrStore& store, const ZarrArrayInfo& info, const std::vector<ZarrSlice>& region) {
  const std::size_t rank = info.rank();
  if (region.size() > rank) {
    throw std::invalid_argument("zarr_read: more slices than dimensions");
  }
  if (info.element_bytes != sizeof(T)) {
    throw std::invalid_argument("zarr_read: element size mismatch for array '" + info.path + "'");
  }

  std::vector<ZarrSlice> selection = region;
  selection.resize(rank);

  std::vector<std::size_t> start(rank, 0);
  std::vector<std::size_t> count(rank, 0);
  for (std::size_t d = 0; d < rank; ++d) {
    start[d] = std::min(selection[d].start, info.shape[d]);
    count[d] = std::min(selection[d].count, info.shape[d] - start[d]);
  }

  xt::xarray<T> out = xt::xarray<T>::from_shape(count);
  std::fill(out.begin(), out.end(), fill_value_as<T>(info.fill_value));
  if (std::any_of(count.begin(), count.end(), [](std::size_t n) { return n == 0; })) {
    return out;
  }

  const std::vector<std::size_t> out_strides = c_strides(count);
  const std::size_t full_chunk_elements = product(info.chunks);

  std::vector<std::size_t> chunk_begin(rank, 0);
  std::vector<std::size_t> chunk_end(rank, 0);
  for (std::size_t d = 0; d < rank; ++d) {
    chunk_begin[d] = start[d] / info.chunks[d];
    chunk_end[d] = (start[d] + count[d] - 1) / info.chunks[d] + 1;
  }

  std::vector<std::size_t> chunk_index = chunk_begin;
  while (true) {
    std::vector<std::size_t> chunk_origin(rank, 0);
    std::vector<std::size_t> begin(rank, 0);
    std::vector<std::size_t> end(rank, 0);
    bool empty = false;
    for (std::size_t d = 0; d < rank; ++d) {
      chunk_origin[d] = chunk_index[d] * info.chunks[d];
      begin[d] = std::max(start[d], chunk_origin[d]);
      end[d] = std::min({start[d] + count[d], chunk_origin[d] + info.chunks[d], info.shape[d]});
      if (end[d] <= begin[d]) {
        empty = true;
      }
    }

    if (!empty && store.has_chunk(info, chunk_index)) {
      std::vector<std::byte> raw = zarr_decompress(info, store.read_chunk_raw(info, chunk_index));

      // Edge chunks may be stored either padded to the full chunk shape or
      // trimmed to the valid region.
      std::vector<std::size_t> stored(rank, 0);
      if (raw.size() == full_chunk_elements * sizeof(T)) {
        stored = info.chunks;
      } else {
        for (std::size_t d = 0; d < rank; ++d) {
          stored[d] = std::min(info.chunks[d], info.shape[d] - chunk_origin[d]);
        }
        if (raw.size() != product(stored) * sizeof(T)) {
          throw std::runtime_error("zarr: unexpected chunk size for '" + info.path + "'");
        }
      }

      if (needs_byte_swap(info.endianness) && sizeof(T) > 1) {
        // View the raw chunk bytes as a span of T elements and swap each one
        // in place. The buffer comes from ::operator new and is therefore
        // aligned for T; the assert documents that assumption.
        std::span<T> values(reinterpret_cast<T*>(raw.data()), raw.size() / sizeof(T));
        assert(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(T) == 0);
        for (T& value : values) {
          swap_scalar_endianness(value);
        }
      }
      if (info.fortran_order) {
        raw = fortran_to_c(raw, stored, sizeof(T));
      }

      const T* chunk = reinterpret_cast<const T*>(raw.data());
      const std::vector<std::size_t> chunk_strides = c_strides(stored);
      copy_c_box<T>(out.data(), out_strides, chunk, chunk_strides, begin, end, start, chunk_origin);
    }

    std::size_t d = 0;
    for (; d < rank; ++d) {
      if (++chunk_index[d] < chunk_end[d]) {
        break;
      }
      chunk_index[d] = chunk_begin[d];
    }
    if (d == rank) {
      break;
    }
  }
  return out;
}

#define RASTRO_INSTANTIATE_READ(T)                                                                                     \
  template xt::xarray<T> zarr_read<T>(const ZarrStore&, const ZarrArrayInfo&, const std::vector<ZarrSlice>&);

RASTRO_INSTANTIATE_READ(bool)
RASTRO_INSTANTIATE_READ(std::int8_t)
RASTRO_INSTANTIATE_READ(std::int16_t)
RASTRO_INSTANTIATE_READ(std::int32_t)
RASTRO_INSTANTIATE_READ(std::int64_t)
RASTRO_INSTANTIATE_READ(std::uint8_t)
RASTRO_INSTANTIATE_READ(std::uint16_t)
RASTRO_INSTANTIATE_READ(std::uint32_t)
RASTRO_INSTANTIATE_READ(std::uint64_t)
RASTRO_INSTANTIATE_READ(float)
RASTRO_INSTANTIATE_READ(double)
RASTRO_INSTANTIATE_READ(std::complex<float>)
RASTRO_INSTANTIATE_READ(std::complex<double>)

#undef RASTRO_INSTANTIATE_READ

// ---------------------------------------------------------------------------
// String arrays
// ---------------------------------------------------------------------------

std::vector<std::string> zarr_read_strings(const ZarrStore& store, const ZarrArrayInfo& info) {
  if (!info.is_string()) {
    throw std::invalid_argument("zarr_read_strings: array is not a string array");
  }
  if (info.rank() != 1) {
    throw std::runtime_error("zarr_read_strings: only one-dimensional string arrays are supported");
  }

  const std::size_t count = info.shape[0];
  const std::size_t chunk = info.chunks[0];
  std::vector<std::string> result(count);

  for (std::size_t chunk_index = 0; chunk_index * chunk < count; ++chunk_index) {
    if (!store.has_chunk(info, {chunk_index})) {
      continue;
    }
    const std::vector<std::byte> bytes = zarr_decompress(info, store.read_chunk_raw(info, {chunk_index}));
    const std::size_t origin = chunk_index * chunk;
    const std::size_t extent = std::min(chunk, count - origin);

    if (info.dtype == ZarrDtype::Utf32Fixed) {
      const std::size_t width = info.element_bytes;
      const std::size_t available = width == 0 ? 0 : std::min(extent, bytes.size() / width);
      for (std::size_t i = 0; i < available; ++i) {
        result[origin + i] = utf32le_to_utf8(std::span<const std::byte>(bytes).subspan(i * width, width), width);
      }
    } else {
      // vlen_utf8: for each element, a 4-byte little-endian length then UTF-8.
      std::size_t cursor = 0;
      for (std::size_t i = 0; i < extent; ++i) {
        if (cursor + sizeof(std::uint32_t) > bytes.size()) {
          throw std::runtime_error("zarr: truncated vlen_utf8 chunk");
        }
        std::uint32_t length = 0;
        std::memcpy(&length, bytes.data() + cursor, sizeof(length));
        cursor += sizeof(length);
        if (cursor + length > bytes.size()) {
          throw std::runtime_error("zarr: truncated vlen_utf8 string");
        }
        result[origin + i].assign(reinterpret_cast<const char*>(bytes.data() + cursor), length);
        cursor += length;
      }
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// ZarrWriter
// ---------------------------------------------------------------------------

ZarrWriter::ZarrWriter(std::string root) : m_root(std::move(root)) {
  while (m_root.size() > 1 && m_root.back() == '/') {
    m_root.pop_back();
  }
}

void ZarrWriter::create_group(std::string_view path, const nlohmann::json& attributes) {
  nlohmann::json json;
  json["zarr_format"] = 3;
  json["node_type"] = "group";
  if (!attributes.empty()) {
    json["attributes"] = attributes;
  }
  const fs::path key = path.empty() ? fs::path("zarr.json") : fs::path(path) / "zarr.json";
  write_file_atomic(join_path(m_root, key), as_byte_span(json.dump(4)));
}

void ZarrWriter::write_attributes(std::string_view path, const nlohmann::json& attributes) {
  const fs::path full = join_path(m_root, path) / "zarr.json";
  nlohmann::json json = file_exists(full) ? parse_json(read_file(full)) : nlohmann::json::object();
  json["attributes"] = attributes;
  write_file_atomic(full, as_byte_span(json.dump(4)));
}

void ZarrWriter::create_array(const ZarrArrayInfo& info) {
  nlohmann::json json;
  json["zarr_format"] = 3;
  json["node_type"] = "array";
  json["shape"] = info.shape;
  json["chunk_grid"] = {{"name", "regular"}, {"configuration", {{"chunk_shape", info.chunks}}}};
  json["chunk_key_encoding"] = {{"name", "default"}, {"configuration", {{"separator", "/"}}}};
  json["fill_value"] = info.fill_value;
  json["codecs"] = nlohmann::json::array();
  const std::string_view endian = info.endianness == std::endian::big ? "big" : "little";
  json["codecs"].push_back({{"name", "bytes"}, {"configuration", {{"endian", endian}}}});
  if (info.dtype == ZarrDtype::Utf32Fixed) {
    json["data_type"] = {{"name", "fixed_length_utf32"}, {"configuration", {{"length_bytes", info.element_bytes}}}};
  } else {
    json["data_type"] = zarr_dtype_name(info.dtype);
  }
  if (info.codec != ZarrCodec::None) {
    std::string_view name;
    switch (info.codec) {
    case ZarrCodec::Blosc:
      name = "blosc";
      break;
    case ZarrCodec::Zstd:
      name = "zstd";
      break;
    case ZarrCodec::Gzip:
      name = "gzip";
      break;
    case ZarrCodec::Zlib:
      name = "zlib";
      break;
    case ZarrCodec::Lz4:
      name = "lz4";
      break;
    case ZarrCodec::None:
      break;
    }
    json["codecs"].push_back({{"name", name}, {"configuration", info.codec_configuration}});
  }
  if (info.crc32c) {
    json["codecs"].push_back({{"name", "crc32c"}});
  }
  if (!info.dimension_names.empty()) {
    json["dimension_names"] = info.dimension_names;
  }
  if (!info.attributes.empty()) {
    json["attributes"] = info.attributes;
  }
  write_file_atomic(join_path(m_root, fs::path(info.path) / "zarr.json"), as_byte_span(json.dump(4)));
}

void ZarrWriter::write_chunk(const ZarrArrayInfo& info, const std::vector<std::size_t>& index,
                             std::span<const std::byte> raw) {
  write_file_atomic(join_path(m_root, zarr_chunk_key(info, index)), raw);
}

template <class T> void ZarrWriter::write_array(const ZarrArrayInfo& info, const xt::xarray<T>& data) {
  const std::size_t rank = info.rank();
  const std::size_t full_chunk_elements = product(info.chunks);
  std::vector<std::size_t> chunk_count(rank, 0);
  for (std::size_t d = 0; d < rank; ++d) {
    chunk_count[d] = (info.shape[d] + info.chunks[d] - 1) / info.chunks[d];
  }

  std::vector<std::size_t> index(rank, 0);
  while (true) {
    std::vector<std::size_t> origin(rank, 0);
    std::vector<std::size_t> extent(rank, 0);
    for (std::size_t d = 0; d < rank; ++d) {
      origin[d] = index[d] * info.chunks[d];
      extent[d] = std::min(info.chunks[d], info.shape[d] - origin[d]);
    }

    // Gather the chunk in C order, padding the edge to the full chunk shape.
    std::vector<std::byte> raw(full_chunk_elements * sizeof(T));
    T* buffer = reinterpret_cast<T*>(raw.data());
    std::vector<std::size_t> position(rank, 0);
    const std::size_t elements = product(extent);
    for (std::size_t i = 0; i < elements; ++i) {
      std::size_t value = i;
      for (std::size_t d = rank; d-- > 0;) {
        position[d] = value % extent[d];
        value /= extent[d];
      }
      std::size_t source_offset = 0;
      for (std::size_t d = 0; d < rank; ++d) {
        source_offset = source_offset * info.shape[d] + (origin[d] + position[d]);
      }
      std::size_t target_offset = 0;
      for (std::size_t d = 0; d < rank; ++d) {
        target_offset = target_offset * info.chunks[d] + position[d];
      }
      buffer[target_offset] = data.data()[source_offset];
    }

    // Honour the declared endianness: swap every element when it differs
    // from the host's. Complex elements are swapped per component.
    if (needs_byte_swap(info.endianness) && sizeof(T) > 1) {
      for (std::size_t i = 0; i < full_chunk_elements; ++i) {
        swap_scalar_endianness(buffer[i]);
      }
    }
    write_chunk(info, index, zarr_compress(info, raw));

    std::size_t d = 0;
    for (; d < rank; ++d) {
      if (++index[d] < chunk_count[d]) {
        break;
      }
      index[d] = 0;
    }
    if (d == rank) {
      break;
    }
  }
}

template <class T>
void ZarrWriter::write_region(const ZarrArrayInfo& info, const std::vector<std::size_t>& start,
                              const xt::xarray<T>& data) {
  const std::size_t rank = info.rank();
  if (start.size() != rank || data.dimension() != rank) {
    throw std::invalid_argument("zarr write_region: rank mismatch for '" + info.path + "'");
  }
  std::vector<std::size_t> region(rank, 0);
  for (std::size_t d = 0; d < rank; ++d) {
    region[d] = data.shape()[d];
    if (start[d] + region[d] > info.shape[d]) {
      throw std::out_of_range("zarr write_region: region exceeds array '" + info.path + "'");
    }
  }
  if (std::any_of(region.begin(), region.end(), [](std::size_t n) { return n == 0; })) {
    return;
  }

  std::vector<std::size_t> chunk_begin(rank, 0);
  std::vector<std::size_t> chunk_end(rank, 0);
  for (std::size_t d = 0; d < rank; ++d) {
    chunk_begin[d] = start[d] / info.chunks[d];
    chunk_end[d] = (start[d] + region[d] - 1) / info.chunks[d] + 1;
  }

  const std::vector<std::size_t> data_strides = c_strides(region);
  const std::size_t full_chunk_elements = product(info.chunks);
  const std::vector<std::size_t> chunk_strides = c_strides(info.chunks);
  const ZarrStore store(m_root);

  std::vector<std::size_t> chunk_index = chunk_begin;
  while (true) {
    std::vector<std::size_t> origin(rank, 0);
    std::vector<std::size_t> begin(rank, 0);
    std::vector<std::size_t> end(rank, 0);
    bool full = true;
    for (std::size_t d = 0; d < rank; ++d) {
      origin[d] = chunk_index[d] * info.chunks[d];
      begin[d] = std::max(start[d], origin[d]);
      end[d] = std::min({start[d] + region[d], origin[d] + info.chunks[d], info.shape[d]});
      if (begin[d] != origin[d] || end[d] != origin[d] + info.chunks[d]) {
        full = false;
      }
    }

    std::vector<std::byte> raw(full_chunk_elements * sizeof(T));
    T* buffer = reinterpret_cast<T*>(raw.data());
    if (full) {
      // Entire chunk overwritten below: no need to read the previous payload.
    } else if (store.has_chunk(info, chunk_index)) {
      const std::vector<std::byte> existing = zarr_decompress(info, store.read_chunk_raw(info, chunk_index));
      if (existing.size() >= raw.size()) {
        std::memcpy(raw.data(), existing.data(), raw.size());
        if (needs_byte_swap(info.endianness)) {
          for (std::size_t i = 0; i < full_chunk_elements; ++i) {
            swap_scalar_endianness(buffer[i]);
          }
        }
      } else {
        std::fill(buffer, buffer + full_chunk_elements, fill_value_as<T>(info.fill_value));
      }
    } else {
      std::fill(buffer, buffer + full_chunk_elements, fill_value_as<T>(info.fill_value));
    }

    const std::size_t run = end[rank - 1] - begin[rank - 1];
    if (run > 0) {
      std::vector<std::size_t> index(rank, 0);
      for (std::size_t d = 0; d + 1 < rank; ++d) {
        index[d] = begin[d];
      }
      while (true) {
        std::size_t data_offset = (begin[rank - 1] - start[rank - 1]) * data_strides[rank - 1];
        std::size_t chunk_offset = (begin[rank - 1] - origin[rank - 1]) * chunk_strides[rank - 1];
        for (std::size_t d = 0; d + 1 < rank; ++d) {
          data_offset += (index[d] - start[d]) * data_strides[d];
          chunk_offset += (index[d] - origin[d]) * chunk_strides[d];
        }
        std::memcpy(reinterpret_cast<std::byte*>(buffer + chunk_offset), data.data() + data_offset, run * sizeof(T));
        if (rank == 1) {
          break;
        }
        std::size_t d = 0;
        for (; d + 1 < rank; ++d) {
          if (++index[d] < end[d]) {
            break;
          }
          index[d] = begin[d];
        }
        if (d + 1 == rank) {
          break;
        }
      }
    }

    if (needs_byte_swap(info.endianness) && sizeof(T) > 1) {
      for (std::size_t i = 0; i < full_chunk_elements; ++i) {
        swap_scalar_endianness(buffer[i]);
      }
    }
    write_chunk(info, chunk_index, zarr_compress(info, raw));

    std::size_t d = 0;
    for (; d < rank; ++d) {
      if (++chunk_index[d] < chunk_end[d]) {
        break;
      }
      chunk_index[d] = chunk_begin[d];
    }
    if (d == rank) {
      break;
    }
  }
}

void ZarrWriter::write_string_array(const ZarrArrayInfo& info, const std::vector<std::string>& values) {
  if (info.rank() != 1) {
    throw std::invalid_argument("zarr write_string_array: only one-dimensional arrays are supported");
  }
  if (info.dtype != ZarrDtype::Utf32Fixed) {
    throw std::invalid_argument("zarr write_string_array: expected fixed_length_utf32");
  }
  const std::size_t width = info.element_bytes;
  const std::size_t count = info.shape[0];
  const std::size_t chunk = info.chunks[0];

  for (std::size_t chunk_index = 0; chunk_index * chunk < count; ++chunk_index) {
    const std::size_t origin = chunk_index * chunk;
    const std::size_t extent = std::min(chunk, count - origin);
    std::vector<std::byte> raw(extent * width);
    for (std::size_t i = 0; i < extent; ++i) {
      const std::string& text = values[origin + i];
      utf8_to_utf32le(text, std::span<std::byte>(raw).subspan(i * width, width));
    }
    write_chunk(info, {chunk_index}, zarr_compress(info, raw));
  }
}

#define RASTRO_INSTANTIATE_WRITE(T)                                                                                    \
  template void ZarrWriter::write_array<T>(const ZarrArrayInfo&, const xt::xarray<T>&);                                \
  template void ZarrWriter::write_region<T>(const ZarrArrayInfo&, const std::vector<std::size_t>&,                     \
                                            const xt::xarray<T>&);

RASTRO_INSTANTIATE_WRITE(bool)
RASTRO_INSTANTIATE_WRITE(std::int8_t)
RASTRO_INSTANTIATE_WRITE(std::int16_t)
RASTRO_INSTANTIATE_WRITE(std::int32_t)
RASTRO_INSTANTIATE_WRITE(std::int64_t)
RASTRO_INSTANTIATE_WRITE(std::uint8_t)
RASTRO_INSTANTIATE_WRITE(std::uint16_t)
RASTRO_INSTANTIATE_WRITE(std::uint32_t)
RASTRO_INSTANTIATE_WRITE(std::uint64_t)
RASTRO_INSTANTIATE_WRITE(float)
RASTRO_INSTANTIATE_WRITE(double)
RASTRO_INSTANTIATE_WRITE(std::complex<float>)
RASTRO_INSTANTIATE_WRITE(std::complex<double>)

#undef RASTRO_INSTANTIATE_WRITE

double unix_seconds_to_mjd(double seconds) { return 40587.0 + seconds / 86400.0; }

} // namespace rastro
