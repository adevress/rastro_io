#include "measurement_set.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace rastro {
namespace {

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

std::string first_string(const std::vector<std::string>& values) {
  return values.empty() ? std::string{} : values.front();
}

void print_observation(const MeasurementSetV4& ms, std::ostream& out) {
  const nlohmann::json info = ms.observation_info();
  const std::string observer = info.contains("observer") && info.at("observer").is_array()
                                   ? first_string(info.at("observer").get<std::vector<std::string>>())
                                   : info.value("observer", std::string(""));
  const std::string project = info.value("project_UID", std::string(""));

  const xt::xarray<double> time = ms.time();
  const double t_min = time.size() > 0 ? *std::min_element(time.begin(), time.end()) : 0.0;
  const double t_max = time.size() > 0 ? *std::max_element(time.begin(), time.end()) : 0.0;

  out << "Observations: 1\n";
  out << "  - [0]\n";
  out << "      telescope name : " << ms.telescope_name() << "\n";
  out << "      observer       : " << observer << "\n";
  out << "      project        : " << project << "\n";
  out << "      schedule type  : "
      << "\n";
  out << "      time range     : " << unix_seconds_to_mjd(t_min) << " MJD -> " << unix_seconds_to_mjd(t_max)
      << " MJD\n";
}

void print_antennas(const MeasurementSetV4& ms, bool verbose, std::ostream& out) {
  const std::vector<std::string> names = ms.antenna_name();
  out << "Antennas: " << names.size() << "\n";
  if (names.empty() || !verbose) {
    return;
  }

  const std::vector<std::string> mounts = ms.antenna_mount();
  const std::vector<std::string> stations = ms.station_name();
  const xt::xarray<double> positions = ms.antenna_position();
  const xt::xarray<double> diameters = ms.antenna_dish_diameter();

  for (std::size_t i = 0; i < names.size(); ++i) {
    out << "  - [" << i << "] " << names[i];
    if (i < stations.size() && !stations[i].empty() && names[i].find(stations[i]) == std::string::npos) {
      out << " (station " << stations[i] << ")";
    }
    out << "\n";
    out << "      mount          : " << (i < mounts.size() ? mounts[i] : "") << "\n";
    if (i < diameters.shape()[0]) {
      out << "      dish diameter  : " << diameters(i) << " m\n";
    }
    if (positions.dimension() == 2 && i < positions.shape()[0]) {
      out << "      position       : [" << positions(i, 0) << ", " << positions(i, 1) << ", " << positions(i, 2)
          << "] m\n";
    }
  }
}

void print_fields(const MeasurementSetV4& ms, bool verbose, std::ostream& out) {
  const std::vector<std::string> names = ms.field_name();
  out << "Fields: " << names.size() << "\n";
  if (names.empty() || !verbose) {
    return;
  }

  const xt::xarray<double> phase = ms.field_phase_center_direction();
  for (std::size_t i = 0; i < names.size(); ++i) {
    out << "  - [" << i << "] " << names[i] << "\n";
    if (phase.dimension() == 2 && i < phase.shape()[0]) {
      out << "      phase dir      : [" << phase(i, 0) * kRadToDeg << ", " << phase(i, 1) * kRadToDeg << "] deg\n";
    }
  }
}

void print_spectral_windows(const MeasurementSetV4& ms, std::ostream& out) {
  const ZarrArrayInfo& frequency = ms.metadata("frequency");
  const std::size_t channels = frequency.shape.empty() ? 0 : frequency.shape[0];
  const double channel_width = frequency_channel_width(frequency);
  const double reference = frequency_reference(frequency);
  const double bandwidth = static_cast<double>(channels) * channel_width;
  const std::string name = frequency.attributes.value("spectral_window_name", std::string(""));

  out << "Spectral windows: 1\n";
  out << "  - [0] " << name << "\n";
  out << "      num channels   : " << channels << "\n";
  out << "      ref frequency  : " << reference / 1.0e6 << " MHz\n";
  out << "      total bandwidth: " << bandwidth / 1.0e6 << " MHz\n";
  out << "      channel width  : " << channel_width / 1.0e3 << " kHz\n";
}

void print_polarizations(const MeasurementSetV4& ms, std::ostream& out) {
  const std::vector<std::string> types = ms.polarization();
  out << "Polarizations: 1\n";
  out << "  - [0] num correlations: " << types.size() << "\n";
  out << "      correlation types:";
  for (const std::string& type : types) {
    out << " " << type;
  }
  out << "\n";
}

void print_data_descriptions(std::ostream& out) {
  out << "Data descriptions: 1\n";
  out << "  - [0] spectral window 0, polarization 0\n";
}

void print_feeds(const MeasurementSetV4& ms, std::ostream& out) {
  out << "Feeds: " << ms.antenna_name().size() << "\n";
}

void print_sources(const MeasurementSetV4& ms, std::ostream& out) {
  const std::vector<std::string> names = ms.source_name();
  out << "Sources: " << names.size() << "\n";
  for (std::size_t i = 0; i < names.size(); ++i) {
    out << "  - [" << i << "] " << names[i] << "\n";
  }
}

void print_partition(const MeasurementSetV4& ms, bool verbose, std::ostream& out) {
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
  print_data_descriptions(out);
  out << "\n";
  print_feeds(ms, out);
  out << "\n";
  print_sources(ms, out);
}

} // namespace

bool is_msv4_processing_set(const std::string& path) {
  try {
    ProcessingSet ps(path);
    return !ps.partitions().empty();
  } catch (...) {
    return false;
  }
}

void print_msv4_summary(const std::string& path, std::ostream& out, bool verbose) {
  const ProcessingSet ps(path);
  const std::vector<std::string>& partitions = ps.partitions();
  if (partitions.empty()) {
    throw std::runtime_error("no MSv4 partitions found in " + path);
  }

  std::size_t total_rows = 0;
  std::size_t columns = 0;
  std::string schema_version;
  for (const std::string& name : partitions) {
    const MeasurementSetV4 ms(ps.store(), name);
    const std::vector<std::size_t> time = ms.shape("time");
    const std::vector<std::size_t> baseline = ms.shape("baseline_id");
    total_rows += time[0] * baseline[0];
    columns = std::max(columns, ms.data_variables().size());
    if (schema_version.empty()) {
      schema_version = ms.schema_version();
    }
  }

  out << std::setprecision(10);
  out << "Summary:\n";
  out << "  filename          : " << path << "\n";
  out << "  number of rows    : " << total_rows << "\n";
  out << "  number of columns : " << columns << "\n";
  out << "  MS version        : " << schema_version << "\n";
  out << "\n";

  for (const std::string& name : partitions) {
    if (partitions.size() > 1) {
      out << "Measurement set v4: " << name << "\n";
    }
    print_partition(MeasurementSetV4(ps.store(), name), verbose, out);
    if (partitions.size() > 1) {
      out << "\n";
    }
  }
}

} // namespace rastro
