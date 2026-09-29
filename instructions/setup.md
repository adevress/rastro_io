# Setup

Rastro-io is a project of I/O toolkit for radiostronomy

Particularly, it aims to provide an I/O layer for UVH5 in C++ and a set of tools to convert back and for from this format

You will in order for this project do

 1 - Write a read me

 2- Fetch an AGENTS.md inspired from the one of arcane-lang for the coding style. Ask me for review

 3- Setup a C++ CMake based project with C++20 as a requirements

 4- Setup an environment with a default.nix and a flake that can resolve the following dependencies:
   - CMake
   - clang-format
   - Highfive (the df5 library, last version)
   - HDF5
   - Casacore
   - GCC
   - cxxopts

  You can use Nix and astro-nix

5 - You will create a foler tools/. Within tool I want the source code and cmake configuration for a binary tool named rastro

 The CMake need to have an option to make rastro a purely static binary

 Rastro need to have the subcommands "help" and  "summary"

 Summary will accept a parameter that can be a file. 

 If the file is a MSv2 file. You should print all the relevant metadata informations about this visibility file using the casacore API.

 Note that the summary command should avoid to read the entire file. It will be problematic for big files
