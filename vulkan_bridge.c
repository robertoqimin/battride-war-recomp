/* Port-local Vulkan vertex-texture support; SDK remains unchanged. */
/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4) -- the stage names in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * Batched: draws, clears, uploads and copies whose results the host does
 * not read back accumulate in one open command buffer (vk_end_op), and go to
 * the GPU together when something needs them finished -- a readback, a
 * present, a resource about to be destroyed, the engine's submit_and_wait,
 * or a full batch. Their per-draw vertex, index, constant and staging bytes
 * come from a linear arena and their descriptor sets from a pool, both reset
 * after each submission (s_batch). One submission per draw, each waited on,
 * cost the opening mission most of its frame time. PS3RECOMP_VK_SYNC=1 goes
 * back to submitting and waiting on every operation.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping.
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */

#if !defined(_WIN32) || defined(PS3RECOMP_RSX_VULKAN)

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "rsx_vulkan_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_draw_engine.h"
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"

/* SDL2 is already required off Windows (cellPad, cellAudio); here it only
 * opens the optional window. Same include style as those two. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#ifdef _WIN32
/* vulkan-1.dll through SDL's loader keeps <windows.h> out of this file. */
#define dlopen(name, flags) SDL_LoadObject(name)
#define dlsym SDL_LoadFunction
#define dlclose SDL_UnloadObject
#define dlerror SDL_GetError
#else
#include <dlfcn.h>
#endif
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCmdDispatch) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBounds) \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */

typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;
    VkImageAspectFlags depth_aspect;  /* DEPTH, plus STENCIL if it has one  */

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    vk_image         dummy;           /* 1x1 magenta: "nothing bound"    */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;
    int              recording;       /* cmd is open and holds unsubmitted work */
    u32              batch_ops;       /* operations recorded into it so far    */
    /* Two batch slots: a frame's batch executes in one while the walker
     * records the next into the other. cmd/fence alias the current slot's. */
    VkCommandBuffer  slot_cmd[2];
    VkFence          slot_fence[2];
    int              slot_busy[2];    /* submitted, not yet waited on */
    u32              slot;

    /* A present records and submits on its own pool, command buffer and
     * fences, so its second half can run on a host thread of its own while
     * the FIFO walker records the next frame (rsx_vulkan_backend.h,
     * present_begin/_finish). The queue is shared, and Vulkan wants every
     * vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle on it externally
     * synchronised: queue_mu. present_mu keeps the present's own objects to
     * one present at a time, which matters when a flip in the command stream
     * presents on the walker's thread while a host thread finishes the last. */
    VkCommandPool    present_pool;
    VkCommandBuffer  present_cmd;
    VkFence          present_fence;   /* the copy out of the presented surface */
    VkFence          window_fence;    /* the blit into the swapchain image     */
    int              present_inflight;/* copy submitted, not yet finished      */
    int              present_split;   /* inside rsx_vulkan_backend_present_begin */
    int              present_order;   /* the walker's next command buffer waits */
    int              present_readback;/* the in-flight present copies to readback */
    int              present_shown;   /* ...and already blitted + queued to the window */
    pthread_mutex_t  queue_mu;
    pthread_mutex_t  present_mu;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              depth_bounds_ok; /* depthBounds enabled                */
    u32              max_dim;         /* maxImageDimension2D of the device  */
    int              windowed;
    SDL_Window*      window;
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire;
    double           hold_seconds;
    /* Display options (PORT_DISPLAY_*, see vk_window_open). */
    int              display_mode;    /* VK_DISPLAY_* the window is in now  */
    int              fullscreen_mode; /* what Alt+Enter / F11 switches to   */
    int              win_w, win_h;    /* windowed size to restore           */
    int              fs_w, fs_h;      /* exclusive fullscreen video mode    */
    int              stretch;         /* fill the window, ignoring 16:9     */
    int              sc_dirty;        /* window resized: rebuild swapchain  */
    u32              res_scale;       /* PORT_RENDER_SCALE: internal resolution */
    vk_image         display;         /* the frame at res_scale, for the window */
    /* FSR 1 upscaling into the window (vk_fsr_*). */
    int              fsr_mode;        /* PORT_DISPLAY_FILTER: 1 fsr, 0 linear */
    int              fsr_broken;      /* setup failed: linear from then on  */
    float            fsr_sharpness;   /* RCAS, in stops (0 the strongest)   */
    VkDescriptorSetLayout fsr_dsl;
    VkPipelineLayout fsr_layout;
    VkPipeline       fsr_easu, fsr_rcas;
    VkDescriptorPool fsr_pool;
    VkDescriptorSet  fsr_sets[2];     /* frame -> mid, mid -> out           */
    VkSampler        fsr_sampler;
    vk_image         fsr_mid, fsr_out;
    u32              fsr_w, fsr_h;
    VkImageView      fsr_src;         /* the frame image EASU reads         */
    vk_buffer        win_dump;        /* PORT_WINDOW_DUMP: one frame to a PPM */
    int              win_dump_state;  /* 0 not asked, 1 copy pending, 2 done */
    u32              win_dump_w, win_dump_h;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    const char*      dump_path;
    u32              dump_every;       /* write every nth presented frame */
} vk_state;

static vk_state s_vk;

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
static int vk_load_library(void)
{
#ifdef _WIN32
    static const char* names[] = { "vulkan-1.dll" };
#else
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
#endif
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) { VK_LOG("libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    /* Vulkan guarantees 4096; the engine's targets and textures may use what
     * the device really has (a title's 4608-wide target on RADV). */
    s_vk.max_dim = p.limits.maxImageDimension2D >= 4096u ? p.limits.maxImageDimension2D : 4096u;
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!levels) levels = 1;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &im->img), "vkCreateImage");
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem), "vkAllocateMemory(image)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK(pvkCreateImageView(s_vk.device, &vci, NULL, &im->view), "vkCreateImageView");
    return 0;
}

static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

static void vk_present_wait_gpu(void);
static int vk_submit_and_wait(void);

/* Objects released while the batch that may use them is still open: freed
 * once it has been submitted and waited on, so a release never forces a
 * submission of its own. */
#define VK_GRAVEYARD 4096
typedef struct { VkImage img; VkImageView view; VkBuffer buf; VkDeviceMemory mem; int mapped;
                 VkFramebuffer fb; } vk_grave_entry;
static vk_grave_entry s_grave[2][VK_GRAVEYARD];   /* per batch slot */
static u32 s_grave_n[2];

/* Queued on the current slot, which retires after any batch already in
 * flight, so whichever of them used the object is finished by then. */
static int vk_grave_push(VkImage img, VkImageView view, VkBuffer buf, VkDeviceMemory mem,
                         int mapped, VkFramebuffer fb)
{
    if (!s_vk.recording && !s_vk.slot_busy[0] && !s_vk.slot_busy[1]) return 0;
    const u32 k = s_vk.slot;
    if (s_grave_n[k] == VK_GRAVEYARD) { vk_submit_and_wait(); return 0; }
    vk_grave_entry* g = &s_grave[k][s_grave_n[k]++];
    g->img = img; g->view = view; g->buf = buf; g->mem = mem; g->mapped = mapped; g->fb = fb;
    return 1;
}

static void vk_grave_empty(u32 k)
{
    if (!s_grave_n[k]) return;
    vk_present_wait_gpu();
    for (u32 i = 0; i < s_grave_n[k]; i++) {
        const vk_grave_entry* g = &s_grave[k][i];
        if (g->fb)     pvkDestroyFramebuffer(s_vk.device, g->fb, NULL);
        if (g->view)   pvkDestroyImageView(s_vk.device, g->view, NULL);
        if (g->img)    pvkDestroyImage(s_vk.device, g->img, NULL);
        if (g->mapped) pvkUnmapMemory(s_vk.device, g->mem);
        if (g->buf)    pvkDestroyBuffer(s_vk.device, g->buf, NULL);
        if (g->mem)    pvkFreeMemory(s_vk.device, g->mem, NULL);
    }
    s_grave_n[k] = 0;
}

static void vk_destroy_image(vk_image* im)
{
    if ((im->img || im->view) && vk_grave_push(im->img, im->view, VK_NULL_HANDLE, im->mem, 0, VK_NULL_HANDLE)) {
        memset(im, 0, sizeof *im);
        return;
    }
    /* Never under a present's copy that may still read it. */
    if (im->img) vk_present_wait_gpu();
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    /* A buffer the GPU only copies into is one the CPU reads back, and the
     * first coherent type is usually write-combined and uncached (RADV's
     * GTT): there the readback memcpy was 82% of the FIFO thread's time in
     * Twisted Metal. Take a cached type for it when there is one; reads
     * invalidate, so a non-coherent one is fine. */
    const VkMemoryPropertyFlags cached = vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const int readback = usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (readback && vk_find_memory_type(req.memoryTypeBits, cached | coh, &type) == 0) b->coherent = 1;
    else if (readback && vk_find_memory_type(req.memoryTypeBits, cached, &type) == 0) b->coherent = 0;
    else if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

static void vk_destroy_buffer(vk_buffer* b)
{
    if (b->buf && vk_grave_push(VK_NULL_HANDLE, VK_NULL_HANDLE, b->buf, b->mem, b->ptr != NULL, VK_NULL_HANDLE)) {
        memset(b, 0, sizeof *b);
        return;
    }
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) pvkFreeMemory(s_vk.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host writes to non-coherent memory need a flush; there is no
 * vkFlushMappedMemoryRanges loaded, so writes only ever go to coherent
 * buffers (every desktop/L4T driver exposes one). Reads invalidate. */
static void vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
}

/* ---------------------------------------------------------------------------
 * One-shot command submission
 *
 * The queue is shared with the present (see present_mu in vk_state), so every
 * operation on it goes through these.
 * -------------------------------------------------------------------------*/
static VkResult vk_queue_submit(const VkSubmitInfo* si, VkFence fence)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueueSubmit(s_vk.queue, 1, si, fence);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static VkResult vk_queue_present(const VkPresentInfoKHR* pi)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueuePresentKHR(s_vk.queue, pi);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static void vk_device_idle(void)
{
    if (!s_vk.device || !pvkDeviceWaitIdle) return;
    pthread_mutex_lock(&s_vk.queue_mu);
    pvkDeviceWaitIdle(s_vk.device);
    pthread_mutex_unlock(&s_vk.queue_mu);
}

/* A full execution and memory dependency on everything submitted before
 * this command buffer, the present's copy included. */
static void vk_cmd_after_everything(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* Generic operations end an engine pass before transfers, clears, snapshots,
 * or presentation. Adjacent engine draws may share the same attachments. */
static VkPipeline s_bound_pipeline;
static VkRenderPass s_open_pass;
static VkFramebuffer s_open_fb;
static int s_draw_begin;
static void vk_close_engine_pass(void)
{
    if (!s_open_pass) return;
    pvkCmdEndRenderPass(s_vk.cmd);
    s_open_pass = VK_NULL_HANDLE;
    s_open_fb = VK_NULL_HANDLE;
}
static int vk_merge_passes(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PORT_VK_MERGE_PASSES"); on = !e || *e != '0'; }
    return on;
}

static void vk_bind_pipeline(VkPipeline pipe)
{
    if (s_bound_pipeline == pipe) return;
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    s_bound_pipeline = pipe;
}

static int vk_begin(void)
{
    if (!s_draw_begin) vk_close_engine_pass();
    if (s_vk.recording) {
        if (s_vk.present_order) { vk_cmd_after_everything(s_vk.cmd); s_vk.present_order = 0; }
        return 0;
    }
    s_bound_pipeline = VK_NULL_HANDLE;
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    /* A present's copy may still be reading the surface this frame is about
     * to draw over: order everything here after it. Set only by a present,
     * which never runs alongside the walker's recording. */
    if (s_vk.present_order) { vk_cmd_after_everything(s_vk.cmd); s_vk.present_order = 0; }
    s_vk.recording = 1;
    return 0;
}

static void vk_batch_reset(u32 k);

/* Wait for slot k's batch if it is in flight, then free what it held. */
static void vk_slot_retire(u32 k)
{
    if (s_vk.slot_busy[k]) {
        pvkWaitForFences(s_vk.device, 1, &s_vk.slot_fence[k], VK_TRUE, UINT64_MAX);
        s_vk.slot_busy[k] = 0;
    }
    vk_batch_reset(k);
    vk_grave_empty(k);
}

static int vk_submit_current(void)
{
    if (!s_vk.recording) return 0;
    vk_close_engine_pass();
    s_vk.recording = 0;
    s_vk.batch_ops = 0;
    VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
    VK_CHECK(vk_queue_submit(&si, s_vk.fence), "vkQueueSubmit");
    s_vk.slot_busy[s_vk.slot] = 1;
    return 0;
}

/* Submit everything recorded so far and wait for all of it: after this the
 * host may read what the GPU wrote and reuse or destroy anything it used. */
static int vk_submit_and_wait(void)
{
    const int rc = vk_submit_current();
    vk_slot_retire(s_vk.slot ^ 1u);
    vk_slot_retire(s_vk.slot);
    return rc;
}

/* Submit without waiting and carry on in the other slot, once the batch
 * that was last submitted from it has finished. */
static int vk_submit_async(void)
{
    if (!s_vk.recording) return 0;
    const int rc = vk_submit_current();
    s_vk.slot ^= 1u;
    s_vk.cmd = s_vk.slot_cmd[s_vk.slot];
    s_vk.fence = s_vk.slot_fence[s_vk.slot];
    vk_slot_retire(s_vk.slot);
    return rc;
}

/* The end of an operation the host does not need back: it stays in the open
 * command buffer for the next submission. */
#define VK_BATCH_OPS 4096u
static int vk_sync_mode(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_VK_SYNC"); on = e && *e && *e != '0'; }
    return on;
}
static int vk_end_op(void)
{
    if (vk_sync_mode()) return vk_submit_and_wait();
    if (++s_vk.batch_ops >= VK_BATCH_OPS) return vk_submit_async();
    return 0;
}

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image_on(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                                VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    vk_barrier_image_on(s_vk.cmd, img, aspect, old_layout);
}

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5..8, unset */
    }
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler smp = VK_NULL_HANDLE;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { vk_tex_off(unit); return; }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { vk_tex_off(unit); return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) { vk_tex_off(unit); return; }   /* no BC path yet */

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
static int vk_pick_depth_format(void)
{
    /* With stencil first -- titles use it for shadows, reflections and UI
     * masks -- and D32 first within each group: a D32 depth aspect copies out
     * as 32-bit floats, which is what depth snapshots are built from. */
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
chosen:
    s_vk.depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (s_vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        s_vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
        s_vk.depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return 0;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,     /* ignored without stencil */
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.width,
                                    .height = s_vk.height, .layers = 1 };
    VK_CHECK(pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK(pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler), "vkCreateSampler");

    /* A 1x1 magenta dummy keeps the descriptor valid before any bind --
     * the same "not bound" colour the null backend's sampler returns. */
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    if (vk_upload_texture(&s_vk.dummy, magenta, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_SPV_WORDS     (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    u32*                  spv;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    u32 n = 0;
    if (rsx_hlsl_to_spirv(s_g.hlsl, stage, s_g.spv, VK_SPV_WORDS, &n,
                          s_g.log, sizeof s_g.log) != 0) {
        VK_LOG("%s: %s\n", what, s_g.log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = s_g.spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    s_g.spv  = (u32*)malloc(VK_SPV_WORDS * sizeof(u32));
    if (!s_g.hlsl || !s_g.spv) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4 + 2 * RSX_MAX_VERTEX_TEXTURES] = {
        { RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; ++u) {
        b[4 + 2*u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING+16+u,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
        b[5 + 2*u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING+16+u,
            VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    }
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4 + 2 * RSX_MAX_VERTEX_TEXTURES, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES + RSX_MAX_VERTEX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES + RSX_MAX_VERTEX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.spv);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue as the
     * Metal and D3D12 backends write it -- x/y identity, and z remapped from
     * GL clip space to [0, 1] when the guest programmed a z scale. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, s_g.ps_off, fp_len },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vk_bind_pipeline(pipe);
    VkViewport vpt = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                             0, 1, &s_g.set, 0, NULL);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    if (vk_guest_draw(st, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vk_bind_pipeline(pipe);
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                       /* no stencil buffer yet */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, 0 };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    if (s_vk.device && pvkDeviceWaitIdle) {
        vk_device_idle();
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        if (s_vk.sem_acquire) pvkDestroySemaphore(s_vk.device, s_vk.sem_acquire, NULL);
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
}

/* Display modes: a resizable window, a borderless window covering the
 * desktop at its own resolution, or exclusive fullscreen in a video mode of
 * its own. The game always renders its 1280x720 frame; the present scales it
 * into the window keeping 16:9 (black bars elsewhere) unless stretched.
 *
 *   PORT_DISPLAY_MODE    windowed (default), borderless or fullscreen
 *   PORT_DISPLAY_SIZE    WxH: the window size, or the exclusive video mode
 *                        (the closest the display offers); default 1280x720
 *                        for a window and the desktop's mode for fullscreen
 *   PORT_DISPLAY_STRETCH 1 fills the window instead of keeping 16:9
 *
 * Alt+Enter and F11 switch between the window and fullscreen (the chosen
 * fullscreen mode, or borderless when started windowed). The older
 * PS3RECOMP_VK_FULLSCREEN=1 means borderless. */
enum { VK_DISPLAY_WINDOWED, VK_DISPLAY_BORDERLESS, VK_DISPLAY_FULLSCREEN };

static Uint32 vk_display_flags(int mode)
{
    return mode == VK_DISPLAY_FULLSCREEN ? SDL_WINDOW_FULLSCREEN
         : mode == VK_DISPLAY_BORDERLESS ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
}

static void vk_mouse_capture(int on);   /* with the event pump, below */

/* Enter a display mode on the open window. Returns 0 on success. */
static int vk_display_apply(int mode)
{
    if (mode == VK_DISPLAY_FULLSCREEN) {
        SDL_DisplayMode want = { 0 }, got;
        const int display = SDL_GetWindowDisplayIndex(s_vk.window);
        if (SDL_GetDesktopDisplayMode(display < 0 ? 0 : display, &want) == 0) {
            if (s_vk.fs_w > 0 && s_vk.fs_h > 0) { want.w = s_vk.fs_w; want.h = s_vk.fs_h; }
            if (SDL_GetClosestDisplayMode(display < 0 ? 0 : display, &want, &got))
                SDL_SetWindowDisplayMode(s_vk.window, &got);
        }
    }
    if (SDL_SetWindowFullscreen(s_vk.window, vk_display_flags(mode)) != 0) {
        VK_LOG("display mode change failed: %s\n", SDL_GetError());
        return -1;
    }
    if (mode == VK_DISPLAY_WINDOWED) SDL_SetWindowSize(s_vk.window, s_vk.win_w, s_vk.win_h);
    SDL_ShowCursor(mode == VK_DISPLAY_WINDOWED ? SDL_ENABLE : SDL_DISABLE);
    s_vk.display_mode = mode;
    vk_mouse_capture(mode != VK_DISPLAY_WINDOWED);
    __atomic_store_n(&s_vk.sc_dirty, 1, __ATOMIC_RELEASE);
    const char* names[] = { "windowed", "borderless fullscreen", "exclusive fullscreen" };
    VK_LOG("display: %s\n", names[mode]);
    return 0;
}

static void vk_display_options(void)
{
    const char* m = getenv("PORT_DISPLAY_MODE");
    s_vk.display_mode = VK_DISPLAY_WINDOWED;
    if (m && !strcmp(m, "borderless")) s_vk.display_mode = VK_DISPLAY_BORDERLESS;
    else if (m && !strcmp(m, "fullscreen")) s_vk.display_mode = VK_DISPLAY_FULLSCREEN;
    else if (!m && vk_env_on("PS3RECOMP_VK_FULLSCREEN")) s_vk.display_mode = VK_DISPLAY_BORDERLESS;
    s_vk.fullscreen_mode = s_vk.display_mode == VK_DISPLAY_WINDOWED ? VK_DISPLAY_BORDERLESS
                                                                    : s_vk.display_mode;
    int w = 0, h = 0;
    const char* size = getenv("PORT_DISPLAY_SIZE");
    if (size && sscanf(size, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 16384 && h <= 16384) {
        s_vk.fs_w = w; s_vk.fs_h = h;
    } else {
        w = (int)s_vk.width; h = (int)s_vk.height;
    }
    s_vk.win_w = w; s_vk.win_h = h;
    s_vk.stretch = vk_env_on("PORT_DISPLAY_STRETCH");
    const char* filter = getenv("PORT_DISPLAY_FILTER");
    s_vk.fsr_mode = !(filter && !strcmp(filter, "linear"));
    const char* sharp = getenv("PORT_FSR_SHARPNESS");
    s_vk.fsr_sharpness = sharp ? (float)atof(sharp) : 0.2f;
    if (s_vk.fsr_sharpness < 0.0f) s_vk.fsr_sharpness = 0.0f;
}

static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    /* On a Wayland session use Wayland. SDL before 2.0.22 (the AppImage
     * bundles Ubuntu 22.04's 2.0.20) picks X11 when both exist, and through
     * XWayland the NVIDIA driver offers no MAILBOX present mode, so the
     * window fell back to vsync-blocking FIFO. An explicit SDL_VIDEODRIVER
     * is left alone; if Wayland cannot start, SDL's own choice follows. */
#ifdef _WIN32
    int video = SDL_InitSubSystem(SDL_INIT_VIDEO);
#else
    int forced_wayland = 0;
    if (getenv("WAYLAND_DISPLAY") && !getenv("SDL_VIDEODRIVER")) {
        setenv("SDL_VIDEODRIVER", "wayland", 1);
        forced_wayland = 1;
    }
    int video = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (forced_wayland) {
        unsetenv("SDL_VIDEODRIVER");
        if (video != 0) {
            VK_LOG("window: Wayland video failed (%s), trying SDL's default\n", SDL_GetError());
            video = SDL_InitSubSystem(SDL_INIT_VIDEO);
        }
    }
#endif
    if (video != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    vk_display_options();
    /* Created as a window, then switched, so exclusive fullscreen can pick
     * its video mode first and Alt+Enter has a window size to return to. */
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   s_vk.win_w, s_vk.win_h,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_ALLOW_HIGHDPI);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    SDL_SetWindowMinimumSize(s_vk.window, 320, 180);
    if (s_vk.display_mode != VK_DISPLAY_WINDOWED) {
        const int mode = s_vk.display_mode;
        s_vk.display_mode = VK_DISPLAY_WINDOWED;
        vk_display_apply(mode);
    }
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s)\n",
           SDL_GetCurrentVideoDriver(), n);
}

static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    /* RADV on Wayland with Mesa's colour management lists well over 64
     * format/colour-space pairs. VK_INCOMPLETE only means some were left out
     * of the array: the UNORM formats we want come early, so it is no error. */
    VkSurfaceFormatKHR fmts[256];
    if (nf > 256) nf = 256;
    VkResult fr = pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts);
    if (fr != VK_SUCCESS && fr != VK_INCOMPLETE) {
        VK_LOG("vkGetPhysicalDeviceSurfaceFormatsKHR failed (VkResult %d)\n", (int)fr);
        return -1;
    }
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
        SDL_Vulkan_GetDrawableSize(s_vk.window, &w, &hh);
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) { VK_LOG("window has no drawable area\n"); return -1; }

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    /* MAILBOX when the driver has it: the window shows the newest frame at
     * the next vblank and a present never waits for one. FIFO blocks the
     * present -- which runs on the FIFO walker's thread -- until a vblank, so
     * a frame a little over 33 ms waited to 50 ms and missions ran at 20-24
     * fps instead of 30. Without MAILBOX (NVIDIA through XWayland, for one),
     * IMMEDIATE never waits either, at the cost of possible tearing.
     * PS3RECOMP_VK_VSYNC=1 keeps FIFO. */
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;   /* the one mode every driver has */
    if (!vk_env_on("PS3RECOMP_VK_VSYNC")) {
        u32 nm = 0;
        VkPresentModeKHR modes[16];
        int mailbox = 0, immediate = 0;
        if (pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &nm, NULL) == VK_SUCCESS) {
            if (nm > 16) nm = 16;
            if (pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &nm, modes) >= VK_SUCCESS)
                for (u32 i = 0; i < nm; i++) {
                    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mailbox = 1;
                    if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) immediate = 1;
                }
        }
        if (mailbox) mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (immediate) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    static const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (size_t k = 0; k < 4; k++)
        if (caps.supportedCompositeAlpha & alphas[k]) { alpha = alphas[k]; break; }

    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = mode,
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_count = n;
    VK_LOG("window swapchain %ux%u, %u images, format %d, %s\n", ext.width, ext.height, n, (int)fmt.format,
           mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" :
           mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo (vsync)");
    return 0;
}

static void vk_swapchain_lost(void)
{
    vk_device_idle();
    if (vk_create_swapchain()) vk_window_drop("the swapchain could not be recreated");
}

/* ---------------------------------------------------------------------------
 * FSR 1 for the window
 *
 * When the window shows the frame larger than the game renders it, the
 * present upscales with AMD FidelityFX Super Resolution 1.0 instead of a
 * bilinear blit: EASU (edge-adaptive upscaling) into fsr_mid at the output
 * size, RCAS (sharpening) into fsr_out, then a 1:1 blit into the swapchain
 * image, which converts to its format. Compute shaders and constants come
 * from fsr1_bridge.c. PORT_DISPLAY_FILTER=linear keeps the bilinear blit;
 * PORT_FSR_SHARPNESS sets RCAS in stops (default 0.2, 0 the strongest).
 * Any setup failure is logged once and the window keeps the bilinear blit.
 * -------------------------------------------------------------------------*/
/* What the window shows: the frame at the internal resolution when there is
 * one (s_vk.display), else the guest-size display image. */
static const vk_image* vk_window_source(u32* w, u32* h)
{
    if (s_vk.res_scale > 1 && s_vk.display.img) {
        *w = s_vk.width * s_vk.res_scale; *h = s_vk.height * s_vk.res_scale;
        return &s_vk.display;
    }
    *w = s_vk.width; *h = s_vk.height;
    return &s_vk.color;
}

const uint32_t* port_fsr1_easu_code(size_t* bytes);
const uint32_t* port_fsr1_rcas_code(size_t* bytes);
void port_fsr1_easu_constants(uint32_t out[16], uint32_t in_w, uint32_t in_h,
                              uint32_t out_w, uint32_t out_h);
void port_fsr1_rcas_constants(uint32_t out[4], float sharpness);

static int vk_fsr_fail(const char* what)
{
    VK_LOG("FSR disabled, using bilinear scaling: %s\n", what);
    s_vk.fsr_broken = 1;
    return -1;
}

static int vk_fsr_pipeline(const uint32_t* code, size_t bytes, VkPipeline* out)
{
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = bytes, .pCode = code };
    VkShaderModule m;
    if (pvkCreateShaderModule(s_vk.device, &mci, NULL, &m) != VK_SUCCESS) return -1;
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main" },
        .layout = s_vk.fsr_layout,
    };
    const VkResult r = pvkCreateComputePipelines(s_vk.device, VK_NULL_HANDLE, 1, &ci, NULL, out);
    pvkDestroyShaderModule(s_vk.device, m, NULL);
    return r == VK_SUCCESS ? 0 : -1;
}

/* Pipelines, layout, sampler and descriptor sets, made on first use. */
static int vk_fsr_init(void)
{
    if (s_vk.fsr_easu) return 0;
    if (s_vk.fsr_broken) return -1;
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return vk_fsr_fail("RGBA8 storage images are not supported");
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.fsr_sampler) != VK_SUCCESS)
        return vk_fsr_fail("vkCreateSampler");
    const VkDescriptorSetLayoutBinding binds[3] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 3, .pBindings = binds };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &dci, NULL, &s_vk.fsr_dsl) != VK_SUCCESS)
        return vk_fsr_fail("vkCreateDescriptorSetLayout");
    const VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &s_vk.fsr_dsl,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    if (pvkCreatePipelineLayout(s_vk.device, &lci, NULL, &s_vk.fsr_layout) != VK_SUCCESS)
        return vk_fsr_fail("vkCreatePipelineLayout");
    size_t bytes;
    const uint32_t* code = port_fsr1_easu_code(&bytes);
    if (vk_fsr_pipeline(code, bytes, &s_vk.fsr_easu)) return vk_fsr_fail("EASU pipeline");
    code = port_fsr1_rcas_code(&bytes);
    if (vk_fsr_pipeline(code, bytes, &s_vk.fsr_rcas)) return vk_fsr_fail("RCAS pipeline");
    const VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 2, .poolSizeCount = 3, .pPoolSizes = sizes };
    if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.fsr_pool) != VK_SUCCESS)
        return vk_fsr_fail("vkCreateDescriptorPool");
    const VkDescriptorSetLayout layouts[2] = { s_vk.fsr_dsl, s_vk.fsr_dsl };
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = s_vk.fsr_pool, .descriptorSetCount = 2,
                                       .pSetLayouts = layouts };
    if (pvkAllocateDescriptorSets(s_vk.device, &ai, s_vk.fsr_sets) != VK_SUCCESS)
        return vk_fsr_fail("vkAllocateDescriptorSets");
    VK_LOG("FSR 1 upscaling for the window (RCAS sharpness %.2f)\n", (double)s_vk.fsr_sharpness);
    return 0;
}

static void vk_fsr_bind(VkDescriptorSet set, VkImageView in, VkImageView out)
{
    const VkDescriptorImageInfo src = { VK_NULL_HANDLE, in, VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorImageInfo smp = { s_vk.fsr_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    const VkDescriptorImageInfo dst = { VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL };
    const VkWriteDescriptorSet w[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &src },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &smp },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dst },
    };
    pvkUpdateDescriptorSets(s_vk.device, 3, w, 0, NULL);
}

/* The two intermediate images at the output size. Only rebuilt when the
 * size changes (a resize), after the GPU is done with the old ones. */
static int vk_fsr_targets(u32 w, u32 h, VkImageView src)
{
    if (s_vk.fsr_w == w && s_vk.fsr_h == h && s_vk.fsr_out.img) {
        if (s_vk.fsr_src != src) { vk_device_idle(); vk_fsr_bind(s_vk.fsr_sets[0], src, s_vk.fsr_mid.view); s_vk.fsr_src = src; }
        return 0;
    }
    vk_device_idle();
    vk_destroy_image(&s_vk.fsr_mid);
    vk_destroy_image(&s_vk.fsr_out);
    s_vk.fsr_w = s_vk.fsr_h = 0;
    if (vk_create_image(&s_vk.fsr_mid, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        vk_create_image(&s_vk.fsr_out, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return vk_fsr_fail("intermediate images");
    vk_fsr_bind(s_vk.fsr_sets[0], src, s_vk.fsr_mid.view);
    vk_fsr_bind(s_vk.fsr_sets[1], s_vk.fsr_mid.view, s_vk.fsr_out.view);
    s_vk.fsr_w = w; s_vk.fsr_h = h; s_vk.fsr_src = src;
    return 0;
}

static void vk_fsr_image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from,
                                 VkAccessFlags src_access, VkPipelineStageFlags src_stage,
                                 VkAccessFlags dst_access, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Record EASU and RCAS from the frame into fsr_out at w x h. Returns 0 when
 * recorded, nonzero to fall back to the bilinear blit. */
static int vk_fsr_record(VkCommandBuffer cb, const vk_image* src, u32 sw, u32 sh, u32 w, u32 h)
{
    if (!s_vk.fsr_mode || vk_fsr_init() || vk_fsr_targets(w, h, src->view)) return -1;
    const VkPipelineStageFlags cs = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    /* The frame: written by rendering, transfers and the readback copy. */
    vk_fsr_image_barrier(cb, src->img, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_SHADER_READ_BIT, cs);
    vk_fsr_image_barrier(cb, s_vk.fsr_mid.img, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_ACCESS_SHADER_READ_BIT, cs, VK_ACCESS_SHADER_WRITE_BIT, cs);
    uint32_t con[16];
    port_fsr1_easu_constants(con, sw, sh, w, h);
    const u32 gx = (w + 15u) / 16u, gy = (h + 15u) / 16u;
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_vk.fsr_easu);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_vk.fsr_layout, 0, 1, &s_vk.fsr_sets[0], 0, NULL);
    pvkCmdPushConstants(cb, s_vk.fsr_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof con, con);
    pvkCmdDispatch(cb, gx, gy, 1);
    vk_fsr_image_barrier(cb, s_vk.fsr_mid.img, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_SHADER_WRITE_BIT, cs, VK_ACCESS_SHADER_READ_BIT, cs);
    vk_fsr_image_barrier(cb, s_vk.fsr_out.img, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT, cs);
    memset(con, 0, sizeof con);
    port_fsr1_rcas_constants(con, s_vk.fsr_sharpness);
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_vk.fsr_rcas);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_vk.fsr_layout, 0, 1, &s_vk.fsr_sets[1], 0, NULL);
    pvkCmdPushConstants(cb, s_vk.fsr_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof con, con);
    pvkCmdDispatch(cb, gx, gy, 1);
    vk_fsr_image_barrier(cb, s_vk.fsr_out.img, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_SHADER_WRITE_BIT, cs, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* Later writes to the frame wait for these reads, as for the blit. */
    vk_fsr_image_barrier(cb, src->img, VK_IMAGE_LAYOUT_GENERAL,
                         0, cs, 0, VK_PIPELINE_STAGE_TRANSFER_BIT);
    return 0;
}

static void vk_fsr_destroy(void)
{
    if (!s_vk.device) return;
    vk_destroy_image(&s_vk.fsr_mid);
    vk_destroy_image(&s_vk.fsr_out);
    if (s_vk.win_dump.buf) vk_destroy_buffer(&s_vk.win_dump);
    if (s_vk.fsr_easu) pvkDestroyPipeline(s_vk.device, s_vk.fsr_easu, NULL);
    if (s_vk.fsr_rcas) pvkDestroyPipeline(s_vk.device, s_vk.fsr_rcas, NULL);
    if (s_vk.fsr_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.fsr_layout, NULL);
    if (s_vk.fsr_pool) pvkDestroyDescriptorPool(s_vk.device, s_vk.fsr_pool, NULL);
    if (s_vk.fsr_dsl) pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.fsr_dsl, NULL);
    if (s_vk.fsr_sampler) pvkDestroySampler(s_vk.device, s_vk.fsr_sampler, NULL);
    s_vk.fsr_easu = s_vk.fsr_rcas = VK_NULL_HANDLE;
    s_vk.fsr_layout = VK_NULL_HANDLE; s_vk.fsr_pool = VK_NULL_HANDLE;
    s_vk.fsr_dsl = VK_NULL_HANDLE; s_vk.fsr_sampler = VK_NULL_HANDLE;
    s_vk.fsr_w = s_vk.fsr_h = 0;
}

/* The frame's place in the window: all of it when stretched, otherwise the
 * largest centred rectangle with the frame's own aspect ratio. */
static void vk_window_rect(int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1)
{
    const u32 W = s_vk.sc_extent.width, H = s_vk.sc_extent.height;
    u32 w = W, h = H;
    if (!s_vk.stretch) {
        if ((u64)W * s_vk.height > (u64)H * s_vk.width) w = (u32)((u64)H * s_vk.width / s_vk.height);
        else                                             h = (u32)((u64)W * s_vk.height / s_vk.width);
        if (!w) w = 1;
        if (!h) h = 1;
    }
    *x0 = (int32_t)((W - w) / 2); *y0 = (int32_t)((H - h) / 2);
    *x1 = *x0 + (int32_t)w;       *y1 = *y0 + (int32_t)h;
}

/* PORT_WINDOW_DUMP=<file>: write one frame as the window gets it (the FSR
 * output, or the frame at the internal resolution) to a PPM, once, after
 * PORT_WINDOW_DUMP_FRAME presented frames (default 600), to check scaling
 * without a screenshot tool. With PORT_WINDOW_DUMP_TRIGGER=<file> instead, the
 * frame after that file appears is written (and the file removed), so a test
 * script can pick a moment rather than a frame count. Debug only: it waits for
 * the GPU. */
static void vk_window_dump(VkCommandBuffer cb, VkImage img, u32 w, u32 h)
{
    const char* path = getenv("PORT_WINDOW_DUMP");
    if (!path || s_vk.win_dump_state == 2) return;
    if (s_vk.win_dump_state == 1) {
        vk_device_idle();
        vk_invalidate(&s_vk.win_dump);
        FILE* f = fopen(path, "wb");
        if (f) {
            const unsigned char* px = (const unsigned char*)s_vk.win_dump.ptr;
            fprintf(f, "P6\n%u %u\n255\n", s_vk.win_dump_w, s_vk.win_dump_h);
            for (size_t i = 0; i < (size_t)s_vk.win_dump_w * s_vk.win_dump_h; i++) fwrite(px + i * 4, 1, 3, f);
            fclose(f);
            VK_LOG("window frame %ux%u written to %s\n", s_vk.win_dump_w, s_vk.win_dump_h, path);
        }
        s_vk.win_dump_state = 2;
        return;
    }
    const char* trigger = getenv("PORT_WINDOW_DUMP_TRIGGER");
    if (trigger && *trigger) {
        if (access(trigger, F_OK) != 0) return;
        unlink(trigger);
    } else {
        const char* at = getenv("PORT_WINDOW_DUMP_FRAME");
        if (s_vk.presented < (at ? (u32)atoi(at) : 600u)) return;
    }
    if (
        vk_create_host_buffer(&s_vk.win_dump, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return;
    vk_barrier_image_on(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    const VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                       .imageExtent = { w, h, 1 } };
    pvkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, s_vk.win_dump.buf, 1, &region);
    s_vk.win_dump_w = w; s_vk.win_dump_h = h;
    s_vk.win_dump_state = 1;
}

/* Record the frame's blit into swapchain image idx, black around it, and its
 * transition for presenting. */
static void vk_window_blit(VkCommandBuffer cb, u32 idx)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    int32_t x0, y0, x1, y1;
    vk_window_rect(&x0, &y0, &x1, &y1);
    if (x0 > 0 || y0 > 0) {
        const VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
        const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pvkCmdClearColorImage(cb, s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &black, 1, &all);
        VkMemoryBarrier wb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 1, &wb, 0, NULL, 0, NULL);
    }
    const u32 out_w = (u32)(x1 - x0), out_h = (u32)(y1 - y0);
    u32 sw, sh;
    const vk_image* src = vk_window_source(&sw, &sh);
    vk_barrier_image_on(cb, src->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    /* FSR only enlarges; a window at or below the frame size blits. */
    const int fsr = (out_w > sw || out_h > sh) && !vk_fsr_record(cb, src, sw, sh, out_w, out_h);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, fsr ? (VkOffset3D){ (int32_t)out_w, (int32_t)out_h, 1 }
                                         : (VkOffset3D){ (int32_t)sw, (int32_t)sh, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { x0, y0, 0 }, { x1, y1, 1 } },
    };
    pvkCmdBlitImage(cb, fsr ? s_vk.fsr_out.img : src->img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, fsr ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    vk_window_dump(cb, fsr ? s_vk.fsr_out.img : src->img, fsr ? out_w : sw, fsr ? out_h : sh);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
}

/* A resize or display mode change seen by the event pump. */
static void vk_window_check_resize(void)
{
    if (__atomic_exchange_n(&s_vk.sc_dirty, 0, __ATOMIC_ACQ_REL)) vk_swapchain_lost();
}

/* Blit the offscreen target into the next swapchain image and present it. */
static void vk_present_window(void)
{
    if (!s_vk.windowed || !s_vk.swapchain) return;
    vk_window_check_resize();
    if (!s_vk.windowed || !s_vk.swapchain) return;

    u32 idx = 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return;
    }

    /* The present's own command buffer: free, since every caller has waited
     * for its copy (present_fence) first. */
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) { VK_LOG("vkResetCommandBuffer (window) failed\n"); return; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { VK_LOG("vkBeginCommandBuffer (window) failed\n"); return; }
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_window_blit(cb, idx);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer failed\n"); return; }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sem_acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    pvkResetFences(s_vk.device, 1, &s_vk.window_fence);
    if (vk_queue_submit(&si, s_vk.window_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (window) failed\n"); return;
    }
    pvkWaitForFences(s_vk.device, 1, &s_vk.window_fence, VK_TRUE, UINT64_MAX);

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    r = vk_queue_present(&pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    FILE* f = fopen(s_vk.dump_path, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", s_vk.dump_path); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.width, s_vk.height);
    for (size_t i = 0; i < (size_t)s_vk.width * s_vk.height; i++)
        fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Present
 *
 * In two halves. The first records, on the present's own command buffer,
 * whatever puts the frame into the offscreen target (the engine's blit of
 * the flipped surface) and the copy of that target into `readback`, and
 * submits it without waiting. The second waits for it, reads the centre
 * pixel and the frame dump back, and blits the target into the window.
 *
 * The first half reads the frame, so it must not run while the walker is
 * recording; the second touches only the target, `readback` and the
 * present's own objects, none of which the engine path draws into, so a host
 * can run it on a thread of its own beside the walker
 * (rsx_vulkan_backend_present_begin / _finish). On the fixed-function path
 * the walker draws into the target itself, so there the second half always
 * follows the first at once.
 * -------------------------------------------------------------------------*/

/* Wait until the GPU is done with a present's copy. Safe from any thread:
 * present_fence is reset only when a present starts, which never runs
 * alongside the walker. */
/* Record the blit of the frame into the next swapchain image on cb, which
 * the caller submits waiting on sem_acquire and signalling sc_done[*idx];
 * then vk_window_queue presents it. The window path no longer has a submit
 * and a CPU wait of its own: the copy, the blit and the present all run on
 * the GPU behind the frame's batch. Returns 0 when no image was acquired. */
static int vk_window_record(VkCommandBuffer cb, u32* idx)
{
    if (!s_vk.windowed || !s_vk.swapchain) return 0;
    vk_window_check_resize();
    if (!s_vk.windowed || !s_vk.swapchain) return 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return 0;
    }
    vk_window_blit(cb, *idx);
    return 1;
}

static void vk_window_queue(u32 idx)
{
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    VkResult r = vk_queue_present(&pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
}

static void vk_present_wait_gpu(void)
{
    if (__atomic_load_n(&s_vk.present_inflight, __ATOMIC_ACQUIRE) && s_vk.present_fence)
        pvkWaitForFences(s_vk.device, 1, &s_vk.present_fence, VK_TRUE, UINT64_MAX);
}

/* present_mu held. */
static void vk_present_complete_locked(void)
{
    if (!s_vk.present_inflight) return;
    pvkWaitForFences(s_vk.device, 1, &s_vk.present_fence, VK_TRUE, UINT64_MAX);
    __atomic_store_n(&s_vk.present_inflight, 0, __ATOMIC_RELEASE);

    s_vk.presented++;
    if (s_vk.present_readback) {
        vk_invalidate(&s_vk.readback);
        const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
        size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
        s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
        if (s_vk.dump_path && (s_vk.presented % s_vk.dump_every) == 0) vk_dump_ppm(px);
    }
    if (!s_vk.present_shown) vk_present_window();
}

/* present_mu held: a previous present finished, the present's command
 * buffer begun and ordered after everything submitted so far. */
static VkCommandBuffer vk_present_record_begin(void)
{
    vk_present_complete_locked();
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_cmd_after_everything(cb);
    return cb;
}

/* present_mu held: copy the target out, submit, and finish at once unless a
 * host asked for the halves separately on the engine path. */
static void vk_present_record_end(VkCommandBuffer cb)
{
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    /* The host copy of the frame feeds the PPM dumps and the headless
     * readback checks; a windowed run without dumps skips it. */
    s_vk.present_readback = !s_vk.windowed || s_vk.dump_path != NULL;
    u32 sc_idx = 0;
    s_vk.present_shown = vk_window_record(cb, &sc_idx);
    if (s_vk.present_readback) {
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    }
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (present) failed\n"); return; }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (s_vk.present_shown) {
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s_vk.sem_acquire; si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s_vk.sc_done[sc_idx];
    }
    pvkResetFences(s_vk.device, 1, &s_vk.present_fence);
    if (vk_queue_submit(&si, s_vk.present_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (present) failed\n"); return;
    }
    if (s_vk.present_shown) vk_window_queue(sc_idx);
    __atomic_store_n(&s_vk.present_inflight, 1, __ATOMIC_RELEASE);
    s_vk.present_order = 1;
    if (!(s_vk.present_split && s_vk.eng_active)) vk_present_complete_locked();
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;
    vk_submit_and_wait();
    pthread_mutex_lock(&s_vk.present_mu);
    VkCommandBuffer cb = vk_present_record_begin();
    if (cb) vk_present_record_end(cb);
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Every operation is still one synchronous submission, so submit_and_wait has
 * nothing left to do by the time the engine calls it -- which is exactly the
 * contract it asks for (staging is free to reuse on return).
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 2048          /* the engine's texture cache is 1024 */

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8       kind;
    VkFormat fmt;
    u32      bpp;                    /* bytes per texel, 0 for depth     */
    u32      w, h;
    vk_image im;                     /* a VIEW owns only im.view            */
    u32      block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32      surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
    u8       cube;                   /* TEXTURE: six layers, a CUBE view    */
    u8       snapshot;               /* COLOR: a feedback copy, never a target */
    u8       scale;                  /* render targets and their copies: the
                                      * internal resolution scale (w, h are the
                                      * physical size); 0 or 1 = guest size  */
} vk_eng_obj;

static u32 vk_obj_scale(const vk_eng_obj* o) { return o && o->scale > 1 ? o->scale : 1u; }

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];

/* Per-batch memory for the engine path (see the header): one host-coherent
 * buffer handed out linearly, and a descriptor pool with room for
 * VK_BATCH_SETS sets of either layout. An operation reserves everything it
 * needs before recording (vk_batch_reserve), so the submission a full arena
 * forces never lands in the middle of one. Constants are copied in when they
 * change or a new batch starts; draws in between share the copy. */
#define VK_BATCH_BYTES (32u << 20)
#define VK_BATCH_SETS  2048u
#define VK_BATCH_UBO_ALIGN 256u        /* the largest minUniformBufferOffsetAlignment allowed */
typedef struct {
    vk_buffer        buf;
    VkDeviceSize     used;
    VkDescriptorPool pool;
    u32              sets;
    u32              gen;              /* unique per reset, across both slots */
    u32              vs_gen, ps_gen;   /* batch the current constants were copied into */
    VkDeviceSize     vs_off, ps_off;
} vk_batch;
static vk_batch s_batches[2];
static u32 s_batch_gen;
#define s_batch (s_batches[s_vk.slot])
static u8  s_vs_shadow[VK_VPCONST_BYTES + 256u];
static u8  s_ps_shadow[(RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u];

static void vk_batch_reset(u32 k)
{
    vk_batch* b = &s_batches[k];
    b->used = 0;
    b->gen = ++s_batch_gen;
    if (b->sets && b->pool) pvkResetDescriptorPool(s_vk.device, b->pool, 0);
    b->sets = 0;
}

static int vk_batch_init(void)
{
    if (!s_batch.pool) {
        VkDescriptorPoolSize ps[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2u * VK_BATCH_SETS },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (RSX_MAX_TEXTURES + RSX_MAX_VERTEX_TEXTURES) * VK_BATCH_SETS },
            { VK_DESCRIPTOR_TYPE_SAMPLER, (RSX_MAX_TEXTURES + RSX_MAX_VERTEX_TEXTURES) * VK_BATCH_SETS },
        };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .maxSets = VK_BATCH_SETS, .poolSizeCount = 3, .pPoolSizes = ps };
        if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_batch.pool) != VK_SUCCESS) {
            s_batch.pool = VK_NULL_HANDLE; return -1;
        }
    }
    return 0;
}

/* Room for `bytes` (plus alignment slack) and `sets` descriptor sets in the
 * current batch, submitting it first if it is full. */
static int vk_batch_reserve(VkDeviceSize bytes, u32 sets)
{
    if (vk_batch_init()) return -1;
    bytes += 4u * VK_BATCH_UBO_ALIGN;
    if (s_batch.buf.buf && s_batch.used + bytes <= s_batch.buf.size && s_batch.sets + sets <= VK_BATCH_SETS)
        return 0;
    vk_submit_async();              /* the other slot, now empty */
    if (vk_batch_init()) return -1;
    if (s_batch.buf.buf && bytes <= s_batch.buf.size) return 0;
    VkDeviceSize size = s_batch.buf.size ? s_batch.buf.size : VK_BATCH_BYTES;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_batch.buf);
    if (vk_create_host_buffer(&s_batch.buf, size,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !s_batch.buf.coherent) {
        vk_destroy_buffer(&s_batch.buf);
        VK_LOG("batch: no host-coherent memory\n");
        return -1;
    }
    return 0;
}

/* Inside a reservation: never fails. */
static VkDeviceSize vk_batch_take(const void* src, VkDeviceSize bytes, VkDeviceSize align)
{
    const VkDeviceSize off = (s_batch.used + align - 1) & ~(align - 1);
    if (src) memcpy((u8*)s_batch.buf.ptr + off, src, (size_t)bytes);
    s_batch.used = off + bytes;
    return off;
}

static VkDescriptorSet vk_batch_set(VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_batch.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &layout };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_batch.sets++;
    return set;
}

/* Defined with the E2 pipeline state further down. */
static void vk_eng_drop_framebuffers(void);
static void vk_eng2_shutdown(void* user);

static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} s_eng_bound;

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

/* Colour formats the E2 passes render into: RGBA8 for ordinary and display
 * targets, RGBA16F for SET_SURFACE_FORMAT 0xB HDR targets. Both are required
 * colour attachment formats with blending in Vulkan 1.0. */
#define VK_ENG_RT_FORMATS 2
static int vk_eng_rt_index(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:      return 0;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 1;
    default:                            return -1;
    }
}

/* The largest image side the engine creates: the device's own limit once a
 * device is picked, the 4096 Vulkan guarantees before. */
static u32 vk_max_dim(void) { return s_vk.max_dim ? s_vk.max_dim : 4096u; }

static u32 vk_eng_alloc(void)
{
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_FREE) return i;
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == kind) ? o : NULL;
}

/* Copy `rows` rows of `w` texels from host memory into a w x h rectangle at
 * (x, y) of one level / layer of an image that is in GENERAL. */
static int vk_eng_upload_region(VkImage img, u32 layer, u32 mip, u32 x, u32 y,
                                u32 w, u32 h, size_t tight, u32 rows,
                                const void* src, u32 row_bytes)
{
    if (tight > row_bytes) tight = row_bytes;
    const VkDeviceSize bytes = (VkDeviceSize)tight * rows;
    if (bytes <= VK_BATCH_BYTES / 4u && !vk_batch_reserve(bytes, 0)) {
        const VkDeviceSize off = vk_batch_take(NULL, bytes, 16u);
        for (u32 y = 0; y < rows; y++)
            memcpy((u8*)s_batch.buf.ptr + off + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
        if (vk_begin()) return -1;
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .bufferOffset = off,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, s_batch.buf.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        /* Visible to the draws recorded after it (they sample it). */
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        return vk_end_op();
    }
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * rows, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !st.coherent) { vk_destroy_buffer(&st); return -1; }
    for (u32 y = 0; y < rows; y++)
        memcpy((u8*)st.ptr + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&st);
    return rc;
}

/* The same into the whole of a level, from its origin. */
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes)
{
    return vk_eng_upload_region(img, layer, mip, 0, 0, w, h, tight, rows, src, row_bytes);
}

static int vk_eng_upload(VkImage img, u32 layer, u32 mip, u32 w, u32 h, u32 bpp,
                         const void* src, u32 row_bytes)
{
    return vk_eng_upload_rows(img, layer, mip, w, h, (size_t)w * bpp, h, src, row_bytes);
}

/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                            int cube, VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, cube ? 6u : 1u },
    };
    return pvkCreateImageView(s_vk.device, &vci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static int vk_eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    memset(s_eobj, 0, sizeof s_eobj);
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    return s_vk.device ? 0 : -1;
}

static void vk_eng_snapshot_recycle(vk_eng_obj* o);

static void vk_eng_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE) return;
    if (s_eobj[handle].kind == VK_ENG_COLOR)
        for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
                vk_destroy_image(&s_eobj[i].im);
                memset(&s_eobj[i], 0, sizeof s_eobj[i]);
            }
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].surface) {
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR && s_eobj[handle].snapshot) {
        /* Never an attachment, so no framebuffer names it; the image goes
         * back to the pool for the next feedback draw. */
        vk_eng_snapshot_recycle(&s_eobj[handle]);
        memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
        return;
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH)
        vk_eng_drop_framebuffers();
    vk_destroy_image(&s_eobj[handle].im);      /* idle: submissions are waited on */
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

static void vk_eng_snapshot_pool_drain(void);

static void vk_eng_shutdown(void* user)
{
    vk_eng2_shutdown(user);
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) vk_eng_release(user, i);
    vk_eng_snapshot_pool_drain();
}

static void vk_eng_submit_and_wait(void* user, u32 reason)
{
    (void)user; (void)reason;
    vk_submit_and_wait();
}

/* ---- internal resolution ------------------------------------------------
 * PORT_RENDER_SCALE=N renders every colour and depth target N times larger
 * in each direction. The engine keeps working in guest sizes; this backend
 * multiplies at its edges: target sizes, viewport and scissor, the screen
 * position fragment programs read (vk_eng_scale_wpos), and the pixels that
 * cross to or from guest memory. Those are converted on the CPU, each guest
 * texel repeated N x N going up and one sample per N x N block coming back,
 * never averaged: titles pack depth and other data into colour targets. */
static u32 vk_target_scale(u32 w, u32 h)
{
    const u32 sc = s_vk.res_scale > 1 ? s_vk.res_scale : 1u;
    return (w * sc <= vk_max_dim() && h * sc <= vk_max_dim()) ? sc : 1u;
}

/* A w x h block of bpp-byte texels, each repeated sc x sc. NULL on no memory. */
static void* vk_scale_rows_up(const void* rows, u32 row_bytes, u32 w, u32 h, u32 bpp, u32 sc)
{
    const size_t out_row = (size_t)w * sc * bpp;
    u8* out = (u8*)malloc(out_row * h * sc);
    if (!out) return NULL;
    for (u32 y = 0; y < h; y++) {
        const u8* src = (const u8*)rows + (size_t)y * row_bytes;
        u8* dst = out + (size_t)y * sc * out_row;
        for (u32 x = 0; x < w; x++)
            for (u32 k = 0; k < sc; k++) memcpy(dst + ((size_t)x * sc + k) * bpp, src + (size_t)x * bpp, bpp);
        for (u32 k = 1; k < sc; k++) memcpy(dst + k * out_row, dst, out_row);
    }
    return out;
}

/* One texel from the middle of each sc x sc block of `phys` (tight rows of
 * w*sc texels) into out. */
static void vk_scale_rows_down(const u8* phys, u32 w, u32 h, u32 bpp, u32 sc, void* out, u32 out_pitch)
{
    const size_t phys_row = (size_t)w * sc * bpp;
    for (u32 y = 0; y < h; y++) {
        const u8* src = phys + ((size_t)y * sc + sc / 2) * phys_row;
        u8* dst = (u8*)out + (size_t)y * out_pitch;
        if (sc == 1) { memcpy(dst, src, (size_t)w * bpp); continue; }
        for (u32 x = 0; x < w; x++) memcpy(dst + (size_t)x * bpp, src + ((size_t)x * sc + sc / 2) * bpp, bpp);
    }
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    /* Only formats the E2 passes are built for (vk_eng_rt_index). */
    if (vk_eng_rt_index(vf) < 0 || !w || !h || w > vk_max_dim() || h > vk_max_dim()) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    const u32 sc = vk_target_scale(w, h);
    if (vk_create_image(&o->im, vf, w * sc, h * sc,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = w * sc; o->h = h * sc; o->scale = (u8)sc;
    /* UNDEFINED -> GENERAL, then the guest's own bytes when the engine could
     * resolve them, else transparent black so the contents are defined. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    if (vk_end_op()) { vk_eng_release(user, hd); return 0; }
    if (seed && seed_row_bytes) {
        void* up = sc > 1 ? vk_scale_rows_up(seed, seed_row_bytes, w, h, bpp, sc) : NULL;
        if (sc == 1) vk_eng_upload(o->im.img, 0, 0, w, h, bpp, seed, seed_row_bytes);
        else if (up) vk_eng_upload(o->im.img, 0, 0, w * sc, h * sc, bpp, up, w * sc * bpp);
        free(up);
    }
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    if (!w || !h || w > vk_max_dim() || h > vk_max_dim()) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    const u32 sc = vk_target_scale(w, h);
    if (vk_create_image(&o->im, s_vk.depth_format, w * sc, h * sc,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        s_vk.depth_aspect)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_DEPTH; o->fmt = s_vk.depth_format; o->w = w * sc; o->h = h * sc; o->scale = (u8)sc;
    /* Far plane, for the reason the display depth starts there. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_end_op()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_bound.rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    s_eng_bound.nrt = count;
    s_eng_bound.depth = depth;
}

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || vk_begin()) return;
    VkClearColorValue v;
    memcpy(v.float32, rgba, sizeof v.float32);
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    vk_end_op();
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    VkImageAspectFlags aspect = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & RSX_BE_CLEAR_STENCIL) aspect |= s_vk.depth_aspect & VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!o || !aspect || vk_begin()) return;
    VkClearDepthStencilValue d = { depth_value, stencil };
    VkImageSubresourceRange r = { aspect, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    vk_end_op();
}

/* The named surface becomes the frame: blit it onto the display image, then
 * the existing present does the readback, the PPM dump and the window. */
static void vk_eng_present(void* user, u32 surface)
{
    (void)user;
    if (!s_vk.device) return;
    vk_submit_async();             /* the frame's batch, queued ahead of the copy out of it */
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    pthread_mutex_lock(&s_vk.present_mu);
    VkCommandBuffer cb = vk_present_record_begin();
    if (cb) {
        if (o) {
            vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            VkImageBlit blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
            };
            pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                            (o->w == s_vk.width && o->h == s_vk.height) ? VK_FILTER_NEAREST
                                                                        : VK_FILTER_LINEAR);
            /* At an internal scale the window shows the full-size frame:
             * the guest-size copy above stays for readback and dumps. */
            const u32 dw = s_vk.width * s_vk.res_scale, dh = s_vk.height * s_vk.res_scale;
            if (s_vk.res_scale > 1 && s_vk.windowed && !s_vk.display.img &&
                !vk_create_image(&s_vk.display, VK_FORMAT_R8G8B8A8_UNORM, dw, dh,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
                vk_barrier_image_on(cb, s_vk.display.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
            if (s_vk.display.img) {
                vk_barrier_image_on(cb, s_vk.display.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
                blit.dstOffsets[1] = (VkOffset3D){ (int32_t)dw, (int32_t)dh, 1 };
                pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                                s_vk.display.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                                (o->w == dw && o->h == dh) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
            }
        }
        vk_present_record_end(cb);
    }
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* Pixels a 2D transfer put into guest memory, pushed into the colour target
 * that rectangle belongs to. */
static void vk_eng_color_target_write(void* user, u32 surface, u32 x, u32 y,
                                      u32 w, u32 h, const void* rows, u32 row_bytes)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || o->fmt != VK_FORMAT_R8G8B8A8_UNORM) return;   /* rows are R,G,B,A */
    const u32 sc = vk_obj_scale(o);
    if (!rows || !w || !h || x + w > o->w / sc || y + h > o->h / sc) return;
    if (sc == 1) {
        vk_eng_upload_region(o->im.img, 0, 0, x, y, w, h, (size_t)w * o->bpp, h, rows, row_bytes);
        return;
    }
    void* up = vk_scale_rows_up(rows, row_bytes, w, h, o->bpp, sc);
    if (up) vk_eng_upload_region(o->im.img, 0, 0, x * sc, y * sc, w * sc, h * sc,
                                 (size_t)w * sc * o->bpp, h * sc, up, w * sc * o->bpp);
    free(up);
}

static void vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    const u32 sc = vk_obj_scale(o);
    if (!o || !out || !out_pitch || !w || !h || x + w > o->w / sc || y + h > o->h / sc) return;
    /* The guest rectangle's physical pixels, then one per sc x sc block. */
    const u32 gw = w, gh = h;
    x *= sc; y *= sc; w *= sc; h *= sc;
    const size_t tight = (size_t)w * o->bpp;
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return;
    }
    if (!vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        if (!vk_submit_and_wait()) {
            vk_invalidate(&st);
            vk_scale_rows_down((const u8*)st.ptr, gw, gh, o->bpp, sc, out, out_pitch);
        }
    }
    vk_destroy_buffer(&st);
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. A cube map (faces == 6) is a
 * six-layer cube-compatible image, each face uploaded into its layer, sampled
 * through a CUBE view; the pipelines that sample one use the split set layout
 * (vk_eng_split_layout). */
static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > vk_max_dim() || h > vk_max_dim() ||
        (bc && !s_vk.bc_ok))
        return 0;
    const int cube = (faces == 6);
    if ((faces != 1 && !cube) || (cube && w != h)) return 0;   /* cube faces are square */
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = VK_IMAGE_TYPE_2D,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = cube ? 6u : 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), cube, &o->im.view))
        goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->cube = (u8)cube;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    /* UNDEFINED -> GENERAL for every level and face before the uploads arrive. */
    if (vk_begin()) goto fail;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_end_op()) goto fail;
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || !src || !row_bytes || !rows || face >= (o->cube ? 6u : 1u)) return;
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    vk_eng_upload_rows(o->im.img, face, mip, w, h, tight, rows, src, row_bytes);
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t) return 0;
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
            s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format)
            return i;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_eng_make_view(t->im.img, t->fmt, 1, vk_remap(remap, rsx_format), 0, &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h; o->scale = t->scale;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    return hd;
}
/* ---- colour feedback snapshots ---------------------------------------------
 * A draw that samples a colour target it is also writing (in-place deferred
 * lighting: read the albedo, write the lit colour over it) gets a copy of
 * that target taken just before it. The engine asks for one per such draw
 * (rsx_draw_backend.color_snapshot) and releases the previous one; without
 * this hook it falls back to the guest-memory bytes at the target's address,
 * which hold whatever was last stored there -- for Twisted Metal the previous
 * frame's UI, lit and composited into the next frame.
 *
 * Every operation here is one synchronous submission, so the copy is already
 * in command order: after the draws before it, before the draw that samples
 * it. The copy is a COLOR object marked `snapshot` so the engine can take a
 * crossbar view of it (vk_eng_surface_view); it is never an attachment.
 * Released copies keep their images in a small pool, since a lit frame asks
 * for one per light. */
#define VK_SNAP_POOL 8
static struct { vk_image im; VkFormat fmt; u32 w, h; } s_snap_pool[VK_SNAP_POOL];
static u32 s_snap_pool_n;

static void vk_eng_snapshot_recycle(vk_eng_obj* o)
{
    if (s_snap_pool_n < VK_SNAP_POOL && o->im.img) {
        s_snap_pool[s_snap_pool_n].im = o->im;
        s_snap_pool[s_snap_pool_n].fmt = o->fmt;
        s_snap_pool[s_snap_pool_n].w = o->w;
        s_snap_pool[s_snap_pool_n].h = o->h;
        s_snap_pool_n++;
    } else {
        vk_destroy_image(&o->im);
    }
}

static void vk_eng_snapshot_pool_drain(void)
{
    for (u32 i = 0; i < s_snap_pool_n; i++) vk_destroy_image(&s_snap_pool[i].im);
    s_snap_pool_n = 0;
}

static u32 vk_eng_color_snapshot(void* user, u32 surface)
{
    (void)user;
    const vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t || t->snapshot) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    int reused = 0;
    for (u32 i = 0; i < s_snap_pool_n; i++)
        if (s_snap_pool[i].fmt == t->fmt && s_snap_pool[i].w == t->w && s_snap_pool[i].h == t->h) {
            o->im = s_snap_pool[i].im;
            s_snap_pool[i] = s_snap_pool[--s_snap_pool_n];
            reused = 1;
            break;
        }
    if (!reused && vk_create_image(&o->im, t->fmt, t->w, t->h,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im);
        memset(o, 0, sizeof *o);
        return 0;
    }
    o->kind = VK_ENG_COLOR; o->snapshot = 1;
    o->fmt = t->fmt; o->bpp = t->bpp; o->w = t->w; o->h = t->h; o->scale = t->scale;
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT,
                     reused ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy c = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { t->w, t->h, 1 },
    };
    pvkCmdCopyImage(s_vk.cmd, t->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    o->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    /* The copy, made visible to whatever samples it next. */
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    if (vk_end_op()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer -> an R32_SFLOAT
 * texture. Other depth formats would need a conversion pass: they report 0.
 *
 * Each depth target keeps ONE snapshot texture (its handle in `surface`),
 * refilled on every request, so nothing accumulates however often the engine
 * invalidates and asks again; it goes when the depth target goes. */
static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
        s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: depth snapshots need D32_SFLOAT, this device uses %d\n",
                                  (int)s_vk.depth_format); warned_fmt = 1; }
        return 0;
    }
    w *= vk_obj_scale(z); h *= vk_obj_scale(z);   /* guest size in, physical copy */
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;

    u32 snap = z->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (!t || t->w != w || t->h != h) {
        if (t) vk_eng_release(user, snap);
        z->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        t = &s_eobj[snap];
        if (vk_create_image(&t->im, VK_FORMAT_R32_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R32_SFLOAT; t->bpp = 4; t->w = w; t->h = h;
        t->scale = z->scale;
        z = vk_eng_get(depth, VK_ENG_DEPTH);          /* same slot; reload for clarity */
        z->surface = snap;
        if (vk_begin()) return 0;
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        if (vk_end_op()) return 0;
    }

    /* depth -> batch memory -> texture, all on the GPU. */
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4u;
    if (vk_batch_reserve(bytes, 0)) return 0;
    const VkDeviceSize off = vk_batch_take(NULL, bytes, 16u);
    vk_buffer st = s_batch.buf;
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy out = { .bufferOffset = off,
                                  .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = off, .size = bytes };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .bufferOffset = off,
                                 .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        /* The copy, made visible to whatever samples it next. */
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        rc = vk_end_op();
    }
    return rc ? 0 : snap;
}
/* The depth aspect of a D32_SFLOAT target copies out as 32-bit floats, which
 * is what the engine asks for; other depth formats report failure. */
static int vk_eng_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                 float* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    const u32 sc = vk_obj_scale(z);
    if (!z || !out || !w || !h || x + w > z->w / sc || y + h > z->h / sc ||
        out_pitch < w * 4u || (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
                                  s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT))
        return -1;
    const u32 gw = w, gh = h;
    x *= sc; y *= sc; w *= sc; h *= sc;
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)w * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        rc = vk_submit_and_wait();
        if (!rc) {
            vk_invalidate(&st);
            vk_scale_rows_down((const u8*)st.ptr, gw, gh, 4u, sc, out, out_pitch);
        }
    }
    vk_destroy_buffer(&st);
    return rc ? -1 : 0;
}
/* ---- E2: pipelines and draws ---------------------------------------------
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), colour targets
 * other than RGBA8 and RGBA16F, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
/* As many as the engine keeps (ENG_MAX_PIPELINES in rsx_draw_engine.c). God of
 * War: Ascension selects ~740 distinct fragment programs in its first level on
 * top of the menu's; with 512 slots every later pipeline failed silently and the
 * level drew nothing but its clears. */
#define VK_ENG_MAX_PIPE   8192
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   64

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    VkFormat            rt_fmt;          /* picks the render pass it is built against */
    u32                 cube_mask;       /* TextureCube units: the split set layout */
    VkPipeline          variant[VK_ENG_TOPOLOGIES];
} vk_eng_pipe;

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

static struct {
    VkRenderPass rp[VK_ENG_RT_FORMATS][RSX_BE_MAX_COLOR_TARGETS + 1]; /* by format, colour count */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h;
    u32          pipeline;
    u32          vs_bytes, ps_bytes;
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    u32          vtex[RSX_MAX_VERTEX_TEXTURES], vtex_mask;
    rsx_be_sampler_desc vsamp[RSX_MAX_VERTEX_TEXTURES];
    float        vp[4];
    u32          sc[4];
    int          have_vp, have_sc;
    u32          stencil_ref;
    float        depth_bounds[2];     /* min, max for the next draw */
    u32          warned;
    /* The split set layout (vk_eng_split_layout) and its one set, made the
     * first time a pipeline samples a cube map; the 1x1 cube a cube unit
     * with nothing bound samples. */
    VkDescriptorSetLayout split_layout;
    VkDescriptorPool      split_pool;
    VkDescriptorSet       split_set;
    VkPipelineLayout      split_pipe_layout;
    u32                   dummy_cube;
} s_e2;

#define VK_ENG_SAMP_CACHE 64
static struct { u64 key; VkSampler s; } s_esamp[VK_ENG_SAMP_CACHE];
static u32 s_esamp_count;


static void vk_eng_drop_framebuffers(void)
{
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (s_e2.fb[i].fb && !vk_grave_push(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                            VK_NULL_HANDLE, 0, s_e2.fb[i].fb))
            pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
    s_e2.fb_count = 0;
}

/* The render pass for `n` colour attachments of format `cf` plus depth,
 * LOAD/STORE in GENERAL like the vtable path's: clears are separate
 * operations. An MRT set shares target A's format (engine contract). */
static VkRenderPass vk_eng_render_pass(VkFormat cf, u32 n)
{
    const int fi = vk_eng_rt_index(cf);
    if (fi < 0 || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_e2.rp[fi][n]) return s_e2.rp[fi][n];
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? cf : s_vk.depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            /* Stencil lives across draws like depth does (a title writes it in
             * one pass and tests it in the next); ignored for colour and for
             * depth formats without stencil. */
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_e2.rp[fi][n]) != VK_SUCCESS)
        s_e2.rp[fi][n] = VK_NULL_HANDLE;
    return s_e2.rp[fi][n];
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;                  /* FUNC_ADD 0x8006 */
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

static int vk_eng_translate(const char* hlsl, int stage, VkShaderModule* out)
{
    u32 words = 0;
    char* vertex_source = NULL;
    if (stage == RSX_SHADER_STAGE_VERTEX) {
        size_t cap = strlen(hlsl) + 64;
        vertex_source = (char*)malloc(cap);
        if (!vertex_source) return -1;
        const char* in = hlsl; char* dst = vertex_source;
        while (*in) {
            int unit = -1;
            for (int u = 0; u < RSX_MAX_VERTEX_TEXTURES; ++u) {
                char token[32]; snprintf(token, sizeof token, "register(s%d)", u);
                if (!strncmp(in, token, strlen(token))) { unit = u; break; }
            }
            if (unit >= 0) { dst += sprintf(dst, "register(s%d)", 16 + unit); in += 12; }
            else *dst++ = *in++;
        }
        *dst = 0; hlsl = vertex_source;
    }
    int translated = rsx_hlsl_to_spirv(hlsl, stage, s_g.spv, VK_SPV_WORDS, &words, s_g.log, sizeof s_g.log);
    free(vertex_source);
    if (translated || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", s_g.log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

/* A pixel program with a cube unit declares all sixteen units one by one
 * (TextureCube or Texture2D rsx_texN : register(tN)) instead of the
 * Texture2D array, and register(tN) is binding RSX_SPIRV_TEXTURE_BINDING + N
 * (rsx_shader_spirv.h): sixteen single bindings, where the shared layout has
 * one binding of sixteen. Those pipelines use this layout, made on first use
 * so a title without cube maps never builds it. */
static VkPipelineLayout vk_eng_split_layout(void)
{
    if (s_e2.split_pipe_layout) return s_e2.split_pipe_layout;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[3 + RSX_MAX_TEXTURES + 2 * RSX_MAX_VERTEX_TEXTURES];
    b[0] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[1] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[2] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,
                                           RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        b[3 + u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING + u,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; ++u) {
        b[3 + RSX_MAX_TEXTURES + 2*u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING+16+u,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
        b[4 + RSX_MAX_TEXTURES + 2*u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING+16+u,
            VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    }
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3 + RSX_MAX_TEXTURES + 2 * RSX_MAX_VERTEX_TEXTURES, .pBindings = b };
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES + RSX_MAX_VERTEX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES + RSX_MAX_VERTEX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_e2.split_layout) != VK_SUCCESS ||
        pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_e2.split_pool) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_e2.split_pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_e2.split_layout };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_e2.split_layout };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &s_e2.split_set) != VK_SUCCESS ||
        pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_e2.split_pipe_layout) != VK_SUCCESS) {
        s_e2.split_pipe_layout = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    return s_e2.split_pipe_layout;
}

/* Which units a pixel program declares as TextureCube. */
static u32 vk_eng_cube_units(const char* ps_hlsl)
{
    u32 mask = 0;
    char decl[40];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "TextureCube rsx_tex%u ", u);
        if (strstr(ps_hlsl, decl)) mask |= 1u << u;
    }
    return mask;
}

/* A fragment program reading its screen position (WPOS, SV_POSITION in the
 * decompiled HLSL) gets guest coordinates back at an internal scale: pixel
 * centres map to the guest pixel they fall in, (p - 0.5) / N + 0.5, so
 * post-process passes that turn the position into texture coordinates keep
 * working. Returns a new string, or NULL when there is nothing to change. */
static char* vk_eng_scale_wpos(const char* ps)
{
    const u32 sc = s_vk.res_scale;
    const char* main_at = strstr(ps, "main(PSInput input)");
    if (sc <= 1 || !main_at || !strstr(main_at, "input.position")) return NULL;
    const char* body = strchr(main_at, '{');
    if (!body) return NULL;
    body++;
    char decl[160];
    snprintf(decl, sizeof decl,
             "\n    float4 _rsx_wpos = float4((input.position.xy - 0.5) / %u.0 + 0.5, input.position.zw);",
             (unsigned)sc);
    size_t uses = 0;
    for (const char* q = body; (q = strstr(q, "input.position")); q += 14) uses++;
    const size_t len = strlen(ps) + strlen(decl) + uses * 2 + 1;   /* "_rsx_wpos" is 2 longer */
    char* out = (char*)malloc(len);
    if (!out) return NULL;
    size_t o = (size_t)(body - ps);
    memcpy(out, ps, o);
    memcpy(out + o, decl, strlen(decl)); o += strlen(decl);
    for (const char* q = body; *q;) {
        if (!strncmp(q, "input.position", 14)) { memcpy(out + o, "_rsx_wpos", 9); o += 9; q += 14; }
        else out[o++] = *q++;
    }
    out[o] = '\0';
    return out;
}

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    u32 rt_bpp;
    const VkFormat cf = vk_eng_format(rt_fmt, &rt_bpp);
    if (vk_eng_rt_index(cf) < 0) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    u32 slot = VK_ENG_MAX_PIPE;
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot == VK_ENG_MAX_PIPE) {
        if (!(s_e2.warned & 128u)) {
            VK_LOG("engine: pipeline table full (%u), draws will drop\n", (unsigned)VK_ENG_MAX_PIPE);
            s_e2.warned |= 128u;
        }
        return 0;
    }
    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    if (vk_eng_translate(vs_hlsl, RSX_SHADER_STAGE_VERTEX, &P->vs)) return 0;
    char* scaled_ps = vk_eng_scale_wpos(ps_hlsl);
    const int ps_failed = vk_eng_translate(scaled_ps ? scaled_ps : ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, &P->fs);
    free(scaled_ps);
    if (ps_failed) {
        pvkDestroyShaderModule(s_vk.device, P->vs, NULL); memset(P, 0, sizeof *P); return 0;
    }
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->rt_fmt = cf;
    P->cube_mask = vk_eng_cube_units(ps_hlsl);
    P->used = 1;
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    vk_submit_and_wait();
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        if (P->variant[t]) pvkDestroyPipeline(s_vk.device, P->variant[t], NULL);
    if (P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    memset(P, 0, sizeof *P);
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx)
{
    static const VkPrimitiveTopology topo[VK_ENG_TOPOLOGIES] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    if (P->variant[topo_idx]) return P->variant[topo_idx];
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, P->rt_count);
    if (!rp || P->nslots > VK_GUEST_ATTRS) return VK_NULL_HANDLE;
    const VkPipelineLayout layout = P->cube_mask ? vk_eng_split_layout() : s_g.pipe_layout;
    if (!layout) return VK_NULL_HANDLE;
    const rsx_be_render_state* rs = &P->rs;

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = P->fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, P->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < P->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = P->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo[topo_idx] };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* CULL_FACE FRONT 0x0404 / BACK 0x0405 / FRONT_AND_BACK 0x0408 (front, as
     * in D3D12 and Metal); FRONT_FACE CCW 0x0901 maps directly: both APIs
     * judge winding on screen, and the Y flip in the vertex stage makes this
     * screen the same one D3D's viewport produces. */
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = !rs->cull_enable ? VK_CULL_MODE_NONE
                  : (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT
                                                                           : VK_CULL_MODE_BACK_BIT,
        .frontFace = (rs->front_face == 0x0901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                 rs->s_func_mask, rs->s_write_mask);
    VkStencilOpState back = rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->s_func_mask, rs->s_write_mask)
        : front;
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (rs->depth_test && rs->depth_write) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = vk_gcm_compare(rs->depth_func),
        .stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE,
        .front = front, .back = back,
        /* The bounds themselves are dynamic (vk_eng_set_depth_bounds). */
        .depthBoundsTestEnable = (rs->depth_bounds_test && s_vk.depth_bounds_ok) ? VK_TRUE : VK_FALSE,
        .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f };
    if (rs->depth_bounds_test && !s_vk.depth_bounds_ok && !(s_e2.warned & 64u)) {
        VK_LOG("engine: the device has no depthBounds; the depth bounds test is ignored\n");
        s_e2.warned |= 64u;
    }
    /* nv40 COLOR_MASK: B [0:7], G [8:15], R [16:23], A [24:31], any bit set.
     * Every attachment takes target A's blend and mask (see rsx_draw_engine.h). */
    VkColorComponentFlags wm = 0;
    if ((rs->color_mask >> 16) & 0xFFu) wm |= VK_COLOR_COMPONENT_R_BIT;
    if ((rs->color_mask >>  8) & 0xFFu) wm |= VK_COLOR_COMPONENT_G_BIT;
    if ((rs->color_mask >>  0) & 0xFFu) wm |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask >> 24) & 0xFFu) wm |= VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 r = 0; r < P->rt_count; r++)
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = rs->blend_enable ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = vk_gcm_blend_factor(rs->sf_rgb),
            .dstColorBlendFactor = vk_gcm_blend_factor(rs->df_rgb),
            .colorBlendOp = vk_gcm_blend_op(rs->eq_rgb),
            .srcAlphaBlendFactor = vk_gcm_blend_factor(rs->sf_a),
            .dstAlphaBlendFactor = vk_gcm_blend_factor(rs->df_a),
            .alphaBlendOp = vk_gcm_blend_op(rs->eq_a),
            .colorWriteMask = wm };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = P->rt_count, .pAttachments = cba };
    VkDynamicState dyn[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BOUNDS };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = s_vk.depth_bounds_ok ? 4u : 3u, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = layout, .renderPass = rp, .subpass = 0 };
    if (pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL,
                                   &P->variant[topo_idx]) != VK_SUCCESS) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed\n");
        P->variant[topo_idx] = VK_NULL_HANDLE;
    }
    return P->variant[topo_idx];
}

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e2.pipeline = pipeline; }

static void vk_eng2_shutdown(void* user)
{
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_eng_drop_framebuffers();
    for (u32 f = 0; f < VK_ENG_RT_FORMATS; f++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            if (s_e2.rp[f][n]) pvkDestroyRenderPass(s_vk.device, s_e2.rp[f][n], NULL);
    vk_destroy_image(&s_e2.scratch_depth);
    if (s_e2.split_pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_e2.split_pipe_layout, NULL);
    if (s_e2.split_pool)        pvkDestroyDescriptorPool(s_vk.device, s_e2.split_pool, NULL);
    if (s_e2.split_layout)      pvkDestroyDescriptorSetLayout(s_vk.device, s_e2.split_layout, NULL);
    memset(&s_e2, 0, sizeof s_e2);
    vk_submit_and_wait();
    for (u32 k = 0; k < 2; k++) {
        vk_destroy_buffer(&s_batches[k].buf);
        if (s_batches[k].pool) pvkDestroyDescriptorPool(s_vk.device, s_batches[k].pool, NULL);
    }
    memset(s_batches, 0, sizeof s_batches);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
}

/* Constants are kept host-side until a draw copies them into its batch
 * (vk_eng_draw): draws already recorded keep the copy they were given. */
static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.vs_bytes = 0; return; }
    u32 cap = (u32)s_g.ps_off;
    if (cap > sizeof s_vs_shadow) cap = sizeof s_vs_shadow;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    if (s_e2.vs_bytes == bytes && !memcmp(s_vs_shadow, data, bytes)) return;
    memcpy(s_vs_shadow, data, bytes);
    s_e2.vs_bytes = bytes;
    s_batch.vs_gen = 0;
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.ps_bytes = 0; return; }
    u32 cap = (u32)(s_g.ubo.size - s_g.ps_off);
    if (cap > sizeof s_ps_shadow) cap = sizeof s_ps_shadow;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    if (s_e2.ps_bytes == bytes && !memcmp(s_ps_shadow, data, bytes)) return;
    memcpy(s_ps_shadow, data, bytes);
    s_e2.ps_bytes = bytes;
    s_batch.ps_gen = 0;
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e2.tex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e2.samp[u] = samplers[u];
    }
    s_e2.tex_mask = mask;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    s_e2.vtex_mask = mask;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; ++u) {
        s_e2.vtex[u] = ((mask >> u) & 1u) ? textures[u] : 0;
        if ((mask >> u) & 1u) s_e2.vsamp[u] = samplers[u];
    }
    if (mask && !(s_e2.warned & 8u)) { VK_LOG("engine: vertex textures enabled, independent descriptors\n"); s_e2.warned |= 8u; }
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_e2.vp[0] = x; s_e2.vp[1] = y; s_e2.vp[2] = w; s_e2.vp[3] = h; s_e2.have_vp = 1;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_e2.sc[0] = x; s_e2.sc[1] = y; s_e2.sc[2] = w; s_e2.sc[3] = h; s_e2.have_sc = 1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { (void)user; s_e2.stencil_ref = ref; }

/* Vulkan wants the bounds inside [0, 1] without VK_EXT_depth_range_unrestricted;
 * a NaN clamps to the open end so a garbage register cannot reject a draw. */
static float vk_depth_bound(float v, float open)
{
    if (!(v == v)) return open;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void vk_eng_set_depth_bounds(void* user, float min, float max)
{
    (void)user;
    s_e2.depth_bounds[0] = vk_depth_bound(min, 0.0f);
    s_e2.depth_bounds[1] = vk_depth_bound(max, 1.0f);
}

static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (!memcmp(s_e2.fb[i].key, key, sizeof key)) return s_e2.fb[i].fb;
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffers();
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_e2.fb[s_e2.fb_count].key, key, sizeof key);
    s_e2.fb[s_e2.fb_count++].fb = fb;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers();
    vk_destroy_image(&s_e2.scratch_depth);
    if (vk_create_image(&s_e2.scratch_depth, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        s_vk.depth_aspect) || vk_begin())
        return VK_NULL_HANDLE;
    vk_barrier_image(s_e2.scratch_depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_e2.scratch_depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) return VK_NULL_HANDLE;
    s_e2.scratch_w = w; s_e2.scratch_h = h;
    return s_e2.scratch_depth.view;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8 in the low nibble, which vk_gcm_wrap already
 * translates, and the high nibble carries the border colour class from
 * rsx_engine_bridge.c's eng_border_class. */
static const VkBorderColor vk_eng_border[3] = {
    VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
    VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE };
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d)
{
    u32 lo, hi;
    memcpy(&lo, &d->min_lod, 4); memcpy(&hi, &d->max_lod, 4);
    const u64 key = (u64)d->min_linear | ((u64)d->mag_linear << 1) | ((u64)d->mip_linear << 2)
                  | ((u64)d->mip_present << 3) | ((u64)(d->wrap_s & 0xF) << 4)
                  | ((u64)(d->wrap_t & 0xF) << 8) | ((u64)(d->wrap_r & 0xF) << 12)
                  | ((u64)(lo >> 16) << 16) | ((u64)(hi >> 16) << 32);
    const u32 border = (d->wrap_s >> 4) < 3 ? (u32)(d->wrap_s >> 4) : 0u;
    const u64 full_key = key | ((u64)border << 48);
    for (u32 i = 0; i < s_esamp_count; i++) if (s_esamp[i].key == full_key) return s_esamp[i].s;
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) return s_vk.sampler;
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = d->mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(d->wrap_s), .addressModeV = vk_gcm_wrap(d->wrap_t),
        .addressModeW = vk_gcm_wrap(d->wrap_r),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = vk_eng_border[border] };
    VkSampler smp;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return s_vk.sampler;
    s_esamp[s_esamp_count].key = full_key;
    s_esamp[s_esamp_count++].s = smp;
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view.
 * A unit gets a view of its own kind only -- a cube for a TextureCube unit, a
 * 2D one otherwise -- and NULL for anything else, which then samples the
 * dummy of the unit's kind. */
static VkImageView vk_eng_sample_view(u32 handle, int want_cube)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind != VK_ENG_TEXTURE && o->kind != VK_ENG_COLOR && o->kind != VK_ENG_VIEW)
        return VK_NULL_HANDLE;
    return ((int)o->cube == want_cube) ? o->im.view : VK_NULL_HANDLE;
}

/* What a cube unit with nothing bound samples: a 1x1 cube, magenta on every
 * face like the 2D dummy, made on first use. */
static VkImageView vk_eng_dummy_cube_view(void)
{
    if (!vk_eng_get(s_e2.dummy_cube, VK_ENG_TEXTURE)) {
        static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
        s_e2.dummy_cube = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 6,
                                                0xAAE4u, 0x85u);
        if (!s_e2.dummy_cube) return VK_NULL_HANDLE;
        for (u32 f = 0; f < 6; f++)
            vk_eng_texture_upload(NULL, s_e2.dummy_cube, f, 0, 1, 1, magenta, 4, 1);
    }
    return s_eobj[s_e2.dummy_cube].im.view;
}

/* Fill the set the bound pipeline uses: the shared one (sixteen textures in
 * one binding), or for a pipeline with cube units the split one (one binding
 * per unit). Returns the set, or VK_NULL_HANDLE. */
static VkDescriptorSet vk_eng_write_descriptors(const vk_eng_pipe* P)
{
    const int split = P->cube_mask != 0;
    if (split ? !s_e2.split_set : !s_g.set) return VK_NULL_HANDLE;   /* layouts not built */
    const VkImageView dummy_cube = split ? vk_eng_dummy_cube_view() : VK_NULL_HANDLE;
    if (split && !dummy_cube) return VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[2] = {
        { s_batch.buf.buf, s_batch.vs_off, s_e2.vs_bytes ? s_e2.vs_bytes : 16u },
        { s_batch.buf.buf, s_batch.ps_off, s_e2.ps_bytes ? s_e2.ps_bytes : 16u },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const int cube = (P->cube_mask >> u) & 1u;
        VkImageView v = ((s_e2.tex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.tex[u], cube)
                                                     : VK_NULL_HANDLE;
        VkSampler smp = v ? vk_eng_sampler(&s_e2.samp[u]) : s_vk.sampler;
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         v ? v : (cube ? dummy_cube : s_vk.dummy.view),
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4 + RSX_MAX_TEXTURES + 2 * RSX_MAX_VERTEX_TEXTURES];
    u32 n = 0;
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si };
    if (split) {
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
            w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = set, .dstBinding = RSX_SPIRV_TEXTURE_BINDING + u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[u] };
    } else {
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii };
    }
    VkDescriptorImageInfo vi[RSX_MAX_VERTEX_TEXTURES], vs[RSX_MAX_VERTEX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; ++u) {
        VkImageView view = ((s_e2.vtex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.vtex[u], 0) : VK_NULL_HANDLE;
        VkSampler sampler = view ? vk_eng_sampler(&s_e2.vsamp[u]) : s_vk.sampler;
        vi[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, view ? view : s_vk.dummy.view, VK_IMAGE_LAYOUT_GENERAL };
        vs[u] = (VkDescriptorImageInfo){ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        w[n++] = (VkWriteDescriptorSet){ .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=set,
            .dstBinding=RSX_SPIRV_TEXTURE_BINDING+16+u, .descriptorCount=1,
            .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo=&vi[u] };
        w[n++] = (VkWriteDescriptorSet){ .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=set,
            .dstBinding=RSX_SPIRV_SAMPLER_BINDING+16+u, .descriptorCount=1,
            .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo=&vs[u] };
    }
    struct descriptor_key {
        VkDescriptorBufferInfo buffers[2];
        VkDescriptorImageInfo images[RSX_MAX_TEXTURES], samplers[RSX_MAX_TEXTURES];
        VkDescriptorImageInfo vertex_images[RSX_MAX_VERTEX_TEXTURES], vertex_samplers[RSX_MAX_VERTEX_TEXTURES];
        u32 cube_mask;
    } key = {0};
    memcpy(key.buffers, bi, sizeof bi);
    memcpy(key.images, ii, sizeof ii); memcpy(key.samplers, si, sizeof si);
    memcpy(key.vertex_images, vi, sizeof vi); memcpy(key.vertex_samplers, vs, sizeof vs);
    key.cube_mask = P->cube_mask;
    static struct descriptor_key last;
    static u32 last_gen;
    static VkDescriptorSet last_set;
    if (last_set && last_gen == s_batch.gen && !memcmp(&key, &last, sizeof key)) return last_set;
    set = vk_batch_set(split ? s_e2.split_layout : s_g.set_layout);
    if (!set) return VK_NULL_HANDLE;
    for (u32 i = 0; i < n; i++) w[i].dstSet = set;
    pvkUpdateDescriptorSets(s_vk.device, n, w, 0, NULL);
    last = key; last_gen = s_batch.gen; last_set = set;
    return set;
}

static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    (void)user;
    if (!s_e2.pipeline || !vertices || !vertex_count || !stride) return;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return;

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        return;
    }
    vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt0) return;
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR)) return;
    if (rt0->fmt != P->rt_fmt) {
        if (!(s_e2.warned & 32u)) { VK_LOG("engine: draw into colour format %d with a pipeline built for %d skipped\n", (int)rt0->fmt, (int)P->rt_fmt); s_e2.warned |= 32u; }
        return;
    }
    const u32 w = rt0->w, h = rt0->h;
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    VkImageView dview = (zo && zo->w >= w && zo->h >= h) ? zo->im.view : vk_eng_scratch_depth(w, h);
    if (!dview) return;
    const u32 dkey = (zo && dview == zo->im.view) ? s_eng_bound.depth : 0xFFFFFFFFu;

    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u);
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, n);
    VkFramebuffer fb = vk_eng_framebuffer(rp, s_eng_bound.rt, n, dview, dkey, w, h);
    if (!pipe || !rp || !fb) return;

    const VkDeviceSize vbytes = (VkDeviceSize)vertex_count * stride;
    const VkDeviceSize ibytes = (indices && index_count) ? (VkDeviceSize)index_count * 4u : 0;
    /* The scratch depth, framebuffer and dummy cube may submit the batch, so
     * they come before this draw takes anything from it. */
    if (P->cube_mask && !vk_eng_dummy_cube_view()) return;
    if (vk_batch_reserve(vbytes + ibytes + 16u + s_e2.vs_bytes + s_e2.ps_bytes, 1)) return;
    const VkDeviceSize voff = vk_batch_take(vertices, vbytes, 16u);
    const VkDeviceSize ioff = ibytes ? vk_batch_take(indices, ibytes, 4u) : 0;
    if (s_batch.vs_gen != s_batch.gen) {
        s_batch.vs_off = vk_batch_take(s_vs_shadow, s_e2.vs_bytes ? s_e2.vs_bytes : 16u, VK_BATCH_UBO_ALIGN);
        s_batch.vs_gen = s_batch.gen;
    }
    if (s_batch.ps_gen != s_batch.gen) {
        s_batch.ps_off = vk_batch_take(s_ps_shadow, s_e2.ps_bytes ? s_e2.ps_bytes : 16u, VK_BATCH_UBO_ALIGN);
        s_batch.ps_gen = s_batch.gen;
    }
    const VkDescriptorSet set = vk_eng_write_descriptors(P);
    if (!set) return;

    s_draw_begin = 1;
    const int begin_rc = vk_begin();
    s_draw_begin = 0;
    if (begin_rc) return;
    const int reuse_pass = s_open_pass == rp && s_open_fb == fb;
    if (!reuse_pass) vk_close_engine_pass();
    /* A new pass orders its attachments after earlier operations. Draws
     * inside one pass retain Vulkan's rasterization order for blending and
     * depth/stencil, without ending the pass and inserting a full barrier. */
    if (!reuse_pass) for (u32 i = 0; i < n; i++)
        vk_barrier_image(s_eobj[s_eng_bound.rt[i]].im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = fb,
                                  .renderArea = { { 0, 0 }, { w, h } } };
    if (!reuse_pass) {
        pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        s_open_pass = rp; s_open_fb = fb;
    }
    vk_bind_pipeline(pipe);
    /* The guest's viewport and scissor, at the targets' internal scale. */
    const u32 rs = vk_obj_scale(n ? &s_eobj[s_eng_bound.rt[0]] : vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH));
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0] * (float)rs; vp.y = s_e2.vp[1] * (float)rs;
        vp.width = s_e2.vp[2] * (float)rs; vp.height = s_e2.vp[3] * (float)rs;
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        const u64 sx = (u64)s_e2.sc[0] * rs, sy = (u64)s_e2.sc[1] * rs;
        const u64 sx1 = sx + (u64)s_e2.sc[2] * rs, sy1 = sy + (u64)s_e2.sc[3] * rs;
        u32 x0 = sx < w ? (u32)sx : w, y0 = sy < h ? (u32)sy : h;
        u32 x1 = sx1 < w ? (u32)sx1 : w, y1 = sy1 < h ? (u32)sy1 : h;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdSetStencilReference(s_vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s_e2.stencil_ref & 0xFFu);
    if (s_vk.depth_bounds_ok)
        pvkCmdSetDepthBounds(s_vk.cmd, s_e2.depth_bounds[0], s_e2.depth_bounds[1]);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             P->cube_mask ? s_e2.split_pipe_layout : s_g.pipe_layout,
                             0, 1, &set, 0, NULL);
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_batch.buf.buf, &voff);
    if (ibytes) {
        pvkCmdBindIndexBuffer(s_vk.cmd, s_batch.buf.buf, ioff, VK_INDEX_TYPE_UINT32);
        pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
    }
    if (!vk_merge_passes()) vk_close_engine_pass();
    vk_end_op();
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = vk_eng_init,
    .shutdown             = vk_eng_shutdown,
    .submit_and_wait      = vk_eng_submit_and_wait,
    .texture_create       = vk_eng_texture_create,
    .texture_upload       = vk_eng_texture_upload,
    .texture_release      = vk_eng_release,
    .color_target_create  = vk_eng_color_target_create,
    .color_target_release = vk_eng_release,
    .surface_view         = vk_eng_surface_view,
    .depth_target_create  = vk_eng_depth_target_create,
    .depth_target_release = vk_eng_release,
    .depth_snapshot       = vk_eng_depth_snapshot,
    .color_snapshot       = vk_eng_color_snapshot,
    .pipeline_create      = vk_eng_pipeline_create,
    .pipeline_release     = vk_eng_pipeline_release,
    .bind_targets         = vk_eng_bind_targets,
    .bind_pipeline        = vk_eng_bind_pipeline,
    .bind_vs_constants    = vk_eng_bind_vs_constants,
    .bind_ps_constants    = vk_eng_bind_ps_constants,
    .bind_textures        = vk_eng_bind_textures,
    .bind_vertex_textures = vk_eng_bind_vertex_textures,
    .set_viewport         = vk_eng_set_viewport,
    .set_scissor          = vk_eng_set_scissor,
    .set_stencil_ref      = vk_eng_set_stencil_ref,
    .set_depth_bounds     = vk_eng_set_depth_bounds,
    .draw                 = vk_eng_draw,
    .clear_color          = vk_eng_clear_color,
    .clear_depth_stencil  = vk_eng_clear_depth_stencil,
    .present              = vk_eng_present,
    .readback             = vk_eng_readback,
    .depth_readback       = vk_eng_depth_readback,
    .color_target_write   = vk_eng_color_target_write,
};

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = vk_cb_track_state,
    .set_vertex_attribs = vk_cb_track_state,
    .set_depth_stencil  = vk_cb_track_state,
    .clear              = vk_cb_clear,
    .draw_arrays        = vk_cb_draw_arrays,
    .draw_indexed       = vk_cb_draw_indexed,
    .bind_texture       = vk_cb_bind_texture,
    .present            = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height, const char* title)
{
    pthread_mutex_init(&s_vk.queue_mu, NULL);
    pthread_mutex_init(&s_vk.present_mu, NULL);
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");
    /* Writing a full frame to disk on every present costs more than the
     * present itself and slows a title enough to change what it does; a
     * live title wants one now and then, not all of them. */
    { const char* e = getenv("PS3RECOMP_VK_DUMP_EVERY");
      s_vk.dump_every = (e && atoi(e) > 0) ? (u32)atoi(e) : 1; }
    { const char* e = getenv("PORT_RENDER_SCALE");     /* see vk_target_scale */
      const int n = e ? atoi(e) : 1;
      s_vk.res_scale = n >= 2 && n <= 4 ? (u32)n : 1u;
      if (s_vk.res_scale > 1) VK_LOG("internal resolution %ux (%ux%u)\n", s_vk.res_scale,
                                     s_vk.width * s_vk.res_scale, s_vk.height * s_vk.res_scale); }

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    if (vk_load_library()) return -1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else if (!SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface)) {
            char why[256];
            snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            vk_window_drop(why);
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    static const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    /* The depth bounds test: a deferred renderer bounds its light pass to
     * the geometry and its sky to the far plane with it. Optional in
     * Vulkan; without it the test stays off, as before. */
    want.depthBounds = have.depthBounds;
    s_vk.depth_bounds_ok = have.depthBounds ? 1 : 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = s_vk.windowed ? 1u : 0u,
                               .ppEnabledExtensionNames = s_vk.windowed ? dev_exts : NULL,
                               .pEnabledFeatures = &want };
    VK_CHECK(pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    if (vk_load_device_functions()) return -1;
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 2 };
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, s_vk.slot_cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.slot_fence[0]), "vkCreateFence");
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.slot_fence[1]), "vkCreateFence");
    s_vk.slot = 0;
    s_vk.cmd = s_vk.slot_cmd[0];
    s_vk.fence = s_vk.slot_fence[0];
    /* The present's own (see vk_state). */
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.present_pool), "vkCreateCommandPool");
    cai.commandPool = s_vk.present_pool;
    cai.commandBufferCount = 1;
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.present_cmd), "vkAllocateCommandBuffers");
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.present_fence), "vkCreateFence");
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.window_fence), "vkCreateFence");

    if (vk_pick_depth_format()) return -1;
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,   /* FSR input */
                        VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, s_vk.depth_aspect)) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.width * s_vk.height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed, fixed-function path only\n");
        vk_guest_shutdown();
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;

    if (s_vk.windowed) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sem_acquire) != VK_SUCCESS ||
            vk_create_swapchain())
            vk_window_drop("the swapchain could not be created");
    }
    return 0;
}

void port_thread_affinity_start(void);

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    port_thread_affinity_start();   /* thread_affinity.c */
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    rsx_draw_engine_set_backend(&s_vk_engine_backend);
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.width, s_vk.height) == 0)
        s_vk.eng_active = 1;
    else
        rsx_set_backend(&s_vulkan_backend);
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    VK_LOG("offscreen target %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, (int)s_vk.depth_format, title ? title : "");
    return 0;
}

void rsx_vulkan_backend_shutdown(void)
{
    rsx_vulkan_backend_present_finish();   /* a host that began one and quit */
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const Uint32 end = SDL_GetTicks() + (Uint32)(s_vk.hold_seconds * 1000.0);
        while (s_vk.windowed && !SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            vk_present_window();         /* FIFO: paced by the display */
        }
    }
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        vk_device_idle();
        vk_guest_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_fsr_destroy();
        vk_destroy_image(&s_vk.display);
        vk_destroy_image(&s_vk.depth);
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        for (u32 k = 0; k < 2; k++)
            if (s_vk.slot_fence[k]) pvkDestroyFence(s_vk.device, s_vk.slot_fence[k], NULL);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        if (s_vk.present_fence) pvkDestroyFence(s_vk.device, s_vk.present_fence, NULL);
        if (s_vk.window_fence)  pvkDestroyFence(s_vk.device, s_vk.window_fence, NULL);
        if (s_vk.present_pool)  pvkDestroyCommandPool(s_vk.device, s_vk.present_pool, NULL);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
    if (s_vk.lib) dlclose(s_vk.lib);
    free(s_vk.cpu_verts);
    pthread_mutex_destroy(&s_vk.queue_mu);
    pthread_mutex_destroy(&s_vk.present_mu);
    memset(&s_vk, 0, sizeof s_vk);
}

/* Keyboard and mouse as pad port 0 (input_bridge.c). The mouse drives the
 * camera while captured in relative mode: a click in the window or entering
 * fullscreen captures it, Alt or losing focus releases it. */
extern int port_mouse_captured, port_mouse_buttons;
void port_input_mouse_motion(int dx, int dy);
int port_input_keyboard_enabled(void);

static void vk_mouse_capture(int on)
{
    if (!s_vk.window || !port_input_keyboard_enabled() || on == port_mouse_captured) return;
    if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) != 0) return;
    __atomic_store_n(&port_mouse_buttons, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&port_mouse_captured, on, __ATOMIC_RELEASE);
}

int rsx_vulkan_backend_pump_messages(void)
{
    if (!s_vk.windowed) return 0;
    int quit = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        /* Esc is the pad's START with the keyboard on; it quits only without. */
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !port_input_keyboard_enabled()) quit = 1;
        else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_LALT || e.key.keysym.sym == SDLK_RALT))
            vk_mouse_capture(0);
        else if (e.type == SDL_MOUSEBUTTONDOWN && !port_mouse_captured)
            vk_mouse_capture(1);                 /* the capturing click is not a press */
        else if (e.type == SDL_MOUSEBUTTONDOWN)
            __atomic_or_fetch(&port_mouse_buttons, (int)SDL_BUTTON(e.button.button), __ATOMIC_RELAXED);
        else if (e.type == SDL_MOUSEBUTTONUP)
            __atomic_and_fetch(&port_mouse_buttons, ~(int)SDL_BUTTON(e.button.button), __ATOMIC_RELAXED);
        else if (e.type == SDL_MOUSEMOTION && port_mouse_captured)
            port_input_mouse_motion(e.motion.xrel, e.motion.yrel);
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            vk_mouse_capture(0);
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED &&
                 s_vk.display_mode != VK_DISPLAY_WINDOWED)
            vk_mouse_capture(1);
        else if (e.type == SDL_KEYDOWN && !e.key.repeat && s_vk.window &&
                 (e.key.keysym.sym == SDLK_F11 ||
                  (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))))
            vk_display_apply(s_vk.display_mode == VK_DISPLAY_WINDOWED ? s_vk.fullscreen_mode
                                                                      : VK_DISPLAY_WINDOWED);
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            if (s_vk.display_mode == VK_DISPLAY_WINDOWED) {
                s_vk.win_w = e.window.data1; s_vk.win_h = e.window.data2;
            }
            __atomic_store_n(&s_vk.sc_dirty, 1, __ATOMIC_RELEASE);
        }
    }
    /* Closing the window ends the game at once. Returning -1 only stopped this
     * thread's drain: the window stayed up unanswered ("not responding") and
     * the guest threads hung on fences. _exit, not exit: the guest threads are
     * still running, and static destructors or a Vulkan teardown under them
     * can hang or crash. The logs are flushed first. */
    if (quit) {
        printf("[RSX vulkan] window closed, exiting\n");
        fflush(NULL);
        _exit(0);
    }
    return 0;
}

void rsx_vulkan_backend_present(void)
{
    rsx_vulkan_backend_present_begin();
    rsx_vulkan_backend_present_finish();
}

/* PORT_FPS_LIMIT=<fps>: hold each guest frame to at most that rate. The
 * frame clock calls the present at each guest flip and drains the FIFO after
 * it, so waiting here holds the guest at its next fence. 0 or unset: no limit. */
static void vk_frame_limit(void)
{
    static long long interval = -1, next;
    if (interval < 0) {
        const char* e = getenv("PORT_FPS_LIMIT");
        const double fps = e ? atof(e) : 0.0;
        interval = fps >= 1.0 && fps <= 1000.0 ? (long long)(1e9 / fps) : 0;
        if (interval) printf("[RSX vulkan] frame limit %.1f fps\n", fps);
    }
    if (!interval) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long long now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (next > now) {
        ts.tv_sec = next / 1000000000LL;
        ts.tv_nsec = next % 1000000000LL;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
        next += interval;
    } else {
        next = now + interval;           /* late: no catch-up burst */
    }
}

void rsx_vulkan_backend_present_begin(void)
{
    if (!s_vk.device) return;
    vk_frame_limit();
    s_vk.present_split = 1;
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
    s_vk.present_split = 0;
}

void rsx_vulkan_backend_present_finish(void)
{
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    vk_present_complete_locked();
    pthread_mutex_unlock(&s_vk.present_mu);
}

/* Finish a begun present only if the GPU is already done with it, so the
 * host keeps walking the next frame while this one renders. A present that
 * is still running is finished by the next begin (which waits for it) or by
 * a later poll. */
void rsx_vulkan_backend_present_poll(void)
{
    if (!s_vk.device || !__atomic_load_n(&s_vk.present_inflight, __ATOMIC_ACQUIRE)) return;
    if (pthread_mutex_trylock(&s_vk.present_mu)) return;
    if (s_vk.present_inflight && pvkGetFenceStatus(s_vk.device, s_vk.present_fence) == VK_SUCCESS)
        vk_present_complete_locked();
    pthread_mutex_unlock(&s_vk.present_mu);
}

u32 rsx_vulkan_backend_presented_center(void)
{
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}

int rsx_vulkan_backend_depth_bounds(void)
{
    return s_vk.eng_active && s_vk.depth_bounds_ok;
}

#endif /* !_WIN32 || PS3RECOMP_RSX_VULKAN */
