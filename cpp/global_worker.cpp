#include "global_worker.hpp"
namespace imaging {
GlobalWorker::GlobalWorker(Compute f) : compute(std::move(f)), thread([this] { run(); }) {}
GlobalWorker::~GlobalWorker() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
    pending.reset();
  }
  condition.notify_one();
  if (thread.joinable())
    thread.join();
}
void GlobalWorker::submit(const Images &images, double sourceTime) {
  Images copy;
  for (int i = 0; i < 3; ++i)
    copy[i] = images[i].clone();
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping)
      return;
    if (pending)
      ++replaced;
    pending = Job{++requested, sourceTime, now(), std::move(copy)};
  }
  condition.notify_one();
}
std::optional<GlobalWorker::Result> GlobalWorker::take(double sourceTime, double maxAge) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!completed)
    return {};
  auto result = std::move(completed);
  completed.reset();
  if (std::max(now() - result->submitted, sourceTime - result->sourceTime) > maxAge) {
    ++stale;
    return {};
  }
  return result;
}
json GlobalWorker::status() const {
  std::lock_guard<std::mutex> lock(mutex);
  return {{"mode", "async"},
          {"busy", busy},
          {"pending", pending.has_value()},
          {"requested", requested},
          {"completed", finished},
          {"replaced_pending", replaced},
          {"discarded_stale", stale},
          {"errors", errors},
          {"last_worker_ms", lastMs},
          {"mean_worker_ms", finished ? sumMs / finished : 0.},
          {"max_worker_ms", maxMs},
          {"max_result_age_ms", 750}};
}
void GlobalWorker::run() {
  cv::setRNGSeed(0);
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return stopping || pending.has_value(); });
      if (stopping)
        return;
      job = std::move(*pending);
      pending.reset();
      busy = true;
    }
    Result result;
    result.id = job.id;
    result.sourceTime = job.sourceTime;
    result.submitted = job.submitted;
    double tick = now();
    try {
      result.matrices = compute(job.images);
    } catch (const std::exception &e) {
      result.error = e.what();
    } catch (...) {
      result.error = "Unknown global registration error";
    }
    result.elapsedMs = ms(tick);
    {
      std::lock_guard<std::mutex> lock(mutex);
      busy = false;
      if (stopping)
        return;
      ++finished;
      errors += !result.error.empty();
      lastMs = result.elapsedMs;
      sumMs += lastMs;
      maxMs = std::max(maxMs, lastMs);
      completed = std::move(result);
    }
  }
}
} // namespace imaging
