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

    void drawFrame(const std::vector<DrawCmd>& cmds, const float clear[3]);

private:
    bool ensureDevice();
    bool createSwapchain();
    void destroySwapchain();
    bool createRenderPass();
    bool createPipeline();
    bool createVertexBuffer();
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

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkBuffer vbo_ = VK_NULL_HANDLE;
    VkDeviceMemory vboMem_ = VK_NULL_HANDLE;
    uint32_t shapeFirst_[SHAPE_COUNT] = {0};
    uint32_t shapeCount_[SHAPE_COUNT] = {0};

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    static const int kFramesInFlight = 2;
    VkCommandBuffer cmdBufs_[kFramesInFlight] = {VK_NULL_HANDLE};
    VkSemaphore imageAvailable_[kFramesInFlight] = {VK_NULL_HANDLE};
    VkSemaphore renderFinished_[kFramesInFlight] = {VK_NULL_HANDLE};
    VkFence inFlight_[kFramesInFlight] = {VK_NULL_HANDLE};
    uint32_t frame_ = 0;

    bool deviceReady_ = false;
    bool swapchainReady_ = false;
};
