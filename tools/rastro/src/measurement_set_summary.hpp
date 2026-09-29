#pragma once

#include <iosfwd>
#include <string>

namespace rastro {

/// Returns true if the given path points to a readable MeasurementSet v2.
///
/// Only the table metadata is inspected; no visibility data is read.
bool is_measurement_set(const std::string& path);

/// Print all the relevant metadata of a MeasurementSet v2.
///
/// The implementation opens the main table and its (small) subtables through
/// the casacore API and never reads the bulk visibility columns, which keeps
/// the operation cheap even for very large files.
void print_measurement_set_summary(const std::string& path, std::ostream& out);

} // namespace rastro
