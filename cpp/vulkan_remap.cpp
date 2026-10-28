#include "vulkan_remap.hpp"
#include "local_remap_spirv.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>
namespace imaging {
using json = nlohmann::json;
namespace {
void vkCheck(VkResult result, const char *operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " (VkResult " + std::to_string(result) + ")");
}
double clockNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double elapsed(double start) {
  return (clockNow() - start) * 1000;
}
std::string deviceType(VkPhysicalDeviceType t) {
  switch (t) {
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    return "integrated_gpu";
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    return "discrete_gpu";
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    return "virtual_gpu";
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    return "cpu_software";
  default:
    return "other";
  }
}
bool softwareDevice(const VkPhysicalDeviceProperties &p) {
  std::string name = p.deviceName;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
         name.find("llvmpipe") != std::string::npos || name.find("lavapipe") != std::string::npos ||
         name.find("software") != std::string::npos;
}
json deviceJson(const VkPhysicalDeviceProperties &p) {
  return {{"name", p.deviceName},
          {"type", deviceType(p.deviceType)},
          {"software", softwareDevice(p)},
          {"vendor_id", p.vendorID},
          {"device_id", p.deviceID},
          {"driver_version", p.driverVersion},
          {"api_version", std::to_string(VK_VERSION_MAJOR(p.apiVersion)) + "." +
                              std::to_string(VK_VERSION_MINOR(p.apiVersion)) + "." +
                              std::to_string(VK_VERSION_PATCH(p.apiVersion))}};
}
VkInstance makeInstance() {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "MultispectralCompute";
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  vkCheck(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
  return instance;
}
std::vector<VkPhysicalDevice> devices(VkInstance instance) {
  uint32_t n = 0;
  vkCheck(vkEnumeratePhysicalDevices(instance, &n, nullptr), "Enumerate Vulkan devices");
  std::vector<VkPhysicalDevice> d(n);
  if (n)
    vkCheck(vkEnumeratePhysicalDevices(instance, &n, d.data()), "Enumerate Vulkan devices");
  return d;
}
struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  VkDeviceSize size = 0;
  bool coherent = false;
};
struct Parameters {
  uint32_t width, height, lowWidth, lowHeight, check;
};
} // namespace
struct VulkanRemapper::Impl {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet descriptors = VK_NULL_HANDLE;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool queries = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties{};
  uint32_t family = 0, validTimestampBits = 0;
  Buffer input, flows, output;
  int lowWidth;
  json information;
  explicit Impl(int width) : lowWidth(width) {}
  ~Impl() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (queries)
        vkDestroyQueryPool(device, queries, nullptr);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
      if (commandPool)
        vkDestroyCommandPool(device, commandPool, nullptr);
      if (descriptorPool)
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      if (pipelineLayout)
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      if (layout)
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
      for (Buffer *b : {&input, &flows, &output}) {
        if (b->mapped)
          vkUnmapMemory(device, b->memory);
        if (b->buffer)
          vkDestroyBuffer(device, b->buffer, nullptr);
        if (b->memory)
          vkFreeMemory(device, b->memory, nullptr);
      }
      vkDestroyDevice(device, nullptr);
    }
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
  void createBuffer(Buffer &b, VkDeviceSize size) {
    b.size = size;
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device, &ci, nullptr, &b.buffer), "Create storage buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    int chosen = -1, best = -1;
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
      auto flags = memory.memoryTypes[i].propertyFlags;
      if ((req.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        int score = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? 4 : 0) +
                    (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? 2 : 0) +
                    (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? 1 : 0);
        if (score > best) {
          best = score;
          chosen = i;
        }
      }
    }
    if (chosen < 0)
      throw std::runtime_error("No host-visible Vulkan memory");
    b.coherent = memory.memoryTypes[chosen].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = chosen;
    vkCheck(vkAllocateMemory(device, &alloc, nullptr, &b.memory), "Allocate Vulkan memory");
    vkCheck(vkBindBufferMemory(device, b.buffer, b.memory, 0), "Bind Vulkan memory");
    vkCheck(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "Map Vulkan memory");
  }
  void flush(Buffer &b) {
    if (b.coherent)
      return;
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = b.memory;
    r.offset = 0;
    r.size = VK_WHOLE_SIZE;
    vkCheck(vkFlushMappedMemoryRanges(device, 1, &r), "Flush mapped memory");
  }
  void initialize(bool allowSoftware) {
    if (lowWidth != 320 && lowWidth != 480 && lowWidth != 640)
      throw std::runtime_error("Invalid Vulkan flow width");
    instance = makeInstance();
    int score = -1;
    for (auto d : devices(instance)) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);
      bool software = softwareDevice(p);
      if (software && !allowSoftware)
        continue;
      uint32_t n = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &n, nullptr);
      std::vector<VkQueueFamilyProperties> q(n);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &n, q.data());
      for (uint32_t i = 0; i < n; i++)
        if (q[i].queueCount && (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
          int candidate = software ? 0 : 10;
          if (candidate > score) {
            score = candidate;
            physical = d;
            properties = p;
            family = i;
            validTimestampBits = q[i].timestampValidBits;
          }
          break;
        }
    }
    if (!physical)
      throw std::runtime_error(
          allowSoftware ? "No Vulkan compute device"
                        : "No hardware Vulkan compute device (software devices are excluded)");
    if (properties.limits.maxComputeWorkGroupInvocations < 64 ||
        properties.limits.maxComputeWorkGroupSize[0] < 8 ||
        properties.limits.maxComputeWorkGroupSize[1] < 8)
      throw std::runtime_error("Vulkan workgroup limits too small");
    float priority = 1;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo dc{};
    dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qi;
    vkCheck(vkCreateDevice(physical, &dc, nullptr, &device), "Create compute device");
    vkGetDeviceQueue(device, family, 0, &queue);
    createBuffer(input, 640u * 480u * sizeof(uint32_t));
    createBuffer(flows, VkDeviceSize(4) * lowWidth * (lowWidth * 3 / 4) * sizeof(cv::Vec2f));
    createBuffer(output, 640u * 480u * sizeof(uint32_t));
    if (flows.size > properties.limits.maxStorageBufferRange)
      throw std::runtime_error("Vulkan storage buffer limit too small");
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; i++) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = bindings;
    vkCheck(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout), "Create descriptor layout");
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(Parameters);
    VkPipelineLayoutCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &layout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &range;
    vkCheck(vkCreatePipelineLayout(device, &pi, nullptr, &pipelineLayout),
            "Create pipeline layout");
    VkShaderModuleCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = sizeof(local_remap_spirv);
    si.pCode = local_remap_spirv;
    VkShaderModule shader = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device, &si, nullptr, &shader), "Create compute shader");
    VkComputePipelineCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.layout = pipelineLayout;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = shader;
    cp.stage.pName = "main";
    VkResult pipelineResult =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline);
    vkDestroyShaderModule(device, shader, nullptr);
    vkCheck(pipelineResult, "Create compute pipeline");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    vkCheck(vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool),
            "Create descriptor pool");
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    vkCheck(vkAllocateDescriptorSets(device, &ai, &descriptors), "Allocate descriptor set");
    VkDescriptorBufferInfo bufferInfo[3] = {{input.buffer, 0, input.size},
                                            {flows.buffer, 0, flows.size},
                                            {output.buffer, 0, output.size}};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; i++) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = descriptors;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &bufferInfo[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    VkCommandPoolCreateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandInfo.queueFamilyIndex = family;
    vkCheck(vkCreateCommandPool(device, &commandInfo, nullptr, &commandPool),
            "Create command pool");
    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = commandPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(device, &ca, &command), "Allocate command buffer");
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCheck(vkCreateFence(device, &fi, nullptr, &fence), "Create fence");
    if (validTimestampBits && properties.limits.timestampComputeAndGraphics) {
      VkQueryPoolCreateInfo qp{};
      qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qp.queryCount = 2;
      vkCheck(vkCreateQueryPool(device, &qp, nullptr, &queries), "Create timestamp pool");
    }
    information = deviceJson(properties);
    information["compiled"] = true;
    information["timestamp_supported"] = bool(queries);
    information["host_coherent"] = input.coherent && flows.coherent && output.coherent;
  }
  VulkanResult apply(const std::array<cv::Mat, 3> &g, const std::array<cv::Mat, 2> &forward,
                     const std::array<cv::Mat, 2> &backward, bool check) {
    VulkanResult result;
    result.channels[0] = g[0];
    result.channels[1].create(480, 640, CV_8U);
    result.channels[2].create(480, 640, CV_8U);
    result.validity.create(480, 640, CV_8U);
    json timings;
    double tick = clockNow();
    for (const auto &m : g)
      if (m.type() != CV_8U || m.size() != cv::Size(640, 480))
        throw std::runtime_error("Vulkan expects 640x480 grayscale");
    auto *packed = static_cast<uint32_t *>(input.mapped);
    cv::parallel_for_(cv::Range(0, 480), [&](const cv::Range &r) {
      for (int y = r.start; y < r.end; y++) {
        const auto *a = g[0].ptr<uchar>(y);
        const auto *b = g[1].ptr<uchar>(y);
        const auto *c = g[2].ptr<uchar>(y);
        for (int x = 0; x < 640; x++)
          packed[y * 640 + x] = uint32_t(a[x]) | (uint32_t(b[x]) << 8u) | (uint32_t(c[x]) << 16u);
      }
    });
    timings["gpu_pack_ms"] = elapsed(tick);
    tick = clockNow();
    size_t planeBytes = size_t(lowWidth) * (lowWidth * 3 / 4) * sizeof(cv::Vec2f);
    auto *data = static_cast<unsigned char *>(flows.mapped);
    for (int i = 0; i < (check ? 4 : 2); i++) {
      const cv::Mat &m = i < 2 ? forward[i] : backward[i - 2];
      if (m.type() != CV_32FC2 || m.size() != cv::Size(lowWidth, lowWidth * 3 / 4))
        throw std::runtime_error("Invalid flow input for Vulkan");
      if (m.isContinuous())
        std::memcpy(data + i * planeBytes, m.data, planeBytes);
      else
        for (int y = 0; y < m.rows; y++)
          std::memcpy(data + i * planeBytes + y * m.cols * sizeof(cv::Vec2f), m.ptr(y),
                      m.cols * sizeof(cv::Vec2f));
    }
    flush(input);
    flush(flows);
    timings["gpu_upload_ms"] = elapsed(tick);
    tick = clockNow();
    vkCheck(vkResetFences(device, 1, &fence), "Reset compute fence");
    vkCheck(vkResetCommandBuffer(command, 0), "Reset compute command");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command, &begin), "Begin compute command");
    VkBufferMemoryBarrier before[3]{};
    Buffer *buffers[3] = {&input, &flows, &output};
    for (int i = 0; i < 3; i++) {
      before[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      before[i].srcAccessMask = i == 2 ? VK_ACCESS_HOST_READ_BIT : VK_ACCESS_HOST_WRITE_BIT;
      before[i].dstAccessMask = i == 2 ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
      before[i].srcQueueFamilyIndex = before[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      before[i].buffer = buffers[i]->buffer;
      before[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 3, before, 0, nullptr);
    if (queries) {
      vkCmdResetQueryPool(command, queries, 0, 2);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                            &descriptors, 0, nullptr);
    Parameters parameters{640, 480, uint32_t(lowWidth), uint32_t(lowWidth * 3 / 4),
                          check ? 1u : 0u};
    vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters),
                       &parameters);
    vkCmdDispatch(command, 80, 60, 1);
    if (queries)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
    VkBufferMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    after.srcQueueFamilyIndex = after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.buffer = output.buffer;
    after.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &after, 0, nullptr);
    vkCheck(vkEndCommandBuffer(command), "End compute command");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    vkCheck(vkQueueSubmit(queue, 1, &submit, fence), "Submit compute command");
    vkCheck(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL), "Wait compute fence");
    timings["gpu_submit_wait_ms"] = elapsed(tick);
    timings["gpu_kernel_ms"] = 0.;
    if (queries) {
      uint64_t values[2]{};
      if (vkGetQueryPoolResults(device, queries, 0, 2, sizeof(values), values, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        uint64_t mask =
            validTimestampBits >= 64 ? UINT64_MAX : ((uint64_t(1) << validTimestampBits) - 1);
        timings["gpu_kernel_ms"] =
            double((values[1] - values[0]) & mask) * properties.limits.timestampPeriod / 1e6;
      }
    }
    tick = clockNow();
    if (!output.coherent) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = output.memory;
      range.size = VK_WHOLE_SIZE;
      vkCheck(vkInvalidateMappedMemoryRanges(device, 1, &range), "Invalidate compute output");
    }
    auto *out = static_cast<const uint32_t *>(output.mapped);
    cv::parallel_for_(cv::Range(0, 480), [&](const cv::Range &r) {
      for (int y = r.start; y < r.end; y++) {
        auto *a = result.channels[1].ptr<uchar>(y);
        auto *b = result.channels[2].ptr<uchar>(y);
        auto *v = result.validity.ptr<uchar>(y);
        for (int x = 0; x < 640; x++) {
          uint32_t packed = out[y * 640 + x];
          a[x] = packed & 255u;
          b[x] = (packed >> 8u) & 255u;
          v[x] = (packed >> 16u) & 255u;
        }
      }
    });
    timings["gpu_download_ms"] = elapsed(tick);
    result.timings = timings;
    return result;
  }
};
VulkanRemapper::VulkanRemapper(int width, bool allowSoftware)
    : impl(std::make_unique<Impl>(width)) {
  impl->initialize(allowSoftware);
}
VulkanRemapper::~VulkanRemapper() = default;
VulkanResult VulkanRemapper::apply(const std::array<cv::Mat, 3> &g, const std::array<cv::Mat, 2> &f,
                                   const std::array<cv::Mat, 2> &b, bool check) {
  return impl->apply(g, f, b, check);
}
json VulkanRemapper::info() const {
  return impl->information;
}
json vulkanInfo() {
  json result = {{"compiled", true}, {"devices", json::array()}};
  VkInstance instance = VK_NULL_HANDLE;
  try {
    instance = makeInstance();
    for (auto d : devices(instance)) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);
      auto data = deviceJson(p);
      uint32_t n = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &n, nullptr);
      std::vector<VkQueueFamilyProperties> families(n);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &n, families.data());
      bool compute = false;
      for (auto &q : families)
        compute |= q.queueCount && (q.queueFlags & VK_QUEUE_COMPUTE_BIT);
      data["compute"] = compute;
      result["devices"].push_back(data);
    }
    vkDestroyInstance(instance, nullptr);
  } catch (const std::exception &e) {
    if (instance)
      vkDestroyInstance(instance, nullptr);
    result["error"] = e.what();
  }
  return result;
}
} // namespace imaging
