// Unit test for the tiled conversion engine, using in-memory reader/writer
// implementations of the property API.

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "meas/measurement_set.hpp"
#include "meas/properties.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

using rastro::complex_t;

/// Deterministic synthetic value for a sample of the reference dataset.
complex_t sample_value(std::size_t t, std::size_t b, std::size_t c, std::size_t p) {
  return complex_t(static_cast<float>(t * 1000 + b * 100 + c * 10 + p),
                   static_cast<float>(-static_cast<int>(t * 100 + b * 10 + c + p)));
}

rastro::MeasurementSetMetadata make_metadata() {
  rastro::MeasurementSetMetadata metadata;
  metadata.name = "synthetic";
  metadata.creator = "rastro-test";
  metadata.schema_version = "4.0.0";
  metadata.observation.telescope_name = "SyntheticTelescope";
  metadata.observation.observer = {"tester"};
  metadata.observation.time_range = xt::xarray<double>{0.0, 100.0};
  metadata.antennas.name = {"A0", "A1", "A2", "A3"};
  metadata.antennas.station = {"S0", "S1", "S2", "S3"};
  metadata.antennas.mount = {"FIXED", "FIXED", "FIXED", "FIXED"};
  metadata.antennas.position = xt::xarray<double>{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {3.0, 0.0, 0.0}};
  metadata.antennas.dish_diameter = xt::xarray<double>{1.0, 1.0, 1.0, 1.0};
  metadata.fields.name = {"field0"};
  metadata.fields.phase_direction = xt::xarray<double>{{1.0, -0.5}};
  metadata.fields.delay_direction = xt::xarray<double>{{1.0, -0.5}};
  metadata.fields.direction_frame = "J2000";
  metadata.spectral_window.name = "spw0";
  metadata.spectral_window.frequency = xt::xarray<double>{1.0e8, 1.1e8, 1.2e8};
  metadata.spectral_window.channel_width = xt::xarray<double>{1.0e6, 1.0e6, 1.0e6};
  metadata.spectral_window.reference_frequency = 1.1e8;
  metadata.spectral_window.total_bandwidth = 3.0e6;
  metadata.polarization.correlation_type = {"XX", "YY"};
  metadata.sources.name = {"src0"};
  metadata.sources.direction = xt::xarray<double>{{1.0, -0.5}};
  return metadata;
}

rastro::VisibilityLayout make_layout() {
  rastro::VisibilityLayout layout;
  layout.num_times = 7;
  layout.num_baselines = 5;
  layout.num_channels = 3;
  layout.num_correlations = 2;
  layout.time = xt::xarray<double>{0.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
  layout.baseline_antenna1 = xt::xarray<std::int32_t>{0, 0, 1, 1, 2};
  layout.baseline_antenna2 = xt::xarray<std::int32_t>{1, 2, 2, 3, 3};
  layout.field_id = xt::xarray<std::int32_t>{0, 0, 0, 0, 0, 0, 0};
  layout.scan_number = xt::xarray<std::int32_t>{0, 0, 0, 0, 0, 0, 0};
  return layout;
}

/// Reader regenerating the synthetic dataset on demand.
class MemoryReader : public rastro::MeasurementSetReader {
public:
  MemoryReader() : m_metadata(make_metadata()), m_layout(make_layout()) {}

  std::string format_name() const override { return "memory"; }
  const rastro::MeasurementSetMetadata& metadata() const override { return m_metadata; }
  const rastro::VisibilityLayout& layout() const override { return m_layout; }

  rastro::VisibilityTile read_tile(std::size_t time_start, std::size_t baseline_start, std::size_t num_times,
                                   std::size_t num_baselines) const override {
    rastro::VisibilityTile tile;
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
        xt::xarray<complex_t>::from_shape({num_times, num_baselines, m_layout.num_channels, m_layout.num_correlations});
    tile.flag =
        xt::xarray<bool>::from_shape({num_times, num_baselines, m_layout.num_channels, m_layout.num_correlations});
    tile.weight =
        xt::xarray<float>::from_shape({num_times, num_baselines, m_layout.num_channels, m_layout.num_correlations});
    for (std::size_t t = 0; t < num_times; ++t) {
      for (std::size_t b = 0; b < num_baselines; ++b) {
        const std::size_t gt = time_start + t;
        const std::size_t gb = baseline_start + b;
        tile.time(t, b) = m_layout.time(gt) + 0.25;
        tile.time_centroid(t, b) = m_layout.time(gt) + 0.5;
        tile.exposure(t, b) = 8.0;
        tile.interval(t, b) = 8.0;
        tile.uvw(t, b, 0) = static_cast<double>(gt);
        tile.uvw(t, b, 1) = static_cast<double>(gb);
        tile.uvw(t, b, 2) = static_cast<double>(gt + gb);
        tile.scan_number(t, b) = 3;
        tile.field_id(t, b) = 0;
        for (std::size_t c = 0; c < m_layout.num_channels; ++c) {
          for (std::size_t p = 0; p < m_layout.num_correlations; ++p) {
            tile.data(t, b, c, p) = sample_value(gt, gb, c, p);
            tile.flag(t, b, c, p) = ((gt + gb + c + p) % 3) == 0;
            tile.weight(t, b, c, p) = static_cast<float>(gt + gb + c + p) + 0.5F;
          }
        }
      }
    }
    return tile;
  }

private:
  rastro::MeasurementSetMetadata m_metadata;
  rastro::VisibilityLayout m_layout;
};

/// Writer accumulating the tiles in memory for later comparison.
class MemoryWriter : public rastro::MeasurementSetWriter {
public:
  void create(const rastro::MeasurementSetMetadata& metadata, const rastro::VisibilityLayout& layout) override {
    m_metadata = metadata;
    m_layout = layout;
    m_data = xt::xarray<complex_t>::from_shape(
        {layout.num_times, layout.num_baselines, layout.num_channels, layout.num_correlations});
    m_flag = xt::xarray<bool>::from_shape(
        {layout.num_times, layout.num_baselines, layout.num_channels, layout.num_correlations});
    m_weight = xt::xarray<float>::from_shape(
        {layout.num_times, layout.num_baselines, layout.num_channels, layout.num_correlations});
    m_time = xt::xarray<double>::from_shape({layout.num_times, layout.num_baselines});
    m_uvw = xt::xarray<double>::from_shape({layout.num_times, layout.num_baselines, 3});
    m_tile_count = 0;
  }

  void write_tile(const rastro::VisibilityTile& tile) override {
    ++m_tile_count;
    const std::size_t nt = tile.data.shape()[0];
    const std::size_t nb = tile.data.shape()[1];
    const std::size_t nc = tile.data.shape()[2];
    const std::size_t np = tile.data.shape()[3];
    for (std::size_t t = 0; t < nt; ++t) {
      for (std::size_t b = 0; b < nb; ++b) {
        const std::size_t gt = tile.time_start + t;
        const std::size_t gb = tile.baseline_start + b;
        m_time(gt, gb) = tile.time(t, b);
        for (std::size_t d = 0; d < 3; ++d) {
          m_uvw(gt, gb, d) = tile.uvw(t, b, d);
        }
        for (std::size_t c = 0; c < nc; ++c) {
          for (std::size_t p = 0; p < np; ++p) {
            m_data(gt, gb, c, p) = tile.data(t, b, c, p);
            m_flag(gt, gb, c, p) = tile.flag(t, b, c, p);
            m_weight(gt, gb, c, p) = tile.weight(t, b, c, p);
          }
        }
      }
    }
  }

  void finalize() override { m_finalized = true; }

  std::size_t tile_count() const { return m_tile_count; }
  bool finalized() const { return m_finalized; }
  const rastro::MeasurementSetMetadata& metadata() const { return m_metadata; }
  const xt::xarray<complex_t>& data() const { return m_data; }
  const xt::xarray<bool>& flag() const { return m_flag; }
  const xt::xarray<float>& weight() const { return m_weight; }
  const xt::xarray<double>& time() const { return m_time; }
  const xt::xarray<double>& uvw() const { return m_uvw; }

private:
  rastro::MeasurementSetMetadata m_metadata;
  rastro::VisibilityLayout m_layout;
  xt::xarray<complex_t> m_data;
  xt::xarray<bool> m_flag;
  xt::xarray<float> m_weight;
  xt::xarray<double> m_time;
  xt::xarray<double> m_uvw;
  std::size_t m_tile_count = 0;
  bool m_finalized = false;
};

void test_conversion() {
  MemoryReader reader;
  MemoryWriter writer;
  rastro::ConversionOptions options;
  options.time_tile = 3;
  options.baseline_tile = 2;

  std::size_t progress_calls = 0;
  rastro::convert_measurement_set(reader, writer, options,
                                  [&](std::size_t, std::size_t, const rastro::VisibilityTile&) { ++progress_calls; });

  const rastro::VisibilityLayout& layout = reader.layout();
  check(writer.finalized(), "writer finalized");
  // ceil(7/3) * ceil(5/2) = 3 * 3 = 9 tiles.
  check(writer.tile_count() == 9, "tile count");
  check(progress_calls == 9, "progress callback count");
  check(writer.metadata().observation.telescope_name == "SyntheticTelescope", "metadata forwarded");

  for (std::size_t t = 0; t < layout.num_times; ++t) {
    for (std::size_t b = 0; b < layout.num_baselines; ++b) {
      check(writer.time()(t, b) == layout.time(t) + 0.25, "time tile value");
      check(writer.uvw()(t, b, 0) == static_cast<double>(t), "uvw tile value");
      for (std::size_t c = 0; c < layout.num_channels; ++c) {
        for (std::size_t p = 0; p < layout.num_correlations; ++p) {
          check(writer.data()(t, b, c, p) == sample_value(t, b, c, p), "data tile value");
          check(writer.flag()(t, b, c, p) == (((t + b + c + p) % 3) == 0), "flag tile value");
          check(writer.weight()(t, b, c, p) == static_cast<float>(t + b + c + p) + 0.5F, "weight tile value");
        }
      }
    }
  }
}

void test_single_tile() {
  MemoryReader reader;
  MemoryWriter writer;
  rastro::ConversionOptions options;
  options.time_tile = 0;     // all times
  options.baseline_tile = 0; // all baselines
  rastro::convert_measurement_set(reader, writer, options);
  check(writer.tile_count() == 1, "single tile when neither tile size is set");
  check(writer.finalized(), "single tile writer finalized");
}

} // namespace

int main() {
  test_conversion();
  test_single_tile();

  if (g_failures == 0) {
    std::cout << "convert test passed\n";
    return 0;
  }
  std::cerr << g_failures << " checks failed\n";
  return 1;
}
