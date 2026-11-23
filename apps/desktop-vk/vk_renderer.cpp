#include "vk_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <limits>
#include <string_view>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <png.h>

#include "shaders_spv.hpp"

namespace orangutan::vkui {
namespace {

struct PushConstants {
  float width;
  float height;
  float target_is_srgb;
  float dpr;
};

[[nodiscard]] bool write_rgba_png(const std::string& path, std::uint32_t w, std::uint32_t h,
                                  const std::uint8_t* rgba, std::string& err) {
  std::FILE* fp = std::fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    err = std::format("failed to open output PNG '{}'", path);
    return false;
  }
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    std::fclose(fp);
    err = "png_create_write_struct failed";
    return false;
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    std::fclose(fp);
    err = "png_create_info_struct failed";
    return false;
  }
  if (setjmp(png_jmpbuf(png)) != 0) {
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    err = "libpng write error";
    return false;
  }

  png_init_io(png, fp);
  png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  std::vector<png_const_bytep> rows(h);
  for (std::uint32_t y = 0; y < h; ++y) {
    rows[y] = rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4U;
  }
  png_write_image(png, const_cast<png_bytepp>(rows.data()));
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  std::fclose(fp);
  return true;
}

[[nodiscard]] bool read_rgba_png_512(const std::filesystem::path& path, std::vector<std::uint8_t>& out_rgba) {
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    return false;
  }
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    std::fclose(fp);
    return false;
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    std::fclose(fp);
    return false;
  }
  if (setjmp(png_jmpbuf(png)) != 0) {
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return false;
  }

  png_init_io(png, fp);
  png_read_info(png, info);
  const std::uint32_t w = png_get_image_width(png, info);
  const std::uint32_t h = png_get_image_height(png, info);
  const int color_type = png_get_color_type(png, info);
  const int bit_depth = png_get_bit_depth(png, info);
  if (w != 512U || h != 512U) {
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return false;
  }
  if (bit_depth == 16) {
    png_set_strip_16(png);
  }
  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS) != 0) {
    png_set_tRNS_to_alpha(png);
  }
  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }
  png_read_update_info(png, info);

  out_rgba.resize(512ULL * 512ULL * 4ULL);
  std::vector<png_bytep> rows(512U);
  for (std::uint32_t y = 0; y < 512U; ++y) {
    rows[y] = out_rgba.data() + static_cast<std::size_t>(y) * 512ULL * 4ULL;
  }
  png_read_image(png, rows.data());
  png_destroy_read_struct(&png, &info, nullptr);
  std::fclose(fp);
  return true;
}

[[nodiscard]] std::filesystem::path find_companion_asset_dir() {
  std::vector<std::filesystem::path> candidates;
  candidates.emplace_back("docs/assets/companions");
  candidates.emplace_back("assets/companions");
  std::array<char, 4096> exe_buf{};
  const auto n = ::readlink("/proc/self/exe", exe_buf.data(), exe_buf.size() - 1);
  if (n > 0) {
    const auto exe_dir = std::filesystem::path{std::string_view{exe_buf.data(), static_cast<std::size_t>(n)}}.parent_path();
    candidates.push_back(exe_dir / "companions");
    candidates.push_back(exe_dir / "docs/assets/companions");
    candidates.push_back(exe_dir / "../../../../docs/assets/companions");
    candidates.push_back(exe_dir / "../../../docs/assets/companions");
  }
  for (const auto& dir : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(dir / "oran.png", ec)) {
      return dir;
    }
  }
  return {};
}

[[nodiscard]] float srgb_to_linear_channel(float c) noexcept {
  if (c <= 0.04045F) {
    return c / 12.92F;
  }
  return std::pow((c + 0.055F) / 1.055F, 2.4F);
}

template <typename Fn>
bool load_inst_fn(const VkApi& vk, VkInstance inst, const char* name, Fn& out) {
  out = reinterpret_cast<Fn>(vk.get_instance_proc_addr(inst, name));
  return out != nullptr;
}

template <typename Fn>
bool load_dev_fn(const VkApi& vk, VkDevice dev, const char* name, Fn& out) {
  out = reinterpret_cast<Fn>(vk.get_device_proc_addr(dev, name));
  return out != nullptr;
}

}  // namespace

VkRenderer::~VkRenderer() {
  shutdown();
}

bool VkRenderer::load_global_api(PFN_vkGetInstanceProcAddr gipa, std::string& err) {
  if (gipa == nullptr) {
    err = "vkGetInstanceProcAddr is null";
    return false;
  }
  vk_.get_instance_proc_addr = gipa;
  if (!load_inst_fn(vk_, VK_NULL_HANDLE, "vkCreateInstance", vk_.create_instance) ||
      !load_inst_fn(vk_, VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties", vk_.enumerate_instance_layer_properties)) {
    err = "failed to load global Vulkan entry points";
    return false;
  }
  return true;
}

bool VkRenderer::load_instance_api(std::string& err) {
  const bool ok =
      load_inst_fn(vk_, instance_, "vkDestroyInstance", vk_.destroy_instance) &&
      load_inst_fn(vk_, instance_, "vkEnumeratePhysicalDevices", vk_.enumerate_physical_devices) &&
      load_inst_fn(vk_, instance_, "vkGetPhysicalDeviceProperties", vk_.get_physical_device_properties) &&
      load_inst_fn(vk_, instance_, "vkGetPhysicalDeviceQueueFamilyProperties", vk_.get_physical_device_queue_family_properties) &&
      load_inst_fn(vk_, instance_, "vkGetPhysicalDeviceMemoryProperties", vk_.get_physical_device_memory_properties) &&
      load_inst_fn(vk_, instance_, "vkCreateDevice", vk_.create_device) &&
      load_inst_fn(vk_, instance_, "vkGetDeviceProcAddr", vk_.get_device_proc_addr);
  if (!ok) {
    err = "failed to load Vulkan instance functions";
    return false;
  }
  if (!offscreen_) {
    const bool surf_ok =
        load_inst_fn(vk_, instance_, "vkGetPhysicalDeviceSurfaceSupportKHR", vk_.get_physical_device_surface_support_khr) &&
        load_inst_fn(vk_, instance_, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", vk_.get_physical_device_surface_capabilities_khr) &&
        load_inst_fn(vk_, instance_, "vkGetPhysicalDeviceSurfaceFormatsKHR", vk_.get_physical_device_surface_formats_khr) &&
        load_inst_fn(vk_, instance_, "vkGetPhysicalDeviceSurfacePresentModesKHR", vk_.get_physical_device_surface_present_modes_khr) &&
        load_inst_fn(vk_, instance_, "vkDestroySurfaceKHR", vk_.destroy_surface_khr);
    if (!surf_ok) {
      err = "failed to load VK_KHR_surface functions";
      return false;
    }
  }
  return true;
}

bool VkRenderer::load_device_api(std::string& err) {
  const bool ok =
      load_dev_fn(vk_, device_, "vkDestroyDevice", vk_.destroy_device) &&
      load_dev_fn(vk_, device_, "vkGetDeviceQueue", vk_.get_device_queue) &&
      load_dev_fn(vk_, device_, "vkDeviceWaitIdle", vk_.device_wait_idle) &&
      load_dev_fn(vk_, device_, "vkCreateBuffer", vk_.create_buffer) &&
      load_dev_fn(vk_, device_, "vkDestroyBuffer", vk_.destroy_buffer) &&
      load_dev_fn(vk_, device_, "vkGetBufferMemoryRequirements", vk_.get_buffer_memory_requirements) &&
      load_dev_fn(vk_, device_, "vkBindBufferMemory", vk_.bind_buffer_memory) &&
      load_dev_fn(vk_, device_, "vkCreateImage", vk_.create_image) &&
      load_dev_fn(vk_, device_, "vkDestroyImage", vk_.destroy_image) &&
      load_dev_fn(vk_, device_, "vkGetImageMemoryRequirements", vk_.get_image_memory_requirements) &&
      load_dev_fn(vk_, device_, "vkBindImageMemory", vk_.bind_image_memory) &&
      load_dev_fn(vk_, device_, "vkAllocateMemory", vk_.allocate_memory) &&
      load_dev_fn(vk_, device_, "vkFreeMemory", vk_.free_memory) &&
      load_dev_fn(vk_, device_, "vkMapMemory", vk_.map_memory) &&
      load_dev_fn(vk_, device_, "vkUnmapMemory", vk_.unmap_memory) &&
      load_dev_fn(vk_, device_, "vkCreateImageView", vk_.create_image_view) &&
      load_dev_fn(vk_, device_, "vkDestroyImageView", vk_.destroy_image_view) &&
      load_dev_fn(vk_, device_, "vkCreateSampler", vk_.create_sampler) &&
      load_dev_fn(vk_, device_, "vkDestroySampler", vk_.destroy_sampler) &&
      load_dev_fn(vk_, device_, "vkCreateShaderModule", vk_.create_shader_module) &&
      load_dev_fn(vk_, device_, "vkDestroyShaderModule", vk_.destroy_shader_module) &&
      load_dev_fn(vk_, device_, "vkCreateRenderPass", vk_.create_render_pass) &&
      load_dev_fn(vk_, device_, "vkDestroyRenderPass", vk_.destroy_render_pass) &&
      load_dev_fn(vk_, device_, "vkCreateFramebuffer", vk_.create_framebuffer) &&
      load_dev_fn(vk_, device_, "vkDestroyFramebuffer", vk_.destroy_framebuffer) &&
      load_dev_fn(vk_, device_, "vkCreateDescriptorSetLayout", vk_.create_descriptor_set_layout) &&
      load_dev_fn(vk_, device_, "vkDestroyDescriptorSetLayout", vk_.destroy_descriptor_set_layout) &&
      load_dev_fn(vk_, device_, "vkCreatePipelineLayout", vk_.create_pipeline_layout) &&
      load_dev_fn(vk_, device_, "vkDestroyPipelineLayout", vk_.destroy_pipeline_layout) &&
      load_dev_fn(vk_, device_, "vkCreateGraphicsPipelines", vk_.create_graphics_pipelines) &&
      load_dev_fn(vk_, device_, "vkDestroyPipeline", vk_.destroy_pipeline) &&
      load_dev_fn(vk_, device_, "vkCreateDescriptorPool", vk_.create_descriptor_pool) &&
      load_dev_fn(vk_, device_, "vkDestroyDescriptorPool", vk_.destroy_descriptor_pool) &&
      load_dev_fn(vk_, device_, "vkAllocateDescriptorSets", vk_.allocate_descriptor_sets) &&
      load_dev_fn(vk_, device_, "vkUpdateDescriptorSets", vk_.update_descriptor_sets) &&
      load_dev_fn(vk_, device_, "vkCreateCommandPool", vk_.create_command_pool) &&
      load_dev_fn(vk_, device_, "vkDestroyCommandPool", vk_.destroy_command_pool) &&
      load_dev_fn(vk_, device_, "vkAllocateCommandBuffers", vk_.allocate_command_buffers) &&
      load_dev_fn(vk_, device_, "vkResetCommandBuffer", vk_.reset_command_buffer) &&
      load_dev_fn(vk_, device_, "vkBeginCommandBuffer", vk_.begin_command_buffer) &&
      load_dev_fn(vk_, device_, "vkEndCommandBuffer", vk_.end_command_buffer) &&
      load_dev_fn(vk_, device_, "vkCmdBeginRenderPass", vk_.cmd_begin_render_pass) &&
      load_dev_fn(vk_, device_, "vkCmdEndRenderPass", vk_.cmd_end_render_pass) &&
      load_dev_fn(vk_, device_, "vkCmdBindPipeline", vk_.cmd_bind_pipeline) &&
      load_dev_fn(vk_, device_, "vkCmdBindDescriptorSets", vk_.cmd_bind_descriptor_sets) &&
      load_dev_fn(vk_, device_, "vkCmdPushConstants", vk_.cmd_push_constants) &&
      load_dev_fn(vk_, device_, "vkCmdSetViewport", vk_.cmd_set_viewport) &&
      load_dev_fn(vk_, device_, "vkCmdSetScissor", vk_.cmd_set_scissor) &&
      load_dev_fn(vk_, device_, "vkCmdDraw", vk_.cmd_draw) &&
      load_dev_fn(vk_, device_, "vkCmdPipelineBarrier", vk_.cmd_pipeline_barrier) &&
      load_dev_fn(vk_, device_, "vkCmdCopyBufferToImage", vk_.cmd_copy_buffer_to_image) &&
      load_dev_fn(vk_, device_, "vkCmdCopyImageToBuffer", vk_.cmd_copy_image_to_buffer) &&
      load_dev_fn(vk_, device_, "vkCreateSemaphore", vk_.create_semaphore) &&
      load_dev_fn(vk_, device_, "vkDestroySemaphore", vk_.destroy_semaphore) &&
      load_dev_fn(vk_, device_, "vkCreateFence", vk_.create_fence) &&
      load_dev_fn(vk_, device_, "vkDestroyFence", vk_.destroy_fence) &&
      load_dev_fn(vk_, device_, "vkWaitForFences", vk_.wait_for_fences) &&
      load_dev_fn(vk_, device_, "vkResetFences", vk_.reset_fences) &&
      load_dev_fn(vk_, device_, "vkQueueSubmit", vk_.queue_submit) &&
      load_dev_fn(vk_, device_, "vkQueueWaitIdle", vk_.queue_wait_idle);
  if (!ok) {
    err = "failed to load Vulkan device functions";
    return false;
  }
  if (!offscreen_) {
    const bool sc_ok =
        load_dev_fn(vk_, device_, "vkCreateSwapchainKHR", vk_.create_swapchain_khr) &&
        load_dev_fn(vk_, device_, "vkDestroySwapchainKHR", vk_.destroy_swapchain_khr) &&
        load_dev_fn(vk_, device_, "vkGetSwapchainImagesKHR", vk_.get_swapchain_images_khr) &&
        load_dev_fn(vk_, device_, "vkAcquireNextImageKHR", vk_.acquire_next_image_khr) &&
        load_dev_fn(vk_, device_, "vkQueuePresentKHR", vk_.queue_present_khr);
    if (!sc_ok) {
      err = "failed to load VK_KHR_swapchain functions";
      return false;
    }
  }
  return true;
}

bool VkRenderer::create_instance(const std::vector<const char*>& extensions, bool enable_validation, std::string& err) {
  std::vector<const char*> layers;
  if (enable_validation) {
    std::uint32_t count = 0;
    vk_.enumerate_instance_layer_properties(&count, nullptr);
    std::vector<VkLayerProperties> avail(count);
    vk_.enumerate_instance_layer_properties(&count, avail.data());
    for (const auto& l : avail) {
      if (std::string_view{l.layerName} == "VK_LAYER_KHRONOS_validation") {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        validation_active_ = true;
        break;
      }
    }
  }

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "Orangutan Personal Agent";
  app.applicationVersion = VK_MAKE_API_VERSION(0, 2, 0, 0);
  app.pEngineName = "OrangutanVkUI";
  app.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
  app.apiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);

  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  ci.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
  ci.ppEnabledLayerNames = layers.data();
  ci.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();

  const VkResult r = vk_.create_instance(&ci, nullptr, &instance_);
  if (r != VK_SUCCESS) {
    err = std::format("vkCreateInstance failed ({})", static_cast<int>(r));
    return false;
  }
  return load_instance_api(err);
}

bool VkRenderer::pick_device_and_create(bool require_present, std::string& err) {
  std::uint32_t dev_count = 0;
  vk_.enumerate_physical_devices(instance_, &dev_count, nullptr);
  if (dev_count == 0) {
    err = "no Vulkan physical devices found";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(dev_count);
  vk_.enumerate_physical_devices(instance_, &dev_count, devices.data());

  int best_score = -1;
  for (VkPhysicalDevice pd : devices) {
    std::uint32_t qcount = 0;
    vk_.get_physical_device_queue_family_properties(pd, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vk_.get_physical_device_queue_family_properties(pd, &qcount, qprops.data());

    std::uint32_t found_family = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t i = 0; i < qcount; ++i) {
      if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
        continue;
      }
      if (require_present) {
        VkBool32 present = VK_FALSE;
        vk_.get_physical_device_surface_support_khr(pd, i, surface_, &present);
        if (present == VK_FALSE) {
          continue;
        }
      }
      found_family = i;
      break;
    }
    if (found_family == std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }

    VkPhysicalDeviceProperties props{};
    vk_.get_physical_device_properties(pd, &props);
    int score = 10;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      score = 100;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
      score = 80;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
      score = 20;
    }
    if (score > best_score) {
      best_score = score;
      physical_device_ = pd;
      graphics_family_ = found_family;
      device_name_ = props.deviceName;
    }
  }

  if (physical_device_ == VK_NULL_HANDLE) {
    err = "no suitable Vulkan graphics queue family found";
    return false;
  }
  vk_.get_physical_device_memory_properties(physical_device_, &mem_props_);

  const float prio = 1.0F;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = graphics_family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  std::vector<const char*> dev_exts;
  if (require_present) {
    dev_exts.push_back("VK_KHR_swapchain");
  }

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
  dci.ppEnabledExtensionNames = dev_exts.data();

  const VkResult r = vk_.create_device(physical_device_, &dci, nullptr, &device_);
  if (r != VK_SUCCESS) {
    err = std::format("vkCreateDevice failed ({})", static_cast<int>(r));
    return false;
  }
  if (!load_device_api(err)) {
    return false;
  }
  vk_.get_device_queue(device_, graphics_family_, 0, &graphics_queue_);
  return true;
}

std::uint32_t VkRenderer::find_memory_type(std::uint32_t type_bits, VkFlags required_props) const {
  for (std::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
    if ((type_bits & (1U << i)) != 0U && (mem_props_.memoryTypes[i].propertyFlags & required_props) == required_props) {
      return i;
    }
  }
  return std::numeric_limits<std::uint32_t>::max();
}

bool VkRenderer::create_buffer(VkDeviceSize size, VkFlags usage, VkFlags mem_props, VkBuffer& buf, VkDeviceMemory& mem) {
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vk_.create_buffer(device_, &bci, nullptr, &buf) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  vk_.get_buffer_memory_requirements(device_, buf, &req);
  const auto mt = find_memory_type(req.memoryTypeBits, mem_props);
  if (mt == std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;
  if (vk_.allocate_memory(device_, &mai, nullptr, &mem) != VK_SUCCESS) {
    return false;
  }
  return vk_.bind_buffer_memory(device_, buf, mem, 0) == VK_SUCCESS;
}

bool VkRenderer::create_atlas_resources(std::string& err) {
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8_UNORM;
  ici.extent = VkExtent3D{FontAtlas::kAtlasSize, FontAtlas::kAtlasSize, 1U};
  ici.mipLevels = 1U;
  ici.arrayLayers = 1U;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk_.create_image(device_, &ici, nullptr, &atlas_image_) != VK_SUCCESS) {
    err = "failed to create glyph atlas VkImage";
    return false;
  }

  VkMemoryRequirements req{};
  vk_.get_image_memory_requirements(device_, atlas_image_, &req);
  const auto mt = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mt == std::numeric_limits<std::uint32_t>::max()) {
    err = "no device-local memory type for glyph atlas";
    return false;
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;
  if (vk_.allocate_memory(device_, &mai, nullptr, &atlas_mem_) != VK_SUCCESS ||
      vk_.bind_image_memory(device_, atlas_image_, atlas_mem_, 0) != VK_SUCCESS) {
    err = "failed to allocate/bind glyph atlas memory";
    return false;
  }

  VkImageViewCreateInfo ivci{};
  ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ivci.image = atlas_image_;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = VK_FORMAT_R8_UNORM;
  ivci.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vk_.create_image_view(device_, &ivci, nullptr, &atlas_view_) != VK_SUCCESS) {
    err = "failed to create glyph atlas image view";
    return false;
  }

  VkSamplerCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxAnisotropy = 1.0F;
  if (vk_.create_sampler(device_, &sci, nullptr, &atlas_sampler_) != VK_SUCCESS) {
    err = "failed to create glyph atlas sampler";
    return false;
  }

  const VkDeviceSize atlas_bytes = static_cast<VkDeviceSize>(FontAtlas::kAtlasSize) * FontAtlas::kAtlasSize;
  if (!create_buffer(atlas_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, atlas_staging_buf_,
                     atlas_staging_mem_) ||
      vk_.map_memory(device_, atlas_staging_mem_, 0, atlas_bytes, 0, &atlas_staging_mapped_) != VK_SUCCESS) {
    err = "failed to create glyph atlas staging buffer";
    return false;
  }
  return true;
}

bool VkRenderer::create_companion_atlas_resources(std::string& err) {
  constexpr std::uint32_t kTile = 512U;
  constexpr std::uint32_t kAtlasW = kTile * 3U;  // 1536
  constexpr std::uint32_t kAtlasH = kTile * 2U;  // 1024

  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = VkExtent3D{kAtlasW, kAtlasH, 1U};
  ici.mipLevels = 1U;
  ici.arrayLayers = 1U;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk_.create_image(device_, &ici, nullptr, &companion_image_) != VK_SUCCESS) {
    err = "failed to create companion atlas VkImage";
    return false;
  }

  VkMemoryRequirements req{};
  vk_.get_image_memory_requirements(device_, companion_image_, &req);
  const auto mt = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mt == std::numeric_limits<std::uint32_t>::max()) {
    err = "no device-local memory type for companion atlas";
    return false;
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;
  if (vk_.allocate_memory(device_, &mai, nullptr, &companion_mem_) != VK_SUCCESS ||
      vk_.bind_image_memory(device_, companion_image_, companion_mem_, 0) != VK_SUCCESS) {
    err = "failed to bind companion atlas memory";
    return false;
  }

  VkImageViewCreateInfo ivci{};
  ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ivci.image = companion_image_;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ivci.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vk_.create_image_view(device_, &ivci, nullptr, &companion_view_) != VK_SUCCESS) {
    err = "failed to create companion atlas image view";
    return false;
  }

  VkSamplerCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxAnisotropy = 1.0F;
  if (vk_.create_sampler(device_, &sci, nullptr, &companion_sampler_) != VK_SUCCESS) {
    err = "failed to create companion atlas sampler";
    return false;
  }

  const VkDeviceSize bytes = static_cast<VkDeviceSize>(kAtlasW) * kAtlasH * 4ULL;
  if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, companion_staging_buf_,
                     companion_staging_mem_)) {
    err = "failed to create companion staging buffer";
    return false;
  }

  void* mapped = nullptr;
  if (vk_.map_memory(device_, companion_staging_mem_, 0, bytes, 0, &mapped) != VK_SUCCESS) {
    err = "failed to map companion staging buffer";
    return false;
  }
  auto* dst_base = static_cast<std::uint8_t*>(mapped);
  std::memset(dst_base, 0xF6, static_cast<std::size_t>(bytes));

  const auto asset_dir = find_companion_asset_dir();
  if (!asset_dir.empty()) {
    constexpr std::array<const char*, 6> kFiles{"oran.png", "scout.png", "moss.png", "fern.png", "pebble.png", "pip.png"};
    std::vector<std::uint8_t> tile_rgba;
    int loaded_count = 0;
    for (std::size_t i = 0; i < kFiles.size(); ++i) {
      if (read_rgba_png_512(asset_dir / kFiles[i], tile_rgba)) {
        const std::size_t col = i % 3U;
        const std::size_t row = i / 3U;
        for (std::size_t y = 0; y < kTile; ++y) {
          const std::size_t dst_off = ((row * kTile + y) * kAtlasW + col * kTile) * 4ULL;
          const std::size_t src_off = y * kTile * 4ULL;
          std::memcpy(dst_base + dst_off, tile_rgba.data() + src_off, kTile * 4ULL);
        }
        ++loaded_count;
      }
    }
    companion_loaded_ = (loaded_count == 6);
  }
  vk_.unmap_memory(device_, companion_staging_mem_);
  return true;
}

bool VkRenderer::create_core_resources(VkFormat target_format, VkImageLayout final_layout, std::string& err) {
  color_format_ = target_format;
  target_is_srgb_ = (target_format == VK_FORMAT_B8G8R8A8_SRGB || target_format == VK_FORMAT_R8G8B8A8_SRGB);

  VkAttachmentDescription color_att{};
  color_att.format = color_format_;
  color_att.samples = VK_SAMPLE_COUNT_1_BIT;
  color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color_att.finalLayout = final_layout;

  VkAttachmentReference color_ref{};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = 0;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &color_att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &dep;
  if (vk_.create_render_pass(device_, &rpci, nullptr, &render_pass_) != VK_SUCCESS) {
    err = "failed to create Vulkan render pass";
    return false;
  }

  std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  bindings[2].binding = 2;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[2].descriptorCount = 1;
  bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo dslci{};
  dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dslci.bindingCount = static_cast<std::uint32_t>(bindings.size());
  dslci.pBindings = bindings.data();
  if (vk_.create_descriptor_set_layout(device_, &dslci, nullptr, &desc_layout_) != VK_SUCCESS) {
    err = "failed to create descriptor set layout";
    return false;
  }

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pcr.offset = 0;
  pcr.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &desc_layout_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  if (vk_.create_pipeline_layout(device_, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
    err = "failed to create pipeline layout";
    return false;
  }

  VkShaderModule vert_mod{VK_NULL_HANDLE};
  VkShaderModule frag_mod{VK_NULL_HANDLE};
  VkShaderModuleCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  vci.codeSize = kUiVertSpv.size() * sizeof(std::uint32_t);
  vci.pCode = kUiVertSpv.data();
  VkShaderModuleCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  fci.codeSize = kUiFragSpv.size() * sizeof(std::uint32_t);
  fci.pCode = kUiFragSpv.data();
  if (vk_.create_shader_module(device_, &vci, nullptr, &vert_mod) != VK_SUCCESS ||
      vk_.create_shader_module(device_, &fci, nullptr, &frag_mod) != VK_SUCCESS) {
    err = "failed to create SPIR-V shader modules";
    return false;
  }

  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_mod;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_mod;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp{};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rs.lineWidth = 1.0F;

  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState cba{};
  cba.blendEnable = VK_TRUE;
  cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.colorBlendOp = VK_BLEND_OP_ADD;
  cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.alphaBlendOp = VK_BLEND_OP_ADD;
  cba.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.logicOp = VK_LOGIC_OP_COPY;
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;

  std::array<VkDynamicState, 2> dyn_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{};
  ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  ds.dynamicStateCount = static_cast<std::uint32_t>(dyn_states.size());
  ds.pDynamicStates = dyn_states.data();

  VkGraphicsPipelineCreateInfo gpci{};
  gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpci.stageCount = static_cast<std::uint32_t>(stages.size());
  gpci.pStages = stages.data();
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &cb;
  gpci.pDynamicState = &ds;
  gpci.layout = pipeline_layout_;
  gpci.renderPass = render_pass_;
  gpci.subpass = 0;

  const VkResult pipe_res = vk_.create_graphics_pipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_);
  vk_.destroy_shader_module(device_, vert_mod, nullptr);
  vk_.destroy_shader_module(device_, frag_mod, nullptr);
  if (pipe_res != VK_SUCCESS) {
    err = "failed to create Vulkan graphics pipeline";
    return false;
  }

  if (!create_atlas_resources(err) || !create_companion_atlas_resources(err)) {
    return false;
  }

  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = graphics_family_;
  if (vk_.create_command_pool(device_, &cpci, nullptr, &cmd_pool_) != VK_SUCCESS) {
    err = "failed to create Vulkan command pool";
    return false;
  }

  std::array<VkDescriptorPoolSize, 2> pool_sizes{{
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 2U},
  }};
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = kFramesInFlight;
  dpci.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
  dpci.pPoolSizes = pool_sizes.data();
  if (vk_.create_descriptor_pool(device_, &dpci, nullptr, &desc_pool_) != VK_SUCCESS) {
    err = "failed to create descriptor pool";
    return false;
  }

  const VkDeviceSize inst_bytes = static_cast<VkDeviceSize>(kMaxInstances) * sizeof(UiInstance);
  for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
    auto& f = frames_[i];
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vk_.allocate_command_buffers(device_, &cbai, &f.cmd) != VK_SUCCESS) {
      err = "failed to allocate command buffer";
      return false;
    }

    VkSemaphoreCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fnci{};
    fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fnci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vk_.create_semaphore(device_, &smci, nullptr, &f.image_available) != VK_SUCCESS ||
        vk_.create_semaphore(device_, &smci, nullptr, &f.render_finished) != VK_SUCCESS ||
        vk_.create_fence(device_, &fnci, nullptr, &f.in_flight) != VK_SUCCESS) {
      err = "failed to create sync primitives";
      return false;
    }

    if (!create_buffer(inst_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, f.instance_buf,
                       f.instance_mem) ||
        vk_.map_memory(device_, f.instance_mem, 0, inst_bytes, 0, &f.instance_mapped) != VK_SUCCESS) {
      err = "failed to create per-frame instance SSBO";
      return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = desc_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &desc_layout_;
    if (vk_.allocate_descriptor_sets(device_, &dsai, &f.desc_set) != VK_SUCCESS) {
      err = "failed to allocate descriptor set";
      return false;
    }

    VkDescriptorBufferInfo dbi{};
    dbi.buffer = f.instance_buf;
    dbi.offset = 0;
    dbi.range = inst_bytes;

    VkDescriptorImageInfo dii{};
    dii.sampler = atlas_sampler_;
    dii.imageView = atlas_view_;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo cii{};
    cii.sampler = companion_sampler_;
    cii.imageView = companion_view_;
    cii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = f.desc_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &dbi;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = f.desc_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &dii;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = f.desc_set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &cii;

    vk_.update_descriptor_sets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
  }
  return true;
}

bool VkRenderer::create_swapchain(std::uint32_t w, std::uint32_t h, std::string& err) {
  VkSurfaceCapabilitiesKHR caps{};
  vk_.get_physical_device_surface_capabilities_khr(physical_device_, surface_, &caps);

  std::uint32_t fmt_count = 0;
  vk_.get_physical_device_surface_formats_khr(physical_device_, surface_, &fmt_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fmt_count);
  vk_.get_physical_device_surface_formats_khr(physical_device_, surface_, &fmt_count, formats.data());
  if (formats.empty()) {
    err = "no Vulkan surface formats available";
    return false;
  }

  VkSurfaceFormatKHR chosen = formats[0];
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      chosen = f;
      break;
    }
  }

  std::uint32_t pm_count = 0;
  vk_.get_physical_device_surface_present_modes_khr(physical_device_, surface_, &pm_count, nullptr);
  std::vector<VkPresentModeKHR> modes(pm_count);
  vk_.get_physical_device_surface_present_modes_khr(physical_device_, surface_, &pm_count, modes.data());
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  for (auto m : modes) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
      present_mode = m;
      break;
    }
  }

  if (caps.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
    extent_ = caps.currentExtent;
  } else {
    extent_.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent_.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
  }
  extent_.width = std::max(extent_.width, 1U);
  extent_.height = std::max(extent_.height, 1U);

  std::uint32_t img_count = caps.minImageCount + 1U;
  if (caps.maxImageCount > 0U && img_count > caps.maxImageCount) {
    img_count = caps.maxImageCount;
  }

  VkSwapchainCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.surface = surface_;
  sci.minImageCount = img_count;
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = extent_;
  sci.imageArrayLayers = 1U;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = present_mode;
  sci.clipped = VK_TRUE;

  if (vk_.create_swapchain_khr(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) {
    err = "failed to create Vulkan swapchain";
    return false;
  }

  std::uint32_t sc_count = 0;
  vk_.get_swapchain_images_khr(device_, swapchain_, &sc_count, nullptr);
  swapchain_images_.resize(sc_count);
  vk_.get_swapchain_images_khr(device_, swapchain_, &sc_count, swapchain_images_.data());

  if (render_pass_ == VK_NULL_HANDLE) {
    if (!create_core_resources(chosen.format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, err)) {
      return false;
    }
  }

  swapchain_views_.resize(sc_count, VK_NULL_HANDLE);
  framebuffers_.resize(sc_count, VK_NULL_HANDLE);
  for (std::uint32_t i = 0; i < sc_count; ++i) {
    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = swapchain_images_[i];
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = chosen.format;
    ivci.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk_.create_image_view(device_, &ivci, nullptr, &swapchain_views_[i]) != VK_SUCCESS) {
      err = "failed to create swapchain image view";
      return false;
    }

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = render_pass_;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &swapchain_views_[i];
    fbci.width = extent_.width;
    fbci.height = extent_.height;
    fbci.layers = 1;
    if (vk_.create_framebuffer(device_, &fbci, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
      err = "failed to create swapchain framebuffer";
      return false;
    }
  }
  swapchain_dirty_ = false;
  return true;
}

void VkRenderer::destroy_swapchain() {
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  for (auto fb : framebuffers_) {
    if (fb != VK_NULL_HANDLE) {
      vk_.destroy_framebuffer(device_, fb, nullptr);
    }
  }
  framebuffers_.clear();
  for (auto iv : swapchain_views_) {
    if (iv != VK_NULL_HANDLE) {
      vk_.destroy_image_view(device_, iv, nullptr);
    }
  }
  swapchain_views_.clear();
  swapchain_images_.clear();
  if (swapchain_ != VK_NULL_HANDLE) {
    vk_.destroy_swapchain_khr(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

bool VkRenderer::create_offscreen_target(std::uint32_t w, std::uint32_t h, std::string& err) {
  extent_ = VkExtent2D{std::max(w, 1U), std::max(h, 1U)};

  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = VkExtent3D{extent_.width, extent_.height, 1U};
  ici.mipLevels = 1U;
  ici.arrayLayers = 1U;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk_.create_image(device_, &ici, nullptr, &offscreen_image_) != VK_SUCCESS) {
    err = "failed to create offscreen VkImage";
    return false;
  }

  VkMemoryRequirements req{};
  vk_.get_image_memory_requirements(device_, offscreen_image_, &req);
  const auto mt = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mt == std::numeric_limits<std::uint32_t>::max()) {
    err = "no memory type for offscreen image";
    return false;
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;
  if (vk_.allocate_memory(device_, &mai, nullptr, &offscreen_mem_) != VK_SUCCESS ||
      vk_.bind_image_memory(device_, offscreen_image_, offscreen_mem_, 0) != VK_SUCCESS) {
    err = "failed to bind offscreen image memory";
    return false;
  }

  VkImageViewCreateInfo ivci{};
  ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ivci.image = offscreen_image_;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ivci.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vk_.create_image_view(device_, &ivci, nullptr, &offscreen_view_) != VK_SUCCESS) {
    err = "failed to create offscreen image view";
    return false;
  }

  if (!create_core_resources(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, err)) {
    return false;
  }

  VkFramebufferCreateInfo fbci{};
  fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbci.renderPass = render_pass_;
  fbci.attachmentCount = 1;
  fbci.pAttachments = &offscreen_view_;
  fbci.width = extent_.width;
  fbci.height = extent_.height;
  fbci.layers = 1;
  if (vk_.create_framebuffer(device_, &fbci, nullptr, &offscreen_fb_) != VK_SUCCESS) {
    err = "failed to create offscreen framebuffer";
    return false;
  }

  const VkDeviceSize read_bytes = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4ULL;
  if (!create_buffer(read_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback_buf_,
                     readback_mem_)) {
    err = "failed to create offscreen readback buffer";
    return false;
  }
  return true;
}

bool VkRenderer::init_window(SDL_Window* window, bool enable_validation, std::string& error_out) {
  offscreen_ = false;
  window_ = window;

  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (!load_global_api(gipa, error_out)) {
    return false;
  }

  Uint32 ext_count = 0;
  const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
  if (sdl_exts == nullptr || ext_count == 0) {
    error_out = std::format("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
    return false;
  }
  std::vector<const char*> exts(sdl_exts, sdl_exts + ext_count);
  if (!create_instance(exts, enable_validation, error_out)) {
    return false;
  }

  if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
    error_out = std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
    return false;
  }

  if (!pick_device_and_create(true, error_out)) {
    return false;
  }

  int pw = 1280;
  int ph = 820;
  SDL_GetWindowSizeInPixels(window_, &pw, &ph);
  return create_swapchain(static_cast<std::uint32_t>(std::max(pw, 1)), static_cast<std::uint32_t>(std::max(ph, 1)),
                          error_out);
}

bool VkRenderer::init_offscreen(std::uint32_t width, std::uint32_t height, bool enable_validation,
                                std::string& error_out) {
  offscreen_ = true;
  dl_vulkan_ = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (dl_vulkan_ == nullptr) {
    dl_vulkan_ = ::dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  }
  if (dl_vulkan_ == nullptr) {
    error_out = "failed to dlopen libvulkan.so.1";
    return false;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(dl_vulkan_, "vkGetInstanceProcAddr"));
  if (!load_global_api(gipa, error_out)) {
    return false;
  }
  if (!create_instance({}, enable_validation, error_out)) {
    return false;
  }
  if (!pick_device_and_create(false, error_out)) {
    return false;
  }
  return create_offscreen_target(width, height, error_out);
}

bool VkRenderer::upload_atlas_if_dirty(VkCommandBuffer cmd, FontAtlas& atlas) {
  if (!companion_uploaded_ && companion_image_ != VK_NULL_HANDLE) {
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = companion_image_;
    to_dst.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vk_.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_dst);

    VkBufferImageCopy region{};
    region.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = VkExtent3D{1536U, 1024U, 1U};
    vk_.cmd_copy_buffer_to_image(cmd, companion_staging_buf_, companion_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &region);

    VkImageMemoryBarrier to_read{};
    to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.image = companion_image_;
    to_read.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vk_.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_read);
    companion_uploaded_ = true;
  }

  if (!atlas.dirty() && atlas_initialized_) {
    return true;
  }
  const auto& px = atlas.pixels();
  std::memcpy(atlas_staging_mapped_, px.data(), px.size());

  VkImageMemoryBarrier to_dst{};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.oldLayout = atlas_initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = atlas_image_;
  to_dst.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_dst.srcAccessMask = atlas_initialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

  vk_.cmd_pipeline_barrier(cmd,
                           atlas_initialized_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

  VkBufferImageCopy region{};
  region.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = VkExtent3D{FontAtlas::kAtlasSize, FontAtlas::kAtlasSize, 1U};
  vk_.cmd_copy_buffer_to_image(cmd, atlas_staging_buf_, atlas_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier to_read{};
  to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_read.image = atlas_image_;
  to_read.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  vk_.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &to_read);

  atlas.clear_dirty();
  atlas_initialized_ = true;
  return true;
}

void VkRenderer::on_resize(std::uint32_t width, std::uint32_t height) {
  if (width > 0 && height > 0) {
    extent_ = VkExtent2D{width, height};
    swapchain_dirty_ = true;
  }
}

bool VkRenderer::render_frame(const DrawList& dl, FontAtlas& atlas, Color clear_color) {
  if (offscreen_ || device_ == VK_NULL_HANDLE) {
    return false;
  }
  if (swapchain_dirty_) {
    vk_.device_wait_idle(device_);
    destroy_swapchain();
    std::string err;
    int pw = 1280;
    int ph = 820;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    if (pw <= 0 || ph <= 0) {
      return true;
    }
    if (!create_swapchain(static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph), err)) {
      return false;
    }
  }

  auto& f = frames_[frame_index_];
  vk_.wait_for_fences(device_, 1, &f.in_flight, VK_TRUE, std::numeric_limits<std::uint64_t>::max());

  std::uint32_t img_index = 0;
  const VkResult acq = vk_.acquire_next_image_khr(device_, swapchain_, std::numeric_limits<std::uint64_t>::max(),
                                                  f.image_available, VK_NULL_HANDLE, &img_index);
  if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
    swapchain_dirty_ = true;
    return true;
  }
  if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
    return false;
  }

  vk_.reset_fences(device_, 1, &f.in_flight);
  vk_.reset_command_buffer(f.cmd, 0);

  const auto& insts = dl.instances();
  const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::size_t>(insts.size(), kMaxInstances));
  if (count > 0) {
    std::memcpy(f.instance_mapped, insts.data(), static_cast<std::size_t>(count) * sizeof(UiInstance));
  }

  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk_.begin_command_buffer(f.cmd, &bi);

  upload_atlas_if_dirty(f.cmd, atlas);

  VkClearValue clear{};
  clear.color.float32[0] = target_is_srgb_ ? srgb_to_linear_channel(clear_color.r) : clear_color.r;
  clear.color.float32[1] = target_is_srgb_ ? srgb_to_linear_channel(clear_color.g) : clear_color.g;
  clear.color.float32[2] = target_is_srgb_ ? srgb_to_linear_channel(clear_color.b) : clear_color.b;
  clear.color.float32[3] = 1.0F;

  VkRenderPassBeginInfo rpbi{};
  rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpbi.renderPass = render_pass_;
  rpbi.framebuffer = framebuffers_[img_index];
  rpbi.renderArea = VkRect2D{{0, 0}, extent_};
  rpbi.clearValueCount = 1;
  rpbi.pClearValues = &clear;

  vk_.cmd_begin_render_pass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  if (count > 0) {
    vk_.cmd_bind_pipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vk_.cmd_bind_descriptor_sets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &f.desc_set, 0, nullptr);
    const VkViewport vp{0.0F, 0.0F, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0F, 1.0F};
    const VkRect2D sc{{0, 0}, extent_};
    vk_.cmd_set_viewport(f.cmd, 0, 1, &vp);
    vk_.cmd_set_scissor(f.cmd, 0, 1, &sc);
    const PushConstants pc{
        static_cast<float>(extent_.width),
        static_cast<float>(extent_.height),
        target_is_srgb_ ? 1.0F : 0.0F,
        dl.dpr(),
    };
    vk_.cmd_push_constants(f.cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);
    vk_.cmd_draw(f.cmd, 6U, count, 0U, 0U);
  }
  vk_.cmd_end_render_pass(f.cmd);
  vk_.end_command_buffer(f.cmd);

  const VkFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &f.image_available;
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &f.render_finished;
  if (vk_.queue_submit(graphics_queue_, 1, &si, f.in_flight) != VK_SUCCESS) {
    return false;
  }

  VkPresentInfoKHR pi{};
  pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &f.render_finished;
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &img_index;
  const VkResult pres = vk_.queue_present_khr(graphics_queue_, &pi);
  if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
    swapchain_dirty_ = true;
  }

  frame_index_ = (frame_index_ + 1U) % kFramesInFlight;
  return true;
}

bool VkRenderer::render_offscreen_png(const DrawList& dl, FontAtlas& atlas, Color clear_color,
                                      const std::string& png_path, std::string& error_out) {
  if (!offscreen_ || device_ == VK_NULL_HANDLE) {
    error_out = "renderer is not initialized in offscreen mode";
    return false;
  }

  auto& f = frames_[0];
  vk_.wait_for_fences(device_, 1, &f.in_flight, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
  vk_.reset_fences(device_, 1, &f.in_flight);
  vk_.reset_command_buffer(f.cmd, 0);

  const auto& insts = dl.instances();
  const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::size_t>(insts.size(), kMaxInstances));
  if (count > 0) {
    std::memcpy(f.instance_mapped, insts.data(), static_cast<std::size_t>(count) * sizeof(UiInstance));
  }

  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk_.begin_command_buffer(f.cmd, &bi);

  upload_atlas_if_dirty(f.cmd, atlas);

  VkClearValue clear{};
  clear.color.float32[0] = clear_color.r;
  clear.color.float32[1] = clear_color.g;
  clear.color.float32[2] = clear_color.b;
  clear.color.float32[3] = 1.0F;

  VkRenderPassBeginInfo rpbi{};
  rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpbi.renderPass = render_pass_;
  rpbi.framebuffer = offscreen_fb_;
  rpbi.renderArea = VkRect2D{{0, 0}, extent_};
  rpbi.clearValueCount = 1;
  rpbi.pClearValues = &clear;

  vk_.cmd_begin_render_pass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  if (count > 0) {
    vk_.cmd_bind_pipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vk_.cmd_bind_descriptor_sets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &f.desc_set, 0, nullptr);
    const VkViewport vp{0.0F, 0.0F, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0F, 1.0F};
    const VkRect2D sc{{0, 0}, extent_};
    vk_.cmd_set_viewport(f.cmd, 0, 1, &vp);
    vk_.cmd_set_scissor(f.cmd, 0, 1, &sc);
    const PushConstants pc{
        static_cast<float>(extent_.width),
        static_cast<float>(extent_.height),
        0.0F,
        dl.dpr(),
    };
    vk_.cmd_push_constants(f.cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);
    vk_.cmd_draw(f.cmd, 6U, count, 0U, 0U);
  }
  vk_.cmd_end_render_pass(f.cmd);

  // Copy rendered image from TRANSFER_SRC_OPTIMAL to host-visible readback buffer.
  VkBufferImageCopy copy{};
  copy.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = VkExtent3D{extent_.width, extent_.height, 1U};
  vk_.cmd_copy_image_to_buffer(f.cmd, offscreen_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf_, 1, &copy);

  vk_.end_command_buffer(f.cmd);

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cmd;
  if (vk_.queue_submit(graphics_queue_, 1, &si, f.in_flight) != VK_SUCCESS) {
    error_out = "vkQueueSubmit failed in offscreen render";
    return false;
  }
  vk_.wait_for_fences(device_, 1, &f.in_flight, VK_TRUE, std::numeric_limits<std::uint64_t>::max());

  void* mapped = nullptr;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4ULL;
  if (vk_.map_memory(device_, readback_mem_, 0, bytes, 0, &mapped) != VK_SUCCESS) {
    error_out = "failed to map readback buffer";
    return false;
  }
  const bool ok = write_rgba_png(png_path, extent_.width, extent_.height, static_cast<const std::uint8_t*>(mapped),
                                 error_out);
  vk_.unmap_memory(device_, readback_mem_);
  return ok;
}

void VkRenderer::shutdown() {
  if (device_ != VK_NULL_HANDLE) {
    vk_.device_wait_idle(device_);
    destroy_swapchain();

    for (auto& f : frames_) {
      if (f.instance_mapped != nullptr && f.instance_mem != VK_NULL_HANDLE) {
        vk_.unmap_memory(device_, f.instance_mem);
        f.instance_mapped = nullptr;
      }
      if (f.instance_buf != VK_NULL_HANDLE) {
        vk_.destroy_buffer(device_, f.instance_buf, nullptr);
        f.instance_buf = VK_NULL_HANDLE;
      }
      if (f.instance_mem != VK_NULL_HANDLE) {
        vk_.free_memory(device_, f.instance_mem, nullptr);
        f.instance_mem = VK_NULL_HANDLE;
      }
      if (f.in_flight != VK_NULL_HANDLE) {
        vk_.destroy_fence(device_, f.in_flight, nullptr);
        f.in_flight = VK_NULL_HANDLE;
      }
      if (f.render_finished != VK_NULL_HANDLE) {
        vk_.destroy_semaphore(device_, f.render_finished, nullptr);
        f.render_finished = VK_NULL_HANDLE;
      }
      if (f.image_available != VK_NULL_HANDLE) {
        vk_.destroy_semaphore(device_, f.image_available, nullptr);
        f.image_available = VK_NULL_HANDLE;
      }
    }

    if (atlas_staging_mapped_ != nullptr && atlas_staging_mem_ != VK_NULL_HANDLE) {
      vk_.unmap_memory(device_, atlas_staging_mem_);
      atlas_staging_mapped_ = nullptr;
    }
    if (atlas_staging_buf_ != VK_NULL_HANDLE) {
      vk_.destroy_buffer(device_, atlas_staging_buf_, nullptr);
      atlas_staging_buf_ = VK_NULL_HANDLE;
    }
    if (atlas_staging_mem_ != VK_NULL_HANDLE) {
      vk_.free_memory(device_, atlas_staging_mem_, nullptr);
      atlas_staging_mem_ = VK_NULL_HANDLE;
    }
    if (atlas_sampler_ != VK_NULL_HANDLE) {
      vk_.destroy_sampler(device_, atlas_sampler_, nullptr);
      atlas_sampler_ = VK_NULL_HANDLE;
    }
    if (atlas_view_ != VK_NULL_HANDLE) {
      vk_.destroy_image_view(device_, atlas_view_, nullptr);
      atlas_view_ = VK_NULL_HANDLE;
    }
    if (atlas_image_ != VK_NULL_HANDLE) {
      vk_.destroy_image(device_, atlas_image_, nullptr);
      atlas_image_ = VK_NULL_HANDLE;
    }
    if (atlas_mem_ != VK_NULL_HANDLE) {
      vk_.free_memory(device_, atlas_mem_, nullptr);
      atlas_mem_ = VK_NULL_HANDLE;
    }

    if (companion_staging_buf_ != VK_NULL_HANDLE) {
      vk_.destroy_buffer(device_, companion_staging_buf_, nullptr);
      companion_staging_buf_ = VK_NULL_HANDLE;
    }
    if (companion_staging_mem_ != VK_NULL_HANDLE) {
      vk_.free_memory(device_, companion_staging_mem_, nullptr);
      companion_staging_mem_ = VK_NULL_HANDLE;
    }
    if (companion_sampler_ != VK_NULL_HANDLE) {
      vk_.destroy_sampler(device_, companion_sampler_, nullptr);
      companion_sampler_ = VK_NULL_HANDLE;
    }
    if (companion_view_ != VK_NULL_HANDLE) {
      vk_.destroy_image_view(device_, companion_view_, nullptr);
      companion_view_ = VK_NULL_HANDLE;
    }
    if (companion_image_ != VK_NULL_HANDLE) {
      vk_.destroy_image(device_, companion_image_, nullptr);
      companion_image_ = VK_NULL_HANDLE;
    }
    if (companion_mem_ != VK_NULL_HANDLE) {
      vk_.free_memory(device_, companion_mem_, nullptr);
      companion_mem_ = VK_NULL_HANDLE;
    }

    if (offscreen_fb_ != VK_NULL_HANDLE) {
      vk_.destroy_framebuffer(device_, offscreen_fb_, nullptr);
      offscreen_fb_ = VK_NULL_HANDLE;
    }
    if (offscreen_view_ != VK_NULL_HANDLE) {
      vk_.destroy_image_view(device_, offscreen_view_, nullptr);
      offscreen_view_ = VK_NULL_HANDLE;
    }
    if (offscreen_image_ != VK_NULL_HANDLE) {
      vk_.destroy_image(device_, offscreen_image_, nullptr);
      offscreen_image_ = VK_NULL_HANDLE;
    }
    if (offscreen_mem_ != VK_NULL_HANDLE) {
      vk_.free_memory(device_, offscreen_mem_, nullptr);
      offscreen_mem_ = VK_NULL_HANDLE;
    }
    if (readback_buf_ != VK_NULL_HANDLE) {
      vk_.destroy_buffer(device_, readback_buf_, nullptr);
      readback_buf_ = VK_NULL_HANDLE;
    }
    if (readback_mem_ != VK_NULL_HANDLE) {
      vk_.free_memory(device_, readback_mem_, nullptr);
      readback_mem_ = VK_NULL_HANDLE;
    }

    if (desc_pool_ != VK_NULL_HANDLE) {
      vk_.destroy_descriptor_pool(device_, desc_pool_, nullptr);
      desc_pool_ = VK_NULL_HANDLE;
    }
    if (cmd_pool_ != VK_NULL_HANDLE) {
      vk_.destroy_command_pool(device_, cmd_pool_, nullptr);
      cmd_pool_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
      vk_.destroy_pipeline(device_, pipeline_, nullptr);
      pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
      vk_.destroy_pipeline_layout(device_, pipeline_layout_, nullptr);
      pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (desc_layout_ != VK_NULL_HANDLE) {
      vk_.destroy_descriptor_set_layout(device_, desc_layout_, nullptr);
      desc_layout_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      vk_.destroy_render_pass(device_, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }

    vk_.destroy_device(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }

  if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
    vk_.destroy_surface_khr(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    vk_.destroy_instance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
  if (dl_vulkan_ != nullptr) {
    ::dlclose(dl_vulkan_);
    dl_vulkan_ = nullptr;
  }
}

}  // namespace orangutan::vkui
