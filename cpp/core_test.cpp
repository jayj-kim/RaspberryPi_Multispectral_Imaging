#include "core.hpp"
#include <iostream>
static void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
int main() {
  using namespace imaging;
  using namespace cv;
  try {
    setNumThreads(2);
    setRNGSeed(0);
    json ref = {{80, 60}, {560, 60}, {560, 420}, {80, 420}},
         src = {{100, 80}, {510, 50}, {550, 390}, {65, 400}};
    auto h = registration(json::array({ref, src, json::array()}));
    std::vector<Point2f> a, b;
    for (auto &p : src)
      a.emplace_back(p[0], p[1]);
    perspectiveTransform(a, b, h[1]);
    for (int i = 0; i < 4; i++)
      check(norm(b[i] - Point2f(ref[i][0], ref[i][1])) < .001, "Wrong perspective mapping");
    bool rejected = false;
    try {
      registration(json::array({ref, json{{10, 10}, {20, 20}, {30, 30}, {40, 40}}, json::array()}));
    } catch (const std::exception &) {
      rejected = true;
    }
    check(rejected, "Collinear points accepted");
    std::vector<Matrices> path;
    for (int n = 0; n < 100; n++) {
      auto row = identities();
      row[1].at<double>(0, 2) = n * .2 + (n % 2 ? 2 : -2);
      if (n == 50)
        row[1].at<double>(0, 2) += 20;
      row[2].at<double>(1, 2) = n * .1;
      path.push_back(row);
    }
    auto smoothed = smoothPath(path, 15);
    check(std::abs(smoothed[50][1].at<double>(0, 2) - 10) < 1, "Smoothing did not reject spike");
    for (int n = 20; n < 80; n++)
      check(std::abs(smoothed[n][2].at<double>(1, 2) - n * .1) < .001,
            "Centered filter lagged linear path");
    Profile profile;
    for (int i = 0; i < 5; i++)
      profile.record({{"total_ms", 999}, {"global_ms", 999}}, true, false);
    profile.record({{"total_ms", 10}, {"global_ms", 0}}, false, false);
    profile.record({{"total_ms", 30}, {"global_ms", 20}}, true, true);
    auto p = profile.summary();
    check(p["measured_frames"] == 2, "Warmup count wrong");
    check(p["stages"]["global_ms"]["mean_ms"] == 10, "Mean wrong");
    check(p["stages"]["global_ms"]["active_mean_ms"] == 20, "Active mean wrong");
    Mat texture(480, 640, CV_8U);
    RNG rng(92);
    rng.fill(texture, RNG::UNIFORM, 20, 230);
    GaussianBlur(texture, texture, Size(5, 5), 0);
    Mat coords(480, 640, CV_32FC2);
    for (int y = 0; y < 480; y++)
      for (int x = 0; x < 640; x++)
        coords.at<Vec2f>(y, x) = Vec2f(x + (x < 320 ? 8 : -6), y);
    Mat source;
    remap(texture, source, coords, noArray(), INTER_LINEAR);
    LocalRefiner local;
    auto r = local.refine({texture, source, source});
    double before = 0, after = 0;
    int count = 0;
    for (int y = 40; y < 440; y++)
      for (int x = 40; x < 600; x++)
        if (x < 280 || x >= 360) {
          before += std::abs(int(texture.at<uchar>(y, x)) - source.at<uchar>(y, x));
          after += std::abs(int(texture.at<uchar>(y, x)) - r.channels[1].at<uchar>(y, x));
          count++;
        }
    check(after < before * .2, "Local two-depth correction failed");
    check(norm(texture, r.channels[0], NORM_INF) == 0, "Reference image changed");
    Images blank = {Mat::zeros(480, 640, CV_8U), Mat::zeros(480, 640, CV_8U),
                    Mat::zeros(480, 640, CV_8U)};
    check(countNonZero(local.refine(blank).validity) == 0, "Blank pixels marked consistent");
    auto estimate = automatic({texture, texture, texture});
    check(estimate.reports.size() == 2, "Automatic registration failed");
    FlowTracker tracker;
    tracker.estimate({texture, texture, texture});
    Mat shifted;
    Mat movement = (Mat_<double>(2, 3) << 1, 0, 2, 0, 1, 1);
    warpAffine(texture, shifted, movement, Size(640, 480));
    auto tracked = tracker.estimate({shifted, shifted, shifted});
    check(tracked.method == "optical_flow", "Temporal feature tracking did not run");
    GlobalStabilizer global;
    std::vector<double> rawMotion, stabilizedMotion;
    for (int n = 0; n < 30; n++) {
      double dx = n % 2 ? 3 : -3;
      Mat transform = (Mat_<double>(2, 3) << 1, 0, dx, 0, 1, 0), frame;
      warpAffine(texture, frame, transform, Size(640, 480));
      auto result = global.update(frame);
      if (n > 5) {
        rawMotion.push_back(dx);
        stabilizedMotion.push_back(dx + result.first.x);
      }
    }
    auto deviation = [](const std::vector<double> &v) {
      double mean = 0;
      for (double x : v)
        mean += x;
      mean /= v.size();
      double sum = 0;
      for (double x : v)
        sum += (x - mean) * (x - mean);
      return std::sqrt(sum / v.size());
    };
    check(deviation(stabilizedMotion) < deviation(rawMotion) * .4,
          "Global translation stabilization failed");
    Temporal temporal(identities(), "strong");
    temporal.update(identities());
    auto jump = identities();
    jump[1].at<double>(0, 2) = 80;
    check(temporal.update(jump).second == "held_jump", "One-frame outlier was accepted");
    temporal.update(jump);
    check(temporal.update(jump).second == "reacquired", "Sustained jump was never reacquired");
    LocalRefiner fast(320, true);
    auto f = fast.refine({texture, source, source});
    check(f.checked, "First live check missing");
    check(!fast.refine({texture, source, source}).checked, "Live checks not amortized");
    Mat color;
    cvtColor(texture, color, COLOR_GRAY2BGR);
    LivePipeline pipeline({{"cpu_remap", "fixed"}, {"flow_width", 320}});
    auto output = pipeline.process({color, color, color}, 0);
    double topLevel = 0;
    for (auto it = output.stages.begin(); it != output.stages.end(); ++it)
      if (it.key() != "total_ms" && it.key().rfind("cpu_", 0) != 0 &&
          it.key().rfind("gpu_", 0) != 0)
        topLevel += it.value().get<double>();
    check(std::abs(topLevel - output.stages["total_ms"].get<double>()) < .001,
          "Nested CPU timing counted twice in total/other");
    std::cout << "PASS: perspective, invalid points, centered smoothing, profile, dense two-depth "
                 "registration, blank validity, ORB, temporal tracking, global stabilization, jump "
                 "rejection, reduced live flow\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
