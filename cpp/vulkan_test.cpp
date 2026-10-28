#include "core.hpp"
#include "vulkan_remap.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace imaging;
using namespace cv;
static void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static VulkanResult reference(const Images &images, const std::array<Mat, 2> &forward,
                              const std::array<Mat, 2> &backward, bool verify) {
  VulkanResult out;
  out.channels[0] = images[0];
  out.validity = Mat(480, 640, CV_8U, Scalar(255));
  for (int camera = 0; camera < 2; camera++) {
    Mat flow, back, coords(480, 640, CV_32FC2), mapped, reverse;
    if (forward[camera].cols == 640)
      flow = forward[camera];
    else {
      resize(forward[camera], flow, Size(640, 480));
      flow *= 640.f / forward[camera].cols;
    }
    for (int y = 0; y < 480; y++)
      for (int x = 0; x < 640; x++)
        coords.at<Vec2f>(y, x) = flow.at<Vec2f>(y, x) + Vec2f(x, y);
    remap(images[camera + 1], mapped, coords, noArray(), INTER_LINEAR);
    if (verify) {
      if (backward[camera].cols == 640)
        back = backward[camera];
      else {
        resize(backward[camera], back, Size(640, 480));
        back *= 640.f / backward[camera].cols;
      }
      remap(back, reverse, coords, noArray(), INTER_LINEAR, BORDER_CONSTANT, Scalar(1000, 1000));
    }
    for (int y = 0; y < 480; y++)
      for (int x = 0; x < 640; x++) {
        Vec2f f = flow.at<Vec2f>(y, x), p = coords.at<Vec2f>(y, x);
        bool usable = std::isfinite(p[0]) && std::isfinite(p[1]) && f.dot(f) < 3600;
        if (!usable)
          mapped.at<uchar>(y, x) = images[camera + 1].at<uchar>(y, x);
        if (verify) {
          Vec2f e = f + reverse.at<Vec2f>(y, x);
          bool valid = usable && e.dot(e) < 2.25 && p[0] >= 0 && p[0] < 639 && p[1] >= 0 &&
                       p[1] < 479 && images[0].at<uchar>(y, x) > 2 && mapped.at<uchar>(y, x) > 2;
          if (!valid)
            out.validity.at<uchar>(y, x) = 0;
        }
      }
    out.channels[camera + 1] = mapped;
  }
  return out;
}
int main() {
  try {
    setNumThreads(2);
    bool allowSoftware = std::getenv("IMAGING_TEST_ALLOW_SOFTWARE_VULKAN") != nullptr;
    auto devices = vulkanInfo();
    bool available = false;
    for (auto &d : devices["devices"])
      if (d.value("compute", false) && (allowSoftware || !d.value("software", true)))
        available = true;
    if (!available) {
      std::cout << "SKIP: no selected Vulkan device: " << devices.dump() << '\n';
      return 77;
    }
    RNG rng(62);
    Images g;
    for (auto &image : g) {
      image.create(480, 640, CV_8U);
      rng.fill(image, RNG::UNIFORM, 0, 256);
    }
    for (int width : {320, 480, 640}) {
      VulkanRemapper gpu(width, allowSoftware);
      std::cout << "Testing " << gpu.info().dump() << " width=" << width << std::endl;
      std::array<Mat, 2> forward, backward;
      for (int i = 0; i < 2; i++) {
        forward[i].create(width * 3 / 4, width, CV_32FC2);
        backward[i].create(width * 3 / 4, width, CV_32FC2);
        for (int y = 0; y < forward[i].rows; y++)
          for (int x = 0; x < width; x++) {
            Vec2f flow((i ? -.8f : 1.2f) + std::sin(x * .03f) * .7f, std::cos(y * .04f) * .6f);
            forward[i].at<Vec2f>(y, x) = flow;
            backward[i].at<Vec2f>(y, x) = -flow;
          }
      }
      for (bool verify : {false, true}) {
        auto expected = reference(g, forward, backward, verify),
             actual = gpu.apply(g, forward, backward, verify);
        for (int i = 1; i < 3; i++) {
          Mat difference;
          absdiff(expected.channels[i], actual.channels[i], difference);
          double average = mean(difference)[0], maximum = norm(difference, NORM_INF);
          std::cout << " check=" << verify << " camera=" << i + 1 << " mean difference=" << average
                    << " max=" << maximum << std::endl;
          check(average < .15 && maximum <= 12,
                "Vulkan remap does not agree with CPU interpolation");
        }
        Mat mismatch;
        cv::compare(expected.validity, actual.validity, mismatch, CMP_NE);
        check(double(countNonZero(mismatch)) / (640 * 480) < .002,
              "Vulkan consistency mask differs excessively");
        check(norm(actual.channels[0], g[0], NORM_INF) == 0, "Reference camera changed");
      }
      // Exercise large displacements, nonfinite data and border interpolation.
      forward[0].setTo(Scalar(80, 0));
      forward[1].setTo(Scalar(-.015625, .015625));
      forward[1].at<Vec2f>(20, 20) = Vec2f(std::numeric_limits<float>::quiet_NaN(), 0);
      auto expected = reference(g, forward, backward, true),
           actual = gpu.apply(g, forward, backward, true);
      check(norm(actual.channels[1], g[1], NORM_INF) == 0,
            "Large flow was not replaced by original channel");
      check(countNonZero(actual.validity) == 0, "Invalid large flow passed confidence");
      Mat difference;
      absdiff(expected.channels[2], actual.channels[2], difference);
      check(mean(difference)[0] < .2, "Subpixel or nonfinite boundary mismatch");
    }
    std::cout << "PASS: Vulkan/CPU parity, bounds, invalid flow, confidence, 320/480/640, "
                 "checked/unchecked"
              << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << std::endl;
    return 1;
  }
}
