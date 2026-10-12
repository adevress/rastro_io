// Self-contained MSv2 writer/reader round-trip test.
//
// Builds a small synthetic MeasurementSet v2 with the property API, reads it
// back with the MSv2 reader and checks that every value survives. This
// validates the casacore array (de)serialisation and tiling.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "meas/measurement_set.hpp"
#include "msv2/measurement_set.hpp"
#include "msv2/msv2_reader.hpp"
#include "msv2/msv2_writer.hpp"

#include <casacore/casa/Arrays/Array.h>
#include <casacore/casa/Arrays/IPosition.h>
#include <casacore/ms/MeasurementSets/MeasurementSet.h>
#include <casacore/tables/Tables/ArrayColumn.h>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

using rastro::complex_t;

rastro::MeasurementSetMetadata make_metadata() {
  rastro::MeasurementSetMetadata metadata;
  metadata.name = "synthetic";
  metadata.creator = "rastro-test";
  metadata.schema_version = "4.0.0";
  metadata.observation.telescope_name = "SyntheticTelescope";
  metadata.observation.observer = {"tester"};
  metadata.observation.time_range = xt::xarray<double>{0.0, 100.0};
  metadata.antennas.name = {"A0", "A1", "A2"};
  metadata.antennas.station = {"S0", "S1", "S2"};
  metadata.antennas.mount = {"FIXED", "FIXED", "FIXED"};
  metadata.antennas.position = xt::xarray<double>{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0}};
  metadata.antennas.dish_diameter = xt::xarray<double>{1.0, 1.0, 1.0};
  metadata.fields.name = {"field0"};
  metadata.fields.phase_direction = xt::xarray<double>{{1.0, -0.5}};
  metadata.fields.delay_direction = xt::xarray<double>{{1.0, -0.5}};
  metadata.fields.reference_direction = xt::xarray<double>{{1.0, -0.5}};
  metadata.fields.direction_frame = "J2000";
  metadata.spectral_window.name = "spw0";
  metadata.spectral_window.frequency = xt::xarray<double>{1.0e8, 1.1e8, 1.2e8};
  metadata.spectral_window.channel_width = xt::xarray<double>{1.0e6, 1.0e6, 1.0e6};
  metadata.spectral_window.reference_frequency = 1.05e8;
  metadata.spectral_window.total_bandwidth = 2.0e6;
  metadata.polarization.correlation_type = {"XX", "YY"};
  return metadata;
}

rastro::VisibilityLayout make_layout() {
  rastro::VisibilityLayout layout;
  layout.num_times = 2;
  layout.num_baselines = 3;
  layout.num_channels = 3;
  layout.num_correlations = 2;
  layout.time = xt::xarray<double>{0.0, 10.0};
  layout.baseline_antenna1 = xt::xarray<std::int32_t>{0, 0, 1};
  layout.baseline_antenna2 = xt::xarray<std::int32_t>{1, 2, 2};
  layout.field_id = xt::xarray<std::int32_t>{0, 0};
  layout.scan_number = xt::xarray<std::int32_t>{5, 5};
  return layout;
}

complex_t sample(std::size_t t, std::size_t b, std::size_t c, std::size_t p) {
  return complex_t(static_cast<float>(t * 1000 + b * 100 + c * 10 + p), static_cast<float>(t + b + c + p) * 0.5F);
}

void write_synthetic(rastro::Msv2Writer& writer, const rastro::VisibilityLayout& layout) {
  writer.create(make_metadata(), layout);
  for (std::size_t t = 0; t < layout.num_times; ++t) {
    rastro::VisibilityTile tile;
    tile.time_start = t;
    tile.baseline_start = 0;
    const std::size_t nt = 1;
    const std::size_t nb = layout.num_baselines;
    tile.time = xt::xarray<double>::from_shape({nt, nb});
    tile.uvw = xt::xarray<double>::from_shape({nt, nb, 3});
    tile.exposure = xt::xarray<double>::from_shape({nt, nb});
    tile.interval = xt::xarray<double>::from_shape({nt, nb});
    tile.time_centroid = xt::xarray<double>::from_shape({nt, nb});
    tile.data = xt::xarray<complex_t>::from_shape({nt, nb, layout.num_channels, layout.num_correlations});
    tile.flag = xt::xarray<bool>::from_shape({nt, nb, layout.num_channels, layout.num_correlations});
    tile.weight = xt::xarray<float>::from_shape({nt, nb, layout.num_channels, layout.num_correlations});
    for (std::size_t b = 0; b < nb; ++b) {
      tile.time(0, b) = layout.time(t) + 0.25;
      tile.time_centroid(0, b) = layout.time(t) + 0.5;
      tile.exposure(0, b) = 8.0;
      tile.interval(0, b) = 8.0;
      tile.uvw(0, b, 0) = static_cast<double>(t);
      tile.uvw(0, b, 1) = static_cast<double>(b);
      tile.uvw(0, b, 2) = static_cast<double>(t + b);
      for (std::size_t c = 0; c < layout.num_channels; ++c) {
        for (std::size_t p = 0; p < layout.num_correlations; ++p) {
          tile.data(0, b, c, p) = sample(t, b, c, p);
          tile.flag(0, b, c, p) = ((t + b + c + p) % 2) == 0;
          tile.weight(0, b, c, p) = static_cast<float>(t + b + c + p) + 0.75F;
        }
      }
    }
    writer.write_tile(tile);
  }
  writer.finalize();
}

} // namespace

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "rastro_msv2_roundtrip.ms";
  std::filesystem::remove_all(root);

  const rastro::VisibilityLayout layout = make_layout();

  try {
    {
      rastro::Msv2Writer writer(root.string());
      write_synthetic(writer, layout);
    }

    check(rastro::is_msv2_measurement_set(root.string()), "written file is detected as MSv2");

    {
      casacore::MeasurementSet check_ms(root.string());
      casacore::ArrayColumn<casacore::Complex> col(check_ms, "DATA");
      casacore::Array<casacore::Complex> cell;
      col.get(0, cell);
      const casacore::IPosition shape = cell.shape();
      std::cerr << "[single-row get] shape=(" << shape[0] << "," << shape[1]
                << ") (0,0)=" << cell(casacore::IPosition(2, 0, 0)) << " (0,1)=" << cell(casacore::IPosition(2, 0, 1))
                << " (1,0)=" << cell(casacore::IPosition(2, 1, 0)) << "\n";
    }

    rastro::Msv2Reader reader(root.string());
    check(reader.layout().num_times == layout.num_times, "time count");
    check(reader.layout().num_baselines == layout.num_baselines, "baseline count");
    check(reader.layout().num_channels == layout.num_channels, "channel count");
    check(reader.layout().num_correlations == layout.num_correlations, "correlation count");
    check(reader.metadata().antennas.name == make_metadata().antennas.name, "antenna names");
    check(reader.metadata().polarization.correlation_type == make_metadata().polarization.correlation_type,
          "polarisation types");

    for (std::size_t t = 0; t < layout.num_times; ++t) {
      const rastro::VisibilityTile tile = reader.read_tile(t, 0, 1, layout.num_baselines);
      for (std::size_t b = 0; b < layout.num_baselines; ++b) {
        check(std::abs(tile.time(0, b) - (layout.time(t) + 0.25)) < 1.0e-6, "time round-trip");
        check(std::abs(tile.exposure(0, b) - 8.0) < 1.0e-6, "exposure round-trip");
        for (std::size_t c = 0; c < layout.num_channels; ++c) {
          for (std::size_t p = 0; p < layout.num_correlations; ++p) {
            const complex_t expected = sample(t, b, c, p);
            check(std::abs(tile.data(0, b, c, p) - expected) < 1.0e-3F, "visibility round-trip");
            check(tile.flag(0, b, c, p) == (((t + b + c + p) % 2) == 0), "flag round-trip");
            check(std::abs(tile.weight(0, b, c, p) - (static_cast<float>(t + b + c + p) + 0.75F)) < 1.0e-3F,
                  "weight round-trip");
          }
        }
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
    ++g_failures;
  }

  std::filesystem::remove_all(root);

  if (g_failures == 0) {
    std::cout << "msv2 round-trip test passed\n";
    return 0;
  }
  std::cerr << g_failures << " checks failed\n";
  return 1;
}
