#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <set>
#include <stdexcept>
namespace imaging {
using namespace cv;
static void require(bool x, const std::string &s) {
  if (!x)
    throw std::runtime_error(s);
}
json fallback(const json &j, const std::string &k, const json &v) {
  return j.contains(k) && !j[k].is_null() ? j[k] : v;
}
void writeJson(const fs::path &p, const json &j) {
  std::ofstream f(p);
  require(bool(f), "Cannot write " + p.string());
  f << j.dump(2);
  require(bool(f), "Write failed");
}
json readJson(const fs::path &p) {
  std::ifstream f(p);
  require(bool(f), "Cannot read " + p.string());
  json j;
  f >> j;
  return j;
}
Mat matrix(const json &j) {
  require(j.is_array() && j.size() == 3, "Expected 3x3 matrix");
  Mat h(3, 3, CV_64F);
  for (int y = 0; y < 3; y++) {
    require(j[y].size() == 3, "Expected 3x3 matrix");
    for (int x = 0; x < 3; x++)
      h.at<double>(y, x) = j[y][x].get<double>();
  }
  require(checkRange(h), "Nonfinite matrix");
  return h;
}
json toJson(const Mat &m) {
  Mat h;
  m.convertTo(h, CV_64F);
  json j = json::array();
  for (int y = 0; y < h.rows; y++) {
    json r = json::array();
    for (int x = 0; x < h.cols; x++)
      r.push_back(h.at<double>(y, x));
    j.push_back(r);
  }
  return j;
}
json toJson(const Matrices &h) {
  json j = json::array();
  for (auto &m : h)
    j.push_back(toJson(m));
  return j;
}
Matrices identities() {
  return {Mat::eye(3, 3, CV_64F), Mat::eye(3, 3, CV_64F), Mat::eye(3, 3, CV_64F)};
}
Matrices readMatrices(const json &j) {
  require(j.size() == 3, "Three matrices required");
  return {matrix(j[0]), matrix(j[1]), matrix(j[2])};
}
static std::vector<Point2f> corners() {
  return {{0, 0}, {639, 0}, {639, 479}, {0, 479}};
}
static std::vector<Point2f> project(const Mat &h, const std::vector<Point2f> &p = corners()) {
  std::vector<Point2f> r;
  perspectiveTransform(p, r, h);
  return r;
}
static bool validH(const Mat &h) {
  if (h.empty() || !checkRange(h) || std::abs(determinant(h)) < 1e-10)
    return false;
  double lo = 1e99, hi = -1e99;
  for (auto p : corners()) {
    double d = h.at<double>(2, 0) * p.x + h.at<double>(2, 1) * p.y + h.at<double>(2, 2);
    lo = std::min(lo, d);
    hi = std::max(hi, d);
  }
  return !(lo <= 0 && hi >= 0);
}
static double median(std::vector<double> a) {
  require(!a.empty(), "Empty median");
  std::sort(a.begin(), a.end());
  int n = a.size();
  return n % 2 ? a[n / 2] : (a[n / 2 - 1] + a[n / 2]) / 2;
}
static json pointJson(const std::vector<Point2f> &p) {
  json j = json::array();
  for (auto v : p)
    j.push_back({v.x, v.y});
  return j;
}
static std::vector<Point2f> readPoints(const json &j) {
  require(j.is_array(), "Invalid points");
  std::vector<Point2f> p;
  for (auto &q : j) {
    require(q.is_array() && q.size() == 2, "Invalid point");
    double x = q[0], y = q[1];
    require(std::isfinite(x) && std::isfinite(y) && x >= 0 && x < 640 && y >= 0 && y < 480,
            "Point outside image");
    p.emplace_back(x, y);
  }
  return p;
}
Matrices registration(const json &points, const json &adjustments) {
  json p = points.is_null() ? json::array({json::array(), json::array(), json::array()}) : points;
  json a = adjustments.is_null() ? json{{0, 1}, {0, 1}, {0, 1}} : adjustments;
  require(p.size() == 3 && a.size() == 3, "Three camera settings required");
  std::array<std::vector<Point2f>, 3> pts;
  for (int i = 0; i < 3; i++) {
    pts[i] = readPoints(p[i]);
    if (!pts[i].empty()) {
      require(pts[i].size() == 4, "Select four corresponding points");
      for (int omit = 0; omit < 4; omit++) {
        std::vector<Point2f> t;
        for (int k = 0; k < 4; k++)
          if (k != omit)
            t.push_back(pts[i][k]);
        require(std::abs((t[1] - t[0]).cross(t[2] - t[0])) >= 20, "Collinear/duplicate points");
      }
    }
  }
  auto result = identities();
  for (int i = 0; i < 3; i++) {
    if (i && !pts[i].empty()) {
      require(pts[0].size() == 4, "cam1 reference points required");
      result[i] = getPerspectiveTransform(pts[i], pts[0]);
      require(validH(result[i]), "Unstable homography");
      SVD svd(result[i]);
      require(svd.w.at<double>(0) / svd.w.at<double>(2) < 1e9, "Ill-conditioned homography");
    }
    require(a[i].size() == 2, "Rotation and scale required");
    double angle = a[i][0], scale = a[i][1];
    require(std::isfinite(angle) && std::abs(angle) <= 180 && std::isfinite(scale) && scale >= .2 &&
                scale <= 5,
            "Invalid rotation/scale");
    Mat fine = Mat::eye(3, 3, CV_64F);
    getRotationMatrix2D(Point2f(320, 240), angle, scale).copyTo(fine(Rect(0, 0, 3, 2)));
    result[i] = fine * result[i];
  }
  return result;
}
Mat warp(const Mat &g, const Mat &h, const json &o) {
  require(o.size() == 2, "Invalid offset");
  double x = o[0], y = o[1];
  require(std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 640 && std::abs(y) <= 640,
          "Invalid offset");
  // The reference camera is normally unchanged. Avoid a full perspective resample,
  // but retain independent output storage just like warpPerspective.
  if (g.size() == Size(640, 480) && x == 0 && y == 0 &&
      norm(h, Mat::eye(3, 3, h.type()), NORM_INF) == 0)
    return g.clone();
  Mat t = (Mat_<double>(3, 3) << 1, 0, x, 0, 1, y, 0, 0, 1), out;
  warpPerspective(g, out, t * h, Size(640, 480));
  return out;
}
static std::tuple<Mat, json, TrackPair> fit(const std::vector<Point2f> &src,
                                            const std::vector<Point2f> &dst, int cam) {
  require(src.size() >= 20, "Not enough matches cam" + std::to_string(cam));
  Mat mask;
  Mat h = findHomography(src, dst, RANSAC, 3, mask);
  require(!h.empty(), "Homography fit failed");
  TrackPair pair;
  for (size_t i = 0; i < src.size(); i++)
    if (mask.at<uchar>(i)) {
      pair.source.push_back(src[i]);
      pair.target.push_back(dst[i]);
    }
  double ratio = double(pair.source.size()) / src.size();
  require(pair.source.size() >= 15 && ratio >= .45, "Low inlier confidence");
  auto projected = project(h, pair.source);
  std::vector<double> errors;
  for (size_t i = 0; i < projected.size(); i++)
    errors.push_back(norm(projected[i] - pair.target[i]));
  std::vector<Point2f> hs, ht;
  convexHull(pair.source, hs);
  convexHull(pair.target, ht);
  double coverage = std::min(contourArea(hs), contourArea(ht)) / (640. * 480);
  double error = median(errors);
  require(checkRange(h) && error <= 2.5 && coverage >= .03,
          "Large error or narrow feature coverage");
  json report = {
      {"camera", cam},         {"matches", src.size()},    {"inliers", pair.source.size()},
      {"inlier_ratio", ratio}, {"median_error_px", error}, {"coverage_percent", coverage * 100}};
  return {h, report, pair};
}
static void encode(Estimate &r) {
  for (int margin : {60, 90, 120, 150}) {
    std::vector<Point2f> ref = {{float(margin), float(margin)},
                                {float(639 - margin), float(margin)},
                                {float(639 - margin), float(479 - margin)},
                                {float(margin), float(479 - margin)}};
    json pts = json::array({pointJson(ref)});
    try {
      for (int i = 1; i < 3; i++)
        pts.push_back(pointJson(project(r.h[i].inv(), ref)));
      registration(pts);
      r.points = pts;
      return;
    } catch (const std::exception &) {
    }
  }
  throw std::runtime_error("Insufficient common registration region");
}
Estimate automatic(const Images &g) {
  auto orb = ORB::create(3000, 1.2f, 8, 31, 0, 2, ORB::HARRIS_SCORE, 31, 12);
  std::vector<KeyPoint> ref;
  Mat rd;
  orb->detectAndCompute(g[0], noArray(), ref, rd);
  require(!rd.empty() && ref.size() >= 25, "cam1 has insufficient features");
  Estimate result;
  result.h = identities();
  result.reports = json::array();
  for (int i = 1; i < 3; i++) {
    std::vector<KeyPoint> kp;
    Mat d;
    orb->detectAndCompute(g[i], noArray(), kp, d);
    require(!d.empty() && kp.size() >= 25, "Insufficient camera features");
    std::vector<std::vector<DMatch>> pairs;
    BFMatcher(NORM_HAMMING).knnMatch(d, rd, pairs, 2);
    std::vector<DMatch> good;
    for (auto &p : pairs)
      if (p.size() == 2 && p[0].distance < .75 * p[1].distance)
        good.push_back(p[0]);
    std::stable_sort(good.begin(), good.end(),
                     [](auto a, auto b) { return a.distance < b.distance; });
    std::set<int> seen;
    std::vector<Point2f> src, dst;
    for (auto &m : good)
      if (seen.insert(m.trainIdx).second) {
        src.push_back(kp[m.queryIdx].pt);
        dst.push_back(ref[m.trainIdx].pt);
      }
    auto [h, report, track] = fit(src, dst, i + 1);
    result.h[i] = h;
    result.reports.push_back(report);
    result.tracks[i - 1] = track;
  }
  encode(result);
  return result;
}
static std::pair<std::vector<Point2f>, std::vector<uchar>> track(const Mat &prev, const Mat &cur,
                                                                 const std::vector<Point2f> &p) {
  require(!p.empty(), "Empty tracks");
  std::vector<Point2f> q, back;
  std::vector<uchar> f, b;
  std::vector<float> err;
  calcOpticalFlowPyrLK(prev, cur, p, q, f, err, Size(21, 21), 3);
  require(!q.empty(), "Tracking failed");
  calcOpticalFlowPyrLK(cur, prev, q, back, b, err, Size(21, 21), 3);
  for (size_t i = 0; i < p.size(); i++)
    f[i] = f[i] && b[i] && norm(back[i] - p[i]) < 1 && q[i].x >= 0 && q[i].x < 640 && q[i].y >= 0 &&
           q[i].y < 480;
  return {q, f};
}
Estimate FlowTracker::estimate(const Images &g) {
  Estimate r;
  bool success = false;
  if (ready && age < 30) {
    try {
      r.h = identities();
      r.reports = json::array();
      for (int i = 1; i < 3; i++) {
        auto [a, va] = track(previous[i], g[i], tracks[i - 1].source);
        auto [b, vb] = track(previous[0], g[0], tracks[i - 1].target);
        std::vector<Point2f> src, dst;
        for (size_t k = 0; k < a.size(); k++)
          if (va[k] && vb[k]) {
            src.push_back(a[k]);
            dst.push_back(b[k]);
          }
        auto [h, report, tp] = fit(src, dst, i + 1);
        r.h[i] = h;
        r.reports.push_back(report);
        r.tracks[i - 1] = tp;
      }
      encode(r);
      r.method = "optical_flow";
      age++;
      success = true;
    } catch (const std::exception &) {
    }
  }
  if (!success) {
    ready = false;
    r = automatic(g);
    r.method = "orb_reacquire";
    age = 0;
  }
  for (int i = 0; i < 3; i++)
    previous[i] = g[i].clone();
  tracks = r.tracks;
  ready = true;
  return r;
}
Temporal::Temporal(const Matrices &h, const std::string &mode) : last(h) {
  if (mode == "strong") {
    alpha = .15;
    deadband = 1;
  } else if (mode == "balanced") {
    alpha = .3;
    deadband = .7;
  } else if (mode == "responsive") {
    alpha = .6;
    deadband = .3;
  } else
    throw std::runtime_error("Invalid stabilization mode");
}
void Temporal::miss() {
  pendingCount = 0;
}
std::pair<Matrices, std::string> Temporal::update(const Matrices &candidate) {
  if (!ready) {
    last = candidate;
    ready = true;
    return {last, "initialized"};
  }
  std::array<std::vector<Point2f>, 2> cur, prev;
  double shift[2] = {0, 0};
  for (int j = 0; j < 2; j++) {
    cur[j] = project(candidate[j + 1]);
    prev[j] = project(last[j + 1]);
    for (int k = 0; k < 4; k++)
      shift[j] = std::max(shift[j], norm(cur[j][k] - prev[j][k]));
  }
  if (std::max(shift[0], shift[1]) > 30) {
    double diff = 0;
    if (pendingCount)
      for (int j = 0; j < 2; j++)
        for (int k = 0; k < 4; k++)
          diff = std::max(diff, norm(cur[j][k] - pending[j][k]));
    bool consistent = pendingCount && diff < 8;
    pendingCount = consistent ? pendingCount + 1 : 1;
    pending = cur;
    if (pendingCount < 3)
      return {last, "held_jump"};
    last = candidate;
    miss();
    return {last, "reacquired"};
  }
  miss();
  Matrices out = last;
  out[0] = candidate[0];
  bool changed = false;
  for (int j = 0; j < 2; j++)
    if (shift[j] > deadband) {
      changed = true;
      std::vector<Point2f> dst;
      for (int k = 0; k < 4; k++)
        dst.push_back(cur[j][k] * alpha + prev[j][k] * (1 - alpha));
      out[j + 1] = getPerspectiveTransform(corners(), dst);
      require(validH(out[j + 1]), "Invalid smoothed matrix");
    }
  last = out;
  return {last, changed ? "smoothed" : "held_small"};
}
std::pair<Point2d, json> GlobalStabilizer::update(const Mat &g) {
  std::string state = "initialized", reason;
  try {
    if (!previous.empty()) {
      std::vector<Point2f> p;
      goodFeaturesToTrack(previous, p, 300, .01, 12);
      require(p.size() >= 20, "Insufficient global features");
      auto [q, v] = track(previous, g, p);
      std::vector<Point2f> a, b;
      for (size_t i = 0; i < p.size(); i++)
        if (v[i]) {
          a.push_back(p[i]);
          b.push_back(q[i]);
        }
      require(a.size() >= 20, "Insufficient global tracks");
      Mat mask;
      auto affine = estimateAffinePartial2D(a, b, mask, RANSAC, 2);
      require(!affine.empty() && countNonZero(mask) >= 20 &&
                  double(countNonZero(mask)) / a.size() >= .5,
              "Global motion fit failed");
      std::vector<Point2f> chosen;
      std::vector<double> dx, dy;
      for (size_t i = 0; i < a.size(); i++)
        if (mask.at<uchar>(i)) {
          chosen.push_back(a[i]);
          dx.push_back(b[i].x - a[i].x);
          dy.push_back(b[i].y - a[i].y);
        }
      std::vector<Point2f> hull;
      convexHull(chosen, hull);
      require(contourArea(hull) / (640. * 480) >= .08, "Narrow global features");
      Point2d d(median(dx), median(dy));
      require(norm(d) <= 40, "Large global motion");
      path += d;
      smooth = .12 * path + .88 * smooth;
      correction = smooth - path;
      correction.x = std::clamp(correction.x, -50., 50.);
      correction.y = std::clamp(correction.y, -50., 50.);
      state = "stabilized";
    }
  } catch (const std::exception &e) {
    state = "held";
    reason = e.what();
  }
  previous = g.clone();
  return {correction,
          {{"state", state},
           {"reason", reason.empty() ? json(nullptr) : json(reason)},
           {"translation_px", {correction.x, correction.y}}}};
}
LocalRefiner::LocalRefiner(int w, bool l, const std::string &backend, bool allowSoftware,
                           const std::string &cpuRemap)
    : width(w), live(l), fixedRemap(cpuRemap == "fixed") {
  require(cpuRemap == "float" || cpuRemap == "fixed", "Invalid CPU remap mode");
  require(w == 320 || w == 480 || w == 640, "Invalid flow width");
  engine = DISOpticalFlow::create(l ? DISOpticalFlow::PRESET_FAST : DISOpticalFlow::PRESET_MEDIUM);
  clahe = createCLAHE(2, Size(8, 8));
  require(backend == "cpu" || backend == "vulkan" || backend == "auto", "Invalid compute backend");
  backendInfo = {
      {"requested", backend}, {"actual", "cpu"}, {"cpu_remap", cpuRemap}, {"warning", nullptr}};
  if (backend != "cpu") {
    require(live, "Vulkan currently supports the realtime path only");
    try {
      gpu = std::make_unique<VulkanRemapper>(width, allowSoftware);
      backendInfo["actual"] = "vulkan";
      backendInfo["device"] = gpu->info();
    } catch (const std::exception &e) {
      backendInfo["warning"] = std::string("Vulkan unavailable; CPU fallback: ") + e.what();
    }
  }
}
json LocalRefiner::backendStatus() const {
  return backendInfo;
}

Refined LocalRefiner::refine(const Images &g) {
  Refined r;
  r.channels[0] = g[0];
  r.checked = !live || count % 5 == 0;
  r.reports = json::array();
  r.timings = {{"local_prep_ms", 0.},
               {"dis_forward_ms", 0.},
               {"dis_reverse_ms", 0.},
               {"local_warp_ms", 0.},
               {"consistency_ms", 0.}};
  for (const std::string key :
       {"gpu_pack_ms", "gpu_upload_ms", "gpu_submit_wait_ms", "gpu_download_ms", "gpu_kernel_ms"})
    r.timings[key] = 0.;
  for (const std::string key : {"cpu_flow_resize_ms", "cpu_coords_ms", "cpu_map_convert_ms",
                                "cpu_image_remap_ms", "cpu_restore_ms", "cpu_reverse_remap_ms"})
    r.timings[key] = 0.;
  auto addTime = [&](const char *key, double start) {
    r.timings[key] = r.timings[key].get<double>() + ms(start);
  };
  auto &small = smallBuffers;
  double tick = now();
  for (int i = 0; i < 3; i++) {
    Mat &a = resizeBuffers[i];
    if (width == 640)
      a = g[i];
    else
      resize(g[i], a, Size(width, width * 3 / 4));
    clahe->apply(a, small[i]);
  }
  r.timings["local_prep_ms"] = ms(tick);
  if (gpu) {
    try {
      std::array<Mat, 2> forward, backward;
      for (int i = 0; i < 2; i++) {
        tick = now();
        engine->calc(small[0], small[i + 1], forward[i]);
        r.timings["dis_forward_ms"] = r.timings["dis_forward_ms"].get<double>() + ms(tick);
        if (r.checked) {
          tick = now();
          engine->calc(small[i + 1], small[0], backward[i]);
          r.timings["dis_reverse_ms"] = r.timings["dis_reverse_ms"].get<double>() + ms(tick);
        }
      }
      tick = now();
      auto result = gpu->apply(g, forward, backward, r.checked);
      r.timings["local_warp_ms"] = ms(tick);
      r.channels = result.channels;
      r.validity = result.validity;
      for (auto it = result.timings.begin(); it != result.timings.end(); ++it)
        r.timings[it.key()] = it.value();
      if (r.checked)
        lastConsistency = double(countNonZero(r.validity)) / (640 * 480);
      r.consistency = lastConsistency;
      count++;
      return r;
    } catch (const std::exception &e) {
      gpu.reset();
      if (backendInfo.contains("device")) {
        backendInfo["last_failed_device"] = backendInfo["device"];
        backendInfo.erase("device");
      }
      backendInfo["actual"] = "cpu";
      backendInfo["warning"] = std::string("Vulkan dispatch failed; CPU fallback: ") + e.what();
      // Recompute this frame on CPU; do not publish a partial GPU result.
    }
  }
  Mat validAll(480, 640, CV_8U, Scalar(255));
  r.reports = json::array();
  for (int cam = 1; cam < 3; cam++) {
    Mat low, mapped;
    Mat &flow = flowBuffer, &coords = coordsBuffer, &invalid = invalidBuffer;
    coords.create(480, 640, CV_32FC2);
    invalid.create(480, 640, CV_8U);
    tick = now();
    engine->calc(small[0], small[cam], low);
    r.timings["dis_forward_ms"] = r.timings["dis_forward_ms"].get<double>() + ms(tick);
    tick = now();
    double subTick = now();
    if (width == 640)
      flow = low;
    else {
      resize(low, flow, Size(640, 480));
    }
    addTime("cpu_flow_resize_ms", subTick);
    subTick = now();
    parallel_for_(Range(0, 480), [&](const Range &range) {
      for (int y = range.start; y < range.end; y++) {
        auto *f = flow.ptr<Vec2f>(y);
        auto *c = coords.ptr<Vec2f>(y);
        auto *bad = invalid.ptr<uchar>(y);
        for (int x = 0; x < 640; x++) {
          if (width != 640)
            f[x] *= 640.f / width;
          c[x] = f[x] + Vec2f(float(x), float(y));
          bad[x] = !(std::isfinite(c[x][0]) && std::isfinite(c[x][1]) && f[x].dot(f[x]) < 3600)
                       ? 255
                       : 0;
        }
      }
    });
    addTime("cpu_coords_ms", subTick);
    if (fixedRemap) {
      subTick = now();
      convertMaps(coords, noArray(), fixedCoords, fractions, CV_16SC2);
      addTime("cpu_map_convert_ms", subTick);
    }
    subTick = now();
    if (fixedRemap)
      remap(g[cam], mapped, fixedCoords, fractions, INTER_LINEAR);
    else
      remap(g[cam], mapped, coords, noArray(), INTER_LINEAR);
    addTime("cpu_image_remap_ms", subTick);
    subTick = now();
    // OpenCV's masked copy avoids another scalar per-pixel fallback pass.
    g[cam].copyTo(mapped, invalid);
    addTime("cpu_restore_ms", subTick);
    r.channels[cam] = mapped;
    r.timings["local_warp_ms"] = r.timings["local_warp_ms"].get<double>() + ms(tick);
    if (r.checked) {
      Mat lowBack;
      Mat &back = backBuffer, &reverse = reverseBuffer;
      tick = now();
      engine->calc(small[cam], small[0], lowBack);
      r.timings["dis_reverse_ms"] = r.timings["dis_reverse_ms"].get<double>() + ms(tick);
      tick = now();
      if (width == 640)
        back = lowBack;
      else {
        resize(lowBack, back, Size(640, 480));
        back *= 640.f / width;
      }
      subTick = now();
      if (fixedRemap)
        remap(back, reverse, fixedCoords, fractions, INTER_LINEAR, BORDER_CONSTANT,
              Scalar(1000, 1000));
      else
        remap(back, reverse, coords, noArray(), INTER_LINEAR, BORDER_CONSTANT, Scalar(1000, 1000));
      addTime("cpu_reverse_remap_ms", subTick);
      Mat valid;
      if (!live)
        valid.create(480, 640, CV_8U);
      std::vector<double> err, mag;
      if (!live) {
        err.reserve(640 * 480);
        mag.reserve(640 * 480);
      }
      parallel_for_(Range(0, 480), [&](const Range &range) {
        for (int y = range.start; y < range.end; y++) {
          auto *f = flow.ptr<Vec2f>(y);
          auto *b = reverse.ptr<Vec2f>(y);
          auto *c = coords.ptr<Vec2f>(y);
          auto *bad = invalid.ptr<uchar>(y);
          auto *all = validAll.ptr<uchar>(y);
          auto *v = live ? nullptr : valid.ptr<uchar>(y);
          auto *ref = g[0].ptr<uchar>(y);
          auto *src = mapped.ptr<uchar>(y);
          for (int x = 0; x < 640; x++) {
            Vec2f roundtrip = f[x] + b[x];
            bool good = !bad[x] && roundtrip.dot(roundtrip) < 2.25 && c[x][0] >= 0 &&
                        c[x][0] < 639 && c[x][1] >= 0 && c[x][1] < 479 && ref[x] > 2 && src[x] > 2;
            if (v)
              v[x] = good ? 255 : 0;
            all[x] &= good ? 255 : 0;
          }
        }
      });
      if (!live) {
        for (int y = 0; y < 480; y++) {
          auto *f = flow.ptr<Vec2f>(y);
          auto *b = reverse.ptr<Vec2f>(y);
          for (int x = 0; x < 640; x++) {
            err.push_back(norm(f[x] + b[x]));
            mag.push_back(norm(f[x]));
          }
        }
        r.reports.push_back({{"camera", cam + 1},
                             {"consistent_fraction", double(countNonZero(valid)) / (640 * 480)},
                             {"median_roundtrip_error_px", median(err)},
                             {"median_residual_flow_px", median(mag)}});
      }
      r.timings["consistency_ms"] = r.timings["consistency_ms"].get<double>() + ms(tick);
    }
  }
  if (r.checked)
    lastConsistency = double(countNonZero(validAll)) / (640 * 480);
  r.consistency = lastConsistency;
  r.validity = validAll;
  count++;
  return r;
}
std::vector<Matrices> smoothPath(const std::vector<Matrices> &input, int window) {
  require(window == 15 || window == 31 || window == 61, "Invalid temporal window");
  require(!input.empty(), "Empty path");
  int n = input.size(), radius = window / 2;
  using Row = std::array<std::array<double, 8>, 2>;
  std::vector<Row> a(n), clean(n), filtered(n);
  for (int i = 0; i < n; i++)
    for (int cam = 0; cam < 2; cam++) {
      auto p = project(input[i][cam + 1]);
      for (int k = 0; k < 4; k++) {
        a[i][cam][2 * k] = p[k].x;
        a[i][cam][2 * k + 1] = p[k].y;
      }
    }
  for (int i = 0; i < n; i++)
    for (int cam = 0; cam < 2; cam++)
      for (int k = 0; k < 8; k++) {
        std::vector<double> v, d;
        for (int j = -2; j <= 2; j++)
          v.push_back(a[std::clamp(i + j, 0, n - 1)][cam][k]);
        double med = median(v);
        for (double x : v)
          d.push_back(std::abs(x - med));
        double threshold = std::max(6., 3 * 1.4826 * median(d));
        clean[i][cam][k] = std::abs(a[i][cam][k] - med) > threshold ? med : a[i][cam][k];
      }
  std::vector<double> w;
  double sum = 0;
  for (int j = -radius; j <= radius; j++) {
    double v = std::exp(-.5 * std::pow(j / (window / 5.), 2));
    w.push_back(v);
    sum += v;
  }
  for (double &v : w)
    v /= sum;
  for (int i = 0; i < n; i++)
    for (int cam = 0; cam < 2; cam++)
      for (int k = 0; k < 8; k++) {
        double v = 0;
        for (int j = -radius; j <= radius; j++)
          v += w[j + radius] * clean[std::clamp(i + j, 0, n - 1)][cam][k];
        filtered[i][cam][k] = v;
      }
  auto out = input;
  for (int i = 0; i < n; i++)
    for (int cam = 0; cam < 2; cam++) {
      std::vector<Point2f> p;
      for (int k = 0; k < 4; k++)
        p.emplace_back(filtered[i][cam][2 * k], filtered[i][cam][2 * k + 1]);
      Mat h = getPerspectiveTransform(corners(), p);
      if (validH(h))
        out[i][cam + 1] = h;
    }
  return out;
}
Profile::Profile(json c) : config(c) {}
void Profile::record(const json &stage, bool refresh, bool check, bool recording) {
  seen++;
  if (seen <= 5)
    return;
  count++;
  recordingCount += recording;
  samples.push_back(stage);
  if (samples.size() > 300)
    samples.pop_front();
  for (auto it = stage.begin(); it != stage.end(); ++it) {
    double v = it.value();
    auto &x = totals[it.key()];
    if (x.is_null())
      x = {{"sum", 0.}, {"max", 0.}, {"active", 0}};
    x["sum"] = x["sum"].get<double>() + v;
    x["max"] = std::max(x["max"].get<double>(), v);
    x["active"] = x["active"].get<int>() + (v > 0);
  }
  std::string name = refresh ? (check ? "global+check" : "global") : (check ? "check" : "ordinary");
  if (recording)
    name += "+recording";
  auto &g = groups[name];
  if (g.is_null())
    g = {{"count", 0}, {"sum", 0.}, {"max", 0.}};
  g["count"] = g["count"].get<int>() + 1;
  g["sum"] = g["sum"].get<double>() + stage["total_ms"].get<double>();
  g["max"] = std::max(g["max"].get<double>(), stage["total_ms"].get<double>());
}
void Profile::setBackend(const json &info) {
  config["local_backend"] = info;
}
static double percentile(std::vector<double> v, double p) {
  if (v.empty())
    return 0;
  std::sort(v.begin(), v.end());
  double index = (v.size() - 1) * p;
  size_t lo = std::floor(index), hi = std::ceil(index);
  return v[lo] + (v[hi] - v[lo]) * (index - lo);
}
json Profile::summary() const {
  json s = json::object(), g = json::object();
  for (auto it = totals.begin(); it != totals.end(); ++it) {
    std::vector<double> recent, active;
    for (auto &r : samples) {
      double v = r[it.key()];
      recent.push_back(v);
      if (v > 0)
        active.push_back(v);
    }
    const auto &x = it.value();
    int n = x["active"];
    s[it.key()] = {
        {"mean_ms", x["sum"].get<double>() / count},
        {"max_ms", x["max"]},
        {"active_count", n},
        {"active_mean_ms", x["sum"].get<double>() / std::max(1, n)},
        {"p95_recent_ms", percentile(recent, .95)},
        {"active_p95_recent_ms", active.empty() ? json(nullptr) : json(percentile(active, .95))}};
  }
  for (auto it = groups.begin(); it != groups.end(); ++it)
    g[it.key()] = {{"count", it.value()["count"]},
                   {"mean_ms", it.value()["sum"].get<double>() / it.value()["count"].get<int>()},
                   {"max_ms", it.value()["max"]}};
  return {{"config", config},
          {"outputs_seen", seen},
          {"warmup_excluded", std::min(5, seen)},
          {"measured_frames", count},
          {"recording_frames", recordingCount},
          {"percentile_window_frames", samples.size()},
          {"stages", s},
          {"groups", g},
          {"notes",
           {"Wall-clock timings; first five outputs excluded.",
            "Percentiles use latest 300 outputs; means/max cover full measured run.",
            "Camera exposure, target-rate waits, network and browser excluded.",
            "cpu_* and gpu_* are nested timings already included in parent stages; do not add "
            "twice."}}};
}
LivePipeline::LivePipeline(json c)
    : h(registration(fallback(c, "points", nullptr), fallback(c, "adjustments", nullptr))),
      config(c), temporal(h, "responsive"),
      local(c.value("flow_width", 320), true, c.value("compute_backend", std::string("cpu")),
            c.value("allow_software_vulkan", false), c.value("cpu_remap", std::string("float"))) {}
PipelineResult LivePipeline::process(const Images &bgr, double inputTime) {
  double begin = now(), tick = now();
  PipelineResult r;
  Images gray;
  for (int i = 0; i < 3; i++)
    cvtColor(bgr[i], gray[i], COLOR_BGR2GRAY);
  r.stages = {{"frame_pick_ms", 0.}, {"grayscale_ms", ms(tick)}, {"global_ms", 0.}};
  bool refresh = inputTime - lastRefresh >= 1;
  if (refresh) {
    lastRefresh = inputTime;
    tick = now();
    try {
      auto estimate = automatic(gray);
      auto candidate = registration(estimate.points, fallback(config, "adjustments", nullptr));
      h = temporal.update(candidate).first;
      globalValid = true;
    } catch (const std::exception &e) {
      r.warning = std::string("Global refresh failed; using prior correction: ") + e.what();
    }
    r.stages["global_ms"] = ms(tick);
  }
  if (!globalValid && r.warning.empty())
    r.warning = "Global alignment unverified; using initial correction";
  tick = now();
  json offsets = fallback(config, "offsets", json{{0, 0}, {0, 0}, {0, 0}});
  Images warped;
  for (int i = 0; i < 3; i++)
    warped[i] = warp(gray[i], h[i], offsets[i]);
  r.stages["global_warp_ms"] = ms(tick);
  auto result = local.refine(warped);
  for (auto it = result.timings.begin(); it != result.timings.end(); ++it)
    r.stages[it.key()] = it.value();
  r.checked = result.checked;
  r.consistency = result.consistency;
  tick = now();
  auto mapping = fallback(config, "mapping", json{0, 1, 2}).get<std::vector<int>>();
  require(mapping.size() == 3 &&
              std::set<int>(mapping.begin(), mapping.end()) == std::set<int>({0, 1, 2}),
          "RGB 채널에 서로 다른 카메라를 선택하세요. 예: R=cam1, G=cam2, B=cam3");
  std::vector<Mat> channels = {result.channels[mapping[2]], result.channels[mapping[1]],
                               result.channels[mapping[0]]};
  merge(channels, r.image);
  r.stages["merge_ms"] = ms(tick);
  r.stages["jpeg_ms"] = 0.;
  double sum = 0;
  for (auto it = r.stages.begin(); it != r.stages.end(); ++it)
    if (it.key().rfind("gpu_", 0) != 0 && it.key().rfind("cpu_", 0) != 0)
      sum += it.value().get<double>();
  double total = ms(begin);
  r.stages["other_ms"] = std::max(0., total - sum);
  r.stages["total_ms"] = total;
  r.globalValid = globalValid;
  r.backend = local.backendStatus();
  return r;
}
static int nearest(const std::vector<int64_t> &a, int64_t stamp) {
  auto it = std::lower_bound(a.begin(), a.end(), stamp);
  int n = it - a.begin();
  if (n == 0)
    return 0;
  if (n == int(a.size()))
    return n - 1;
  return stamp - a[n - 1] <= a[n] - stamp ? n - 1 : n;
}
Session::Session(const fs::path &p) : folder(p) {
  for (int i = 0; i < 3; i++) {
    std::ifstream f(folder / ("cam" + std::to_string(i + 1) + ".csv"));
    require(bool(f), "Missing timestamp CSV");
    std::string line;
    std::getline(f, line);
    require(line.find("monotonic_ns") != std::string::npos, "Invalid CSV header");
    int index = 0;
    while (std::getline(f, line)) {
      if (line.empty())
        continue;
      auto comma = line.find(',');
      require(comma != std::string::npos, "Invalid CSV row");
      require(std::stoi(line.substr(0, comma)) == index++, "CSV frame indices must be consecutive");
      int64_t t = std::stoll(line.substr(comma + 1));
      require(times[i].empty() || t > times[i].back(), "Timestamps must increase");
      times[i].push_back(t);
    }
    require(times[i].size() >= 2, "At least two frames per camera required");
    caps[i].open((folder / ("cam" + std::to_string(i + 1) + ".avi")).string());
    require(caps[i].isOpened(), "Cannot open source video");
  }
  int64_t begin = std::max({times[0].front(), times[1].front(), times[2].front()}),
          end = std::min({times[0].back(), times[1].back(), times[2].back()});
  for (auto t : times[0])
    if (t >= begin && t <= end)
      targets.push_back(t);
  require(targets.size() >= 2, "No common timeline");
  std::vector<double> delta;
  for (size_t i = 1; i < targets.size(); i++)
    delta.push_back(targets[i] - targets[i - 1]);
  fps = 1e9 / median(delta);
}
Images Session::at(int64_t stamp) {
  for (int i = 0; i < 3; i++) {
    int index = nearest(times[i], stamp);
    if (index < positions[i]) {
      caps[i].set(CAP_PROP_POS_FRAMES, index);
      positions[i] = index - 1;
    }
    while (positions[i] < index) {
      require(caps[i].read(frames[i]), "Source video shorter than timestamp log");
      require(frames[i].size() == Size(640, 480), "Expected 640x480 source");
      positions[i]++;
    }
  }
  return frames;
}
void Session::rewind() {
  for (int i = 0; i < 3; i++) {
    caps[i].set(CAP_PROP_POS_FRAMES, 0);
    positions[i] = -1;
  }
}
int64_t Session::position(double v) const {
  require(std::isfinite(v) && v >= 0 && v <= 1, "Invalid timeline position");
  int64_t begin = std::max({times[0].front(), times[1].front(), times[2].front()}),
          end = std::min({times[0].back(), times[1].back(), times[2].back()});
  return begin + int64_t((end - begin) * v);
}
json preview(const fs::path &path, const json &config, Mat &image) {
  if (config.value("load_profile", false)) {
    return fs::exists(path / "alignment.json") ? readJson(path / "alignment.json") : json::object();
  }
  Session s(path);
  auto frames = s.at(s.position(config.value("position", .5)));
  Images gray;
  for (int i = 0; i < 3; i++)
    cvtColor(frames[i], gray[i], COLOR_BGR2GRAY);
  if (config.value("auto_register", false)) {
    auto r = automatic(gray);
    return {
        {"points", r.points}, {"reports", r.reports}, {"note", "Selected-frame static transform"}};
  }
  if (config.contains("raw") && !config["raw"].is_null()) {
    int i = config["raw"];
    require(i >= 0 && i < 3, "Invalid camera");
    image = gray[i];
    return nullptr;
  }
  auto h =
      registration(fallback(config, "points", nullptr), fallback(config, "adjustments", nullptr));
  auto offset = fallback(config, "offsets", json{{0, 0}, {0, 0}, {0, 0}});
  auto map = fallback(config, "mapping", json{0, 1, 2}).get<std::vector<int>>();
  require(map.size() == 3 && std::set<int>(map.begin(), map.end()) == std::set<int>({0, 1, 2}),
          "RGB 채널에 서로 다른 카메라를 선택하세요. 예: R=cam1, G=cam2, B=cam3");
  for (int i = 0; i < 3; i++)
    gray[i] = warp(gray[i], h[i], offset[i]);
  merge(std::vector<Mat>{gray[map[2]], gray[map[1]], gray[map[0]]}, image);
  return nullptr;
}
json compose(const fs::path &folder, const json &config,
             const std::function<void(json)> &progress) {
  Session s(folder);
  bool dynamic = config.value("dynamic", false),
       localEnabled = config.value("local_refinement", false),
       whole = config.value("global_stabilization", false);
  std::string mode = config.value("registration_method", std::string("orb")),
              stability = config.value("stabilization", std::string("balanced"));
  int window = config.value("temporal_window", 0);
  require(mode == "orb" || mode == "tracking", "Invalid registration method");
  require(window == 0 || window == 15 || window == 31 || window == 61, "Invalid temporal window");
  require(!localEnabled || dynamic, "Local requires dynamic mode");
  require(!(localEnabled && whole), "Local confidence requires global stabilization disabled");
  auto initial =
      registration(fallback(config, "points", nullptr), fallback(config, "adjustments", nullptr));
  auto offset = fallback(config, "offsets", json{{0, 0}, {0, 0}, {0, 0}});
  auto map = fallback(config, "mapping", json{0, 1, 2}).get<std::vector<int>>();
  require(map.size() == 3 && std::set<int>(map.begin(), map.end()) == std::set<int>({0, 1, 2}),
          "RGB 채널에 서로 다른 카메라를 선택하세요. 예: R=cam1, G=cam2, B=cam3");
  std::vector<Matrices> path;
  std::vector<json> records;
  std::vector<Point2d> wholeMotion;
  Temporal temporal(initial, stability);
  FlowTracker tracker;
  GlobalStabilizer global;
  auto last = initial;
  int estimated = 0, failed = 0;
  std::string stem = dynamic ? "composite_dynamic" : "composite";
  if (dynamic && mode == "tracking")
    stem += "_tracking";
  if (whole)
    stem += "_stabilized";
  if (dynamic && window)
    stem += "_smooth";
  if (localEnabled)
    stem += "_local";
  fs::path destination = folder / (stem + ".avi");
  writeJson(folder / "registration_dynamic_summary.json", {{"completed", false}});
  std::string baseStem = dynamic ? "composite_dynamic" : "composite";
  if (dynamic && mode == "tracking")
    baseStem += "_tracking";
  if (whole)
    baseStem += "_stabilized";
  fs::path baseFile = folder / (baseStem + ".avi");
  VideoWriter baseline;
  if (baseFile != destination) {
    baseline.open(baseFile.string(), VideoWriter::fourcc('M', 'J', 'P', 'G'), s.fps,
                  Size(640, 480));
    require(baseline.isOpened(), "Cannot create baseline video");
  }
  for (size_t n = 0; n < s.targets.size(); n++) {
    auto bgr = s.at(s.targets[n]);
    Images gray;
    for (int i = 0; i < 3; i++)
      cvtColor(bgr[i], gray[i], COLOR_BGR2GRAY);
    std::string state = "static", reason, filter, method;
    json quality = json::array(), globalReport = nullptr;
    Point2d motion(0, 0);
    if (whole) {
      auto result = global.update(gray[0]);
      motion = result.first;
      globalReport = result.second;
    }
    if (dynamic) {
      try {
        auto result = mode == "tracking" ? tracker.estimate(gray) : automatic(gray);
        method = result.method;
        auto candidate = registration(result.points, fallback(config, "adjustments", nullptr));
        auto filtered = temporal.update(candidate);
        last = filtered.first;
        filter = filtered.second;
        quality = result.reports;
        if (filter == "held_jump") {
          failed++;
          state = "fallback_previous";
          reason = "Awaiting consistent large change";
        } else {
          estimated++;
          state = "estimated";
        }
      } catch (const std::exception &e) {
        temporal.miss();
        failed++;
        state = estimated ? "fallback_previous" : "fallback_initial";
        reason = e.what();
      }
    }
    if (baseline.isOpened()) {
      Images corrected;
      for (int i = 0; i < 3; i++)
        corrected[i] = warp(gray[i], last[i], offset[i]);
      Mat image;
      merge(std::vector<Mat>{corrected[map[2]], corrected[map[1]], corrected[map[0]]}, image);
      if (whole) {
        Mat t = (Mat_<double>(2, 3) << 1, 0, motion.x, 0, 1, motion.y);
        Mat shifted;
        warpAffine(image, shifted, t, Size(640, 480));
        image = shifted;
      }
      baseline.write(image);
    }
    path.push_back(last);
    wholeMotion.push_back(motion);
    json timestamps = json::array();
    for (int i = 0; i < 3; i++)
      timestamps.push_back(s.times[i][nearest(s.times[i], s.targets[n])]);
    records.push_back({{"frame", n},
                       {"monotonic_ns", s.targets[n]},
                       {"source_monotonic_ns", timestamps},
                       {"state", state},
                       {"reason", reason},
                       {"filter_state", filter},
                       {"method", method},
                       {"quality", quality},
                       {"matrices", toJson(last)},
                       {"global_stabilization", globalReport}});
    if (progress)
      progress({{"running", true},
                {"phase", "정합 분석"},
                {"done", n + 1},
                {"total", s.targets.size()},
                {"estimated", estimated},
                {"fallback", failed}});
  }
  baseline.release();
  if (dynamic) {
    std::ofstream log(folder / "registration_dynamic.jsonl");
    require(bool(log), "Cannot create diagnostics");
    for (auto &r : records)
      log << r.dump() << '\n';
  }
  auto original = path;
  if (dynamic && window)
    path = smoothPath(path, window);
  s.rewind();
  VideoWriter output(destination.string(), VideoWriter::fourcc('M', 'J', 'P', 'G'), s.fps,
                     Size(640, 480));
  require(output.isOpened(), "Cannot open output encoder");
  std::unique_ptr<LocalRefiner> local;
  VideoWriter masks;
  std::ofstream localLog, smoothLog;
  fs::path maskPath = folder / (stem + "_validity.avi");
  double consistencyTotal = 0;
  if (localEnabled) {
    local = std::make_unique<LocalRefiner>();
    masks.open(maskPath.string(), VideoWriter::fourcc('M', 'J', 'P', 'G'), s.fps, Size(640, 480));
    require(masks.isOpened(), "Cannot open confidence encoder");
    localLog.open(folder / "local_registration.jsonl");
    require(bool(localLog), "Cannot create local diagnostics");
  }
  if (dynamic && window)
    smoothLog.open(folder / "registration_smoothed.jsonl");
  for (size_t n = 0; n < s.targets.size(); n++) {
    auto bgr = s.at(s.targets[n]);
    Images channels;
    for (int i = 0; i < 3; i++) {
      Mat gray;
      cvtColor(bgr[i], gray, COLOR_BGR2GRAY);
      channels[i] = warp(gray, path[n][i], offset[i]);
    }
    if (local) {
      auto r = local->refine(channels);
      channels = r.channels;
      Mat bgrMask;
      cvtColor(r.validity, bgrMask, COLOR_GRAY2BGR);
      masks.write(bgrMask);
      consistencyTotal += r.consistency;
      localLog << json({{"frame", n},
                        {"consistent_fraction", r.consistency},
                        {"cameras", r.reports}})
                      .dump()
               << '\n';
    }
    Mat composite;
    merge(std::vector<Mat>{channels[map[2]], channels[map[1]], channels[map[0]]}, composite);
    if (whole) {
      Mat t = (Mat_<double>(2, 3) << 1, 0, wholeMotion[n].x, 0, 1, wholeMotion[n].y);
      Mat adjusted;
      warpAffine(composite, adjusted, t, Size(640, 480));
      composite = adjusted;
    }
    output.write(composite);
    if (smoothLog.is_open()) {
      json r = records[n];
      r["unsmoothed_matrices"] = toJson(original[n]);
      r["matrices"] = toJson(path[n]);
      r["temporal_window"] = window;
      smoothLog << r.dump() << '\n';
    }
    if (progress)
      progress({{"running", true},
                {"phase", "영상 저장"},
                {"done", n + 1},
                {"total", s.targets.size()},
                {"estimated", estimated},
                {"fallback", failed}});
  }
  output.release();
  masks.release();
  json profile = {{"rgb_cameras", map},
                  {"xy_offsets", offset},
                  {"points", fallback(config, "points", nullptr)},
                  {"adjustments", fallback(config, "adjustments", nullptr)},
                  {"dynamic", dynamic},
                  {"registration_method", mode},
                  {"stabilization", stability},
                  {"temporal_window", window},
                  {"global_stabilization", whole},
                  {"local_refinement", localEnabled}};
  writeJson(folder / "alignment.json", profile);
  json result = {
      {"file", destination.string()},
      {"baseline_file", baseFile.string()},
      {"frames", s.targets.size()},
      {"fps", s.fps},
      {"estimated", estimated},
      {"fallback", failed},
      {"completed", true},
      {"implementation", "C++"},
      {"dynamic", dynamic},
      {"warning", dynamic && !estimated
                      ? json("No successful automatic registration; initial correction used")
                      : json(nullptr)}};
  if (localEnabled) {
    result["validity_file"] = maskPath.string();
    result["local_consistent_fraction"] = consistencyTotal / s.targets.size();
    result["local_warning"] =
        "Consistency does not guarantee true correspondence; inspect validity video";
  }
  writeJson(folder / "registration_dynamic_summary.json", result);
  return result;
}
json compare(const fs::path &folder) {
  std::vector<fs::path> candidates;
  for (auto &e : fs::directory_iterator(folder))
    if (e.is_regular_file() && e.path().filename().string().size() > 10 &&
        e.path().stem().string().size() >= 6 &&
        e.path().stem().string().substr(e.path().stem().string().size() - 6) == "_local")
      candidates.push_back(e.path());
  require(!candidates.empty(), "No local composition to compare");
  std::sort(candidates.begin(), candidates.end(),
            [](auto a, auto b) { return fs::last_write_time(a) < fs::last_write_time(b); });
  auto local = candidates.back();
  std::string stem = local.stem().string();
  stem.resize(stem.size() - 6);
  fs::path baseline = folder / (stem + ".avi");
  if (!fs::exists(baseline)) {
    auto p = stem.find("_smooth");
    if (p != std::string::npos)
      stem.erase(p, 7);
    baseline = folder / (stem + ".avi");
  }
  require(fs::exists(baseline), "No baseline found. Run a global composition first");
  std::vector<fs::path> files = {baseline, local};
  auto validity = folder / (local.stem().string() + "_validity.avi");
  if (fs::exists(validity))
    files.push_back(validity);
  fs::path output = folder / "comparison.avi";
  require(!fs::exists(output), "comparison.avi already exists");
  std::vector<VideoCapture> caps;
  for (auto &p : files) {
    caps.emplace_back(p.string());
    require(caps.back().isOpened(), "Cannot open comparison input");
  }
  VideoWriter writer(output.string(), VideoWriter::fourcc('M', 'J', 'P', 'G'),
                     caps[1].get(CAP_PROP_FPS), Size(640 * files.size(), 480));
  require(writer.isOpened(), "Cannot open comparison encoder");
  int count = 0;
  while (true) {
    std::vector<Mat> frames;
    int success = 0;
    for (auto &cap : caps) {
      Mat f;
      if (cap.read(f))
        success++;
      frames.push_back(f);
    }
    if (!success)
      break;
    require(success == int(caps.size()), "Comparison lengths differ");
    for (size_t i = 0; i < frames.size(); i++) {
      rectangle(frames[i], Rect(0, 0, 640, 35), Scalar(0, 0, 0), FILLED);
      putText(frames[i],
              i == 0   ? "Global"
              : i == 1 ? "Local"
                       : "Consistency",
              Point(10, 25), FONT_HERSHEY_SIMPLEX, .7, Scalar(255, 255, 255), 1);
    }
    Mat joined;
    hconcat(frames, joined);
    writer.write(joined);
    count++;
  }
  return {{"file", output.string()}, {"frames", count}};
}
json benchmark(const fs::path &folder, const json &config, const fs::path &report, int stride) {
  require(stride > 0, "Invalid stride");
  Session session(folder);
  LivePipeline pipeline(config);
  json metadata = config;
  metadata["implementation"] = "C++";
  metadata["opencv"] = CV_VERSION;
  metadata["opencv_build_info"] = getBuildInformation();
  metadata["compiler"] = __VERSION__;
  metadata["opencv_threads"] = getNumThreads();
  metadata["benchmark_stride"] = stride;
  Profile profile(metadata);
  std::vector<int> indices;
  for (int n = 0; n < int(session.targets.size()); n += stride)
    indices.push_back(n);
  VideoWriter writer;
  auto video = report.parent_path() / (report.stem().string() + ".avi");
  writer.open(video.string(), VideoWriter::fourcc('M', 'J', 'P', 'G'), session.fps / stride,
              Size(640, 480));
  require(writer.isOpened(), "Cannot create benchmark video");
  for (int n : indices) {
    auto frames = session.at(session.targets[n]);
    auto r = pipeline.process(frames, (session.targets[n] - session.targets.front()) / 1e9);
    double tick = now();
    std::vector<uchar> encoded;
    imencode(".jpg", r.image, encoded, {IMWRITE_JPEG_QUALITY, 75});
    double elapsed = ms(tick);
    r.stages["jpeg_ms"] = elapsed;
    r.stages["total_ms"] = r.stages["total_ms"].get<double>() + elapsed;
    profile.setBackend(r.backend);
    profile.record(r.stages, r.stages["global_ms"].get<double>() > 0, r.checked);
    writer.write(r.image);
  }
  writer.release();
  auto result = profile.summary();
  result["video"] = video.string();
  writeJson(report, result);
  return result;
}
} // namespace imaging
