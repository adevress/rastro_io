// Integration test for the MSv4 reader. Requires a real processing set whose
// path is passed on the command line (the CMake test is registered from
// RASTRO_MSV4_DATASET).

#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <string>

#include "msv4/measurement_set.hpp"

namespace {

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << "\n";
  return 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: rastro_msv4_reader_test <processing_set.zarr>\n";
    return 2;
  }

  try {
    rastro::ProcessingSet ps(argv[1]);
    if (ps.partitions().empty()) {
      return fail("no partitions found");
    }

    rastro::MeasurementSetV4 ms(ps.store(), ps.partitions().front());

    const std::vector<std::size_t> shape = ms.shape("VISIBILITY");
    if (shape.size() != 4) {
      return fail("VISIBILITY is not four-dimensional");
    }

    // Bulk data, one integration (chunk-aligned along time).
    const auto vis = ms.read<std::complex<float>>("VISIBILITY", {rastro::ZarrSlice{0, 1}});
    if (vis.shape()[0] != 1 || vis.shape()[1] != shape[1] || vis.shape()[2] != shape[2] || vis.shape()[3] != shape[3]) {
      return fail("VISIBILITY region shape mismatch");
    }
    double magnitude = 0.0;
    for (const auto& value : vis) {
      magnitude += std::abs(value);
    }
    if (!std::isfinite(magnitude) || magnitude <= 0.0) {
      return fail("VISIBILITY slice is empty");
    }

    const auto flag = ms.read<bool>("FLAG", {rastro::ZarrSlice{0, 1}});
    if (flag.shape() != vis.shape()) {
      return fail("FLAG shape does not match VISIBILITY");
    }

    const auto weight = ms.read<float>("WEIGHT", {rastro::ZarrSlice{0, 1}});
    if (weight.shape() != vis.shape()) {
      return fail("WEIGHT shape does not match VISIBILITY");
    }

    // A non-chunk-aligned, strided slice.
    const auto small = ms.read<std::complex<float>>("VISIBILITY", {rastro::ZarrSlice{7, 3}, rastro::ZarrSlice{100, 10},
                                                                   rastro::ZarrSlice{200, 5}, rastro::ZarrSlice{1, 2}});
    if (small.shape()[0] != 3 || small.shape()[1] != 10 || small.shape()[2] != 5 || small.shape()[3] != 2) {
      return fail("strided VISIBILITY region shape mismatch");
    }

    // Coordinates and metadata.
    const auto time = ms.time();
    if (time.size() == 0 || !std::isfinite(time(0))) {
      return fail("time coordinate is empty");
    }
    if (ms.frequency().size() == 0) {
      return fail("frequency coordinate is empty");
    }
    if (ms.antenna_name().empty()) {
      return fail("antenna_name is empty");
    }
    if (ms.polarization().empty()) {
      return fail("polarization is empty");
    }

    std::cout << "MSv4 reader test passed: " << ps.partitions().front() << " shape=(" << shape[0] << "," << shape[1]
              << "," << shape[2] << "," << shape[3] << ") |sum|=" << magnitude << "\n";
    return 0;
  } catch (const std::exception& error) {
    return fail(error.what());
  }
}
