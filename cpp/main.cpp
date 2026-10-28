#include "runtime.hpp"
#include <iostream>
int main(int argc, char **argv) {
  using namespace imaging;
  try {
    json options = json::object();
    bool demo = false;
    int port = 8001, threads = 2, stride = 1;
    fs::path output = "captures_cpp", web = DEFAULT_WEB, session, settings,
             report = "cpp_benchmark.json";
    std::string action = "serve";
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      auto value = [&]() {
        if (++i >= argc)
          throw std::runtime_error("Missing value for " + a);
        return std::string(argv[i]);
      };
      if (a == "--demo")
        demo = true;
      else if (a == "--port")
        port = std::stoi(value());
      else if (a == "--cv-threads")
        threads = std::stoi(value());
      else if (a == "--output")
        output = value();
      else if (a == "--web")
        web = value();
      else if (a == "--settings")
        settings = value();
      else if (a == "--report")
        report = value();
      else if (a == "--stride")
        stride = std::stoi(value());
      else if (a == "--benchmark-session") {
        session = value();
        action = "benchmark";
      } else if (a == "--compose-session") {
        session = value();
        action = "compose";
      } else if (a == "--compare-session") {
        session = value();
        action = "compare";
      } else if (a == "--help") {
        std::cout << "imaging_cpp [--demo] [--port 8001] [--output captures_cpp] [--cv-threads "
                     "2]\n  --benchmark-session PATH --settings JSON --report JSON [--stride N]\n  "
                     "--compose-session PATH --settings JSON\n  --compare-session PATH\nNo Python "
                     "runtime is used.\n";
        return 0;
      } else
        throw std::runtime_error("Unknown argument " + a);
    }
    if (threads < 1 || port < 1 || port > 65535)
      throw std::runtime_error("Invalid threads/port");
    cv::setNumThreads(threads);
    cv::setRNGSeed(0);
    if (!settings.empty())
      options = readJson(settings);
    if (action == "benchmark") {
      if (!report.parent_path().empty())
        fs::create_directories(report.parent_path());
      auto result = benchmark(session, options, report, stride);
      std::cout << result.dump(2) << std::endl;
      return 0;
    }
    if (action == "compose") {
      std::cout << compose(session, options).dump(2) << std::endl;
      return 0;
    }
    if (action == "compare") {
      std::cout << compare(session).dump(2) << std::endl;
      return 0;
    }
    return serve(output, demo, port, web);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}
