#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "properties.hpp"

namespace rastro {

/// Read side of the measurement-set property API.
///
/// A reader exposes its metadata and geometry once, then streams rectangular
/// tiles of the visibility grid. Tiles are bounded in memory, so datasets much
/// larger than the available RAM can be converted.
class MeasurementSetReader {
public:
  virtual ~MeasurementSetReader() = default;

  /// Human-readable format name, e.g. `msv2` or `msv4`.
  virtual std::string format_name() const = 0;

  /// All the metadata, read once when the reader is constructed.
  virtual const MeasurementSetMetadata& metadata() const = 0;

  /// Geometry of the visibility grid.
  virtual const VisibilityLayout& layout() const = 0;

  /// Read the tile covering times `[time_start, time_start + num_times)` and
  /// baselines `[baseline_start, baseline_start + num_baselines)`.
  virtual VisibilityTile read_tile(std::size_t time_start, std::size_t baseline_start, std::size_t num_times,
                                   std::size_t num_baselines) const = 0;
};

/// Write side of the measurement-set property API.
///
/// The writer is created empty, then metadata and layout are given once, then
/// tiles are streamed in, and finally the store is closed/committed.
class MeasurementSetWriter {
public:
  virtual ~MeasurementSetWriter() = default;

  /// Create the destination store with the given metadata and geometry.
  virtual void create(const MeasurementSetMetadata& metadata, const VisibilityLayout& layout) = 0;

  /// Write one tile of visibility data. Tiles may arrive in any order.
  virtual void write_tile(const VisibilityTile& tile) = 0;

  /// Commit the store (write indexes, close files, ...).
  virtual void finalize() = 0;
};

/// Tiling configuration for a conversion.
struct ConversionOptions {
  /// Number of integrations per tile.
  std::size_t time_tile = 1;
  /// Number of baselines per tile; 0 selects all baselines.
  std::size_t baseline_tile = 0;
};

/// Progress callback invoked after every written tile.
using TileProgress = std::function<void(std::size_t tile_index, std::size_t tile_count, const VisibilityTile&)>;

/// Stream a measurement set from `reader` into `writer`, tile by tile.
///
/// The metadata and layout are forwarded to the writer, then every tile of the
/// `(time, baseline)` grid is read and written. The peak memory footprint is
/// the size of one tile; no full-dataset buffer is ever allocated.
void convert_measurement_set(const MeasurementSetReader& reader, MeasurementSetWriter& writer,
                             const ConversionOptions& options = {}, const TileProgress& progress = {});

} // namespace rastro
