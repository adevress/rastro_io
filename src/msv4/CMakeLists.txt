## Reusable library with the MSv4 (Zarr v2/v3) reading logic.
##
## The Zarr reader/writer is implemented from scratch in zarr_io.cpp. It only
## depends on xtensor, nlohmann_json and the Zarr compression libraries.
add_library(rastro_msv4
  measurement_set.cpp
  msv4_reader.cpp
  msv4_summary.cpp
  msv4_writer.cpp
  zarr_io.cpp
)

target_include_directories(rastro_msv4
  PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(rastro_msv4 PRIVATE ${RASTRO_COMMON_FLAGS})

pkg_check_modules(BLOSC REQUIRED IMPORTED_TARGET blosc)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
find_package(ZLIB REQUIRED)

target_link_libraries(rastro_msv4
  PUBLIC
    rastro_meas
    PkgConfig::XTENSOR
    xtl
    nlohmann_json::nlohmann_json
    PkgConfig::BLOSC
    PkgConfig::ZSTD
    PkgConfig::LZ4
    ZLIB::ZLIB
)

## Integration test over a real MSv4 processing set. Enable it by pointing
## RASTRO_MSV4_DATASET at a `.ps.zarr` directory at configure time.
add_executable(rastro_msv4_reader_test tests/msv4_reader_test.cpp)
target_link_libraries(rastro_msv4_reader_test PRIVATE rastro_msv4)
target_compile_options(rastro_msv4_reader_test PRIVATE ${RASTRO_COMMON_FLAGS})

## Self-contained round-trip test for the Zarr reader/writer.
add_executable(rastro_zarr_io_test tests/zarr_io_test.cpp)
target_link_libraries(rastro_zarr_io_test PRIVATE rastro_msv4)
target_compile_options(rastro_zarr_io_test PRIVATE ${RASTRO_COMMON_FLAGS})
add_test(NAME rastro_zarr_io COMMAND rastro_zarr_io_test)

if(DEFINED ENV{RASTRO_MSV4_DATASET})
  add_test(
    NAME rastro_msv4_reader
    COMMAND rastro_msv4_reader_test $ENV{RASTRO_MSV4_DATASET}
  )
endif()

## Self-contained MSv4 writer/reader round-trip test.
add_executable(rastro_msv4_roundtrip_test tests/msv4_roundtrip_test.cpp)
target_link_libraries(rastro_msv4_roundtrip_test PRIVATE rastro_msv4)
target_compile_options(rastro_msv4_roundtrip_test PRIVATE ${RASTRO_COMMON_FLAGS})
add_test(NAME rastro_msv4_roundtrip COMMAND rastro_msv4_roundtrip_test)
