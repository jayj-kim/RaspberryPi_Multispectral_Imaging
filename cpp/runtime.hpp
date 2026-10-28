#pragma once
#include "core.hpp"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
namespace imaging {
struct Frame {
  int64_t stamp = 0;
  cv::Mat image;
  int sequence = 0;
};
struct Rig {
  fs::path output;
  bool demo;
  std::atomic<bool> halt{false};
  mutable std::mutex mutex;
  std::array<std::deque<Frame>, 3> history;
  std::array<json, 3> camera;
  std::array<std::thread, 3> threads;
  bool recording = false;
  fs::path recordingPath;
  std::array<cv::VideoWriter, 3> writers;
  std::array<std::ofstream, 3> csv;
  std::array<int, 3> counts{{0, 0, 0}};
  Rig(fs::path, bool);
  ~Rig();
  void start();
  void stop();
  void capture(int);
  std::string begin();
  std::string finish();
  std::array<Frame, 3> snapshot();
  json status() const;
};
class Live {
  Rig &rig;
  mutable std::mutex mutex;
  std::mutex control;
  std::condition_variable condition;
  std::thread thread;
  std::atomic<bool> halt{true};
  json metrics = {{"running", false}};
  Profile profile;
  std::vector<uchar> jpeg;
  int sequence = 0;
  void run(json);

public:
  explicit Live(Rig &);
  ~Live();
  json status() const;
  json measurement() const;
  void start(json);
  void stop();
  bool next(int &last, std::string &packet);
};
int serve(fs::path output, bool demo, int port, const fs::path &web,
          const std::string &defaultBackend = "cpu", bool allowSoftware = false);
std::string timestamp();
} // namespace imaging
