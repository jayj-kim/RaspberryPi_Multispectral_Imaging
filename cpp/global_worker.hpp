#pragma once
#include "core.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
namespace imaging {
class GlobalWorker {
public:
  struct Result {
    uint64_t id = 0;
    double sourceTime = 0, submitted = 0, elapsedMs = 0;
    Matrices matrices;
    std::string error;
  };
  using Compute = std::function<Matrices(const Images &)>;
  explicit GlobalWorker(Compute);
  ~GlobalWorker();
  void submit(const Images &, double sourceTime);
  std::optional<Result> take(double sourceTime, double maxAge = .75);
  json status() const;

private:
  struct Job {
    uint64_t id;
    double sourceTime, submitted;
    Images images;
  };
  Compute compute;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::optional<Job> pending;
  std::optional<Result> completed;
  bool stopping = false, busy = false;
  uint64_t requested = 0, finished = 0, replaced = 0, stale = 0, errors = 0;
  double lastMs = 0, sumMs = 0, maxMs = 0;
  std::thread thread;
  void run();
};
} // namespace imaging
