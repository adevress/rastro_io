#pragma once

#include "measurement_set_format.hpp"

#include <iosfwd>
#include <string>

namespace rastro {

/// Returns true if the given path points to a readable MeasurementSet v2.
///
/// Only the table metadata is inspected; no visibility data is read.
bool is_msv2_measurement_set(const std::string& path);

/// Print all the relevant metadata of a MeasurementSet v2.
///
/// When `verbose` is true, also print the per-antenna, per-field and per-feed
/// details and the log details stored in the HISTORY subtable of the
/// MeasurementSet.
///
/// The implementation opens the main table and its (small) subtables through
/// the casacore API and never reads the bulk visibility columns, which keeps
/// the operation cheap even for very large files.
void print_msv2_summary(const std::string& path, std::ostream& out, bool verbose = false);

/// MSv2 adapter for the `MeasurementSetFormat` concept. This is the only
/// surface the front-end needs; no MSv2 header content leaks into MSv4.
struct MeasurementSetV2Format {
  static bool detect(const std::string& path) { return is_msv2_measurement_set(path); }
  static void summary(const std::string& path, std::ostream& out, bool verbose) {
    print_msv2_summary(path, out, verbose);
  }
};

static_assert(MeasurementSetFormat<MeasurementSetV2Format>);

} // namespace rastro
