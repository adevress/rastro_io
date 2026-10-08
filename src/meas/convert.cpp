#include "measurement_set.hpp"

#include <algorithm>
#include <cstddef>

namespace rastro {
namespace {

/// Clamp a tile size to a positive count no larger than `total`.
std::size_t effective_tile(std::size_t requested, std::size_t total) {
  if (total == 0) {
    return 0;
  }
  if (requested == 0 || requested > total) {
    return total;
  }
  return requested;
}

} // namespace

void convert_measurement_set(const MeasurementSetReader& reader, MeasurementSetWriter& writer,
                             const ConversionOptions& options, const TileProgress& progress) {
  const VisibilityLayout& layout = reader.layout();
  const MeasurementSetMetadata& metadata = reader.metadata();

  writer.create(metadata, layout);

  const std::size_t time_tile = effective_tile(options.time_tile, layout.num_times);
  const std::size_t baseline_tile = effective_tile(options.baseline_tile, layout.num_baselines);

  if (time_tile == 0 || baseline_tile == 0) {
    writer.finalize();
    return;
  }

  const std::size_t time_tiles = (layout.num_times + time_tile - 1) / time_tile;
  const std::size_t baseline_tiles = (layout.num_baselines + baseline_tile - 1) / baseline_tile;
  const std::size_t tile_count = time_tiles * baseline_tiles;

  std::size_t tile_index = 0;
  for (std::size_t time_start = 0; time_start < layout.num_times; time_start += time_tile) {
    const std::size_t time_count = std::min(time_tile, layout.num_times - time_start);
    for (std::size_t baseline_start = 0; baseline_start < layout.num_baselines; baseline_start += baseline_tile) {
      const std::size_t baseline_count = std::min(baseline_tile, layout.num_baselines - baseline_start);
      const VisibilityTile tile = reader.read_tile(time_start, baseline_start, time_count, baseline_count);
      writer.write_tile(tile);
      if (progress) {
        progress(tile_index, tile_count, tile);
      }
      ++tile_index;
    }
  }

  writer.finalize();
}

} // namespace rastro
