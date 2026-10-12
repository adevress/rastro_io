#pragma once

// Typed, xtensor-backed representation of the properties of a measurement set.
//
// This header is the internal API shared by the MSv2 and MSv4 formats. Every
// dataset property (observation, antennas, feeds, fields, spectral window,
// polarisation, sources, visibilities) is exposed as a plain type whose
// multi-dimensional content is an `xt::xarray`. No format-specific type
// (casacore, Zarr, ...) appears here, so the conversion engine can move data
// between formats without knowing either of them.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <xtensor/containers/xarray.hpp>

namespace rastro {

/// Visibility sample scalar type, matching the MeasurementSet DATA column.
using complex_t = std::complex<float>;

/// Seconds between the modified Julian date epoch (1858-11-17) and the Unix
/// epoch (1970-01-01), i.e. 40587 days.
inline constexpr double k_mjd_to_unix_seconds = 40587.0 * 86400.0;

/// Convert a casacore/MSv2 time (seconds since the MJD epoch) to Unix seconds.
inline double mjd_seconds_to_unix(double seconds) { return seconds - k_mjd_to_unix_seconds; }

/// Convert Unix seconds to a casacore/MSv2 time (seconds since the MJD epoch).
inline double unix_seconds_to_mjd_seconds(double seconds) { return seconds + k_mjd_to_unix_seconds; }

/// OBSERVATION table / `observation_info` dictionary.
struct ObservationProperties {
  std::string telescope_name;
  std::vector<std::string> observer;
  std::string project;
  std::string schedule_type;
  std::string release_date;
  /// Start and end time in Unix seconds, shape `(2)`.
  xt::xarray<double> time_range;
};

/// ANTENNA table / `antenna_xds`.
struct AntennaProperties {
  std::vector<std::string> name;
  std::vector<std::string> station;
  std::vector<std::string> type;
  std::vector<std::string> mount;
  /// Geocentric positions in metres, shape `(n_antenna, 3)`.
  xt::xarray<double> position;
  /// Position offsets in metres, shape `(n_antenna, 3)`.
  xt::xarray<double> offset;
  /// Dish diameters in metres, shape `(n_antenna)`.
  xt::xarray<double> dish_diameter;
  /// Per-antenna flag, shape `(n_antenna)`.
  xt::xarray<bool> flag_row;
  std::string telescope_name;
};

/// FEED table.
struct FeedProperties {
  xt::xarray<std::int32_t> antenna_id;
  xt::xarray<std::int32_t> feed_id;
  xt::xarray<std::int32_t> spectral_window_id;
  xt::xarray<std::int32_t> num_receptors;
  xt::xarray<std::int32_t> beam_id;
  /// Receptor positions in metres, shape `(n_feed, n_receptor, 3)`.
  xt::xarray<double> position;
  /// Receptor beam offsets in metres, shape `(n_feed, n_receptor, 3)`.
  xt::xarray<double> beam_offset;
  /// Receptor polarisation angles in radians, shape `(n_feed, n_receptor)`.
  xt::xarray<double> polarization_angle;
  /// Receptor polarisation types, `(n_feed, n_receptor)` flattened row-major.
  std::vector<std::string> polarization_type;
};

/// FIELD table / `field_and_source_*_xds`.
struct FieldProperties {
  std::vector<std::string> name;
  std::vector<std::string> code;
  xt::xarray<std::int32_t> source_id;
  /// Phase centre in radians, shape `(n_field, 2)` ordered (ra, dec).
  xt::xarray<double> phase_direction;
  /// Delay centre in radians, shape `(n_field, 2)`.
  xt::xarray<double> delay_direction;
  /// Reference centre in radians, shape `(n_field, 2)`.
  xt::xarray<double> reference_direction;
  /// Direction reference frame (e.g. `J2000`, `ICRS`, `FK5`).
  std::string direction_frame;
};

/// SPECTRAL_WINDOW table / `frequency` coordinate.
struct SpectralWindowProperties {
  std::string name;
  /// Channel centre frequencies in Hz, shape `(n_channel)`.
  xt::xarray<double> frequency;
  /// Channel widths in Hz, shape `(n_channel)`.
  xt::xarray<double> channel_width;
  /// Reference frequency in Hz.
  double reference_frequency = 0.0;
  /// Total bandwidth in Hz.
  double total_bandwidth = 0.0;
  std::string frame;
};

/// POLARIZATION table / `polarization` coordinate.
struct PolarizationProperties {
  /// Correlation labels (`XX`, `XY`, `YX`, `YY`, `RR`, ...).
  std::vector<std::string> correlation_type;
  /// Correlation products (`XX`, `XY`, ...); empty when unknown.
  std::vector<std::string> correlation_product;
};

/// One row of the DATA_DESCRIPTION table.
struct DataDescriptionProperties {
  std::int32_t spectral_window_id = 0;
  std::int32_t polarization_id = 0;
};

/// SOURCE table.
struct SourceProperties {
  std::vector<std::string> name;
  std::vector<std::string> code;
  /// Source direction in radians, shape `(n_source, 2)`.
  xt::xarray<double> direction;
  std::string direction_frame;
};

/// PROCESSOR table / `processor_info` dictionary.
struct ProcessorProperties {
  std::string type;
  std::string sub_type;
};

/// All the metadata of a measurement set, format independent.
struct MeasurementSetMetadata {
  std::string name;
  std::string creator;
  std::string schema_version;
  ObservationProperties observation;
  AntennaProperties antennas;
  FeedProperties feeds;
  FieldProperties fields;
  SpectralWindowProperties spectral_window;
  PolarizationProperties polarization;
  SourceProperties sources;
  ProcessorProperties processor;
  DataDescriptionProperties data_description;
  /// Additional attributes that should be preserved verbatim across formats.
  std::map<std::string, std::string> extra_attributes;
};

/// Geometry of the visibility grid: `(time, baseline, frequency, polarization)`.
struct VisibilityLayout {
  std::size_t num_times = 0;
  std::size_t num_baselines = 0;
  std::size_t num_channels = 0;
  std::size_t num_correlations = 0;
  /// Integration times in Unix seconds, shape `(num_times)`.
  xt::xarray<double> time;
  /// First antenna (0-based index into the antenna table) per baseline.
  xt::xarray<std::int32_t> baseline_antenna1;
  /// Second antenna per baseline.
  xt::xarray<std::int32_t> baseline_antenna2;
  /// Field id per time step, shape `(num_times)`; optional (may be empty).
  xt::xarray<std::int32_t> field_id;
  /// Scan number per time step, shape `(num_times)`; optional.
  xt::xarray<std::int32_t> scan_number;
};

/// Return true when the layout carries a complete pair of baseline antennas.
inline bool has_baselines(const VisibilityLayout& layout) {
  return layout.baseline_antenna1.size() == layout.num_baselines &&
         layout.baseline_antenna2.size() == layout.num_baselines;
}

/// True when an xtensor actually holds data.
///
/// A default-constructed `xt::xarray` has rank 0 and size 1, so `size() > 0`
/// must never be used to detect a populated optional array.
template <class T> inline bool has_values(const xt::xarray<T>& array) {
  return array.dimension() > 0 && array.size() > 0;
}

/// A rectangular tile of the `(time, baseline)` visibility grid.
///
/// The tile is contiguous in the grid: it covers times
/// `[time_start, time_start + num_times)` and baselines
/// `[baseline_start, baseline_start + num_baselines)`. All arrays are
/// row-major xtensor containers.
struct VisibilityTile {
  std::size_t time_start = 0;
  std::size_t baseline_start = 0;

  /// Integration times in Unix seconds, shape `(num_times, num_baselines)`.
  xt::xarray<double> time;
  /// UVW coordinates in metres, shape `(num_times, num_baselines, 3)`.
  xt::xarray<double> uvw;
  /// Exposure in seconds, shape `(num_times, num_baselines)`.
  xt::xarray<double> exposure;
  /// Integration interval in seconds, shape `(num_times, num_baselines)`.
  xt::xarray<double> interval;
  /// Time centroid in Unix seconds, shape `(num_times, num_baselines)`.
  xt::xarray<double> time_centroid;

  /// Scan number per cell, shape `(num_times, num_baselines)`; optional.
  xt::xarray<std::int32_t> scan_number;
  /// Field id per cell, shape `(num_times, num_baselines)`; optional.
  xt::xarray<std::int32_t> field_id;

  /// Complex visibilities, shape `(num_times, num_baselines, n_channel, n_corr)`.
  xt::xarray<complex_t> data;
  /// Flags, same shape as `data`.
  xt::xarray<bool> flag;
  /// Weights (`1/sigma^2`), same shape as `data`.
  xt::xarray<float> weight;
  /// Per-channel sigmas, same shape as `data`; optional.
  xt::xarray<float> sigma;
};

} // namespace rastro
