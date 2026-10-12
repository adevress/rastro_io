## Format-independent measurement-set property API and conversion engine.
add_library(rastro_meas
  convert.cpp
  posix_io.cpp
)

target_include_directories(rastro_meas
  PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(rastro_meas PRIVATE ${RASTRO_COMMON_FLAGS})

target_link_libraries(rastro_meas
  PUBLIC
    PkgConfig::XTENSOR
    xtl
)

## Positional-I/O unit test (preadv/pwritev, scatter/gather).
add_executable(rastro_posix_io_test tests/posix_io_test.cpp)
target_link_libraries(rastro_posix_io_test PRIVATE rastro_meas)
target_compile_options(rastro_posix_io_test PRIVATE ${RASTRO_COMMON_FLAGS})
add_test(NAME rastro_posix_io COMMAND rastro_posix_io_test)

## Conversion-engine unit test with an in-memory reader/writer.
add_executable(rastro_convert_test tests/convert_test.cpp)
target_link_libraries(rastro_convert_test PRIVATE rastro_meas)
target_compile_options(rastro_convert_test PRIVATE ${RASTRO_COMMON_FLAGS})
add_test(NAME rastro_convert COMMAND rastro_convert_test)
