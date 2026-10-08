#pragma once

#include "meas/measurement_set.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace rastro {

/// MSv2 writer: turns the format-independent property API into a casacore
/// MeasurementSet v2.
///
/// The main table and its subtables are created in `create`. Bulk visibility
/// data is appended tile by tile in `write_tile`, so the peak memory footprint
/// is one tile.
class Msv2Writer : public MeasurementSetWriter {
public:
  explicit Msv2Writer(std::string path);
  ~Msv2Writer() override;

  Msv2Writer(const Msv2Writer&) = delete;
  Msv2Writer& operator=(const Msv2Writer&) = delete;

  const std::string& path() const { return m_path; }

  void create(const MeasurementSetMetadata& metadata, const VisibilityLayout& layout) override;
  void write_tile(const VisibilityTile& tile) override;
  void finalize() override;

private:
  struct Impl;
  std::string m_path;
  std::unique_ptr<Impl> m_impl;
  VisibilityLayout m_layout;
  std::size_t m_rows_written = 0;
  std::size_t m_num_channels = 0;
  std::size_t m_num_correlations = 0;
  bool m_created = false;
};

} // namespace rastro
