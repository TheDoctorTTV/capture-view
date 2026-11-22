#include "renderer_vulkan.hpp"

#include "frame_generator.hpp"
#include "log.hpp"
#include "vk_device.hpp"
#include "vk_util.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace cv {

namespace {

constexpr uint32_t kConvert[] = {
#include "convert.comp.spv.inc"
};
constexpr uint32_t kPresentVert[] = {
#include "present.vert.spv.inc"
};
constexpr uint32_t kPresentFrag[] = {
#include "present.frag.spv.inc"
};
constexpr uint32_t kOverlayVert[] = {
#include "overlay.vert.spv.inc"
};
constexpr uint32_t kOverlayFrag[] = {
#include "overlay.frag.spv.inc"
};

constexpr uint32_t kSlots = 2;                                 // CPU batches in flight
constexpr uint32_t kPresentsPerBatch = FrameGenerator::kMaxMultiplier; // generated + real

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

// ---------------------------------------------------------------- overlay font

struct BitmapGlyph {
  std::array<uint8_t, 16> rows{};
  int advance = 6;
};

struct BitmapFont {
  std::array<BitmapGlyph, 95> glyphs{};
  bool loaded = false;
};

std::filesystem::path executable_dir() {
  std::array<char, 4096> path{};
  const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length <= 0) {
    return {};
  }
  path[static_cast<size_t>(length)] = '\0';
  return std::filesystem::path(path.data()).parent_path();
}

std::optional<std::ifstream> open_kirsch_font() {
  const std::filesystem::path exe_dir = executable_dir();
  std::vector<std::filesystem::path> paths;
  if (!exe_dir.empty()) {
    paths.push_back(exe_dir.parent_path() / "share/capture-view/kirsch.bdf");
    paths.push_back(exe_dir.parent_path() / "data/kirsch.bdf");
  }
  paths.emplace_back("data/kirsch.bdf");
  paths.emplace_back("/usr/local/share/capture-view/kirsch.bdf");
  paths.emplace_back("/usr/share/capture-view/kirsch.bdf");
  for (const auto& path : paths) {
    std::ifstream file(path);
    if (file.good()) {
      log::info("overlay font loaded: ", path.string());
      return file;
    }
  }
  log::warning("overlay font not found; overlay text disabled");
  return std::nullopt;
}

BitmapFont load_kirsch_font() {
  BitmapFont font;
  auto file = open_kirsch_font();
  if (!file.has_value()) {
    return font;
  }
  constexpr int ascent = 12;
  std::string line;
  int encoding = -1;
  int advance = 6;
  int width = 0;
  int height = 0;
  int x_offset = 0;
  int y_offset = 0;
  std::vector<std::string> bitmap;
  bool in_char = false;
  bool in_bitmap = false;

  auto finish = [&]() {
    if (encoding < 32 || encoding > 126 || height <= 0 || width <= 0) {
      return;
    }
    BitmapGlyph glyph;
    glyph.advance = advance;
    const size_t row_count = std::min(bitmap.size(), static_cast<size_t>(height));
    for (size_t row = 0; row < row_count; ++row) {
      const std::string& hex = bitmap[row];
      if (hex.empty()) {
        continue;
      }
      const auto value = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
      const int bits = static_cast<int>(hex.size() * 4);
      const int global_y = ascent - (y_offset + height) + static_cast<int>(row);
      if (global_y < 0 || global_y >= static_cast<int>(glyph.rows.size())) {
        continue;
      }
      for (int bit = 0; bit < width && bit < 8; ++bit) {
        const uint32_t mask = 1U << static_cast<unsigned int>(bits - 1 - bit);
        if ((value & mask) == 0U) {
          continue;
        }
        const int x = x_offset + bit;
        if (x >= 0 && x < 8) {
          glyph.rows[static_cast<size_t>(global_y)] |= static_cast<uint8_t>(1U << (7 - x));
        }
      }
    }
    font.glyphs[static_cast<size_t>(encoding - 32)] = glyph;
    font.loaded = true;
  };

  while (std::getline(*file, line)) {
    if (line.starts_with("STARTCHAR")) {
      in_char = true;
      encoding = -1;
      advance = 6;
      width = height = x_offset = y_offset = 0;
      bitmap.clear();
      in_bitmap = false;
      continue;
    }
    if (line == "ENDCHAR") {
      finish();
      in_char = false;
      in_bitmap = false;
      continue;
    }
    if (!in_char) {
      continue;
    }
    if (in_bitmap) {
      bitmap.push_back(line);
      continue;
    }
    std::istringstream stream(line);
    std::string key;
    stream >> key;
    if (key == "ENCODING") {
      stream >> encoding;
    } else if (key == "DWIDTH") {
      stream >> advance;
    } else if (key == "BBX") {
      stream >> width >> height >> x_offset >> y_offset;
    } else if (key == "BITMAP") {
      in_bitmap = true;
    }
  }
  return font;
}

const BitmapFont& kirsch_font() {
  static const BitmapFont font = load_kirsch_font();
  return font;
}

const std::vector<std::string>& help_lines() {
  static const std::vector<std::string> lines{
      "keyboard",
      "F fullscreen",
      "Esc exit fullscreen / quit",
      "Q quit",
      "S toggle stats overlay",
      "G toggle status panel",
      "? toggle this help",
      "V toggle vsync",
      "N cycle frame generation",
      "R restart capture",
      "A restart audio",
      "M mute audio",
      "+ - volume",
      "Alt+B toggle borderless",
  };
  return lines;
}

struct OverlayVertex {
  float x;
  float y;
  float r;
  float g;
  float b;
  float a;
};

struct Rect {
  float x = 0;
  float y = 0;
  float w = 0;
  float h = 0;
};

class OverlayBuilder {
public:
  void rect(Rect r, std::array<float, 4> c) {
    const OverlayVertex a{r.x, r.y, c[0], c[1], c[2], c[3]};
    const OverlayVertex b{r.x + r.w, r.y, c[0], c[1], c[2], c[3]};
    const OverlayVertex d{r.x, r.y + r.h, c[0], c[1], c[2], c[3]};
    const OverlayVertex e{r.x + r.w, r.y + r.h, c[0], c[1], c[2], c[3]};
    vertices.insert(vertices.end(), {a, b, d, b, e, d});
  }

  void text(float x, float y, const std::string& value) {
    const BitmapFont& font = kirsch_font();
    if (!font.loaded) {
      return;
    }
    const std::array<float, 4> color{0.96F, 0.96F, 0.96F, 1.0F};
    for (char ch : value) {
      const auto raw = static_cast<unsigned char>(ch);
      const BitmapGlyph& glyph =
          raw >= 32 && raw <= 126 ? font.glyphs[static_cast<size_t>(raw - 32)] : font.glyphs['?' - 32];
      for (size_t row = 0; row < glyph.rows.size(); ++row) {
        // Merge horizontal runs of lit pixels into one quad.
        int col = 0;
        while (col < 8) {
          if ((glyph.rows[row] & static_cast<uint8_t>(1U << (7 - col))) == 0U) {
            ++col;
            continue;
          }
          const int start = col;
          while (col < 8 && (glyph.rows[row] & static_cast<uint8_t>(1U << (7 - col))) != 0U) {
            ++col;
          }
          rect({x + static_cast<float>(start), y + static_cast<float>(row), static_cast<float>(col - start), 1.0F},
               color);
        }
      }
      x += static_cast<float>(glyph.advance + 1);
    }
  }

  std::vector<OverlayVertex> vertices;
};

} // namespace

// ============================================================================

struct VulkanRenderer::Impl {
  // Window / options.
  SDL_Window* window = nullptr;
  bool fullscreen = false;
  bool borderless = false;
  bool vsync = false;
  bool show_stats = false;
  bool show_gui = false;
  bool show_help = false;
  bool window_focused = true;
  bool cursor_visible = true;
  Clock::time_point last_mouse_motion = Clock::now();
  Clock::time_point last_title_update{};
  std::vector<std::string> gui_lines;
  RenderStats stats;

  // Vulkan core.
  vk::Device device;
  vk::Gpu& gpu() { return device.gpu; }

  // Swapchain.
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swap_format = VK_FORMAT_UNDEFINED;
  VkExtent2D swap_extent{};
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  std::vector<VkImage> swap_images;
  std::vector<VkImageView> swap_views;
  std::vector<VkSemaphore> render_done; // one per swapchain image
  bool swapchain_dirty = true;

  // Pipelines.
  vk::ComputePipeline convert;
  VkDescriptorSetLayout present_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout present_layout = VK_NULL_HANDLE;
  VkPipeline present_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout overlay_layout = VK_NULL_HANDLE;
  VkPipeline overlay_pipeline = VK_NULL_HANDLE;
  VkFormat pipeline_format = VK_FORMAT_UNDEFINED;
  VkSampler sampler = VK_NULL_HANDLE;

  // Per-batch resources.
  VkCommandPool pool = VK_NULL_HANDLE;
  struct Slot {
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer process = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kPresentsPerBatch> present{};
    std::array<VkSemaphore, kPresentsPerBatch> acquired{};
    vk::Buffer staging;      // system memory, CPU writes the captured frame
    vk::Buffer staging_vram; // VRAM copy the conversion shader reads
    vk::Buffer overlay;
  };
  std::array<Slot, kSlots> slots{};
  uint64_t batch = 0;

  // Video images (ring of two so DLSS-G and display never race the upload).
  std::array<vk::Image, 2> source{};
  Size source_size{0, 0};
  uint64_t real_frames = 0;

  // Frame generation.
  uint32_t fg_requested = 1;
  uint32_t fg_active = 1;
  std::unique_ptr<FrameGenerator> generator;
  std::string fg_reason;
  bool fg_reset = true;
  uint32_t source_fps = 60;
  double frame_interval_ms = 1000.0 / 60.0;
  Clock::time_point last_arrival{};
  float display_hz = 0.0F;

  // Output frame-rate measurement.
  uint64_t presents = 0;
  uint64_t presents_at_report = 0;
  Clock::time_point fps_report = Clock::now();

  // ------------------------------------------------------------------ setup
  void create(const RendererOptions& options);
  void destroy();
  void create_pipelines(VkFormat format);
  void destroy_pipelines();
  void create_swapchain();
  void destroy_swapchain();
  void ensure_source(VkCommandBuffer cmd, Size size);
  void ensure_buffer(vk::Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage);
  void enable_frame_generation();

  // ------------------------------------------------------------------ frame
  void submit_frame(const void* bytes, size_t length, Size size, uint32_t format, uint32_t stride, uint32_t uv_offset,
                    Clock::time_point captured_at, const std::string& stats_text);
  bool present_image(Slot& slot, uint32_t index, const vk::Image& image, const OverlayBuilder& overlay, bool last,
                     double& present_ms);
  void finish_batch(Slot& slot);
  void build_overlay(OverlayBuilder& out, const std::string& stats_text) const;
  [[nodiscard]] Rect destination_rect(Size frame) const;
  void update_title(const std::string& stats_text);
  void count_present();
};

void VulkanRenderer::Impl::create(const RendererOptions& options) {
  fullscreen = options.fullscreen;
  borderless = options.borderless;
  vsync = options.vsync;
  fg_requested = std::clamp<uint32_t>(options.frame_generation, 1, FrameGenerator::kMaxMultiplier);
  source_fps = options.source_fps == 0 ? 60 : options.source_fps;
  frame_interval_ms = 1000.0 / static_cast<double>(source_fps);

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    throw AppError(std::string("SDL_Init failed: ") + SDL_GetError());
  }
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (fullscreen) {
    flags |= SDL_WINDOW_FULLSCREEN;
  }
  if (borderless) {
    flags |= SDL_WINDOW_BORDERLESS;
  }
  window = SDL_CreateWindow(options.title.c_str(), static_cast<int>(options.size.width),
                            static_cast<int>(options.size.height), flags);
  if (window == nullptr) {
    throw AppError(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
  }
  if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)); mode != nullptr) {
    display_hz = mode->refresh_rate;
  }

  vk::DeviceRequest request;
  Uint32 count = 0;
  const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
  if (names == nullptr) {
    throw AppError(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
  }
  for (Uint32 i = 0; i < count; ++i) {
    request.instance_extensions.emplace_back(names[i]);
  }
  request.want_surface = true;
  // Always ask for the DLSS-G extensions on NVIDIA so frame generation can be
  // switched on later without recreating the device.
  request.want_frame_generation = true;
  device = vk::create_device(
      request,
      [](VkInstance instance, void* user) -> VkSurfaceKHR {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(user), instance, nullptr, &surface)) {
          log::error("SDL_Vulkan_CreateSurface failed: ", SDL_GetError());
          return VK_NULL_HANDLE;
        }
        return surface;
      },
      window);
  log::info("renderer selected: vulkan (", gpu().properties.deviceName, ")");

  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = gpu().queue_family;
  vk::check(vkCreateCommandPool(gpu().device, &pool_info, nullptr, &pool), "vkCreateCommandPool");
  for (auto& slot : slots) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vk::check(vkAllocateCommandBuffers(gpu().device, &alloc, &slot.process), "vkAllocateCommandBuffers");
    alloc.commandBufferCount = kPresentsPerBatch;
    vk::check(vkAllocateCommandBuffers(gpu().device, &alloc, slot.present.data()), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vk::check(vkCreateFence(gpu().device, &fence, nullptr, &slot.fence), "vkCreateFence");
    for (auto& semaphore : slot.acquired) {
      VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      vk::check(vkCreateSemaphore(gpu().device, &info, nullptr, &semaphore), "vkCreateSemaphore");
    }
  }

  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = 0.0F;
  vk::check(vkCreateSampler(gpu().device, &sampler_info, nullptr, &sampler), "vkCreateSampler");

  const VkDescriptorType convert_bindings[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  convert = vk::create_compute_pipeline(gpu(), kConvert, convert_bindings, 5 * sizeof(uint32_t));

  create_swapchain();
  if (fg_requested > 1) {
    enable_frame_generation();
  }
}

void VulkanRenderer::Impl::enable_frame_generation() {
  if (generator) {
    return;
  }
  if (!device.frame_generation_extensions) {
    fg_reason = device.frame_generation_reason.empty() ? "unavailable" : device.frame_generation_reason;
    log::warning("frame generation off: ", fg_reason);
    return;
  }
  try {
    generator = std::make_unique<FrameGenerator>(gpu());
  } catch (const AppError& error) {
    fg_reason = error.what();
    log::warning("frame generation off: ", fg_reason);
    return;
  }
  if (!generator->caps().available) {
    fg_reason = generator->caps().reason;
    log::warning("frame generation off: ", fg_reason);
    return;
  }
  fg_reason.clear();
  fg_reset = true;
  log::info("frame generation ready: up to ", generator->caps().max_multiplier, "x");
}

void VulkanRenderer::Impl::destroy() {
  if (gpu().device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(gpu().device);
    generator.reset();
    for (auto& image : source) {
      vk::destroy_image(gpu(), image);
    }
    for (auto& slot : slots) {
      vk::destroy_buffer(gpu(), slot.staging);
      vk::destroy_buffer(gpu(), slot.staging_vram);
      vk::destroy_buffer(gpu(), slot.overlay);
      for (auto semaphore : slot.acquired) {
        vkDestroySemaphore(gpu().device, semaphore, nullptr);
      }
      vkDestroyFence(gpu().device, slot.fence, nullptr);
    }
    destroy_swapchain();
    destroy_pipelines();
    vk::destroy_pipeline(gpu(), convert);
    vkDestroySampler(gpu().device, sampler, nullptr);
    vkDestroyCommandPool(gpu().device, pool, nullptr);
  }
  vk::destroy_device(device);
  if (window != nullptr) {
    SDL_DestroyWindow(window);
    window = nullptr;
  }
  SDL_Quit();
}

void VulkanRenderer::Impl::create_pipelines(VkFormat format) {
  destroy_pipelines();
  pipeline_format = format;
  VkDevice dev = gpu().device;

  // Present pipeline: one combined image sampler via push descriptor.
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  vk::check(vkCreateDescriptorSetLayout(dev, &set_info, nullptr, &present_set_layout), "vkCreateDescriptorSetLayout");
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &present_set_layout;
  vk::check(vkCreatePipelineLayout(dev, &layout_info, nullptr, &present_layout), "vkCreatePipelineLayout");

  VkPushConstantRange overlay_range{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
  VkPipelineLayoutCreateInfo overlay_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  overlay_layout_info.pushConstantRangeCount = 1;
  overlay_layout_info.pPushConstantRanges = &overlay_range;
  vk::check(vkCreatePipelineLayout(dev, &overlay_layout_info, nullptr, &overlay_layout), "vkCreatePipelineLayout");

  auto build = [&](std::span<const uint32_t> vert_code, std::span<const uint32_t> frag_code, VkPipelineLayout layout,
                   bool overlay) {
    VkShaderModule vert = vk::create_shader(gpu(), vert_code);
    VkShaderModule frag = vk::create_shader(gpu(), frag_code);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding{0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 2 * sizeof(float)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (overlay) {
      vertex_input.vertexBindingDescriptionCount = 1;
      vertex_input.pVertexBindingDescriptions = &vertex_binding;
      vertex_input.vertexAttributeDescriptionCount = 2;
      vertex_input.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (overlay) {
      blend_attachment.blendEnable = VK_TRUE;
      blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
      blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    vk::check(result, "vkCreateGraphicsPipelines");
    return pipeline;
  };
  present_pipeline = build(kPresentVert, kPresentFrag, present_layout, false);
  overlay_pipeline = build(kOverlayVert, kOverlayFrag, overlay_layout, true);
}

void VulkanRenderer::Impl::destroy_pipelines() {
  VkDevice dev = gpu().device;
  if (present_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, present_pipeline, nullptr);
  if (overlay_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, overlay_pipeline, nullptr);
  if (present_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, present_layout, nullptr);
  if (overlay_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, overlay_layout, nullptr);
  if (present_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, present_set_layout, nullptr);
  present_pipeline = overlay_pipeline = VK_NULL_HANDLE;
  present_layout = overlay_layout = VK_NULL_HANDLE;
  present_set_layout = VK_NULL_HANDLE;
}

void VulkanRenderer::Impl::create_swapchain() {
  int pixel_w = 0;
  int pixel_h = 0;
  SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
  if (pixel_w <= 0 || pixel_h <= 0) {
    swapchain_dirty = true; // minimised; try again later
    return;
  }
  vkDeviceWaitIdle(gpu().device);
  VkSwapchainKHR old = swapchain;
  for (auto view : swap_views) {
    vkDestroyImageView(gpu().device, view, nullptr);
  }
  swap_views.clear();
  for (auto semaphore : render_done) {
    vkDestroySemaphore(gpu().device, semaphore, nullptr);
  }
  render_done.clear();

  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu().physical, device.surface, &caps);
  uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu().physical, device.surface, &format_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu().physical, device.surface, &format_count, formats.data());
  VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                              : formats.front();
  // Video is already gamma encoded: use a UNORM (not sRGB) swapchain format.
  for (const auto& format : formats) {
    if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = format;
      break;
    }
  }

  uint32_t mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(gpu().physical, device.surface, &mode_count, nullptr);
  std::vector<VkPresentModeKHR> modes(mode_count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(gpu().physical, device.surface, &mode_count, modes.data());
  auto supports = [&](VkPresentModeKHR mode) { return std::ranges::find(modes, mode) != modes.end(); };
  present_mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync) {
    if (supports(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
      present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if (supports(VK_PRESENT_MODE_MAILBOX_KHR)) {
      present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
  }

  VkExtent2D extent{static_cast<uint32_t>(pixel_w), static_cast<uint32_t>(pixel_h)};
  if (caps.currentExtent.width != UINT32_MAX) {
    extent = caps.currentExtent;
  }
  extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  uint32_t image_count = std::max(caps.minImageCount + 1, 3U);
  if (caps.maxImageCount != 0) {
    image_count = std::min(image_count, caps.maxImageCount);
  }

  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = device.surface;
  info.minImageCount = image_count;
  info.imageFormat = chosen.format;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
    for (uint32_t bit = 1; bit != 0; bit <<= 1U) {
      if ((caps.supportedCompositeAlpha & bit) != 0) {
        info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
        break;
      }
    }
  }
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = old;
  vk::check(vkCreateSwapchainKHR(gpu().device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
  if (old != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(gpu().device, old, nullptr);
  }
  swap_format = chosen.format;
  swap_extent = extent;

  uint32_t count = 0;
  vkGetSwapchainImagesKHR(gpu().device, swapchain, &count, nullptr);
  swap_images.resize(count);
  vkGetSwapchainImagesKHR(gpu().device, swapchain, &count, swap_images.data());
  for (VkImage image : swap_images) {
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = swap_format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    vk::check(vkCreateImageView(gpu().device, &view_info, nullptr, &view), "vkCreateImageView(swapchain)");
    swap_views.push_back(view);
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    vk::check(vkCreateSemaphore(gpu().device, &semaphore_info, nullptr, &semaphore), "vkCreateSemaphore");
    render_done.push_back(semaphore);
  }
  if (pipeline_format != swap_format) {
    create_pipelines(swap_format);
  }
  swapchain_dirty = false;
  log::info("swapchain ", extent.width, "x", extent.height, " images=", count, " present_mode=",
            present_mode == VK_PRESENT_MODE_FIFO_KHR      ? "fifo"
            : present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                                                          : "immediate");
}

void VulkanRenderer::Impl::destroy_swapchain() {
  for (auto view : swap_views) {
    vkDestroyImageView(gpu().device, view, nullptr);
  }
  swap_views.clear();
  for (auto semaphore : render_done) {
    vkDestroySemaphore(gpu().device, semaphore, nullptr);
  }
  render_done.clear();
  if (swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(gpu().device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
  }
}

void VulkanRenderer::Impl::ensure_buffer(vk::Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
  if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= size) {
    return;
  }
  vk::destroy_buffer(gpu(), buffer);
  buffer = vk::create_host_buffer(gpu(), std::max<VkDeviceSize>(size + size / 4, 64 * 1024), usage);
}

void VulkanRenderer::Impl::ensure_source(VkCommandBuffer cmd, Size size) {
  if (source_size.width == size.width && source_size.height == size.height && source[0].image != VK_NULL_HANDLE) {
    return;
  }
  vkDeviceWaitIdle(gpu().device);
  for (auto& image : source) {
    vk::destroy_image(gpu(), image);
    image = vk::create_image(gpu(), size.width, size.height, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vk::init_general_layout(cmd, image);
  }
  source_size = size;
  fg_reset = true;
}

Rect VulkanRenderer::Impl::destination_rect(Size frame) const {
  const auto window_w = static_cast<float>(swap_extent.width);
  const auto window_h = static_cast<float>(swap_extent.height);
  if (window_w <= 0 || window_h <= 0 || frame.width == 0 || frame.height == 0) {
    return {0, 0, window_w, window_h};
  }
  const float scale = std::min(window_w / static_cast<float>(frame.width), window_h / static_cast<float>(frame.height));
  const float w = static_cast<float>(frame.width) * scale;
  const float h = static_cast<float>(frame.height) * scale;
  return {std::floor((window_w - w) * 0.5F), std::floor((window_h - h) * 0.5F), std::round(w), std::round(h)};
}

void VulkanRenderer::Impl::build_overlay(OverlayBuilder& out, const std::string& stats_text) const {
  const auto window_w = static_cast<float>(swap_extent.width);
  const auto window_h = static_cast<float>(swap_extent.height);
  if (show_stats) {
    out.rect({8.0F, 8.0F, 860.0F, 24.0F}, {0.0F, 0.0F, 0.0F, 0.70F});
    out.text(14.0F, 12.0F, stats_text);
  }
  if (show_gui && !gui_lines.empty()) {
    const float height = 32.0F + static_cast<float>(gui_lines.size()) * 18.0F;
    out.rect({14.0F, 44.0F, 600.0F, height}, {0.05F, 0.07F, 0.11F, 0.86F});
    out.text(24.0F, 54.0F, "status");
    float y = 76.0F;
    for (const auto& line : gui_lines) {
      out.text(24.0F, y, line);
      y += 18.0F;
    }
  }
  if (show_help) {
    const auto& lines = help_lines();
    const float width = 430.0F;
    const float height = 32.0F + static_cast<float>(lines.size()) * 18.0F;
    const float x = std::max(14.0F, window_w - width - 14.0F);
    const float y0 = std::max(44.0F, window_h - height - 14.0F);
    out.rect({x, y0, width, height}, {0.03F, 0.04F, 0.06F, 0.92F});
    float y = y0 + 10.0F;
    for (const auto& line : lines) {
      out.text(x + 10.0F, y, line);
      y += 18.0F;
    }
  }
}

void VulkanRenderer::Impl::update_title(const std::string& stats_text) {
  if (!show_stats) {
    return;
  }
  const auto now = Clock::now();
  if (last_title_update.time_since_epoch().count() != 0 && now - last_title_update < std::chrono::milliseconds(250)) {
    return;
  }
  SDL_SetWindowTitle(window, stats_text.c_str());
  last_title_update = now;
}

void VulkanRenderer::Impl::count_present() {
  ++presents;
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - fps_report).count();
  if (seconds >= 0.5) {
    stats.output_fps = static_cast<double>(presents - presents_at_report) / seconds;
    presents_at_report = presents;
    fps_report = now;
  }
}

bool VulkanRenderer::Impl::present_image(Slot& slot, uint32_t index, const vk::Image& image,
                                         const OverlayBuilder& overlay, bool last, double& present_ms) {
  if (swapchain == VK_NULL_HANDLE || swapchain_dirty) {
    return false;
  }
  uint32_t image_index = 0;
  const VkResult acquire = vkAcquireNextImageKHR(gpu().device, swapchain, UINT64_MAX, slot.acquired[index],
                                                 VK_NULL_HANDLE, &image_index);
  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
    swapchain_dirty = true;
    return false;
  }
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    vk::check(acquire, "vkAcquireNextImageKHR");
  }
  if (acquire == VK_SUBOPTIMAL_KHR) {
    swapchain_dirty = true;
  }

  VkCommandBuffer cmd = slot.present[index];
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);

  // Make compute / DLSS-G writes visible to the fragment shader, and move the
  // swapchain image into the attachment layout.
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  VkImageMemoryBarrier to_attachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_attachment.srcAccessMask = 0;
  to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_attachment.image = swap_images[image_index];
  to_attachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1,
                       &memory, 0, nullptr, 1, &to_attachment);

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = swap_views[image_index];
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{0.0F, 0.0F, 0.0F, 1.0F}};
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, swap_extent};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vkCmdBeginRendering(cmd, &rendering);

  const VkRect2D full_scissor{{0, 0}, swap_extent};
  if (image.image != VK_NULL_HANDLE) {
    const Rect dst = destination_rect({image.width, image.height});
    const VkViewport viewport{dst.x, dst.y, dst.w, dst.h, 0.0F, 1.0F};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline);
    VkDescriptorImageInfo info{sampler, image.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    gpu().push_descriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &write);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }

  if (!overlay.vertices.empty() && slot.overlay.buffer != VK_NULL_HANDLE) {
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(swap_extent.width),
                              static_cast<float>(swap_extent.height), 0.0F, 1.0F};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &slot.overlay.buffer, &offset);
    const float inverse[2] = {1.0F / static_cast<float>(swap_extent.width),
                              1.0F / static_cast<float>(swap_extent.height)};
    vkCmdPushConstants(cmd, overlay_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(inverse), inverse);
    vkCmdDraw(cmd, static_cast<uint32_t>(overlay.vertices.size()), 1, 0, 0);
  }
  vkCmdEndRendering(cmd);

  VkImageMemoryBarrier to_present = to_attachment;
  to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  to_present.dstAccessMask = 0;
  to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &to_present);
  vkEndCommandBuffer(cmd);

  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &slot.acquired[index];
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &render_done[image_index];
  vk::check(vkQueueSubmit(gpu().queue, 1, &submit, last ? slot.fence : VK_NULL_HANDLE), "vkQueueSubmit(present)");

  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &render_done[image_index];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain;
  present.pImageIndices = &image_index;
  const auto start = Clock::now();
  const VkResult result = vkQueuePresentKHR(gpu().queue, &present);
  present_ms = elapsed_ms(start, Clock::now());
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    swapchain_dirty = true;
  } else if (result != VK_SUCCESS) {
    vk::check(result, "vkQueuePresentKHR");
  }
  count_present();
  return true;
}

void VulkanRenderer::Impl::finish_batch(Slot& slot) {
  // Nothing signalled the slot fence (e.g. the window is minimised): submit an
  // empty batch so the fence still tracks this slot's GPU work.
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  vk::check(vkQueueSubmit(gpu().queue, 1, &submit, slot.fence), "vkQueueSubmit(fence)");
}

void VulkanRenderer::Impl::submit_frame(const void* bytes, size_t length, Size size, uint32_t format, uint32_t stride,
                                        uint32_t uv_offset, Clock::time_point captured_at,
                                        const std::string& stats_text) {
  update_title(stats_text);
  if (swapchain_dirty) {
    create_swapchain();
  }

  Slot& slot = slots[batch % kSlots];
  ++batch;
  vk::check(vkWaitForFences(gpu().device, 1, &slot.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
  vk::check(vkResetFences(gpu().device, 1, &slot.fence), "vkResetFences");

  // Upload: one memcpy into host-visible memory; conversion runs on the GPU.
  const auto upload_start = Clock::now();
  const VkDeviceSize upload_size = (length + 3) & ~size_t{3};
  ensure_buffer(slot.staging, upload_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  if (slot.staging_vram.buffer == VK_NULL_HANDLE || slot.staging_vram.size < upload_size) {
    vk::destroy_buffer(gpu(), slot.staging_vram); // the slot fence was waited above
    slot.staging_vram = vk::create_device_buffer(gpu(), slot.staging.size,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  }
  std::memcpy(slot.staging.mapped, bytes, length);

  OverlayBuilder overlay;
  build_overlay(overlay, stats_text);
  if (!overlay.vertices.empty()) {
    ensure_buffer(slot.overlay, overlay.vertices.size() * sizeof(OverlayVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    std::memcpy(slot.overlay.mapped, overlay.vertices.data(), overlay.vertices.size() * sizeof(OverlayVertex));
  }
  stats.upload_ms = elapsed_ms(upload_start, Clock::now());

  // Frame interval estimate for pacing generated frames.
  const auto arrival = captured_at.time_since_epoch().count() != 0 ? captured_at : Clock::now();
  if (last_arrival.time_since_epoch().count() != 0) {
    const double measured = elapsed_ms(last_arrival, arrival);
    const double nominal = 1000.0 / static_cast<double>(source_fps);
    if (measured > nominal * 0.5 && measured < nominal * 2.5) {
      frame_interval_ms = frame_interval_ms * 0.9 + measured * 0.1;
    } else if (measured >= nominal * 2.5) {
      fg_reset = true; // a stall: the previous frame is too old to blend with
    }
  }
  last_arrival = arrival;

  // ---------------------------------------------------------------- process
  VkCommandBuffer cmd = slot.process;
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  ensure_source(cmd, size);
  vk::full_barrier(cmd);
  // One bulk DMA copy into VRAM; shaders never read system memory.
  vk::copy_buffer(cmd, slot.staging, slot.staging_vram, upload_size, true);

  const vk::Image& target = source[real_frames % 2];
  ++real_frames;
  {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, convert.pipeline);
    VkDescriptorBufferInfo buffer_info{slot.staging_vram.buffer, 0, upload_size};
    VkDescriptorImageInfo image_info{VK_NULL_HANDLE, target.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {vk::storage_buffer_write(0, &buffer_info),
                                      vk::storage_image_write(1, &image_info)};
    gpu().push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, convert.layout, 0, 2, writes);
    const uint32_t push[5] = {size.width, size.height, stride, format, uv_offset};
    vkCmdPushConstants(cmd, convert.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, vk::group_count(size.width, 16), vk::group_count(size.height, 16), 1);
    vk::full_barrier(cmd);
  }

  uint32_t generated = 0;
  fg_active = 1;
  if (fg_requested > 1 && generator && generator->caps().available) {
    if (generator->configure(cmd, size.width, size.height, fg_requested)) {
      fg_active = std::min(fg_requested, generator->caps().max_multiplier);
      generated = generator->record(cmd, target, fg_active, fg_reset);
      fg_reset = false;
    } else {
      fg_reason = generator->last_error();
    }
  }
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vk::check(vkQueueSubmit(gpu().queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(process)");

  // ---------------------------------------------------------------- present
  // Generated frames first (the first one immediately), then the real frame,
  // all spaced evenly across one source frame interval.
  const auto start = Clock::now();
  const double step_ms = frame_interval_ms / static_cast<double>(generated + 1);
  bool fence_submitted = false;
  double present_ms = 0.0;
  double total_present_ms = 0.0;
  for (uint32_t i = 0; i <= generated; ++i) {
    const bool real = i == generated;
    if (i > 0) {
      const auto target_time =
          start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(step_ms * i));
      const auto now = Clock::now();
      if (!real && now > target_time + std::chrono::duration_cast<Clock::duration>(
                                           std::chrono::duration<double, std::milli>(step_ms * 0.5))) {
        ++stats.generated_skipped; // running late: drop this generated frame
        continue;
      }
      if (now < target_time) {
        std::this_thread::sleep_until(target_time - std::chrono::microseconds(300));
        while (Clock::now() < target_time) {
          std::this_thread::yield();
        }
      }
    }
    const vk::Image& image = real ? target : generator->generated(i);
    if (present_image(slot, i, image, overlay, real, present_ms)) {
      total_present_ms += present_ms;
      if (real) {
        fence_submitted = true;
      } else {
        ++stats.generated_presented;
      }
    }
  }
  if (!fence_submitted) {
    finish_batch(slot);
  }
  stats.present_ms = total_present_ms;
}

// ============================================================================

VulkanRenderer::VulkanRenderer(const RendererOptions& options) : impl_(std::make_unique<Impl>()) {
  try {
    impl_->create(options);
  } catch (...) {
    impl_->destroy();
    throw;
  }
}

VulkanRenderer::~VulkanRenderer() { impl_->destroy(); }

bool VulkanRenderer::handle_events(bool& restart_requested, bool& audio_restart_requested, bool& mute_requested,
                                   float& volume_delta) {
  Impl& d = *impl_;
  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      return false;
    }
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED) {
      d.swapchain_dirty = true;
      continue;
    }
    if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
      if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(d.window)); mode != nullptr) {
        d.display_hz = mode->refresh_rate;
      }
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      d.last_mouse_motion = Clock::now();
      if (!d.cursor_visible) {
        (void)SDL_ShowCursor();
        d.cursor_visible = true;
      }
      continue;
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
      d.window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
      d.last_mouse_motion = Clock::now();
      if (!d.cursor_visible) {
        (void)SDL_ShowCursor();
        d.cursor_visible = true;
      }
      continue;
    }
    if (event.type != SDL_EVENT_KEY_DOWN) {
      continue;
    }
    const SDL_Keycode key = event.key.key;
    if (key == SDLK_Q) {
      return false;
    }
    if (key == SDLK_ESCAPE) {
      if (d.fullscreen) {
        d.fullscreen = false;
        SDL_SetWindowFullscreen(d.window, false);
      } else {
        return false;
      }
    } else if (key == SDLK_F) {
      d.fullscreen = !d.fullscreen;
      SDL_SetWindowFullscreen(d.window, d.fullscreen);
    } else if (key == SDLK_B && (event.key.mod & SDL_KMOD_ALT) != 0) {
      d.borderless = !d.borderless;
      SDL_SetWindowBordered(d.window, !d.borderless);
    } else if (key == SDLK_V) {
      set_vsync(!d.vsync);
    } else if (key == SDLK_N) {
      // Cycle off -> 2x -> ... -> max -> off.
      if (!d.generator && d.fg_requested == 1) {
        d.enable_frame_generation();
      }
      const uint32_t max = d.generator && d.generator->caps().available ? d.generator->caps().max_multiplier : 1;
      d.fg_requested = d.fg_requested >= max ? 1 : d.fg_requested + 1;
      d.fg_reset = true;
      log::info("frame generation set to ", d.fg_requested == 1 ? std::string("off") : std::to_string(d.fg_requested) + "x");
    } else if (key == SDLK_S) {
      d.show_stats = !d.show_stats;
    } else if (key == SDLK_G) {
      d.show_gui = !d.show_gui;
    } else if (key == SDLK_QUESTION || (key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT) != 0)) {
      d.show_help = !d.show_help;
    } else if (key == SDLK_R) {
      restart_requested = true;
      d.fg_reset = true;
    } else if (key == SDLK_A) {
      audio_restart_requested = true;
    } else if (key == SDLK_M) {
      mute_requested = true;
    } else if (key == SDLK_EQUALS || key == SDLK_PLUS) {
      volume_delta = 0.05F;
    } else if (key == SDLK_MINUS) {
      volume_delta = -0.05F;
    }
  }
  if (d.window_focused && d.cursor_visible && Clock::now() - d.last_mouse_motion >= std::chrono::seconds(3)) {
    (void)SDL_HideCursor();
    d.cursor_visible = false;
  }
  return true;
}

void VulkanRenderer::render(const RgbaFrame& frame, const std::string& stats_text, Clock::time_point captured_at) {
  if (frame.size.width == 0 || frame.size.height == 0 ||
      frame.pixels.size() < static_cast<size_t>(frame.size.width) * frame.size.height * 4) {
    throw AppError("invalid RGBA frame");
  }
  impl_->submit_frame(frame.pixels.data(), frame.pixels.size(), frame.size, 0, frame.size.width * 4, 0, captured_at,
                      stats_text);
}

void VulkanRenderer::render(FrameView frame, const std::string& stats_text) {
  if (frame.format != PixelFormat::Yuyv && frame.format != PixelFormat::Nv12) {
    throw AppError("raw renderer supports yuyv and nv12 only");
  }
  const size_t stride = raw_frame_stride(frame);
  const bool yuyv = frame.format == PixelFormat::Yuyv;
  const size_t rows = static_cast<size_t>(frame.size.height) + (yuyv ? 0U : frame.size.height / 2U);
  const size_t length = std::min(frame.bytes.size(), rows * stride);
  impl_->submit_frame(frame.bytes.data(), length, frame.size, yuyv ? 1U : 2U, static_cast<uint32_t>(stride),
                      static_cast<uint32_t>(stride * frame.size.height), frame.captured_at, stats_text);
}

void VulkanRenderer::set_gui_lines(std::vector<std::string> lines) { impl_->gui_lines = std::move(lines); }

void VulkanRenderer::set_vsync(bool enabled) {
  if (impl_->vsync != enabled) {
    impl_->vsync = enabled;
    impl_->swapchain_dirty = true;
  }
}

void VulkanRenderer::reset_frame_generation() { impl_->fg_reset = true; }

bool VulkanRenderer::show_stats() const { return impl_->show_stats; }
bool VulkanRenderer::show_gui() const { return impl_->show_gui; }
bool VulkanRenderer::vsync() const { return impl_->vsync; }
bool VulkanRenderer::fullscreen() const { return impl_->fullscreen; }
bool VulkanRenderer::borderless() const { return impl_->borderless; }
RenderStats VulkanRenderer::last_stats() const { return impl_->stats; }
uint32_t VulkanRenderer::frame_generation() const { return impl_->fg_requested; }
uint32_t VulkanRenderer::active_frame_generation() const { return impl_->fg_active; }

std::string VulkanRenderer::frame_generation_status() const {
  const Impl& d = *impl_;
  if (d.fg_requested <= 1) {
    return "off";
  }
  if (d.fg_active <= 1) {
    return "off (" + (d.fg_reason.empty() ? std::string("unavailable") : d.fg_reason) + ")";
  }
  std::string status = std::to_string(d.fg_active) + "x";
  if (d.fg_active < d.fg_requested) {
    status += " (max for this gpu)";
  }
  const double wanted = static_cast<double>(d.source_fps) * d.fg_active;
  if (d.display_hz > 0.0F && wanted > static_cast<double>(d.display_hz) * 1.05) {
    status += " exceeds " + std::to_string(static_cast<int>(std::lround(d.display_hz))) + "hz display";
  }
  return status;
}

} // namespace cv
