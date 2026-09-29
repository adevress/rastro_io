## Code style instructions
- Use `//` comment style and `///` for documentation
- Use `#pragma once` instead of include guards
- Respect the existing namespace hierarchy (everything lives under `rastro`)
- Never use `using namespace` in C++
- Types need to be written in CamelCase
- Functions and variables are in snake_case
- For the rest, in C++ use a style similar to the C++ Core Guidelines

## Project layout
- `tools/rastro/` — source of the `rastro` command-line binary
- Reusable libraries (e.g. the UVH5 I/O layer) live at the repository root and
  are added with `add_subdirectory`

## Compilation instructions
- Create build directory `mkdir -p build`
- Run `cmake -G Ninja -S . -B build && cmake --build build`

## Test instructions
- Run `ctest --test-dir build -V`

## Nix instructions
- Enter the development environment with `nix develop`
- Legacy shell with `nix-shell`

## Formatting instructions
- Run `task format`
