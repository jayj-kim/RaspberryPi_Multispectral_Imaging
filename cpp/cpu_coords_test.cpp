#include "cpu_coords.hpp"
#include <cmath>
#include <iostream>
#include <limits>
using namespace imaging;
using namespace cv;
static bool equal(float a, float b) {
  return a == b || (std::isnan(a) && std::isnan(b));
}
int main() {
  try {
    setNumThreads(2);
    RNG rng(998);
    for (Size size : {Size(640, 480), Size(19, 7)}) {
      Mat input(size, CV_32FC2);
      rng.fill(input, RNG::UNIFORM, -100, 100);
      const float inf = std::numeric_limits<float>::infinity();
      const float nan = std::numeric_limits<float>::quiet_NaN();
      std::vector<Vec2f> cases = {{0, 0},
                                  {60, 0},
                                  {-60, 0},
                                  {0, 60},
                                  {std::nextafter(60.f, 0.f), 0},
                                  {std::nextafter(60.f, inf), 0},
                                  {nan, 0},
                                  {0, nan},
                                  {inf, 0},
                                  {0, -inf},
                                  {3.e38f, 3.e38f},
                                  {-0.f, 0.f},
                                  {0.015625f, -0.015625f},
                                  {-59.99f, .5f}};
      // Exercise each vector lane, scalar tails, borders and exceptional values.
      for (int y = 0; y < size.height; ++y)
        for (int x = 0; x < size.width; ++x)
          if (y < 3 || x >= size.width - 16)
            input.at<Vec2f>(y, x) = cases[(x + y) % cases.size()];
      for (float scale : {1.f, 2.f, 640.f / 480})
        for (bool retain : {false, true})
          for (bool simd : {false, true}) {
            Mat flow = input.clone(), coords, invalid;
            prepareCoordinates(flow, coords, invalid, scale, retain, simd);
            for (int y = 0; y < size.height; ++y)
              for (int x = 0; x < size.width; ++x) {
                auto original = input.at<Vec2f>(y, x), value = original;
                if (scale != 1.f)
                  value *= scale;
                auto expected = value + Vec2f(float(x), float(y));
                auto actual = coords.at<Vec2f>(y, x);
                bool good = std::isfinite(expected[0]) && std::isfinite(expected[1]) &&
                            value.dot(value) < 3600;
                if (invalid.at<uchar>(y, x) != (good ? 0 : 255))
                  throw std::runtime_error("Validity threshold differs from baseline");
                auto stored = flow.at<Vec2f>(y, x), wanted = retain ? value : original;
                for (int i = 0; i < 2; ++i)
                  if (!equal(actual[i], expected[i]) || !equal(stored[i], wanted[i]))
                    throw std::runtime_error("Coordinate/scaled-flow output mismatch");
              }
          }
    }
    std::cout << "PASS: " << cpuCoordinateKernel()
              << ", scalar fallback, tails, NaN/Inf, threshold and retained flow\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
