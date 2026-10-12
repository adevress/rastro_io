#include "msv4_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace rastro {
namespace {

bool has_child(const MeasurementSetV4& ms, const std::string& name) {
  for (const std::string& child : ms.children()) {
    if (child == name) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> json_strings(const nlohmann::json& value) {
  if (value.is_array()) {
    return value.get<std::vector<std::string>>();
  }
  if (value.is_string()) {
    return {value.get<std::string>()};
  }
  return {};
}

/// Parse a `scan-<n>` name back to its number, or -1.
std::int32_t parse_scan(const std::string& name) {
  if (name.rfind("scan-", 0) != 0) {
    return -1;
  }
  try {
    return static_cast<std::int32_t>(std::stoi(name.substr(5)));
  } catch (const std::exception&) {
    return -1;
  }
}

/// Index of `name` in `names`, or 0 when absent.
std::int32_t index_of(const std::vector<std::string>& names, const std::string& name) {
  const auto it = std::find(names.begin(), names.end(), name);
  return it == names.end() ? 0 : static_cast<std::int32_t>(std::distance(names.begin(), it));
}

} // namespace

Msv4Reader::Msv4Reader(const std::string& path) : m_set(path) {
  const std::vector<std::string>& partitions = m_set.partitions();
  if (partitions.empty()) {
    throw std::runtime_error("no MSv4 partitions found in " + path);
  }
  m_partition = partitions.front();
  m_ms.emplace(m_set.store(), m_partition);
  const MeasurementSetV4& ms = *m_ms;

  m_metadata.name = m_partition;
  m_metadata.creator = ms.attributes().value("creator", std::string(""));
  m_metadata.schema_version = ms.schema_version();

  // ---- Observation ------------------------------------------------------
  const nlohmann::json observation = ms.observation_info();
  m_metadata.observation.telescope_name = ms.telescope_name();
  if (observation.contains("observer")) {
    m_metadata.observation.observer = json_strings(observation.at("observer"));
  }
  m_metadata.observation.project = observation.value("project_UID", std::string(""));
  m_metadata.observation.schedule_type = observation.value("schedule_type", std::string(""));

  // ---- Antennas ---------------------------------------------------------
  m_metadata.antennas.name = ms.antenna_name();
  m_metadata.antennas.station = ms.station_name();
  m_metadata.antennas.mount = ms.antenna_mount();
  m_metadata.antennas.telescope_name = ms.telescope_name();
  m_metadata.antennas.position = ms.antenna_position();
  m_metadata.antennas.dish_diameter = ms.antenna_dish_diameter();

  // ---- Fields and sources ----------------------------------------------
  m_metadata.fields.name = ms.field_name();
  m_metadata.fields.phase_direction = ms.field_phase_center_direction();
  if (m_metadata.fields.phase_direction.size() == 0 && !m_metadata.fields.name.empty()) {
    m_metadata.fields.phase_direction = xt::xarray<double>::from_shape({m_metadata.fields.name.size(), 2});
  }
  m_metadata.fields.direction_frame = "J2000";
  try {
    m_metadata.fields.direction_frame = ms.metadata("field_and_source_base_xds/FIELD_PHASE_CENTER_DIRECTION")
                                            .attributes.value("frame", std::string("J2000"));
  } catch (const std::exception&) {
    // Keep the default frame when the array is absent.
  }
  m_metadata.sources.name = ms.source_name();
  // MSv4 pads absent sources with "Unknown"; do not turn that back into a
  // spurious SOURCE table row when converting to MSv2.
  const bool has_real_source = std::any_of(m_metadata.sources.name.begin(), m_metadata.sources.name.end(),
                                           [](const std::string& name) { return !name.empty() && name != "Unknown"; });
  if (!has_real_source) {
    m_metadata.sources.name.clear();
  }
  m_metadata.sources.direction = m_metadata.fields.phase_direction;
  m_metadata.sources.direction_frame = m_metadata.fields.direction_frame;
  m_metadata.fields.delay_direction = m_metadata.fields.phase_direction;
  m_metadata.fields.reference_direction = m_metadata.fields.phase_direction;

  // ---- Spectral window --------------------------------------------------
  const ZarrArrayInfo& frequency = ms.metadata("frequency");
  m_metadata.spectral_window.frequency = ms.frequency();
  m_metadata.spectral_window.reference_frequency = frequency_reference(frequency);
  const double channel_width = frequency_channel_width(frequency);
  const std::size_t channels = m_metadata.spectral_window.frequency.size();
  m_metadata.spectral_window.total_bandwidth = channel_width * static_cast<double>(channels);
  m_metadata.spectral_window.channel_width = xt::xarray<double>::from_shape({channels});
  std::fill(m_metadata.spectral_window.channel_width.begin(), m_metadata.spectral_window.channel_width.end(),
            channel_width);
  m_metadata.spectral_window.name = frequency.attributes.value("spectral_window_name", std::string(""));

  // ---- Polarisation and processor --------------------------------------
  m_metadata.polarization.correlation_type = ms.polarization();
  const nlohmann::json processor = ms.processor_info();
  m_metadata.processor.type = processor.value("type", std::string(""));
  m_metadata.processor.sub_type = processor.value("sub_type", std::string(""));

  // ---- Visibility grid --------------------------------------------------
  m_layout.time = ms.time();
  m_layout.num_times = m_layout.time.size();
  m_layout.num_channels = channels;
  m_layout.num_correlations = m_metadata.polarization.correlation_type.size();

  const std::vector<std::size_t> baseline_shape = ms.shape("baseline_id");
  m_layout.num_baselines = baseline_shape.empty() ? 0 : baseline_shape[0];

  std::vector<std::string> antenna1_names(m_layout.num_baselines);
  std::vector<std::string> antenna2_names(m_layout.num_baselines);
  if (has_child(ms, "baseline_antenna1_name")) {
    antenna1_names = zarr_read_strings(m_set.store(), ms.metadata("baseline_antenna1_name"));
  }
  if (has_child(ms, "baseline_antenna2_name")) {
    antenna2_names = zarr_read_strings(m_set.store(), ms.metadata("baseline_antenna2_name"));
  }
  m_layout.baseline_antenna1 = xt::xarray<std::int32_t>::from_shape({m_layout.num_baselines});
  m_layout.baseline_antenna2 = xt::xarray<std::int32_t>::from_shape({m_layout.num_baselines});
  for (std::size_t i = 0; i < m_layout.num_baselines; ++i) {
    m_layout.baseline_antenna1(i) = static_cast<std::int32_t>(index_of(m_metadata.antennas.name, antenna1_names[i]));
    m_layout.baseline_antenna2(i) = static_cast<std::int32_t>(index_of(m_metadata.antennas.name, antenna2_names[i]));
  }

  if (has_child(ms, "field_name")) {
    const std::vector<std::string> names = zarr_read_strings(m_set.store(), ms.metadata("field_name"));
    m_layout.field_id = xt::xarray<std::int32_t>::from_shape({names.size()});
    for (std::size_t i = 0; i < names.size(); ++i) {
      m_layout.field_id(i) = index_of(m_metadata.fields.name, names[i]);
    }
  }
  if (has_child(ms, "scan_name")) {
    const std::vector<std::string> names = zarr_read_strings(m_set.store(), ms.metadata("scan_name"));
    m_layout.scan_number = xt::xarray<std::int32_t>::from_shape({names.size()});
    for (std::size_t i = 0; i < names.size(); ++i) {
      m_layout.scan_number(i) = parse_scan(names[i]);
    }
  }

  m_has_flag = has_child(ms, "FLAG");
  m_has_weight = has_child(ms, "WEIGHT");
  m_has_time_centroid = has_child(ms, "TIME_CENTROID");
  m_has_effective_integration_time = has_child(ms, "EFFECTIVE_INTEGRATION_TIME");

  if (m_layout.time.size() > 0) {
    const double minimum = *std::min_element(m_layout.time.begin(), m_layout.time.end());
    const double maximum = *std::max_element(m_layout.time.begin(), m_layout.time.end());
    m_metadata.observation.time_range = xt::xarray<double>{minimum, maximum};
  }
}

VisibilityTile Msv4Reader::read_tile(std::size_t time_start, std::size_t baseline_start, std::size_t num_times,
                                     std::size_t num_baselines) const {
  const MeasurementSetV4& ms = *m_ms;
  VisibilityTile tile;
  tile.time_start = time_start;
  tile.baseline_start = baseline_start;

  const std::vector<ZarrSlice> cell_region = {ZarrSlice{time_start, num_times},
                                              ZarrSlice{baseline_start, num_baselines}};

  const xt::xarray<double> time = ms.read<double>("time", {ZarrSlice{time_start, num_times}});
  tile.time = xt::xarray<double>::from_shape({num_times, num_baselines});
  for (std::size_t t = 0; t < num_times; ++t) {
    for (std::size_t b = 0; b < num_baselines; ++b) {
      tile.time(t, b) = time(t);
    }
  }

  tile.data = ms.read<complex_t>("VISIBILITY", cell_region);
  if (m_has_flag) {
    tile.flag = ms.read<bool>("FLAG", cell_region);
  } else {
    tile.flag = xt::xarray<bool>::from_shape(tile.data.shape());
    std::fill(tile.flag.begin(), tile.flag.end(), false);
  }
  if (m_has_weight) {
    tile.weight = ms.read<float>("WEIGHT", cell_region);
  } else {
    tile.weight = xt::xarray<float>::from_shape(tile.data.shape());
    std::fill(tile.weight.begin(), tile.weight.end(), 1.0F);
  }

  tile.uvw = ms.read<double>("UVW", {ZarrSlice{time_start, num_times}, ZarrSlice{baseline_start, num_baselines}});

  if (m_has_time_centroid) {
    tile.time_centroid = ms.read<double>("TIME_CENTROID", cell_region);
  } else {
    tile.time_centroid = xt::xarray<double>::from_shape({num_times, num_baselines});
    for (std::size_t t = 0; t < num_times; ++t) {
      for (std::size_t b = 0; b < num_baselines; ++b) {
        tile.time_centroid(t, b) = time(t);
      }
    }
  }
  if (m_has_effective_integration_time) {
    tile.exposure = ms.read<double>("EFFECTIVE_INTEGRATION_TIME", cell_region);
    tile.interval = tile.exposure;
  } else {
    tile.exposure = xt::xarray<double>::from_shape({num_times, num_baselines});
    tile.interval = tile.exposure;
  }
  return tile;
}

} // namespace rastro
