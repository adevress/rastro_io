#include "commands.hpp"

#include "measurement_set_format.hpp"
#include "msv2/measurement_set.hpp"
#include "msv4/measurement_set.hpp"

#include <cxxopts.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace rastro {
namespace {

/// Dispatch a summary request to a measurement-set format.
///
/// The format only has to satisfy `MeasurementSetFormat`; the front-end never
/// touches a format-specific header beyond the adapter type.
template <MeasurementSetFormat Format>
bool print_summary_if_format(const std::string& path, std::ostream& out, bool verbose) {
  if (!Format::detect(path)) {
    return false;
  }
  Format::summary(path, out, verbose);
  return true;
}

constexpr const char* general_usage = R"(rastro — I/O toolkit for radio astronomy

Usage:
  rastro help [command]
  rastro summary [--verbose] <file>

Commands:
  help      Print usage information
  summary   Print metadata of a visibility file (MeasurementSet v2 or MSv4)
)";

constexpr const char* summary_usage = R"(rastro summary — print metadata of a visibility file

Usage:
  rastro summary [--verbose] <file>

The file must be a MeasurementSet v2 directory or an MSv4 processing set
(Zarr v3). Only the table/array metadata and the small subtables or coordinate
arrays are read; the bulk visibility data is never loaded, so the command
stays cheap even for very large files.

Options:
  --verbose   Print also per-antenna/per-field/per-feed details and the log (HISTORY table or sub-datasets)
)";

void print_general_usage(std::ostream& out) { out << general_usage; }

void print_summary_usage(std::ostream& out) { out << summary_usage; }

/// Handle `rastro help [command]`.
int run_help(const std::vector<std::string>& args) {
  if (!args.empty() && (args[0] == "summary")) {
    print_summary_usage(std::cout);
    return 0;
  }
  print_general_usage(std::cout);
  return 0;
}

/// Handle `rastro summary <file>`.
int run_summary(const std::vector<std::string>& args) {
  cxxopts::Options options("rastro summary", "Print metadata of a visibility file");
  options.add_options()("h,help", "Print usage")(
      "verbose", "Print also per-antenna/per-field/per-feed details and the log (HISTORY table)",
      cxxopts::value<bool>())("file", "Visibility file (MeasurementSet v2 or MSv4)", cxxopts::value<std::string>());
  options.parse_positional({"file"});

  cxxopts::ParseResult result;
  try {
    std::vector<std::string> tokens;
    tokens.reserve(args.size() + 1);
    tokens.emplace_back("rastro summary");
    tokens.insert(tokens.end(), args.begin(), args.end());

    std::vector<const char*> argv;
    argv.reserve(tokens.size());
    for (const std::string& token : tokens) {
      argv.push_back(token.c_str());
    }

    result = options.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const cxxopts::exceptions::exception& error) {
    std::cerr << "error: " << error.what() << "\n\n";
    print_summary_usage(std::cerr);
    return 1;
  }

  if (result.count("help") > 0) {
    print_summary_usage(std::cout);
    return 0;
  }

  if (result.count("file") == 0) {
    std::cerr << "error: missing <file> argument\n\n";
    print_summary_usage(std::cerr);
    return 1;
  }

  const std::string path = result["file"].as<std::string>();

  if (!std::filesystem::exists(path)) {
    std::cerr << "error: no such file or directory: " << path << "\n";
    return 1;
  }

  const bool verbose = result.count("verbose") > 0;

  if (print_summary_if_format<MeasurementSetV2Format>(path, std::cout, verbose)) {
    return 0;
  }

  if (print_summary_if_format<MeasurementSetV4Format>(path, std::cout, verbose)) {
    return 0;
  }

  std::cerr << "error: unsupported file format (expected a MeasurementSet v2 or an MSv4 processing set): " << path
            << "\n";
  return 1;
}

} // namespace

int run_command_line(int argc, char** argv) {
  if (argc < 2) {
    print_general_usage(std::cerr);
    return 1;
  }

  const std::string command = argv[1];
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  if (command == "help" || command == "--help" || command == "-h") {
    return run_help(args);
  }
  if (command == "summary") {
    return run_summary(args);
  }

  std::cerr << "error: unknown command '" << command << "'\n\n";
  print_general_usage(std::cerr);
  return 1;
}

} // namespace rastro
