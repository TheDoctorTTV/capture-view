#include "frame_generator.hpp"

#include "log.hpp"
#include "nvidia_runtime.hpp"
#include "vk_device.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#if CAPTURE_VIEW_HAVE_NGX
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

namespace cv {

namespace {

constexpr uint32_t kFlowLuma[] = {
#include "flow_luma.comp.spv.inc"
};
constexpr uint32_t kFlowSearch[] = {
#include "flow_search.comp.spv.inc"
};
constexpr uint32_t kFlowMedian[] = {
#include "flow_median.comp.spv.inc"
};

// Motion is estimated on a 4-level luma pyramid starting at half resolution.
constexpr uint32_t kLevels = 4;

struct LumaPush {
  int32_t src_size[2];
  int32_t dst_size[2];
  uint32_t mode;
  uint32_t factor;
};

struct SearchPush {
  int32_t size[2];
  int32_t coarse_size[2];
  int32_t has_coarse;
  int32_t radius;
  int32_t window;
  int32_t subpixel;
  float smoothness;
};

struct MedianPush {
  int32_t size[2];
};

#if CAPTURE_VIEW_HAVE_NGX
// GUID-like project id required for custom engines.
constexpr const char* kProjectId = "6c1f3d2a-8b5e-4f9a-9c27-3e8d5b1a7f40";

std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// NGX reports a lot of harmless diagnostics (optional config files it did not
// find, the system-wide model folder it may not write to). Only keep them with
// --verbose; capture-view logs NGX result codes itself when something fails.
void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string line = message != nullptr ? message : "";
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  log::debug("ngx: ", line);
}

struct NgxPaths {
  std::wstring data_dir;
  const wchar_t* list[1]{};
  NVSDK_NGX_FeatureCommonInfo info{};

  NgxPaths() {
    data_dir = widen(nvidia_runtime_dir().string());
    list[0] = data_dir.c_str();
    info.PathListInfo.Path = list;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
  }
};

NgxPaths& ngx_paths() {
  static NgxPaths paths;
  return paths;
}

NVSDK_NGX_FeatureDiscoveryInfo discovery_info() {
  NVSDK_NGX_FeatureDiscoveryInfo info{};
  info.SDKVersion = NVSDK_NGX_Version_API;
  info.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
  info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
  info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
  info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  info.Identifier.v.ProjectDesc.EngineVersion = "1.0";
  info.ApplicationDataPath = ngx_paths().data_dir.c_str();
  info.FeatureInfo = &ngx_paths().info;
  return info;
}

std::string ngx_result_string(NVSDK_NGX_Result result) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(result));
  return buffer;
}
#endif

} // namespace

void frame_generation_instance_extensions(std::vector<std::string>& out) {
#if CAPTURE_VIEW_HAVE_NGX
  const NVSDK_NGX_FeatureDiscoveryInfo info = discovery_info();
  uint32_t count = 0;
  VkExtensionProperties* extensions = nullptr;
  if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &extensions))) {
    for (uint32_t i = 0; i < count; ++i) {
      out.emplace_back(extensions[i].extensionName);
    }
  }
#else
  (void)out;
#endif
}

bool frame_generation_device_extensions(VkInstance instance, VkPhysicalDevice physical,
                                        std::vector<std::string>& out, std::string& reason) {
#if CAPTURE_VIEW_HAVE_NGX
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical, &props);
  if (props.vendorID != 0x10DE) {
    reason = "DLSS Frame Generation needs an NVIDIA RTX GPU";
    return false;
  }
  const NVSDK_NGX_FeatureDiscoveryInfo info = discovery_info();
  NVSDK_NGX_FeatureRequirement requirement{};
  const NVSDK_NGX_Result req = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &info, &requirement);
  if (NVSDK_NGX_SUCCEED(req) && requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
    reason = "this GPU or driver does not support DLSS Frame Generation (requirement flags " +
             std::to_string(static_cast<unsigned>(requirement.FeatureSupported)) + ")";
    if ((requirement.FeatureSupported & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) != 0) {
      reason = "the NVIDIA driver is too old for DLSS Frame Generation";
    } else if ((requirement.FeatureSupported & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) != 0) {
      reason = "DLSS Frame Generation needs a GeForce RTX 40 series or newer GPU";
    }
    return false;
  }
  uint32_t count = 0;
  VkExtensionProperties* extensions = nullptr;
  const NVSDK_NGX_Result result =
      NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &info, &count, &extensions);
  if (NVSDK_NGX_FAILED(result)) {
    reason = "NVIDIA NGX is unavailable (" + ngx_result_string(result) + "); is the NVIDIA driver installed?";
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    out.emplace_back(extensions[i].extensionName);
  }
  return true;
#else
  (void)instance;
  (void)physical;
  (void)out;
  reason = "this build of capture-view was compiled without DLSS Frame Generation support";
  return false;
#endif
}

struct FrameGenerator::Impl {
  vk::ComputePipeline luma;
  vk::ComputePipeline search;
  vk::ComputePipeline median;

  uint32_t width = 0;
  uint32_t height = 0;
  // Motion is estimated from 1/base_factor resolution: 2 up to 1080p, 4 above
  // (DLSS-G refines the vectors with its own optical flow).
  uint32_t base_factor = 2;
  std::array<uint32_t, kLevels> level_w{};
  std::array<uint32_t, kLevels> level_h{};
  std::array<std::array<vk::Image, kLevels>, 2> pyramid{}; // [parity][level], r32f
  std::array<vk::Image, kLevels> flow{};                     // rg32f
  vk::Image mvec;                                            // rg16f, level 0 size
  vk::Image depth;                                           // r32f, constant zero
  uint32_t parity = 0;
  uint32_t generated_count = 0; // generated-frame images currently allocated
  bool have_previous = false;
  uint64_t frame_id = 0;

#if CAPTURE_VIEW_HAVE_NGX
  bool ngx_initialised = false;
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Handle* handle = nullptr;
#endif
};

FrameGenerator::FrameGenerator(const vk::Gpu& gpu) : gpu_(gpu), impl_(std::make_unique<Impl>()) {
  const VkDescriptorType image = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  const VkDescriptorType luma_bindings[] = {image, image, image};
  const VkDescriptorType search_bindings[] = {image, image, image, image};
  const VkDescriptorType median_bindings[] = {image, image};
  impl_->luma = vk::create_compute_pipeline(gpu_, kFlowLuma, luma_bindings, sizeof(LumaPush));
  impl_->search = vk::create_compute_pipeline(gpu_, kFlowSearch, search_bindings, sizeof(SearchPush));
  impl_->median = vk::create_compute_pipeline(gpu_, kFlowMedian, median_bindings, sizeof(MedianPush));

#if CAPTURE_VIEW_HAVE_NGX
  const auto runtime = nvidia_runtime_status(false);
  if (!runtime.installed) {
    caps_.reason = "the NVIDIA DLSS Frame Generation runtime is not downloaded yet";
    return;
  }
  NgxPaths& paths = ngx_paths();
  const NVSDK_NGX_Result init = NVSDK_NGX_VULKAN_Init_with_ProjectID(
      kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", paths.data_dir.c_str(), gpu_.instance, gpu_.physical,
      gpu_.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &paths.info, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(init)) {
    caps_.reason = "NVIDIA NGX failed to initialise (" + ngx_result_string(init) + ")";
    log::warning("frame generation: ", caps_.reason);
    return;
  }
  impl_->ngx_initialised = true;

  const NVSDK_NGX_Result caps_result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&impl_->params);
  if (NVSDK_NGX_FAILED(caps_result) || impl_->params == nullptr) {
    caps_.reason = "could not query NGX capabilities (" + ngx_result_string(caps_result) + ")";
    return;
  }
  int available = 0;
  const NVSDK_NGX_Result got = impl_->params->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
  unsigned int needs_driver = 0;
  impl_->params->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
  int init_result = 0;
  impl_->params->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init_result);
  unsigned int max_count = 0;
  const NVSDK_NGX_Result got_max = impl_->params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &max_count);
  log::info("frame generation: available=", available, " multi_frame_count_max=", max_count,
            " needs_driver=", needs_driver, " init_result=", ngx_result_string(static_cast<NVSDK_NGX_Result>(init_result)));
  if (NVSDK_NGX_FAILED(got) || available == 0) {
    caps_.reason = needs_driver != 0 ? "the NVIDIA driver is too old for DLSS Frame Generation"
                                     : "DLSS Frame Generation is not available on this GPU";
    return;
  }
  caps_.available = true;
  const uint32_t generated = NVSDK_NGX_SUCCEED(got_max) && max_count >= 1 ? max_count : 1;
  caps_.max_multiplier = std::min<uint32_t>(generated + 1, kMaxMultiplier);
#else
  caps_.reason = "this build of capture-view was compiled without DLSS Frame Generation support";
#endif
}

FrameGenerator::~FrameGenerator() {
  vkDeviceWaitIdle(gpu_.device);
  release_size_dependent();
#if CAPTURE_VIEW_HAVE_NGX
  if (impl_->params != nullptr) {
    NVSDK_NGX_VULKAN_DestroyParameters(impl_->params);
  }
  if (impl_->ngx_initialised) {
    NVSDK_NGX_VULKAN_Shutdown1(gpu_.device);
  }
#endif
  vk::destroy_pipeline(gpu_, impl_->luma);
  vk::destroy_pipeline(gpu_, impl_->search);
  vk::destroy_pipeline(gpu_, impl_->median);
}

const vk::Image& FrameGenerator::motion_vectors() const { return impl_->mvec; }

void FrameGenerator::release_size_dependent() {
#if CAPTURE_VIEW_HAVE_NGX
  if (impl_->handle != nullptr) {
    NVSDK_NGX_VULKAN_ReleaseFeature(impl_->handle);
    impl_->handle = nullptr;
  }
#endif
  for (auto& parity : impl_->pyramid) {
    for (auto& image : parity) {
      vk::destroy_image(gpu_, image);
    }
  }
  for (auto& image : impl_->flow) {
    vk::destroy_image(gpu_, image);
  }
  vk::destroy_image(gpu_, impl_->mvec);
  vk::destroy_image(gpu_, impl_->depth);
  for (auto& image : generated_) {
    vk::destroy_image(gpu_, image);
  }
  impl_->width = 0;
  impl_->height = 0;
  impl_->generated_count = 0;
  impl_->have_previous = false;
}

bool FrameGenerator::configure(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t multiplier) {
  if (!caps_.available) {
    last_error_ = caps_.reason;
    return false;
  }
  const uint32_t wanted =
      std::clamp<uint32_t>(std::min(multiplier, caps_.max_multiplier), 2, kMaxMultiplier) - 1;
  const auto allocate_generated = [&](uint32_t count) {
    // Generated frames are full-size RGBA images; only allocate what the
    // chosen multiplier needs (2x needs one, 6x five).
    for (uint32_t i = 0; i < generated_.size(); ++i) {
      if (i < count && generated_[i].image == VK_NULL_HANDLE) {
        generated_[i] = vk::create_image(gpu_, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        vk::init_general_layout(cmd, generated_[i]);
      }
    }
  };
  if (impl_->width == width && impl_->height == height) {
    if (wanted > impl_->generated_count) {
      allocate_generated(wanted);
      impl_->generated_count = wanted;
      vk::full_barrier(cmd);
    }
    return true;
  }
  vkDeviceWaitIdle(gpu_.device);
  release_size_dependent();

  impl_->width = width;
  impl_->height = height;
  impl_->base_factor = width > 2048 ? 4U : 2U;
  uint32_t w = (width + impl_->base_factor - 1) / impl_->base_factor;
  uint32_t h = (height + impl_->base_factor - 1) / impl_->base_factor;
  for (uint32_t level = 0; level < kLevels; ++level) {
    impl_->level_w[level] = std::max(w, 1U);
    impl_->level_h[level] = std::max(h, 1U);
    w = (w + 1) / 2;
    h = (h + 1) / 2;
  }

  const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  for (auto& parity : impl_->pyramid) {
    for (uint32_t level = 0; level < kLevels; ++level) {
      parity[level] = vk::create_image(gpu_, impl_->level_w[level], impl_->level_h[level], VK_FORMAT_R32_SFLOAT, storage);
      vk::init_general_layout(cmd, parity[level]);
    }
  }
  for (uint32_t level = 0; level < kLevels; ++level) {
    impl_->flow[level] =
        vk::create_image(gpu_, impl_->level_w[level], impl_->level_h[level], VK_FORMAT_R32G32_SFLOAT, storage);
    vk::init_general_layout(cmd, impl_->flow[level]);
  }
  impl_->mvec = vk::create_image(gpu_, impl_->level_w[0], impl_->level_h[0], VK_FORMAT_R16G16_SFLOAT,
                                 storage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  vk::init_general_layout(cmd, impl_->mvec);
  impl_->depth = vk::create_image(gpu_, impl_->level_w[0], impl_->level_h[0], VK_FORMAT_R32_SFLOAT,
                                  storage | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  vk::init_general_layout(cmd, impl_->depth);
  allocate_generated(wanted);
  impl_->generated_count = wanted;
  vk::full_barrier(cmd);
  const VkClearColorValue zero{};
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, impl_->depth.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  vk::full_barrier(cmd);

#if CAPTURE_VIEW_HAVE_NGX
  NVSDK_NGX_Parameter* params = impl_->params;
  const unsigned int always = NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer | NVSDK_NGX_DLSSG_ResourceFlags_MVecs |
                              NVSDK_NGX_DLSSG_ResourceFlags_Depth;
  const unsigned int never = NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI |
                             NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
                             NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField;
  params->Set(NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, always);
  params->Set(NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, never);

  // NGX_VK_CREATE_DLSSG still sets the generic Width/Height; the DLSSG-specific
  // names are what current runtimes read (avoids a deprecation warning).
  params->Set(NVSDK_NGX_DLSSG_Parameter_Width, width);
  params->Set(NVSDK_NGX_DLSSG_Parameter_Height, height);
  NVSDK_NGX_DLSSG_Create_Params create{};
  create.Width = width;
  create.Height = height;
  create.NativeBackbufferFormat = VK_FORMAT_R8G8B8A8_UNORM;
  create.RenderWidth = impl_->level_w[0];
  create.RenderHeight = impl_->level_h[0];
  create.DynamicResolutionScaling = false;
  const NVSDK_NGX_Result result = NGX_VK_CREATE_DLSSG(cmd, 1, 1, &impl_->handle, params, &create);
  log::info("frame generation: create ", width, "x", height, " result=", ngx_result_string(result));
  if (NVSDK_NGX_FAILED(result) || impl_->handle == nullptr) {
    last_error_ = "DLSS Frame Generation could not start (" + ngx_result_string(result) + ")";
    impl_->handle = nullptr;
    caps_.available = false;
    caps_.reason = last_error_;
    return false;
  }
  vk::full_barrier(cmd);
  return true;
#else
  last_error_ = caps_.reason;
  return false;
#endif
}

uint32_t FrameGenerator::record(VkCommandBuffer cmd, const vk::Image& current, uint32_t multiplier, bool reset) {
#if CAPTURE_VIEW_HAVE_NGX
  Impl& d = *impl_;
  if (d.handle == nullptr || multiplier < 2) {
    return 0;
  }
  multiplier = std::min({multiplier, caps_.max_multiplier, d.generated_count + 1});
  const uint32_t cur = d.parity;
  const uint32_t prev = 1 - d.parity;
  const bool use_previous = d.have_previous && !reset;

  auto image_info = [](const vk::Image& image) {
    return VkDescriptorImageInfo{VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_GENERAL};
  };

  // 1. Luma pyramid of the current frame.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.luma.pipeline);
  for (uint32_t level = 0; level < kLevels; ++level) {
    const vk::Image& src_luma = level == 0 ? d.pyramid[cur][0] : d.pyramid[cur][level - 1];
    VkDescriptorImageInfo infos[3] = {image_info(current), image_info(src_luma), image_info(d.pyramid[cur][level])};
    VkWriteDescriptorSet writes[3] = {vk::storage_image_write(0, &infos[0]), vk::storage_image_write(1, &infos[1]),
                                      vk::storage_image_write(2, &infos[2])};
    gpu_.push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.luma.layout, 0, 3, writes);
    LumaPush push{};
    push.src_size[0] = static_cast<int32_t>(level == 0 ? d.width : d.level_w[level - 1]);
    push.src_size[1] = static_cast<int32_t>(level == 0 ? d.height : d.level_h[level - 1]);
    push.dst_size[0] = static_cast<int32_t>(d.level_w[level]);
    push.dst_size[1] = static_cast<int32_t>(d.level_h[level]);
    push.mode = level == 0 ? 0U : 1U;
    push.factor = d.base_factor;
    vkCmdPushConstants(cmd, d.luma.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, vk::group_count(d.level_w[level], 8), vk::group_count(d.level_h[level], 8), 1);
    vk::full_barrier(cmd);
  }

  // 2. Coarse-to-fine motion search current -> previous.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.search.pipeline);
  for (int level = static_cast<int>(kLevels) - 1; level >= 0; --level) {
    const auto l = static_cast<uint32_t>(level);
    const bool coarsest = l == kLevels - 1;
    const vk::Image& previous = use_previous ? d.pyramid[prev][l] : d.pyramid[cur][l];
    const vk::Image& coarse = coarsest ? d.flow[l] : d.flow[l + 1];
    VkDescriptorImageInfo infos[4] = {image_info(d.pyramid[cur][l]), image_info(previous), image_info(coarse),
                                      image_info(d.flow[l])};
    VkWriteDescriptorSet writes[4] = {vk::storage_image_write(0, &infos[0]), vk::storage_image_write(1, &infos[1]),
                                      vk::storage_image_write(2, &infos[2]), vk::storage_image_write(3, &infos[3])};
    gpu_.push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.search.layout, 0, 4, writes);
    SearchPush push{};
    push.size[0] = static_cast<int32_t>(d.level_w[l]);
    push.size[1] = static_cast<int32_t>(d.level_h[l]);
    push.coarse_size[0] = static_cast<int32_t>(coarsest ? d.level_w[l] : d.level_w[l + 1]);
    push.coarse_size[1] = static_cast<int32_t>(coarsest ? d.level_h[l] : d.level_h[l + 1]);
    push.has_coarse = coarsest ? 0 : 1;
    push.radius = coarsest ? 4 : (l == 0 ? 1 : 2);
    push.window = coarsest ? 3 : 2;
    push.subpixel = l == 0 ? 1 : 0;
    push.smoothness = 0.002F;
    vkCmdPushConstants(cmd, d.search.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, vk::group_count(d.level_w[l], 8), vk::group_count(d.level_h[l], 8), 1);
    vk::full_barrier(cmd);
  }

  // 3. Median filter into the DLSS-G motion vector texture.
  {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.median.pipeline);
    VkDescriptorImageInfo infos[2] = {image_info(d.flow[0]), image_info(d.mvec)};
    VkWriteDescriptorSet writes[2] = {vk::storage_image_write(0, &infos[0]), vk::storage_image_write(1, &infos[1])};
    gpu_.push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.median.layout, 0, 2, writes);
    MedianPush push{};
    push.size[0] = static_cast<int32_t>(d.level_w[0]);
    push.size[1] = static_cast<int32_t>(d.level_h[0]);
    vkCmdPushConstants(cmd, d.median.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, vk::group_count(d.level_w[0], 8), vk::group_count(d.level_h[0], 8), 1);
    vk::full_barrier(cmd);
  }

  // 4. DLSS-G evaluations, one per generated frame.
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  NVSDK_NGX_Resource_VK backbuffer = NVSDK_NGX_Create_ImageView_Resource_VK(
      current.view, current.image, range, current.format, current.width, current.height, false);
  NVSDK_NGX_Resource_VK depth = NVSDK_NGX_Create_ImageView_Resource_VK(
      d.depth.view, d.depth.image, range, d.depth.format, d.depth.width, d.depth.height, false);
  NVSDK_NGX_Resource_VK mvecs = NVSDK_NGX_Create_ImageView_Resource_VK(
      d.mvec.view, d.mvec.image, range, d.mvec.format, d.mvec.width, d.mvec.height, false);

  const uint32_t count = multiplier - 1;
  ++d.frame_id;
  d.params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, static_cast<unsigned long long>(d.frame_id));
  bool failed = false;
  for (uint32_t index = 1; index <= count; ++index) {
    vk::Image& out = generated_[index - 1];
    NVSDK_NGX_Resource_VK output =
        NVSDK_NGX_Create_ImageView_Resource_VK(out.view, out.image, range, out.format, out.width, out.height, true);

    NVSDK_NGX_VK_DLSSG_Eval_Params eval{};
    eval.pBackbuffer = &backbuffer;
    eval.pDepth = &depth;
    eval.pMVecs = &mvecs;
    eval.pOutputInterpFrame = &output;

    NVSDK_NGX_DLSSG_Opt_Eval_Params opt{};
    opt.multiFrameCount = count;
    opt.multiFrameIndex = index;
    // Stationary pinhole camera: all temporal change lives in the motion
    // vectors, but the matrices must still be consistent with each other.
    const float aspect = static_cast<float>(d.width) / static_cast<float>(d.height);
    const float near_plane = 0.1F;
    const float far_plane = 1000.0F;
    const float pz = far_plane / (far_plane - near_plane);
    const float pw = -near_plane * pz;
    std::memset(opt.cameraViewToClip, 0, sizeof(opt.cameraViewToClip));
    std::memset(opt.clipToCameraView, 0, sizeof(opt.clipToCameraView));
    std::memset(opt.clipToLensClip, 0, sizeof(opt.clipToLensClip));
    std::memset(opt.clipToPrevClip, 0, sizeof(opt.clipToPrevClip));
    std::memset(opt.prevClipToClip, 0, sizeof(opt.prevClipToClip));
    for (int i = 0; i < 4; ++i) {
      opt.clipToLensClip[i][i] = 1.0F;
      opt.clipToPrevClip[i][i] = 1.0F;
      opt.prevClipToClip[i][i] = 1.0F;
    }
    opt.cameraViewToClip[0][0] = 1.0F / aspect; // tan(fov / 2) = 1
    opt.cameraViewToClip[1][1] = 1.0F;
    opt.cameraViewToClip[2][2] = pz;
    opt.cameraViewToClip[2][3] = 1.0F;
    opt.cameraViewToClip[3][2] = pw;
    opt.clipToCameraView[0][0] = aspect;
    opt.clipToCameraView[1][1] = 1.0F;
    opt.clipToCameraView[2][3] = 1.0F / pw;
    opt.clipToCameraView[3][2] = 1.0F;
    opt.clipToCameraView[3][3] = -pz / pw;
    opt.jitterOffset[0] = 0.0F;
    opt.jitterOffset[1] = 0.0F;
    // Flow is stored in mvec-texture pixels as (previous - current), which is
    // DLSS-G's native unit and direction.
    opt.mvecScale[0] = 1.0F;
    opt.mvecScale[1] = 1.0F;
    opt.cameraPinholeOffset[0] = 0.0F;
    opt.cameraPinholeOffset[1] = 0.0F;
    opt.cameraPos[0] = opt.cameraPos[1] = opt.cameraPos[2] = 0.0F;
    opt.cameraUp[0] = 0.0F;
    opt.cameraUp[1] = 1.0F;
    opt.cameraUp[2] = 0.0F;
    opt.cameraRight[0] = 1.0F;
    opt.cameraRight[1] = 0.0F;
    opt.cameraRight[2] = 0.0F;
    opt.cameraFwd[0] = 0.0F;
    opt.cameraFwd[1] = 0.0F;
    opt.cameraFwd[2] = 1.0F;
    opt.cameraNear = near_plane;
    opt.cameraFar = far_plane;
    opt.cameraFOV = 1.5707963F;
    opt.cameraAspectRatio = aspect;
    opt.colorBuffersHDR = false;
    opt.depthInverted = false;
    opt.cameraMotionIncluded = true;
    opt.reset = !use_previous;
    opt.automodeOverrideReset = false;
    opt.notRenderingGameFrames = false;
    opt.orthoProjection = false;
    opt.motionVectorsInvalidValue = 3.402823466e38F;
    opt.motionVectorsDilated = false;
    opt.menuDetectionEnabled = false;
    opt.mvecsSubrectSize = {d.mvec.width, d.mvec.height};
    opt.depthSubrectSize = {d.depth.width, d.depth.height};
    opt.backbufferSubrectSize = {d.width, d.height};
    opt.outputInterpSubrectSize = {d.width, d.height};

    const NVSDK_NGX_Result result = NGX_VK_EVALUATE_DLSSG(cmd, d.handle, d.params, &eval, &opt);
    vk::full_barrier(cmd);
    if (NVSDK_NGX_FAILED(result)) {
      last_error_ = "DLSS Frame Generation evaluation failed (" + ngx_result_string(result) + ")";
      log::warning("frame generation: ", last_error_);
      failed = true;
      break;
    }
  }

  d.parity = prev;
  d.have_previous = true;
  if (failed) {
    return 0;
  }
  return use_previous ? count : 0;
#else
  (void)cmd;
  (void)current;
  (void)multiplier;
  (void)reset;
  return 0;
#endif
}

FrameGenerationCaps probe_frame_generation() {
  FrameGenerationCaps caps;
  vk::Device device;
  try {
    vk::DeviceRequest request;
    request.want_frame_generation = true;
    device = vk::create_device(request);
    if (!device.frame_generation_extensions) {
      caps.reason = device.frame_generation_reason.empty() ? "DLSS Frame Generation is unavailable"
                                                           : device.frame_generation_reason;
    } else {
      FrameGenerator generator(device.gpu);
      caps = generator.caps();
    }
  } catch (const AppError& error) {
    caps.available = false;
    caps.reason = error.what();
  }
  vk::destroy_device(device);
  return caps;
}

} // namespace cv
