// 模块说明：应用主体的类型定义与接口声明。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <VkBootstrap.h>
#include <vulkan/vulkan.hpp>

#include "util.h"

struct GLFWwindow;

namespace siggraph {

// 一组着色器文件名（顶点 + 片段），用于创建着色器对象。
struct ShaderFilePair {
    std::string_view vertexName;
    std::string_view fragmentName;
};

// 场景资源路径与预设场景表；路径都相对于资源目录。
namespace SceneData {

constexpr std::string_view defaultSceneRelativePath = "modular-demo/modular-demo.gltf";

// 一个预设场景：显示名称，以及组成它的 glTF 文件列表。
struct ScenePreset {
    std::string_view name;
    std::span<const std::string_view> files;
};

constexpr std::array<std::string_view, 1> modularDemoSceneFiles = {"modular-demo/modular-demo.gltf"};

constexpr std::array<std::string_view, 2> sponzaSceneFiles = {"external/sponza/Models/Sponza/glTF/Sponza.gltf",
                                                              "SponzaExtras/camera.gltf"};

constexpr std::array<std::string_view, 3> sponzaWithLogoSceneFiles = {
    "external/sponza/Models/Sponza/glTF/Sponza.gltf", "SponzaExtras/VulkanLogo/VulkanLogo.gltf",
    "SponzaExtras/camera.gltf"};

constexpr std::array<std::string_view, 1> vulkanLogoSceneFiles = {"SponzaExtras/VulkanLogo/VulkanLogo.gltf"};

constexpr std::array<ScenePreset, 4> scenePresets = {
    ScenePreset{.name = "modular-demo", .files = modularDemoSceneFiles},
    ScenePreset{.name = "Sponza（含相机）", .files = sponzaSceneFiles},
    ScenePreset{.name = "Sponza + Vulkan Logo", .files = sponzaWithLogoSceneFiles},
    ScenePreset{.name = "Vulkan Logo", .files = vulkanLogoSceneFiles},
};

constexpr ShaderFilePair albedoAndNormalShaderFilePair{.vertexName = "basic.vert.spv",
                                                       .fragmentName = "basic.frag.spv"};

constexpr ShaderFilePair albedoShaderFilePair{.vertexName = "basic.vert.spv", .fragmentName = "albedo.frag.spv"};

constexpr ShaderFilePair solidColorShaderFilePair{.vertexName = "basic.vert.spv",
                                                  .fragmentName = "solid_color.frag.spv"};

} // namespace SceneData

// 应用主体：持有窗口、Vulkan 对象、场景资源与界面状态。
class Application {
public:
    Application() = default;

    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void run();

    void setFrameLimit(std::uint32_t frameLimit);

    void requestSceneImport(std::string gltfPath);

private:
    static constexpr vk::ImageSubresourceRange
    CreateImageSubresourceRange(const std::uint32_t baseMipLevel = 0,
                                const vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor,
                                const std::uint32_t levelCount = 1)
    {
        return vk::ImageSubresourceRange{
            .aspectMask = aspectMask,
            .baseMipLevel = baseMipLevel,
            .levelCount = levelCount,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
    }

    // 记录图像的布局与读写阶段，用于自动生成布局转换屏障。
    struct ImageState {
        vk::ImageLayout m_layout = vk::ImageLayout::eUndefined;
        vk::PipelineStageFlags2 m_stageMask = vk::PipelineStageFlagBits2::eNone;
        vk::AccessFlags2 m_accessMask = vk::AccessFlagBits2::eNone;
        vk::ImageAspectFlags m_aspectMask = vk::ImageAspectFlagBits::eColor;

        bool operator==(const ImageState&) const = default;
    };

    // GPU 缓冲：句柄、显存、GPU 地址与 CPU 映射地址。
    struct GpuBuffer {
        vk::UniqueDeviceMemory m_memory{};
        vk::UniqueBuffer m_buffer{};
        vk::DeviceAddress m_addressGPU{};
        vk::DeviceSize m_size = 0;
        void* m_addressCPU = nullptr;

        void destroy(vk::Device device);
    };

    // GPU 图像：句柄、显存、mip 层级与逐层布局状态。
    struct GpuImage {
        vk::UniqueDeviceMemory m_memory{};
        vk::UniqueImage m_image{};
        std::uint32_t m_mipLevels = 1;
        std::vector<ImageState> m_states{};

        void destroy();
        void transition(vk::CommandBuffer commandBuffer, const ImageState& newState);
        void transition(vk::CommandBuffer commandBuffer, const ImageState& newState, std::uint32_t baseMipLevel,
                        std::uint32_t mipLevelCount);
    };

    // 图像与其视图的组合；销毁顺序为先视图后图像。
    struct GpuViewImage {
        GpuImage m_image{};
        vk::UniqueImageView m_imageView{};

        void destroy();
    };

    // 每个并行帧独占的命令缓冲区、相机缓冲、信号量与围栏。
    struct FrameInFlightResources {
        vk::CommandBuffer m_commandBuffer{};
        GpuBuffer m_camera{};

        vk::UniqueSemaphore m_imageAvailableSemaphore{};
        vk::UniqueFence m_inFlightFence{};

        GpuViewImage m_depthImage{};
    };

    // 每张交换链图像对应的视图、渲染完成信号量与布局状态。
    struct SwapchainImageResources {
        vk::Image m_image{};

        vk::UniqueImageView m_imageView{};

        vk::UniqueSemaphore m_renderFinishedSemaphore{};

        ImageState m_state{};
    };

    // 初始化阶段专用的辅助命令缓冲区及其激活标志。
    struct HelperCommandBuffer {
        vk::CommandBuffer m_commandBuffer{};
        bool m_active = false;
    };

    // 传给着色器的相机视图投影矩阵与相机位置。
    struct CameraData {
        glm::mat4 viewProjection;
        glm::vec4 cameraPosition;
    };

    // 每个实例的模型矩阵；顶点着色器按 firstInstance 索引。
    struct alignas(16) ObjectData {
        glm::mat4 model;
    };
    static_assert(sizeof(ObjectData) == 64);

    // 点光源的位置/强度与环境光的颜色/强度。
    struct alignas(16) LightData {
        glm::vec3 pointPosition;
        float pointIntensity;
        glm::vec3 ambientColor;
        float ambientIntensity;
    };
    static_assert(offsetof(LightData, pointIntensity) == 12);
    static_assert(offsetof(LightData, ambientColor) == 16);
    static_assert(offsetof(LightData, ambientIntensity) == 28);
    static_assert(sizeof(LightData) == 32);

    // 纯色着色器变体使用的颜色。
    struct alignas(16) SolidColorData {
        glm::vec4 color;
    };
    static_assert(sizeof(SolidColorData) == 16);

    // 着色器变体：反照率+法线贴图、仅反照率、纯色。
    enum class ShaderVariant : std::uint32_t {
        AlbedoAndNormal,
        Albedo,
        SolidColor,
    };

    // 一次绘制所需的全部信息：网格、实例、着色器变体与贴图索引。
    struct SceneDraw {
        std::uint32_t m_meshId = 0;
        std::uint32_t m_objectIndex = 0;
        ShaderVariant m_shaderVariant = ShaderVariant::AlbedoAndNormal;
        std::uint32_t m_albedoTextureIndex = util::gltf::invalidGltfId;
        std::uint32_t m_normalTextureIndex = util::gltf::invalidGltfId;
        std::string m_debugName;
    };

    // 一个网格的顶点缓冲、索引缓冲与索引数量。
    struct GpuMesh {
        GpuBuffer m_vertices{};
        GpuBuffer m_indices{};
        std::uint32_t m_indexCount = 0;
    };

    // 当前场景的全部 GPU 资源。
    struct SceneResources {
        GpuBuffer m_objects{};
        GpuBuffer m_pointLight{};
        GpuBuffer m_solidColor{};
        std::vector<GpuMesh> m_meshes{};
        std::vector<GpuImage> m_textures{};
        std::vector<SceneDraw> m_drawData{};
    };

    // 描述符堆：堆缓冲、设备相关的描述符尺寸与各描述符的堆内偏移。
    struct DescriptorHeapResources {

        struct DescriptorHeapData {
            GpuBuffer m_buffer;

            VkDeviceSize m_bindOffset = 0;

            VkDeviceSize m_rangeSize = 0;

            VkDeviceSize m_alignment = 0;
            VkDeviceSize m_minReservedRange = 0;
        };

        DescriptorHeapData m_resourceHeap{};
        DescriptorHeapData m_samplerHeap{};

        VkDeviceSize m_sampledImageSize = 0;
        VkDeviceSize m_samplerSize = 0;
        VkDeviceSize m_uniformBufferSize = 0;
        VkDeviceSize m_storageBufferSize = 0;
        VkDeviceSize m_imageDescriptorAlignment = 0;
        VkDeviceSize m_bufferDescriptorAlignment = 0;
        VkDeviceSize m_samplerDescriptorAlignment = 0;
        VkDeviceSize m_maxPushDataSize = 0;
        std::uint32_t m_uniformBufferStride = 0;
        std::uint32_t m_sampledImageStride = 0;

        std::uint32_t m_cameraOffset = 0;
        std::uint32_t m_objectsOffset = 0;
        std::uint32_t m_pointLightOffset = 0;
        std::uint32_t m_solidColorOffset = 0;
        std::uint32_t m_pushTextureOffset = 0;
        std::uint32_t m_linearSamplerOffset = 0;
        std::uint32_t m_nearestSamplerOffset = 0;
    };

    // 一对顶点/片段着色器对象。
    struct ShaderGroup {
        vk::UniqueShaderEXT m_vertex{};
        vk::UniqueShaderEXT m_fragment{};
    };

    void initGLFWWindow();

    void initVulkanVKB();

    void initCommandPool();

    void initFramesInFlightResources();

    void initSwapchainImageSyncObjects();

    void initDepthImages();

    void initImGui();

    void initEmptyScene();

    void scanAvailableScenes();

    void destroySceneResources();

    void importScene(std::span<const std::filesystem::path> gltfPaths);

    void queueImport(std::span<const std::filesystem::path> gltfPaths, std::string_view label);

    void buildSceneFromGltf(const util::gltf::ParsedData& gltfData);

    void resetToEmptyScene();

    void copyToImportPathBuffer(std::string_view path);

    void initSceneMeshesAndDrawData(const util::gltf::ParsedData& gltfData, std::vector<ObjectData>& objects);

    void initSceneCamera(const glm::vec3& cameraPos, const glm::vec3& cameraLookAt);

    void initObjectBuffer(std::span<const ObjectData> objects);

    void initPointLight(const glm::vec3& focusPoint);

    void initSolidColor();

    void initSceneTextures(const util::gltf::ParsedData& gltfData);

    void initDescriptorHeaps();

    [[nodiscard]] std::vector<vk::DescriptorSetAndBindingMappingEXT> buildShaderDescriptorMappings(
        const std::unordered_map<std::string, util::slang::ShaderResourceBinding>& shaderResourceBindings) const;

    void calculateVertexInputs();

    void initShaderObjects();

    ShaderGroup createShaderGroup(const ShaderFilePair& shaderFilePair);

    void mainLoop();

    void updateCamera(float deltaSeconds);

    void updateImGui();

    void recordImGuiDrawData(vk::CommandBuffer commandBuffer);

    void renderFrame();

    void waitForFrameResources(FrameInFlightResources& frame);

    std::uint32_t acquireSwapchainImage(FrameInFlightResources& frame);

    void uploadCameraData(FrameInFlightResources& frame);

    void startRecordingCommandBuffer(FrameInFlightResources& frame);

    void recordRenderingCommandBuffer(FrameInFlightResources& frame, std::uint32_t frameIndex,
                                      std::uint32_t swapchainImageIndex);
    void finishAndSubmitMainCommandBuffer(FrameInFlightResources& frame, std::uint32_t swapchainImageIndex);

    [[nodiscard]] vk::CommandBuffer getHelperCommandBuffer() const;

    void beginHelperCommands();

    void endHelperCommandsAndFlushUploads();

    void cleanup();

    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t typeBits, vk::MemoryPropertyFlags properties) const;
    [[nodiscard]] GpuBuffer createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                                         vk::MemoryPropertyFlags memoryProperties, std::string_view debugName) const;
    [[nodiscard]] GpuBuffer uploadToNewStagingBuffer(std::span<const std::byte> data, std::string_view debugName) const;
    [[nodiscard]] GpuBuffer uploadToNewGpuBuffer(std::span<const std::byte> data, vk::BufferUsageFlags finalUsage,
                                                 std::string_view debugName);
    void uploadBuffer(const GpuBuffer& buffer, std::span<const std::byte> data) const;
    void allocateGpuImage(GpuImage& image) const;
    [[nodiscard]] GpuImage createTexture(const util::ImageRgba8& image, std::string_view debugName);

    template <typename Handle>
    void setDebugName(Handle handle, vk::ObjectType objectType, const std::string& name) const;
    static void beginDebugLabel(vk::CommandBuffer commandBuffer, const std::string& name,
                                const std::array<float, 4>& color);
    static void endDebugLabel(vk::CommandBuffer commandBuffer);

    static void transitionImage(vk::CommandBuffer commandBuffer, vk::Image image, ImageState& currentState,
                                const ImageState& newState, vk::ImageSubresourceRange subresourceRange);

    GLFWwindow* m_window = nullptr;

    struct {
        vkb::Instance m_instance{};
        vkb::Device m_device{};
        vkb::Swapchain m_swapchain{};
    } m_vkbData;

    vk::SurfaceKHR m_surface{};
    vk::Device m_logicalDevice{};
    std::vector<vk::MemoryPropertyFlags> m_memoryTypeFlags{};

    vk::Queue m_graphicsQueue{};
    vk::Queue m_presentQueue{};
    std::uint32_t m_graphicsQueueFamily = 0;

    vk::Format m_swapchainFormat{};
    vk::Extent2D m_swapchainExtent{};
    std::vector<SwapchainImageResources> m_swapchainImages;

    vk::UniqueCommandPool m_commandPool{};
    HelperCommandBuffer m_helperCommandBuffer{};
    std::vector<GpuBuffer> m_pendingUploadStagingBuffers{};

    static constexpr std::uint32_t maxFramesInFlight = 2;
    std::array<FrameInFlightResources, maxFramesInFlight> m_framesInFlight{};

    std::uint32_t m_currentFrameInFlight = 0;

    std::uint32_t m_remainingFrameLimit = 0;

    struct VertexInput {
        std::vector<vk::VertexInputBindingDescription2EXT> m_vertexBindings{};
        std::vector<vk::VertexInputAttributeDescription2EXT> m_vertexAttributes{};
    };
    VertexInput m_vertexInput;

    struct ShaderObjects {
        ShaderGroup m_albedoAndNormal{};
        ShaderGroup m_albedo{};
        ShaderGroup m_solidColor{};
    };
    ShaderObjects m_shaderObjects;

    struct ShaderBinaryCacheProperties {
        std::array<std::uint8_t, VK_UUID_SIZE> m_shaderBinaryUUID{};
        std::uint32_t m_shaderBinaryVersion = 0;
    };
    ShaderBinaryCacheProperties m_shaderBinaryCacheProperties{};

    SceneResources m_scene{};
    DescriptorHeapResources m_descriptorHeaps{};

    // 显示模式：实体模式填充分边形，网格模式只画线框。
    // 线框依赖设备的 fillModeNonSolid 特性，不支持时界面会禁用该选项。
    bool m_wireframeSupported = false;
    bool m_wireframeMode = false;

    // 是否已成功导入过场景，仅用于界面显示。
    bool m_sceneLoaded = false;
    // 待导入场景的文件列表，由主循环在两帧之间消费。
    std::vector<std::filesystem::path> m_pendingImportPaths;
    std::string m_pendingImportLabel;
    std::string m_importStatus = "尚未导入场景";
    std::array<char, 512> m_importPathBuffer{};
    std::vector<std::string> m_availableScenes;
    std::array<bool, SceneData::scenePresets.size()> m_presetAvailable{};
    int m_selectedSceneIndex = -1;
    bool m_sceneListScanned = false;

    // 相机状态：位置与偏航/俯仰角。
    glm::vec3 m_cameraPos{};
    float m_cameraYaw = 0.0F;
    float m_cameraPitch = 0.0F;

    // 右键视角拖拽状态：上一帧光标位置，用于计算帧间位移。
    bool m_mouseLookActive = false;
    double m_lastMouseX = 0.0;
    double m_lastMouseY = 0.0;

    bool m_imGuiInitialized = false;
    bool m_showImGuiDemoWindow = false;

    bool m_showSceneWindow = true;

    bool m_needDefaultDockLayout = false;

    float m_lastFrameTimeMilliseconds = 0.0F;
    float m_lastFramesPerSecond = 0.0F;

    // 俯仰角上限，避免视线与世界上方向共线导致叉乘退化。
    static constexpr float cameraPitchLimit = glm::radians(89.0F);

    // 鼠标每移动 1 像素对应的旋转弧度。
    static constexpr float mouseLookSensitivity = 0.004F;

    // 固定窗口尺寸；本示例未实现窗口缩放。
    static constexpr int windowWidth = 1280;
    static constexpr int windowHeight = 720;

    // 调试开关与各阶段调试标签的颜色。
    static struct DebugData {
        static constexpr bool enableGpuDebug = true;

        static constexpr std::array<float, 4> frameColor{0.18F, 0.34F, 0.78F, 1.0F};
        static constexpr std::array<float, 4> setupColor{0.62F, 0.42F, 0.16F, 1.0F};
        static constexpr std::array<float, 4> transferColor{0.78F, 0.42F, 0.18F, 1.0F};
        static constexpr std::array<float, 4> renderColor{0.18F, 0.55F, 0.35F, 1.0F};
        static constexpr std::array<float, 4> drawColor{0.62F, 0.24F, 0.72F, 1.0F};
        static constexpr std::array<float, 4> barrierColor{0.48F, 0.48F, 0.48F, 1.0F};

    } debugData;

    // 深度附件格式与主贴图格式。
    static constexpr vk::Format depthFormat = vk::Format::eD32Sfloat;
    static constexpr vk::Format mainTextureFormat = vk::Format::eR8G8B8A8Unorm;

    static constexpr vk::ImageLayout colorAttachmentLayout = vk::ImageLayout::eAttachmentOptimal;
    static constexpr vk::ImageLayout depthAttachmentLayout = vk::ImageLayout::eAttachmentOptimal;

    static constexpr vk::PipelineStageFlags2 swapchainAcquireWaitStage =
        vk::PipelineStageFlagBits2::eColorAttachmentOutput;

    static constexpr vk::ComponentMapping identityComponentMapping{
        .r = vk::ComponentSwizzle::eIdentity,
        .g = vk::ComponentSwizzle::eIdentity,
        .b = vk::ComponentSwizzle::eIdentity,
        .a = vk::ComponentSwizzle::eIdentity,
    };

}; // class Application

using Vertex = util::PackedVertex;

struct DescriptorHeapDrawPushIndicesAlbedoAndNormal {
    std::uint32_t cameraIndex;
    std::uint32_t albedoTextureIndex;
    std::uint32_t normalTextureIndex;
};

struct DescriptorHeapDrawPushIndicesAlbedo {
    std::uint32_t cameraIndex;
    std::uint32_t albedoTextureIndex;
};

struct DescriptorHeapDrawPushIndicesSolidColor {
    std::uint32_t cameraIndex;
};

static_assert(offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, cameraIndex) == 0);
static_assert(offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, albedoTextureIndex) == sizeof(std::uint32_t));
static_assert(offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, normalTextureIndex) == sizeof(std::uint32_t) * 2U);

static_assert(offsetof(DescriptorHeapDrawPushIndicesAlbedo, cameraIndex) == 0);
static_assert(offsetof(DescriptorHeapDrawPushIndicesAlbedo, albedoTextureIndex) == sizeof(std::uint32_t));
static_assert(offsetof(DescriptorHeapDrawPushIndicesSolidColor, cameraIndex) == 0);
} // namespace siggraph
