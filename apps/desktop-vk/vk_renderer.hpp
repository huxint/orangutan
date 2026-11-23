#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "font_atlas.hpp"
#include "primitives.hpp"
#include "theme.hpp"
#include "vk_abi.hpp"

struct SDL_Window;

namespace orangutan::vkui {

class VkRenderer {
public:
  VkRenderer() = default;
  ~VkRenderer();

  VkRenderer(const VkRenderer&) = delete;
  VkRenderer& operator=(const VkRenderer&) = delete;

  bool init_window(SDL_Window* window, bool enable_validation, std::string& error_out);
  bool init_offscreen(std::uint32_t width, std::uint32_t height, bool enable_validation, std::string& error_out);

  bool render_frame(const DrawList& dl, FontAtlas& atlas, Color clear_color);
  bool render_offscreen_png(const DrawList& dl, FontAtlas& atlas, Color clear_color, const std::string& png_path,
                            std::string& error_out);

  void on_resize(std::uint32_t width, std::uint32_t height);
  void shutdown();

  [[nodiscard]] std::uint32_t width() const noexcept {
    return extent_.width;
  }
  [[nodiscard]] std::uint32_t height() const noexcept {
    return extent_.height;
  }
  [[nodiscard]] const std::string& device_name() const noexcept {
    return device_name_;
  }
  [[nodiscard]] bool validation_enabled() const noexcept {
    return validation_active_;
  }

private:
  static constexpr std::uint32_t kFramesInFlight = 2U;
  static constexpr std::uint32_t kMaxInstances = 32768U;

  struct FrameSync {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    VkSemaphore image_available{VK_NULL_HANDLE};
    VkSemaphore render_finished{VK_NULL_HANDLE};
    VkFence in_flight{VK_NULL_HANDLE};
    VkBuffer instance_buf{VK_NULL_HANDLE};
    VkDeviceMemory instance_mem{VK_NULL_HANDLE};
    void* instance_mapped{nullptr};
    VkDescriptorSet desc_set{VK_NULL_HANDLE};
  };

  bool load_global_api(PFN_vkGetInstanceProcAddr gipa, std::string& err);
  bool load_instance_api(std::string& err);
  bool load_device_api(std::string& err);
  bool create_instance(const std::vector<const char*>& extensions, bool enable_validation, std::string& err);
  bool pick_device_and_create(bool require_present, std::string& err);
  bool create_core_resources(VkFormat target_format, VkImageLayout final_layout, std::string& err);
  bool create_atlas_resources(std::string& err);
  bool create_swapchain(std::uint32_t w, std::uint32_t h, std::string& err);
  void destroy_swapchain();
  bool create_offscreen_target(std::uint32_t w, std::uint32_t h, std::string& err);
  bool upload_atlas_if_dirty(VkCommandBuffer cmd, FontAtlas& atlas);
  std::uint32_t find_memory_type(std::uint32_t type_bits, VkFlags required_props) const;
  bool create_buffer(VkDeviceSize size, VkFlags usage, VkFlags mem_props, VkBuffer& buf, VkDeviceMemory& mem);

  VkApi vk_{};
  void* dl_vulkan_{nullptr};
  SDL_Window* window_{nullptr};
  bool offscreen_{false};
  bool validation_active_{false};
  bool target_is_srgb_{false};
  bool swapchain_dirty_{false};
  std::string device_name_;

  VkInstance instance_{VK_NULL_HANDLE};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphics_queue_{VK_NULL_HANDLE};
  std::uint32_t graphics_family_{0U};
  VkPhysicalDeviceMemoryProperties mem_props_{};

  VkFormat color_format_{VK_FORMAT_B8G8R8A8_UNORM};
  VkExtent2D extent_{1280U, 820U};

  VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
  std::vector<VkImage> swapchain_images_;
  std::vector<VkImageView> swapchain_views_;
  std::vector<VkFramebuffer> framebuffers_;

  // Offscreen target resources
  VkImage offscreen_image_{VK_NULL_HANDLE};
  VkDeviceMemory offscreen_mem_{VK_NULL_HANDLE};
  VkImageView offscreen_view_{VK_NULL_HANDLE};
  VkFramebuffer offscreen_fb_{VK_NULL_HANDLE};
  VkBuffer readback_buf_{VK_NULL_HANDLE};
  VkDeviceMemory readback_mem_{VK_NULL_HANDLE};

  // Pipeline & descriptors
  VkRenderPass render_pass_{VK_NULL_HANDLE};
  VkDescriptorSetLayout desc_layout_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  VkDescriptorPool desc_pool_{VK_NULL_HANDLE};
  VkCommandPool cmd_pool_{VK_NULL_HANDLE};

  // Glyph atlas GPU resources
  VkImage atlas_image_{VK_NULL_HANDLE};
  VkDeviceMemory atlas_mem_{VK_NULL_HANDLE};
  VkImageView atlas_view_{VK_NULL_HANDLE};
  VkSampler atlas_sampler_{VK_NULL_HANDLE};
  VkBuffer atlas_staging_buf_{VK_NULL_HANDLE};
  VkDeviceMemory atlas_staging_mem_{VK_NULL_HANDLE};
  void* atlas_staging_mapped_{nullptr};
  bool atlas_initialized_{false};

  FrameSync frames_[kFramesInFlight]{};
  std::uint32_t frame_index_{0U};
};

}  // namespace orangutan::vkui
