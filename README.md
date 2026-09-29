# rastro-io

## Summary

**rastro-io** is an I/O toolkit for radio astronomy.

Its main objective is to provide a fast, memory-conscious I/O layer for the
[UVH5](https://github.com/RadioAstronomySoftwareGroup/pyuvdata) visibility
format in C++, together with a set of command-line tools to convert back and
forth between UVH5 and other common radio-astronomy formats (notably
MeasurementSet v2).

The project is organised around a single reusable C++ library and the `rastro`
command-line front-end.

## Status

Early development. The current milestone provides the project skeleton, the
Nix/CMake build environment and the first `rastro` subcommands:

- `rastro help` — print usage information.
- `rastro summary <file>` — print metadata of a MeasurementSet v2 visibility
  file using the casacore API, without reading the bulk of the data.

## Requirements

- A C++20 compiler (GCC or Clang)
- CMake >= 3.22
- Ninja (recommended)
- [casacore](https://casacore.github.io/casacore/)
- HDF5
- [HighFive](https://github.com/BlueBrain/HighFive)
- [cxxopts](https://github.com/jarro2783/cxxopts)

All of them are provided by the Nix environment (see below).

## Building

### With Nix (recommended)

```bash
nix develop          # enter a shell with every dependency available
cmake -G Ninja -S . -B build
cmake --build build
./build/tools/rastro/rastro help
```

A legacy `nix-shell` environment is also available:

```bash
nix-shell
```

### Without Nix

Make sure the dependencies above are discoverable by CMake, then:

```bash
cmake -G Ninja -S . -B build
cmake --build build
```

### Fully static binary

The `rastro` binary can be linked as a fully static executable:

```bash
cmake -G Ninja -S . -B build -DRASTRO_STATIC=ON
cmake --build build
```

A fully static link requires static versions of every dependency. With Nix this
is best achieved from a static package set (`pkgsStatic`); see `flake.nix`.

## Usage

```bash
rastro help
rastro summary /path/to/observation.ms
```

`summary` detects MeasurementSet v2 inputs and prints the main metadata
(observations, antennas, fields, spectral windows, polarisations, ...) by reading
only the table metadata and subtables, never the bulk visibility data.

### Compatibility and performance

`summary` was validated against the SKA-Low / OSKAR MeasurementSets of the
`PI26-Low-G3` production datasets (files larger than 100 GB, millions of rows).
Because only the main-table metadata and the small subtables are read, the
command completes in well under a second with a constant, small memory
footprint, independently of the file size.

## Tests

```bash
ctest --test-dir build -V
```

An optional integration test runs `summary` over every `*.ms` found below a
directory. Point `RASTRO_MS_DATASETS` at it at configure time:

```bash
RASTRO_MS_DATASETS=/path/to/datasets cmake -G Ninja -S . -B build
ctest --test-dir build --output-on-failure
```

## License

This project is released under the [European Union Public Licence v1.2
(EUPL-1.2)](https://eupl.eu/1.2/en/). The full licence text is available in
[`LICENSE`](./LICENSE).

## Contributions

Contributions are welcome when they align with the objectives of the project.
