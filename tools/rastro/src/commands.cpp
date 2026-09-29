#include "commands.hpp"

#include "measurement_set_summary.hpp"

#include <cxxopts.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace rastro {
namespace {

constexpr const char* general_usage = R"(rastro — I/O toolkit for radio astronomy

Usage:
  rastro help [command]
  rastro summary <file>

Commands:
  help      Print usage information
  summary   Print metadata of a visibility file (MeasurementSet v2)
)";

constexpr const char* summary_usage = R"(rastro summary — print metadata of a visibility file

Usage:
  rastro summary <file>

The file must be a MeasurementSet v2. Only the table metadata and the small
subtables are read; the bulk visibility data is never loaded, so the command
stays cheap even for very large files.
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
  options.add_options()("h,help", "Print usage")("file", "Visibility file (MeasurementSet v2)",
                                                 cxxopts::value<std::string>());
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

  if (is_measurement_set(path)) {
    print_measurement_set_summary(path, std::cout);
    return 0;
  }

  std::cerr << "error: unsupported file format (expected a MeasurementSet v2): " << path << "\n";
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
