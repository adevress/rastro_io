#include "msv2_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <xtensor/views/xslice.hpp>
#include <xtensor/views/xview.hpp>

#include <casacore/casa/Arrays/Array.h>
#include <casacore/casa/Arrays/IPosition.h>
#include <casacore/casa/Arrays/Matrix.h>
#include <casacore/casa/Arrays/Vector.h>
#include <casacore/casa/Quanta/Unit.h>
#include <casacore/measures/Measures/MDirection.h>
#include <casacore/measures/Measures/MFrequency.h>
#include <casacore/measures/Measures/Stokes.h>
#include <casacore/ms/MeasurementSets/MSAntenna.h>
#include <casacore/ms/MeasurementSets/MSAntennaColumns.h>
#include <casacore/ms/MeasurementSets/MSColumns.h>
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
#include <casacore/tables/Tables/ScalarColumn.h>
#include <casacore/tables/Tables/SetupNewTab.h>
#include <casacore/tables/Tables/TableDesc.h>

namespace rastro {
namespace {

std::vector<std::size_t> c_strides(const std::vector<std::size_t>& extent) {
  std::vector<std::size_t> strides(extent.size(), 1);
  for (std::size_t d = extent.size(); d-- > 1;) {
    strides[d - 1] = strides[d] * extent[d];
  }
  return strides;
}

/// Copy a row-major xtensor expression into a casacore Array (column-major
/// storage). Any xtensor expression is evaluated into an owning xarray first.
template <class E> casacore::Array<typename E::value_type> xtensor_to_casacore(const E& expression) {
  using T = typename E::value_type;
  const xt::xarray<T> data = expression;
  const std::size_t rank = data.dimension();
  casacore::IPosition shape(static_cast<casacore::uInt>(rank));
  for (std::size_t d = 0; d < rank; ++d) {
    shape[static_cast<casacore::uInt>(d)] = static_cast<casacore::Int>(data.shape()[d]);
  }
  casacore::Array<T> array(shape);
  if (data.size() == 0) {
    return array;
  }

  bool deleteIt = false;
  T* storage = array.getStorage(deleteIt);

  std::vector<std::size_t> target_strides(rank, 1);
  for (std::size_t d = 1; d < rank; ++d) {
    target_strides[d] = target_strides[d - 1] * data.shape()[d - 1];
  }
  const std::vector<std::size_t> source_strides =
      c_strides(std::vector<std::size_t>(data.shape().begin(), data.shape().end()));

  std::vector<std::size_t> index(rank, 0);
  for (std::size_t i = 0; i < data.size(); ++i) {
    std::size_t source = 0;
    std::size_t target = 0;
    for (std::size_t d = 0; d < rank; ++d) {
      source += index[d] * source_strides[d];
      target += index[d] * target_strides[d];
    }
    storage[target] = data.data()[source];
    for (std::size_t d = rank; d-- > 0;) {
      if (++index[d] < data.shape()[d]) {
        break;
      }
      index[d] = 0;
    }
  }
  array.putStorage(storage, deleteIt);
  return array;
}

/// Extract row `index` of a 2-D xtensor as a 1-D xtensor.
template <class T> xt::xarray<T> row_of(const xt::xarray<T>& matrix, std::size_t index) {
  xt::xarray<T> result = xt::xarray<T>::from_shape({matrix.shape()[1]});
  for (std::size_t j = 0; j < matrix.shape()[1]; ++j) {
    result(j) = matrix(index, j);
  }
  return result;
}

/// Extract the `index`-th `(x, y)` plane of a 3-D xtensor.
template <class T> xt::xarray<T> plane_of(const xt::xarray<T>& tensor, std::size_t index) {
  xt::xarray<T> result = xt::xarray<T>::from_shape({tensor.shape()[1], tensor.shape()[2]});
  for (std::size_t i = 0; i < tensor.shape()[1]; ++i) {
    for (std::size_t j = 0; j < tensor.shape()[2]; ++j) {
      result(i, j) = tensor(index, i, j);
    }
  }
  return result;
}

/// MS field/source direction columns are 2-D `(2, 1)` arrays holding `(ra,
/// dec)` in radians.
xt::xarray<double> direction_matrix(const xt::xarray<double>& direction) {
  xt::xarray<double> result = xt::xarray<double>::from_shape({2, 1});
  result(0, 0) = direction(0);
  result(1, 0) = direction(1);
  return result;
}

/// Build a casacore 2-D visibility cell from one tile element.
///
/// The cell is stored as `(ncorr, nchan)` to match the layout used by the
/// reference SKA/MWA MeasurementSets (casacore's `get` then returns a
/// `(ncorr, nchan)` cell whose element `(p, c)` is the visibility of channel
/// `c` and correlation `p`).
template <class T>
casacore::Array<T> cell_2d(const xt::xarray<T>& tile, std::size_t t, std::size_t b, std::size_t nchan,
                           std::size_t ncorr) {
  casacore::Matrix<T> cell(static_cast<casacore::uInt>(ncorr), static_cast<casacore::uInt>(nchan));
  for (std::size_t c = 0; c < nchan; ++c) {
    for (std::size_t p = 0; p < ncorr; ++p) {
      cell(static_cast<casacore::uInt>(p), static_cast<casacore::uInt>(c)) = tile(t, b, c, p);
    }
  }
  return cell;
}

casacore::Slicer row_slicer(std::size_t start, std::size_t count) {
  return casacore::Slicer(casacore::IPosition(1, static_cast<casacore::Int>(start)),
                          casacore::IPosition(1, static_cast<casacore::Int>(count)));
}

casacore::MDirection::Types direction_type(const std::string& frame) {
  static const std::map<std::string, casacore::MDirection::Types> table = {
      {"J2000", casacore::MDirection::J2000},         {"JMEAN", casacore::MDirection::JMEAN},
      {"JTRUE", casacore::MDirection::JTRUE},         {"APP", casacore::MDirection::APP},
      {"B1950", casacore::MDirection::B1950},         {"B1950_VLA", casacore::MDirection::B1950_VLA},
      {"BMEAN", casacore::MDirection::BMEAN},         {"BTRUE", casacore::MDirection::BTRUE},
      {"GALACTIC", casacore::MDirection::GALACTIC},   {"HADEC", casacore::MDirection::HADEC},
      {"AZEL", casacore::MDirection::AZEL},           {"JNAT", casacore::MDirection::JNAT},
      {"ECLIPTIC", casacore::MDirection::ECLIPTIC},   {"MECLIPTIC", casacore::MDirection::MECLIPTIC},
      {"TECLIPTIC", casacore::MDirection::TECLIPTIC}, {"SUPERGAL", casacore::MDirection::SUPERGAL},
      {"ITRF", casacore::MDirection::ITRF},           {"TOPO", casacore::MDirection::TOPO},
      {"ICRS", casacore::MDirection::ICRS},
  };
  const auto it = table.find(frame);
  return it == table.end() ? casacore::MDirection::J2000 : it->second;
}

casacore::MFrequency::Types frequency_type(const std::string& frame) {
  static const std::map<std::string, casacore::MFrequency::Types> table = {
      {"REST", casacore::MFrequency::REST},       {"LSRK", casacore::MFrequency::LSRK},
      {"LSRD", casacore::MFrequency::LSRD},       {"BARY", casacore::MFrequency::BARY},
      {"GEO", casacore::MFrequency::GEO},         {"TOPO", casacore::MFrequency::TOPO},
      {"GALACTO", casacore::MFrequency::GALACTO}, {"LGROUP", casacore::MFrequency::LGROUP},
      {"CMB", casacore::MFrequency::CMB},
  };
  const auto it = table.find(frame);
  return it == table.end() ? casacore::MFrequency::TOPO : it->second;
}

void fill_observation(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  casacore::MSObservation& table = ms.observation();
  table.addRow();
  casacore::MSObservationColumns columns(table);
  columns.telescopeName().put(0, metadata.observation.telescope_name);
  columns.observer().put(0, metadata.observation.observer.empty()
                                ? casacore::String{}
                                : casacore::String(metadata.observation.observer.front()));
  columns.project().put(0, metadata.observation.project);
  columns.scheduleType().put(0, metadata.observation.schedule_type);

  casacore::Vector<casacore::Double> range(2);
  if (metadata.observation.time_range.size() >= 2) {
    range(0) = unix_seconds_to_mjd_seconds(metadata.observation.time_range(0));
    range(1) = unix_seconds_to_mjd_seconds(metadata.observation.time_range(1));
  } else {
    range = 0.0;
  }
  columns.timeRange().put(0, range);
}

void fill_antennas(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  casacore::MSAntenna& table = ms.antenna();
  const std::size_t count = metadata.antennas.name.size();
  if (count == 0) {
    return;
  }
  table.addRow(static_cast<casacore::rownr_t>(count));
  casacore::MSAntennaColumns columns(table);
  for (std::size_t i = 0; i < count; ++i) {
    const casacore::rownr_t row = static_cast<casacore::rownr_t>(i);
    columns.name().put(row, metadata.antennas.name[i]);
    columns.station().put(row, i < metadata.antennas.station.size() ? metadata.antennas.station[i] : std::string{});
    columns.type().put(row, i < metadata.antennas.type.size() ? metadata.antennas.type[i] : std::string{});
    columns.mount().put(row, i < metadata.antennas.mount.size() ? metadata.antennas.mount[i] : std::string{});
    columns.dishDiameter().put(row,
                               metadata.antennas.dish_diameter.size() > i ? metadata.antennas.dish_diameter(i) : 0.0);
    if (has_values(metadata.antennas.position)) {
      columns.position().put(row, xtensor_to_casacore(row_of(metadata.antennas.position, i)));
    }
    if (has_values(metadata.antennas.offset)) {
      columns.offset().put(row, xtensor_to_casacore(row_of(metadata.antennas.offset, i)));
    }
  }
}

void fill_feeds(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  const auto& feeds = metadata.feeds;
  if (!has_values(feeds.antenna_id)) {
    return;
  }
  const std::size_t count = feeds.antenna_id.size();
  casacore::MSFeed& table = ms.feed();
  table.addRow(static_cast<casacore::rownr_t>(count));
  casacore::MSFeedColumns columns(table);
  for (std::size_t i = 0; i < count; ++i) {
    const casacore::rownr_t row = static_cast<casacore::rownr_t>(i);
    columns.antennaId().put(row, feeds.antenna_id(i));
    columns.feedId().put(row, feeds.feed_id(i));
    columns.spectralWindowId().put(row, feeds.spectral_window_id(i));
    columns.numReceptors().put(row, feeds.num_receptors(i));
    columns.beamId().put(row, feeds.beam_id(i));
    if (has_values(feeds.position)) {
      columns.position().put(row, xtensor_to_casacore(plane_of(feeds.position, i)));
    }
    if (has_values(feeds.beam_offset)) {
      columns.beamOffset().put(row, xtensor_to_casacore(plane_of(feeds.beam_offset, i)));
    }
    if (has_values(feeds.polarization_angle)) {
      columns.receptorAngle().put(row, xtensor_to_casacore(row_of(feeds.polarization_angle, i)));
    }
    if (!feeds.polarization_type.empty()) {
      const std::size_t receptors = static_cast<std::size_t>(feeds.num_receptors(i));
      casacore::Vector<casacore::String> types(static_cast<casacore::uInt>(receptors));
      for (std::size_t r = 0; r < receptors; ++r) {
        const std::size_t index = i * receptors + r;
        types(static_cast<casacore::uInt>(r)) =
            index < feeds.polarization_type.size() ? feeds.polarization_type[index] : std::string{};
      }
      columns.polarizationType().put(row, types);
    }
  }
}

void fill_fields(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  const std::size_t count = metadata.fields.name.size();
  if (count == 0) {
    return;
  }
  casacore::MSField& table = ms.field();
  casacore::MSFieldColumns columns(table);
  columns.setDirectionRef(direction_type(metadata.fields.direction_frame));
  table.addRow(static_cast<casacore::rownr_t>(count));
  for (std::size_t i = 0; i < count; ++i) {
    const casacore::rownr_t row = static_cast<casacore::rownr_t>(i);
    columns.name().put(row, metadata.fields.name[i]);
    columns.code().put(row, i < metadata.fields.code.size() ? metadata.fields.code[i] : std::string{});
    columns.sourceId().put(row, metadata.fields.source_id.size() > i ? metadata.fields.source_id(i) : 0);
    if (has_values(metadata.fields.phase_direction)) {
      const xt::xarray<double> direction = row_of(metadata.fields.phase_direction, i);
      columns.phaseDir().put(row, xtensor_to_casacore(direction_matrix(direction)));
    }
    if (has_values(metadata.fields.delay_direction)) {
      const xt::xarray<double> direction = row_of(metadata.fields.delay_direction, i);
      columns.delayDir().put(row, xtensor_to_casacore(direction_matrix(direction)));
    }
    if (has_values(metadata.fields.reference_direction)) {
      const xt::xarray<double> direction = row_of(metadata.fields.reference_direction, i);
      columns.referenceDir().put(row, xtensor_to_casacore(direction_matrix(direction)));
    }
  }
}

void fill_spectral_window(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  if (!has_values(metadata.spectral_window.frequency)) {
    return;
  }
  const std::size_t channels = metadata.spectral_window.frequency.size();
  if (channels == 0) {
    return;
  }
  casacore::MSSpectralWindow& table = ms.spectralWindow();
  table.addRow();
  casacore::MSSpWindowColumns columns(table);
  columns.name().put(0, metadata.spectral_window.name);
  columns.numChan().put(0, static_cast<casacore::Int>(channels));
  columns.refFrequency().put(0, metadata.spectral_window.reference_frequency);
  columns.totalBandwidth().put(0, metadata.spectral_window.total_bandwidth);
  columns.measFreqRef().put(0, static_cast<casacore::Int>(frequency_type(metadata.spectral_window.frame)));

  casacore::Vector<casacore::Double> frequency(static_cast<casacore::uInt>(channels));
  casacore::Vector<casacore::Double> width(static_cast<casacore::uInt>(channels));
  for (std::size_t c = 0; c < channels; ++c) {
    frequency(static_cast<casacore::uInt>(c)) = metadata.spectral_window.frequency(c);
    width(static_cast<casacore::uInt>(c)) =
        metadata.spectral_window.channel_width.size() > c ? metadata.spectral_window.channel_width(c) : 0.0;
  }
  columns.chanFreq().put(0, frequency);
  columns.chanWidth().put(0, width);
  columns.resolution().put(0, width);
}

void fill_polarization(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  const std::size_t correlations = metadata.polarization.correlation_type.size();
  if (correlations == 0) {
    return;
  }
  casacore::MSPolarization& table = ms.polarization();
  table.addRow();
  casacore::MSPolarizationColumns columns(table);
  columns.numCorr().put(0, static_cast<casacore::Int>(correlations));

  casacore::Vector<casacore::Int> types(static_cast<casacore::uInt>(correlations));
  for (std::size_t c = 0; c < correlations; ++c) {
    types(static_cast<casacore::uInt>(c)) =
        static_cast<casacore::Int>(casacore::Stokes::type(metadata.polarization.correlation_type[c]));
  }
  columns.corrType().put(0, types);

  casacore::Matrix<casacore::Int> product(2, static_cast<casacore::uInt>(correlations), 0);
  columns.corrProduct().put(0, product);
}

void fill_data_description(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  casacore::MSDataDescription& table = ms.dataDescription();
  table.addRow();
  casacore::MSDataDescColumns columns(table);
  columns.spectralWindowId().put(0, metadata.data_description.spectral_window_id);
  columns.polarizationId().put(0, metadata.data_description.polarization_id);
}

void fill_sources(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  const std::size_t count = metadata.sources.name.size();
  if (count == 0) {
    return;
  }
  casacore::MSSource& table = ms.source();
  if (table.isNull()) {
    return;
  }
  table.addRow(static_cast<casacore::rownr_t>(count));
  casacore::MSSourceColumns columns(table);
  for (std::size_t i = 0; i < count; ++i) {
    const casacore::rownr_t row = static_cast<casacore::rownr_t>(i);
    columns.name().put(row, metadata.sources.name[i]);
    columns.code().put(row, i < metadata.sources.code.size() ? metadata.sources.code[i] : std::string{});
    columns.sourceId().put(row, static_cast<casacore::Int>(i));
    if (has_values(metadata.sources.direction)) {
      const xt::xarray<double> direction = row_of(metadata.sources.direction, i);
      columns.direction().put(row, xtensor_to_casacore(direction_matrix(direction)));
    }
  }
}

void fill_processor(casacore::MeasurementSet& ms, const MeasurementSetMetadata& metadata) {
  casacore::MSProcessor& table = ms.processor();
  table.addRow();
  casacore::MSProcessorColumns columns(table);
  columns.type().put(0, metadata.processor.type);
  columns.subType().put(0, metadata.processor.sub_type);
}

} // namespace

struct Msv2Writer::Impl {
  std::unique_ptr<casacore::MeasurementSet> ms;
};

Msv2Writer::Msv2Writer(std::string path) : m_path(std::move(path)), m_impl(new Impl()) {}

Msv2Writer::~Msv2Writer() = default;

void Msv2Writer::create(const MeasurementSetMetadata& metadata, const VisibilityLayout& layout) {
  if (std::filesystem::exists(m_path)) {
    throw std::runtime_error("destination already exists: " + m_path);
  }
  if (layout.num_channels == 0 || layout.num_correlations == 0) {
    throw std::invalid_argument("Msv2Writer: spectral window or polarisation is empty");
  }
  m_layout = layout;
  m_num_channels = layout.num_channels;
  m_num_correlations = layout.num_correlations;

  casacore::TableDesc desc = casacore::MS::requiredTableDesc();
  const auto ensure = [&](casacore::MS::PredefinedColumns column) {
    const casacore::String name = casacore::MS::columnName(column);
    if (!desc.isColumn(name)) {
      casacore::MS::addColumnToDesc(desc, column);
    }
  };
  const auto ensure_ndim = [&](casacore::MS::PredefinedColumns column, casacore::Int ndim) {
    const casacore::String name = casacore::MS::columnName(column);
    if (!desc.isColumn(name)) {
      casacore::MS::addColumnToDesc(desc, column, ndim);
    }
  };
  // The visibility columns are declared with a fixed rank; the concrete cell
  // shape is set when the first cell is written. This mirrors the
  // (ncorr, nchan) cell layout of the reference MeasurementSets.
  ensure_ndim(casacore::MS::DATA, 2);
  ensure_ndim(casacore::MS::FLAG, 2);
  ensure_ndim(casacore::MS::WEIGHT_SPECTRUM, 2);
  ensure(casacore::MS::WEIGHT);
  ensure(casacore::MS::SIGMA);
  ensure(casacore::MS::UVW);
  ensure(casacore::MS::TIME_CENTROID);

  casacore::SetupNewTable setup(m_path, desc, casacore::Table::New);
  m_impl->ms = std::make_unique<casacore::MeasurementSet>(setup, 0);
  casacore::MeasurementSet& ms = *m_impl->ms;
  ms.createDefaultSubtables(casacore::Table::New);
  if (!ms.keywordSet().isDefined("MS_VERSION")) {
    ms.rwKeywordSet().define("MS_VERSION", casacore::Double(2.0));
  }

  fill_observation(ms, metadata);
  fill_antennas(ms, metadata);
  fill_feeds(ms, metadata);
  fill_fields(ms, metadata);
  fill_spectral_window(ms, metadata);
  fill_polarization(ms, metadata);
  fill_data_description(ms, metadata);
  fill_sources(ms, metadata);
  fill_processor(ms, metadata);

  m_created = true;
}

void Msv2Writer::write_tile(const VisibilityTile& tile) {
  if (!m_created) {
    throw std::logic_error("Msv2Writer: write_tile called before create");
  }
  const std::size_t num_times = tile.data.shape()[0];
  const std::size_t num_baselines = tile.data.shape()[1];
  const std::size_t rows = num_times * num_baselines;
  if (rows == 0) {
    return;
  }
  casacore::MeasurementSet& ms = *m_impl->ms;
  const std::size_t row_start = m_rows_written;
  ms.addRow(static_cast<casacore::rownr_t>(rows));

  casacore::MSColumns columns(ms);
  const casacore::Slicer slicer = row_slicer(row_start, rows);

  // ---- Per-row scalars --------------------------------------------------
  casacore::Vector<casacore::Double> time(static_cast<casacore::uInt>(rows));
  casacore::Vector<casacore::Double> exposure(static_cast<casacore::uInt>(rows));
  casacore::Vector<casacore::Double> interval(static_cast<casacore::uInt>(rows));
  casacore::Vector<casacore::Double> centroid(static_cast<casacore::uInt>(rows));
  casacore::Vector<casacore::Int> antenna1(static_cast<casacore::uInt>(rows));
  casacore::Vector<casacore::Int> antenna2(static_cast<casacore::uInt>(rows));
  casacore::Vector<casacore::Int> field_id(static_cast<casacore::uInt>(rows));
  casacore::Vector<casacore::Int> scan_number(static_cast<casacore::uInt>(rows));
  casacore::Vector<casacore::Bool> flag_row(static_cast<casacore::uInt>(rows));

  for (std::size_t t = 0; t < num_times; ++t) {
    const std::size_t time_index = tile.time_start + t;
    const casacore::Int field = m_layout.field_id.size() > time_index
                                    ? m_layout.field_id(time_index)
                                    : (has_values(tile.field_id) ? tile.field_id(t, 0) : 0);
    const casacore::Int scan = m_layout.scan_number.size() > time_index
                                   ? m_layout.scan_number(time_index)
                                   : (has_values(tile.scan_number) ? tile.scan_number(t, 0) : 0);
    for (std::size_t b = 0; b < num_baselines; ++b) {
      const std::size_t index = t * num_baselines + b;
      const std::size_t baseline = tile.baseline_start + b;
      time(static_cast<casacore::uInt>(index)) = unix_seconds_to_mjd_seconds(tile.time(t, b));
      centroid(static_cast<casacore::uInt>(index)) = has_values(tile.time_centroid)
                                                         ? unix_seconds_to_mjd_seconds(tile.time_centroid(t, b))
                                                         : time(static_cast<casacore::uInt>(index));
      exposure(static_cast<casacore::uInt>(index)) = has_values(tile.exposure) ? tile.exposure(t, b) : 0.0;
      interval(static_cast<casacore::uInt>(index)) =
          has_values(tile.interval) ? tile.interval(t, b) : exposure(static_cast<casacore::uInt>(index));
      antenna1(static_cast<casacore::uInt>(index)) =
          baseline < m_layout.baseline_antenna1.size() ? m_layout.baseline_antenna1(baseline) : 0;
      antenna2(static_cast<casacore::uInt>(index)) =
          baseline < m_layout.baseline_antenna2.size() ? m_layout.baseline_antenna2(baseline) : 0;
      field_id(static_cast<casacore::uInt>(index)) = field;
      scan_number(static_cast<casacore::uInt>(index)) = scan;
      flag_row(static_cast<casacore::uInt>(index)) = false;
    }
  }

  casacore::Vector<casacore::Int> zero_int(static_cast<casacore::uInt>(rows), 0);
  columns.time().putColumnRange(slicer, time);
  columns.timeCentroid().putColumnRange(slicer, centroid);
  columns.exposure().putColumnRange(slicer, exposure);
  columns.interval().putColumnRange(slicer, interval);
  columns.antenna1().putColumnRange(slicer, antenna1);
  columns.antenna2().putColumnRange(slicer, antenna2);
  columns.fieldId().putColumnRange(slicer, field_id);
  columns.scanNumber().putColumnRange(slicer, scan_number);
  columns.flagRow().putColumnRange(slicer, flag_row);
  columns.dataDescId().putColumnRange(slicer, zero_int);
  columns.feed1().putColumnRange(slicer, zero_int);
  columns.feed2().putColumnRange(slicer, zero_int);
  columns.arrayId().putColumnRange(slicer, zero_int);
  columns.observationId().putColumnRange(slicer, zero_int);
  columns.processorId().putColumnRange(slicer, zero_int);
  columns.stateId().putColumnRange(slicer, zero_int);

  // ---- Array columns ----------------------------------------------------
  for (std::size_t t = 0; t < num_times; ++t) {
    for (std::size_t b = 0; b < num_baselines; ++b) {
      const casacore::rownr_t row = static_cast<casacore::rownr_t>(row_start + t * num_baselines + b);
      columns.data().put(row, cell_2d(tile.data, t, b, m_num_channels, m_num_correlations));
      if (tile.flag.size() == tile.data.size()) {
        columns.flag().put(row, cell_2d(tile.flag, t, b, m_num_channels, m_num_correlations));
      }
      if (tile.weight.size() == tile.data.size()) {
        columns.weightSpectrum().put(row, cell_2d(tile.weight, t, b, m_num_channels, m_num_correlations));
      }
      if (has_values(tile.uvw)) {
        casacore::Array<casacore::Double> uvw(casacore::IPosition(1, 3));
        bool deleteIt = false;
        casacore::Double* storage = uvw.getStorage(deleteIt);
        storage[0] = tile.uvw(t, b, 0);
        storage[1] = tile.uvw(t, b, 1);
        storage[2] = tile.uvw(t, b, 2);
        uvw.putStorage(storage, deleteIt);
        columns.uvw().put(row, uvw);
      }
    }
  }

  // WEIGHT is written both spectrally and as a per-correlation average so
  // that every consumer finds the information where it expects it.
  if (tile.weight.size() == tile.data.size()) {
    xt::xarray<casacore::Float> average = xt::xarray<casacore::Float>::from_shape({m_num_correlations, rows});
    xt::xarray<casacore::Float> sigma = xt::xarray<casacore::Float>::from_shape({m_num_correlations, rows});
    for (std::size_t t = 0; t < num_times; ++t) {
      for (std::size_t b = 0; b < num_baselines; ++b) {
        const std::size_t row = t * num_baselines + b;
        for (std::size_t p = 0; p < m_num_correlations; ++p) {
          double sum = 0.0;
          for (std::size_t c = 0; c < m_num_channels; ++c) {
            sum += tile.weight(t, b, c, p);
          }
          const double mean = sum / static_cast<double>(m_num_channels);
          average(p, row) = static_cast<casacore::Float>(mean);
          sigma(p, row) = mean > 0.0 ? static_cast<casacore::Float>(1.0 / std::sqrt(mean)) : 0.0F;
        }
      }
    }
    for (std::size_t t = 0; t < num_times; ++t) {
      for (std::size_t b = 0; b < num_baselines; ++b) {
        const std::size_t index = t * num_baselines + b;
        const casacore::rownr_t row = static_cast<casacore::rownr_t>(row_start + index);
        casacore::Array<casacore::Float> weight_cell(
            casacore::IPosition(1, static_cast<casacore::Int>(m_num_correlations)));
        casacore::Array<casacore::Float> sigma_cell(
            casacore::IPosition(1, static_cast<casacore::Int>(m_num_correlations)));
        bool deleteWeight = false;
        bool deleteSigma = false;
        casacore::Float* w = weight_cell.getStorage(deleteWeight);
        casacore::Float* s = sigma_cell.getStorage(deleteSigma);
        for (std::size_t p = 0; p < m_num_correlations; ++p) {
          w[p] = average(p, index);
          s[p] = sigma(p, index);
        }
        weight_cell.putStorage(w, deleteWeight);
        sigma_cell.putStorage(s, deleteSigma);
        columns.weight().put(row, weight_cell);
        columns.sigma().put(row, sigma_cell);
      }
    }
  }

  m_rows_written += rows;
}

void Msv2Writer::finalize() {
  if (m_impl->ms) {
    m_impl->ms->flush();
  }
}

} // namespace rastro
