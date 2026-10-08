#pragma once

#include "meas/measurement_set.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace casacore {
class MeasurementSet;
}

namespace rastro {

/// MSv2 reader: adapts a MeasurementSet v2 (casacore tables) to the
/// format-independent property API.
///
/// Metadata and the small subtables are read once at construction. Bulk
/// visibility data is read tile by tile, using casacore's row-range column
/// access so that only the requested cells are touched.
class Msv2Reader : public MeasurementSetReader {
public:
  explicit Msv2Reader(const std::string& path);
  ~Msv2Reader() override;

  Msv2Reader(const Msv2Reader&) = delete;
  Msv2Reader& operator=(const Msv2Reader&) = delete;

  std::string format_name() const override { return "msv2"; }
  const MeasurementSetMetadata& metadata() const override { return m_metadata; }
  const VisibilityLayout& layout() const override { return m_layout; }

  VisibilityTile read_tile(std::size_t time_start, std::size_t baseline_start, std::size_t num_times,
                           std::size_t num_baselines) const override;

private:
  struct Impl;
  Impl* m_impl = nullptr;
  MeasurementSetMetadata m_metadata;
  VisibilityLayout m_layout;
};

} // namespace rastro
