// 模块说明：应用主体。负责窗口与 Vulkan 初始化、渲染循环、场景导入、相机控制以及 ImGui 调试界面。
#include "main.h"

#include <system_error>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int codePage);
inline constexpr unsigned int consoleCodePageUtf8 = 65001U;
#endif

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace siggraph {
namespace {

// 把 ImGui 后端的 VkResult 转成异常，避免错误被静默忽略。
void checkImGuiVulkanResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::format("ImGui Vulkan 后端调用失败：{}", util::vkResultName(result)));
    }
}

// 把面板里输入的路径解析成真实路径；相对路径按资源目录解析。
std::filesystem::path resolveScenePath(std::string_view input)
{
    std::filesystem::path path{input};
    if (path.is_relative()) {
        path = std::filesystem::path{NEXUS_FPS_ASSET_DIR} / path;
    }
    return path;
}

} // namespace

// 程序主流程：先创建窗口与常驻 Vulkan 资源，再进入主循环。
void Application::run()
{
    initGLFWWindow();

    initVulkanVKB();

    {
        initCommandPool();
        initFramesInFlightResources();
        initSwapchainImageSyncObjects();

        beginHelperCommands();
        initDepthImages();
        initEmptyScene();
        endHelperCommandsAndFlushUploads();

        initDescriptorHeaps();
        calculateVertexInputs();
        initShaderObjects();

        initImGui();
    }

    copyToImportPathBuffer(SceneData::defaultSceneRelativePath);

    mainLoop();
}

// 设置渲染帧数上限，主要用于自动化测试；0 表示不限制。
void Application::setFrameLimit(std::uint32_t frameLimit) { m_remainingFrameLimit = frameLimit; }

// 命令行 --import 的入口：把指定场景加入导入队列。
void Application::requestSceneImport(std::string gltfPath)
{
    const std::filesystem::path resolved = resolveScenePath(gltfPath);
    queueImport(std::span{&resolved, std::size_t{1}}, gltfPath);
}

// 创建 GLFW 窗口；禁用 OpenGL 上下文，并禁止缩放以简化交换链管理。
void Application::initGLFWWindow()
{
    util::log_msg("[初始化] 创建 GLFW 窗口");
    util::require(glfwInit() == GLFW_TRUE, "GLFW 初始化失败");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    m_window = glfwCreateWindow(windowWidth, windowHeight, "Nexus_FPS", nullptr, nullptr);
    util::require(m_window != nullptr, "创建 GLFW 窗口失败");
}

// 用 vk-bootstrap 创建实例、表面、物理设备、逻辑设备与交换链，并缓存描述符大小与堆对齐要求。
void Application::initVulkanVKB()
{
    util::log_msg("[初始化] 使用 vk-bootstrap 初始化 Vulkan");

    const auto vkbGetIfValid = [](auto&& result, const std::string_view operation) {

        if (!result) {
            std::string errorMessage = std::format("{} 失败：{}", operation, result.error().message());
            for (const std::string& reason : result.detailed_failure_reasons()) {
                errorMessage += "\n - " + reason;
            }
            throw std::runtime_error(errorMessage);
        }

        return std::move(result).value();
    };

    {

        const vkb::SystemInfo systemInfo =
            vkbGetIfValid(vkb::SystemInfo::get_system_info(), "查询 Vulkan 系统信息");

        auto instanceBuilder =
            vkb::InstanceBuilder()
                .set_app_name("Vulkan SIGGRAPH 教程")
                .set_engine_name("无引擎")  // Vulkan allows us to set an engine name and application name.
                .require_api_version(1, 4, 0); // Specify the minimum Vulkan API version.

        if (debugData.enableGpuDebug) {

            util::require(systemInfo.validation_layers_available,
                          "已启用 GPU 调试，但找不到 Vulkan 验证层");

            instanceBuilder.request_validation_layers(true).use_default_debug_messenger();
        }

        m_vkbData.m_instance = vkbGetIfValid(instanceBuilder.build(), "创建 Vulkan 实例");

        {
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_vkbData.m_instance.instance, vkGetInstanceProcAddr);
        }
    }

    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult result = glfwCreateWindowSurface(m_vkbData.m_instance.instance, m_window, nullptr, &surface);
        util::checkVk(result, "创建 GLFW Vulkan 表面");
        m_surface = vk::SurfaceKHR{surface};
    }

    {

        const std::array requiredExtensions{
            VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
            VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
            VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        };

        vkb::PhysicalDeviceSelector physicalDeviceSelector{m_vkbData.m_instance};

        {
            physicalDeviceSelector
                .add_required_extensions(requiredExtensions.size(), requiredExtensions.data())
                .set_surface(static_cast<VkSurfaceKHR>(m_surface))
                .set_minimum_version(1, 4)
                .add_required_extension_features(VkPhysicalDeviceVulkan14Features{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                    .maintenance5 = VK_TRUE,
                })
                .add_required_extension_features(VkPhysicalDeviceVulkan13Features{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                    .synchronization2 = VK_TRUE,
                    .dynamicRendering = VK_TRUE,
                })
                .add_required_extension_features(VkPhysicalDeviceVulkan12Features{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                    .bufferDeviceAddress = VK_TRUE,
                })
                .add_required_extension_features(VkPhysicalDeviceVulkan11Features{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                    .shaderDrawParameters = VK_TRUE,
                })
                .add_required_extension_features(VkPhysicalDeviceShaderObjectFeaturesEXT{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
                    .shaderObject = VK_TRUE,
                })
                .add_required_extension_features(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
                    .extendedDynamicState3DepthClampEnable = VK_TRUE,
                    .extendedDynamicState3PolygonMode = VK_TRUE,
                    .extendedDynamicState3RasterizationSamples = VK_TRUE,
                    .extendedDynamicState3SampleMask = VK_TRUE,
                    .extendedDynamicState3AlphaToCoverageEnable = VK_TRUE,
                    .extendedDynamicState3AlphaToOneEnable = VK_TRUE,
                    .extendedDynamicState3LogicOpEnable = VK_TRUE,
                    .extendedDynamicState3ColorBlendEnable = VK_TRUE,
                    .extendedDynamicState3ColorWriteMask = VK_TRUE,
                })
                .add_required_extension_features(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
                    .vertexInputDynamicState = VK_TRUE,
                })
                .add_required_extension_features(VkPhysicalDeviceDescriptorHeapFeaturesEXT{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
                    .descriptorHeap = VK_TRUE,
                });
        }

        std::optional<vkb::PhysicalDevice> selectedCandidate;
        {
            const std::vector<vkb::PhysicalDevice> physicalDeviceCandidates =
                vkbGetIfValid(physicalDeviceSelector.select_devices(), "选择 Vulkan 物理设备");

            for (const vkb::PhysicalDevice& vkbPhysicalDeviceCandidate : physicalDeviceCandidates) {
                bool valid = true;
                vk::PhysicalDevice physicalDeviceCandidate{vkbPhysicalDeviceCandidate.physical_device};

                {
                    const vk::FormatProperties2 formatProperties =
                        physicalDeviceCandidate.getFormatProperties2(mainTextureFormat);
                    const vk::FormatFeatureFlags requiredFeatures =
                        vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst |
                        vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
                    if ((formatProperties.formatProperties.optimalTilingFeatures & requiredFeatures) !=
                        requiredFeatures) {
                        valid = false;
                    }
                }
                if (valid) {
                    selectedCandidate = vkbPhysicalDeviceCandidate;
                    break;
                }
            }
            util::require(selectedCandidate.has_value(),
                          "没有支持为 R8G8B8A8_UNORM 纹理做线性 blit 生成 mipmap 的 Vulkan 物理设备");
        }

        vkb::PhysicalDevice physicalDevice = *selectedCandidate;
        const vk::PhysicalDevice selectedPhysicalDevice{physicalDevice.physical_device};

        {

            {
                const vk::PhysicalDeviceMemoryProperties2 memoryProperties =
                    selectedPhysicalDevice.getMemoryProperties2();
                m_memoryTypeFlags.clear();

                m_memoryTypeFlags.reserve(memoryProperties.memoryProperties.memoryTypeCount);

                for (std::uint32_t i = 0; i < memoryProperties.memoryProperties.memoryTypeCount; ++i) {
                    m_memoryTypeFlags.push_back(memoryProperties.memoryProperties.memoryTypes[i].propertyFlags);
                }
            }

            {
                vk::PhysicalDeviceDescriptorHeapPropertiesEXT heapProperties{};
                vk::PhysicalDeviceShaderObjectPropertiesEXT shaderObjectProperties{
                    .pNext = &heapProperties,
                };
                vk::PhysicalDeviceProperties2 properties2{
                    .pNext = &shaderObjectProperties,
                };
                selectedPhysicalDevice.getProperties2(&properties2);

                {
                    m_descriptorHeaps.m_sampledImageSize =
                        selectedPhysicalDevice.getDescriptorSizeEXT(vk::DescriptorType::eSampledImage);
                    m_descriptorHeaps.m_samplerSize =
                        selectedPhysicalDevice.getDescriptorSizeEXT(vk::DescriptorType::eSampler);
                    m_descriptorHeaps.m_uniformBufferSize =
                        selectedPhysicalDevice.getDescriptorSizeEXT(vk::DescriptorType::eUniformBuffer);
                    m_descriptorHeaps.m_storageBufferSize =
                        selectedPhysicalDevice.getDescriptorSizeEXT(vk::DescriptorType::eStorageBuffer);
                }

                {
                    m_descriptorHeaps.m_imageDescriptorAlignment = heapProperties.imageDescriptorAlignment;
                    m_descriptorHeaps.m_bufferDescriptorAlignment = heapProperties.bufferDescriptorAlignment;
                    m_descriptorHeaps.m_samplerDescriptorAlignment = heapProperties.samplerDescriptorAlignment;

                    m_descriptorHeaps.m_resourceHeap.m_alignment = heapProperties.resourceHeapAlignment;
                    m_descriptorHeaps.m_samplerHeap.m_alignment = heapProperties.samplerHeapAlignment;

                    m_descriptorHeaps.m_resourceHeap.m_minReservedRange = heapProperties.minResourceHeapReservedRange;
                    m_descriptorHeaps.m_samplerHeap.m_minReservedRange = heapProperties.minSamplerHeapReservedRange;

                    m_descriptorHeaps.m_maxPushDataSize = heapProperties.maxPushDataSize;

                    {
                        m_descriptorHeaps.m_uniformBufferStride = util::safeCastToU32(util::alignUp(
                            m_descriptorHeaps.m_uniformBufferSize, m_descriptorHeaps.m_bufferDescriptorAlignment));

                        m_descriptorHeaps.m_sampledImageStride = util::safeCastToU32(util::alignUp(
                            m_descriptorHeaps.m_sampledImageSize, m_descriptorHeaps.m_imageDescriptorAlignment));
                    }
                }

                {
                    std::copy(shaderObjectProperties.shaderBinaryUUID.begin(),
                              shaderObjectProperties.shaderBinaryUUID.end(),
                              m_shaderBinaryCacheProperties.m_shaderBinaryUUID.begin());
                    m_shaderBinaryCacheProperties.m_shaderBinaryVersion = shaderObjectProperties.shaderBinaryVersion;
                }
            }
        }

        {

            // 网格（线框）模式需要设备的 fillModeNonSolid 特性。设备支持才启用，
            // 不支持时界面会禁用这个选项，而不是让整个程序启动失败。
            VkPhysicalDeviceFeatures optionalFeatures{};
            optionalFeatures.fillModeNonSolid = VK_TRUE;
            m_wireframeSupported = physicalDevice.enable_features_if_present(optionalFeatures);
            util::log_msg("[初始化] 网格（线框）模式：{}", m_wireframeSupported ? "可用" : "当前设备不支持");

            m_vkbData.m_device =
                vkbGetIfValid(vkb::DeviceBuilder(physicalDevice).build(), "创建 Vulkan 逻辑设备");

            m_logicalDevice = vk::Device{m_vkbData.m_device.device};

            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_vkbData.m_instance.instance, vkGetInstanceProcAddr,
                                               m_vkbData.m_device.device, vkGetDeviceProcAddr);
        }
    }

    {

        m_graphicsQueue =
            vk::Queue{vkbGetIfValid(m_vkbData.m_device.get_queue(vkb::QueueType::graphics), "获取图形队列")};

        m_presentQueue =
            vk::Queue{vkbGetIfValid(m_vkbData.m_device.get_queue(vkb::QueueType::present), "获取呈现队列")};

        m_graphicsQueueFamily = vkbGetIfValid(m_vkbData.m_device.get_queue_index(vkb::QueueType::graphics),
                                              "获取图形队列族");

        const uint32_t presentQueueFamily =
            vkbGetIfValid(m_vkbData.m_device.get_queue_index(vkb::QueueType::present), "获取呈现队列族");

        util::require(m_graphicsQueueFamily == presentQueueFamily,
                      "本示例要求图形队列与呈现队列来自同一个队列族");
    }

    {

        const VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

        {
            vkb::SwapchainBuilder swapchainBuilder =
                vkb::SwapchainBuilder(m_vkbData.m_device)
                    .set_desired_extent(windowWidth, windowHeight)
                    .set_desired_present_mode(presentMode)
                    .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            m_vkbData.m_swapchain = vkbGetIfValid(swapchainBuilder.build(), "创建 Vulkan 交换链");
        }

        {
            m_swapchainFormat = vk::Format{m_vkbData.m_swapchain.image_format};
            m_swapchainExtent = vk::Extent2D{
                .width = m_vkbData.m_swapchain.extent.width,
                .height = m_vkbData.m_swapchain.extent.height,
            };
        }

        {
            const std::vector images = vkbGetIfValid(m_vkbData.m_swapchain.get_images(), "获取交换链图像");

            m_swapchainImages.reserve(images.size());

            for (VkImage image : images) {

                const vk::Image swapchainImage{image};

                const vk::ImageViewCreateInfo imageViewInfo{
                    .image = swapchainImage,
                    .viewType = vk::ImageViewType::e2D,
                    .format = m_swapchainFormat,
                    .components = identityComponentMapping,
                    .subresourceRange = CreateImageSubresourceRange(),
                };
                m_swapchainImages.emplace_back(SwapchainImageResources{
                    .m_image = swapchainImage,
                    .m_imageView = m_logicalDevice.createImageViewUnique(imageViewInfo),
                    .m_state = ImageState{.m_aspectMask = vk::ImageAspectFlagBits::eColor},
                });
                const std::size_t swapchainImageIndex = m_swapchainImages.size() - 1U;

                setDebugName(swapchainImage, vk::ObjectType::eImage,
                             std::format("SwapchainImage[{}]", swapchainImageIndex));
                setDebugName(*m_swapchainImages.back().m_imageView, vk::ObjectType::eImageView,
                             std::format("SwapchainImageView[{}]", swapchainImageIndex));
            }
        }
    }
}

// 初始化 Dear ImGui：加载界面字体、建立或恢复停靠布局，并创建 GLFW 与 Vulkan 两个后端。
void Application::initImGui()
{
    util::log_msg("[初始化] 初始化 Dear ImGui");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    {
        const std::filesystem::path fontPath = NEXUS_FPS_UI_FONT;
        if (std::filesystem::is_regular_file(fontPath)) {
            ImFont* const font = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 16.0F);
            util::require(font != nullptr, std::format("加载界面字体失败：{}", fontPath.string()));
            util::log_msg("[初始化] 界面字体：{}", fontPath.string());
        }
        else {
            util::log_msg("[初始化] 找不到字体 {}，改用 ImGui 内置字体", fontPath.string());
        }
    }

    {
        std::error_code iniError;
        const std::filesystem::path iniPath =
            (io.IniFilename != nullptr) ? std::filesystem::path{io.IniFilename} : std::filesystem::path{};
        m_needDefaultDockLayout = iniPath.empty() || !std::filesystem::exists(iniPath, iniError);
        util::log_msg("[初始化] ImGui 布局文件 {} {}", iniPath.string(), m_needDefaultDockLayout ? "不存在" : "已存在");
    }

    util::require(ImGui_ImplGlfw_InitForVulkan(m_window, true), "初始化 ImGui GLFW 后端失败");

    const VkFormat swapchainFormat = static_cast<VkFormat>(m_swapchainFormat);
    const VkFormat depthAttachmentFormat = static_cast<VkFormat>(depthFormat);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_4;
    initInfo.Instance = m_vkbData.m_instance.instance;
    initInfo.PhysicalDevice = m_vkbData.m_device.physical_device;
    initInfo.Device = m_vkbData.m_device.device;
    initInfo.QueueFamily = m_graphicsQueueFamily;
    initInfo.Queue = m_graphicsQueue;
    initInfo.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    initInfo.CheckVkResultFn = checkImGuiVulkanResult;

    initInfo.MinImageCount = maxFramesInFlight;
    initInfo.ImageCount = std::max(maxFramesInFlight, util::safeCastToU32(m_swapchainImages.size()));

    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = depthAttachmentFormat;

    util::require(ImGui_ImplVulkan_Init(&initInfo), "初始化 ImGui Vulkan 后端失败");

    m_imGuiInitialized = true;
}

// 创建命令池；主命令缓冲区与初始化用的辅助命令缓冲区都由它分配。
void Application::initCommandPool()
{

    util::log_msg("[初始化] 创建命令池");

    const vk::CommandPoolCreateInfo commandPoolInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = m_graphicsQueueFamily,
    };

    m_commandPool = m_logicalDevice.createCommandPoolUnique(commandPoolInfo);
    setDebugName(*m_commandPool, vk::ObjectType::eCommandPool, "MainCommandPool");
}

// 为每个并行帧创建命令缓冲区、相机缓冲、图像可用信号量与飞行围栏。
void Application::initFramesInFlightResources()
{

    util::log_msg("[初始化] 创建帧并行资源");

    const vk::SemaphoreCreateInfo semaphoreInfo{};

    const vk::FenceCreateInfo fenceInfo{
        .flags = vk::FenceCreateFlagBits::eSignaled,
    };

    const vk::CommandBufferAllocateInfo allocateInfo{
        .commandPool = *m_commandPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = util::safeCastToU32(m_framesInFlight.size() + 1),
    };

    const std::vector<vk::CommandBuffer> commandBuffers = m_logicalDevice.allocateCommandBuffers(allocateInfo);
    util::require(commandBuffers.size() == (m_framesInFlight.size() + 1),
                  "命令缓冲区数量应为「帧并行数 + 1 个辅助命令缓冲区」");

    for (std::size_t frameIndex = 0; frameIndex < m_framesInFlight.size(); ++frameIndex) {
        FrameInFlightResources& frame = m_framesInFlight[frameIndex];

        frame.m_commandBuffer = commandBuffers[frameIndex];

        frame.m_camera = createBuffer(
            sizeof(CameraData), vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            std::format("FrameCameraBuffer[{}]", frameIndex));

        frame.m_imageAvailableSemaphore = m_logicalDevice.createSemaphoreUnique(semaphoreInfo);

        frame.m_inFlightFence = m_logicalDevice.createFenceUnique(fenceInfo);

        {
            setDebugName(frame.m_commandBuffer, vk::ObjectType::eCommandBuffer,
                         std::format("FrameCommandBuffer[{}]", frameIndex));
            setDebugName(*frame.m_imageAvailableSemaphore, vk::ObjectType::eSemaphore,
                         std::format("ImageAvailableSemaphore[{}]", frameIndex));
            setDebugName(*frame.m_inFlightFence, vk::ObjectType::eFence,
                         std::format("FrameInFlightFence[{}]", frameIndex));
        }
    }

    m_helperCommandBuffer.m_commandBuffer = commandBuffers.back();
    m_helperCommandBuffer.m_active = false;
    setDebugName(m_helperCommandBuffer.m_commandBuffer, vk::ObjectType::eCommandBuffer, "HelperCommandBuffer");
}

// 为每张交换链图像创建渲染完成信号量，避免上一帧仍在使用时被复用。
void Application::initSwapchainImageSyncObjects()
{
    util::log_msg("[初始化] 创建交换链图像同步对象");

    const vk::SemaphoreCreateInfo semaphoreInfo{};

    for (std::uint32_t swapchainImageIndex = 0; swapchainImageIndex < m_swapchainImages.size(); ++swapchainImageIndex) {
        SwapchainImageResources& swapchainImage = m_swapchainImages[swapchainImageIndex];

        swapchainImage.m_renderFinishedSemaphore = m_logicalDevice.createSemaphoreUnique(semaphoreInfo);

        setDebugName(*swapchainImage.m_renderFinishedSemaphore, vk::ObjectType::eSemaphore,
                     std::format("RenderFinishedSemaphore[{}]", swapchainImageIndex));

        swapchainImage.m_state = ImageState{.m_aspectMask = vk::ImageAspectFlagBits::eColor};
    }
}

// 为每个并行帧创建深度图像与视图，供动态渲染的深度附件使用。
void Application::initDepthImages()
{
    util::log_msg("[初始化] 创建深度图像");

    const vk::CommandBuffer commandBuffer = getHelperCommandBuffer();

    for (std::uint32_t frameIndex = 0; frameIndex < m_framesInFlight.size(); ++frameIndex) {
        GpuViewImage& depthImage = m_framesInFlight[frameIndex].m_depthImage;

        {
            const vk::ImageCreateInfo imageInfo{
                .imageType = vk::ImageType::e2D,
                .format = depthFormat,
                .extent =
                    vk::Extent3D{.width = m_swapchainExtent.width, .height = m_swapchainExtent.height, .depth = 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
                .sharingMode = vk::SharingMode::eExclusive,
                .initialLayout = vk::ImageLayout::eUndefined,
            };
            depthImage.m_image.m_image = m_logicalDevice.createImageUnique(imageInfo);
            depthImage.m_image.m_states.resize(1, ImageState{.m_aspectMask = vk::ImageAspectFlagBits::eDepth});
            setDebugName(*depthImage.m_image.m_image, vk::ObjectType::eImage,
                         std::format("DepthImage[{}]", frameIndex));
        }

        allocateGpuImage(depthImage.m_image);

        {
            const vk::ImageViewCreateInfo viewInfo{
                .image = *depthImage.m_image.m_image,
                .viewType = vk::ImageViewType::e2D,
                .format = depthFormat,
                .subresourceRange = CreateImageSubresourceRange(0, vk::ImageAspectFlagBits::eDepth),
            };
            depthImage.m_imageView = m_logicalDevice.createImageViewUnique(viewInfo);
            setDebugName(*depthImage.m_imageView, vk::ObjectType::eImageView,
                         std::format("DepthImageView[{}]", frameIndex));
        }

        depthImage.m_image.transition(commandBuffer,
                                      ImageState{
                                          .m_layout = depthAttachmentLayout,
                                          .m_stageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
                                          .m_accessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                                          .m_aspectMask = vk::ImageAspectFlagBits::eDepth,
                                      });
    }
}

// 建立空场景：只创建渲染路径必需的占位缓冲，保证未导入场景时渲染依然合法。
void Application::initEmptyScene()
{
    util::log_msg("[初始化] 建立空场景（尚未导入任何场景）");

    const std::array placeholderObjects = {ObjectData{.model = glm::mat4{1.0F}}};
    initObjectBuffer(placeholderObjects);

    initSceneCamera(glm::vec3{0.0F, 2.0F, 6.0F}, glm::vec3{0.0F});

    initPointLight(glm::vec3{0.0F});
    initSolidColor();
}

// 扫描资源目录收集可导入的 glTF 文件，并刷新各预设场景的可用状态。
void Application::scanAvailableScenes()
{
    m_sceneListScanned = true;
    m_availableScenes.clear();

    for (std::size_t presetIndex = 0; presetIndex < SceneData::scenePresets.size(); ++presetIndex) {
        const SceneData::ScenePreset& preset = SceneData::scenePresets[presetIndex];
        m_presetAvailable[presetIndex] =
            std::all_of(preset.files.begin(), preset.files.end(), [](std::string_view relativeFile) {
                return std::filesystem::is_regular_file(resolveScenePath(relativeFile));
            });
    }

    const std::filesystem::path assetsDir = NEXUS_FPS_ASSET_DIR;
    std::error_code scanError;
    if (!std::filesystem::is_directory(assetsDir, scanError)) {
        util::log_msg("[导入] 找不到资源目录：{}", assetsDir.string());
        return;
    }

    std::filesystem::recursive_directory_iterator it(assetsDir, scanError);
    const std::filesystem::recursive_directory_iterator end;
    while (it != end && !scanError) {
        if (it->is_regular_file(scanError)) {
            const std::string extension = it->path().extension().string();
            if (extension == ".gltf" || extension == ".glb") {
                std::error_code relativeError;
                const std::filesystem::path relativePath =
                    std::filesystem::relative(it->path(), assetsDir, relativeError);
                m_availableScenes.push_back(relativeError ? it->path().string() : relativePath.string());
            }
        }
        it.increment(scanError);
    }

    std::sort(m_availableScenes.begin(), m_availableScenes.end());
    util::log_msg("[导入] 发现 {} 个可导入的 glTF 文件，位于 {}", m_availableScenes.size(),
                  assetsDir.string());
}

// 释放当前场景的 GPU 资源与描述符堆；未加载场景时调用也安全。
void Application::destroySceneResources()
{
    m_descriptorHeaps.m_samplerHeap.m_buffer.destroy(m_logicalDevice);
    m_descriptorHeaps.m_resourceHeap.m_buffer.destroy(m_logicalDevice);

    const auto resetHeapState = [](DescriptorHeapResources::DescriptorHeapData& heap) {
        heap.m_bindOffset = 0;
        heap.m_rangeSize = 0;
    };
    resetHeapState(m_descriptorHeaps.m_resourceHeap);
    resetHeapState(m_descriptorHeaps.m_samplerHeap);

    m_descriptorHeaps.m_cameraOffset = 0;
    m_descriptorHeaps.m_objectsOffset = 0;
    m_descriptorHeaps.m_pointLightOffset = 0;
    m_descriptorHeaps.m_solidColorOffset = 0;
    m_descriptorHeaps.m_pushTextureOffset = 0;
    m_descriptorHeaps.m_linearSamplerOffset = 0;
    m_descriptorHeaps.m_nearestSamplerOffset = 0;

    for (GpuImage& image : m_scene.m_textures) {
        image.destroy();
    }
    m_scene.m_textures.clear();

    for (GpuMesh& mesh : m_scene.m_meshes) {
        mesh.m_indices.destroy(m_logicalDevice);
        mesh.m_vertices.destroy(m_logicalDevice);
    }
    m_scene.m_meshes.clear();

    m_scene.m_objects.destroy(m_logicalDevice);
    m_scene.m_pointLight.destroy(m_logicalDevice);
    m_scene.m_solidColor.destroy(m_logicalDevice);
    m_scene.m_drawData.clear();
    m_scene = {};
}

// 导入一个场景：先解析全部 glTF 文件，再销毁旧场景、上传新资源并重建描述符堆。
void Application::importScene(std::span<const std::filesystem::path> gltfPaths)
{
    util::require(!gltfPaths.empty(), "导入场景时至少要有一个 glTF 文件");

    for (const std::filesystem::path& gltfPath : gltfPaths) {
        util::log_msg("[导入] 场景文件：{}", gltfPath.string());
        util::require(std::filesystem::is_regular_file(gltfPath),
                      std::format("找不到 glTF 文件：{}", gltfPath.string()));
    }

    const util::gltf::ParsedData gltfData = util::gltf::parseGltfFiles(gltfPaths);

    m_logicalDevice.waitIdle();
    destroySceneResources();
    m_sceneLoaded = false;

    try {
        buildSceneFromGltf(gltfData);
    }
    catch (const std::exception& error) {
        util::log_msg("[导入] 上传失败：{}", error.what());
        resetToEmptyScene();
        throw;
    }

    initDescriptorHeaps();

    m_sceneLoaded = true;
    util::log_msg("[导入] 已导入 {} 个网格、{} 次绘制、{} 张贴图", m_scene.m_meshes.size(),
                  m_scene.m_drawData.size(), m_scene.m_textures.size());
}

// 把解析结果上传到 GPU：网格、实例矩阵、光照、纯色数据与贴图。
void Application::buildSceneFromGltf(const util::gltf::ParsedData& gltfData)
{
    std::vector<ObjectData> objects;

    beginHelperCommands();
    try {
        util::log_msg("[导入] 正在上传场景网格");
        initSceneMeshesAndDrawData(gltfData, objects);

        if (gltfData.hasCamera) {
            initSceneCamera(gltfData.cameraPos, gltfData.cameraLookAt);
        }
        else {
            util::log_msg("[导入] glTF 没有相机，改用默认视角");
            initSceneCamera(glm::vec3{0.0F, 2.0F, 6.0F}, glm::vec3{0.0F});
        }

        initObjectBuffer(objects);
        initPointLight(gltfData.hasCamera ? gltfData.cameraLookAt : glm::vec3{0.0F});
        initSolidColor();
        initSceneTextures(gltfData);

        endHelperCommandsAndFlushUploads();
    }
    catch (...) {
        endHelperCommandsAndFlushUploads();
        throw;
    }
}

// 重建空场景，用于导入失败后把渲染状态恢复到合法的最小集合。
void Application::resetToEmptyScene()
{
    beginHelperCommands();
    initEmptyScene();
    endHelperCommandsAndFlushUploads();
    initDescriptorHeaps();
}

// 把待导入场景写入队列；真正的加载由主循环在两帧之间执行。
void Application::queueImport(std::span<const std::filesystem::path> gltfPaths, std::string_view label)
{
    m_pendingImportPaths.assign(gltfPaths.begin(), gltfPaths.end());
    m_pendingImportLabel = std::string{label};
}

// 把路径写入面板的可编辑缓冲；ImGui 需要可写且以 0 结尾的字符数组。
void Application::copyToImportPathBuffer(std::string_view path)
{
    const std::string text{path};
    const std::size_t count = std::min(text.size(), m_importPathBuffer.size() - 1);
    std::memcpy(m_importPathBuffer.data(), text.data(), count);
    m_importPathBuffer[count] = '\0';
}

// 为每个网格创建顶点/索引缓冲，并生成绘制列表与着色器变体选择。
void Application::initSceneMeshesAndDrawData(const util::gltf::ParsedData& gltfData, std::vector<ObjectData>& objects)
{

    objects.resize(gltfData.nodes.size());

    m_scene.m_meshes.reserve(gltfData.meshes.size());
    m_scene.m_drawData.reserve(gltfData.nodes.size());

    const std::uint32_t meshCount = util::safeCastToU32(gltfData.meshes.size());
    for (std::uint32_t meshId = 0; meshId < meshCount; ++meshId) {

        const util::MeshGeometryData geometry = util::gltf::buildMeshGeometryData(meshId, gltfData);

        const GpuMesh& gpuMesh = m_scene.m_meshes.emplace_back(GpuMesh{
            .m_vertices = uploadToNewGpuBuffer(std::as_bytes(std::span{geometry.vertices}),
                                               vk::BufferUsageFlagBits::eVertexBuffer,
                                               std::format("{} VertexBuffer", geometry.name)),
            .m_indices =
                uploadToNewGpuBuffer(std::as_bytes(std::span{geometry.indices}), vk::BufferUsageFlagBits::eIndexBuffer,
                                     std::format("{} IndexBuffer", geometry.name)),
            .m_indexCount = util::safeCastToU32(geometry.indices.size()),
        });

        {
            util::require(gpuMesh.m_indexCount > 0, std::format("{} 的网格没有任何索引", geometry.name));

            util::require(gpuMesh.m_indices.m_size ==
                              (static_cast<vk::DeviceSize>(gpuMesh.m_indexCount) * sizeof(std::uint32_t)),
                          std::format("{} 的索引缓冲区字节数与记录的索引数量不一致", geometry.name));

            util::require(*std::max_element(geometry.indices.begin(), geometry.indices.end()) <
                              geometry.vertices.size(),
                          std::format("{} 存在超出顶点范围的索引。", geometry.name));

            util::require((gpuMesh.m_vertices.m_size > 0) &&
                              (gpuMesh.m_vertices.m_size == (sizeof(Vertex) * geometry.vertices.size())),
                          std::format("{} 的网格顶点缓冲区大小无效：{}（{} × {}）", geometry.name,
                                      gpuMesh.m_vertices.m_size, sizeof(Vertex), geometry.vertices.size()));
        }
    }

    const std::uint32_t nodeCount = util::safeCastToU32(gltfData.nodes.size());
    for (std::uint32_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {

        const util::gltf::Node& node = gltfData.nodes[nodeIndex];

        objects[nodeIndex].model = util::math::generateModel(node.pos, node.eulerAngles, node.scale);

        util::require(node.meshId < m_scene.m_meshes.size(), "glTF 节点引用了无效的网格 id");

        const ShaderVariant shaderVariant =
            node.albedoTextureId == util::gltf::invalidGltfId
                ? ShaderVariant::SolidColor
                : (node.normalTextureId == util::gltf::invalidGltfId ? ShaderVariant::Albedo
                                                                     : ShaderVariant::AlbedoAndNormal);

        m_scene.m_drawData.push_back(SceneDraw{
            .m_meshId = node.meshId,
            .m_objectIndex = nodeIndex,
            .m_shaderVariant = shaderVariant,
            .m_albedoTextureIndex = node.albedoTextureId,
            .m_normalTextureIndex = node.normalTextureId,
            .m_debugName = node.name,
        });
    }
}

// 用相机位置与注视点初始化运行时自由相机，并同步到各帧的相机缓冲。
void Application::initSceneCamera(const glm::vec3& cameraPos, const glm::vec3& cameraLookAt)
{
    m_cameraPos = cameraPos;
    const auto [cameraYaw, cameraPitch] = util::math::calculateYawPitch(cameraPos, cameraLookAt);
    m_cameraYaw = cameraYaw;
    m_cameraPitch = cameraPitch;

    for (FrameInFlightResources& frame : m_framesInFlight) {
        uploadCameraData(frame);
    }
}

// 上传每个实例的模型矩阵；顶点着色器按 firstInstance 索引该缓冲。
void Application::initObjectBuffer(std::span<const ObjectData> objects)
{
    m_scene.m_objects = uploadToNewGpuBuffer(
        std::as_bytes(objects), vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        "SceneObjectBuffer");
}

// 上传硬编码的点光源位置、强度与环境光参数。
void Application::initPointLight(const glm::vec3& focusPoint)
{
    const glm::vec3 lightPosition = focusPoint + glm::vec3{0.0F, 4.0F, 2.0F};
    const LightData light{
        .pointPosition = lightPosition,
        .pointIntensity = 0.90F,
        .ambientColor = glm::vec3{1.0F, 0.92F, 0.78F},
        .ambientIntensity = 0.12F,
    };
    m_scene.m_pointLight =
        uploadToNewGpuBuffer(std::as_bytes(std::span{&light, std::size_t{1}}),
                             vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
                             "ScenePointLightBuffer");
}

// 上传纯色着色器使用的固定颜色（Vulkan 标志红）。
void Application::initSolidColor()
{
    const SolidColorData solidColor{
        .color = glm::vec4{util::sRgbToLinear(164, 30, 34), 1.0F},
    };
    m_scene.m_solidColor =
        uploadToNewGpuBuffer(std::as_bytes(std::span{&solidColor, std::size_t{1}}),
                             vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
                             "SceneSolidColorBuffer");
}

// 解码 glTF 引用的图像并上传为 GPU 贴图，同时生成 mipmap。
void Application::initSceneTextures(const util::gltf::ParsedData& gltfData)
{

    m_scene.m_textures.reserve(gltfData.textures.size());

    for (const util::gltf::GltfTexture& textureInfo : gltfData.textures) {
        util::log_msg("[初始化] 读取贴图：{}", textureInfo.filename);
        const util::ImageRgba8 texture = util::readImageFileRgba8(textureInfo.filename);

        m_scene.m_textures.push_back(createTexture(texture, textureInfo.name));
    }
    if (m_scene.m_textures.empty()) {
        util::log_msg("[导入] glTF 没有贴图，将使用纯色着色器");
    }
}

// 按固定布局计算描述符堆偏移，分配堆缓冲并写入缓冲、图像与采样器描述符。
void Application::initDescriptorHeaps()
{
    util::log_msg("[初始化] 创建描述符堆");

    {

        VkDeviceSize cursorResourceHeap = m_descriptorHeaps.m_resourceHeap.m_minReservedRange;

        {

            const auto cursorAllocateResourceDescriptorRange = [&cursorResourceHeap](VkDeviceSize size,
                                                                                     VkDeviceSize alignment) {
                cursorResourceHeap = util::alignUp(cursorResourceHeap, alignment);

                const std::uint32_t offset = util::safeCastToU32(cursorResourceHeap);

                cursorResourceHeap += size;

                return offset;
            };

            m_descriptorHeaps.m_cameraOffset = cursorAllocateResourceDescriptorRange(
                static_cast<VkDeviceSize>(m_descriptorHeaps.m_uniformBufferStride) * m_framesInFlight.size(),
                m_descriptorHeaps.m_bufferDescriptorAlignment);

            m_descriptorHeaps.m_objectsOffset = cursorAllocateResourceDescriptorRange(
                m_descriptorHeaps.m_storageBufferSize, m_descriptorHeaps.m_bufferDescriptorAlignment);
            m_descriptorHeaps.m_pointLightOffset = cursorAllocateResourceDescriptorRange(
                m_descriptorHeaps.m_uniformBufferSize, m_descriptorHeaps.m_bufferDescriptorAlignment);
            m_descriptorHeaps.m_solidColorOffset = cursorAllocateResourceDescriptorRange(
                m_descriptorHeaps.m_uniformBufferSize, m_descriptorHeaps.m_bufferDescriptorAlignment);

            m_descriptorHeaps.m_pushTextureOffset = cursorAllocateResourceDescriptorRange(
                static_cast<VkDeviceSize>(m_descriptorHeaps.m_sampledImageStride) * m_scene.m_textures.size(),
                m_descriptorHeaps.m_imageDescriptorAlignment);
        }

        m_descriptorHeaps.m_resourceHeap.m_rangeSize = cursorResourceHeap;
    }
    {

        VkDeviceAddress cursorSamplerHeap = m_descriptorHeaps.m_samplerHeap.m_minReservedRange;

        cursorSamplerHeap = util::alignUp(cursorSamplerHeap, m_descriptorHeaps.m_samplerDescriptorAlignment);

        m_descriptorHeaps.m_linearSamplerOffset = util::safeCastToU32(cursorSamplerHeap);

        cursorSamplerHeap = util::alignUp(cursorSamplerHeap + m_descriptorHeaps.m_samplerSize,
                                          m_descriptorHeaps.m_samplerDescriptorAlignment);

        m_descriptorHeaps.m_nearestSamplerOffset = util::safeCastToU32(cursorSamplerHeap);

        m_descriptorHeaps.m_samplerHeap.m_rangeSize = cursorSamplerHeap + m_descriptorHeaps.m_samplerSize;
    }

    {

        auto allocateAlignedBuffer = [this](DescriptorHeapResources::DescriptorHeapData& descriptorHeapData,
                                            const std::string& debugName) {

            const vk::BufferUsageFlags heapUsage{vk::BufferUsageFlagBits::eDescriptorHeapEXT |
                                                 vk::BufferUsageFlagBits::eShaderDeviceAddress};

            const VkDeviceSize alignedSize =
                util::alignedAllocationSize(descriptorHeapData.m_rangeSize, descriptorHeapData.m_alignment);

            const vk::MemoryPropertyFlags memoryProperties =
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;

            descriptorHeapData.m_buffer = createBuffer(alignedSize, heapUsage, memoryProperties, debugName);

            descriptorHeapData.m_bindOffset =
                util::alignedOffset(descriptorHeapData.m_buffer.m_addressGPU, descriptorHeapData.m_alignment);

            {

                util::require(util::alignUp(descriptorHeapData.m_buffer.m_addressGPU + descriptorHeapData.m_bindOffset,
                                            descriptorHeapData.m_alignment) ==
                                  descriptorHeapData.m_buffer.m_addressGPU + descriptorHeapData.m_bindOffset,
                              "描述符堆绑定地址未对齐");

                util::require(descriptorHeapData.m_bindOffset + descriptorHeapData.m_rangeSize <=
                                  descriptorHeapData.m_buffer.m_size,
                              "对齐后的资源堆范围放不进已分配的缓冲区");
            }
        };

        {
            allocateAlignedBuffer(m_descriptorHeaps.m_resourceHeap, "ResourceDescriptorHeap");
            allocateAlignedBuffer(m_descriptorHeaps.m_samplerHeap, "SamplerDescriptorHeap");
        }
    }

    {

        std::vector<vk::ResourceDescriptorInfoEXT> resources;
        std::vector<vk::HostAddressRangeEXT> descriptorRanges;
        std::vector<vk::DeviceAddressRangeEXT> bufferRanges;
        std::vector<vk::ImageViewCreateInfo> imageViewInfos;
        std::vector<vk::ImageDescriptorInfoEXT> imageInfos;

        constexpr std::size_t fixedSceneBufferDescriptorCount = 3;

        const std::size_t cameraDescriptorCount = m_framesInFlight.size();
        const std::size_t totalBufferDescriptorCount = cameraDescriptorCount + fixedSceneBufferDescriptorCount;
        const std::size_t totalImageDescriptorCount = m_scene.m_textures.size();
        const std::size_t totalDescriptorCount = totalBufferDescriptorCount + totalImageDescriptorCount;

        {

            resources.reserve(totalDescriptorCount);
            descriptorRanges.reserve(totalDescriptorCount);

            bufferRanges.reserve(totalBufferDescriptorCount);

            imageViewInfos.reserve(totalImageDescriptorCount);
            imageInfos.reserve(totalImageDescriptorCount);
        }

        auto emplaceNoRealloc = [](auto& vec, auto&&... args) -> auto& {
            util::require(vec.size() < vec.capacity(), "容器超出预留容量");
            return vec.emplace_back(std::forward<decltype(args)>(args)...);
        };

        {
            util::require(m_descriptorHeaps.m_resourceHeap.m_buffer.m_addressCPU != nullptr,
                          "写入描述符前，资源描述符堆必须已映射到 CPU");

            std::byte* const resourceHeapBase =
                static_cast<std::byte*>(m_descriptorHeaps.m_resourceHeap.m_buffer.m_addressCPU) +
                m_descriptorHeaps.m_resourceHeap.m_bindOffset;

            auto addBufferDescriptor = [&bufferRanges, &descriptorRanges, &resources, emplaceNoRealloc,
                                        resourceHeapBase](vk::DescriptorType type, const GpuBuffer& buffer,
                                                          vk::DeviceSize bufferOffset, vk::DeviceSize size,
                                                          std::uint32_t heapOffset, vk::DeviceSize descriptorSize) {
                {
                    util::require(buffer.m_addressGPU != 0, "缓冲区描述符需要 GPU 地址");
                    util::require(bufferOffset <= buffer.m_size && size <= buffer.m_size - bufferOffset,
                                  "缓冲区描述符范围超出缓冲区");
                }

                {
                    vk::DeviceAddressRangeEXT& bufferRange = emplaceNoRealloc(
                        bufferRanges,
                        vk::DeviceAddressRangeEXT{.address = buffer.m_addressGPU + bufferOffset, .size = size});

                    vk::ResourceDescriptorInfoEXT& descriptor =
                        emplaceNoRealloc(resources, vk::ResourceDescriptorInfoEXT{
                                                        .type = type,
                                                    });
                    descriptor.data.pAddressRange = &bufferRange;

                    emplaceNoRealloc(descriptorRanges, vk::HostAddressRangeEXT{
                                                           .address = resourceHeapBase + heapOffset,
                                                           .size = static_cast<std::size_t>(descriptorSize),
                                                       });
                }
            };

            const std::uint32_t frameCount = util::safeCastToU32(m_framesInFlight.size());
            for (std::uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {

                std::uint32_t uboWriteHeapOffset =
                    m_descriptorHeaps.m_cameraOffset + frameIndex * m_descriptorHeaps.m_uniformBufferStride;

                addBufferDescriptor(vk::DescriptorType::eUniformBuffer, m_framesInFlight[frameIndex].m_camera, 0,
                                    sizeof(CameraData), uboWriteHeapOffset, m_descriptorHeaps.m_uniformBufferSize);
            }

            addBufferDescriptor(vk::DescriptorType::eStorageBuffer, m_scene.m_objects, 0, m_scene.m_objects.m_size,
                                m_descriptorHeaps.m_objectsOffset, m_descriptorHeaps.m_storageBufferSize);

            addBufferDescriptor(vk::DescriptorType::eUniformBuffer, m_scene.m_pointLight, 0, sizeof(LightData),
                                m_descriptorHeaps.m_pointLightOffset, m_descriptorHeaps.m_uniformBufferSize);

            addBufferDescriptor(vk::DescriptorType::eUniformBuffer, m_scene.m_solidColor, 0, sizeof(SolidColorData),
                                m_descriptorHeaps.m_solidColorOffset, m_descriptorHeaps.m_uniformBufferSize);

            const std::uint32_t textureCount = util::safeCastToU32(m_scene.m_textures.size());
            for (std::uint32_t i = 0; i < textureCount; ++i) {

                vk::ImageViewCreateInfo& imageViewInfo = emplaceNoRealloc(
                    imageViewInfos, vk::ImageViewCreateInfo{
                                        .image = *m_scene.m_textures[i].m_image,
                                        .viewType = vk::ImageViewType::e2D,
                                        .format = mainTextureFormat,
                                        .components = identityComponentMapping,
                                        .subresourceRange = CreateImageSubresourceRange(
                                            0, vk::ImageAspectFlagBits::eColor, m_scene.m_textures[i].m_mipLevels),
                                    });

                vk::ImageDescriptorInfoEXT& imageInfo =
                    emplaceNoRealloc(imageInfos, vk::ImageDescriptorInfoEXT{
                                                     .pView = &imageViewInfo,
                                                     .layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                                 });

                vk::ResourceDescriptorInfoEXT& descriptor =
                    emplaceNoRealloc(resources, vk::ResourceDescriptorInfoEXT{
                                                    .type = vk::DescriptorType::eSampledImage,
                                                });
                descriptor.data.pImage = &imageInfo;

                const std::uint32_t imageWriteHeapOffset =
                    m_descriptorHeaps.m_pushTextureOffset + i * m_descriptorHeaps.m_sampledImageStride;
                emplaceNoRealloc(descriptorRanges,
                                 vk::HostAddressRangeEXT{
                                     .address = resourceHeapBase + imageWriteHeapOffset,
                                     .size = static_cast<std::size_t>(m_descriptorHeaps.m_sampledImageSize),
                                 });
            }
            util::require(resources.size() == descriptorRanges.size(), "资源描述与地址范围的数量必须一致");

            util::checkVk(static_cast<VkResult>(m_logicalDevice.writeResourceDescriptorsEXT(
                              util::safeCastToU32(resources.size()), resources.data(), descriptorRanges.data())),
                          "写入资源堆描述符");
        }
    }

    {
        util::require(m_descriptorHeaps.m_samplerHeap.m_buffer.m_addressCPU != nullptr,
                      "写入描述符前，采样器描述符堆必须已映射到 CPU");

        std::array samplerInfo = {
            vk::SamplerCreateInfo{
                .magFilter = vk::Filter::eLinear,
                .minFilter = vk::Filter::eLinear,
                .mipmapMode = vk::SamplerMipmapMode::eLinear,
                .addressModeU = vk::SamplerAddressMode::eRepeat,
                .addressModeV = vk::SamplerAddressMode::eRepeat,
                .addressModeW = vk::SamplerAddressMode::eRepeat,
                .maxLod = VK_LOD_CLAMP_NONE,
            },
            vk::SamplerCreateInfo{
                .magFilter = vk::Filter::eNearest,
                .minFilter = vk::Filter::eNearest,
                .mipmapMode = vk::SamplerMipmapMode::eNearest,
                .addressModeU = vk::SamplerAddressMode::eRepeat,
                .addressModeV = vk::SamplerAddressMode::eRepeat,
                .addressModeW = vk::SamplerAddressMode::eRepeat,
                .maxLod = VK_LOD_CLAMP_NONE,
            }};

        std::array samplerRange = {
            vk::HostAddressRangeEXT{
                .address = static_cast<std::byte*>(m_descriptorHeaps.m_samplerHeap.m_buffer.m_addressCPU) +
                           m_descriptorHeaps.m_samplerHeap.m_bindOffset + m_descriptorHeaps.m_linearSamplerOffset,
                .size = static_cast<std::size_t>(m_descriptorHeaps.m_samplerSize),
            },
            vk::HostAddressRangeEXT{
                .address = static_cast<std::byte*>(m_descriptorHeaps.m_samplerHeap.m_buffer.m_addressCPU) +
                           m_descriptorHeaps.m_samplerHeap.m_bindOffset + m_descriptorHeaps.m_nearestSamplerOffset,
                .size = static_cast<std::size_t>(m_descriptorHeaps.m_samplerSize),
            }};

        util::require(samplerInfo.size() == samplerRange.size(), "采样器描述与地址范围的数量必须一致");
        util::checkVk(static_cast<VkResult>(m_logicalDevice.writeSamplerDescriptorsEXT(
                          util::safeCastToU32(samplerInfo.size()), samplerInfo.data(), samplerRange.data())),
                      "写入采样器堆描述符");
    }
}

// 把 Slang 反射得到的 set/binding 映射为描述符堆中的字节偏移。
std::vector<vk::DescriptorSetAndBindingMappingEXT> Application::buildShaderDescriptorMappings(
    const std::unordered_map<std::string, util::slang::ShaderResourceBinding>& shaderResourceBindings) const
{

    std::vector<vk::DescriptorSetAndBindingMappingEXT> mappings;
    mappings.reserve(shaderResourceBindings.size());

    std::vector<std::string> validateBindingNames;
    std::vector<std::pair<uint32_t, uint32_t>> validateBindingId;

    const auto makeDescriptorHeapMapping = [](const util::slang::ShaderResourceBinding& binding,
                                              vk::DescriptorMappingSourceEXT source,
                                              const vk::DescriptorMappingSourceDataEXT& sourceData) {

        return vk::DescriptorSetAndBindingMappingEXT{
            .descriptorSet = binding.set,
            .firstBinding = binding.binding,
            .bindingCount = 1,
            .resourceMask = vk::SpirvResourceTypeFlagsEXT{binding.resourceMask},
            .source = source,
            .sourceData = sourceData,
        };
    };

    const auto makeCombinedImageSamplerPushIndexSourceData = [pushTextureOffset = m_descriptorHeaps.m_pushTextureOffset,
                                                              sampledImageStride =
                                                                  m_descriptorHeaps.m_sampledImageStride,
                                                              samplerOffset = m_descriptorHeaps.m_linearSamplerOffset,
                                                              samplerStride = m_descriptorHeaps.m_nearestSamplerOffset -
                                                                              m_descriptorHeaps.m_linearSamplerOffset](
                                                                 std::uint32_t pushOffset) {

        util::require((pushOffset % 4) == 0, std::format("推送数据偏移 {} 必须按 4 字节对齐", pushOffset));

        return vk::DescriptorMappingSourceDataEXT{vk::DescriptorMappingSourcePushIndexEXT{
            .heapOffset = pushTextureOffset,
            .pushOffset = pushOffset,
            .heapIndexStride = sampledImageStride,
            .heapArrayStride = 0,
            .pEmbeddedSampler = nullptr,
            .useCombinedImageSamplerIndex = vk::True,
            .samplerHeapOffset = samplerOffset,
            .samplerPushOffset = 0, // Unused since we are using useCombinedImageSamplerIndex.
            .samplerHeapIndexStride = samplerStride,
            .samplerHeapArrayStride = 0, // Unused since we only have single-binding scalar descriptors.
        }};
    };

    for (const auto& bindingIt : shaderResourceBindings) {

        {

            auto nameIt = std::find(validateBindingNames.begin(), validateBindingNames.end(), bindingIt.first);
            util::require(nameIt == validateBindingNames.end(),
                          std::format("着色器资源绑定名称重复：{}", bindingIt.first));
            validateBindingNames.push_back(bindingIt.first);

            auto idPair = std::make_pair(bindingIt.second.set, bindingIt.second.binding);
            auto idIt = std::find(validateBindingId.begin(), validateBindingId.end(), idPair);
            util::require(idIt == validateBindingId.end(),
                          std::format("着色器资源绑定重复：集合 {} 绑定点 {}", bindingIt.second.set,
                                      bindingIt.second.binding));
            validateBindingId.push_back(idPair);
        }

        if (bindingIt.first == "g_camera") {
            const vk::DescriptorMappingSourceDataEXT cameraSourceData{vk::DescriptorMappingSourcePushIndexEXT{
                .heapOffset = m_descriptorHeaps.m_cameraOffset,
                .pushOffset = util::safeCastToU32(offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, cameraIndex)),
                .heapIndexStride = m_descriptorHeaps.m_uniformBufferStride,
            }};

            static_assert(
                offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, cameraIndex) ==
                    offsetof(DescriptorHeapDrawPushIndicesAlbedo, cameraIndex) &&
                offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, cameraIndex) ==
                    offsetof(DescriptorHeapDrawPushIndicesSolidColor, cameraIndex) &&
                "相机索引在所有推送数据结构中的偏移必须一致。这样本示例检测偏移时"
                "会更简单");

            mappings.push_back(makeDescriptorHeapMapping(
                bindingIt.second, vk::DescriptorMappingSourceEXT::eHeapWithPushIndex, cameraSourceData));
        }
        else if (bindingIt.first == "g_objects") {
            const vk::DescriptorMappingSourceDataEXT objectsSourceData{vk::DescriptorMappingSourceConstantOffsetEXT{
                .heapOffset = m_descriptorHeaps.m_objectsOffset,
            }};
            mappings.push_back(makeDescriptorHeapMapping(
                bindingIt.second, vk::DescriptorMappingSourceEXT::eHeapWithConstantOffset, objectsSourceData));
        }
        else if (bindingIt.first == "g_pointLight") {
            const vk::DescriptorMappingSourceDataEXT pointLightSourceData{vk::DescriptorMappingSourceConstantOffsetEXT{
                .heapOffset = m_descriptorHeaps.m_pointLightOffset,
            }};
            mappings.push_back(makeDescriptorHeapMapping(
                bindingIt.second, vk::DescriptorMappingSourceEXT::eHeapWithConstantOffset, pointLightSourceData));
        }
        else if (bindingIt.first == "g_solidColor") {
            const vk::DescriptorMappingSourceDataEXT solidColorSourceData{vk::DescriptorMappingSourceConstantOffsetEXT{
                .heapOffset = m_descriptorHeaps.m_solidColorOffset,
            }};
            mappings.push_back(makeDescriptorHeapMapping(
                bindingIt.second, vk::DescriptorMappingSourceEXT::eHeapWithConstantOffset, solidColorSourceData));
        }
        else if (bindingIt.first == "g_albedoTexture") {

            static_assert(offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, albedoTextureIndex) ==
                              offsetof(DescriptorHeapDrawPushIndicesAlbedo, albedoTextureIndex),
                          "反照率贴图索引在所有反照率推送数据结构中的偏移必须一致");
            const vk::DescriptorMappingSourceDataEXT albedoTextureSourceData =
                makeCombinedImageSamplerPushIndexSourceData(
                    util::safeCastToU32(offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, albedoTextureIndex)));

            mappings.push_back(makeDescriptorHeapMapping(
                bindingIt.second, vk::DescriptorMappingSourceEXT::eHeapWithPushIndex, albedoTextureSourceData));
        }

        else {
            util::require(bindingIt.first == "g_normalTexture",
                          std::format("意外的着色器资源绑定名称：{}", bindingIt.first));
            const vk::DescriptorMappingSourceDataEXT normalTextureSourceData =
                makeCombinedImageSamplerPushIndexSourceData(
                    util::safeCastToU32(offsetof(DescriptorHeapDrawPushIndicesAlbedoAndNormal, normalTextureIndex)));

            mappings.push_back(makeDescriptorHeapMapping(
                bindingIt.second, vk::DescriptorMappingSourceEXT::eHeapWithPushIndex, normalTextureSourceData));
        }
    }

    return mappings;
}

// 从 Slang 反射读取顶点属性位置与格式，供动态顶点输入状态使用。
void Application::calculateVertexInputs()
{

    util::log_msg("[初始化] 计算顶点输入布局");

    const std::filesystem::path reflectionPath{
        std::filesystem::path{NEXUS_FPS_SHADER_DIR} /
        std::format("{}.json", SceneData::albedoAndNormalShaderFilePair.vertexName)};

    const util::slang::PackedVertexInputLayout layout = util::slang::calculatePackedVertexInputLayout(reflectionPath);

    m_vertexInput.m_vertexBindings = layout.bindings;

    m_vertexInput.m_vertexAttributes = layout.attributes;
}

// 创建三套着色器对象：反照率+法线贴图、仅反照率、纯色。
void Application::initShaderObjects()
{

    util::log_msg("[初始化] 创建着色器对象");

    m_shaderObjects.m_albedoAndNormal = createShaderGroup(SceneData::albedoAndNormalShaderFilePair);
    m_shaderObjects.m_albedo = createShaderGroup(SceneData::albedoShaderFilePair);
    m_shaderObjects.m_solidColor = createShaderGroup(SceneData::solidColorShaderFilePair);
}

// 读取 SPIR-V（优先使用着色器二进制缓存）并创建一对顶点/片段着色器对象。
Application::ShaderGroup Application::createShaderGroup(const ShaderFilePair& shaderFilePair)
{

    const std::filesystem::path shaderDir = NEXUS_FPS_SHADER_DIR;

    std::vector<vk::DescriptorSetAndBindingMappingEXT> mappings;
    {

        const std::array shaderReflectionPaths{
            shaderDir / std::format("{}.json", shaderFilePair.vertexName),
            shaderDir / std::format("{}.json", shaderFilePair.fragmentName),
        };

        const std::unordered_map<std::string, util::slang::ShaderResourceBinding>& shaderResourceBindings =
            util::slang::collectShaderResourceBindings(shaderReflectionPaths);

        mappings = buildShaderDescriptorMappings(shaderResourceBindings);
    }

    const vk::ShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{
        .mappingCount = util::safeCastToU32(mappings.size()),
        .pMappings = mappings.data(),
    };

    vk::ShaderCreateFlagsEXT shaderFlags = vk::ShaderCreateFlagBitsEXT::eDescriptorHeap;

    constexpr bool enable_linking = true;
    if constexpr (enable_linking) {
        shaderFlags |= vk::ShaderCreateFlagBitsEXT::eLinkStage;
    }

    constexpr std::size_t numShadersInPipeline = 2; // Vertex + fragment.

    std::vector<util::BinaryBuffer> currentShadersCode;
    std::vector<vk::ShaderCreateInfoEXT> createInfos;

    currentShadersCode.reserve(numShadersInPipeline);
    createInfos.reserve(numShadersInPipeline);

    const std::filesystem::path vertexSpirvPath = shaderDir / shaderFilePair.vertexName;
    const std::filesystem::path fragmentSpirvPath = shaderDir / shaderFilePair.fragmentName;
    util::BinaryBuffer vertexSpirvWords = util::readSpirvFile(vertexSpirvPath);
    util::BinaryBuffer fragmentSpirvWords = util::readSpirvFile(fragmentSpirvPath);

    constexpr bool enable_shader_binary_cache = true;

    const std::filesystem::path shaderBinaryCacheDir = shaderDir / "shaderBinaryCache";

    struct ShaderCacheMiss {
        std::filesystem::path cachePath;
        std::uint32_t shaderIndex;
    };

    std::vector<ShaderCacheMiss> cacheMisses;

    std::uint64_t shaderPairCacheKey = 0;

    if (enable_shader_binary_cache) {

        constexpr std::uint32_t shaderBinaryCacheVersion = 1;

        shaderPairCacheKey = util::combineHash(
            shaderBinaryCacheVersion, std::span<const std::uint8_t>{m_shaderBinaryCacheProperties.m_shaderBinaryUUID},
            m_shaderBinaryCacheProperties.m_shaderBinaryVersion);

        if (enable_linking) {
            shaderPairCacheKey = util::combineHash(vertexSpirvWords.as_byte_span(), fragmentSpirvWords.as_byte_span(),
                                                   shaderPairCacheKey);
        }

        {
            shaderPairCacheKey =
                util::combineHash(shaderPairCacheKey, m_descriptorHeaps.m_resourceHeap.m_minReservedRange,
                                  m_descriptorHeaps.m_samplerHeap.m_minReservedRange, m_descriptorHeaps.m_cameraOffset,
                                  m_descriptorHeaps.m_uniformBufferStride, m_descriptorHeaps.m_objectsOffset,
                                  m_descriptorHeaps.m_pointLightOffset, m_descriptorHeaps.m_solidColorOffset,
                                  m_descriptorHeaps.m_pushTextureOffset, m_descriptorHeaps.m_sampledImageStride,
                                  m_descriptorHeaps.m_nearestSamplerOffset, m_descriptorHeaps.m_linearSamplerOffset);
        }
    }

    const auto addShaderCreateInfo = [&cacheMisses, &createInfos, &currentShadersCode, &mappingInfo,
                                      shaderBinaryCacheDir, shaderFlags, shaderPairCacheKey](
                                         std::string_view shaderName,
                                         vk::ShaderStageFlagBits stage, std::span<const std::byte> spirvBytes) {
        static_cast<void>(shaderPairCacheKey);
        std::optional<std::span<const std::byte>> shaderCode;

        vk::ShaderCodeTypeEXT codeType = vk::ShaderCodeTypeEXT::eSpirv;

        if (enable_shader_binary_cache) {

            uint64_t shaderBinaryCacheKey = enable_linking ? util::combineHash(stage, shaderPairCacheKey)
                                                           : util::combineHash(spirvBytes, shaderPairCacheKey);

            const std::filesystem::path shaderCachePath =
                shaderBinaryCacheDir / std::format("{:016x}.bin", shaderBinaryCacheKey);

            std::size_t shaderBinaryAlignment = 16;

            std::optional<util::BinaryBuffer> cachedShaderBinary =
                util::readBinaryFile(shaderCachePath, shaderBinaryAlignment);

            const bool hasShaderBinary = cachedShaderBinary.has_value();

            if (hasShaderBinary) {
                util::log_msg("[初始化] 读取缓存的着色器二进制：{}", shaderCachePath.string());

                shaderCode = currentShadersCode.emplace_back(std::move(cachedShaderBinary.value())).as_byte_span();

                codeType = vk::ShaderCodeTypeEXT::eBinary;
            }
            else {
                cacheMisses.push_back(ShaderCacheMiss{
                    .cachePath = shaderCachePath,
                    .shaderIndex = util::safeCastToU32(createInfos.size()),
                });
            }
        }

        if (!shaderCode.has_value()) {
            util::log_msg("[初始化] 读取 SPIR-V 着色器：{}", shaderName);

            util::require(!spirvBytes.empty(), "生成着色器键之前必须先加载 SPIR-V");

            util::require(codeType == vk::ShaderCodeTypeEXT::eSpirv, "SPIR-V 着色器代码必须使用 eSpirv 代码类型");

            shaderCode = spirvBytes;
        }

        {
            util::require(reinterpret_cast<std::size_t>(shaderCode->data()) ==
                              util::alignUp(reinterpret_cast<std::size_t>(shaderCode->data()),
                                            codeType == vk::ShaderCodeTypeEXT::eSpirv ? size_t(4) : size_t(16)),
                          "创建着色器对象时，二进制对象要求代码按 16 字节对齐，"
                          "SPIR-V 则要求按 4 字节对齐");
        }

        createInfos.emplace_back(vk::ShaderCreateInfoEXT{
            .pNext = (codeType == vk::ShaderCodeTypeEXT::eSpirv) ? &mappingInfo : nullptr,
            .flags = shaderFlags,
            .stage = stage,
            .nextStage = (stage == vk::ShaderStageFlagBits::eVertex) ? vk::ShaderStageFlagBits::eFragment
                                                                     : vk::ShaderStageFlagBits(0),
            .codeType = codeType,
            .codeSize = shaderCode->size(),
            .pCode = shaderCode->data(),
            .pName = "main",
        });
    };

    addShaderCreateInfo(shaderFilePair.vertexName, vk::ShaderStageFlagBits::eVertex, vertexSpirvWords.as_byte_span());
    addShaderCreateInfo(shaderFilePair.fragmentName, vk::ShaderStageFlagBits::eFragment,
                        fragmentSpirvWords.as_byte_span());

    util::log_msg("[初始化] 创建着色器对象：{} + {}", shaderFilePair.vertexName, shaderFilePair.fragmentName);

    if (enable_linking) {
        util::require(
            cacheMisses.empty() || (createInfos.size() == cacheMisses.size()),
            std::format("着色器 {} 和 {} 的缓存有问题。所有着色器都应为二进制或 SPIR-V。"
                        "可以考虑删除着色器缓存。",
                        shaderFilePair.vertexName, shaderFilePair.fragmentName));
    }

    auto result = m_logicalDevice.createShadersEXTUnique(createInfos);

    util::require(result.has_value(),
                  std::format("为 {} 和 {} 创建着色器对象失败：{}", shaderFilePair.vertexName,
                              shaderFilePair.fragmentName, vk::to_string(result.result)));

    std::vector<vk::UniqueShaderEXT> shaders = std::move(result.value);
    util::require(shaders.size() == 2, "期望得到已链接的顶点与片段着色器对象");

    if (enable_shader_binary_cache) {

        for (const ShaderCacheMiss& cacheMiss : cacheMisses) {

            const std::vector<std::uint8_t> shaderBinary =
                m_logicalDevice.getShaderBinaryDataEXT(*shaders[cacheMiss.shaderIndex]);

            util::writeBinaryFile(cacheMiss.cachePath, shaderBinary);

            util::log_msg("[初始化] 写入着色器二进制缓存：{}", cacheMiss.cachePath.filename().string());
        }
    }

    ShaderGroup shaderGroup{
        .m_vertex = std::move(shaders[0]),
        .m_fragment = std::move(shaders[1]),
    };

    {
        const std::string vertexDebugName =
            std::format("Vertex Shader ({} + {})", shaderFilePair.vertexName, shaderFilePair.fragmentName);
        setDebugName(*shaderGroup.m_vertex, vk::ObjectType::eShaderEXT, vertexDebugName);

        const std::string fragmentDebugName =
            std::format("Fragment Shader ({} + {})", shaderFilePair.vertexName, shaderFilePair.fragmentName);
        setDebugName(*shaderGroup.m_fragment, vk::ObjectType::eShaderEXT, fragmentDebugName);
    }
    return shaderGroup;
}

// 主循环：处理输入、更新界面、按需导入场景，并提交每一帧的渲染。
void Application::mainLoop()
{

    double previousTime = glfwGetTime();
    double lastFpsLogTime = previousTime;

    constexpr uint32_t framesPerLog = 20;
    uint32_t remainingFramesLog = framesPerLog;

    if (m_remainingFrameLimit > 0) {
        util::log_msg("[运行] 帧数上限：{}", m_remainingFrameLimit);
    }

    while (glfwWindowShouldClose(m_window) == GLFW_FALSE) {

        const double currentTime = glfwGetTime();
        const float deltaSeconds = static_cast<float>(currentTime - previousTime);

        util::require(deltaSeconds >= 0.0f, std::format("帧间时间差 {} 无效", deltaSeconds));

        previousTime = currentTime;

        if (--remainingFramesLog == 0) {
            const float elapsedSeconds = static_cast<float>(currentTime - lastFpsLogTime);

            util::require(elapsedSeconds > 0.0f, std::format("经过的秒数 {} 无效", elapsedSeconds));

            const float fps = framesPerLog / elapsedSeconds;
            util::log_msg("帧率：{}", fps);
            remainingFramesLog = framesPerLog;
            lastFpsLogTime = currentTime;
        }

        glfwPollEvents();

        if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            break;
        }

        updateCamera(deltaSeconds);

        m_lastFrameTimeMilliseconds = deltaSeconds * 1000.0F;
        if (deltaSeconds > 0.0F) {
            m_lastFramesPerSecond = 1.0F / deltaSeconds;
        }

        updateImGui();

        if (!m_pendingImportPaths.empty()) {
            const std::vector<std::filesystem::path> gltfPaths = std::move(m_pendingImportPaths);
            m_pendingImportPaths.clear();

            const std::string label =
                m_pendingImportLabel.empty() ? gltfPaths.front().filename().string() : m_pendingImportLabel;
            m_pendingImportLabel.clear();

            try {
                importScene(gltfPaths);
                m_importStatus = std::format("已导入 {}", label);
            }
            catch (const std::exception& error) {
                m_importStatus = std::format("导入失败：{}", error.what());
                util::log_msg("[导入] {}", m_importStatus);
            }
        }

        renderFrame();

        if (m_remainingFrameLimit > 0) {
            --m_remainingFrameLimit;
            util::log_msg("剩余帧数：{}", m_remainingFrameLimit);
            if (m_remainingFrameLimit == 0) {
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
        }
    }
}

// 相机输入：按住鼠标右键拖动转视角，WASD 平移，QE 升降。
void Application::updateCamera(float deltaSeconds)
{

    deltaSeconds = std::min(deltaSeconds, 0.3f);

    {
        double mouseX = 0.0;
        double mouseY = 0.0;
        glfwGetCursorPos(m_window, &mouseX, &mouseY);

        const bool rightButtonDown = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        const bool lookingAround = rightButtonDown && !ImGui::GetIO().WantCaptureMouse;

        if (lookingAround && m_mouseLookActive) {
            const double deltaX = mouseX - m_lastMouseX;
            const double deltaY = mouseY - m_lastMouseY;
            m_cameraYaw += static_cast<float>(deltaX) * mouseLookSensitivity;
            m_cameraPitch -= static_cast<float>(deltaY) * mouseLookSensitivity;

            m_cameraPitch = std::clamp(m_cameraPitch, -cameraPitchLimit, cameraPitchLimit);
        }

        m_mouseLookActive = lookingAround;
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
    }

    {
        const glm::vec3 forward = util::math::calculateForward(m_cameraPitch, m_cameraYaw);
        const glm::vec3 worldUp{0.0F, 1.0F, 0.0F};
        const glm::vec3 right = util::math::calculateRight(forward, worldUp);

        glm::vec3 movement{0.0F};
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) {
            movement += forward;
        }
        if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) {
            movement -= forward;
        }
        if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) {
            movement += right;
        }
        if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) {
            movement -= right;
        }
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) {
            movement += worldUp;
        }
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) {
            movement -= worldUp;
        }

        if (glm::length(movement) > 0.0F) {
            constexpr float cameraMoveSpeed = 4.0F;
            m_cameraPos += glm::normalize(movement) * cameraMoveSpeed * deltaSeconds;
        }
    }
}

// 构建一帧界面：菜单栏、停靠布局、渲染器面板与场景导入面板。
void Application::updateImGui()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("退出")) {
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            ImGui::MenuItem("场景", nullptr, &m_showSceneWindow);
            ImGui::MenuItem("Dear ImGui 示例", nullptr, &m_showImGuiDemoWindow);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    const ImGuiID dockspaceId =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    if (m_needDefaultDockLayout) {
        m_needDefaultDockLayout = false;

        const ImGuiDockNodeFlags dockNodeFlags =
            static_cast<ImGuiDockNodeFlags>(static_cast<int>(ImGuiDockNodeFlags_DockSpace) |
                                            static_cast<int>(ImGuiDockNodeFlags_PassthruCentralNode));

        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, dockNodeFlags);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID centerNode = dockspaceId;
        const ImGuiID leftNode = ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Left, 0.30F, nullptr, &centerNode);
        const ImGuiID rightNode = ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Right, 0.40F, nullptr, &centerNode);

        ImGui::DockBuilderDockWindow("场景", leftNode);
        ImGui::DockBuilderDockWindow("渲染器", rightNode);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    ImGui::Begin("渲染器");
    ImGui::Text("帧耗时：%.2f 毫秒（%.1f 帧/秒）", m_lastFrameTimeMilliseconds, m_lastFramesPerSecond);
    ImGui::Text("交换链：%u x %u", m_swapchainExtent.width, m_swapchainExtent.height);
    ImGui::Text("绘制次数：%zu", m_scene.m_drawData.size());
    ImGui::Text("并行帧数：%u", maxFramesInFlight);

    ImGui::SeparatorText("显示模式");

    // 线框依赖设备的 fillModeNonSolid 特性，不支持时置灰并说明原因。
    if (!m_wireframeSupported) {
        ImGui::BeginDisabled();
    }
    if (ImGui::RadioButton("实体模式", !m_wireframeMode)) {
        m_wireframeMode = false;
    }
    if (ImGui::RadioButton("网格模式（线框）", m_wireframeMode)) {
        m_wireframeMode = true;
    }
    if (!m_wireframeSupported) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("当前设备不支持线框模式");
    }

    ImGui::SeparatorText("相机");

    ImGui::DragFloat3("位置", &m_cameraPos.x, 0.1F);

    float yawDegrees = glm::degrees(m_cameraYaw);
    if (ImGui::SliderFloat("偏航", &yawDegrees, -180.0F, 180.0F, "%.1f 度")) {
        m_cameraYaw = glm::radians(yawDegrees);
    }

    const float pitchLimitDegrees = glm::degrees(cameraPitchLimit);
    float pitchDegrees = glm::degrees(m_cameraPitch);
    if (ImGui::SliderFloat("俯仰", &pitchDegrees, -pitchLimitDegrees, pitchLimitDegrees, "%.1f 度")) {
        m_cameraPitch = glm::radians(pitchDegrees);
    }

    ImGui::TextDisabled("按住鼠标右键拖动转视角，WASD 移动，QE 升降。");
    ImGui::End();

    if (m_showSceneWindow) {
        ImGui::Begin("场景", &m_showSceneWindow);

        ImGui::TextWrapped("启动时不加载场景：选择 glTF 文件后点击「导入」才会加载。");
        ImGui::Spacing();

        if (!m_sceneListScanned) {
            scanAvailableScenes();
        }

        ImGui::SeparatorText("预设场景");
        for (std::size_t presetIndex = 0; presetIndex < SceneData::scenePresets.size(); ++presetIndex) {
            const SceneData::ScenePreset& preset = SceneData::scenePresets[presetIndex];

            const bool available = m_presetAvailable[presetIndex];
            if (!available) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(std::string{preset.name}.c_str(), ImVec2(-1.0F, 0.0F))) {
                std::vector<std::filesystem::path> presetPaths;
                presetPaths.reserve(preset.files.size());
                for (const std::string_view relativeFile : preset.files) {
                    presetPaths.push_back(resolveScenePath(relativeFile));
                }
                queueImport(presetPaths, preset.name);
                m_importStatus = std::format("正在加载 {}…", preset.name);
            }
            if (!available) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("资源目录下缺少这个场景的 glTF 文件");
                }
            }
        }

        ImGui::SeparatorText("自定义单个文件");

        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##scenePath", "modular-demo/modular-demo.gltf", m_importPathBuffer.data(),
                                 m_importPathBuffer.size());

        const std::filesystem::path resolvedPath = resolveScenePath(m_importPathBuffer.data());
        ImGui::TextDisabled("实际路径：%s", resolvedPath.string().c_str());

        if (ImGui::Button("导入", ImVec2(120.0F, 0.0F))) {
            const std::string typedPath = m_importPathBuffer.data();
            if (typedPath.empty()) {
                m_importStatus = "导入失败：路径为空";
            }
            else {
                queueImport(std::span{&resolvedPath, std::size_t{1}}, typedPath);
                m_importStatus = std::format("正在加载 {}…", resolvedPath.filename().string());
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("重新扫描", ImVec2(140.0F, 0.0F))) {
            scanAvailableScenes();
        }

        if (!m_availableScenes.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("在资源目录下发现 %zu 个 glTF 文件", m_availableScenes.size());

            const ImVec2 listSize{-1.0F, 8.0F * ImGui::GetTextLineHeightWithSpacing()};
            if (ImGui::BeginListBox("##sceneList", listSize)) {
                for (int index = 0; index < static_cast<int>(m_availableScenes.size()); ++index) {
                    const bool isSelected = (m_selectedSceneIndex == index);
                    if (ImGui::Selectable(m_availableScenes[static_cast<std::size_t>(index)].c_str(), isSelected)) {
                        m_selectedSceneIndex = index;
                        copyToImportPathBuffer(m_availableScenes[static_cast<std::size_t>(index)]);
                    }
                }
                ImGui::EndListBox();
            }
        }

        ImGui::SeparatorText("状态");
        ImGui::TextWrapped("%s", m_importStatus.c_str());
        ImGui::Text("场景已加载：%s", m_sceneLoaded ? "是" : "否");
        ImGui::Text("网格：%zu   绘制：%zu   贴图：%zu", m_scene.m_meshes.size(), m_scene.m_drawData.size(),
                    m_scene.m_textures.size());

        ImGui::End();
    }

    if (m_showImGuiDemoWindow) {
        ImGui::ShowDemoWindow(&m_showImGuiDemoWindow);
    }

    ImGui::Render();
}

// 把界面绘制数据回放到命令缓冲区，与场景共用同一个动态渲染范围。
void Application::recordImGuiDrawData(vk::CommandBuffer commandBuffer)
{
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->CmdListsCount == 0) {
        return;
    }

    beginDebugLabel(commandBuffer, "Dear ImGui", debugData.renderColor);
    ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
    endDebugLabel(commandBuffer);
}

// 渲染一帧的编排：等待帧资源、获取交换链图像、录制命令、提交并呈现。
void Application::renderFrame()
{

    util::require(!m_helperCommandBuffer.m_active, "渲染时辅助命令缓冲区不应处于激活状态");

    const std::size_t frameIndex = m_currentFrameInFlight;
    FrameInFlightResources& frame = m_framesInFlight[frameIndex];

    waitForFrameResources(frame);

    const std::uint32_t swapchainImageIndex = acquireSwapchainImage(frame);

    startRecordingCommandBuffer(frame);

    uploadCameraData(frame);

    recordRenderingCommandBuffer(frame, util::safeCastToU32(frameIndex), swapchainImageIndex);

    finishAndSubmitMainCommandBuffer(frame, swapchainImageIndex);

    m_currentFrameInFlight =
        util::safeCastToU32((static_cast<std::size_t>(m_currentFrameInFlight) + 1U) % m_framesInFlight.size());
}

// 等待上一轮使用该帧资源的 GPU 工作结束。
void Application::waitForFrameResources(FrameInFlightResources& frame)
{

    const std::array frameFences{*frame.m_inFlightFence};

    const bool waitAll = true;
    const uint64_t timeout = UINT64_MAX;

    const vk::Result waitResult = m_logicalDevice.waitForFences(frameFences, waitAll, timeout);
    util::require(waitResult == vk::Result::eSuccess, "等待帧围栏失败");
}

// 获取下一张交换链图像，并据此选择本帧使用的渲染资源。
std::uint32_t Application::acquireSwapchainImage(FrameInFlightResources& frame)
{

    const auto acquireResult = m_logicalDevice.acquireNextImageKHR(
        vk::SwapchainKHR{m_vkbData.m_swapchain.swapchain}, UINT64_MAX, *frame.m_imageAvailableSemaphore, nullptr);

    {
        if (acquireResult.result == vk::Result::eSuboptimalKHR) {
            util::log_msg("忽略交换链次优状态："
                          "交换链仍可呈现，但应尽快重建。");
        }
        else {
            util::require(acquireResult.result != vk::Result::eErrorOutOfDateKHR,
                          "交换链已过期。本示例未实现窗口缩放。");
            util::require(
                acquireResult.result == vk::Result::eSuccess,
                std::format("获取交换链图像失败。返回码 {}\n", vk::to_string(acquireResult.result)));
        }
    }
    const std::uint32_t swapchainImageIndex = acquireResult.value;
    SwapchainImageResources& swapchainImage = m_swapchainImages[swapchainImageIndex];

    swapchainImage.m_state.m_stageMask |= swapchainAcquireWaitStage;

    return swapchainImageIndex;
}

// 把当前相机矩阵写入该帧的相机缓冲。
void Application::uploadCameraData(FrameInFlightResources& frame)
{

    util::require(m_swapchainExtent.height > 0, "更新相机前，交换链高度必须非零");

    const float aspectRatio =
        static_cast<float>(m_swapchainExtent.width) / static_cast<float>(m_swapchainExtent.height);
    const CameraData camera{
        .viewProjection = util::math::calculateViewProjection(m_cameraPitch, m_cameraYaw, m_cameraPos, aspectRatio),
        .cameraPosition = glm::vec4{m_cameraPos, 1.0F}};

    uploadBuffer(frame.m_camera, std::as_bytes(std::span{&camera, std::size_t{1}}));
}

// 开始录制帧命令缓冲区。
void Application::startRecordingCommandBuffer(FrameInFlightResources& frame)
{

    frame.m_commandBuffer.reset();

    frame.m_commandBuffer.begin(vk::CommandBufferBeginInfo{});
}

// 录制一帧的全部渲染命令：布局转换、动态状态、描述符堆、场景绘制与界面。
void Application::recordRenderingCommandBuffer(FrameInFlightResources& frame, std::uint32_t frameIndex,
                                               std::uint32_t swapchainImageIndex)
{

    util::require(frameIndex < m_framesInFlight.size(), "缺少帧索引");

    util::require(swapchainImageIndex < m_swapchainImages.size(), "缺少交换链图像");
    SwapchainImageResources& swapchainImage = m_swapchainImages[swapchainImageIndex];

    const GpuViewImage& depthImage = frame.m_depthImage;
    const vk::CommandBuffer commandBuffer = frame.m_commandBuffer;

    const std::string frameDebugLabel =
        std::format("Rendering Frame {} SwapchainImage {}", frameIndex, swapchainImageIndex);
    beginDebugLabel(commandBuffer, frameDebugLabel, debugData.frameColor);

    {
        const ImageState attachmentState{
            .m_layout = colorAttachmentLayout,
            .m_stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .m_accessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .m_aspectMask = vk::ImageAspectFlagBits::eColor,
        };
        transitionImage(commandBuffer, swapchainImage.m_image, swapchainImage.m_state, attachmentState,
                        CreateImageSubresourceRange());
    }

    {

        vk::ClearValue colorClearValue{};
        colorClearValue.color.float32 = std::array{0.55F, 0.35F, 0.35F, 1.0F}; // Greyish pink for sky/background.
        vk::ClearValue depthClearValue{};
        depthClearValue.depthStencil.depth = 1.0F;

        const std::array colorAttachments = {vk::RenderingAttachmentInfo{
            .imageView = *swapchainImage.m_imageView, // We render directly to the swapchain.
            .imageLayout = colorAttachmentLayout,
            .resolveMode = vk::ResolveModeFlagBits::eNone,
            .resolveImageView = nullptr,
            .resolveImageLayout = vk::ImageLayout::eUndefined,
            .loadOp = vk::AttachmentLoadOp::eClear, // Clear before drawing.
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = colorClearValue,
        }};

        const vk::RenderingAttachmentInfo depthAttachment{
            .imageView = *depthImage.m_imageView, // Use per-frame depth image.
            .imageLayout = depthAttachmentLayout,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eDontCare,
            .clearValue = depthClearValue,
        };

        util::require(m_swapchainExtent.height > 0 && m_swapchainExtent.width > 0,
                      "渲染区域尺寸必须大于 0");

        const vk::RenderingInfo renderingInfo{
            .renderArea = // Area to render.
            vk::Rect2D{
                .offset = vk::Offset2D{.x = 0, .y = 0},
                .extent = m_swapchainExtent,
            },
            .layerCount = 1, // This is commonly 1 except for VR or other layered rendering.
            .colorAttachmentCount = util::safeCastToU32(colorAttachments.size()),
            .pColorAttachments = colorAttachments.data(),
            .pDepthAttachment = &depthAttachment,
        };

        commandBuffer.beginRendering(renderingInfo);
    }

    {
        beginDebugLabel(commandBuffer, "Set Dynamic Graphics State", debugData.renderColor);

        {
            const vk::Viewport viewport{
                .x = 0.0F,
                .y = 0.0F,
                .width = static_cast<float>(m_swapchainExtent.width),
                .height = static_cast<float>(m_swapchainExtent.height),
                .minDepth = 0.0F,
                .maxDepth = 1.0F,
            };
            commandBuffer.setViewportWithCountEXT(viewport);
        }

        {
            const vk::Rect2D scissor{
                .offset = vk::Offset2D{.x = 0, .y = 0},
                .extent = m_swapchainExtent,
            };
            commandBuffer.setScissorWithCountEXT(scissor);
        }

        commandBuffer.setPrimitiveTopologyEXT(vk::PrimitiveTopology::eTriangleList);
        commandBuffer.setPrimitiveRestartEnableEXT(vk::False);

        commandBuffer.setRasterizerDiscardEnableEXT(vk::False);
        commandBuffer.setDepthClampEnableEXT(vk::False);
        // 显示模式由界面控制：实体模式填充分边形，网格模式只画线框。
        commandBuffer.setPolygonModeEXT(m_wireframeMode ? vk::PolygonMode::eLine : vk::PolygonMode::eFill);

        commandBuffer.setCullModeEXT(vk::CullModeFlagBits::eNone);
        commandBuffer.setFrontFaceEXT(vk::FrontFace::eCounterClockwise);
        commandBuffer.setDepthBiasEnableEXT(vk::False);

        commandBuffer.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
        {
            const std::array sampleMasks{vk::SampleMask{0xFFFFFFFFU}};

            commandBuffer.setSampleMaskEXT(vk::SampleCountFlagBits::e1, sampleMasks);
        }
        commandBuffer.setAlphaToCoverageEnableEXT(vk::False);
        commandBuffer.setAlphaToOneEnableEXT(vk::False);

        commandBuffer.setLogicOpEnableEXT(vk::False);
        {
            const std::array colorBlendEnables{vk::False};
            commandBuffer.setColorBlendEnableEXT(0, colorBlendEnables);
        }
        {
            const std::array colorWriteMasks{
                vk::ColorComponentFlags{vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA}};
            commandBuffer.setColorWriteMaskEXT(0, colorWriteMasks);
        }

        commandBuffer.setDepthTestEnableEXT(vk::True);
        commandBuffer.setDepthWriteEnableEXT(vk::True);
        commandBuffer.setDepthCompareOpEXT(vk::CompareOp::eLess);
        commandBuffer.setDepthBoundsTestEnableEXT(vk::False);
        commandBuffer.setStencilTestEnableEXT(vk::False);

        endDebugLabel(commandBuffer);
    }

    {
        VertexInput& vertexInput = m_vertexInput;
        commandBuffer.setVertexInputEXT(vertexInput.m_vertexBindings, vertexInput.m_vertexAttributes);
    }

    {
        util::require(m_descriptorHeaps.m_resourceHeap.m_buffer.m_addressGPU != 0,
                      "资源堆必须有 GPU 地址");
        util::require(m_descriptorHeaps.m_samplerHeap.m_buffer.m_addressGPU != 0,
                      "采样器堆必须有 GPU 地址");
    }

    {

        beginDebugLabel(commandBuffer, "Bind Descriptor Heaps", debugData.setupColor);

        auto createBindInfo = [](const DescriptorHeapResources::DescriptorHeapData& descriptorHeapData) {
            return vk::BindHeapInfoEXT{
                .heapRange =
                    vk::DeviceAddressRangeEXT{
                        .address = descriptorHeapData.m_buffer.m_addressGPU + descriptorHeapData.m_bindOffset,
                        .size = descriptorHeapData.m_rangeSize,
                    },
                .reservedRangeOffset = 0,
                .reservedRangeSize = descriptorHeapData.m_minReservedRange,
            };
        };

        const vk::BindHeapInfoEXT resourceHeapInfo = createBindInfo(m_descriptorHeaps.m_resourceHeap);
        const vk::BindHeapInfoEXT samplerHeapInfo = createBindInfo(m_descriptorHeaps.m_samplerHeap);

        commandBuffer.bindResourceHeapEXT(resourceHeapInfo);
        commandBuffer.bindSamplerHeapEXT(samplerHeapInfo);

        endDebugLabel(commandBuffer);
    }

    const auto bindShaderGroup = [commandBuffer](const ShaderGroup& shaderGroup) {
        beginDebugLabel(commandBuffer, "Bind Shader Objects", debugData.setupColor);

        const std::array stages{vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eFragment};
        const std::array shaders{*shaderGroup.m_vertex, *shaderGroup.m_fragment};
        commandBuffer.bindShadersEXT(stages, shaders);

        endDebugLabel(commandBuffer);
    };

    const auto pushDescriptorHeapShaderData = [maxPushDataSize = m_descriptorHeaps.m_maxPushDataSize,
                                               commandBuffer](const auto& pushData) {

        constexpr std::uint32_t pushDataOffset = 0;

        constexpr std::uint32_t pushDataSize = sizeof(pushData);

        {
            util::require(pushDataOffset <= maxPushDataSize && pushDataSize <= maxPushDataSize - pushDataOffset,
                          "描述符堆推送数据超过 maxPushDataSize");

            static_assert((pushDataSize % 4) == 0, "推送数据大小必须是 4 的倍数");
            static_assert((pushDataOffset % 4) == 0, "推送数据偏移必须是 4 的倍数");
        }

        const vk::PushDataInfoEXT pushDataInfo{
            .offset = pushDataOffset,
            .data =
                vk::HostAddressRangeConstEXT{
                    .address = &pushData,
                    .size = pushDataSize,
                },
        };
        commandBuffer.pushDataEXT(pushDataInfo);
    };

    auto createCombinedImageSamplerIndex = [](std::uint32_t imageIndex, std::uint32_t samplerIndex) {

        util::require(imageIndex <= 0xFFFFF, "图像索引必须能放进 20 位");
        util::require(samplerIndex <= 0xFFF, "采样器索引必须能放进 12 位");
        return (samplerIndex << 20) | imageIndex;
    };

    std::optional<ShaderVariant> boundShaderVariant;

    beginDebugLabel(commandBuffer, "Scene Draws", debugData.drawColor);
    for (const SceneDraw& draw : m_scene.m_drawData) {
        beginDebugLabel(commandBuffer, std::format("Draw {}", draw.m_debugName), debugData.drawColor);

        util::require(draw.m_meshId < m_scene.m_meshes.size(), "场景绘制引用了无效的网格");
        const GpuMesh& mesh = m_scene.m_meshes[draw.m_meshId];

        if (!boundShaderVariant.has_value() || *boundShaderVariant != draw.m_shaderVariant) {
            switch (draw.m_shaderVariant) {
            case ShaderVariant::AlbedoAndNormal:
                bindShaderGroup(m_shaderObjects.m_albedoAndNormal);
                break;
            case ShaderVariant::Albedo:
                bindShaderGroup(m_shaderObjects.m_albedo);
                break;
            case ShaderVariant::SolidColor:
                bindShaderGroup(m_shaderObjects.m_solidColor);
                break;
            default:
                util::require(false, "不支持的着色器变体");
                break;
            }
            boundShaderVariant = draw.m_shaderVariant;
        }

        {

            constexpr uint32_t linearSamplerIndex = 0;
            constexpr uint32_t nearestSamplerIndex = 1;

            if (draw.m_shaderVariant == ShaderVariant::AlbedoAndNormal) {

                {
                    util::require(draw.m_albedoTextureIndex < m_scene.m_textures.size(),
                                  "场景绘制引用了无效的反照率贴图");
                    util::require(draw.m_normalTextureIndex < m_scene.m_textures.size(),
                                  "场景绘制引用了无效的法线贴图");
                }

                const DescriptorHeapDrawPushIndicesAlbedoAndNormal pushData{
                    .cameraIndex = frameIndex,
                    .albedoTextureIndex =
                        createCombinedImageSamplerIndex(draw.m_albedoTextureIndex, linearSamplerIndex),
                    .normalTextureIndex =
                        createCombinedImageSamplerIndex(draw.m_normalTextureIndex, nearestSamplerIndex),
                };

                pushDescriptorHeapShaderData(pushData);
            }
            else if (draw.m_shaderVariant == ShaderVariant::Albedo) {

                util::require(draw.m_albedoTextureIndex < m_scene.m_textures.size(),
                              "场景绘制引用了无效的反照率贴图");

                const DescriptorHeapDrawPushIndicesAlbedo pushData{
                    .cameraIndex = frameIndex,
                    .albedoTextureIndex =
                        createCombinedImageSamplerIndex(draw.m_albedoTextureIndex, linearSamplerIndex),
                };

                pushDescriptorHeapShaderData(pushData);
            }
            else {
                util::require(draw.m_shaderVariant == ShaderVariant::SolidColor, "不支持的着色器变体");
                const DescriptorHeapDrawPushIndicesSolidColor pushData{
                    .cameraIndex = frameIndex,
                };
                pushDescriptorHeapShaderData(pushData);
            }
        }

        {

            beginDebugLabel(commandBuffer, "Bind Geometry", debugData.setupColor);

            const std::array vertexBuffers{*mesh.m_vertices.m_buffer};
            const std::array vertexOffsets{vk::DeviceSize{0}};

            const std::array vertexSizes{mesh.m_vertices.m_size};
            const std::array vertexStrides{vk::DeviceSize{sizeof(Vertex)}};

            constexpr uint32_t firstBinding = 0;

            {
                util::require(vertexSizes.size() == vertexOffsets.size() &&
                                  vertexSizes.size() == vertexBuffers.size() &&
                                  vertexSizes.size() == vertexStrides.size(),
                              "绑定缓冲区信息的数量必须一致");

                util::require(mesh.m_indexCount > 0,
                              std::format("绘制 {} 的网格没有任何索引", draw.m_debugName));

                for (uint32_t a = 0; a < vertexSizes.size(); ++a) {
                    util::require(vertexSizes[a] > 0,
                                  std::format("绘制 {} 的网格顶点缓冲区为空", draw.m_debugName));
                    util::require(
                        (vertexSizes[a] % vertexStrides[a]) == 0,
                        std::format("绘制 {} 的网格顶点缓冲区大小未对齐", draw.m_debugName));
                }
            }

            commandBuffer.bindVertexBuffers2(firstBinding, vertexBuffers, vertexOffsets, vertexSizes, vertexStrides);

            constexpr vk::DeviceSize indexBufferOffset = 0;
            commandBuffer.bindIndexBuffer2(*mesh.m_indices.m_buffer, indexBufferOffset, mesh.m_indices.m_size,
                                           vk::IndexType::eUint32);

            endDebugLabel(commandBuffer);
        }

        {
            util::require(mesh.m_indexCount > 0, "场景绘制引用了空网格");

            const std::uint32_t firstIndex = 0;
            const std::int32_t vertexOffset = 0;

            const std::uint32_t instanceCount = 1;
            const std::uint32_t firstInstance = draw.m_objectIndex;

            commandBuffer.drawIndexed(mesh.m_indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        }

        endDebugLabel(commandBuffer);
    }

    endDebugLabel(commandBuffer);

    recordImGuiDrawData(commandBuffer);

    commandBuffer.endRendering();

    {
        beginDebugLabel(commandBuffer, "Transition Swapchain To Present", debugData.barrierColor);
        const ImageState presentState{
            .m_layout = vk::ImageLayout::ePresentSrcKHR,
            .m_stageMask = vk::PipelineStageFlagBits2::eNone,
            .m_accessMask = vk::AccessFlagBits2::eNone,
            .m_aspectMask = vk::ImageAspectFlagBits::eColor,
        };
        transitionImage(commandBuffer, swapchainImage.m_image, swapchainImage.m_state, presentState,
                        CreateImageSubresourceRange());
        endDebugLabel(commandBuffer);
    }

    endDebugLabel(commandBuffer);
}

// 结束录制并提交命令缓冲区，用信号量把渲染与呈现串起来。
void Application::finishAndSubmitMainCommandBuffer(FrameInFlightResources& frame, std::uint32_t swapchainImageIndex)
{
    util::require(swapchainImageIndex < m_swapchainImages.size(), "缺少交换链图像");
    SwapchainImageResources& swapchainImage = m_swapchainImages[swapchainImageIndex];

    frame.m_commandBuffer.end();

    {
        const std::array commandBufferInfos = {vk::CommandBufferSubmitInfo{
            .commandBuffer = frame.m_commandBuffer,
        }};

        const std::array waitInfos = {vk::SemaphoreSubmitInfo{
            .semaphore = *frame.m_imageAvailableSemaphore,
            .stageMask = swapchainAcquireWaitStage,
        }};

        const std::array signalInfos = {vk::SemaphoreSubmitInfo{
            .semaphore = *swapchainImage.m_renderFinishedSemaphore,
            .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
        }};

        const vk::SubmitInfo2 submitInfo{
            .waitSemaphoreInfoCount = util::safeCastToU32(waitInfos.size()),
            .pWaitSemaphoreInfos = waitInfos.data(),
            .commandBufferInfoCount = util::safeCastToU32(commandBufferInfos.size()),
            .pCommandBufferInfos = commandBufferInfos.data(),
            .signalSemaphoreInfoCount = util::safeCastToU32(signalInfos.size()),
            .pSignalSemaphoreInfos = signalInfos.data(),
        };

        {
            const std::array frameFences{*frame.m_inFlightFence};
            m_logicalDevice.resetFences(frameFences);
        }

        m_graphicsQueue.submit2(submitInfo, *frame.m_inFlightFence);
    }

    {

        const std::array waitSemaphores{*swapchainImage.m_renderFinishedSemaphore};

        const std::array swapchains{vk::SwapchainKHR{m_vkbData.m_swapchain.swapchain}};
        const std::array imageIndices{swapchainImageIndex};

        const vk::PresentInfoKHR presentInfo{
            .waitSemaphoreCount = util::safeCastToU32(waitSemaphores.size()),
            .pWaitSemaphores = waitSemaphores.data(),
            .swapchainCount = util::safeCastToU32(swapchains.size()),
            .pSwapchains = swapchains.data(),
            .pImageIndices = imageIndices.data(),
        };
        const vk::Result presentResult = m_presentQueue.presentKHR(presentInfo);
        util::require(presentResult == vk::Result::eSuccess || presentResult == vk::Result::eSuboptimalKHR,
                      "呈现交换链图像失败");
    }
}

// 取出初始化用的辅助命令缓冲区，并校验它处于激活状态。
vk::CommandBuffer Application::getHelperCommandBuffer() const
{

    util::require(m_helperCommandBuffer.m_commandBuffer, "辅助命令缓冲区尚未分配");

    util::require(m_helperCommandBuffer.m_active, "记录上传命令前，辅助命令缓冲区必须处于激活状态");

    return m_helperCommandBuffer.m_commandBuffer;
}

// 开始录制辅助命令缓冲区，用于初始化阶段的上传与布局转换。
void Application::beginHelperCommands()
{

    util::require(m_helperCommandBuffer.m_commandBuffer, "辅助命令缓冲区尚未分配");

    util::require(!m_helperCommandBuffer.m_active, "辅助命令缓冲区已经处于激活状态");

    m_helperCommandBuffer.m_commandBuffer.reset();
    m_helperCommandBuffer.m_commandBuffer.begin(
        vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    beginDebugLabel(m_helperCommandBuffer.m_commandBuffer, "Setup Uploads", debugData.setupColor);

    m_helperCommandBuffer.m_active = true;
}

// 提交辅助命令缓冲区并等待完成，然后释放暂存的临时缓冲。
void Application::endHelperCommandsAndFlushUploads()
{
    const vk::CommandBuffer commandBuffer = getHelperCommandBuffer();

    endDebugLabel(commandBuffer);

    commandBuffer.end();

    {
        const std::array commandBufferInfo = {vk::CommandBufferSubmitInfo{.commandBuffer = commandBuffer}};
        const vk::SubmitInfo2 submitInfo{
            .commandBufferInfoCount = util::safeCastToU32(commandBufferInfo.size()),
            .pCommandBufferInfos = commandBufferInfo.data(),
        };

        vk::Fence nullFence{};

        m_graphicsQueue.submit2(submitInfo, nullFence);

        m_graphicsQueue.waitIdle();
    }

    {
        m_helperCommandBuffer.m_commandBuffer.reset();
        m_helperCommandBuffer.m_active = false;
    }

    {
        for (GpuBuffer& stagingBuffer : m_pendingUploadStagingBuffers) {
            stagingBuffer.destroy(m_logicalDevice);
        }
        m_pendingUploadStagingBuffers.clear();
    }
}

Application::~Application()
{
    cleanup();
}

// 按依赖顺序销毁全部 Vulkan、GLFW 与 ImGui 资源。
void Application::cleanup()
{

    if (m_logicalDevice) {
        m_logicalDevice.waitIdle();
    }
    {
        if (m_imGuiInitialized) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            m_imGuiInitialized = false;
        }

        m_shaderObjects.m_solidColor.m_fragment.reset();
        m_shaderObjects.m_solidColor.m_vertex.reset();
        m_shaderObjects.m_albedo.m_fragment.reset();
        m_shaderObjects.m_albedo.m_vertex.reset();
        m_shaderObjects.m_albedoAndNormal.m_fragment.reset();
        m_shaderObjects.m_albedoAndNormal.m_vertex.reset();

        destroySceneResources();

        for (GpuBuffer& stagingBuffer : m_pendingUploadStagingBuffers) {
            stagingBuffer.destroy(m_logicalDevice);
        }
        m_pendingUploadStagingBuffers.clear();

        m_helperCommandBuffer = {};

        for (FrameInFlightResources& frame : m_framesInFlight) {
            frame.m_camera.destroy(m_logicalDevice);
            frame.m_commandBuffer = nullptr;
            frame.m_inFlightFence.reset();
            frame.m_imageAvailableSemaphore.reset();

            frame.m_depthImage.destroy();
        }

        m_commandPool.reset();

        m_swapchainImages.clear();
    }

    {
        if (m_vkbData.m_swapchain.swapchain != VK_NULL_HANDLE) {
            vkb::destroy_swapchain(m_vkbData.m_swapchain);
            m_vkbData.m_swapchain = {};
        }

        if (m_vkbData.m_device.device != VK_NULL_HANDLE) {
            vkb::destroy_device(m_vkbData.m_device);
            m_vkbData.m_device = {};
            m_logicalDevice = nullptr;
        }

        if (m_surface) {
            vkb::destroy_surface(m_vkbData.m_instance, static_cast<VkSurfaceKHR>(m_surface));
            m_surface = nullptr;
        }

        if (m_vkbData.m_instance.instance != VK_NULL_HANDLE) {
            vkb::destroy_instance(m_vkbData.m_instance);
            m_vkbData.m_instance = {};
        }
    }

    {
        if (m_window != nullptr) {
            glfwDestroyWindow(m_window);
            m_window = nullptr;
        }
        glfwTerminate();
    }
}

// 按类型位与属性要求挑选可用的显存类型。
std::uint32_t Application::findMemoryType(std::uint32_t typeBits, vk::MemoryPropertyFlags properties) const
{
    for (std::uint32_t i = 0; i < m_memoryTypeFlags.size(); ++i) {
        const bool typeMatches = (typeBits & (1U << i)) != 0U;
        const bool flagsMatch = (m_memoryTypeFlags[i] & properties) == properties;
        if (typeMatches && flagsMatch) {
            return i;
        }
    }

    throw std::runtime_error("找不到兼容的 Vulkan 内存类型");
}

// 创建缓冲区并按需求绑定显存；需要 CPU 访问时返回持久映射地址。
Application::GpuBuffer Application::createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                                                 vk::MemoryPropertyFlags memoryProperties,
                                                 std::string_view debugName) const
{

    util::require(size > 0, "Vulkan 要求缓冲区大小大于 0。");
    util::require(usage != vk::BufferUsageFlags{}, "尝试创建没有指定用途标志的缓冲区。");

    GpuBuffer buffer{.m_size = size};

    {
        const vk::BufferCreateInfo bufferInfo{
            .size = size,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
        };
        buffer.m_buffer = m_logicalDevice.createBufferUnique(bufferInfo);
    }

    setDebugName(*buffer.m_buffer, vk::ObjectType::eBuffer, std::string{debugName});

    const bool needsGpuAddress = (usage & vk::BufferUsageFlagBits::eShaderDeviceAddress) != vk::BufferUsageFlags{};

    {
        const vk::BufferMemoryRequirementsInfo2 requirementsInfo{
            .buffer = *buffer.m_buffer,
        };
        const vk::MemoryRequirements2 requirements = m_logicalDevice.getBufferMemoryRequirements2(requirementsInfo);

        const vk::MemoryAllocateFlagsInfo allocateFlags{
            .flags = needsGpuAddress ? vk::MemoryAllocateFlagBits::eDeviceAddress : vk::MemoryAllocateFlags{},
        };

        const vk::MemoryAllocateInfo allocateInfo{
            .pNext = (allocateFlags.flags != vk::MemoryAllocateFlags{}) ? &allocateFlags : nullptr,
            .allocationSize = requirements.memoryRequirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryRequirements.memoryTypeBits, memoryProperties),
        };

        buffer.m_memory = m_logicalDevice.allocateMemoryUnique(allocateInfo);

        const std::array bindInfos{vk::BindBufferMemoryInfo{
            .buffer = *buffer.m_buffer,
            .memory = *buffer.m_memory,
            .memoryOffset = 0,
        }};
        m_logicalDevice.bindBufferMemory2(bindInfos);
    }

    if (needsGpuAddress) {
        const vk::BufferDeviceAddressInfo addressInfo{.buffer = *buffer.m_buffer};
        buffer.m_addressGPU = m_logicalDevice.getBufferAddress(addressInfo);
        util::require(buffer.m_addressGPU != 0, "获取缓冲区 GPU 地址失败");
    }

    const bool hostVisible = (memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible) != vk::MemoryPropertyFlags{};

    if (hostVisible) {
        buffer.m_addressCPU = m_logicalDevice.mapMemory(*buffer.m_memory, 0, size);

        util::require(buffer.m_addressCPU != nullptr, "映射缓冲区 CPU 地址失败");

        util::require((memoryProperties & vk::MemoryPropertyFlagBits::eHostCoherent) ==
                          vk::MemoryPropertyFlagBits::eHostCoherent,
                      "本示例中映射的缓冲区必须是 host coherent，以避免手动刷新");
    }

    return buffer;
}

// 创建临时暂存缓冲并把数据写入其中，供后续拷贝到 GPU 缓冲。
Application::GpuBuffer Application::uploadToNewStagingBuffer(std::span<const std::byte> data,
                                                             std::string_view debugName) const
{

    const vk::MemoryPropertyFlags stagingMemoryProperties =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;

    GpuBuffer stagingBuffer = createBuffer(static_cast<vk::DeviceSize>(data.size()),
                                           vk::BufferUsageFlagBits::eTransferSrc, stagingMemoryProperties, debugName);

    uploadBuffer(stagingBuffer, data);
    return stagingBuffer;
}

// 先用暂存缓冲写入数据，再拷贝到设备本地缓冲并返回结果。
Application::GpuBuffer Application::uploadToNewGpuBuffer(std::span<const std::byte> data,
                                                         vk::BufferUsageFlags finalUsage, std::string_view debugName)
{

    const vk::CommandBuffer commandBuffer = getHelperCommandBuffer();

    const std::string stagingName = std::format("{} Staging", debugName);
    GpuBuffer stagingBuffer = uploadToNewStagingBuffer(data, stagingName);

    GpuBuffer gpuBuffer = createBuffer(static_cast<vk::DeviceSize>(data.size()),
                                       finalUsage | vk::BufferUsageFlagBits::eTransferDst,
                                       vk::MemoryPropertyFlagBits::eDeviceLocal, debugName);

    const std::string uploadDebugLabel = std::format("上传缓冲区：{}", debugName);
    beginDebugLabel(commandBuffer, uploadDebugLabel, debugData.transferColor);

    {
        const std::array copyRegion = {vk::BufferCopy2{
            .srcOffset = 0,
            .dstOffset = 0,
            .size = static_cast<vk::DeviceSize>(data.size()),
        }};
        const vk::CopyBufferInfo2 copyInfo{
            .srcBuffer = *stagingBuffer.m_buffer,
            .dstBuffer = *gpuBuffer.m_buffer,
            .regionCount = util::safeCastToU32(copyRegion.size()),
            .pRegions = copyRegion.data(),
        };
        commandBuffer.copyBuffer2(copyInfo);
    }

    {
        const std::array uploadBarrier = {vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *gpuBuffer.m_buffer,
            .offset = 0,
            .size = static_cast<vk::DeviceSize>(data.size()),
        }};
        const vk::DependencyInfo dependencyInfo{
            .bufferMemoryBarrierCount = util::safeCastToU32(uploadBarrier.size()),
            .pBufferMemoryBarriers = uploadBarrier.data(),
        };
        commandBuffer.pipelineBarrier2(dependencyInfo);
    }
    endDebugLabel(commandBuffer);

    m_pendingUploadStagingBuffers.push_back(std::move(stagingBuffer));
    return gpuBuffer;
}

// 把数据拷贝进已映射的缓冲区。
void Application::uploadBuffer(const GpuBuffer& buffer, std::span<const std::byte> data) const
{

    util::require(buffer.m_addressCPU != nullptr, "CPU 上传要求缓冲区保持持久映射");
    util::require(data.size() <= buffer.m_size, "上传数据大于目标缓冲区");

    std::memcpy(buffer.m_addressCPU, data.data(), data.size());
}

// 为图像分配显存并绑定到图像对象。
void Application::allocateGpuImage(GpuImage& image) const
{
    util::require(static_cast<bool>(image.m_image), "分配内存前 GpuImage 必须已存在");

    const vk::ImageMemoryRequirementsInfo2 requirementsInfo{
        .image = *image.m_image,
    };
    const vk::MemoryRequirements2 requirements = m_logicalDevice.getImageMemoryRequirements2(requirementsInfo);
    const vk::MemoryAllocateInfo allocateInfo{
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex =
            findMemoryType(requirements.memoryRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
    };
    image.m_memory = m_logicalDevice.allocateMemoryUnique(allocateInfo);

    const std::array bindInfos{vk::BindImageMemoryInfo{
        .image = *image.m_image,
        .memory = *image.m_memory,
        .memoryOffset = 0,
    }};
    m_logicalDevice.bindImageMemory2(bindInfos);
}

// 上传二维贴图并生成 mipmap，返回可直接用于描述符的图像对象。
Application::GpuImage Application::createTexture(const util::ImageRgba8& sourceImage, std::string_view debugName)
{

    util::require(sourceImage.m_width > 0 && sourceImage.m_height > 0,
                  "纹理图像的宽高必须非零");
    util::require(static_cast<std::size_t>(sourceImage.m_height) * static_cast<std::size_t>(sourceImage.m_width) ==
                      sourceImage.m_pixels.size(),
                  "图像尺寸无效。");

    GpuImage image;
    image.m_mipLevels = static_cast<std::uint32_t>(
        std::floor(std::log2(static_cast<float>(std::max(sourceImage.m_width, sourceImage.m_height)))) + 1.0F);
    image.m_states.resize(image.m_mipLevels, ImageState{.m_aspectMask = vk::ImageAspectFlagBits::eColor});

    {
        const vk::ImageCreateInfo imageInfo{
            .imageType = vk::ImageType::e2D,
            .format = mainTextureFormat,
            .extent = vk::Extent3D{.width = sourceImage.m_width,
                                   .height = sourceImage.m_height,
                                   .depth = 1},
            .mipLevels = image.m_mipLevels,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eSampled,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        image.m_image = m_logicalDevice.createImageUnique(imageInfo);
    }
    setDebugName(*image.m_image, vk::ObjectType::eImage, std::string{debugName});

    allocateGpuImage(image);

    {
        const std::string stagingName = std::format("{} Staging", debugName);
        GpuBuffer stagingBuffer = uploadToNewStagingBuffer(std::as_bytes(std::span{sourceImage.m_pixels}), stagingName);

        const vk::CommandBuffer commandBuffer = getHelperCommandBuffer();

        const std::string uploadDebugLabel =
            debugName.empty() ? std::string{"Upload Texture"} : std::format("上传纹理：{}", debugName);
        beginDebugLabel(commandBuffer, uploadDebugLabel, debugData.transferColor);

        const ImageState transferDstState{
            .m_layout = vk::ImageLayout::eTransferDstOptimal,
            .m_stageMask = vk::PipelineStageFlagBits2::eTransfer,
            .m_accessMask = vk::AccessFlagBits2::eTransferWrite,
            .m_aspectMask = vk::ImageAspectFlagBits::eColor,
        };
        const ImageState transferSrcState{
            .m_layout = vk::ImageLayout::eTransferSrcOptimal,
            .m_stageMask = vk::PipelineStageFlagBits2::eTransfer,
            .m_accessMask = vk::AccessFlagBits2::eTransferRead,
            .m_aspectMask = vk::ImageAspectFlagBits::eColor,
        };
        const ImageState shaderReadState{
            .m_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .m_stageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .m_accessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .m_aspectMask = vk::ImageAspectFlagBits::eColor,
        };

        image.transition(commandBuffer, transferDstState);

        {
            const std::array copyRegion = {vk::BufferImageCopy2{
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    vk::ImageSubresourceLayers{
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = vk::Offset3D{.x = 0, .y = 0, .z = 0},
                .imageExtent = vk::Extent3D{.width = sourceImage.m_width, .height = sourceImage.m_height, .depth = 1},
            }};
            const vk::CopyBufferToImageInfo2 copyInfo{
                .srcBuffer = *stagingBuffer.m_buffer,
                .dstImage = *image.m_image,
                .dstImageLayout = transferDstState.m_layout,
                .regionCount = util::safeCastToU32(copyRegion.size()),
                .pRegions = copyRegion.data(),
            };
            commandBuffer.copyBufferToImage2(copyInfo);
        }

        {
            std::int32_t sourceWidth = util::safeCastTo<std::int32_t>(sourceImage.m_width);
            std::int32_t sourceHeight = util::safeCastTo<std::int32_t>(sourceImage.m_height);

            for (std::uint32_t mipLevel = 1; mipLevel < image.m_mipLevels; ++mipLevel) {
                image.transition(commandBuffer, transferSrcState, mipLevel - 1, 1);

                const std::int32_t destinationWidth = std::max(1, sourceWidth / 2);
                const std::int32_t destinationHeight = std::max(1, sourceHeight / 2);

                {
                    const vk::ImageBlit2 blitRegion{
                        .srcSubresource =
                            vk::ImageSubresourceLayers{
                                .aspectMask = vk::ImageAspectFlagBits::eColor,
                                .mipLevel = mipLevel - 1,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                            },
                        .srcOffsets = std::array{vk::Offset3D{.x = 0, .y = 0, .z = 0},
                                                 vk::Offset3D{.x = sourceWidth, .y = sourceHeight, .z = 1}},
                        .dstSubresource =
                            vk::ImageSubresourceLayers{
                                .aspectMask = vk::ImageAspectFlagBits::eColor,
                                .mipLevel = mipLevel,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                            },
                        .dstOffsets = std::array{vk::Offset3D{.x = 0, .y = 0, .z = 0},
                                                 vk::Offset3D{.x = destinationWidth, .y = destinationHeight, .z = 1}},
                    };
                    const vk::BlitImageInfo2 blitInfo{
                        .srcImage = *image.m_image,
                        .srcImageLayout = transferSrcState.m_layout,
                        .dstImage = *image.m_image,
                        .dstImageLayout = transferDstState.m_layout,
                        .regionCount = 1,
                        .pRegions = &blitRegion,
                        .filter = vk::Filter::eLinear,
                    };
                    commandBuffer.blitImage2(blitInfo);
                }

                sourceWidth = destinationWidth;
                sourceHeight = destinationHeight;
            }

            image.transition(commandBuffer, shaderReadState);
        }
        endDebugLabel(commandBuffer);

        m_pendingUploadStagingBuffers.push_back(std::move(stagingBuffer));
    }
    return image;
}

template <typename Handle>
// 给 Vulkan 对象设置调试名称，便于在 RenderDoc 等工具中定位。
void Application::setDebugName(Handle handle, vk::ObjectType objectType, const std::string& name) const
{
    if (!debugData.enableGpuDebug) {
        return;
    }

    util::require(static_cast<bool>(m_logicalDevice), "设置调试名称需要有效的 Vulkan 设备");
    util::require(!name.empty(), "Vulkan 允许空名称，但本应用不允许");

    using RawHandle = typename Handle::CType;
    const RawHandle rawHandle = static_cast<RawHandle>(handle);
    util::require(rawHandle != VK_NULL_HANDLE, "设置调试名称需要有效的 Vulkan 对象句柄");

    const vk::DebugUtilsObjectNameInfoEXT nameInfo{
        .objectType = objectType,
        .objectHandle = util::rawHandleToUint64(rawHandle),
        .pObjectName = name.c_str(),
    };
    static_cast<void>(m_logicalDevice.setDebugUtilsObjectNameEXT(nameInfo));
}

// 开始一个调试标签区域，用于标记一段命令。
void Application::beginDebugLabel(vk::CommandBuffer commandBuffer, const std::string& name,
                                  const std::array<float, 4>& color)
{
    if (!debugData.enableGpuDebug) {
        return;
    }

    const vk::DebugUtilsLabelEXT label{
        .pLabelName = name.c_str(),
        .color = color,
    };

    commandBuffer.beginDebugUtilsLabelEXT(label);
}

// 结束最近一次调试标签区域。
void Application::endDebugLabel(vk::CommandBuffer commandBuffer)
{
    if constexpr (!debugData.enableGpuDebug) {
        return;
    }

    commandBuffer.endDebugUtilsLabelEXT();
}

// 按同步 2 的方式对图像做布局转换并更新跟踪状态。
void Application::transitionImage(vk::CommandBuffer commandBuffer, vk::Image image, ImageState& currentState,
                                  const ImageState& newState, vk::ImageSubresourceRange subresourceRange)
{

    {
        const auto onlyRead = [](const ImageState& state) {
            const vk::AccessFlags2 writeAccessMask =
                vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
                vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eTransferWrite |
                vk::AccessFlagBits2::eHostWrite | vk::AccessFlagBits2::eMemoryWrite;
            return static_cast<bool>(state.m_accessMask) && !static_cast<bool>(state.m_accessMask & writeAccessMask);
        };

        const bool duplicateReadOnlyState = currentState == newState && onlyRead(currentState);

        if (duplicateReadOnlyState) {
            return;
        }
    }

    const std::array barriers = {vk::ImageMemoryBarrier2{
        .srcStageMask = currentState.m_stageMask,
        .srcAccessMask = currentState.m_accessMask,
        .dstStageMask = newState.m_stageMask,
        .dstAccessMask = newState.m_accessMask,
        .oldLayout = currentState.m_layout,
        .newLayout = newState.m_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = subresourceRange,
    }};

    const vk::DependencyInfo dependencyInfo{
        .imageMemoryBarrierCount = util::safeCastToU32(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    };
    commandBuffer.pipelineBarrier2(dependencyInfo);

    currentState = newState;
}

// 释放缓冲区及其显存并清空句柄。
void Application::GpuBuffer::destroy(vk::Device device)
{

    if (m_addressCPU != nullptr) {
        device.unmapMemory(*m_memory);
        m_addressCPU = nullptr;
    }
    m_buffer.reset();
    m_memory.reset();
    m_addressGPU = {};
    m_size = {};
}

// 释放图像、视图与显存。
void Application::GpuImage::destroy()
{

    m_image.reset();
    m_memory.reset();
    m_mipLevels = 1;
    m_states.clear();
}

// 对整张图像做布局转换。
void Application::GpuImage::transition(vk::CommandBuffer commandBuffer, const ImageState& newState)
{
    transition(commandBuffer, newState, 0, m_mipLevels);
}

void Application::GpuImage::transition(vk::CommandBuffer commandBuffer, const ImageState& newState,
                                       std::uint32_t baseMipLevel, std::uint32_t mipLevelCount)
{

    {
        util::require(static_cast<bool>(m_image), "切换布局前 GpuImage 必须已存在");
        util::require(!m_states.empty(), "GpuImage 必须至少跟踪一个图像状态");
        util::require(mipLevelCount > 0, "GpuImage 布局切换必须覆盖至少一个 mip 层级");
        util::require(baseMipLevel < m_states.size(), "GpuImage 布局切换的起始 mip 层级越界");
        util::require((baseMipLevel + mipLevelCount) <= m_states.size(),
                      "GpuImage 布局切换范围超出已跟踪的 mip 层级");
    }

    const std::uint32_t endMipLevel = baseMipLevel + mipLevelCount;

    const auto transitionRun = [&](const std::uint32_t runBaseMipLevel, const std::uint32_t runMipLevelCount,
                                   ImageState runState) {
        transitionImage(commandBuffer, *m_image, runState, newState,
                        CreateImageSubresourceRange(runBaseMipLevel, newState.m_aspectMask, runMipLevelCount));
        for (std::uint32_t mipLevel = runBaseMipLevel; mipLevel < runBaseMipLevel + runMipLevelCount; ++mipLevel) {
            m_states[mipLevel] = runState;
        }
    };

    std::uint32_t runBaseMipLevel = baseMipLevel;
    ImageState runState = m_states[runBaseMipLevel];

    for (std::uint32_t mipLevel = baseMipLevel + 1; mipLevel < endMipLevel; ++mipLevel) {
        if (m_states[mipLevel] != runState) {

            transitionRun(runBaseMipLevel, mipLevel - runBaseMipLevel, runState);

            runBaseMipLevel = mipLevel;
            runState = m_states[mipLevel];
        }
    }

    transitionRun(runBaseMipLevel, endMipLevel - runBaseMipLevel, runState);
}

// 先销毁视图再销毁图像，保证依赖顺序正确。
void Application::GpuViewImage::destroy()
{
    m_imageView.reset();
    m_image.destroy();
}

} // namespace siggraph

// 程序入口：解析命令行、设置控制台为 UTF-8，然后运行应用。
int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(consoleCodePageUtf8);
#endif

    try {
        siggraph::Application app;

        app.setFrameLimit(siggraph::util::readFrameLimitCLI(argc, argv));
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::string_view{argv[i]} == "--import") {
                app.requestSceneImport(argv[i + 1]);
                break;
            }
        }

        app.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
