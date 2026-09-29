add_executable(rastro
  src/main.cpp
  src/commands.cpp
  src/measurement_set_summary.cpp
)

target_include_directories(rastro
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(rastro PRIVATE ${RASTRO_COMMON_FLAGS})

target_link_libraries(rastro
  PRIVATE
    PkgConfig::CASACORE
    cxxopts::cxxopts
)

if(RASTRO_STATIC)
  target_link_options(rastro PRIVATE ${RASTRO_STATIC_LINK_FLAGS})
endif()

add_test(NAME rastro_help COMMAND rastro help)
add_test(NAME rastro_no_args COMMAND rastro)
set_tests_properties(rastro_no_args PROPERTIES WILL_FAIL TRUE)

# Optional integration test over real MeasurementSet datasets.
# Enable it by pointing RASTRO_MS_DATASETS at a directory that contains them.
if(DEFINED ENV{RASTRO_MS_DATASETS})
  add_test(
    NAME rastro_summary_datasets
    COMMAND
      bash
      ${CMAKE_SOURCE_DIR}/scripts/summary-ms-datasets.sh
      $<TARGET_FILE:rastro>
      $ENV{RASTRO_MS_DATASETS}
  )
endif()

install(TARGETS rastro RUNTIME DESTINATION bin)
