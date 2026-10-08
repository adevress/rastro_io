#pragma once

#include <concepts>
#include <iosfwd>
#include <string>

namespace rastro {

/// Common interface implemented by every measurement-set format understood by
/// the `rastro` front-end.
///
/// A format is a stateless type exposing two static member functions:
///  * `detect(path)` returns true when `path` can be read as that format;
///  * `summary(path, out, verbose)` prints the metadata summary.
///
/// This concept is the *only* contract shared between the MSv2 and MSv4
/// implementations. Neither format header includes the other, and neither
/// depends on a common base class; satisfying the concept is enough for the
/// front-end to handle a format generically.
template <class Format>
concept MeasurementSetFormat = requires(const std::string& path, std::ostream& out, bool verbose) {
  { Format::detect(path) } -> std::convertible_to<bool>;
  { Format::summary(path, out, verbose) } -> std::same_as<void>;
};

} // namespace rastro
