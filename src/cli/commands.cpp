#include "commands.hpp"

#include "meas/measurement_set.hpp"
#include "measurement_set_format.hpp"
#include "msv2/measurement_set.hpp"
#include "msv2/msv2_reader.hpp"
#include "msv2/msv2_writer.hpp"
#include "msv4/measurement_set.hpp"
#include "msv4/msv4_reader.hpp"
#include "msv4/msv4_writer.hpp"

#include <cxxopts.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
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
  rastro convert --to-format=<msv2|msv4> <source> <destination>

Commands:
  help      Print usage information
  summary   Print metadata of a visibility file (MeasurementSet v2 or MSv4)
  convert   Convert a visibility file between formats
)";

constexpr const char* convert_usage = R"(rastro convert — convert a visibility file between formats

Usage:
  rastro convert --to-format=<msv2|msv4> [--time-tile=<n>] [--baseline-tile=<n>] <source> <destination>

Converts between MeasurementSet v2 (casacore directory) and MSv4 (Zarr v3
processing set). The destination is created if its parent directory does not
exist; if the destination itself already exists the command fails.

Data is streamed tile by tile, so files larger than memory can be converted.

Options:
  --to-format=<msv2|msv4>   Output format (required)
  --time-tile=<n>           Integrations per tile (default 1)
  --baseline-tile=<n>       Baselines per tile (default all)
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

void print_convert_usage(std::ostream& out) { out << convert_usage; }

/// Handle `rastro help [command]`.
int run_help(const std::vector<std::string>& args) {
  if (!args.empty() && (args[0] == "summary")) {
    print_summary_usage(std::cout);
    return 0;
  }
  if (!args.empty() && (args[0] == "convert")) {
    print_convert_usage(std::cout);
    return 0;
  }
  print_general_usage(std::cout);
  return 0;
}

/// Turn a token vector into an argv-style vector (cxxopts needs `char*`).
bool parse_options(cxxopts::Options& options, const std::string& program, const std::vector<std::string>& args,
                   cxxopts::ParseResult& result, std::ostream& error, const char* usage) {
  try {
    std::vector<std::string> tokens;
    tokens.reserve(args.size() + 1);
    tokens.push_back(program);
    tokens.insert(tokens.end(), args.begin(), args.end());

    std::vector<const char*> argv;
    argv.reserve(tokens.size());
    for (const std::string& token : tokens) {
      argv.push_back(token.c_str());
    }
    result = options.parse(static_cast<int>(argv.size()), argv.data());
    return true;
  } catch (const cxxopts::exceptions::exception& exception) {
    error << "error: " << exception.what() << "\n\n" << usage;
    return false;
  }
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

/// Open a reader for whichever supported format `path` holds.
std::unique_ptr<MeasurementSetReader> open_reader(const std::string& path) {
  if (is_msv2_measurement_set(path)) {
    return std::make_unique<Msv2Reader>(path);
  }
  if (is_msv4_processing_set(path)) {
    return std::make_unique<Msv4Reader>(path);
  }
  return nullptr;
}

/// Handle `rastro convert --to-format=<...> <source> <destination>`.
int run_convert(const std::vector<std::string>& args) {
  cxxopts::Options options("rastro convert", "Convert a visibility file between formats");
  options.add_options()("h,help", "Print usage")("to-format", "Output format (msv2 or msv4)",
                                                 cxxopts::value<std::string>())(
      "time-tile", "Integrations per tile", cxxopts::value<std::size_t>()->default_value("1"))(
      "baseline-tile", "Baselines per tile (0 = all)", cxxopts::value<std::size_t>()->default_value("0"))(
      "source", "Source visibility file", cxxopts::value<std::string>())("destination", "Destination visibility file",
                                                                         cxxopts::value<std::string>());
  options.parse_positional({"source", "destination"});

  cxxopts::ParseResult result;
  if (!parse_options(options, "rastro convert", args, result, std::cerr, convert_usage)) {
    return 1;
  }
  if (result.count("help") > 0) {
    print_convert_usage(std::cout);
    return 0;
  }
  if (result.count("to-format") == 0 || result.count("source") == 0 || result.count("destination") == 0) {
    std::cerr << "error: expected --to-format, <source> and <destination>\n\n";
    print_convert_usage(std::cerr);
    return 1;
  }

  const std::string to_format = result["to-format"].as<std::string>();
  const std::string source = result["source"].as<std::string>();
  const std::string destination = result["destination"].as<std::string>();

  if (!std::filesystem::exists(source)) {
    std::cerr << "error: no such file or directory: " << source << "\n";
    return 1;
  }
  if (std::filesystem::exists(destination)) {
    std::cerr << "error: destination already exists: " << destination << "\n";
    return 1;
  }

  std::unique_ptr<MeasurementSetWriter> writer;
  if (to_format == "msv2") {
    writer = std::make_unique<Msv2Writer>(destination);
  } else if (to_format == "msv4") {
    writer = std::make_unique<Msv4Writer>(destination);
  } else {
    std::cerr << "error: unknown --to-format '" << to_format << "' (expected msv2 or msv4)\n";
    return 1;
  }

  try {
    const std::unique_ptr<MeasurementSetReader> reader = open_reader(source);
    if (!reader) {
      std::cerr << "error: unsupported source format: " << source << "\n";
      return 1;
    }

    const std::filesystem::path parent = std::filesystem::path(destination).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    ConversionOptions conversion;
    conversion.time_tile = result["time-tile"].as<std::size_t>();
    conversion.baseline_tile = result["baseline-tile"].as<std::size_t>();

    const VisibilityLayout& layout = reader->layout();
    std::cout << "Converting " << source << " [" << reader->format_name() << "] -> " << destination << " [" << to_format
              << "]\n";
    std::cout << "  grid: " << layout.num_times << " times x " << layout.num_baselines << " baselines x "
              << layout.num_channels << " channels x " << layout.num_correlations << " correlations\n";

    std::size_t last_percent = static_cast<std::size_t>(-1);
    convert_measurement_set(*reader, *writer, conversion,
                            [&](std::size_t index, std::size_t total, const VisibilityTile&) {
                              const std::size_t percent = total == 0 ? 100 : (100 * (index + 1)) / total;
                              if (percent != last_percent) {
                                std::cout << "  progress: " << percent << "% (" << (index + 1) << "/" << total << ")\n";
                                last_percent = percent;
                              }
                            });
    std::cout << "Done.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
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
  if (command == "convert") {
    return run_convert(args);
  }

  std::cerr << "error: unknown command '" << command << "'\n\n";
  print_general_usage(std::cerr);
  return 1;
}

} // namespace rastro
