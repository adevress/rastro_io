#include "msv2_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <xtensor/misc/xmanipulation.hpp>

#include <casacore/casa/Arrays/Array.h>
#include <casacore/casa/Arrays/IPosition.h>
#include <casacore/casa/Arrays/Vector.h>
#include <casacore/casa/Quanta/Unit.h>
#include <casacore/measures/Measures/MDirection.h>
#include <casacore/measures/Measures/MEpoch.h>
#include <casacore/measures/Measures/Stokes.h>
#include <casacore/ms/MeasurementSets/MSAntenna.h>
#include <casacore/ms/MeasurementSets/MSAntennaColumns.h>
#include <casacore/ms/MeasurementSets/MSDataDescColumns.h>
#include <casacore/ms/MeasurementSets/MSDataDescription.h>
#include <casacore/ms/MeasurementSets/MSFeed.h>
#include <casacore/ms/MeasurementSets/MSFeedColumns.h>
#include <casacore/ms/MeasurementSets/MSField.h>
#include <casacore/ms/MeasurementSets/MSFieldColumns.h>
#include <casacore/ms/MeasurementSets/MSObsColumns.h>
#include <casacore/ms/MeasurementSets/MSObservation.h>
#include <casacore/ms/MeasurementSets/MSPolColumns.h>
#include <casacore/ms/MeasurementSets/MSPolarization.h>
#include <casacore/ms/MeasurementSets/MSProcessor.h>
#include <casacore/ms/MeasurementSets/MSProcessorColumns.h>
#include <casacore/ms/MeasurementSets/MSSource.h>
#include <casacore/ms/MeasurementSets/MSSourceColumns.h>
#include <casacore/ms/MeasurementSets/MSSpWindowColumns.h>
#include <casacore/ms/MeasurementSets/MSSpectralWindow.h>
#include <casacore/ms/MeasurementSets/MeasurementSet.h>
#include <casacore/tables/Tables/ArrayColumn.h>
#include <casacore/tables/Tables/RefRows.h>
#include <casacore/tables/Tables/ScalarColumn.h>
#include <casacore/tables/Tables/Table.h>

namespace rastro {
namespace {

constexpr std::size_t missing_row = std::numeric_limits<std::size_t>::max();

/// Read a scalar column for a set of rows.
template <class T>
std::vector<T> read_scalar_cells(const casacore::MeasurementSet& ms, const std::string& column,
                                 const casacore::Vector<casacore::rownr_t>& rows) {
  casacore::ScalarColumn<T> col(ms, column);
  casacore::Vector<T> values;
  col.getColumnCells(casacore::RefRows(rows), values, true);
  return std::vector<T>(values.begin(), values.end());
}

void read_observation(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSObservation& observation = ms.observation();
  if (observation.nrow() == 0) {
    return;
  }
  casacore::MSObservationColumns columns(observation);
  metadata.observation.telescope_name = columns.telescopeName().get(0);
  metadata.observation.observer = {columns.observer().get(0)};
  metadata.observation.project = columns.project().get(0);
  metadata.observation.schedule_type = columns.scheduleType().get(0);

  casacore::Vector<casacore::Double> range;
  columns.timeRange().get(0, range);
  if (range.size() >= 2) {
    metadata.observation.time_range = xt::xarray<double>{mjd_seconds_to_unix(range(0)), mjd_seconds_to_unix(range(1))};
  }
}

void read_antennas(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSAntenna& antenna = ms.antenna();
  const std::size_t count = antenna.nrow();
  metadata.antennas.telescope_name = metadata.observation.telescope_name;
  metadata.antennas.name.resize(count);
  metadata.antennas.station.resize(count);
  metadata.antennas.type.resize(count);
  metadata.antennas.mount.resize(count);
  metadata.antennas.position = xt::xarray<double>::from_shape({count, 3});
  metadata.antennas.offset = xt::xarray<double>::from_shape({count, 3});
  metadata.antennas.dish_diameter = xt::xarray<double>::from_shape({count});
  metadata.antennas.flag_row = xt::xarray<bool>::from_shape({count});

  casacore::MSAntennaColumns columns(antenna);
  for (std::size_t i = 0; i < count; ++i) {
    const casacore::rownr_t row = static_cast<casacore::rownr_t>(i);
    metadata.antennas.name[i] = columns.name().get(row);
    metadata.antennas.station[i] = columns.station().get(row);
    metadata.antennas.type[i] = columns.type().get(row);
    metadata.antennas.mount[i] = columns.mount().get(row);
    metadata.antennas.dish_diameter(i) = columns.dishDiameter().get(row);
    metadata.antennas.flag_row(i) = columns.flagRow().get(row);
    casacore::Vector<casacore::Double> position;
    columns.position().get(row, position);
    casacore::Vector<casacore::Double> offset;
    columns.offset().get(row, offset);
    for (std::size_t d = 0; d < 3; ++d) {
      metadata.antennas.position(i, d) = d < position.size() ? position(static_cast<casacore::uInt>(d)) : 0.0;
      metadata.antennas.offset(i, d) = d < offset.size() ? offset(static_cast<casacore::uInt>(d)) : 0.0;
    }
  }
}

void read_feeds(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSFeed& feed = ms.feed();
  const std::size_t count = feed.nrow();
  if (count == 0) {
    return;
  }
  metadata.feeds.antenna_id = xt::xarray<std::int32_t>::from_shape({count});
  metadata.feeds.feed_id = xt::xarray<std::int32_t>::from_shape({count});
  metadata.feeds.spectral_window_id = xt::xarray<std::int32_t>::from_shape({count});
  metadata.feeds.num_receptors = xt::xarray<std::int32_t>::from_shape({count});
  metadata.feeds.beam_id = xt::xarray<std::int32_t>::from_shape({count});

  casacore::MSFeedColumns columns(feed);
  std::size_t max_receptors = 1;
  for (std::size_t i = 0; i < count; ++i) {
    max_receptors = std::max<std::size_t>(max_receptors, static_cast<std::size_t>(columns.numReceptors().get(i)));
  }
  metadata.feeds.position = xt::xarray<double>::from_shape({count, max_receptors, 3});
  metadata.feeds.beam_offset = xt::xarray<double>::from_shape({count, max_receptors, 3});
  metadata.feeds.polarization_angle = xt::xarray<double>::from_shape({count, max_receptors});

  for (std::size_t i = 0; i < count; ++i) {
    const casacore::rownr_t row = static_cast<casacore::rownr_t>(i);
    metadata.feeds.antenna_id(i) = columns.antennaId().get(row);
    metadata.feeds.feed_id(i) = columns.feedId().get(row);
    metadata.feeds.spectral_window_id(i) = columns.spectralWindowId().get(row);
    metadata.feeds.num_receptors(i) = columns.numReceptors().get(row);
    metadata.feeds.beam_id(i) = columns.beamId().get(row);

    casacore::Array<casacore::Double> position;
    columns.position().get(row, position);
    casacore::Array<casacore::Double> offset;
    columns.beamOffset().get(row, offset);
    casacore::Vector<casacore::Double> angle;
    columns.receptorAngle().get(row, angle);
    casacore::Array<casacore::String> types;
    columns.polarizationType().get(row, types);

    const std::vector<std::string> polarization_types(types.begin(), types.end());
    const casacore::IPosition position_shape = position.shape();
    const casacore::IPosition offset_shape = offset.shape();
    const std::size_t receptors = max_receptors;
    for (std::size_t r = 0; r < receptors; ++r) {
      for (std::size_t d = 0; d < 3; ++d) {
        const casacore::IPosition index(2, static_cast<casacore::Int>(r), static_cast<casacore::Int>(d));
        metadata.feeds.position(i, r, d) =
            position_shape.size() == 2 && static_cast<std::size_t>(position_shape[0]) > r ? position(index) : 0.0;
        metadata.feeds.beam_offset(i, r, d) =
            offset_shape.size() == 2 && static_cast<std::size_t>(offset_shape[0]) > r ? offset(index) : 0.0;
      }
      metadata.feeds.polarization_angle(i, r) =
          r < static_cast<std::size_t>(angle.size()) ? angle(static_cast<casacore::uInt>(r)) : 0.0;
      metadata.feeds.polarization_type.push_back(r < polarization_types.size() ? polarization_types[r] : std::string{});
    }
  }
}

/// Convert a casacore direction to radians and return the reference frame name.
void direction_to_radians(const casacore::MDirection& direction, xt::xarray<double>& target, std::size_t row) {
  const casacore::Vector<casacore::Double> angle = direction.getAngle(casacore::Unit("rad")).getValue();
  target(row, 0) = angle(0);
  target(row, 1) = angle(1);
}

void read_fields(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSField& field = ms.field();
  const std::size_t count = field.nrow();
  metadata.fields.name.resize(count);
  metadata.fields.code.resize(count);
  metadata.fields.source_id = xt::xarray<std::int32_t>::from_shape({count});
  metadata.fields.phase_direction = xt::xarray<double>::from_shape({count, 2});
  metadata.fields.delay_direction = xt::xarray<double>::from_shape({count, 2});
  metadata.fields.reference_direction = xt::xarray<double>::from_shape({count, 2});

  casacore::MSFieldColumns columns(field);
  for (std::size_t i = 0; i < count; ++i) {
    const casacore::rownr_t row = static_cast<casacore::rownr_t>(i);
    metadata.fields.name[i] = columns.name().get(row);
    metadata.fields.code[i] = columns.code().get(row);
    metadata.fields.source_id(i) = columns.sourceId().get(row);
    const casacore::MDirection phase = columns.phaseDirMeas(row);
    direction_to_radians(phase, metadata.fields.phase_direction, i);
    metadata.fields.direction_frame = phase.getRefString();
    direction_to_radians(columns.delayDirMeas(row), metadata.fields.delay_direction, i);
    direction_to_radians(columns.referenceDirMeas(row), metadata.fields.reference_direction, i);
  }
}

void read_spectral_window(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSSpectralWindow& spw = ms.spectralWindow();
  if (spw.nrow() == 0) {
    return;
  }
  casacore::MSSpWindowColumns columns(spw);
  metadata.spectral_window.name = columns.name().get(0);
  metadata.spectral_window.reference_frequency = columns.refFrequency().get(0);
  metadata.spectral_window.total_bandwidth = columns.totalBandwidth().get(0);
  casacore::Vector<casacore::Double> frequency;
  columns.chanFreq().get(0, frequency);
  casacore::Vector<casacore::Double> width;
  columns.chanWidth().get(0, width);
  metadata.spectral_window.frequency = xt::xarray<double>::from_shape({frequency.size()});
  metadata.spectral_window.channel_width = xt::xarray<double>::from_shape({width.size()});
  for (std::size_t i = 0; i < frequency.size(); ++i) {
    metadata.spectral_window.frequency(i) = frequency[static_cast<casacore::uInt>(i)];
  }
  for (std::size_t i = 0; i < width.size(); ++i) {
    metadata.spectral_window.channel_width(i) = width[static_cast<casacore::uInt>(i)];
  }
}

void read_polarization(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSPolarization& polarization = ms.polarization();
  if (polarization.nrow() == 0) {
    return;
  }
  casacore::MSPolarizationColumns columns(polarization);
  casacore::Vector<casacore::Int> types;
  columns.corrType().get(0, types);
  for (std::size_t i = 0; i < types.size(); ++i) {
    const auto stokes = static_cast<casacore::Stokes::StokesTypes>(types[static_cast<casacore::uInt>(i)]);
    metadata.polarization.correlation_type.push_back(casacore::Stokes::name(stokes));
  }
}

void read_sources(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSSource& source = ms.source();
  const std::size_t count = source.nrow();
  if (count == 0) {
    return;
  }
  casacore::MSSourceColumns columns(source);
  metadata.sources.name.resize(count);
  metadata.sources.code.resize(count);
  metadata.sources.direction = xt::xarray<double>::from_shape({count, 2});
  for (std::size_t i = 0; i < count; ++i) {
    const casacore::rownr_t row = static_cast<casacore::rownr_t>(i);
    metadata.sources.name[i] = columns.name().get(row);
    metadata.sources.code[i] = columns.code().get(row);
    const casacore::MDirection direction = columns.directionMeas()(row);
    direction_to_radians(direction, metadata.sources.direction, i);
    metadata.sources.direction_frame = direction.getRefString();
  }
}

void read_processor(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSProcessor& processor = ms.processor();
  if (processor.nrow() == 0) {
    return;
  }
  casacore::MSProcessorColumns columns(processor);
  metadata.processor.type = columns.type().get(0);
  metadata.processor.sub_type = columns.subType().get(0);
}

void read_data_description(const casacore::MeasurementSet& ms, MeasurementSetMetadata& metadata) {
  const casacore::MSDataDescription& description = ms.dataDescription();
  if (description.nrow() == 0) {
    return;
  }
  casacore::MSDataDescColumns columns(description);
  metadata.data_description.spectral_window_id = columns.spectralWindowId().get(0);
  metadata.data_description.polarization_id = columns.polarizationId().get(0);
}

} // namespace

struct Msv2Reader::Impl {
  casacore::MeasurementSet ms;
  std::vector<std::size_t> row_of_cell;
  std::vector<double> raw_times;
  std::size_t num_channels = 0;
  std::size_t num_correlations = 0;
  bool has_flag = false;
  bool has_weight = false;
  bool has_weight_spectrum = false;
  bool has_sigma = false;
  bool has_sigma_spectrum = false;
  bool has_uvw = false;

  explicit Impl(const std::string& path) : ms(path) {}
};

Msv2Reader::Msv2Reader(const std::string& path) : m_impl(new Impl(path)) {
  const casacore::MeasurementSet& ms = m_impl->ms;
  if (!ms.keywordSet().isDefined("MS_VERSION")) {
    throw std::runtime_error(path + " is not a MeasurementSet v2");
  }

  m_metadata.name = "visibility";
  m_metadata.creator = "rastro-io";
  m_metadata.schema_version = "4.0.0";

  read_observation(ms, m_metadata);
  read_antennas(ms, m_metadata);
  read_feeds(ms, m_metadata);
  read_fields(ms, m_metadata);
  read_spectral_window(ms, m_metadata);
  read_polarization(ms, m_metadata);
  read_sources(ms, m_metadata);
  read_processor(ms, m_metadata);
  read_data_description(ms, m_metadata);

  // ---- Visibility grid --------------------------------------------------
  casacore::ScalarColumn<casacore::Double> time_column(ms, "TIME");
  casacore::ScalarColumn<casacore::Int> antenna1_column(ms, "ANTENNA1");
  casacore::ScalarColumn<casacore::Int> antenna2_column(ms, "ANTENNA2");
  casacore::ScalarColumn<casacore::Int> field_column(ms, "FIELD_ID");
  casacore::ScalarColumn<casacore::Int> scan_column(ms, "SCAN_NUMBER");

  const casacore::rownr_t num_rows = ms.nrow();
  m_impl->raw_times.resize(num_rows);
  std::vector<casacore::Int> antenna1(num_rows);
  std::vector<casacore::Int> antenna2(num_rows);
  std::vector<casacore::Int> field_ids(num_rows);
  std::vector<casacore::Int> scan_numbers(num_rows);
  for (casacore::rownr_t row = 0; row < num_rows; ++row) {
    m_impl->raw_times[row] = time_column.get(row);
    antenna1[row] = antenna1_column.get(row);
    antenna2[row] = antenna2_column.get(row);
    field_ids[row] = field_column.get(row);
    scan_numbers[row] = scan_column.get(row);
  }

  // Unique integration times.
  std::vector<double> unique_times = m_impl->raw_times;
  std::sort(unique_times.begin(), unique_times.end());
  unique_times.erase(std::unique(unique_times.begin(), unique_times.end()), unique_times.end());
  m_layout.num_times = unique_times.size();
  m_layout.time = xt::xarray<double>::from_shape({m_layout.num_times});
  for (std::size_t i = 0; i < m_layout.num_times; ++i) {
    m_layout.time(i) = mjd_seconds_to_unix(unique_times[i]);
  }

  // Unique baselines.
  std::vector<std::pair<casacore::Int, casacore::Int>> baselines(num_rows);
  for (casacore::rownr_t row = 0; row < num_rows; ++row) {
    baselines[row] = {antenna1[row], antenna2[row]};
  }
  std::sort(baselines.begin(), baselines.end());
  baselines.erase(std::unique(baselines.begin(), baselines.end()), baselines.end());
  m_layout.num_baselines = baselines.size();
  m_layout.baseline_antenna1 = xt::xarray<std::int32_t>::from_shape({m_layout.num_baselines});
  m_layout.baseline_antenna2 = xt::xarray<std::int32_t>::from_shape({m_layout.num_baselines});
  for (std::size_t i = 0; i < m_layout.num_baselines; ++i) {
    m_layout.baseline_antenna1(i) = baselines[i].first;
    m_layout.baseline_antenna2(i) = baselines[i].second;
  }

  const std::size_t cell_count = m_layout.num_times * m_layout.num_baselines;
  if (cell_count > 200'000'000) {
    throw std::runtime_error("Msv2Reader: visibility grid is too large to index (" + std::to_string(cell_count) +
                             " cells)");
  }
  m_impl->row_of_cell.assign(cell_count, missing_row);
  m_layout.field_id = xt::xarray<std::int32_t>::from_shape({m_layout.num_times});
  m_layout.scan_number = xt::xarray<std::int32_t>::from_shape({m_layout.num_times});
  std::vector<bool> time_seen(m_layout.num_times, false);

  for (casacore::rownr_t row = 0; row < num_rows; ++row) {
    const auto time_it = std::lower_bound(unique_times.begin(), unique_times.end(), m_impl->raw_times[row]);
    const std::size_t time_index = static_cast<std::size_t>(time_it - unique_times.begin());
    const auto baseline_it =
        std::lower_bound(baselines.begin(), baselines.end(), std::make_pair(antenna1[row], antenna2[row]));
    const std::size_t baseline_index = static_cast<std::size_t>(baseline_it - baselines.begin());
    m_impl->row_of_cell[time_index * m_layout.num_baselines + baseline_index] = static_cast<std::size_t>(row);
    if (!time_seen[time_index]) {
      time_seen[time_index] = true;
      m_layout.field_id(time_index) = field_ids[row];
      m_layout.scan_number(time_index) = scan_numbers[row];
    }
  }

  const casacore::TableDesc& desc = ms.tableDesc();
  m_impl->has_flag = desc.isColumn("FLAG");
  m_impl->has_weight = desc.isColumn("WEIGHT");
  m_impl->has_weight_spectrum = desc.isColumn("WEIGHT_SPECTRUM");
  m_impl->has_sigma = desc.isColumn("SIGMA");
  m_impl->has_sigma_spectrum = desc.isColumn("SIGMA_SPECTRUM");
  m_impl->has_uvw = desc.isColumn("UVW");

  m_layout.num_channels = m_metadata.spectral_window.frequency.size();
  m_layout.num_correlations = m_metadata.polarization.correlation_type.size();
  m_impl->num_channels = m_layout.num_channels;
  m_impl->num_correlations = m_layout.num_correlations;

  if (m_layout.num_channels == 0 || m_layout.num_correlations == 0) {
    throw std::runtime_error("Msv2Reader: spectral window or polarisation is empty");
  }
  if (m_layout.field_id.size() == 0) {
    m_layout.field_id = xt::xarray<std::int32_t>::from_shape({m_layout.num_times});
  }
}

Msv2Reader::~Msv2Reader() { delete m_impl; }

/// Copy a 2-D casacore cell into a tile at `(t, b)`.
///
/// MSv2 array cells may be laid out as `(nchan, ncorr)` or as the older
/// `(ncorr, nchan)` convention; both are handled by inspecting the cell shape.
template <class T>
void copy_cell_2d(casacore::Array<T>& cell, xt::xarray<T>& target, std::size_t t, std::size_t b, std::size_t nchan,
                  std::size_t ncorr) {
  const casacore::IPosition shape = cell.shape();
  if (shape.size() != 2) {
    throw std::runtime_error("unexpected array cell rank in MSv2 reader");
  }
  bool deleteIt = false;
  const T* storage = cell.getStorage(deleteIt);
  const std::size_t first = static_cast<std::size_t>(shape[0]);
  if (first == nchan) {
    for (std::size_t c = 0; c < nchan; ++c) {
      for (std::size_t p = 0; p < ncorr; ++p) {
        target(t, b, c, p) = storage[c + nchan * p];
      }
    }
  } else if (first == ncorr) {
    for (std::size_t c = 0; c < nchan; ++c) {
      for (std::size_t p = 0; p < ncorr; ++p) {
        target(t, b, c, p) = storage[p + ncorr * c];
      }
    }
  } else {
    throw std::runtime_error("unexpected array cell shape in MSv2 reader");
  }
  cell.freeStorage(storage, deleteIt);
}

/// Copy a 1-D casacore cell into a tile at `(t, b)`.
template <class T>
void copy_cell_1d(casacore::Array<T>& cell, xt::xarray<T>& target, std::size_t t, std::size_t b, std::size_t x) {
  bool deleteIt = false;
  const T* storage = cell.getStorage(deleteIt);
  for (std::size_t i = 0; i < x; ++i) {
    target(t, b, i) = storage[i];
  }
  cell.freeStorage(storage, deleteIt);
}

VisibilityTile Msv2Reader::read_tile(std::size_t time_start, std::size_t baseline_start, std::size_t num_times,
                                     std::size_t num_baselines) const {
  const casacore::MeasurementSet& ms = m_impl->ms;
  VisibilityTile tile;
  tile.time_start = time_start;
  tile.baseline_start = baseline_start;

  tile.time = xt::xarray<double>::from_shape({num_times, num_baselines});
  tile.uvw = xt::xarray<double>::from_shape({num_times, num_baselines, 3});
  tile.exposure = xt::xarray<double>::from_shape({num_times, num_baselines});
  tile.interval = xt::xarray<double>::from_shape({num_times, num_baselines});
  tile.time_centroid = xt::xarray<double>::from_shape({num_times, num_baselines});
  tile.scan_number = xt::xarray<std::int32_t>::from_shape({num_times, num_baselines});
  tile.field_id = xt::xarray<std::int32_t>::from_shape({num_times, num_baselines});
  tile.data =
      xt::xarray<complex_t>::from_shape({num_times, num_baselines, m_impl->num_channels, m_impl->num_correlations});
  tile.flag = xt::xarray<bool>::from_shape({num_times, num_baselines, m_impl->num_channels, m_impl->num_correlations});
  tile.weight =
      xt::xarray<float>::from_shape({num_times, num_baselines, m_impl->num_channels, m_impl->num_correlations});
  std::fill(tile.exposure.begin(), tile.exposure.end(), 0.0);
  std::fill(tile.interval.begin(), tile.interval.end(), 0.0);
  std::fill(tile.scan_number.begin(), tile.scan_number.end(), -1);
  std::fill(tile.field_id.begin(), tile.field_id.end(), 0);
  std::fill(tile.data.begin(), tile.data.end(), complex_t(0.0F, 0.0F));
  std::fill(tile.flag.begin(), tile.flag.end(), true);
  std::fill(tile.weight.begin(), tile.weight.end(), 0.0F);

  // Collect the rows that actually exist for the requested tile.
  std::vector<casacore::rownr_t> row_numbers;
  std::vector<std::pair<std::size_t, std::size_t>> cells;
  row_numbers.reserve(num_times * num_baselines);
  cells.reserve(num_times * num_baselines);
  for (std::size_t t = 0; t < num_times; ++t) {
    for (std::size_t b = 0; b < num_baselines; ++b) {
      const std::size_t cell = (time_start + t) * m_layout.num_baselines + (baseline_start + b);
      if (m_impl->row_of_cell[cell] != missing_row) {
        row_numbers.push_back(static_cast<casacore::rownr_t>(m_impl->row_of_cell[cell]));
        cells.emplace_back(t, b);
      }
    }
  }

  if (!row_numbers.empty()) {
    casacore::Vector<casacore::rownr_t> rows(static_cast<casacore::uInt>(row_numbers.size()));
    for (std::size_t i = 0; i < row_numbers.size(); ++i) {
      rows[static_cast<casacore::uInt>(i)] = row_numbers[i];
    }

    // Array columns are read cell by cell: `ArrayColumn::get(row, cell)` gives
    // the cell with its declared shape, independent of any row-batching
    // convention.
    casacore::ArrayColumn<casacore::Complex> data_column(ms, "DATA");
    casacore::Array<casacore::Complex> data_cell;

    std::unique_ptr<casacore::ArrayColumn<casacore::Bool>> flag_column;
    casacore::Array<casacore::Bool> flag_cell;
    if (m_impl->has_flag) {
      flag_column = std::make_unique<casacore::ArrayColumn<casacore::Bool>>(ms, "FLAG");
    }
    std::unique_ptr<casacore::ArrayColumn<casacore::Float>> weight_spectrum_column;
    casacore::Array<casacore::Float> weight_spectrum_cell;
    if (m_impl->has_weight_spectrum) {
      weight_spectrum_column = std::make_unique<casacore::ArrayColumn<casacore::Float>>(ms, "WEIGHT_SPECTRUM");
    }
    std::unique_ptr<casacore::ArrayColumn<casacore::Float>> weight_column;
    casacore::Array<casacore::Float> weight_cell;
    if (!m_impl->has_weight_spectrum && m_impl->has_weight) {
      weight_column = std::make_unique<casacore::ArrayColumn<casacore::Float>>(ms, "WEIGHT");
    }
    std::unique_ptr<casacore::ArrayColumn<casacore::Double>> uvw_column;
    casacore::Array<casacore::Double> uvw_cell;
    if (m_impl->has_uvw) {
      uvw_column = std::make_unique<casacore::ArrayColumn<casacore::Double>>(ms, "UVW");
    }

    for (std::size_t i = 0; i < row_numbers.size(); ++i) {
      const std::size_t t = cells[i].first;
      const std::size_t b = cells[i].second;
      const casacore::rownr_t row = row_numbers[i];

      data_column.get(row, data_cell);
      copy_cell_2d(data_cell, tile.data, t, b, m_impl->num_channels, m_impl->num_correlations);
      if (flag_column) {
        flag_column->get(row, flag_cell);
        copy_cell_2d(flag_cell, tile.flag, t, b, m_impl->num_channels, m_impl->num_correlations);
      }
      if (weight_spectrum_column) {
        weight_spectrum_column->get(row, weight_spectrum_cell);
        copy_cell_2d(weight_spectrum_cell, tile.weight, t, b, m_impl->num_channels, m_impl->num_correlations);
      } else if (weight_column) {
        weight_column->get(row, weight_cell);
        bool deleteIt = false;
        const casacore::Float* storage = weight_cell.getStorage(deleteIt);
        for (std::size_t c = 0; c < m_impl->num_channels; ++c) {
          for (std::size_t p = 0; p < m_impl->num_correlations; ++p) {
            tile.weight(t, b, c, p) = storage[p];
          }
        }
        weight_cell.freeStorage(storage, deleteIt);
      }
      if (uvw_column) {
        uvw_column->get(row, uvw_cell);
        copy_cell_1d(uvw_cell, tile.uvw, t, b, 3);
      }
    }

    const std::vector<double> time = read_scalar_cells<casacore::Double>(ms, "TIME", rows);
    const std::vector<double> exposure = read_scalar_cells<casacore::Double>(ms, "EXPOSURE", rows);
    const std::vector<double> interval = read_scalar_cells<casacore::Double>(ms, "INTERVAL", rows);
    const std::vector<double> centroid = read_scalar_cells<casacore::Double>(ms, "TIME_CENTROID", rows);
    const std::vector<casacore::Int> field = read_scalar_cells<casacore::Int>(ms, "FIELD_ID", rows);
    const std::vector<casacore::Int> scan = read_scalar_cells<casacore::Int>(ms, "SCAN_NUMBER", rows);
    for (std::size_t i = 0; i < row_numbers.size(); ++i) {
      const std::size_t t = cells[i].first;
      const std::size_t b = cells[i].second;
      tile.time(t, b) = mjd_seconds_to_unix(time[i]);
      tile.time_centroid(t, b) = mjd_seconds_to_unix(centroid[i]);
      tile.exposure(t, b) = exposure[i];
      tile.interval(t, b) = interval[i];
      tile.field_id(t, b) = field[i];
      tile.scan_number(t, b) = scan[i];
    }
  } else {
    for (std::size_t t = 0; t < num_times; ++t) {
      for (std::size_t b = 0; b < num_baselines; ++b) {
        tile.time(t, b) = m_layout.time(time_start + t);
        tile.time_centroid(t, b) = tile.time(t, b);
      }
    }
  }

  return tile;
}

} // namespace rastro
