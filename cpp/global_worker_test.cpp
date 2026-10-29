#include "global_worker.hpp"
#include <atomic>
#include <future>
#include <iostream>
using namespace imaging;
static void check(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
static void finished(GlobalWorker &worker, int n) {
  double deadline = now() + 3;
  while (worker.status()["completed"].get<int>() < n && now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(worker.status()["completed"].get<int>() >= n, "Worker did not complete");
}
int main() {
  try {
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    auto begun = started.get_future();
    std::atomic<int> calls{0}, firstPixel{-1};
    GlobalWorker worker([&](const Images &images) {
      if (++calls == 1) {
        started.set_value();
        gate.wait();
        firstPixel = images[0].at<uchar>(0, 0);
      }
      auto h = identities();
      h[1].at<double>(0, 2) = images[0].at<uchar>(0, 0);
      return h;
    });
    Images input = {cv::Mat(8, 8, CV_8U, cv::Scalar(1)), cv::Mat::zeros(8, 8, CV_8U),
                    cv::Mat::zeros(8, 8, CV_8U)};
    worker.submit(input, 0);
    bool running = begun.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!running) {
      release.set_value();
      throw std::runtime_error("Worker did not start");
    }
    input[0].setTo(2);
    worker.submit(input, 0);
    input[0].setTo(3);
    worker.submit(input, 0);
    auto queued = worker.status();
    release.set_value();
    check(queued["pending"] == true && queued["replaced_pending"] == 1,
          "Queue grew instead of replacing old work");
    finished(worker, 2);
    auto result = worker.take(0, 3);
    check(result && result->id == 3 && result->matrices[1].at<double>(0, 2) == 3,
          "Latest pending job/result lost");
    check(firstPixel == 1 && calls == 2, "Source image ownership violated or obsolete job ran");
    check(!worker.take(0), "Result applied twice");
    worker.submit(input, 0);
    finished(worker, 3);
    check(!worker.take(2), "Old source result was accepted");
    check(worker.status()["discarded_stale"] == 1, "Stale result not counted");
    GlobalWorker failing(
        [](const Images &) -> Matrices { throw std::runtime_error("injected failure"); });
    failing.submit(input, 0);
    finished(failing, 1);
    auto failure = failing.take(0, 3);
    check(failure && failure->error == "injected failure",
          "Worker exception escaped or disappeared");
    std::cout << "PASS: bounded pending queue, immutable frames, latest result, stale rejection, "
                 "exception and shutdown\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
