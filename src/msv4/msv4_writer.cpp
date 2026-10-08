#include "msv4_writer.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace rastro {
namespace {

/// JSON encoding of a scalar quantity: `{"data": value, "attrs": {"units": u}}`.
nlohmann::json quantity(double value, const std::string& units) {
  return nlohmann::json{{"data", value}, {"attrs", {{"units", units}}}};
}

/// Size in bytes that can hold `values` as zero-padded UTF-32.
std::size_t utf32_width(const std::vector<std::string>& values) {
  std::size_t codepoints = 1;
  for (const std::string& value : values) {
    // Count the UTF-8 code points (bytes that are not continuation bytes).
    std::size_t count = 0;
    for (const unsigned char byte : value) {
      if ((byte & 0xC0) != 0x80) {
        ++count;
      }
    }
    codepoints = std::max(codepoints, count);
  }
  return codepoints * sizeof(std::uint32_t);
}

/// Build the metadata of a numeric Zarr array.
/// Default compression codec for MSv4 arrays: Zstandard level 3.
void apply_default_codec(ZarrArrayInfo& info) {
  info.codec = ZarrCodec::Zstd;
  info.codec_configuration = {{"level", 3}};
}

/// Build the metadata of a numeric Zarr array.
ZarrArrayInfo numeric_array(std::string path, std::vector<std::size_t> shape, std::vector<std::size_t> chunks,
                            ZarrDtype dtype, std::size_t element_bytes, std::vector<std::string> dimensions,
                            nlohmann::json attributes, nlohmann::json fill_value) {
  ZarrArrayInfo info;
  info.path = std::move(path);
  info.zarr_format = 3;
  info.shape = std::move(shape);
  info.chunks = std::move(chunks);
  info.dimension_names = std::move(dimensions);
  info.dtype = dtype;
  info.element_bytes = element_bytes;
  info.endianness = std::endian::little;
  info.attributes = std::move(attributes);
  info.fill_value = std::move(fill_value);
  apply_default_codec(info);
  return info;
}

/// Build the metadata of a string Zarr array (`fixed_length_utf32`).
ZarrArrayInfo string_array(std::string path, std::size_t count, std::vector<std::string> dimensions,
                           nlohmann::json attributes, std::size_t width) {
  ZarrArrayInfo info;
  info.path = std::move(path);
  info.zarr_format = 3;
  info.shape = {count};
  info.chunks = {std::max<std::size_t>(count, 1)};
  info.dimension_names = std::move(dimensions);
  info.dtype = ZarrDtype::Utf32Fixed;
  info.element_bytes = width;
  info.attributes = std::move(attributes);
  info.fill_value = "";
  apply_default_codec(info);
  return info;
}

/// Choose a baseline chunk that keeps a visibility chunk under ~64 MiB.
std::size_t baseline_chunk(std::size_t num_baselines, std::size_t num_channels, std::size_t num_correlations,
                           std::size_t element_bytes) {
  const std::size_t per_cell = num_channels * num_correlations * element_bytes;
  constexpr std::size_t budget = 64ULL * 1024ULL * 1024ULL;
  std::size_t chunk = per_cell == 0 ? num_baselines : budget / per_cell;
  chunk = std::max<std::size_t>(chunk, 1);
  return std::min(chunk, num_baselines);
}

/// ISO-8601 UTC timestamp for the current time.
std::string creation_timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

/// Turn an arbitrary metadata name into a safe single path component.
std::string sanitize_name(const std::string& name) {
  std::string result;
  result.reserve(name.size());
  for (const char character : name) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) || character == '_' || character == '-' || character == '.') {
      result.push_back(character);
    } else {
      result.push_back('_');
    }
  }
  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  return result.empty() ? "visibility" : result;
}

/// Write a complete one-dimensional string array.
void write_strings(ZarrWriter& writer, const std::string& path, const std::vector<std::string>& values,
                   std::vector<std::string> dimensions = {}) {
  if (values.empty()) {
    return;
  }
  const ZarrArrayInfo info =
      string_array(path, values.size(), std::move(dimensions), nlohmann::json::object(), utf32_width(values));
  writer.create_array(info);
  writer.write_string_array(info, values);
}

/// Map a per-time id array to strings, falling back to an index-based name.
std::vector<std::string> id_to_name(const xt::xarray<std::int32_t>& ids, const std::vector<std::string>& names,
                                    const std::string& prefix) {
  std::vector<std::string> result;
  if (ids.size() == 0) {
    return result;
  }
  result.reserve(ids.size());
  for (const std::int32_t id : ids) {
    if (id >= 0 && static_cast<std::size_t>(id) < names.size()) {
      result.push_back(names[static_cast<std::size_t>(id)]);
    } else {
      result.push_back(prefix + std::to_string(id));
    }
  }
  return result;
}

} // namespace

Msv4Writer::Msv4Writer(std::string root) : m_writer(std::move(root)) {}

void Msv4Writer::create(const MeasurementSetMetadata& metadata, const VisibilityLayout& layout) {
  if (layout.num_times == 0 || layout.num_baselines == 0 || layout.num_channels == 0 || layout.num_correlations == 0) {
    throw std::invalid_argument("Msv4Writer: cannot create an empty visibility grid");
  }
  m_layout = layout;
  m_partition = sanitize_name(metadata.name);
  if (m_partition.rfind("visibility", 0) != 0) {
    m_partition = "visibility." + m_partition;
  }

  m_writer.create_group("", {{"type", "processing_set"}});

  const bool has_time_range = metadata.observation.time_range.size() >= 2;
  nlohmann::json observation_info;
  observation_info["observer"] = metadata.observation.observer;
  observation_info["project_UID"] = metadata.observation.project;
  observation_info["telescope_name"] = metadata.observation.telescope_name;
  observation_info["schedule_type"] = metadata.observation.schedule_type;
  if (has_time_range) {
    observation_info["time_range"] =
        nlohmann::json::array({metadata.observation.time_range(0), metadata.observation.time_range(1)});
  }
  nlohmann::json processor_info;
  processor_info["type"] = metadata.processor.type;
  processor_info["sub_type"] = metadata.processor.sub_type;

  nlohmann::json group_attributes;
  group_attributes["type"] = "visibility";
  group_attributes["schema_version"] = metadata.schema_version.empty() ? "4.0.0" : metadata.schema_version;
  group_attributes["creator"] = metadata.creator.empty() ? "rastro-io" : metadata.creator;
  group_attributes["creation_date"] = creation_timestamp();
  group_attributes["observation_info"] = observation_info;
  group_attributes["processor_info"] = processor_info;
  group_attributes["data_groups"] = {{"base",
                                      {{"correlated_data", "VISIBILITY"},
                                       {"flag", "FLAG"},
                                       {"weight", "WEIGHT"},
                                       {"uvw", "UVW"},
                                       {"field_and_source", "field_and_source_base_xds"}}}};
  m_writer.create_group(m_partition, group_attributes);

  // ---- Coordinates ------------------------------------------------------
  {
    ZarrArrayInfo time =
        numeric_array(m_partition + "/time", {layout.num_times}, {layout.num_times}, ZarrDtype::Float64, sizeof(double),
                      {"time"}, {{"units", "s"}, {"scale", "utc"}, {"format", "unix"}}, 0.0);
    m_writer.create_array(time);
    m_writer.write_array(time, layout.time);
  }
  {
    const std::size_t count = layout.num_channels;
    ZarrArrayInfo frequency = numeric_array(
        m_partition + "/frequency", {count}, {count}, ZarrDtype::Float64, sizeof(double), {"frequency"},
        {{"units", "Hz"},
         {"reference_frequency", quantity(metadata.spectral_window.reference_frequency, "Hz")},
         {"channel_width",
          quantity(has_values(metadata.spectral_window.channel_width) ? metadata.spectral_window.channel_width(0) : 0.0,
                   "Hz")},
         {"spectral_window_name", metadata.spectral_window.name}},
        0.0);
    m_writer.create_array(frequency);
    m_writer.write_array(frequency, metadata.spectral_window.frequency);
  }
  {
    xt::xarray<std::int64_t> baseline_id = xt::xarray<std::int64_t>::from_shape({layout.num_baselines});
    for (std::size_t i = 0; i < layout.num_baselines; ++i) {
      baseline_id(i) = static_cast<std::int64_t>(i);
    }
    ZarrArrayInfo info =
        numeric_array(m_partition + "/baseline_id", {layout.num_baselines}, {layout.num_baselines}, ZarrDtype::Int64,
                      sizeof(std::int64_t), {"baseline_id"}, nlohmann::json::object(), 0);
    m_writer.create_array(info);
    m_writer.write_array(info, baseline_id);
  }

  write_strings(m_writer, m_partition + "/polarization", metadata.polarization.correlation_type, {"polarization"});
  write_strings(m_writer, m_partition + "/uvw_label", {"u", "v", "w"}, {"uvw_label"});

  {
    std::vector<std::string> antenna1(layout.num_baselines);
    std::vector<std::string> antenna2(layout.num_baselines);
    for (std::size_t i = 0; i < layout.num_baselines; ++i) {
      const std::int32_t a1 = layout.baseline_antenna1.size() > i ? layout.baseline_antenna1(i) : -1;
      const std::int32_t a2 = layout.baseline_antenna2.size() > i ? layout.baseline_antenna2(i) : -1;
      antenna1[i] = a1 >= 0 && static_cast<std::size_t>(a1) < metadata.antennas.name.size()
                        ? metadata.antennas.name[static_cast<std::size_t>(a1)]
                        : "antenna_" + std::to_string(a1);
      antenna2[i] = a2 >= 0 && static_cast<std::size_t>(a2) < metadata.antennas.name.size()
                        ? metadata.antennas.name[static_cast<std::size_t>(a2)]
                        : "antenna_" + std::to_string(a2);
    }
    write_strings(m_writer, m_partition + "/baseline_antenna1_name", antenna1, {"baseline_id"});
    write_strings(m_writer, m_partition + "/baseline_antenna2_name", antenna2, {"baseline_id"});
  }

  if (layout.field_id.size() == layout.num_times) {
    write_strings(m_writer, m_partition + "/field_name", id_to_name(layout.field_id, metadata.fields.name, "field_"),
                  {"time"});
  }
  if (layout.scan_number.size() == layout.num_times) {
    std::vector<std::string> scan_names;
    scan_names.reserve(layout.scan_number.size());
    for (const std::int32_t scan : layout.scan_number) {
      scan_names.push_back("scan-" + std::to_string(scan));
    }
    write_strings(m_writer, m_partition + "/scan_name", scan_names, {"time"});
  }

  // ---- Data variables ---------------------------------------------------
  const std::size_t bchunk =
      baseline_chunk(layout.num_baselines, layout.num_channels, layout.num_correlations, sizeof(complex_t));
  const std::vector<std::size_t> data_shape = {layout.num_times, layout.num_baselines, layout.num_channels,
                                               layout.num_correlations};
  const std::vector<std::size_t> data_chunks = {1, bchunk, layout.num_channels, layout.num_correlations};
  const std::vector<std::string> data_dims = {"time", "baseline_id", "frequency", "polarization"};

  m_visibility =
      numeric_array(m_partition + "/VISIBILITY", data_shape, data_chunks, ZarrDtype::Complex64, sizeof(complex_t),
                    data_dims, {{"type", "quanta"}, {"units", "Jy"}}, nlohmann::json::array({0.0, 0.0}));
  m_writer.create_array(m_visibility);

  m_flag = numeric_array(m_partition + "/FLAG", data_shape, data_chunks, ZarrDtype::Boolean, sizeof(bool), data_dims,
                         nlohmann::json::object(), false);
  m_writer.create_array(m_flag);

  m_weight = numeric_array(m_partition + "/WEIGHT", data_shape, data_chunks, ZarrDtype::Float32, sizeof(float),
                           data_dims, nlohmann::json::object(), 0.0);
  m_writer.create_array(m_weight);

  const std::vector<std::size_t> uvw_shape = {layout.num_times, layout.num_baselines, 3};
  const std::vector<std::size_t> uvw_chunks = {1, bchunk, 3};
  m_uvw = numeric_array(m_partition + "/UVW", uvw_shape, uvw_chunks, ZarrDtype::Float64, sizeof(double),
                        {"time", "baseline_id", "uvw_label"}, {{"units", "m"}, {"frame", "fk5"}}, 0.0);
  m_writer.create_array(m_uvw);

  const std::vector<std::size_t> cell_shape = {layout.num_times, layout.num_baselines};
  const std::vector<std::size_t> cell_chunks = {1, bchunk};
  const std::vector<std::string> cell_dims = {"time", "baseline_id"};
  m_effective_integration_time = numeric_array(m_partition + "/EFFECTIVE_INTEGRATION_TIME", cell_shape, cell_chunks,
                                               ZarrDtype::Float64, sizeof(double), cell_dims, {{"units", "s"}}, 0.0);
  m_writer.create_array(m_effective_integration_time);

  m_time_centroid =
      numeric_array(m_partition + "/TIME_CENTROID", cell_shape, cell_chunks, ZarrDtype::Float64, sizeof(double),
                    cell_dims, {{"units", "s"}, {"scale", "utc"}, {"format", "unix"}}, 0.0);
  m_writer.create_array(m_time_centroid);

  // ---- antenna_xds ------------------------------------------------------
  const std::string antenna_xds = m_partition + "/antenna_xds";
  m_writer.create_group(antenna_xds,
                        {{"type", "antenna"}, {"overall_telescope_name", metadata.antennas.telescope_name}});
  write_strings(m_writer, antenna_xds + "/antenna_name", metadata.antennas.name, {"antenna_name"});
  write_strings(m_writer, antenna_xds + "/station_name", metadata.antennas.station, {"antenna_name"});
  write_strings(m_writer, antenna_xds + "/mount", metadata.antennas.mount, {"antenna_name"});
  {
    std::vector<std::string> telescopes(metadata.antennas.name.size(), metadata.antennas.telescope_name);
    write_strings(m_writer, antenna_xds + "/telescope_name", telescopes, {"antenna_name"});
  }
  if (has_values(metadata.antennas.position)) {
    const std::size_t count = metadata.antennas.position.shape()[0];
    ZarrArrayInfo position =
        numeric_array(antenna_xds + "/ANTENNA_POSITION", {count, 3}, {count, 3}, ZarrDtype::Float64, sizeof(double),
                      {"antenna_name", "cartesian_pos_label"},
                      {{"units", "m"}, {"frame", "ITRS"}, {"coordinate_system", "geocentric"}}, 0.0);
    m_writer.create_array(position);
    m_writer.write_array(position, metadata.antennas.position);
  }
  if (has_values(metadata.antennas.dish_diameter)) {
    const std::size_t count = metadata.antennas.dish_diameter.size();
    ZarrArrayInfo diameter = numeric_array(antenna_xds + "/ANTENNA_DISH_DIAMETER", {count}, {count}, ZarrDtype::Float64,
                                           sizeof(double), {"antenna_name"}, {{"units", "m"}}, 0.0);
    m_writer.create_array(diameter);
    m_writer.write_array(diameter, metadata.antennas.dish_diameter);
  }

  // ---- field_and_source_base_xds ---------------------------------------
  const std::string field_xds = m_partition + "/field_and_source_base_xds";
  m_writer.create_group(field_xds, {{"type", "field_and_source"}});
  write_strings(m_writer, field_xds + "/field_name", metadata.fields.name, {"field_name"});
  {
    std::vector<std::string> sources = metadata.sources.name;
    if (sources.size() < metadata.fields.name.size()) {
      sources.resize(metadata.fields.name.size(), "Unknown");
    }
    write_strings(m_writer, field_xds + "/source_name", sources, {"field_name"});
  }
  if (has_values(metadata.fields.phase_direction)) {
    const std::size_t count = metadata.fields.phase_direction.shape()[0];
    ZarrArrayInfo direction = numeric_array(field_xds + "/FIELD_PHASE_CENTER_DIRECTION", {count, 2}, {count, 2},
                                            ZarrDtype::Float64, sizeof(double), {"field_name", "sky_dir_label"},
                                            {{"units", "rad"}, {"frame", metadata.fields.direction_frame}}, 0.0);
    m_writer.create_array(direction);
    m_writer.write_array(direction, metadata.fields.phase_direction);
  }
  if (has_values(metadata.fields.reference_direction)) {
    const std::size_t count = metadata.fields.reference_direction.shape()[0];
    ZarrArrayInfo reference = numeric_array(field_xds + "/FIELD_REFERENCE_CENTER_DIRECTION", {count, 2}, {count, 2},
                                            ZarrDtype::Float64, sizeof(double), {"field_name", "sky_dir_label"},
                                            {{"units", "rad"}, {"frame", metadata.fields.direction_frame}}, 0.0);
    m_writer.create_array(reference);
    m_writer.write_array(reference, metadata.fields.reference_direction);
  }

  m_created = true;
}

void Msv4Writer::write_tile(const VisibilityTile& tile) {
  if (!m_created) {
    throw std::logic_error("Msv4Writer: write_tile called before create");
  }
  if (tile.data.size() == 0) {
    return;
  }
  const std::vector<std::size_t> data_start = {tile.time_start, tile.baseline_start, 0, 0};
  m_writer.write_region(m_visibility, data_start, tile.data);
  if (tile.flag.size() == tile.data.size()) {
    m_writer.write_region(m_flag, data_start, tile.flag);
  }
  if (tile.weight.size() == tile.data.size()) {
    m_writer.write_region(m_weight, data_start, tile.weight);
  }

  if (has_values(tile.uvw)) {
    const std::vector<std::size_t> uvw_start = {tile.time_start, tile.baseline_start, 0};
    m_writer.write_region(m_uvw, uvw_start, tile.uvw);
  }
  if (has_values(tile.time_centroid)) {
    const std::vector<std::size_t> cell_start = {tile.time_start, tile.baseline_start};
    m_writer.write_region(m_time_centroid, cell_start, tile.time_centroid);
  }
  if (has_values(tile.exposure)) {
    const std::vector<std::size_t> cell_start = {tile.time_start, tile.baseline_start};
    m_writer.write_region(m_effective_integration_time, cell_start, tile.exposure);
  } else if (has_values(tile.interval)) {
    const std::vector<std::size_t> cell_start = {tile.time_start, tile.baseline_start};
    m_writer.write_region(m_effective_integration_time, cell_start, tile.interval);
  }
}

void Msv4Writer::finalize() {
  // Every chunk is written atomically; nothing to flush.
}

} // namespace rastro
