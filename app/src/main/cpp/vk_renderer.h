#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "common.h"

struct ANativeWindow;

// A minimal 2D Vulkan renderer: one pipeline, push-constant transforms, a single
// vertex buffer holding every shape. Draw a frame by handing it a list of DrawCmd.
class VkRenderer {
public:
    bool initInstance();
    // Create surface + (first time) device/pipeline + swapchain for this window.
    bool initWindow(ANativeWindow* window);
    // Tear down swapchain + surface (window gone). Device is kept for resume.
    void termWindow();
    void cleanup();

    bool ready() const { return swapchainReady_; }
    int width() const { return (int)extent_.width; }
    int height() const { return (int)extent_.height; }

    // Trade throughput for input-to-photon latency: minimal swapchain image
    // count, a single frame in flight, and MAILBOX present when available.
    // Used on the AI-Glasses projected display, whose 30 Hz vsync makes every
    // queued frame cost a full 33 ms. Call before initWindow().
    void setLowLatencyMode(bool v) { lowLatency_ = v; }

    // Render into a centered, scaled-down viewport instead of the full
    // surface (0.5 = quarter of the pixels). The surround stays clear-colour.
    // Draw commands are NDC-based, so callers need no changes; on the glasses
    // this shrinks the game to a floating window and cuts raster work 4x.
    void setRenderScale(float s) { renderScale_ = s; }

    void drawFrame(const std::vector<DrawCmd>& cmds, const float clear[3]);

private:
    bool ensureDevice();
    void teardownDevice();
    bool createSwapchain();
    void destroySwapchain();
    bool recreateSurface();
    bool createRenderPass();
    bool createPipeline();
    bool createVertexBuffer();
    bool createInstanceBuffers();
    bool createSyncAndCommands();
    void recordCommandBuffer(VkCommandBuffer cb, uint32_t imageIndex,
                             const std::vector<DrawCmd>& cmds, const float clear[3]);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);

    ANativeWindow* window_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_ = {0, 0};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFormat renderPassFormat_ = VK_FORMAT_UNDEFINED;  // format renderPass_ was built with

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkBuffer vbo_ = VK_NULL_HANDLE;
    VkDeviceMemory vboMem_ = VK_NULL_HANDLE;
    uint32_t shapeFirst_[SHAPE_COUNT] = {0};
    uint32_t shapeCount_[SHAPE_COUNT] = {0};

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    static const int kFramesInFlight = 2;
    // Per-frame host-visible instance buffers (persistently mapped).
    static const uint32_t kMaxInstances = 8192;
    VkBuffer instBuf_[kFramesInFlight] = {VK_NULL_HANDLE};
    VkDeviceMemory instMem_[kFramesInFlight] = {VK_NULL_HANDLE};
    void* instMapped_[kFramesInFlight] = {nullptr};
    VkCommandBuffer cmdBufs_[kFramesInFlight] = {VK_NULL_HANDLE};
    VkSemaphore imageAvailable_[kFramesInFlight] = {VK_NULL_HANDLE};
    // Present-wait semaphores are per swapchain image, not per frame in
    // flight: the in-flight fence covers command-buffer execution but not the
    // presentation engine's wait, so re-signaling a per-frame semaphore can
    // race the present of an earlier frame that used the same one.
    std::vector<VkSemaphore> renderFinished_;
    VkFence inFlight_[kFramesInFlight] = {VK_NULL_HANDLE};
    uint32_t frame_ = 0;
    uint32_t frameCount_ = 0;

    bool deviceReady_ = false;
    bool swapchainReady_ = false;
    bool lowLatency_ = false;
    float renderScale_ = 1.0f;

    int framesInFlight() const { return lowLatency_ ? 1 : kFramesInFlight; }
};
