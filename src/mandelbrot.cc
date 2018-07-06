#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vulkan/vulkan.hpp>
#include "lodepng.h"
#include "vulkan_ext.h"

using namespace std::string_literals;

const char *kAppShortName = "Mandelbrot";

struct Pixel {
  float r, g, b, a;
};

const int kWidth = 3200;
const int kHeight = 2400;
const int kWorkgroupSize = 32;
const size_t buffer_size = sizeof(Pixel) * kWidth * kHeight;

const char kValidationLayer[] = "VK_LAYER_LUNARG_standard_validation";
const char kDebugReportExtension[] = "VK_EXT_debug_report";

class MandelbrotApp {
 public:
  MandelbrotApp() = default;

  ~MandelbrotApp() = default;

  void Run() {
    ProbeInstallation();
    CreateInstance();
    RegisterDebugReportCallback();
    GetPhysicalDevice();
    FindQueueFamily();
    CreateLogicalDevice();
    GetQueue();
    CreateBuffer();
    AllocateDeviceMemory();
    BindDeviceMemory();
    CreateDescriptorSetLayout();
    CreateDescriptorPool();
    CreateDescriptorSets();
    ConnectBufferWithDescriptorSets();
    CreateShaderModule("shaders/comp.spv");
    CreatePipeline();
    CreateCommandPool();
    CreateCommandBuffers();
    FillCommandBuffer();
    SubmitAndWait();
    SaveRenderedImage("mandelbrot.png");
  }

  void ProbeInstallation() {
    std::vector<vk::LayerProperties> layer_props =
        vk::enumerateInstanceLayerProperties();
    std::cerr << "We have " << layer_props.size()
              << " available validation layers" << std::endl;
    for (const auto &layer_property : layer_props) {
      std::cerr << "  " << layer_property.layerName << "\t\t"
                << layer_property.description << std::endl;
      if (std::string(layer_property.layerName) == kValidationLayer) {
        enabled_layers_.push_back(kValidationLayer);
      }
    }
    if (enabled_layers_.empty()) {
      std::cerr << "WARNING: " << kValidationLayer << " layer not available." << std::endl;
    }
    std::vector<vk::ExtensionProperties> extension_props =
        vk::enumerateInstanceExtensionProperties();
    std::cerr << "We have " << extension_props.size() << " available extensions"
              << std::endl;
    for (const auto &extension_prop : extension_props) {
      std::cerr << "  " << extension_prop.extensionName << std::endl;
      if (std::string(extension_prop.extensionName) == kDebugReportExtension) {
        enabled_extensions_.push_back(kDebugReportExtension);
      }
    }
    if (enabled_extensions_.empty()) {
      std::cerr << "WARNING: " << kDebugReportExtension << " extension not available." << std::endl;
    }
  }

  void CreateInstance() {
    auto app_info = vk::ApplicationInfo();
    app_info.setPApplicationName(kAppShortName)
        .setApplicationVersion(1)
        .setPEngineName(kAppShortName)
        .setEngineVersion(1)
        .setApiVersion(VK_API_VERSION_1_0);
    auto inst_info = vk::InstanceCreateInfo();
    inst_info.setPApplicationInfo(&app_info)
        .setEnabledLayerCount(enabled_layers_.size())
        .setPpEnabledLayerNames(enabled_layers_.data())
        .setEnabledExtensionCount(enabled_extensions_.size())
        .setPpEnabledExtensionNames(enabled_extensions_.data());
    instance_ = vk::createInstanceUnique(inst_info);
    vkExtInitInstance(*instance_);
  }

  void RegisterDebugReportCallback() {
    auto create_info = vk::DebugReportCallbackCreateInfoEXT();
    create_info
        .setFlags(vk::DebugReportFlagBitsEXT::eInformation |
                  vk::DebugReportFlagBitsEXT::eWarning |
                  vk::DebugReportFlagBitsEXT::ePerformanceWarning |
                  vk::DebugReportFlagBitsEXT::eError)
        .setPfnCallback(DebugReportCallback);
    debug_report_callback_ =
        instance_->createDebugReportCallbackEXTUnique(create_info);
  }

  void GetPhysicalDevice() {
    auto devices = instance_->enumeratePhysicalDevices();
    if (devices.empty()) {
      throw std::runtime_error("No physical devices found.");
    }
    std::cerr << "Found " << devices.size() << " physical device(s)."
              << std::endl;
    for (const auto &device : devices) {
      std::cerr << "  " << device.getProperties().deviceName << " - "
                << vk::to_string(device.getProperties().deviceType)
                << std::endl;
    }
    physical_device_ = devices[0];
  }

  void FindQueueFamily() {
    auto families = physical_device_.getQueueFamilyProperties();
    std::cerr << "Device contains " << families.size() << " queue family(ies)."
              << std::endl;
    for (const auto &family : families) {
      std::cerr << "  " << family.queueCount << " queue(s) with flags "
                << vk::to_string(family.queueFlags) << std::endl;
    }
    queue_family_index_ = FindQueueFamilyIndex(families);
  }

  void CreateLogicalDevice() {
    const float queue_priorities[1] = {0.0};
    auto queue_info = vk::DeviceQueueCreateInfo();
    queue_info.setQueueFamilyIndex(queue_family_index_)
        .setQueueCount(1)
        .setPQueuePriorities(queue_priorities);
    auto device_info = vk::DeviceCreateInfo();
    device_info.setQueueCreateInfoCount(1).setPQueueCreateInfos(&queue_info);
    device_ = physical_device_.createDeviceUnique(device_info);
  }

  void GetQueue() { queue_ = device_->getQueue(queue_family_index_, 0); }

  void CreateBuffer() {
    auto buffer_create_info = vk::BufferCreateInfo();
    buffer_create_info.setSize(buffer_size)
        .setUsage(vk::BufferUsageFlagBits::eStorageBuffer)
        .setSharingMode(vk::SharingMode::eExclusive);
    buffer_ = device_->createBufferUnique(buffer_create_info);
  }

  void AllocateDeviceMemory() {
    auto memory_requirements = device_->getBufferMemoryRequirements(*buffer_);
    uint32_t memory_type_index =
        FindMemoryType(memory_requirements.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eHostCoherent |
                           vk::MemoryPropertyFlagBits::eHostVisible);
    auto allocate_info = vk::MemoryAllocateInfo();
    allocate_info.setAllocationSize(memory_requirements.size)
        .setMemoryTypeIndex(memory_type_index);
    buffer_memory_ = device_->allocateMemoryUnique(allocate_info);
  }

  void BindDeviceMemory() {
    device_->bindBufferMemory(*buffer_, *buffer_memory_, 0);
  }

  void CreateDescriptorSetLayout() {
    auto descriptor_set_layout_binding = vk::DescriptorSetLayoutBinding();
    descriptor_set_layout_binding
        .setDescriptorType(vk::DescriptorType::eStorageBuffer)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eCompute);
    auto descriptor_set_layout_create_info =
        vk::DescriptorSetLayoutCreateInfo();
    descriptor_set_layout_create_info.setBindingCount(1).setPBindings(
        &descriptor_set_layout_binding);
    descriptor_set_layout_ = device_->createDescriptorSetLayoutUnique(
        descriptor_set_layout_create_info);
  }

  void CreateDescriptorPool() {
    auto descriptor_pool_size = vk::DescriptorPoolSize();
    descriptor_pool_size.setType(vk::DescriptorType::eStorageBuffer)
        .setDescriptorCount(1);
    auto descriptor_pool_create_info = vk::DescriptorPoolCreateInfo();
    descriptor_pool_create_info.setMaxSets(1).setPoolSizeCount(1).setPPoolSizes(
        &descriptor_pool_size);
    descriptor_pool_ =
        device_->createDescriptorPoolUnique(descriptor_pool_create_info);
  }

  void CreateDescriptorSets() {
    auto descriptor_set_allocate_info = vk::DescriptorSetAllocateInfo();
    descriptor_set_allocate_info.setDescriptorPool(*descriptor_pool_)
        .setDescriptorSetCount(1)
        .setPSetLayouts(&descriptor_set_layout_.get());
    descriptor_sets_ =
        device_->allocateDescriptorSets(descriptor_set_allocate_info);
  }

  void ConnectBufferWithDescriptorSets() {
    auto descriptor_buffer_info = vk::DescriptorBufferInfo();
    descriptor_buffer_info.setBuffer(*buffer_).setOffset(0).setRange(
        buffer_size);
    auto write_descriptor_set = vk::WriteDescriptorSet();
    write_descriptor_set.setDstSet(descriptor_sets_[0])
        .setDstBinding(0)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageBuffer)
        .setPBufferInfo(&descriptor_buffer_info);
    device_->updateDescriptorSets({write_descriptor_set}, {});
  }

  void CreateShaderModule(const char *shader_filename) {
    auto code = ReadFile(shader_filename);
    auto shader_create_info = vk::ShaderModuleCreateInfo();
    shader_create_info.setPCode(code.data()).setCodeSize(code.size());
    compute_shader_module_ =
        device_->createShaderModuleUnique(shader_create_info);
  }

  void CreatePipeline() {
    auto shader_stage_create_info = vk::PipelineShaderStageCreateInfo();
    shader_stage_create_info.setStage(vk::ShaderStageFlagBits::eCompute)
        .setModule(*compute_shader_module_)
        .setPName("main");
    auto pipeline_layout_create_info = vk::PipelineLayoutCreateInfo();
    pipeline_layout_create_info.setSetLayoutCount(1).setPSetLayouts(
        &descriptor_set_layout_.get());
    pipeline_layout_ =
        device_->createPipelineLayoutUnique(pipeline_layout_create_info);
    auto pipeline_create_info = vk::ComputePipelineCreateInfo();
    pipeline_create_info.setStage(shader_stage_create_info)
        .setLayout(*pipeline_layout_);
    pipeline_ = device_->createComputePipelineUnique({}, pipeline_create_info);
  }

  void CreateCommandPool() {
    auto command_pool_info = vk::CommandPoolCreateInfo();
    command_pool_info.setQueueFamilyIndex(0);
    command_pool_ = device_->createCommandPoolUnique(command_pool_info);
  }

  void CreateCommandBuffers() {
    auto buffer_info = vk::CommandBufferAllocateInfo();
    buffer_info.setCommandPool(*command_pool_)
        .setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandBufferCount(1);
    command_buffers_ = device_->allocateCommandBuffersUnique(buffer_info);
  }

  void FillCommandBuffer() {
    /* Start recording commands into the command buffer */
    auto begin_info = vk::CommandBufferBeginInfo();
    begin_info.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    command_buffers_[0]->begin(begin_info);

    /* Bind pipeline and descriptor set. */
    command_buffers_[0]->bindPipeline(vk::PipelineBindPoint::eCompute,
                                      *pipeline_);
    command_buffers_[0]->bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                            *pipeline_layout_, 0,
                                            descriptor_sets_, {});

    /* Dispatch commands */
    command_buffers_[0]->dispatch(
        (uint32_t)std::ceil(kWidth / float(kWorkgroupSize)),
        (uint32_t)std::ceil(kHeight / float(kWorkgroupSize)), 1);

    /* Stop recording commands. */
    command_buffers_[0]->end();
  }

  void SubmitAndWait() {
    /* Submit recorded command buffer to a queue. */
    auto submit_info = vk::SubmitInfo();
    submit_info.setCommandBufferCount(1).setPCommandBuffers(
        &command_buffers_[0].get());

    /* Create a fence */
    auto fence = device_->createFenceUnique({});

    /* Submit the command buffer to the queue. */
    queue_.submit({submit_info}, *fence);

    /* Wait for the fence */
    device_->waitForFences({*fence}, VK_TRUE, 100000000000);
  }

  void SaveRenderedImage(const char *outfilename) {
    auto pixel_data = static_cast<Pixel *>(
        device_->mapMemory(*buffer_memory_, 0, buffer_size, {}));
    std::vector<unsigned char> image;
    image.reserve(kWidth * kHeight * 4);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      image.push_back(static_cast<unsigned char>(255.0f * (pixel_data[i].r)));
      image.push_back(static_cast<unsigned char>(255.0f * (pixel_data[i].g)));
      image.push_back(static_cast<unsigned char>(255.0f * (pixel_data[i].b)));
      image.push_back(static_cast<unsigned char>(255.0f * (pixel_data[i].a)));
    }
    device_->unmapMemory(*buffer_memory_);
    unsigned error = lodepng::encode(outfilename, image, kWidth, kHeight);
    if (error) {
      throw std::runtime_error("Encoding error: "s + lodepng_error_text(error));
    }
  }

  static VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(
      VkDebugReportFlagsEXT /* flags */,
      VkDebugReportObjectTypeEXT /* objectType */, uint64_t /* object */,
      size_t /* location */, int32_t /* messageCode */,
      const char *pLayerPrefix, const char *pMessage, void * /* pUserData */) {
    std::cerr << "\033[1;36m" << pLayerPrefix << ": "
              << "\033[0m" << pMessage << std::endl;
    return VK_FALSE;
  }

  uint32_t FindMemoryType(int32_t memory_type_bits,
                          const vk::MemoryPropertyFlags &properties) {
    auto memory_properties = physical_device_.getMemoryProperties();
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((memory_type_bits & (1 << i)) and
          ((memory_properties.memoryTypes[i].propertyFlags & properties) ==
           properties)) {
        return i;
      }
    }
    return -1;
  }

  uint32_t FindQueueFamilyIndex(
      const std::vector<vk::QueueFamilyProperties> &queue_families) {
    for (uint32_t i = 0; i < queue_families.size(); ++i) {
      if (queue_families[i].queueFlags & vk::QueueFlagBits::eCompute) {
        return i;
      }
    }
    throw std::runtime_error(
        "Could not find a queue family with compute capabilities.");
  }

  static std::vector<uint32_t> ReadFile(const char *filename) {
    std::ifstream infile(filename, std::ifstream::binary | std::ifstream::ate);
    if (not infile.good()) {
      throw std::runtime_error(std::string(filename) + ": no such file.");
    }
    size_t file_size = infile.tellg();
    size_t file_size_padded = size_t(std::ceil(file_size / 4.0)) * 4;
    infile.seekg(0);
    std::vector<uint32_t> data(file_size_padded, 0);
    infile.read(reinterpret_cast<char *>(data.data()), file_size);
    return data;
  }

 private:
  std::vector<const char *> enabled_layers_;
  std::vector<const char *> enabled_extensions_;

  vk::UniqueInstance instance_;
  vk::UniqueDebugReportCallbackEXT debug_report_callback_;

  vk::PhysicalDevice physical_device_;

  uint32_t queue_family_index_;
  vk::UniqueDevice device_;
  vk::Queue queue_;

  vk::UniqueBuffer buffer_;
  vk::UniqueDeviceMemory buffer_memory_;

  vk::UniqueDescriptorSetLayout descriptor_set_layout_;
  vk::UniqueDescriptorPool descriptor_pool_;
  std::vector<vk::DescriptorSet> descriptor_sets_;

  vk::UniqueShaderModule compute_shader_module_;
  vk::UniquePipelineLayout pipeline_layout_;
  vk::UniquePipeline pipeline_;

  vk::UniqueCommandPool command_pool_;
  std::vector<vk::UniqueCommandBuffer> command_buffers_;
};

int main() {
  MandelbrotApp app;
  try {
    app.Run();
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
  return 0;
}
