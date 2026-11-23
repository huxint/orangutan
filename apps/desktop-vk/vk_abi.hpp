#pragma once

// Minimal Khronos Vulkan 1.0 + VK_KHR_surface + VK_KHR_swapchain C ABI layer.
// Uses official <vulkan/vulkan.h> whenever installed; otherwise provides the
// exact Khronos 64-bit C ABI declarations so pure Vulkan binaries build cleanly
// against libvulkan.so + SDL3 on systems without the vulkan-headers package.

#include <cstddef>
#include <cstdint>

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#else

#ifndef VULKAN_CORE_H_
#define VULKAN_CORE_H_ 1
#endif

extern "C" {

#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR

#define VK_MAKE_API_VERSION(variant, major, minor, patch) \
  ((((std::uint32_t)(variant)) << 29U) | (((std::uint32_t)(major)) << 22U) | (((std::uint32_t)(minor)) << 12U) | ((std::uint32_t)(patch)))

using VkFlags = std::uint32_t;
using VkBool32 = std::uint32_t;
using VkDeviceSize = std::uint64_t;
using VkSampleMask = std::uint32_t;

inline constexpr VkBool32 VK_FALSE = 0;
inline constexpr VkBool32 VK_TRUE = 1;
inline constexpr std::uint32_t VK_QUEUE_FAMILY_IGNORED = ~0U;
inline constexpr std::uint32_t VK_SUBPASS_EXTERNAL = ~0U;
inline constexpr VkDeviceSize VK_WHOLE_SIZE = ~0ULL;
inline constexpr std::uint32_t VK_MAX_PHYSICAL_DEVICE_NAME_SIZE = 256;
inline constexpr std::uint32_t VK_UUID_SIZE = 16;
inline constexpr std::uint32_t VK_MAX_MEMORY_TYPES = 32;
inline constexpr std::uint32_t VK_MAX_MEMORY_HEAPS = 16;
inline constexpr std::uint32_t VK_MAX_EXTENSION_NAME_SIZE = 256;
inline constexpr std::uint32_t VK_MAX_DESCRIPTION_SIZE = 256;

#define ORAN_VK_DEFINE_HANDLE(name) struct name##_T; using name = name##_T*;
#if defined(__LP64__) || defined(_WIN64) || (defined(__x86_64__) && !defined(__ILP32__))
#define ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(name) struct name##_T; using name = name##_T*;
#define VK_NULL_HANDLE nullptr
#else
#define ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(name) using name = std::uint64_t;
#define VK_NULL_HANDLE 0ULL
#endif

ORAN_VK_DEFINE_HANDLE(VkInstance)
ORAN_VK_DEFINE_HANDLE(VkPhysicalDevice)
ORAN_VK_DEFINE_HANDLE(VkDevice)
ORAN_VK_DEFINE_HANDLE(VkQueue)
ORAN_VK_DEFINE_HANDLE(VkCommandBuffer)

ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFence)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBuffer)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImage)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImageView)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkShaderModule)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineCache)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineLayout)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkRenderPass)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipeline)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSetLayout)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSampler)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorPool)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSet)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFramebuffer)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkCommandPool)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR)
ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSwapchainKHR)

#undef ORAN_VK_DEFINE_HANDLE
#undef ORAN_VK_DEFINE_NON_DISPATCHABLE_HANDLE

struct VkAllocationCallbacks;

enum VkResult : std::int32_t {
  VK_SUCCESS = 0,
  VK_NOT_READY = 1,
  VK_TIMEOUT = 2,
  VK_SUBOPTIMAL_KHR = 1000001003,
  VK_ERROR_OUT_OF_HOST_MEMORY = -1,
  VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
  VK_ERROR_INITIALIZATION_FAILED = -3,
  VK_ERROR_DEVICE_LOST = -4,
  VK_ERROR_LAYER_NOT_PRESENT = -6,
  VK_ERROR_EXTENSION_NOT_PRESENT = -7,
  VK_ERROR_SURFACE_LOST_KHR = -1000000000,
  VK_ERROR_OUT_OF_DATE_KHR = -1000001004,
};

enum VkStructureType : std::int32_t {
  VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
  VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
  VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
  VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
  VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
  VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
  VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
  VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
  VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
  VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
  VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
  VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
  VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
  VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19,
  VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
  VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22,
  VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
  VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24,
  VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26,
  VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27,
  VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
  VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
  VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31,
  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
  VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
  VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
  VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37,
  VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
  VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
  VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
  VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
  VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
  VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
};

enum VkFormat : std::int32_t {
  VK_FORMAT_UNDEFINED = 0,
  VK_FORMAT_R8_UNORM = 9,
  VK_FORMAT_R8G8B8A8_UNORM = 37,
  VK_FORMAT_R8G8B8A8_SRGB = 43,
  VK_FORMAT_B8G8R8A8_UNORM = 44,
  VK_FORMAT_B8G8R8A8_SRGB = 50,
};

enum VkColorSpaceKHR : std::int32_t {
  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0,
};

enum VkPresentModeKHR : std::int32_t {
  VK_PRESENT_MODE_IMMEDIATE_KHR = 0,
  VK_PRESENT_MODE_MAILBOX_KHR = 1,
  VK_PRESENT_MODE_FIFO_KHR = 2,
};

enum VkPhysicalDeviceType : std::int32_t {
  VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
  VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
  VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
  VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
  VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
};

enum VkImageType : std::int32_t { VK_IMAGE_TYPE_2D = 1 };
enum VkImageViewType : std::int32_t { VK_IMAGE_VIEW_TYPE_2D = 1 };
enum VkImageTiling : std::int32_t { VK_IMAGE_TILING_OPTIMAL = 0, VK_IMAGE_TILING_LINEAR = 1 };
enum VkSharingMode : std::int32_t { VK_SHARING_MODE_EXCLUSIVE = 0, VK_SHARING_MODE_CONCURRENT = 1 };

enum VkImageLayout : std::int32_t {
  VK_IMAGE_LAYOUT_UNDEFINED = 0,
  VK_IMAGE_LAYOUT_GENERAL = 1,
  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5,
  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,
  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002,
};

enum VkSampleCountFlagBits : std::uint32_t { VK_SAMPLE_COUNT_1_BIT = 0x00000001 };
enum VkComponentSwizzle : std::int32_t { VK_COMPONENT_SWIZZLE_IDENTITY = 0 };
enum VkAttachmentLoadOp : std::int32_t {
  VK_ATTACHMENT_LOAD_OP_LOAD = 0,
  VK_ATTACHMENT_LOAD_OP_CLEAR = 1,
  VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2,
};
enum VkAttachmentStoreOp : std::int32_t {
  VK_ATTACHMENT_STORE_OP_STORE = 0,
  VK_ATTACHMENT_STORE_OP_DONT_CARE = 1,
};
enum VkPipelineBindPoint : std::int32_t { VK_PIPELINE_BIND_POINT_GRAPHICS = 0 };
enum VkSubpassContents : std::int32_t { VK_SUBPASS_CONTENTS_INLINE = 0 };
enum VkCommandBufferLevel : std::int32_t { VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0 };
enum VkPrimitiveTopology : std::int32_t { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3 };
enum VkPolygonMode : std::int32_t { VK_POLYGON_MODE_FILL = 0 };
enum VkFrontFace : std::int32_t { VK_FRONT_FACE_COUNTER_CLOCKWISE = 0, VK_FRONT_FACE_CLOCKWISE = 1 };
enum VkBlendFactor : std::int32_t {
  VK_BLEND_FACTOR_ZERO = 0,
  VK_BLEND_FACTOR_ONE = 1,
  VK_BLEND_FACTOR_SRC_ALPHA = 6,
  VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7,
};
enum VkBlendOp : std::int32_t { VK_BLEND_OP_ADD = 0 };
enum VkLogicOp : std::int32_t { VK_LOGIC_OP_COPY = 3 };
enum VkDynamicState : std::int32_t {
  VK_DYNAMIC_STATE_VIEWPORT = 0,
  VK_DYNAMIC_STATE_SCISSOR = 1,
};
enum VkDescriptorType : std::int32_t {
  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1,
  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
};
enum VkFilter : std::int32_t { VK_FILTER_NEAREST = 0, VK_FILTER_LINEAR = 1 };
enum VkSamplerMipmapMode : std::int32_t { VK_SAMPLER_MIPMAP_MODE_NEAREST = 0, VK_SAMPLER_MIPMAP_MODE_LINEAR = 1 };
enum VkSamplerAddressMode : std::int32_t { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE = 2 };
enum VkCompareOp : std::int32_t { VK_COMPARE_OP_NEVER = 0 };
enum VkBorderColor : std::int32_t { VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK = 0 };

inline constexpr VkFlags VK_QUEUE_GRAPHICS_BIT = 0x00000001;
inline constexpr VkFlags VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x00000001;
inline constexpr VkFlags VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x00000002;
inline constexpr VkFlags VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x00000004;
inline constexpr VkFlags VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x00000001;
inline constexpr VkFlags VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x00000002;
inline constexpr VkFlags VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x00000020;
inline constexpr VkFlags VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x00000001;
inline constexpr VkFlags VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x00000002;
inline constexpr VkFlags VK_IMAGE_USAGE_SAMPLED_BIT = 0x00000004;
inline constexpr VkFlags VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x00000010;
inline constexpr VkFlags VK_IMAGE_ASPECT_COLOR_BIT = 0x00000001;
inline constexpr VkFlags VK_SHADER_STAGE_VERTEX_BIT = 0x00000001;
inline constexpr VkFlags VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010;
inline constexpr VkFlags VK_CULL_MODE_NONE = 0;
inline constexpr VkFlags VK_COLOR_COMPONENT_R_BIT = 0x1;
inline constexpr VkFlags VK_COLOR_COMPONENT_G_BIT = 0x2;
inline constexpr VkFlags VK_COLOR_COMPONENT_B_BIT = 0x4;
inline constexpr VkFlags VK_COLOR_COMPONENT_A_BIT = 0x8;
inline constexpr VkFlags VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x00000002;
inline constexpr VkFlags VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x00000001;
inline constexpr VkFlags VK_FENCE_CREATE_SIGNALED_BIT = 0x00000001;
inline constexpr VkFlags VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 0x00000001;
inline constexpr VkFlags VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR = 0x00000001;
inline constexpr VkFlags VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x00000001;
inline constexpr VkFlags VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 0x00000080;
inline constexpr VkFlags VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x00000400;
inline constexpr VkFlags VK_PIPELINE_STAGE_TRANSFER_BIT = 0x00001000;
inline constexpr VkFlags VK_PIPELINE_STAGE_HOST_BIT = 0x00004000;
inline constexpr VkFlags VK_ACCESS_SHADER_READ_BIT = 0x00000020;
inline constexpr VkFlags VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 0x00000100;
inline constexpr VkFlags VK_ACCESS_TRANSFER_READ_BIT = 0x00000800;
inline constexpr VkFlags VK_ACCESS_TRANSFER_WRITE_BIT = 0x00001000;
inline constexpr VkFlags VK_ACCESS_HOST_READ_BIT = 0x00002000;

struct VkExtent2D { std::uint32_t width; std::uint32_t height; };
struct VkExtent3D { std::uint32_t width; std::uint32_t height; std::uint32_t depth; };
struct VkOffset2D { std::int32_t x; std::int32_t y; };
struct VkOffset3D { std::int32_t x; std::int32_t y; std::int32_t z; };
struct VkRect2D { VkOffset2D offset; VkExtent2D extent; };
struct VkViewport { float x; float y; float width; float height; float minDepth; float maxDepth; };

struct VkApplicationInfo {
  VkStructureType sType;
  const void* pNext;
  const char* pApplicationName;
  std::uint32_t applicationVersion;
  const char* pEngineName;
  std::uint32_t engineVersion;
  std::uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  const VkApplicationInfo* pApplicationInfo;
  std::uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  std::uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
};

struct VkLayerProperties {
  char layerName[VK_MAX_EXTENSION_NAME_SIZE];
  std::uint32_t specVersion;
  std::uint32_t implementationVersion;
  char description[VK_MAX_DESCRIPTION_SIZE];
};

struct VkPhysicalDeviceLimits {
  std::uint32_t maxImageDimension1D;
  std::uint32_t maxImageDimension2D;
  std::uint32_t _pad[124];
};

struct VkPhysicalDeviceSparseProperties {
  VkBool32 residencyStandard2DBlockShape;
  VkBool32 residencyStandard2DMultisampleBlockShape;
  VkBool32 residencyStandard3DBlockShape;
  VkBool32 residencyAlignedMipSize;
  VkBool32 residencyNonResidentStrict;
};

struct VkPhysicalDeviceProperties {
  std::uint32_t apiVersion;
  std::uint32_t driverVersion;
  std::uint32_t vendorID;
  std::uint32_t deviceID;
  VkPhysicalDeviceType deviceType;
  char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
  std::uint8_t pipelineCacheUUID[VK_UUID_SIZE];
  // Safe oversized buffer matching Khronos ABI (>= 824 bytes total)
  alignas(8) std::uint8_t limitsAndSparse[608];
};

struct VkQueueFamilyProperties {
  VkFlags queueFlags;
  std::uint32_t queueCount;
  std::uint32_t timestampValidBits;
  VkExtent3D minImageTransferGranularity;
};

struct VkMemoryType {
  VkFlags propertyFlags;
  std::uint32_t heapIndex;
};

struct VkMemoryHeap {
  VkDeviceSize size;
  VkFlags flags;
};

struct VkPhysicalDeviceMemoryProperties {
  std::uint32_t memoryTypeCount;
  VkMemoryType memoryTypes[VK_MAX_MEMORY_TYPES];
  std::uint32_t memoryHeapCount;
  VkMemoryHeap memoryHeaps[VK_MAX_MEMORY_HEAPS];
};

struct VkDeviceQueueCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t queueFamilyIndex;
  std::uint32_t queueCount;
  const float* pQueuePriorities;
};

struct VkPhysicalDeviceFeatures {
  VkBool32 features[55];
};

struct VkDeviceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t queueCreateInfoCount;
  const VkDeviceQueueCreateInfo* pQueueCreateInfos;
  std::uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  std::uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
  const VkPhysicalDeviceFeatures* pEnabledFeatures;
};

struct VkMemoryRequirements {
  VkDeviceSize size;
  VkDeviceSize alignment;
  std::uint32_t memoryTypeBits;
};

struct VkMemoryAllocateInfo {
  VkStructureType sType;
  const void* pNext;
  VkDeviceSize allocationSize;
  std::uint32_t memoryTypeIndex;
};

struct VkBufferCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkDeviceSize size;
  VkFlags usage;
  VkSharingMode sharingMode;
  std::uint32_t queueFamilyIndexCount;
  const std::uint32_t* pQueueFamilyIndices;
};

struct VkImageCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkImageType imageType;
  VkFormat format;
  VkExtent3D extent;
  std::uint32_t mipLevels;
  std::uint32_t arrayLayers;
  VkSampleCountFlagBits samples;
  VkImageTiling tiling;
  VkFlags usage;
  VkSharingMode sharingMode;
  std::uint32_t queueFamilyIndexCount;
  const std::uint32_t* pQueueFamilyIndices;
  VkImageLayout initialLayout;
};

struct VkComponentMapping {
  VkComponentSwizzle r;
  VkComponentSwizzle g;
  VkComponentSwizzle b;
  VkComponentSwizzle a;
};

struct VkImageSubresourceRange {
  VkFlags aspectMask;
  std::uint32_t baseMipLevel;
  std::uint32_t levelCount;
  std::uint32_t baseArrayLayer;
  std::uint32_t layerCount;
};

struct VkImageSubresourceLayers {
  VkFlags aspectMask;
  std::uint32_t mipLevel;
  std::uint32_t baseArrayLayer;
  std::uint32_t layerCount;
};

struct VkImageViewCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkImage image;
  VkImageViewType viewType;
  VkFormat format;
  VkComponentMapping components;
  VkImageSubresourceRange subresourceRange;
};

struct VkShaderModuleCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::size_t codeSize;
  const std::uint32_t* pCode;
};

struct VkAttachmentDescription {
  VkFlags flags;
  VkFormat format;
  VkSampleCountFlagBits samples;
  VkAttachmentLoadOp loadOp;
  VkAttachmentStoreOp storeOp;
  VkAttachmentLoadOp stencilLoadOp;
  VkAttachmentStoreOp stencilStoreOp;
  VkImageLayout initialLayout;
  VkImageLayout finalLayout;
};

struct VkAttachmentReference {
  std::uint32_t attachment;
  VkImageLayout layout;
};

struct VkSubpassDescription {
  VkFlags flags;
  VkPipelineBindPoint pipelineBindPoint;
  std::uint32_t inputAttachmentCount;
  const VkAttachmentReference* pInputAttachments;
  std::uint32_t colorAttachmentCount;
  const VkAttachmentReference* pColorAttachments;
  const VkAttachmentReference* pResolveAttachments;
  const VkAttachmentReference* pDepthStencilAttachment;
  std::uint32_t preserveAttachmentCount;
  const std::uint32_t* pPreserveAttachments;
};

struct VkSubpassDependency {
  std::uint32_t srcSubpass;
  std::uint32_t dstSubpass;
  VkFlags srcStageMask;
  VkFlags dstStageMask;
  VkFlags srcAccessMask;
  VkFlags dstAccessMask;
  VkFlags dependencyFlags;
};

struct VkRenderPassCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t attachmentCount;
  const VkAttachmentDescription* pAttachments;
  std::uint32_t subpassCount;
  const VkSubpassDescription* pSubpasses;
  std::uint32_t dependencyCount;
  const VkSubpassDependency* pDependencies;
};

struct VkFramebufferCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkRenderPass renderPass;
  std::uint32_t attachmentCount;
  const VkImageView* pAttachments;
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t layers;
};

struct VkDescriptorSetLayoutBinding {
  std::uint32_t binding;
  VkDescriptorType descriptorType;
  std::uint32_t descriptorCount;
  VkFlags stageFlags;
  const VkSampler* pImmutableSamplers;
};

struct VkDescriptorSetLayoutCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t bindingCount;
  const VkDescriptorSetLayoutBinding* pBindings;
};

struct VkPushConstantRange {
  VkFlags stageFlags;
  std::uint32_t offset;
  std::uint32_t size;
};

struct VkPipelineLayoutCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t setLayoutCount;
  const VkDescriptorSetLayout* pSetLayouts;
  std::uint32_t pushConstantRangeCount;
  const VkPushConstantRange* pPushConstantRanges;
};

struct VkDescriptorPoolSize {
  VkDescriptorType type;
  std::uint32_t descriptorCount;
};

struct VkDescriptorPoolCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t maxSets;
  std::uint32_t poolSizeCount;
  const VkDescriptorPoolSize* pPoolSizes;
};

struct VkDescriptorSetAllocateInfo {
  VkStructureType sType;
  const void* pNext;
  VkDescriptorPool descriptorPool;
  std::uint32_t descriptorSetCount;
  const VkDescriptorSetLayout* pSetLayouts;
};

struct VkDescriptorImageInfo {
  VkSampler sampler;
  VkImageView imageView;
  VkImageLayout imageLayout;
};

struct VkDescriptorBufferInfo {
  VkBuffer buffer;
  VkDeviceSize offset;
  VkDeviceSize range;
};

struct VkWriteDescriptorSet {
  VkStructureType sType;
  const void* pNext;
  VkDescriptorSet dstSet;
  std::uint32_t dstBinding;
  std::uint32_t dstArrayElement;
  std::uint32_t descriptorCount;
  VkDescriptorType descriptorType;
  const VkDescriptorImageInfo* pImageInfo;
  const VkDescriptorBufferInfo* pBufferInfo;
  const void* pTexelBufferView;
};

struct VkSamplerCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkFilter magFilter;
  VkFilter minFilter;
  VkSamplerMipmapMode mipmapMode;
  VkSamplerAddressMode addressModeU;
  VkSamplerAddressMode addressModeV;
  VkSamplerAddressMode addressModeW;
  float mipLodBias;
  VkBool32 anisotropyEnable;
  float maxAnisotropy;
  VkBool32 compareEnable;
  VkCompareOp compareOp;
  float minLod;
  float maxLod;
  VkBorderColor borderColor;
  VkBool32 unnormalizedCoordinates;
};

struct VkPipelineShaderStageCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkFlags stage;
  VkShaderModule module;
  const char* pName;
  const void* pSpecializationInfo;
};

struct VkPipelineVertexInputStateCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t vertexBindingDescriptionCount;
  const void* pVertexBindingDescriptions;
  std::uint32_t vertexAttributeDescriptionCount;
  const void* pVertexAttributeDescriptions;
};

struct VkPipelineInputAssemblyStateCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkPrimitiveTopology topology;
  VkBool32 primitiveRestartEnable;
};

struct VkPipelineViewportStateCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t viewportCount;
  const VkViewport* pViewports;
  std::uint32_t scissorCount;
  const VkRect2D* pScissors;
};

struct VkPipelineRasterizationStateCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkBool32 depthClampEnable;
  VkBool32 rasterizerDiscardEnable;
  VkPolygonMode polygonMode;
  VkFlags cullMode;
  VkFrontFace frontFace;
  VkBool32 depthBiasEnable;
  float depthBiasConstantFactor;
  float depthBiasClamp;
  float depthBiasSlopeFactor;
  float lineWidth;
};

struct VkPipelineMultisampleStateCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkSampleCountFlagBits rasterizationSamples;
  VkBool32 sampleShadingEnable;
  float minSampleShading;
  const VkSampleMask* pSampleMask;
  VkBool32 alphaToCoverageEnable;
  VkBool32 alphaToOneEnable;
};

struct VkPipelineColorBlendAttachmentState {
  VkBool32 blendEnable;
  VkBlendFactor srcColorBlendFactor;
  VkBlendFactor dstColorBlendFactor;
  VkBlendOp colorBlendOp;
  VkBlendFactor srcAlphaBlendFactor;
  VkBlendFactor dstAlphaBlendFactor;
  VkBlendOp alphaBlendOp;
  VkFlags colorWriteMask;
};

struct VkPipelineColorBlendStateCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkBool32 logicOpEnable;
  VkLogicOp logicOp;
  std::uint32_t attachmentCount;
  const VkPipelineColorBlendAttachmentState* pAttachments;
  float blendConstants[4];
};

struct VkPipelineDynamicStateCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t dynamicStateCount;
  const VkDynamicState* pDynamicStates;
};

struct VkGraphicsPipelineCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t stageCount;
  const VkPipelineShaderStageCreateInfo* pStages;
  const VkPipelineVertexInputStateCreateInfo* pVertexInputState;
  const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
  const void* pTessellationState;
  const VkPipelineViewportStateCreateInfo* pViewportState;
  const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
  const VkPipelineMultisampleStateCreateInfo* pMultisampleState;
  const void* pDepthStencilState;
  const VkPipelineColorBlendStateCreateInfo* pColorBlendState;
  const VkPipelineDynamicStateCreateInfo* pDynamicState;
  VkPipelineLayout layout;
  VkRenderPass renderPass;
  std::uint32_t subpass;
  VkPipeline basePipelineHandle;
  std::int32_t basePipelineIndex;
};

struct VkCommandPoolCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  std::uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo {
  VkStructureType sType;
  const void* pNext;
  VkCommandPool commandPool;
  VkCommandBufferLevel level;
  std::uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  const void* pInheritanceInfo;
};

union VkClearColorValue {
  float float32[4];
  std::int32_t int32[4];
  std::uint32_t uint32[4];
};

union VkClearValue {
  VkClearColorValue color;
  struct { float depth; std::uint32_t stencil; } depthStencil;
};

struct VkRenderPassBeginInfo {
  VkStructureType sType;
  const void* pNext;
  VkRenderPass renderPass;
  VkFramebuffer framebuffer;
  VkRect2D renderArea;
  std::uint32_t clearValueCount;
  const VkClearValue* pClearValues;
};

struct VkSemaphoreCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
};

struct VkFenceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
};

struct VkSubmitInfo {
  VkStructureType sType;
  const void* pNext;
  std::uint32_t waitSemaphoreCount;
  const VkSemaphore* pWaitSemaphores;
  const VkFlags* pWaitDstStageMask;
  std::uint32_t commandBufferCount;
  const VkCommandBuffer* pCommandBuffers;
  std::uint32_t signalSemaphoreCount;
  const VkSemaphore* pSignalSemaphores;
};

struct VkImageMemoryBarrier {
  VkStructureType sType;
  const void* pNext;
  VkFlags srcAccessMask;
  VkFlags dstAccessMask;
  VkImageLayout oldLayout;
  VkImageLayout newLayout;
  std::uint32_t srcQueueFamilyIndex;
  std::uint32_t dstQueueFamilyIndex;
  VkImage image;
  VkImageSubresourceRange subresourceRange;
};

struct VkBufferImageCopy {
  VkDeviceSize bufferOffset;
  std::uint32_t bufferRowLength;
  std::uint32_t bufferImageHeight;
  VkImageSubresourceLayers imageSubresource;
  VkOffset3D imageOffset;
  VkExtent3D imageExtent;
};

struct VkSurfaceCapabilitiesKHR {
  std::uint32_t minImageCount;
  std::uint32_t maxImageCount;
  VkExtent2D currentExtent;
  VkExtent2D minImageExtent;
  VkExtent2D maxImageExtent;
  std::uint32_t maxImageArrayLayers;
  VkFlags supportedTransforms;
  VkFlags currentTransform;
  VkFlags supportedCompositeAlpha;
  VkFlags supportedUsageFlags;
};

struct VkSurfaceFormatKHR {
  VkFormat format;
  VkColorSpaceKHR colorSpace;
};

struct VkSwapchainCreateInfoKHR {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  VkSurfaceKHR surface;
  std::uint32_t minImageCount;
  VkFormat imageFormat;
  VkColorSpaceKHR imageColorSpace;
  VkExtent2D imageExtent;
  std::uint32_t imageArrayLayers;
  VkFlags imageUsage;
  VkSharingMode imageSharingMode;
  std::uint32_t queueFamilyIndexCount;
  const std::uint32_t* pQueueFamilyIndices;
  VkFlags preTransform;
  VkFlags compositeAlpha;
  VkPresentModeKHR presentMode;
  VkBool32 clipped;
  VkSwapchainKHR oldSwapchain;
};

struct VkPresentInfoKHR {
  VkStructureType sType;
  const void* pNext;
  std::uint32_t waitSemaphoreCount;
  const VkSemaphore* pWaitSemaphores;
  std::uint32_t swapchainCount;
  const VkSwapchainKHR* pSwapchains;
  const std::uint32_t* pImageIndices;
  VkResult* pResults;
};

using PFN_vkVoidFunction = void(VKAPI_PTR*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(VKAPI_PTR*)(VkInstance, const char*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(VKAPI_PTR*)(VkDevice, const char*);

}  // extern "C"

#endif  // __has_include(<vulkan/vulkan.h>)

namespace orangutan::vkui {

// Dynamic dispatch table loaded cleanly via vkGetInstanceProcAddr / vkGetDeviceProcAddr.
struct VkApi {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr{nullptr};
  PFN_vkGetDeviceProcAddr get_device_proc_addr{nullptr};

  VkResult (*enumerate_instance_layer_properties)(std::uint32_t*, VkLayerProperties*){nullptr};
  VkResult (*create_instance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*){nullptr};
  void (*destroy_instance)(VkInstance, const VkAllocationCallbacks*){nullptr};
  VkResult (*enumerate_physical_devices)(VkInstance, std::uint32_t*, VkPhysicalDevice*){nullptr};
  void (*get_physical_device_properties)(VkPhysicalDevice, VkPhysicalDeviceProperties*){nullptr};
  void (*get_physical_device_queue_family_properties)(VkPhysicalDevice, std::uint32_t*, VkQueueFamilyProperties*){nullptr};
  void (*get_physical_device_memory_properties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*){nullptr};
  VkResult (*create_device)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*){nullptr};
  void (*destroy_device)(VkDevice, const VkAllocationCallbacks*){nullptr};
  void (*get_device_queue)(VkDevice, std::uint32_t, std::uint32_t, VkQueue*){nullptr};
  VkResult (*device_wait_idle)(VkDevice){nullptr};

  VkResult (*get_physical_device_surface_support_khr)(VkPhysicalDevice, std::uint32_t, VkSurfaceKHR, VkBool32*){nullptr};
  VkResult (*get_physical_device_surface_capabilities_khr)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*){nullptr};
  VkResult (*get_physical_device_surface_formats_khr)(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t*, VkSurfaceFormatKHR*){nullptr};
  VkResult (*get_physical_device_surface_present_modes_khr)(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t*, VkPresentModeKHR*){nullptr};
  void (*destroy_surface_khr)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*){nullptr};

  VkResult (*create_swapchain_khr)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*){nullptr};
  void (*destroy_swapchain_khr)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*){nullptr};
  VkResult (*get_swapchain_images_khr)(VkDevice, VkSwapchainKHR, std::uint32_t*, VkImage*){nullptr};
  VkResult (*acquire_next_image_khr)(VkDevice, VkSwapchainKHR, std::uint64_t, VkSemaphore, VkFence, std::uint32_t*){nullptr};
  VkResult (*queue_present_khr)(VkQueue, const VkPresentInfoKHR*){nullptr};

  VkResult (*create_buffer)(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*){nullptr};
  void (*destroy_buffer)(VkDevice, VkBuffer, const VkAllocationCallbacks*){nullptr};
  void (*get_buffer_memory_requirements)(VkDevice, VkBuffer, VkMemoryRequirements*){nullptr};
  VkResult (*bind_buffer_memory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize){nullptr};

  VkResult (*create_image)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*){nullptr};
  void (*destroy_image)(VkDevice, VkImage, const VkAllocationCallbacks*){nullptr};
  void (*get_image_memory_requirements)(VkDevice, VkImage, VkMemoryRequirements*){nullptr};
  VkResult (*bind_image_memory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize){nullptr};

  VkResult (*allocate_memory)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*){nullptr};
  void (*free_memory)(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*){nullptr};
  VkResult (*map_memory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**){nullptr};
  void (*unmap_memory)(VkDevice, VkDeviceMemory){nullptr};

  VkResult (*create_image_view)(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*){nullptr};
  void (*destroy_image_view)(VkDevice, VkImageView, const VkAllocationCallbacks*){nullptr};
  VkResult (*create_sampler)(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler*){nullptr};
  void (*destroy_sampler)(VkDevice, VkSampler, const VkAllocationCallbacks*){nullptr};

  VkResult (*create_shader_module)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*){nullptr};
  void (*destroy_shader_module)(VkDevice, VkShaderModule, const VkAllocationCallbacks*){nullptr};
  VkResult (*create_render_pass)(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*){nullptr};
  void (*destroy_render_pass)(VkDevice, VkRenderPass, const VkAllocationCallbacks*){nullptr};
  VkResult (*create_framebuffer)(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*){nullptr};
  void (*destroy_framebuffer)(VkDevice, VkFramebuffer, const VkAllocationCallbacks*){nullptr};

  VkResult (*create_descriptor_set_layout)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout*){nullptr};
  void (*destroy_descriptor_set_layout)(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*){nullptr};
  VkResult (*create_pipeline_layout)(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*){nullptr};
  void (*destroy_pipeline_layout)(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*){nullptr};
  VkResult (*create_graphics_pipelines)(VkDevice, VkPipelineCache, std::uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*){nullptr};
  void (*destroy_pipeline)(VkDevice, VkPipeline, const VkAllocationCallbacks*){nullptr};

  VkResult (*create_descriptor_pool)(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool*){nullptr};
  void (*destroy_descriptor_pool)(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*){nullptr};
  VkResult (*allocate_descriptor_sets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*){nullptr};
  void (*update_descriptor_sets)(VkDevice, std::uint32_t, const VkWriteDescriptorSet*, std::uint32_t, const void*){nullptr};

  VkResult (*create_command_pool)(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*){nullptr};
  void (*destroy_command_pool)(VkDevice, VkCommandPool, const VkAllocationCallbacks*){nullptr};
  VkResult (*allocate_command_buffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*){nullptr};
  VkResult (*reset_command_buffer)(VkCommandBuffer, VkFlags){nullptr};
  VkResult (*begin_command_buffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*){nullptr};
  VkResult (*end_command_buffer)(VkCommandBuffer){nullptr};

  void (*cmd_begin_render_pass)(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents){nullptr};
  void (*cmd_end_render_pass)(VkCommandBuffer){nullptr};
  void (*cmd_bind_pipeline)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline){nullptr};
  void (*cmd_bind_descriptor_sets)(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, std::uint32_t, std::uint32_t, const VkDescriptorSet*, std::uint32_t, const std::uint32_t*){nullptr};
  void (*cmd_push_constants)(VkCommandBuffer, VkPipelineLayout, VkFlags, std::uint32_t, std::uint32_t, const void*){nullptr};
  void (*cmd_set_viewport)(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkViewport*){nullptr};
  void (*cmd_set_scissor)(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkRect2D*){nullptr};
  void (*cmd_draw)(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t){nullptr};
  void (*cmd_pipeline_barrier)(VkCommandBuffer, VkFlags, VkFlags, VkFlags, std::uint32_t, const void*, std::uint32_t, const void*, std::uint32_t, const VkImageMemoryBarrier*){nullptr};
  void (*cmd_copy_buffer_to_image)(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, std::uint32_t, const VkBufferImageCopy*){nullptr};
  void (*cmd_copy_image_to_buffer)(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, std::uint32_t, const VkBufferImageCopy*){nullptr};

  VkResult (*create_semaphore)(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*){nullptr};
  void (*destroy_semaphore)(VkDevice, VkSemaphore, const VkAllocationCallbacks*){nullptr};
  VkResult (*create_fence)(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence*){nullptr};
  void (*destroy_fence)(VkDevice, VkFence, const VkAllocationCallbacks*){nullptr};
  VkResult (*wait_for_fences)(VkDevice, std::uint32_t, const VkFence*, VkBool32, std::uint64_t){nullptr};
  VkResult (*reset_fences)(VkDevice, std::uint32_t, const VkFence*){nullptr};
  VkResult (*queue_submit)(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence){nullptr};
  VkResult (*queue_wait_idle)(VkQueue){nullptr};
};

}  // namespace orangutan::vkui
