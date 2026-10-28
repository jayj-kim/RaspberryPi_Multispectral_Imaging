#include "runtime.hpp"
#include <iostream>
int main(int argc, char **argv) {
  using namespace imaging;
  try {
    json options = json::object();
    bool demo = false, allowSoftware = false, backendSpecified = false;
    std::string backend = "cpu";
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
      else if (a == "--backend") {
        backend = value();
        backendSpecified = true;
      } else if (a == "--allow-software-vulkan")
        allowSoftware = true;
      else if (a == "--vulkan-info")
        action = "vulkan_info";
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
                     "runtime is used.\n  --backend cpu|vulkan|auto\n  --vulkan-info\n  "
                     "--allow-software-vulkan (testing only; not GPU acceleration)\n";
        return 0;
      } else
        throw std::runtime_error("Unknown argument " + a);
    }
    if (threads < 1 || port < 1 || port > 65535)
      throw std::runtime_error("Invalid threads/port");
    if (backend != "cpu" && backend != "vulkan" && backend != "auto")
      throw std::runtime_error("Invalid backend");
    cv::setNumThreads(threads);
    cv::setRNGSeed(0);
    if (!settings.empty())
      options = readJson(settings);
    if (backendSpecified)
      options["compute_backend"] = backend;
    if (allowSoftware)
      options["allow_software_vulkan"] = true;
    if (action == "vulkan_info") {
      std::cout << vulkanInfo().dump(2) << std::endl;
      return 0;
    }
    if (action == "benchmark") {
      if (!report.parent_path().empty())
        fs::create_directories(report.parent_path());
      auto result = benchmark(session, options, report, stride);
      if (options.value("compute_backend", std::string("cpu")) == "vulkan" &&
          result["config"]["local_backend"].value("actual", std::string("cpu")) != "vulkan")
        throw std::runtime_error("Requested Vulkan benchmark fell back to CPU; report saved with "
                                 "the real backend. This is not a GPU result.");
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
    return serve(output, demo, port, web, backend, allowSoftware);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}
