#include "core.hpp"
#include <iostream>
using namespace imaging;
using namespace cv;

// Independent pre-optimization math, including full-resolution roundtrip checks.
static Refined reference(const Images &g, int width, bool checked) {
  auto engine = DISOpticalFlow::create(DISOpticalFlow::PRESET_FAST);
  auto clahe = createCLAHE(2, Size(8, 8));
  Images small;
  for (int i = 0; i < 3; ++i) {
    Mat a;
    resize(g[i], a, Size(width, width * 3 / 4));
    clahe->apply(a, small[i]);
  }
  Refined r;
  r.channels[0] = g[0];
  r.validity = Mat(480, 640, CV_8U, Scalar(255));
  for (int cam = 1; cam < 3; ++cam) {
    Mat low, flow, coords(480, 640, CV_32FC2), lowBack, back, reverse;
    engine->calc(small[0], small[cam], low);
    resize(low, flow, Size(640, 480));
    flow *= 640.f / width;
    for (int y = 0; y < 480; ++y)
      for (int x = 0; x < 640; ++x)
        coords.at<Vec2f>(y, x) = flow.at<Vec2f>(y, x) + Vec2f(x, y);
    remap(g[cam], r.channels[cam], coords, noArray(), INTER_LINEAR);
    if (checked) {
      engine->calc(small[cam], small[0], lowBack);
      resize(lowBack, back, Size(640, 480));
      back *= 640.f / width;
      remap(back, reverse, coords, noArray(), INTER_LINEAR, BORDER_CONSTANT, Scalar(1000, 1000));
    }
    for (int y = 0; y < 480; ++y)
      for (int x = 0; x < 640; ++x) {
        auto f = flow.at<Vec2f>(y, x), c = coords.at<Vec2f>(y, x);
        bool good = std::isfinite(c[0]) && std::isfinite(c[1]) && f.dot(f) < 3600;
        auto &pixel = r.channels[cam].at<uchar>(y, x);
        if (!good)
          pixel = g[cam].at<uchar>(y, x);
        if (checked) {
          auto error = f + reverse.at<Vec2f>(y, x);
          good = good && error.dot(error) < 2.25 && c[0] >= 0 && c[0] < 639 && c[1] >= 0 &&
                 c[1] < 479 && g[0].at<uchar>(y, x) > 2 && pixel > 2;
          r.validity.at<uchar>(y, x) &= good ? 255 : 0;
        }
      }
  }
  return r;
}
int main() {
  try {
    setNumThreads(2);
    Mat image(480, 640, CV_8U);
    randu(image, 0, 255);
    for (double shift : {0., 0.0001, 4.25}) {
      Mat h = Mat::eye(3, 3, CV_64F), expected;
      h.at<double>(0, 2) = shift;
      warpPerspective(image, expected, h, Size(640, 480));
      auto actual = warp(image, h, json{0, 0});
      if (norm(actual, expected, NORM_INF) != 0 || actual.data == image.data)
        throw std::runtime_error("Identity shortcut changed warp math/output ownership");
    }
    RNG rng(421);
    for (int width : {320, 480, 640}) {
      LocalRefiner local(width, true);
      Images prior, saved;
      for (int frame = 0; frame < 6; ++frame) {
        Images images;
        rng.fill(images[0] = Mat(480, 640, CV_8U), RNG::UNIFORM, 0, 255);
        GaussianBlur(images[0], images[0], Size(5, 5), 0);
        Mat move = (Mat_<double>(2, 3) << 1, 0, frame * 3 + 5, 0, 1, -4);
        warpAffine(images[0], images[1], move, Size(640, 480));
        images[2] = frame == 3 ? Mat::zeros(480, 640, CV_8U) : images[1].clone();
        auto actual = local.refine(images);
        auto expected = reference(images, width, frame % 5 == 0);
        if (actual.checked != (frame % 5 == 0) ||
            norm(actual.validity, expected.validity, NORM_INF) != 0)
          throw std::runtime_error("Validity/cadence mismatch");
        for (int i = 0; i < 3; ++i) {
          if (norm(actual.channels[i], expected.channels[i], NORM_INF) != 0)
            throw std::runtime_error("Output differs from CPU baseline");
          if (!prior[i].empty() && norm(prior[i], saved[i], NORM_INF) != 0)
            throw std::runtime_error("Scratch overwrote a previously returned image");
          prior[i] = actual.channels[i];
          saved[i] = prior[i].clone();
        }
      }
    }
    std::cout << "PASS: exact CPU parity at all widths, checks, blank input, output ownership\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
