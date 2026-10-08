#include "measurement_set.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace rastro {
namespace {

namespace fs = std::filesystem;

/// Find a child of `node` whose name starts with `prefix`, or throw.
std::string find_child(const ZarrStore& store, std::string_view node, std::string_view prefix) {
  for (const std::string& child : store.list_children(node)) {
    if (child.rfind(prefix, 0) == 0) {
      return child;
    }
  }
  throw std::runtime_error("no child '" + std::string(prefix) + "*' under '" + std::string(node) + "'");
}

/// Read a scalar quantity stored either as a bare number or as
/// `{"data": <number>, "attrs": {...}}`.
double quantity_value(const nlohmann::json& json) {
  if (json.is_number()) {
    return json.get<double>();
  }
  if (json.is_object() && json.contains("data") && json.at("data").is_number()) {
    return json.at("data").get<double>();
  }
  return 0.0;
}

} // namespace

ProcessingSet::ProcessingSet(std::string path) : m_store(std::move(path)) {
  for (const std::string& child : m_store.list_children("")) {
    if (!m_store.has_node(child) || !m_store.is_group(child)) {
      continue;
    }
    const nlohmann::json attributes = m_store.read_attributes(child);
    if (attributes.contains("type")) {
      m_partitions.push_back(child);
    }
  }
  std::sort(m_partitions.begin(), m_partitions.end());
}

MeasurementSetV4::MeasurementSetV4(const ZarrStore& store, std::string node_path)
    : m_store(store), m_path(std::move(node_path)), m_attributes(store.read_attributes(m_path)) {}

std::string MeasurementSetV4::schema_version() const { return m_attributes.value("schema_version", std::string("")); }

nlohmann::json MeasurementSetV4::observation_info() const {
  return m_attributes.value("observation_info", nlohmann::json::object());
}

nlohmann::json MeasurementSetV4::processor_info() const {
  return m_attributes.value("processor_info", nlohmann::json::object());
}

nlohmann::json MeasurementSetV4::data_groups() const {
  return m_attributes.value("data_groups", nlohmann::json::object());
}

const ZarrArrayInfo& MeasurementSetV4::metadata(std::string_view name) const {
  const std::string node = (fs::path(m_path) / name).string();
  {
    const std::lock_guard<std::mutex> lock(m_cache_mutex);
    const auto it = m_cache.find(node);
    if (it != m_cache.end()) {
      return it->second;
    }
  }
  // Read outside the lock: ZarrStore is immutable and thread-safe. std::map
  // never invalidates references on insertion, so the reference returned
  // below stays valid even if another thread populates the cache meanwhile.
  ZarrArrayInfo info = m_store.read_array_info(node);
  const std::lock_guard<std::mutex> lock(m_cache_mutex);
  return m_cache.emplace(node, std::move(info)).first->second;
}

std::vector<std::size_t> MeasurementSetV4::shape(std::string_view name) const { return metadata(name).shape; }

std::vector<std::string> MeasurementSetV4::children() const { return m_store.list_children(m_path); }

std::vector<std::string> MeasurementSetV4::data_variables() const {
  const nlohmann::json groups = data_groups();
  std::vector<std::string> names;
  if (groups.is_object() && !groups.empty()) {
    const std::string group_name = groups.contains("base") ? "base" : groups.begin().key();
    const nlohmann::json& group = groups.at(group_name);
    for (const std::string_view key : {"correlated_data", "flag", "weight", "uvw"}) {
      if (group.contains(key)) {
        names.push_back(group.at(key).get<std::string>());
      }
    }
  }
  for (const std::string_view optional : {"TIME_CENTROID", "EFFECTIVE_INTEGRATION_TIME"}) {
    if (m_store.is_array((fs::path(m_path) / optional).string())) {
      names.push_back(std::string(optional));
    }
  }
  return names;
}

xt::xarray<double> MeasurementSetV4::time() const { return read<double>("time"); }

xt::xarray<double> MeasurementSetV4::frequency() const { return read<double>("frequency"); }

std::vector<std::string> MeasurementSetV4::polarization() const {
  return zarr_read_strings(m_store, metadata("polarization"));
}

std::string MeasurementSetV4::antenna_xds_path() const {
  return (fs::path(m_path) / find_child(m_store, m_path, "antenna_xds")).string();
}

std::vector<std::string> MeasurementSetV4::antenna_name() const {
  return zarr_read_strings(m_store, metadata("antenna_xds/antenna_name"));
}

std::vector<std::string> MeasurementSetV4::antenna_mount() const {
  const std::string node = (fs::path(antenna_xds_path()) / "mount").string();
  return m_store.is_array(node) ? zarr_read_strings(m_store, metadata("antenna_xds/mount"))
                                : std::vector<std::string>{};
}

std::vector<std::string> MeasurementSetV4::station_name() const {
  const std::string node = (fs::path(antenna_xds_path()) / "station_name").string();
  return m_store.is_array(node) ? zarr_read_strings(m_store, metadata("antenna_xds/station_name"))
                                : std::vector<std::string>{};
}

std::string MeasurementSetV4::telescope_name() const {
  const nlohmann::json attributes = m_store.read_attributes(antenna_xds_path());
  if (attributes.contains("overall_telescope_name")) {
    return attributes.at("overall_telescope_name").get<std::string>();
  }
  const std::string node = (fs::path(antenna_xds_path()) / "telescope_name").string();
  if (m_store.is_array(node)) {
    const std::vector<std::string> names = zarr_read_strings(m_store, metadata("antenna_xds/telescope_name"));
    if (!names.empty()) {
      return names.front();
    }
  }
  return {};
}

xt::xarray<double> MeasurementSetV4::antenna_position() const { return read<double>("antenna_xds/ANTENNA_POSITION"); }

xt::xarray<double> MeasurementSetV4::antenna_dish_diameter() const {
  return read<double>("antenna_xds/ANTENNA_DISH_DIAMETER");
}

std::string MeasurementSetV4::field_and_source_xds_path() const {
  return (fs::path(m_path) / find_child(m_store, m_path, "field_and_source")).string();
}

std::vector<std::string> MeasurementSetV4::field_name() const {
  const std::string child = find_child(m_store, m_path, "field_and_source");
  const std::string node = (fs::path(m_path) / child / "field_name").string();
  if (!m_store.is_array(node)) {
    return {};
  }
  return zarr_read_strings(m_store, metadata((fs::path(child) / "field_name").string()));
}

std::vector<std::string> MeasurementSetV4::source_name() const {
  const std::string child = find_child(m_store, m_path, "field_and_source");
  const std::string node = (fs::path(m_path) / child / "source_name").string();
  if (!m_store.is_array(node)) {
    return {};
  }
  return zarr_read_strings(m_store, metadata((fs::path(child) / "source_name").string()));
}

xt::xarray<double> MeasurementSetV4::field_phase_center_direction() const {
  const std::string child = find_child(m_store, m_path, "field_and_source");
  return read<double>(child + "/FIELD_PHASE_CENTER_DIRECTION");
}

double frequency_reference(const ZarrArrayInfo& frequency) {
  return quantity_value(frequency.attributes.value("reference_frequency", nlohmann::json(0.0)));
}

double frequency_channel_width(const ZarrArrayInfo& frequency) {
  return quantity_value(frequency.attributes.value("channel_width", nlohmann::json(0.0)));
}

} // namespace rastro
