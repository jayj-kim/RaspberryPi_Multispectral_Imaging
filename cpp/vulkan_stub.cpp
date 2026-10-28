#include "vulkan_remap.hpp"
#include <stdexcept>
namespace imaging {
struct VulkanRemapper::Impl {};
VulkanRemapper::VulkanRemapper(int, bool) {
  throw std::runtime_error(
      "Vulkan not compiled; install libvulkan-dev and glslang-tools, then rebuild");
}
VulkanRemapper::~VulkanRemapper() = default;
VulkanResult VulkanRemapper::apply(const std::array<cv::Mat, 3> &, const std::array<cv::Mat, 2> &,
                                   const std::array<cv::Mat, 2> &, bool) {
  throw std::runtime_error("Vulkan not compiled");
}
nlohmann::json VulkanRemapper::info() const {
  return {{"compiled", false}};
}
nlohmann::json vulkanInfo() {
  return {{"compiled", false}, {"devices", nlohmann::json::array()}};
}
} // namespace imaging
