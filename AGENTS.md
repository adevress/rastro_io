## Code style instructions
- Use `//` comment style and `///` for documentation
- Use `#pragma once` instead of include guards
- Respect the existing namespace hierarchy (everything lives under `rastro`)
- Never use `using namespace` in C++
- Types need to be written in CamelCase
- Functions and variables are in snake_case
- For the rest, in C++ use a style similar to the C++ Core Guidelines

## Project layout
- `src/cli/` — source of the `rastro` command-line binary
- `src/msv2/` — reusable library with the MeasurementSet v2 reading logic
- Reusable libraries live under `src/` and are added with `add_subdirectory`

## Compilation instructions
- Create build directory `mkdir -p build-agent`
- Run `cmake -G Ninja -S . -B build-agent && cmake --build build-agent`

## Test instructions
- Run `ctest --test-dir build -V`

## Nix instruction
- Enter the development environment with `nix develop`
- Legacy shell with `nix-shell`

## Formatting instructions
- Run `task format`
