#pragma once

// From-scratch Zarr v2 / v3 reader and writer built on xtensor.
//
// The implementation is intentionally self-contained: it parses the metadata
// JSON itself, decodes the codec pipeline with the compression libraries
// provided by Nix (Blosc, Zstandard, zlib, LZ4) and represents all
// multi-dimensional data with `xt::xarray`.  There is no dependency on
// xtensor-zarr or zarray.
//
// Thread-safety: `ZarrStore` is immutable and every method is `const` and
// reentrant (no shared mutable state, a fresh file handle per chunk read), so
// a single `ZarrStore` may be shared and read from multiple threads.  The
// codec functions are stateless and use per-call contexts.  `ZarrWriter`
// writes distinct files per chunk; concurrently writing the same key is not
// supported.

#include <bit>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <xtensor/containers/xarray.hpp>

namespace rastro {

/// Scalar element types handled by the Zarr layer.
enum class ZarrDtype {
  Boolean,
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  Float16,
  Float32,
  Float64,
  Complex64,
  Complex128,
  /// Opaque `rN` raw byte types.
  Raw,
  /// Zarr v3 `fixed_length_utf32` extension data type.
  Utf32Fixed,
  /// Zarr v3 `vlen_utf8` / v2 `|U` string data.
  Utf8Variable,
  Unknown,
};

/// Compression codec applied to a chunk.
enum class ZarrCodec { None, Blosc, Zstd, Gzip, Zlib, Lz4 };

/// Sentinel meaning "until the end of the dimension".
inline constexpr std::size_t zarr_all = static_cast<std::size_t>(-1);

/// Selection of one dimension: `count` elements starting at `start`.
struct ZarrSlice {
  std::size_t start = 0;
  std::size_t count = zarr_all;
};

/// Array metadata (Zarr v2 or v3). Never carries bulk data.
struct ZarrArrayInfo {
  std::string path;
  int zarr_format = 3;
  std::vector<std::size_t> shape;
  std::vector<std::size_t> chunks;
  std::vector<std::string> dimension_names;
  ZarrDtype dtype = ZarrDtype::Unknown;
  /// Size in bytes of one element (for `Raw`, the `rN` byte count).
  std::size_t element_bytes = 0;
  /// Byte order declared by the v3 `bytes` codec or the v2 dtype prefix.
  std::endian endianness = std::endian::little;
  /// True for a column-major (Fortran) chunk layout (v2 `order: "F"`).
  bool fortran_order = false;
  ZarrCodec codec = ZarrCodec::None;
  nlohmann::json codec_configuration = nlohmann::json::object();
  /// True when the v3 `crc32c` codec is part of the pipeline.
  bool crc32c = false;
  nlohmann::json fill_value = nullptr;
  nlohmann::json attributes = nlohmann::json::object();
  /// Zarr v2 chunk key separator ('.' or '/'); unused for v3.
  char dimension_separator = '.';

  std::size_t rank() const { return shape.size(); }
  std::size_t size() const;
  bool is_string() const { return dtype == ZarrDtype::Utf32Fixed || dtype == ZarrDtype::Utf8Variable; }
};

/// Byte size of a scalar type (0 for variable length strings / unknown).
std::size_t zarr_element_size(ZarrDtype type);

/// Canonical Zarr v3 name for a scalar type ("float64", ...). For `Utf32Fixed`
/// and `Utf8Variable` the extension names are returned.
std::string zarr_dtype_name(ZarrDtype type);

/// Parse a Zarr v3 data type name into a `ZarrDtype`.
ZarrDtype zarr_dtype_from_name(std::string_view name, std::size_t& element_bytes);

/// Read-only view over a local Zarr v2/v3 store. Immutable and thread-safe.
class ZarrStore {
public:
  explicit ZarrStore(std::string root);

  const std::string& root() const { return m_root; }

  /// True when a metadata document exists at `path`.
  bool has_node(std::string_view path) const;
  /// True when `path` is a Zarr array (v2 `.zarray` or v3 `zarr.json`).
  bool is_array(std::string_view path) const;
  /// True when `path` is a Zarr group (v2 `.zgroup` or v3 `zarr.json`).
  bool is_group(std::string_view path) const;

  /// Parse the array metadata at `path`.
  ZarrArrayInfo read_array_info(std::string_view path) const;
  /// Read the attributes of an array or group.
  nlohmann::json read_attributes(std::string_view path) const;
  /// Names of the direct children (arrays and groups) of a group.
  std::vector<std::string> list_children(std::string_view path) const;

  /// True when the chunk is present on disk.
  bool has_chunk(const ZarrArrayInfo& info, const std::vector<std::size_t>& index) const;
  /// Raw (still encoded) chunk bytes.
  std::vector<std::byte> read_chunk_raw(const ZarrArrayInfo& info, const std::vector<std::size_t>& index) const;

private:
  std::string m_root;
};

/// Key of a chunk relative to the store root.
std::string zarr_chunk_key(const ZarrArrayInfo& info, const std::vector<std::size_t>& index);

/// Decode a chunk payload (codec pipeline applied in reverse) into raw bytes.
std::vector<std::byte> zarr_decompress(const ZarrArrayInfo& info, std::span<const std::byte> compressed);

/// Encode raw bytes into a chunk payload.
std::vector<std::byte> zarr_compress(const ZarrArrayInfo& info, std::span<const std::byte> raw);

/// Read a region of a numeric array into an xtensor of the region shape.
///
/// `region` holds at most one `ZarrSlice` per dimension; missing trailing
/// slices select the whole dimension. Only the chunks intersecting the region
/// are read, so the memory footprint is bounded by the region plus one chunk.
/// Missing chunks are filled with the array's fill value.
template <class T>
xt::xarray<T> zarr_read(const ZarrStore& store, const ZarrArrayInfo& info, const std::vector<ZarrSlice>& region = {});

/// Read a string array (fixed-length UTF-32 or variable-length UTF-8).
std::vector<std::string> zarr_read_strings(const ZarrStore& store, const ZarrArrayInfo& info);

/// Writer for local Zarr v3 stores. One instance may be used from one thread
/// at a time; distinct chunks/arrays can be written concurrently.
class ZarrWriter {
public:
  explicit ZarrWriter(std::string root);

  const std::string& root() const { return m_root; }

  /// Create a group (and its ancestors) with the given attributes.
  void create_group(std::string_view path, const nlohmann::json& attributes = nlohmann::json::object());
  /// Create an array described by `info` (v3 metadata).
  void create_array(const ZarrArrayInfo& info);
  /// Write the attributes of an existing array or group.
  void write_attributes(std::string_view path, const nlohmann::json& attributes);

  /// Write one encoded chunk payload. `raw` must already be encoded with the
  /// array's codec (`zarr_compress`).
  void write_chunk(const ZarrArrayInfo& info, const std::vector<std::size_t>& index, std::span<const std::byte> raw);
  /// Split an xtensor into chunks, encode and write them.
  template <class T> void write_array(const ZarrArrayInfo& info, const xt::xarray<T>& data);

private:
  std::string m_root;
};

/// Convert a Unix-second time coordinate to modified Julian date.
double unix_seconds_to_mjd(double seconds);

} // namespace rastro
