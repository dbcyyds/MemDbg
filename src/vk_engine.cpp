#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
// Load prototypes from loader (do NOT define VK_NO_PROTOTYPES)
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>

#include "vk_engine.hpp"
#include "a_native_window_creator.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "font_embed.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <zlib.h>

// destroy_all_textures used from shutdown()
namespace vkeng {
void destroy_all_textures();
}

// fopen for font probe

namespace vkeng {
namespace {

void check_vk(VkResult err) {
  if (err == VK_SUCCESS) return;
  std::fprintf(stderr, "[vk] VkResult=%d\n", (int)err);
  if (err < 0) std::abort();
}

bool has_ext(const std::vector<VkExtensionProperties>& props, const char* name) {
  for (const auto& p : props)
    if (std::strcmp(p.extensionName, name) == 0) return true;
  return false;
}

struct Engine {
  Config cfg{};
  ANativeWindow* window = nullptr;
  Display disp{};

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;

  ImGui_ImplVulkanH_Window main_wd{};
  uint32_t min_images = 2;
  bool swapchain_rebuild = false;
  bool ready = false;
  char gpu_name[256]{};
};

Engine g;

int theta_to_orient(int32_t theta) {
  if (theta == 90) return 1;
  if (theta == 180) return 2;
  if (theta == 270) return 3;
  return 0;
}

Display read_display() {
  Display d;
  const auto di = android::ANativeWindowCreator::GetDisplayInfo();
  if (di.width > 0 && di.height > 0) {
    d.width = di.width;
    d.height = di.height;
    d.orient = theta_to_orient(di.theta);
    d.side = std::max(di.width, di.height);
  } else {
    d.width = 1080;
    d.height = 1920;
    d.side = 1920;
    d.orient = 0;
  }
  if (d.side < 720) d.side = 1080;
  return d;
}

VkPhysicalDevice pick_gpu(VkInstance inst) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(inst, &count, nullptr);
  if (count == 0) return VK_NULL_HANDLE;
  std::vector<VkPhysicalDevice> devs(count);
  vkEnumeratePhysicalDevices(inst, &count, devs.data());

  VkPhysicalDevice best = VK_NULL_HANDLE;
  int best_score = -1;
  for (auto pd : devs) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 100;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 200;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) score = 1;  // lavapipe
    // Prefer Adreno / Mali / PowerVR names
    if (std::strstr(props.deviceName, "Adreno")) score += 50;
    if (std::strstr(props.deviceName, "Mali")) score += 50;
    if (std::strstr(props.deviceName, "llvmpipe") ||
        std::strstr(props.deviceName, " lavapipe") ||
        std::strstr(props.deviceName, "SwiftShader"))
      score = 0;
    if (score > best_score) {
      best_score = score;
      best = pd;
      std::snprintf(g.gpu_name, sizeof(g.gpu_name), "%s", props.deviceName);
    }
  }
  return best;
}

uint32_t pick_queue_family(VkPhysicalDevice pd, VkSurfaceKHR surf) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
  std::vector<VkQueueFamilyProperties> props(count);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props.data());
  for (uint32_t i = 0; i < count; ++i) {
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &present);
    if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) return i;
  }
  return (uint32_t)-1;
}

void setup_window(int w, int h) {
  ImGui_ImplVulkanH_Window* wd = &g.main_wd;
  wd->Surface = g.surface;

  const VkFormat req_fmt[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                              VK_FORMAT_R8G8B8A8_SRGB};
  const VkColorSpaceKHR req_cs = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
      g.physical, wd->Surface, req_fmt, IM_COUNTOF(req_fmt), req_cs);

  VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR,
                                      VK_PRESENT_MODE_MAILBOX_KHR};
  wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
      g.physical, wd->Surface, present_modes, IM_COUNTOF(present_modes));

  ImGui_ImplVulkanH_CreateOrResizeWindow(g.instance, g.physical, g.device, wd,
                                         g.queue_family, nullptr, w, h,
                                         g.min_images, 0);
}

void frame_render(ImDrawData* draw_data) {
  ImGui_ImplVulkanH_Window* wd = &g.main_wd;
  VkSemaphore image_acquired =
      wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete =
      wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;

  VkResult err = vkAcquireNextImageKHR(g.device, wd->Swapchain, UINT64_MAX,
                                       image_acquired, VK_NULL_HANDLE,
                                       &wd->FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    g.swapchain_rebuild = true;
  if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
  if (err != VK_SUBOPTIMAL_KHR) check_vk(err);

  ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
  check_vk(vkWaitForFences(g.device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
  check_vk(vkResetFences(g.device, 1, &fd->Fence));
  check_vk(vkResetCommandPool(g.device, fd->CommandPool, 0));

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check_vk(vkBeginCommandBuffer(fd->CommandBuffer, &begin));

  // Transparent clear — overlay
  wd->ClearValue.color.float32[0] = 0.f;
  wd->ClearValue.color.float32[1] = 0.f;
  wd->ClearValue.color.float32[2] = 0.f;
  wd->ClearValue.color.float32[3] = 0.f;

  VkRenderPassBeginInfo rp{};
  rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp.renderPass = wd->RenderPass;
  rp.framebuffer = fd->Framebuffer;
  rp.renderArea.extent.width = (uint32_t)wd->Width;
  rp.renderArea.extent.height = (uint32_t)wd->Height;
  rp.clearValueCount = 1;
  rp.pClearValues = &wd->ClearValue;
  vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

  ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
  vkCmdEndRenderPass(fd->CommandBuffer);

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &image_acquired;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &fd->CommandBuffer;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &render_complete;
  check_vk(vkEndCommandBuffer(fd->CommandBuffer));
  check_vk(vkQueueSubmit(g.queue, 1, &submit, fd->Fence));
}

void frame_present() {
  if (g.swapchain_rebuild) return;
  ImGui_ImplVulkanH_Window* wd = &g.main_wd;
  VkSemaphore render_complete =
      wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkPresentInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  info.waitSemaphoreCount = 1;
  info.pWaitSemaphores = &render_complete;
  info.swapchainCount = 1;
  info.pSwapchains = &wd->Swapchain;
  info.pImageIndices = &wd->FrameIndex;
  VkResult err = vkQueuePresentKHR(g.queue, &info);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    g.swapchain_rebuild = true;
  if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
  if (err != VK_SUBOPTIMAL_KHR) check_vk(err);
  wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

}  // namespace

bool init(const Config& cfg) {
  if (g.ready) return true;
  g.cfg = cfg;
  g.min_images = (uint32_t)std::max(2, cfg.min_image_count);
  g.disp = read_display();

  g.window = android::ANativeWindowCreator::Create({
      .name = cfg.layer_name ? cfg.layer_name : "VkImGuiFloat",
      .width = g.disp.side,
      .height = g.disp.side,
      .skipScreenshot = cfg.skip_screenshot,
  });
  if (!g.window) {
    std::fprintf(stderr, "[vk] ANativeWindow create failed (need root?)\n");
    return false;
  }
  ANativeWindow_acquire(g.window);

  // --- Instance ---
  {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data());

    std::vector<const char*> exts;
    exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    if (has_ext(props, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
      exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VkImGuiFloat";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    check_vk(vkCreateInstance(&ci, nullptr, &g.instance));
  }

  // --- Surface ---
  {
    VkAndroidSurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    sci.window = g.window;
    check_vk(vkCreateAndroidSurfaceKHR(g.instance, &sci, nullptr, &g.surface));
  }

  g.physical = pick_gpu(g.instance);
  if (g.physical == VK_NULL_HANDLE) {
    std::fprintf(stderr, "[vk] no physical device\n");
    shutdown();
    return false;
  }
  std::printf("[vk] GPU: %s\n", g.gpu_name);

  g.queue_family = pick_queue_family(g.physical, g.surface);
  if (g.queue_family == (uint32_t)-1) {
    std::fprintf(stderr, "[vk] no graphics+present queue\n");
    shutdown();
    return false;
  }

  // --- Device ---
  {
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    check_vk(vkCreateDevice(g.physical, &dci, nullptr, &g.device));
    vkGetDeviceQueue(g.device, g.queue_family, 0, &g.queue);
  }

  // --- Descriptor pool (ImGui fonts + many app icons) ---
  {
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 16},
    };
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = 512;
    pci.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
    pci.pPoolSizes = pool_sizes;
    check_vk(vkCreateDescriptorPool(g.device, &pci, nullptr, &g.desc_pool));
  }

  setup_window(g.disp.side, g.disp.side);

  // --- ImGui ---
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // 禁止自动写 imgui.ini / 日志（换设备、任意路径运行时 cwd 可能不可写）
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  // 触屏友好尺寸（像素字号，不再二次 FontScaleDpi 放大导致糊）
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(1.6f);
  style.FontScaleDpi = 1.0f;
  style.WindowRounding = 12.f;
  style.FrameRounding = 8.f;
  style.GrabRounding = 8.f;
  style.ScrollbarSize = 24.f;
  style.WindowBorderSize = 1.f;
  style.Alpha = 0.96f;

  // --- 中文字体（内嵌优先，任意设备可用）---
  // 内嵌：ZUKChinese 子集（简体常用 2500 + 拉丁），zlib 压缩；
  // stb_truetype 无法加载 NotoSansCJK.ttc(CFF)，故不用系统 CJK TTC。
  {
    const float font_px = 28.0f;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();

    ImFont* font = nullptr;

    // 1) 内嵌字体：zlib 解压到进程内静态缓冲（不交 ImGui free，生命周期=进程）
    {
      static std::vector<unsigned char> s_embed_ttf;
      if (s_embed_ttf.empty()) {
        s_embed_ttf.resize((size_t)k_embed_font_raw_len);
        uLongf dest_len = (uLongf)k_embed_font_raw_len;
        int zr = uncompress(s_embed_ttf.data(), &dest_len, k_embed_font_z,
                            (uLong)k_embed_font_z_len);
        if (zr != Z_OK || dest_len < 100) {
          std::fprintf(stderr, "[font] embed uncompress fail zr=%d\n", zr);
          s_embed_ttf.clear();
        } else {
          s_embed_ttf.resize((size_t)dest_len);
        }
      }
      if (!s_embed_ttf.empty()) {
        cfg.FontDataOwnedByAtlas = false;  // 我们自己持有
        font = io.Fonts->AddFontFromMemoryTTF(
            s_embed_ttf.data(), (int)s_embed_ttf.size(), font_px, &cfg, ranges);
        if (font) {
          std::printf("[font] embedded CJK subset (%zu bytes, size=%.0f)\n",
                      s_embed_ttf.size(), font_px);
        }
      }
    }

    // 2) 回退：系统 TrueType（部分机型有 ZUKChinese / DroidSansFallback）
    if (!font) {
      const char* candidates[] = {
          "/system/fonts/ZUKChinese.ttf",
          "/system/fonts/DroidSansFallback.ttf",
          "/system/fonts/NotoSansSC-Regular.otf",
          "/system/fonts/Roboto-Regular.ttf",
          "/system/fonts/DroidSans.ttf",
      };
      for (const char* path : candidates) {
        FILE* fp = std::fopen(path, "rb");
        if (!fp) continue;
        std::fclose(fp);
        cfg.FontNo = 0;
        cfg.FontDataOwnedByAtlas = true;
        if (std::strstr(path, ".ttc") || std::strstr(path, ".otc"))
          cfg.FontNo = 2;
        font = io.Fonts->AddFontFromFileTTF(path, font_px, &cfg, ranges);
        if (font) {
          std::printf("[font] loaded %s (FontNo=%u size=%.0f)\n", path,
                      (unsigned)cfg.FontNo, font_px);
          break;
        }
        std::fprintf(stderr, "[font] failed %s\n", path);
      }
    }

    if (!font) {
      std::fprintf(stderr,
                   "[font] no CJK font, using default (Chinese broken)\n");
      io.Fonts->AddFontDefault();
    }
  }

  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.ApiVersion = VK_API_VERSION_1_1;
  init_info.Instance = g.instance;
  init_info.PhysicalDevice = g.physical;
  init_info.Device = g.device;
  init_info.QueueFamily = g.queue_family;
  init_info.Queue = g.queue;
  init_info.DescriptorPool = g.desc_pool;
  init_info.MinImageCount = g.min_images;
  init_info.ImageCount = g.main_wd.ImageCount;
  init_info.PipelineInfoMain.RenderPass = g.main_wd.RenderPass;
  init_info.PipelineInfoMain.Subpass = 0;
  init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  init_info.CheckVkResultFn = check_vk;
  ImGui_ImplVulkan_Init(&init_info);

  g.ready = true;
  std::printf("[vk] ready surface %dx%d logical %dx%d orient=%d\n", g.disp.side,
              g.disp.side, g.disp.width, g.disp.height, g.disp.orient);
  return true;
}

void shutdown() {
  if (g.device) vkDeviceWaitIdle(g.device);
  destroy_all_textures();

  if (g.ready) {
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
  }

  if (g.device && g.main_wd.Swapchain != VK_NULL_HANDLE)
    ImGui_ImplVulkanH_DestroyWindow(g.instance, g.device, &g.main_wd, nullptr);

  if (g.surface) {
    vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
    g.surface = VK_NULL_HANDLE;
  }
  if (g.desc_pool) {
    vkDestroyDescriptorPool(g.device, g.desc_pool, nullptr);
    g.desc_pool = VK_NULL_HANDLE;
  }
  if (g.device) {
    vkDestroyDevice(g.device, nullptr);
    g.device = VK_NULL_HANDLE;
  }
  if (g.instance) {
    vkDestroyInstance(g.instance, nullptr);
    g.instance = VK_NULL_HANDLE;
  }
  if (g.window) {
    android::ANativeWindowCreator::Destroy(g.window);
    g.window = nullptr;
  }
  g.main_wd = {};
  g.ready = false;
}

void sync_display() {
  Display d = read_display();
  if (d.side != g.disp.side || d.width != g.disp.width ||
      d.height != g.disp.height || d.orient != g.disp.orient) {
    g.disp = d;
    g.swapchain_rebuild = true;
  } else {
    g.disp = d;
  }
}

Display display() { return g.disp; }

void request_rebuild() { g.swapchain_rebuild = true; }

void new_frame() {
  if (!g.ready) return;

  if (g.swapchain_rebuild) {
    int w = g.disp.side, h = g.disp.side;
    if (w > 0 && h > 0) {
      ImGui_ImplVulkan_SetMinImageCount(g.min_images);
      ImGui_ImplVulkanH_CreateOrResizeWindow(
          g.instance, g.physical, g.device, &g.main_wd, g.queue_family, nullptr,
          w, h, g.min_images, 0);
      g.main_wd.FrameIndex = 0;
      g.swapchain_rebuild = false;
    }
  }

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)g.disp.side, (float)g.disp.side);
  // Display framebuffer scale 1:1
  io.DisplayFramebufferScale = ImVec2(1.f, 1.f);

  ImGui_ImplVulkan_NewFrame();
  // No platform backend — caller feeds touch into io before NewFrame ends.
  // We still need ImGui::NewFrame() after touch is injected.
}

void end_frame() {
  if (!g.ready) return;
  ImGui::Render();
  ImDrawData* dd = ImGui::GetDrawData();
  if (dd && dd->DisplaySize.x > 0.f && dd->DisplaySize.y > 0.f) {
    frame_render(dd);
    frame_present();
  }
}

ANativeWindow* native_window() { return g.window; }
bool ok() { return g.ready; }

// ── User textures (app icons) ─────────────────────────────
namespace {
struct GpuTex {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDescriptorSet ds = VK_NULL_HANDLE;
  int w = 0, h = 0;
};
std::vector<GpuTex> g_texes;

uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(g.physical, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
      return i;
  return 0;
}

bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags props, VkBuffer& buf,
                   VkDeviceMemory& mem) {
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(g.device, &bi, nullptr, &buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(g.device, buf, &req);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = find_mem_type(req.memoryTypeBits, props);
  if (vkAllocateMemory(g.device, &ai, nullptr, &mem) != VK_SUCCESS) {
    vkDestroyBuffer(g.device, buf, nullptr);
    buf = VK_NULL_HANDLE;
    return false;
  }
  vkBindBufferMemory(g.device, buf, mem, 0);
  return true;
}
}  // namespace

Texture create_texture_rgba(const uint8_t* rgba, int w, int h) {
  Texture out{};
  if (!g.ready || !rgba || w <= 0 || h <= 0 || w > 1024 || h > 1024) return out;
  if (g_texes.size() >= 400) return out;  // 防爆

  GpuTex t;
  t.w = w;
  t.h = h;
  const VkDeviceSize img_size = (VkDeviceSize)w * h * 4;

  // Image
  {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g.device, &ii, nullptr, &t.image) != VK_SUCCESS) return out;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.device, t.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.device, &ai, nullptr, &t.memory) != VK_SUCCESS) {
      vkDestroyImage(g.device, t.image, nullptr);
      return out;
    }
    vkBindImageMemory(g.device, t.image, t.memory, 0);
  }

  // Staging
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  if (!create_buffer(img_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging, staging_mem)) {
    vkDestroyImage(g.device, t.image, nullptr);
    vkFreeMemory(g.device, t.memory, nullptr);
    return out;
  }
  void* map = nullptr;
  vkMapMemory(g.device, staging_mem, 0, img_size, 0, &map);
  std::memcpy(map, rgba, (size_t)img_size);
  vkUnmapMemory(g.device, staging_mem);

  // One-shot command buffer
  VkCommandPool pool = g.main_wd.Frames[0].CommandPool;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.device, &ai, &cmd) != VK_SUCCESS) {
      vkDestroyBuffer(g.device, staging, nullptr);
      vkFreeMemory(g.device, staging_mem, nullptr);
      vkDestroyImage(g.device, t.image, nullptr);
      vkFreeMemory(g.device, t.memory, nullptr);
      return out;
    }
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  auto barrier = [&](VkImageLayout old_l, VkImageLayout new_l,
                     VkAccessFlags src_a, VkAccessFlags dst_a,
                     VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_a;
    b.dstAccessMask = dst_a;
    vkCmdPipelineBarrier(cmd, src_s, dst_s, 0, 0, nullptr, 0, nullptr, 1, &b);
  };

  barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
  vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &region);

  barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(g.queue);
  vkFreeCommandBuffers(g.device, pool, 1, &cmd);
  vkDestroyBuffer(g.device, staging, nullptr);
  vkFreeMemory(g.device, staging_mem, nullptr);

  // ImageView
  {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g.device, &vi, nullptr, &t.view) != VK_SUCCESS) {
      vkDestroyImage(g.device, t.image, nullptr);
      vkFreeMemory(g.device, t.memory, nullptr);
      return out;
    }
  }

  t.ds = ImGui_ImplVulkan_AddTexture(t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  if (!t.ds) {
    vkDestroyImageView(g.device, t.view, nullptr);
    vkDestroyImage(g.device, t.image, nullptr);
    vkFreeMemory(g.device, t.memory, nullptr);
    return out;
  }

  out.id = (uint64_t)(uintptr_t)t.ds;
  out.w = w;
  out.h = h;
  g_texes.push_back(t);
  return out;
}

void destroy_texture(Texture& tex) {
  if (!tex.valid() || !g.device) {
    tex = {};
    return;
  }
  for (size_t i = 0; i < g_texes.size(); ++i) {
    if ((uint64_t)(uintptr_t)g_texes[i].ds != tex.id) continue;
    auto& t = g_texes[i];
    if (t.ds) ImGui_ImplVulkan_RemoveTexture(t.ds);
    if (t.view) vkDestroyImageView(g.device, t.view, nullptr);
    if (t.image) vkDestroyImage(g.device, t.image, nullptr);
    if (t.memory) vkFreeMemory(g.device, t.memory, nullptr);
    g_texes.erase(g_texes.begin() + (std::ptrdiff_t)i);
    break;
  }
  tex = {};
}

void destroy_all_textures() {
  if (g.device) vkDeviceWaitIdle(g.device);
  for (auto& t : g_texes) {
    if (t.ds && g.ready) ImGui_ImplVulkan_RemoveTexture(t.ds);
    if (g.device) {
      if (t.view) vkDestroyImageView(g.device, t.view, nullptr);
      if (t.image) vkDestroyImage(g.device, t.image, nullptr);
      if (t.memory) vkFreeMemory(g.device, t.memory, nullptr);
    }
  }
  g_texes.clear();
}

}  // namespace vkeng
