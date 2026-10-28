#pragma once
#include "vulkan_remap.hpp"
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
namespace imaging {
using json = nlohmann::json;
namespace fs = std::filesystem;
using Images = std::array<cv::Mat, 3>;
using Matrices = std::array<cv::Mat, 3>;
using Clock = std::chrono::steady_clock;
inline double now() {
  return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}
inline int64_t ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}
inline double ms(double start) {
  return (now() - start) * 1000;
}
json fallback(const json &, const std::string &, const json &);
cv::Mat matrix(const json &);
json toJson(const cv::Mat &);
json toJson(const Matrices &);
Matrices identities();
Matrices readMatrices(const json &);
Matrices registration(const json &points = nullptr, const json &adjustments = nullptr);
cv::Mat warp(const cv::Mat &, const cv::Mat &, const json &offset);
struct TrackPair {
  std::vector<cv::Point2f> source, target;
};
struct Estimate {
  Matrices h;
  json points, reports;
  std::array<TrackPair, 2> tracks;
  std::string method = "orb";
};
Estimate automatic(const Images &);
class FlowTracker {
  Images previous;
  std::array<TrackPair, 2> tracks;
  bool ready = false;
  int age = 0;

public:
  Estimate estimate(const Images &);
};
class Temporal {
  double alpha, deadband;
  Matrices last;
  bool ready = false;
  std::array<std::vector<cv::Point2f>, 2> pending;
  int pendingCount = 0;

public:
  Temporal(const Matrices &, const std::string &);
  void miss();
  std::pair<Matrices, std::string> update(const Matrices &);
};
class GlobalStabilizer {
  cv::Mat previous;
  cv::Point2d path{0, 0}, smooth{0, 0}, correction{0, 0};

public:
  std::pair<cv::Point2d, json> update(const cv::Mat &);
};
struct Refined {
  Images channels;
  cv::Mat validity;
  json reports;
  json timings;
  bool checked;
  double consistency = -1;
};
class LocalRefiner {
  cv::Ptr<cv::DISOpticalFlow> engine;
  cv::Ptr<cv::CLAHE> clahe;
  int width;
  bool live;
  int count = 0;
  double lastConsistency = -1;
  std::unique_ptr<VulkanRemapper> gpu;
  json backendInfo;
  // Scratch only: never reuse returned images or DIS outputs (nonempty flow enables warm starts).
  Images smallBuffers, resizeBuffers;
  cv::Mat flowBuffer, coordsBuffer, invalidBuffer, backBuffer, reverseBuffer;
  cv::Mat fixedCoords, fractions;
  bool fixedRemap;

public:
  explicit LocalRefiner(int width = 640, bool live = false, const std::string &backend = "cpu",
                        bool allowSoftware = false, const std::string &cpuRemap = "float");
  json backendStatus() const;
  Refined refine(const Images &);
};
std::vector<Matrices> smoothPath(const std::vector<Matrices> &, int window);
class Profile {
  int seen = 0, count = 0, recordingCount = 0;
  json config, totals = json::object(), groups = json::object();
  std::deque<json> samples;

public:
  explicit Profile(json config = json::object());
  void record(const json &, bool refresh, bool check, bool recording = false);
  json summary() const;
  void setBackend(const json &);
};
struct PipelineResult {
  cv::Mat image;
  json stages;
  bool checked = false;
  double consistency = -1;
  bool globalValid = false;
  json backend;
  std::string warning;
};
class LivePipeline {
  Matrices h;
  json config;
  Temporal temporal;
  LocalRefiner local;
  double lastRefresh = -1e30;
  bool globalValid = false;

public:
  explicit LivePipeline(json);
  PipelineResult process(const Images &bgr, double inputTime);
};
struct Session {
  fs::path folder;
  std::array<std::vector<int64_t>, 3> times;
  std::array<cv::VideoCapture, 3> caps;
  std::array<int, 3> positions{{-1, -1, -1}};
  Images frames;
  std::vector<int64_t> targets;
  double fps;
  explicit Session(const fs::path &);
  Images at(int64_t);
  void rewind();
  int64_t position(double) const;
};
json compose(const fs::path &, const json &, const std::function<void(json)> &progress = {});
json preview(const fs::path &, const json &, cv::Mat &image);
json compare(const fs::path &);
json benchmark(const fs::path &, const json &, const fs::path &report, int stride = 1);
void writeJson(const fs::path &, const json &);
json readJson(const fs::path &);
} // namespace imaging
