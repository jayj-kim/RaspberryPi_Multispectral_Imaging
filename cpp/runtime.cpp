#include "runtime.hpp"
#include <csignal>
#include <ctime>
#include <fstream>
#include <httplib.h>
#include <iomanip>
#include <iostream>
#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
namespace imaging {
static const std::array<std::string, 3> devices = {
    "/dev/v4l/by-path/platform-xhci-hcd.1-usb-0:2:1.0-video-index0",
    "/dev/v4l/by-path/platform-xhci-hcd.0-usb-0:2:1.0-video-index0",
    "/dev/v4l/by-path/platform-xhci-hcd.1-usb-0:1:1.0-video-index0"};
static void require(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
}
std::string timestamp() {
  auto t = std::chrono::system_clock::now();
  auto v = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &v);
#else
  localtime_r(&v, &tm);
#endif
  std::ostringstream s;
  s << std::put_time(&tm, "%Y%m%d_%H%M%S") << '_'
    << std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count() %
           1000000;
  return s.str();
}
Rig::Rig(fs::path p, bool d) : output(fs::absolute(p)), demo(d) {
  for (auto &s : camera)
    s = {{"frames", 0}, {"fps", 0}, {"error", "Starting"}};
}
Rig::~Rig() {
  try {
    stop();
  } catch (...) {
  }
}
void Rig::start() {
  for (int i = 0; i < 3; i++)
    threads[i] = std::thread([this, i] { capture(i); });
}
void Rig::stop() {
  halt = true;
  for (auto &t : threads)
    if (t.joinable())
      t.join();
  finish();
}
void Rig::capture(int i) {
  try {
    cv::VideoCapture cap;
    if (!demo) {
#ifdef __linux__
      int fd = ::open(devices[i].c_str(), O_RDWR | O_NONBLOCK);
      require(fd >= 0, "Cannot open V4L2 camera");
      v4l2_control mode{};
      mode.id = V4L2_CID_EXPOSURE_AUTO;
      mode.value = V4L2_EXPOSURE_MANUAL;
      int a = ioctl(fd, VIDIOC_S_CTRL, &mode);
      mode.id = V4L2_CID_EXPOSURE_ABSOLUTE;
      mode.value = 4500;
      int b = ioctl(fd, VIDIOC_S_CTRL, &mode);
      ::close(fd);
      require(a == 0 && b == 0, "Cannot set manual exposure 4500");
#endif
      cap.open(devices[i], cv::CAP_V4L2);
      require(cap.isOpened(), "Cannot open capture");
      cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V'));
      cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
      cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
      cap.set(cv::CAP_PROP_FPS, 30);
    }
    double started = now();
    int count = 0;
    while (!halt) {
      cv::Mat frame;
      if (demo) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        frame = cv::Mat::zeros(480, 640, CV_8UC3);
        std::vector<cv::Mat> channels(3);
        cv::split(frame, channels);
        channels[i].setTo(160);
        cv::merge(channels, frame);
        cv::circle(frame, cv::Point(int(now() * 70) % 600 + 20, 240), 30, cv::Scalar(255, 255, 255),
                   -1);
        cv::putText(frame, "DEMO cam" + std::to_string(i + 1), cv::Point(20, 50),
                    cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(255, 255, 255), 2);
      } else
        require(cap.read(frame), "Camera read failed; reconnect and restart");
      require(frame.size() == cv::Size(640, 480), "Unexpected camera resolution");
      int64_t stamp = ns();
      count++;
      std::lock_guard<std::mutex> guard(mutex);
      history[i].push_back({stamp, frame, count});
      if (history[i].size() > 8)
        history[i].pop_front();
      camera[i] = {{"frames", count}, {"fps", count / (now() - started)}, {"error", nullptr}};
      if (recording) {
        writers[i].write(frame);
        csv[i] << counts[i]++ << ',' << stamp << '\n';
        require(bool(csv[i]), "Recording timestamp write failed");
      }
    }
  } catch (const std::exception &e) {
    {
      std::lock_guard<std::mutex> guard(mutex);
      camera[i]["error"] = e.what();
    }
    try {
      finish();
    } catch (...) {
    }
  }
}
std::string Rig::begin() {
  std::lock_guard<std::mutex> guard(mutex);
  require(!recording, "Already recording");
  for (auto &s : camera)
    require(s["error"].is_null(), "All cameras must be ready");
  recordingPath = output / timestamp();
  fs::create_directories(recordingPath);
  try {
    for (int i = 0; i < 3; i++) {
      writers[i].open((recordingPath / ("cam" + std::to_string(i + 1) + ".avi")).string(),
                      cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30, cv::Size(640, 480));
      require(writers[i].isOpened(), "Recording encoder failed");
      csv[i].open(recordingPath / ("cam" + std::to_string(i + 1) + ".csv"));
      require(bool(csv[i]), "Cannot open timestamps");
      csv[i] << "frame,monotonic_ns\n";
      counts[i] = 0;
    }
  } catch (...) {
    for (auto &w : writers)
      w.release();
    for (auto &f : csv)
      f.close();
    throw;
  }
  recording = true;
  return recordingPath.filename().string();
}
std::string Rig::finish() {
  std::lock_guard<std::mutex> guard(mutex);
  if (!recording)
    return "";
  recording = false;
  for (auto &w : writers)
    w.release();
  for (auto &f : csv)
    f.close();
  writeJson(recordingPath / "session.json",
            {{"demo", demo},
             {"devices", devices},
             {"frames", counts},
             {"nominal_fps", 30},
             {"implementation", "C++"},
             {"timestamp", "host steady clock after read, not sensor exposure time"}});
  return recordingPath.filename().string();
}
std::array<Frame, 3> Rig::snapshot() {
  std::lock_guard<std::mutex> guard(mutex);
  int64_t stamp = INT64_MAX;
  for (int i = 0; i < 3; i++) {
    require(camera[i]["error"].is_null(), "Camera unavailable");
    require(!history[i].empty(), "Waiting for cameras");
    stamp = std::min(stamp, history[i].back().stamp);
  }
  std::array<Frame, 3> out;
  for (int i = 0; i < 3; i++) {
    auto it = std::min_element(history[i].begin(), history[i].end(), [&](auto &a, auto &b) {
      return std::abs(a.stamp - stamp) < std::abs(b.stamp - stamp);
    });
    out[i] = *it;
    require(ns() - out[i].stamp < 1000000000LL, "Camera frame older than one second");
  }
  return out;
}
json Rig::status() const {
  std::lock_guard<std::mutex> guard(mutex);
  return {{"cameras", camera},
          {"recording", recording ? json(recordingPath.filename().string()) : json(nullptr)}};
}
Live::Live(Rig &r) : rig(r), profile(json::object()) {}
Live::~Live() {
  stop();
}
json Live::status() const {
  std::lock_guard<std::mutex> guard(mutex);
  json j = metrics;
  j["profile"] = profile.summary();
  if (j.contains("source_ns"))
    j["host_age_ms"] = (ns() - j["source_ns"].get<int64_t>()) / 1e6;
  return j;
}
json Live::measurement() const {
  std::lock_guard<std::mutex> guard(mutex);
  return profile.summary();
}
void Live::start(json body) {
  std::lock_guard<std::mutex> operation(control);
  int width = body.value("flow_width", 320), fps = body.value("target_fps", 5);
  std::string backend = body.value("compute_backend", std::string("cpu"));
  require(backend == "cpu" || backend == "auto" || backend == "vulkan", "Invalid compute backend");
  std::string cpuRemap = body.value("cpu_remap", std::string("float"));
  require(cpuRemap == "float" || cpuRemap == "fixed", "Invalid CPU remap mode");
  require(width == 320 || width == 480 || width == 640, "Invalid calculation width");
  require(fps == 5 || fps == 10 || fps == 15 || fps == 20 || fps == 30, "Invalid target fps");
  registration(fallback(body, "points", nullptr), fallback(body, "adjustments", nullptr));
  auto map = fallback(body, "mapping", json{0, 1, 2}).get<std::vector<int>>();
  require(map.size() == 3 && std::set<int>(map.begin(), map.end()) == std::set<int>({0, 1, 2}),
          "Invalid RGB mapping");
  auto offsets = fallback(body, "offsets", json{{0, 0}, {0, 0}, {0, 0}});
  require(offsets.size() == 3, "Three offsets required");
  for (auto &o : offsets) {
    require(o.size() == 2, "Two offset coordinates required");
    double x = o[0], y = o[1];
    require(std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 640 && std::abs(y) <= 640,
            "Invalid offset");
  }
  {
    std::lock_guard<std::mutex> guard(mutex);
    require(!metrics.value("running", false), "Already running");
  }
  if (thread.joinable())
    thread.join();
  {
    std::lock_guard<std::mutex> guard(mutex);
    halt = false;
    jpeg.clear();
    sequence = 0;
    json c = {{"cpu_remap", cpuRemap},
              {"flow_width", width},
              {"compute_backend", backend},
              {"target_fps", fps},
              {"implementation", "C++"},
              {"opencv", CV_VERSION},
              {"opencv_threads", cv::getNumThreads()},
              {"demo", rig.demo}};
    profile = Profile(c);
    metrics = {{"running", true},
               {"processed", 0},
               {"target_fps", fps},
               {"flow_width", width},
               {"warning", "Waiting for frames"}};
  }
  thread = std::thread([this, body] { run(body); });
}
void Live::stop() {
  std::lock_guard<std::mutex> operation(control);
  halt = true;
  condition.notify_all();
  if (thread.joinable())
    thread.join();
}
void Live::run(json config) {
  try {
    LivePipeline pipeline(config);
    int target = config.value("target_fps", 5), processed = 0, skipped = 0, previous = -1;
    std::array<int, 3> last{{-1, -1, -1}};
    double priorTime = -1, rate = 0;
    while (!halt && !rig.halt) {
      double begin = now();
      std::array<Frame, 3> frames;
      try {
        frames = rig.snapshot();
      } catch (const std::exception &e) {
        {
          std::lock_guard<std::mutex> guard(mutex);
          metrics["warning"] = e.what();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      double picked = ms(begin);
      std::array<int, 3> seq{{frames[0].sequence, frames[1].sequence, frames[2].sequence}};
      if (seq == last) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      last = seq;
      if (previous >= 0)
        skipped += std::max(0, seq[0] - previous - 1);
      previous = seq[0];
      Images images = {frames[0].image, frames[1].image, frames[2].image};
      auto result = pipeline.process(images, begin);
      if (result.backend.contains("warning") && !result.backend["warning"].is_null()) {
        if (!result.warning.empty())
          result.warning += "; ";
        result.warning += result.backend["warning"].get<std::string>();
      }
      double tick = now();
      std::vector<uchar> encoded;
      require(cv::imencode(".jpg", result.image, encoded, {cv::IMWRITE_JPEG_QUALITY, 75}),
              "JPEG failed");
      result.stages["jpeg_ms"] = ms(tick);
      result.stages["frame_pick_ms"] = picked;
      double published = now();
      result.stages["total_ms"] = ms(begin);
      if (priorTime > 0) {
        double current = 1 / (published - priorTime);
        rate = rate == 0 ? current : .8 * rate + .2 * current;
      }
      priorTime = published;
      processed++;
      bool recording = rig.status()["recording"].is_string();
      int64_t oldest = std::min({frames[0].stamp, frames[1].stamp, frames[2].stamp}),
              newest = std::max({frames[0].stamp, frames[1].stamp, frames[2].stamp});
      {
        std::lock_guard<std::mutex> guard(mutex);
        profile.setBackend(result.backend);
        profile.record(result.stages, result.stages["global_ms"].get<double>() > 0, result.checked,
                       recording);
        jpeg = std::move(encoded);
        sequence++;
        metrics["running"] = true;
        metrics["processed"] = processed;
        metrics["fps"] = rate;
        metrics["processing_ms"] = result.stages["total_ms"];
        metrics["source_ns"] = oldest;
        metrics["camera_skew_ms"] = (newest - oldest) / 1e6;
        metrics["skipped_cam1"] = skipped;
        metrics["global_valid"] = result.globalValid;
        metrics["backend"] = result.backend;
        metrics["consistency"] = result.consistency < 0 ? json(nullptr) : json(result.consistency);
        metrics["consistency_checked"] = result.checked;
        if (result.checked)
          metrics["consistency_frame"] = processed;
        metrics["warning"] = result.warning.empty() ? json(nullptr) : json(result.warning);
      }
      condition.notify_all();
      std::unique_lock<std::mutex> wait(mutex);
      condition.wait_for(wait,
                         std::chrono::duration<double>(std::max(0., 1. / target - (now() - begin))),
                         [&] { return bool(halt); });
    }
  } catch (const std::exception &e) {
    std::lock_guard<std::mutex> guard(mutex);
    metrics["warning"] = std::string("Live stopped: ") + e.what();
  }
  {
    std::lock_guard<std::mutex> guard(mutex);
    try {
      fs::create_directories(rig.output / "profiles");
      auto path = rig.output / "profiles" / ("live_cpp_" + timestamp() + ".json");
      writeJson(path, profile.summary());
      metrics["profile_file"] = path.string();
    } catch (const std::exception &e) {
      metrics["warning"] = e.what();
    }
    metrics["running"] = false;
  }
  condition.notify_all();
}
bool Live::next(int &last, std::string &packet) {
  std::unique_lock<std::mutex> guard(mutex);
  condition.wait_for(guard, std::chrono::seconds(1), [&] {
    return halt || (!jpeg.empty() && sequence != last) || !metrics.value("running", false);
  });
  if (halt || !metrics.value("running", false))
    return false;
  if (jpeg.empty() || sequence == last) {
    packet.clear();
    return true;
  }
  last = sequence;
  packet = "--frame\r\nContent-Type: image/jpeg\r\n\r\n";
  packet.append(reinterpret_cast<const char *>(jpeg.data()), jpeg.size());
  packet += "\r\n";
  return true;
}
static std::atomic<bool> interrupted{false};
static void interrupt(int) {
  interrupted = true;
}
int serve(fs::path output, bool demo, int port, const fs::path &web,
          const std::string &defaultBackend, bool allowSoftware) {
  Rig rig(output, demo);
  Live live(rig);
  httplib::Server server;
  server.new_task_queue = []() { return new httplib::ThreadPool(16); };
  server.set_payload_max_length(1024 * 1024);
  std::mutex job, progressMutex;
  json processing = {{"running", false}};
  std::ifstream html(web);
  require(bool(html), "Cannot find web UI: " + web.string());
  std::string page((std::istreambuf_iterator<char>(html)), {});
  auto mark = page.find("__DEMO__");
  if (mark != std::string::npos)
    page.replace(mark, 8, demo ? " — DEMO" : "");
  auto backendMark = page.find("__DEFAULT_BACKEND__");
  if (backendMark != std::string::npos)
    page.replace(backendMark, 19, defaultBackend);
  auto reply = [](httplib::Response &r, const json &j, int status = 200) {
    r.status = status;
    r.set_content(j.dump(), "application/json; charset=utf-8");
    r.set_header("Cache-Control", "no-store");
  };
  server.set_pre_routing_handler([&](const httplib::Request &q, httplib::Response &r) {
    std::string host = q.get_header_value("Host");
    auto colon = host.find(':');
    std::string name = host.substr(0, colon);
    if (name != "127.0.0.1" && name != "localhost") {
      reply(r, {{"error", "Local access only"}}, 403);
      return httplib::Server::HandlerResponse::Handled;
    }
    if (q.method == "POST" && q.get_header_value("X-Rig-Request") != "1") {
      reply(r, {{"error", "Missing request header"}}, 403);
      return httplib::Server::HandlerResponse::Handled;
    }
    std::string origin = q.get_header_value("Origin");
    if (!origin.empty() && origin != "http://" + host) {
      reply(r, {{"error", "Foreign origin"}}, 403);
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  server.set_exception_handler(
      [&](const httplib::Request &, httplib::Response &r, std::exception_ptr error) {
        try {
          if (error)
            std::rethrow_exception(error);
        } catch (const std::exception &e) {
          reply(r, {{"error", e.what()}}, 400);
        }
      });
  auto sessionPath = [&](const json &body) {
    std::string name = body.at("session");
    require(!name.empty() && name != "." && name != ".." && fs::path(name).filename() == name,
            "Invalid session name");
    auto root = fs::weakly_canonical(rig.output), path = fs::weakly_canonical(root / name);
    require(path.parent_path() == root && fs::is_regular_file(path / "session.json"),
            "Not a completed session");
    return path;
  };
  server.Get("/", [&](const auto &, auto &r) { r.set_content(page, "text/html; charset=utf-8"); });
  server.Get("/api/status", [&](const auto &, auto &r) {
    auto data = rig.status();
    data["live"] = live.status();
    {
      std::lock_guard<std::mutex> guard(progressMutex);
      data["processing"] = processing;
    }
    reply(r, data);
  });
  server.Get("/api/vulkan", [&](const auto &, auto &r) { reply(r, vulkanInfo()); });
  server.Get("/api/sessions", [&](const auto &, auto &r) {
    std::vector<std::string> names;
    if (fs::exists(rig.output))
      for (auto &e : fs::directory_iterator(rig.output))
        if (e.is_directory() && fs::is_regular_file(e.path() / "session.json"))
          names.push_back(e.path().filename().string());
    std::sort(names.rbegin(), names.rend());
    reply(r, names);
  });
  server.Post("/api/start", [&](const auto &, auto &r) {
    std::unique_lock<std::mutex> guard(job, std::try_to_lock);
    require(guard.owns_lock(), "Another operation is running");
    reply(r, {{"session", rig.begin()}});
  });
  server.Post("/api/stop", [&](const auto &, auto &r) {
    std::unique_lock<std::mutex> guard(job, std::try_to_lock);
    require(guard.owns_lock(), "Another operation is running");
    auto name = rig.finish();
    reply(r, {{"session", name.empty() ? json(nullptr) : json(name)}});
  });
  server.Post("/api/live/start", [&](const auto &q, auto &r) {
    std::unique_lock<std::mutex> guard(job, std::try_to_lock);
    require(guard.owns_lock(), "Offline composition is running");
    auto config = json::parse(q.body);
    config["compute_backend"] = config.value("compute_backend", defaultBackend);
    config["allow_software_vulkan"] = allowSoftware;
    live.start(config);
    reply(r, live.status());
  });
  server.Post("/api/live/stop", [&](const auto &, auto &r) {
    live.stop();
    reply(r, live.status());
  });
  server.Get("/api/live/profile", [&](const auto &q, auto &r) {
    reply(r, live.measurement());
    if (q.has_param("download"))
      r.set_header("Content-Disposition", "attachment; filename=live_profile_cpp.json");
  });
  server.Post("/api/preview", [&](const auto &q, auto &r) {
    json body = json::parse(q.body);
    cv::Mat image;
    auto data = preview(sessionPath(body), body, image);
    if (!image.empty()) {
      std::vector<uchar> encoded;
      require(cv::imencode(".jpg", image, encoded, {cv::IMWRITE_JPEG_QUALITY, 90}),
              "Preview JPEG failed");
      r.set_content(reinterpret_cast<const char *>(encoded.data()), encoded.size(), "image/jpeg");
      r.set_header("Cache-Control", "no-store");
    } else
      reply(r, data);
  });
  server.Post("/api/compose", [&](const auto &q, auto &r) {
    std::unique_lock<std::mutex> guard(job, std::try_to_lock);
    require(guard.owns_lock(), "Another operation is running");
    require(!live.status().value("running", false), "Stop live composition first");
    require(rig.status()["recording"].is_null(), "Stop recording first");
    auto body = json::parse(q.body);
    auto path = sessionPath(body);
    {
      std::lock_guard<std::mutex> lock(progressMutex);
      processing = {{"running", true}, {"done", 0}, {"total", 0}};
    }
    try {
      auto result = compose(path, body, [&](json update) {
        require(!rig.halt, "Composition cancelled during shutdown");
        std::lock_guard<std::mutex> lock(progressMutex);
        processing = update;
      });
      {
        std::lock_guard<std::mutex> lock(progressMutex);
        processing["running"] = false;
      }
      reply(r, result);
    } catch (...) {
      std::lock_guard<std::mutex> lock(progressMutex);
      processing["running"] = false;
      processing["error"] = "Composition failed; see response";
      throw;
    }
  });
  server.Get("/live/stream", [&](const auto &, auto &r) {
    auto last = std::make_shared<int>(-1);
    r.set_header("Cache-Control", "no-store");
    r.set_chunked_content_provider(
        "multipart/x-mixed-replace; boundary=frame", [&, last](size_t, httplib::DataSink &sink) {
          std::string packet;
          if (!live.next(*last, packet))
            return false;
          return packet.empty() || sink.write(packet.data(), packet.size());
        });
  });
  server.Get(R"(/stream/(\d+))", [&](const auto &q, auto &r) {
    int camera = std::stoi(q.matches[1]);
    require(camera >= 0 && camera < 3, "Invalid camera");
    auto nextTime = std::make_shared<double>(0);
    r.set_chunked_content_provider(
        "multipart/x-mixed-replace; boundary=frame",
        [&, camera, nextTime](size_t, httplib::DataSink &sink) {
          if (rig.halt)
            return false;
          double delay = *nextTime - now();
          if (delay > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(delay));
          *nextTime = now() + .1;
          cv::Mat frame;
          {
            std::lock_guard<std::mutex> guard(rig.mutex);
            if (!rig.history[camera].empty())
              frame = rig.history[camera].back().image;
          }
          if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return true;
          }
          std::vector<uchar> encoded;
          cv::imencode(".jpg", frame, encoded, {cv::IMWRITE_JPEG_QUALITY, 65});
          std::string packet = "--frame\r\nContent-Type: image/jpeg\r\n\r\n";
          packet.append(reinterpret_cast<const char *>(encoded.data()), encoded.size());
          packet += "\r\n";
          return sink.write(packet.data(), packet.size());
        });
  });
  interrupted = false;
  std::signal(SIGINT, interrupt);
  std::signal(SIGTERM, interrupt);
  rig.start();
  std::atomic<bool> finished{false};
  std::thread monitor([&] {
    while (!finished && !interrupted)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (interrupted) {
      rig.halt = true;
      live.stop();
      server.stop();
    }
  });
  std::cout << "C++ camera studio http://127.0.0.1:" << port << " (OpenCV " << CV_VERSION << ")"
            << std::endl;
  bool listened = server.listen("127.0.0.1", port);
  finished = true;
  if (monitor.joinable())
    monitor.join();
  rig.halt = true;
  live.stop();
  rig.stop();
  return listened ? 0 : 1;
}
} // namespace imaging
