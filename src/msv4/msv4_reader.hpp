#pragma once

#include "meas/measurement_set.hpp"
#include "msv4/measurement_set.hpp"

#include <optional>
#include <string>

namespace rastro {

/// MSv4 reader: adapts an MSv4 processing set to the format-independent
/// property API used by the conversion engine.
class Msv4Reader : public MeasurementSetReader {
public:
  explicit Msv4Reader(const std::string& path);

  std::string format_name() const override { return "msv4"; }
  const MeasurementSetMetadata& metadata() const override { return m_metadata; }
  const VisibilityLayout& layout() const override { return m_layout; }

  VisibilityTile read_tile(std::size_t time_start, std::size_t baseline_start, std::size_t num_times,
                           std::size_t num_baselines) const override;

private:
  ProcessingSet m_set;
  std::string m_partition;
  std::optional<MeasurementSetV4> m_ms;
  MeasurementSetMetadata m_metadata;
  VisibilityLayout m_layout;
  bool m_has_flag = false;
  bool m_has_weight = false;
  bool m_has_time_centroid = false;
  bool m_has_effective_integration_time = false;
};

} // namespace rastro
