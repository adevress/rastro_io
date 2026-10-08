#pragma once

#include "measurement_set_format.hpp"

#include <cstddef>
#include <iosfwd>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "zarr_io.hpp"

namespace rastro {

/// A Zarr processing set: a root group whose children are MSv4 datasets.
class ProcessingSet {
public:
  /// Open the processing set stored at `path`.
  explicit ProcessingSet(std::string path);

  const std::string& path() const { return m_store.root(); }
  const ZarrStore& store() const { return m_store; }

  /// Names of the MSv4 partitions (children carrying a `type` attribute).
  const std::vector<std::string>& partitions() const { return m_partitions; }

private:
  ZarrStore m_store;
  std::vector<std::string> m_partitions;
};

/// A view of one MSv4 dataset (a node of the processing set).
///
/// Metadata is fetched lazily and cached; bulk visibility data is only touched
/// through the chunk-aware `read` method. The underlying `ZarrStore` is
/// immutable and the metadata cache is guarded by a mutex, so a
/// `MeasurementSetV4` may be read concurrently by several threads. The class
/// is not copyable (the cache mutex is not copyable).
class MeasurementSetV4 {
public:
  MeasurementSetV4(const ZarrStore& store, std::string node_path);

  const std::string& path() const { return m_path; }

  /// Node attributes (`schema_version`, `creator`, `observation_info`, ...).
  const nlohmann::json& attributes() const { return m_attributes; }
  std::string schema_version() const;
  nlohmann::json observation_info() const;
  nlohmann::json processor_info() const;
  nlohmann::json data_groups() const;

  /// Metadata of a data variable or coordinate, without reading any data.
  const ZarrArrayInfo& metadata(std::string_view name) const;
  /// Shape of a variable; throws when the variable is absent.
  std::vector<std::size_t> shape(std::string_view name) const;
  /// Names of the direct children (data variables and coordinates).
  std::vector<std::string> children() const;

  /// The data variables referenced by the selected data group.
  std::vector<std::string> data_variables() const;

  // ---- Chunk-aware multi-dimensional reading ----------------------------
  //
  // `region` holds at most one ZarrSlice per dimension; missing trailing
  // slices select the whole dimension. Only the chunks intersecting the region
  // are read.

  /// Read any numeric variable.
  template <class T> xt::xarray<T> read(std::string_view name, const std::vector<ZarrSlice>& region) const {
    return zarr_read<T>(m_store, metadata(name), region);
  }

  /// Read a full numeric variable.
  template <class T> xt::xarray<T> read(std::string_view name) const { return zarr_read<T>(m_store, metadata(name)); }

  // ---- Small coordinate / metadata accessors ----------------------------

  xt::xarray<double> time() const;
  xt::xarray<double> frequency() const;
  std::vector<std::string> polarization() const;
  std::vector<std::string> field_name() const;
  std::vector<std::string> source_name() const;

  std::string antenna_xds_path() const;
  std::vector<std::string> antenna_name() const;
  std::vector<std::string> antenna_mount() const;
  std::vector<std::string> station_name() const;
  std::string telescope_name() const;
  xt::xarray<double> antenna_position() const;
  xt::xarray<double> antenna_dish_diameter() const;

  std::string field_and_source_xds_path() const;
  xt::xarray<double> field_phase_center_direction() const;

private:
  const ZarrStore& m_store;
  std::string m_path;
  nlohmann::json m_attributes;
  mutable std::map<std::string, ZarrArrayInfo> m_cache;
  mutable std::mutex m_cache_mutex;
};

/// Reference frequency (Hz) recorded on the `frequency` coordinate attributes.
double frequency_reference(const ZarrArrayInfo& frequency);

/// Channel width (Hz) recorded on the `frequency` coordinate attributes.
double frequency_channel_width(const ZarrArrayInfo& frequency);

// ---- Processing-set summary entry points -------------------------------

/// Returns true when `path` is a readable MSv4 processing set.
bool is_msv4_processing_set(const std::string& path);

/// Print a metadata summary of an MSv4 processing set.
///
/// The output mirrors `print_msv2_summary`. Only array metadata and the small
/// coordinate/sub-dataset arrays are read; the bulk visibility data is never
/// touched.
void print_msv4_summary(const std::string& path, std::ostream& out, bool verbose = false);

/// MSv4 adapter for the `MeasurementSetFormat` concept. This is the only
/// surface the front-end needs; no MSv4 reader details leak into MSv2.
struct MeasurementSetV4Format {
  static bool detect(const std::string& path) { return is_msv4_processing_set(path); }
  static void summary(const std::string& path, std::ostream& out, bool verbose) {
    print_msv4_summary(path, out, verbose);
  }
};

static_assert(MeasurementSetFormat<MeasurementSetV4Format>);

} // namespace rastro
