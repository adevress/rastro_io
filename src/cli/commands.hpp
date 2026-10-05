#pragma once

#include <string>
#include <vector>

namespace rastro {

/// Parse the command line and dispatch to the requested subcommand.
///
/// Returns the process exit code.
int run_command_line(int argc, char** argv);

} // namespace rastro
