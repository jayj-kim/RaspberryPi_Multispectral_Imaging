#pragma once
#include <array>
#include <memory>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
namespace imaging {
struct VulkanResult {
  std::array<cv::Mat, 3> channels;
  cv::Mat validity;
  nlohmann::json timings;
};
class VulkanRemapper {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  explicit VulkanRemapper(int lowWidth, bool allowSoftware = false);
  ~VulkanRemapper();
  VulkanRemapper(const VulkanRemapper &) = delete;
  VulkanRemapper &operator=(const VulkanRemapper &) = delete;
  VulkanResult apply(const std::array<cv::Mat, 3> &, const std::array<cv::Mat, 2> &forward,
                     const std::array<cv::Mat, 2> &backward, bool check);
  nlohmann::json info() const;
};
nlohmann::json vulkanInfo();
} // namespace imaging
