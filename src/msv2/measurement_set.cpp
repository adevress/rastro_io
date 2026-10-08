#include "measurement_set.hpp"

#include <casacore/casa/Arrays/Array.h>
#include <casacore/casa/Arrays/IPosition.h>
#include <casacore/casa/Arrays/Vector.h>
#include <casacore/casa/Quanta/Quantum.h>
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
#include <casacore/ms/MeasurementSets/MSHistory.h>
#include <casacore/ms/MeasurementSets/MSHistoryColumns.h>
#include <casacore/ms/MeasurementSets/MSObsColumns.h>
#include <casacore/ms/MeasurementSets/MSObservation.h>
#include <casacore/ms/MeasurementSets/MSPolColumns.h>
#include <casacore/ms/MeasurementSets/MSPolarization.h>
#include <casacore/ms/MeasurementSets/MSSource.h>
#include <casacore/ms/MeasurementSets/MSSourceColumns.h>
#include <casacore/ms/MeasurementSets/MSSpWindowColumns.h>
#include <casacore/ms/MeasurementSets/MSSpectralWindow.h>
#include <casacore/ms/MeasurementSets/MeasurementSet.h>
#include <casacore/tables/Tables/Table.h>

#include <iomanip>
#include <ostream>
#include <string>

namespace rastro {
namespace {

/// Print a direction in degrees together with its reference frame.
void print_direction(const casacore::MDirection& direction, std::ostream& out) {
  const casacore::Vector<casacore::Double> angle = direction.getAngle(casacore::Unit("deg")).getValue();
  out << "[" << angle(0) << ", " << angle(1) << "] deg (" << direction.getRefString() << ")";
}

/// Print a cartesian position in meters.
void print_position(const casacore::Vector<casacore::Double>& position, std::ostream& out) {
  out << "[" << position(0) << ", " << position(1) << ", " << position(2) << "] m";
}

/// Print an epoch as modified Julian date.
void print_epoch(const casacore::MEpoch& epoch, std::ostream& out) {
  out << epoch.get(casacore::Unit("d")).getValue() << " MJD";
}

/// Returns true when the table exposes the given column.
bool has_column(const casacore::Table& table, const casacore::String& name) { return table.tableDesc().isColumn(name); }

void print_observation(const casacore::MeasurementSet& ms, std::ostream& out) {
  const casacore::MSObservation& observation = ms.observation();

  out << "Observations: " << observation.nrow() << "\n";
  if (observation.nrow() == 0) {
    return;
  }

  casacore::MSObservationColumns columns(observation);
  for (casacore::rownr_t i = 0; i < observation.nrow(); ++i) {
    out << "  - [" << i << "]\n";
    out << "      telescope name : " << columns.telescopeName().get(i) << "\n";
    out << "      observer       : " << columns.observer().get(i) << "\n";
    if (has_column(observation, "PROJECT")) {
      out << "      project        : " << columns.project().get(i) << "\n";
    }
    out << "      schedule type  : " << columns.scheduleType().get(i) << "\n";

    const casacore::Array<casacore::MEpoch> time_range = columns.timeRangeMeas()(i);
    out << "      time range     : ";
    print_epoch(time_range(casacore::IPosition(1, 0)), out);
    out << " -> ";
    print_epoch(time_range(casacore::IPosition(1, 1)), out);
    out << "\n";
  }
}

void print_antennas(const casacore::MeasurementSet& ms, bool verbose, std::ostream& out) {
  const casacore::MSAntenna& antenna = ms.antenna();

  out << "Antennas: " << antenna.nrow() << "\n";
  if (antenna.nrow() == 0 || !verbose) {
    return;
  }

  casacore::MSAntennaColumns columns(antenna);
  for (casacore::rownr_t i = 0; i < antenna.nrow(); ++i) {
    out << "  - [" << i << "] " << columns.name().get(i) << " (station " << columns.station().get(i) << ")\n";
    out << "      type           : " << columns.type().get(i) << "\n";
    out << "      mount          : " << columns.mount().get(i) << "\n";
    out << "      dish diameter  : " << columns.dishDiameter().get(i) << " m\n";
    out << "      position       : ";
    print_position(columns.position().get(i), out);
    out << "\n";
  }
}

void print_fields(const casacore::MeasurementSet& ms, bool verbose, std::ostream& out) {
  const casacore::MSField& field = ms.field();

  out << "Fields: " << field.nrow() << "\n";
  if (field.nrow() == 0 || !verbose) {
    return;
  }

  casacore::MSFieldColumns columns(field);
  for (casacore::rownr_t i = 0; i < field.nrow(); ++i) {
    out << "  - [" << i << "] " << columns.name().get(i);
    const casacore::String code = columns.code().get(i);
    if (!code.empty()) {
      out << " (code: " << code << ")";
    }
    out << "\n";
    out << "      source id      : " << columns.sourceId().get(i) << "\n";
    out << "      phase dir      : ";
    print_direction(columns.phaseDirMeas(i), out);
    out << "\n";
    out << "      delay dir      : ";
    print_direction(columns.delayDirMeas(i), out);
    out << "\n";
  }
}

void print_spectral_windows(const casacore::MeasurementSet& ms, std::ostream& out) {
  const casacore::MSSpectralWindow& spw = ms.spectralWindow();

  out << "Spectral windows: " << spw.nrow() << "\n";
  if (spw.nrow() == 0) {
    return;
  }

  casacore::MSSpWindowColumns columns(spw);
  for (casacore::rownr_t i = 0; i < spw.nrow(); ++i) {
    const casacore::Int num_channels = columns.numChan().get(i);
    const casacore::Double ref_frequency = columns.refFrequency().get(i);
    const casacore::Double bandwidth = columns.totalBandwidth().get(i);
    const casacore::Double channel_width =
        (num_channels > 0) ? bandwidth / static_cast<casacore::Double>(num_channels) : 0.0;

    out << "  - [" << i << "] " << columns.name().get(i) << "\n";
    out << "      num channels   : " << num_channels << "\n";
    out << "      ref frequency  : " << ref_frequency / 1.0e6 << " MHz\n";
    out << "      total bandwidth: " << bandwidth / 1.0e6 << " MHz\n";
    out << "      channel width  : " << channel_width / 1.0e3 << " kHz\n";
  }
}

void print_polarizations(const casacore::MeasurementSet& ms, std::ostream& out) {
  const casacore::MSPolarization& polarization = ms.polarization();

  out << "Polarizations: " << polarization.nrow() << "\n";
  if (polarization.nrow() == 0) {
    return;
  }

  casacore::MSPolarizationColumns columns(polarization);
  for (casacore::rownr_t i = 0; i < polarization.nrow(); ++i) {
    const casacore::Vector<casacore::Int> corr_type = columns.corrType().get(i);
    out << "  - [" << i << "] num correlations: " << columns.numCorr().get(i) << "\n";
    out << "      correlation types:";
    for (casacore::uInt c = 0; c < corr_type.nelements(); ++c) {
      const auto stokes = static_cast<casacore::Stokes::StokesTypes>(corr_type(c));
      out << " " << casacore::Stokes::name(stokes);
    }
    out << "\n";
  }
}

void print_data_descriptions(const casacore::MeasurementSet& ms, std::ostream& out) {
  const casacore::MSDataDescription& data_description = ms.dataDescription();

  out << "Data descriptions: " << data_description.nrow() << "\n";
  if (data_description.nrow() == 0) {
    return;
  }

  casacore::MSDataDescColumns columns(data_description);
  for (casacore::rownr_t i = 0; i < data_description.nrow(); ++i) {
    out << "  - [" << i << "] spectral window " << columns.spectralWindowId().get(i) << ", polarization "
        << columns.polarizationId().get(i) << "\n";
  }
}

void print_feeds(const casacore::MeasurementSet& ms, bool verbose, std::ostream& out) {
  const casacore::MSFeed& feed = ms.feed();

  out << "Feeds: " << feed.nrow() << "\n";
  if (feed.nrow() == 0 || !verbose) {
    return;
  }

  casacore::MSFeedColumns columns(feed);
  for (casacore::rownr_t i = 0; i < feed.nrow(); ++i) {
    out << "  - [" << i << "] feed id " << columns.feedId().get(i) << ", antenna " << columns.antennaId().get(i)
        << ", spectral window " << columns.spectralWindowId().get(i) << "\n";
    out << "      num receptors  : " << columns.numReceptors().get(i) << "\n";
    out << "      position       : ";
    print_position(columns.position().get(i), out);
    out << "\n";
  }
}

void print_sources(const casacore::MeasurementSet& ms, std::ostream& out) {
  const casacore::MSSource& source = ms.source();

  out << "Sources: " << source.nrow() << "\n";
  if (source.nrow() == 0) {
    return;
  }

  casacore::MSSourceColumns columns(source);
  for (casacore::rownr_t i = 0; i < source.nrow(); ++i) {
    out << "  - [" << i << "] " << columns.name().get(i);
    const casacore::String code = columns.code().get(i);
    if (!code.empty()) {
      out << " (code: " << code << ")";
    }
    out << "\n";
    out << "      direction      : ";
    print_direction(columns.directionMeas()(i), out);
    out << "\n";
  }
}

void print_history(const casacore::MeasurementSet& ms, std::ostream& out) {
  const casacore::MSHistory& history = ms.history();

  out << "History: " << history.nrow() << "\n";
  if (history.nrow() == 0) {
    return;
  }

  casacore::MSHistoryColumns columns(history);
  for (casacore::rownr_t i = 0; i < history.nrow(); ++i) {
    out << "  - [" << i << "] " << columns.application().get(i) << " (priority " << columns.priority().get(i) << ")\n";
    out << "      time           : ";
    print_epoch(columns.timeMeas()(i), out);
    out << "\n";
    const casacore::String message = columns.message().get(i);
    if (!message.empty()) {
      out << "      message        : " << message << "\n";
    }
  }
}

} // namespace

bool is_msv2_measurement_set(const std::string& path) {
  if (!casacore::Table::isReadable(path)) {
    return false;
  }

  try {
    const casacore::MeasurementSet ms(path);
    // The root table of a MeasurementSet carries the MS_VERSION keyword.
    return ms.keywordSet().isDefined("MS_VERSION");
  } catch (...) {
    return false;
  }
}

void print_msv2_summary(const std::string& path, std::ostream& out, bool verbose) {
  const casacore::MeasurementSet ms(path);

  out << std::setprecision(10);
  out << "Summary:\n";
  out << "  filename          : " << path << "\n";
  out << "  number of rows    : " << ms.nrow() << "\n";
  out << "  number of columns : " << ms.tableDesc().ncolumn() << "\n";
  if (ms.keywordSet().isDefined("MS_VERSION")) {
    out << "  MS version        : " << ms.keywordSet().asDouble("MS_VERSION") << "\n";
  }
  out << "\n";

  print_observation(ms, out);
  out << "\n";
  print_antennas(ms, verbose, out);
  out << "\n";
  print_fields(ms, verbose, out);
  out << "\n";
  print_spectral_windows(ms, out);
  out << "\n";
  print_polarizations(ms, out);
  out << "\n";
  print_data_descriptions(ms, out);
  out << "\n";
  print_feeds(ms, verbose, out);
  out << "\n";
  print_sources(ms, out);
  out << "\n";
  if (verbose) {
    print_history(ms, out);
  }
}

} // namespace rastro
