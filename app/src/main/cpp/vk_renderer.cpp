#include "vk_renderer.h"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>

#include <cstddef>
#include <cstring>
#include <cmath>
#include <array>

// SPIR-V generated at build time by glslc (-mfmt=c) from shaders/shape.{vert,frag}.
static const uint32_t kVertSpv[] =
#include "shape_vert.spv.h"
;
static const uint32_t kFragSpv[] =
#include "shape_frag.spv.h"
;

// Per-instance vertex data (48 bytes) — one entry per DrawCmd, written to a
// host-visible buffer each frame. Consecutive DrawCmds with the same shape
// become one instanced draw, which keeps the guest->host Vulkan command
// stream small (important under emulation, cheap everywhere).
struct InstanceData {
    float mtx[4];
    float trans[2];
    float style[2];  // x: FillStyle, y: reserved
    float color[4];
};

#define VK_CHECK(x)                                                     \
    do {                                                               \
        VkResult err = (x);                                            \
        if (err != VK_SUCCESS) {                                       \
            LOGE("Vulkan error %d at %s:%d", err, __FILE__, __LINE__); \
            return false;                                              \
        }                                                              \
    } while (0)

// Enumerate and categorise every Vulkan extension the driver exposes.
// Three buckets:
//   USED    – enabled by this renderer
//   PRESENT – driver has it, renderer doesn't enable it (why noted inline)
//   ABSENT  – renderer knows about it but this driver doesn't provide it
static void logExtensionAudit(VkPhysicalDevice phys) {
    struct KnownExt { const char* name; bool used; const char* note; };

    // ── Instance extensions ──────────────────────────────────────────────────
    static const KnownExt kInstKnown[] = {
        {"VK_KHR_surface",                        true,  "required — abstract surface type"},
        {"VK_KHR_android_surface",                true,  "required — ANativeWindow swapchain"},
        {"VK_EXT_debug_utils",                    false, "debug labels/markers — not enabled in this build"},
        {"VK_KHR_get_physical_device_properties2",false, "extended device queries — Vulkan 1.1 core, not needed"},
        {"VK_EXT_swapchain_colorspace",           false, "HDR / wide-gamut display — game uses sRGB only"},
        {"VK_KHR_external_memory_capabilities",   false, "cross-process memory — not used"},
    };

    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> instExts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, instExts.data());

    auto instHas = [&](const char* name) {
        for (auto& e : instExts) if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };

    LOGI("─── Vulkan Instance Extension Audit (%u available) ───", n);
    for (auto& k : kInstKnown) {
        bool present = instHas(k.name);
        if (k.used)         LOGI("  ✓ USED     %-52s %s", k.name, k.note);
        else if (present)   LOGI("  ~ PRESENT  %-52s %s", k.name, k.note);
        else                LOGI("  ✗ ABSENT   %-52s %s", k.name, k.note);
    }
    // Log anything the driver exposes that isn't in our known table
    for (auto& e : instExts) {
        bool known = false;
        for (auto& k : kInstKnown) if (strcmp(e.extensionName, k.name) == 0) { known = true; break; }
        if (!known) LOGI("  ? UNKNOWN  %s", e.extensionName);
    }

    // ── Device extensions ────────────────────────────────────────────────────
    static const KnownExt kDevKnown[] = {
        {"VK_KHR_swapchain",                                true,  "required — present images to display"},
        {"VK_KHR_maintenance1",                             false, "negative-height viewports — not needed (Y already flipped in shader)"},
        {"VK_KHR_maintenance2",                             false, "input attachment read fixes — no input attachments used"},
        {"VK_KHR_maintenance3",                             false, "large descriptor sets — empty pipeline layout, no descriptors"},
        {"VK_KHR_dedicated_allocation",                     false, "per-resource memory — single vertex buffer, not worth it"},
        {"VK_KHR_get_memory_requirements2",                 false, "pairs with dedicated_allocation — same reason"},
        {"VK_EXT_memory_budget",                            false, "heap budget queries — renderer allocates once at startup"},
        {"VK_KHR_dynamic_rendering",                        false, "renderpass-free rendering — using classic render passes"},
        {"VK_KHR_buffer_device_address",                    false, "GPU-side pointers — no indirect/ray-tracing workloads"},
        {"VK_EXT_descriptor_indexing",                      false, "bindless textures — no textures in this renderer"},
        {"VK_KHR_timeline_semaphore",                       false, "timeline sync — using binary semaphores, sufficient here"},
        {"VK_KHR_synchronization2",                         false, "enhanced barriers — simple pipeline, not needed"},
        {"VK_EXT_robustness2",                              false, "null descriptor robustness — trusted internal geometry only"},
        {"VK_ANDROID_external_memory_android_hardware_buffer",false,"AHardwareBuffer interop — no camera/media pipeline"},
        {"VK_KHR_shader_non_semantic_info",                 false, "shader printf/debug — not used in release shaders"},
        {"VK_EXT_memory_priority",                          false, "eviction hints — single allocation, trivial footprint"},
    };

    uint32_t dn = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &dn, nullptr);
    std::vector<VkExtensionProperties> devExts(dn);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &dn, devExts.data());

    auto devHas = [&](const char* name) {
        for (auto& e : devExts) if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };

    LOGI("─── Vulkan Device Extension Audit (%u available) ────", dn);
    for (auto& k : kDevKnown) {
        bool present = devHas(k.name);
        if (k.used)         LOGI("  ✓ USED     %-56s %s", k.name, k.note);
        else if (present)   LOGI("  ~ PRESENT  %-56s %s", k.name, k.note);
        else                LOGI("  ✗ ABSENT   %-56s %s", k.name, k.note);
    }
    for (auto& e : devExts) {
        bool known = false;
        for (auto& k : kDevKnown) if (strcmp(e.extensionName, k.name) == 0) { known = true; break; }
        if (!known) LOGI("  ? UNKNOWN  %s", e.extensionName);
    }
    LOGI("─────────────────────────────────────────────────────");
}

bool VkRenderer::initInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Vulkan Space Invaders";
    app.pEngineName = "spaceinvaders";
    // Target Vulkan 1.0 core: it's all this 2D renderer needs, and the API-24
    // libvulkan stub we link only exports 1.0 entry points anyway.
    app.apiVersion = VK_API_VERSION_1_0;

    const char* useExts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = useExts;

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateInstance failed: %d", r);
        return false;
    }
    LOGI("Vulkan instance created");
    return true;
}

uint32_t VkRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VkRenderer::ensureDevice() {
    if (deviceReady_) return true;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) { LOGE("No Vulkan physical devices"); return false; }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Pick the first device that has a graphics + present capable queue family.
    for (auto d : devices) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
        for (uint32_t i = 0; i < qn; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                phys_ = d;
                queueFamily_ = i;
                break;
            }
        }
        if (phys_ != VK_NULL_HANDLE) break;
    }
    if (phys_ == VK_NULL_HANDLE) { LOGE("No suitable GPU/queue"); return false; }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys_, &props);
    LOGI("Using GPU: %s (Vulkan %d.%d.%d)",
         props.deviceName,
         VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion),
         VK_VERSION_PATCH(props.apiVersion));
    logExtensionAudit(phys_);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* devExts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VK_CHECK(vkCreateDevice(phys_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    // On any failure past this point, tear the partial device down so a
    // retry (next INIT_WINDOW) starts clean instead of leaking the old
    // device and its buffers by overwriting the handles.
    if (!createVertexBuffer() ||
        !createInstanceBuffers() ||
        !createSyncAndCommands()) {
        teardownDevice();
        return false;
    }

    // Pipeline layout: all per-draw data travels as instance attributes, so
    // no descriptors and no push constants.
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkResult pr = vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_);
    if (pr != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout failed %d", pr);
        teardownDevice();
        return false;
    }

    deviceReady_ = true;
    return true;
}

bool VkRenderer::createVertexBuffer() {
    std::vector<float> v;  // interleaved x,y
    auto push = [&](float x, float y) { v.push_back(x); v.push_back(y); };

    // QUAD: spans [-1,1] on both axes (used for HUD rectangles / digit segments).
    shapeFirst_[SHAPE_QUAD] = (uint32_t)(v.size() / 2);
    push(-1, -1); push(1, -1); push(1, 1);
    push(-1, -1); push(1, 1); push(-1, 1);
    shapeCount_[SHAPE_QUAD] = 6;

    // DISC: unit circle as a 32-segment triangle fan (explosion rings, gear
    // body, shield bubble, saucer glow).
    const int N = 32;
    shapeFirst_[SHAPE_DISC] = (uint32_t)(v.size() / 2);
    for (int i = 0; i < N; i++) {
        float a0 = (float)(2.0 * M_PI * i / N);
        float a1 = (float)(2.0 * M_PI * ((i + 1) % N) / N);
        push(0.0f, 0.0f);
        push(cosf(a0), sinf(a0));
        push(cosf(a1), sinf(a1));
    }
    shapeCount_[SHAPE_DISC] = N * 3;

    // INVADERS + SAUCER: pixel-art bitmaps in the classic style, two march
    // frames per invader. Each 'X' cell becomes a quad; the grid is normalised
    // to [-1,1] on both axes so shapes scale like every other primitive.
    auto pushBitmap = [&](Shape shape, const char* const* rows, int w, int h) {
        shapeFirst_[shape] = (uint32_t)(v.size() / 2);
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                if (rows[r][c] != 'X') continue;
                float x0 = -1.0f + 2.0f * c / w;
                float x1 = -1.0f + 2.0f * (c + 1) / w;
                float y0 = -1.0f + 2.0f * r / h;
                float y1 = -1.0f + 2.0f * (r + 1) / h;
                push(x0, y0); push(x1, y0); push(x1, y1);
                push(x0, y0); push(x1, y1); push(x0, y1);
            }
        }
        shapeCount_[shape] = (uint32_t)(v.size() / 2) - shapeFirst_[shape];
    };

    // Squid (8x8) — the small top-row invader.
    static const char* kSquid0[8] = {
        "...XX...",
        "..XXXX..",
        ".XXXXXX.",
        "XX.XX.XX",
        "XXXXXXXX",
        "..X..X..",
        ".X.XX.X.",
        "X.X..X.X",
    };
    static const char* kSquid1[8] = {
        "...XX...",
        "..XXXX..",
        ".XXXXXX.",
        "XX.XX.XX",
        "XXXXXXXX",
        ".X.XX.X.",
        "X......X",
        ".X....X.",
    };
    // Crab (11x8) — the mid-row invader.
    static const char* kCrab0[8] = {
        "..X.....X..",
        "...X...X...",
        "..XXXXXXX..",
        ".XX.XXX.XX.",
        "XXXXXXXXXXX",
        "X.XXXXXXX.X",
        "X.X.....X.X",
        "...XX.XX...",
    };
    static const char* kCrab1[8] = {
        "..X.....X..",
        "X..X...X..X",
        "X.XXXXXXX.X",
        "XXX.XXX.XXX",
        "XXXXXXXXXXX",
        ".XXXXXXXXX.",
        "..X.....X..",
        ".X.......X.",
    };
    // Octopus (12x8) — the wide bottom-row invader.
    static const char* kOcto0[8] = {
        "....XXXX....",
        ".XXXXXXXXXX.",
        "XXXXXXXXXXXX",
        "XXX..XX..XXX",
        "XXXXXXXXXXXX",
        "...XX..XX...",
        "..XX.XX.XX..",
        "XX........XX",
    };
    static const char* kOcto1[8] = {
        "....XXXX....",
        ".XXXXXXXXXX.",
        "XXXXXXXXXXXX",
        "XXX..XX..XXX",
        "XXXXXXXXXXXX",
        "..XXX..XXX..",
        ".XX..XX..XX.",
        "..XX....XX..",
    };
    // Mystery saucer (16x7) crossing the top of the screen.
    static const char* kSaucer[7] = {
        ".....XXXXXX.....",
        "...XXXXXXXXXX...",
        "..XXXXXXXXXXXX..",
        ".XX.XX.XX.XX.XX.",
        "XXXXXXXXXXXXXXXX",
        "..XXX..XX..XXX..",
        "....X......X....",
    };
    pushBitmap(SHAPE_INVADER_A0, kSquid0,  8, 8);
    pushBitmap(SHAPE_INVADER_A1, kSquid1,  8, 8);
    pushBitmap(SHAPE_INVADER_B0, kCrab0,  11, 8);
    pushBitmap(SHAPE_INVADER_B1, kCrab1,  11, 8);
    pushBitmap(SHAPE_INVADER_C0, kOcto0,  12, 8);
    pushBitmap(SHAPE_INVADER_C1, kOcto1,  12, 8);
    pushBitmap(SHAPE_SAUCER,     kSaucer, 16, 7);

    // SHIP_WINGS: wide swept wings — dark steel-blue layer behind the body.
    shapeFirst_[SHAPE_SHIP_WINGS] = (uint32_t)(v.size() / 2);
    push( 0.00f, -0.50f); push(-1.00f,  0.20f); push(-0.28f,  0.95f);
    push( 0.00f, -0.50f); push( 0.28f,  0.95f); push( 1.00f,  0.20f);
    push( 0.00f, -0.50f); push(-0.28f,  0.95f); push( 0.28f,  0.95f);
    shapeCount_[SHAPE_SHIP_WINGS] = 9;

    // SHIP_BODY: narrow tall fuselage — bright cyan central layer.
    shapeFirst_[SHAPE_SHIP_BODY] = (uint32_t)(v.size() / 2);
    push( 0.00f, -1.00f); push(-0.22f, -0.30f); push( 0.22f, -0.30f);
    push(-0.22f, -0.30f); push(-0.18f,  0.90f); push( 0.18f,  0.90f);
    push(-0.22f, -0.30f); push( 0.18f,  0.90f); push( 0.22f, -0.30f);
    shapeCount_[SHAPE_SHIP_BODY] = 9;

    // SHIP_NOSE: bright white narrow spike at the very tip.
    shapeFirst_[SHAPE_SHIP_NOSE] = (uint32_t)(v.size() / 2);
    push( 0.00f, -1.00f); push(-0.07f, -0.55f); push( 0.07f, -0.55f);
    shapeCount_[SHAPE_SHIP_NOSE] = 3;

    VkDeviceSize size = v.size() * sizeof(float);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &vbo_));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, vbo_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { LOGE("No host-visible memory"); return false; }
    VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &vboMem_));
    VK_CHECK(vkBindBufferMemory(device_, vbo_, vboMem_, 0));

    void* dst = nullptr;
    VK_CHECK(vkMapMemory(device_, vboMem_, 0, size, 0, &dst));
    memcpy(dst, v.data(), size);
    vkUnmapMemory(device_, vboMem_);
    return true;
}

// One host-visible, persistently-mapped instance buffer per frame in flight.
// Low-latency mode runs a single frame in flight, so it only allocates one
// set of these (and of the sync/command objects below); teardownDevice null-
// checks every slot, so the unused ones are simply skipped.
bool VkRenderer::createInstanceBuffers() {
    VkDeviceSize size = kMaxInstances * sizeof(InstanceData);
    for (int i = 0; i < framesInFlight(); i++) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &instBuf_[i]));

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, instBuf_[i], &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX) { LOGE("No host-visible memory"); return false; }
        VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &instMem_[i]));
        VK_CHECK(vkBindBufferMemory(device_, instBuf_[i], instMem_[i], 0));
        VK_CHECK(vkMapMemory(device_, instMem_[i], 0, size, 0, &instMapped_[i]));
    }
    return true;
}

bool VkRenderer::createSyncAndCommands() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_));

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = (uint32_t)framesInFlight();
    VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, cmdBufs_));

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < framesInFlight(); i++) {
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_[i]));
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &inFlight_[i]));
    }
    // renderFinished_ semaphores live with the swapchain (one per image).
    return true;
}

bool VkRenderer::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_));
    return true;
}

static VkShaderModule makeModule(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

bool VkRenderer::createPipeline() {
    VkShaderModule vs = makeModule(device_, kVertSpv, sizeof(kVertSpv));
    VkShaderModule fs = makeModule(device_, kFragSpv, sizeof(kFragSpv));
    if (!vs || !fs) { LOGE("Shader module creation failed"); return false; }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // Binding 0: per-vertex position. Binding 1: per-instance transform,
    // translation, style and colour (matches InstanceData).
    VkVertexInputBindingDescription binds[2] = {
        {0, sizeof(float) * 2,   VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(InstanceData), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    VkVertexInputAttributeDescription attrs[5] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT,       0},
        {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, mtx)},
        {2, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, trans)},
        {3, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, style)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, color)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = binds;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = pipelineLayout_;
    pci.renderPass = renderPass_;
    pci.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (r != VK_SUCCESS) { LOGE("Pipeline creation failed %d", r); return false; }
    return true;
}

bool VkRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);

    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFF) {
        extent_.width = (uint32_t)ANativeWindow_getWidth(window_);
        extent_.height = (uint32_t)ANativeWindow_getHeight(window_);
    }
    if (extent_.width == 0 || extent_.height == 0) return false;  // minimized

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmtCount, nullptr);
    if (fmtCount == 0) { LOGE("No surface formats"); return false; }  // lost surface mid-recreate
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmtCount, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats) {
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    format_ = chosen.format;

    // Low-latency mode keeps the queue as short as the driver allows; the
    // default adds one image for smooth pipelining.
    uint32_t imageCount = lowLatency_ ? caps.minImageCount : caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    // FIFO is always supported; in low-latency mode prefer MAILBOX (newest
    // frame replaces the queued one instead of waiting behind it).
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (lowLatency_) {
        uint32_t pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pmCount, nullptr);
        std::vector<VkPresentModeKHR> modes(pmCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pmCount, modes.data());
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) { presentMode = m; break; }
    }

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = imageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_));
    if (lowLatency_)
        LOGI("Low-latency swapchain: %u images, present mode %d",
             imageCount, (int)presentMode);

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());

    // One present-wait semaphore per swapchain image (see vk_renderer.h).
    renderFinished_.resize(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; i++)
        VK_CHECK(vkCreateSemaphore(device_, &semci, nullptr, &renderFinished_[i]));

    // The render pass bakes in the attachment format, and the pipeline is
    // built against the render pass: if a recreate chose a different format
    // (e.g. the window moved to a display with another preferred format),
    // both are stale and must be rebuilt.
    if (renderPass_ != VK_NULL_HANDLE && renderPassFormat_ != format_) {
        LOGI("Surface format changed %d -> %d, rebuilding render pass + pipeline",
             (int)renderPassFormat_, (int)format_);
        vkDeviceWaitIdle(device_);
        if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    if (renderPass_ == VK_NULL_HANDLE) {
        if (!createRenderPass()) return false;
        renderPassFormat_ = format_;
    }
    if (pipeline_ == VK_NULL_HANDLE && pipelineLayout_ != VK_NULL_HANDLE) {
        if (!createPipeline()) return false;
    }

    views_.resize(n);
    framebuffers_.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &views_[i]));

        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = renderPass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &views_[i];
        fci.width = extent_.width;
        fci.height = extent_.height;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]));
    }
    return true;
}

void VkRenderer::destroySwapchain() {
    if (device_) vkDeviceWaitIdle(device_);
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto v : views_) if (v) vkDestroyImageView(device_, v, nullptr);
    for (auto s : renderFinished_) if (s) vkDestroySemaphore(device_, s, nullptr);
    framebuffers_.clear();
    views_.clear();
    images_.clear();
    renderFinished_.clear();
    if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
}

bool VkRenderer::initWindow(ANativeWindow* window) {
    window_ = window;

    VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = window;
    VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_));

    if (!ensureDevice()) return false;
    if (!createSwapchain()) return false;
    if (pipeline_ == VK_NULL_HANDLE) {
        if (!createPipeline()) return false;
    }
    swapchainReady_ = true;
    LOGI("Swapchain ready: %ux%u", extent_.width, extent_.height);
    return true;
}

void VkRenderer::termWindow() {
    swapchainReady_ = false;
    destroySwapchain();
    if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    window_ = nullptr;
}

// Android can invalidate the surface without destroying the window (e.g. a
// projected-display disconnect). Rebuild the surface from the live window;
// the swapchain self-heal at the top of drawFrame rebuilds the rest.
bool VkRenderer::recreateSurface() {
    if (!window_) return false;
    LOGW("Surface lost, recreating");
    destroySwapchain();
    if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = window_;
    VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_));
    return true;
}

void VkRenderer::recordCommandBuffer(VkCommandBuffer cb, uint32_t imageIndex,
                                     const std::vector<DrawCmd>& cmds, const float clear[3]) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue cv{};
    cv.color = {{clear[0], clear[1], clear[2], 1.0f}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = renderPass_;
    rp.framebuffer = framebuffers_[imageIndex];
    rp.renderArea.extent = extent_;
    rp.clearValueCount = 1;
    rp.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // Centered viewport, scaled by renderScale_ (1.0 = full surface). The
    // render pass has already cleared the whole framebuffer, so the surround
    // is clear-colour (black = transparent on AR lenses).
    float vw = extent_.width * renderScale_;
    float vh = extent_.height * renderScale_;
    float vx = (extent_.width - vw) * 0.5f;
    float vy = (extent_.height - vh) * 0.5f;
    VkViewport vpt{vx, vy, vw, vh, 0.0f, 1.0f};
    VkRect2D sc{{(int32_t)vx, (int32_t)vy}, {(uint32_t)vw, (uint32_t)vh}};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    // Write this frame's per-instance data (in submission order, so
    // firstInstance below indexes straight into it).
    size_t count = cmds.size();
    if (count > kMaxInstances) count = kMaxInstances;
    auto* inst = static_cast<InstanceData*>(instMapped_[frame_]);
    for (size_t i = 0; i < count; i++) {
        const auto& c = cmds[i];
        InstanceData& d = inst[i];
        d.mtx[0] = c.mtx[0]; d.mtx[1] = c.mtx[1];
        d.mtx[2] = c.mtx[2]; d.mtx[3] = c.mtx[3];
        d.trans[0] = c.tx; d.trans[1] = c.ty;
        d.style[0] = c.style; d.style[1] = c.seed;
        d.color[0] = c.color[0]; d.color[1] = c.color[1];
        d.color[2] = c.color[2]; d.color[3] = c.color[3];
    }

    VkBuffer bufs[2] = {vbo_, instBuf_[frame_]};
    VkDeviceSize offs[2] = {0, 0};
    vkCmdBindVertexBuffers(cb, 0, 2, bufs, offs);

    // Batch consecutive DrawCmds sharing a shape into one instanced draw —
    // painter's order is preserved, but the command stream shrinks from two
    // calls per DrawCmd to one call per run.
    size_t i = 0;
    while (i < count) {
        int shape = cmds[i].shape;
        size_t j = i + 1;
        while (j < count && cmds[j].shape == shape) j++;
        vkCmdDraw(cb, shapeCount_[shape], (uint32_t)(j - i),
                  shapeFirst_[shape], (uint32_t)i);
        i = j;
    }

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
}

void VkRenderer::drawFrame(const std::vector<DrawCmd>& cmds, const float clear[3]) {
    if (!swapchainReady_) return;

    // Foldables resize the surface on fold/unfold without destroying the
    // window (configChanges keeps the activity alive), and the compositor
    // would happily stretch the stale-size swapchain onto the new panel.
    // Rebuild the swapchain whenever the surface extent no longer matches.
    VkSurfaceCapabilitiesKHR caps;
    // The caps query is a synchronous host round trip (expensive on the
    // emulator's guest->host Vulkan), so poll at ~5 Hz instead of per frame;
    // a stale extent also surfaces as OUT_OF_DATE on acquire/present below.
    if (frameCount_++ % 12 == 0) {
        VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) {
            if (!recreateSurface()) return;
        } else if (cr == VK_SUCCESS &&
            caps.currentExtent.width != 0xFFFFFFFF &&
            caps.currentExtent.width != 0 && caps.currentExtent.height != 0 &&
            (caps.currentExtent.width != extent_.width ||
             caps.currentExtent.height != extent_.height)) {
            LOGI("Surface resized %ux%u -> %ux%u, recreating swapchain",
                 extent_.width, extent_.height,
                 caps.currentExtent.width, caps.currentExtent.height);
            destroySwapchain();
        }
    }
    // A recreate can fail transiently (zero-extent surface mid-fold, or an
    // OUT_OF_DATE recreate below that didn't stick). Retry every frame until
    // the surface is usable again rather than rendering with a null swapchain
    // or freezing until the next INIT_WINDOW.
    if (swapchain_ == VK_NULL_HANDLE && !createSwapchain()) return;

    vkWaitForFences(device_, 1, &inFlight_[frame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailable_[frame_], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        destroySwapchain();
        createSwapchain();  // failure is retried at the top of the next frame
        return;
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        recreateSurface();  // swapchain rebuilt by the self-heal next frame
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("acquire failed %d", r);
        return;
    }

    vkResetFences(device_, 1, &inFlight_[frame_]);
    vkResetCommandBuffer(cmdBufs_[frame_], 0);
    recordCommandBuffer(cmdBufs_[frame_], imageIndex, cmds, clear);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_[frame_];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBufs_[frame_];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished_[imageIndex];
    VkResult sr = vkQueueSubmit(queue_, 1, &si, inFlight_[frame_]);
    if (sr != VK_SUCCESS) {
        // The fence was just reset and this failed submit will never signal
        // it; waiting on it next frame would block the main thread forever
        // (ANR). Halt rendering — the next INIT_WINDOW rebuilds everything.
        LOGE("vkQueueSubmit failed %d, halting rendering", sr);
        swapchainReady_ = false;
        return;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        destroySwapchain();
        createSwapchain();  // failure is retried at the top of the next frame
    } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
        recreateSurface();
    }

    frame_ = (frame_ + 1) % framesInFlight();
}

// Destroy everything ensureDevice creates, in reverse order. Called on the
// normal teardown path and when ensureDevice fails partway through, so a
// retry starts from clean handles instead of overwriting (and leaking) them.
void VkRenderer::teardownDevice() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    for (int i = 0; i < kFramesInFlight; i++) {
        if (imageAvailable_[i]) { vkDestroySemaphore(device_, imageAvailable_[i], nullptr); imageAvailable_[i] = VK_NULL_HANDLE; }
        if (inFlight_[i]) { vkDestroyFence(device_, inFlight_[i], nullptr); inFlight_[i] = VK_NULL_HANDLE; }
    }
    if (cmdPool_) { vkDestroyCommandPool(device_, cmdPool_, nullptr); cmdPool_ = VK_NULL_HANDLE; }
    for (int i = 0; i < kFramesInFlight; i++) {
        if (instBuf_[i]) { vkDestroyBuffer(device_, instBuf_[i], nullptr); instBuf_[i] = VK_NULL_HANDLE; }
        if (instMem_[i]) {
            vkUnmapMemory(device_, instMem_[i]);
            vkFreeMemory(device_, instMem_[i], nullptr);
            instMem_[i] = VK_NULL_HANDLE; instMapped_[i] = nullptr;
        }
    }
    if (vbo_) { vkDestroyBuffer(device_, vbo_, nullptr); vbo_ = VK_NULL_HANDLE; }
    if (vboMem_) { vkFreeMemory(device_, vboMem_, nullptr); vboMem_ = VK_NULL_HANDLE; }
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    deviceReady_ = false;
}

void VkRenderer::cleanup() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        if (renderPass_) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
        if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
        teardownDevice();
    }
    // Surface and instance exist independently of the device (a window may
    // never have arrived): destroy them even when the device was never made.
    if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
}
