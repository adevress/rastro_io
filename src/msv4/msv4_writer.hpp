#pragma once

#include "meas/measurement_set.hpp"
#include "msv4/zarr_io.hpp"

#include <string>

namespace rastro {

/// On-disk encoding used for MSv4 string coordinates.
///
/// Zarr v3 defines two string representations: the variable-length `string`
/// data type (UTF-8, `vlen-utf8` codec) and the `fixed_length_utf32` extension.
/// The MSv4 specification documents `fixed_length_utf32`, but UTF-8 is the
/// default here because it is far more compact for the short, mostly-ASCII
/// labels these arrays hold.
enum class Msv4StringEncoding {
  /// Registered `string` data type with the `vlen-utf8` codec (default).
  Utf8,
  /// `fixed_length_utf32` extension data type, as documented by the MSv4 spec.
  Utf32,
};

/// MSv4 writer: turns the format-independent property API into an MSv4
/// processing set stored as a Zarr v3 hierarchy.
///
/// Metadata is written once when `create` is called. Bulk visibility data is
/// streamed tile by tile through `write_tile`, and each tile is written as a
/// Zarr region so that the peak memory footprint is one tile plus one chunk.
class Msv4Writer : public MeasurementSetWriter {
public:
  explicit Msv4Writer(std::string root, Msv4StringEncoding strings = Msv4StringEncoding::Utf8);

  const std::string& root() const { return m_writer.root(); }
  const std::string& partition() const { return m_partition; }

  void create(const MeasurementSetMetadata& metadata, const VisibilityLayout& layout) override;
  void write_tile(const VisibilityTile& tile) override;
  void finalize() override;

private:
  ZarrWriter m_writer;
  Msv4StringEncoding m_string_encoding = Msv4StringEncoding::Utf8;
  std::string m_partition;
  VisibilityLayout m_layout;
  ZarrArrayInfo m_visibility;
  ZarrArrayInfo m_flag;
  ZarrArrayInfo m_weight;
  ZarrArrayInfo m_uvw;
  ZarrArrayInfo m_effective_integration_time;
  ZarrArrayInfo m_time_centroid;
  bool m_created = false;
};

} // namespace rastro
