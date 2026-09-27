#include "pixel/Renderer.hpp"

#include <volk.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace pixel {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (Vulkan " + std::to_string(result) + ")");
}
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[Vulkan validation] " << data->pMessage << '\n';
    return VK_FALSE;
}
struct PushConstants {
    glm::mat4 viewProjection{1};
    glm::vec4 cameraTime{}, sunFog{}, forwardAspect{}, rightTan{};
};
static_assert(sizeof(PushConstants) == 128, "Stay within the Vulkan guaranteed push constant budget");

bool visibleBounds(const glm::mat4& matrix, const glm::vec3& minimum, const glm::vec3& maximum) {
    const glm::mat4 rows = glm::transpose(matrix);
    const std::array<glm::vec4, 6> planes{rows[3]+rows[0],rows[3]-rows[0],
        rows[3]+rows[1],rows[3]-rows[1],rows[2],rows[3]-rows[2]};
    for (const auto& plane : planes) {
        const glm::vec3 corner{plane.x >= 0 ? maximum.x : minimum.x,
            plane.y >= 0 ? maximum.y : minimum.y,plane.z >= 0 ? maximum.z : minimum.z};
        if (glm::dot(glm::vec3(plane),corner)+plane.w < 0) return false;
    }
    return true;
}
bool validationRequested() {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t length = 0;
    const bool present = _dupenv_s(&value,&length,"PIXEL_VULKAN_VALIDATION") == 0 && value != nullptr;
    std::free(value);
    return present;
#else
    return std::getenv("PIXEL_VULKAN_VALIDATION") != nullptr;
#endif
}
}

struct Renderer::Impl {
    static constexpr size_t frameCount = 2;
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0, indexOffset = 0;
        uint32_t indices = 0;
        uint64_t revision = 0;
        glm::vec3 min{}, max{};
    };
    struct Frame {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        // Reclaim only when this slot's next fence signals. That submission is
        // ordered after every previous scene use of the retired buffer.
        std::vector<Buffer> retired;
    };
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImage depth = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        // Present completion belongs to a swapchain image, not a CPU frame slot.
        // Reacquiring this image proves its last presentation wait has finished.
        VkSemaphore presented = VK_NULL_HANDLE;
    };
    GLFWwindow* window = nullptr;
    std::filesystem::path assets;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t minimumImages = 2;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline terrainPipeline = VK_NULL_HANDLE, skyPipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<Frame,frameCount> frames{};
    std::vector<Target> targets;
    std::unordered_map<ChunkKey,Buffer,ChunkKeyHash> meshes;
    std::vector<const Buffer*> visibleMeshes;
    size_t frameNumber = 0;
    bool vsync = true, resizeNeeded = false, imguiContext = false, imguiGlfw = false, imguiVulkan = false;
    std::string gpuName;
    RenderStats renderStats{};

    ~Impl() { cleanup(); }

    uint32_t memoryType(uint32_t allowed, VkMemoryPropertyFlags required) const {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((allowed & (1U << i)) && (memoryProperties.memoryTypes[i].propertyFlags & required) == required)
                return i;
        throw std::runtime_error("The graphics device has no compatible memory type");
    }

    void initialize(GLFWwindow* w, const std::filesystem::path& path) {
        window = w; assets = path;
        check(volkInitialize(), "Loading the Vulkan driver");
        uint32_t extensionCount = 0;
        const char** required = glfwGetRequiredInstanceExtensions(&extensionCount);
        if (!required) throw std::runtime_error("GLFW could not find a Vulkan surface driver");
        std::vector<const char*> extensions(required,required+extensionCount);
        std::vector<const char*> layers;
        if (validationRequested()) {
            uint32_t count = 0;
            check(vkEnumerateInstanceLayerProperties(&count,nullptr),"Enumerating Vulkan layers");
            std::vector<VkLayerProperties> available(count);
            check(vkEnumerateInstanceLayerProperties(&count,available.data()),"Reading Vulkan layers");
            for (const auto& layer : available) if (std::strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                break;
            }
            if (layers.empty()) std::cerr << "Vulkan validation requested but VK_LAYER_KHRONOS_validation is unavailable\n";
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Pixel World"; app.applicationVersion = VK_MAKE_VERSION(0,1,0);
        app.pEngineName = "Pixel Engine"; app.engineVersion = VK_MAKE_VERSION(0,1,0);
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app; create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data(); create.enabledLayerCount = static_cast<uint32_t>(layers.size());
        create.ppEnabledLayerNames = layers.data();
        check(vkCreateInstance(&create,nullptr,&instance),"Creating the Vulkan instance");
        volkLoadInstance(instance);
        if (!layers.empty() && vkCreateDebugUtilsMessengerEXT) {
            VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugInfo.pfnUserCallback = debugMessage;
            check(vkCreateDebugUtilsMessengerEXT(instance,&debugInfo,nullptr,&debug),"Creating validation messenger");
        }
        check(glfwCreateWindowSurface(instance,window,nullptr,&surface),"Creating the window surface");
        selectDevice();
        createCommands();
        createSwapchain();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext(); imguiContext = true;
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (!ImGui_ImplGlfw_InitForVulkan(window,true)) throw std::runtime_error("Could not initialize ImGui GLFW");
        imguiGlfw = true;
        initializeUiRenderer();
    }

    void selectDevice() {
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerating graphics devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Reading graphics devices");
        int bestScore = -1;
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate,&properties);
            if (properties.apiVersion < VK_API_VERSION_1_1) continue;
            uint32_t extCount = 0;
            check(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&extCount,nullptr),"Enumerating device extensions");
            std::vector<VkExtensionProperties> ext(extCount);
            check(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&extCount,ext.data()),"Reading device extensions");
            if (std::none_of(ext.begin(),ext.end(),[](const auto& e) { return std::strcmp(e.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME)==0; })) continue;
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&familyCount,nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&familyCount,families.data());
            for (uint32_t i = 0; i < familyCount; ++i) {
                VkBool32 present = VK_FALSE;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,surface,&present),"Checking presentation support");
                if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 :
                    properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 50 : 10;
                if (score > bestScore) { bestScore = score; physical = candidate; queueFamily = i; gpuName = properties.deviceName; }
                break;
            }
        }
        if (physical == VK_NULL_HANDLE) throw std::runtime_error("A Vulkan 1.1 graphics driver with window presentation support is required");
        vkGetPhysicalDeviceMemoryProperties(physical,&memoryProperties);
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.queueCreateInfoCount = 1; create.pQueueCreateInfos = &queueInfo;
        create.enabledExtensionCount = 1; create.ppEnabledExtensionNames = &extension;
        check(vkCreateDevice(physical,&create,nullptr,&device),"Creating the graphics device");
        volkLoadDevice(device);
        vkGetDeviceQueue(device,queueFamily,0,&queue);
        for (VkFormat format : {VK_FORMAT_D32_SFLOAT,VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_D16_UNORM}) {
            VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
            if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { depthFormat = format; break; }
        }
        if (depthFormat == VK_FORMAT_UNDEFINED) throw std::runtime_error("No supported depth attachment format");
        std::cout << "Vulkan renderer: " << gpuName << '\n';
    }

    void createCommands() {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        check(vkCreateCommandPool(device,&poolInfo,nullptr,&commandPool),"Creating command pool");
        std::array<VkCommandBuffer,frameCount> commands{};
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = static_cast<uint32_t>(frameCount);
        check(vkAllocateCommandBuffers(device,&allocate,commands.data()),"Allocating frame commands");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (size_t i = 0; i < frameCount; ++i) {
            frames[i].command = commands[i];
            check(vkCreateSemaphore(device,&semaphore,nullptr,&frames[i].acquired),"Creating acquire semaphore");
            check(vkCreateFence(device,&fence,nullptr,&frames[i].fence),"Creating frame fence");
        }
    }

    void initializeUiRenderer() {
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_1; info.Instance = instance; info.PhysicalDevice = physical;
        info.Device = device; info.QueueFamily = queueFamily; info.Queue = queue;
        info.DescriptorPoolSize = 64; info.RenderPass = renderPass;
        info.MinImageCount = minimumImages; info.ImageCount = static_cast<uint32_t>(targets.size());
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = [](VkResult result) { check(result,"ImGui Vulkan operation"); };
        if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("Could not initialize ImGui Vulkan");
        imguiVulkan = true;
    }

    void createSwapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&capabilities),"Reading surface capabilities");
        uint32_t formatCount = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formatCount,nullptr),"Enumerating surface formats");
        if (!formatCount) throw std::runtime_error("The window surface has no Vulkan color formats");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formatCount,formats.data()),"Reading surface formats");
        auto selected = formats.front();
        // Linear UNORM output keeps the deliberately muted palette consistent with ImGui.
        for (auto format : formats) if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) selected = format;
        if (selected.format == VK_FORMAT_UNDEFINED) selected.format = VK_FORMAT_B8G8R8A8_UNORM;
        colorFormat = selected.format;
        extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<uint32_t>::max()) {
            int width = 0,height = 0; glfwGetFramebufferSize(window,&width,&height);
            extent.width = std::clamp(static_cast<uint32_t>(std::max(1,width)),capabilities.minImageExtent.width,capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(std::max(1,height)),capabilities.minImageExtent.height,capabilities.maxImageExtent.height);
        }
        uint32_t modeCount = 0;
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&modeCount,nullptr),"Enumerating presentation modes");
        std::vector<VkPresentModeKHR> modes(modeCount);
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&modeCount,modes.data()),"Reading presentation modes");
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync) {
            if (std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        minimumImages = std::max(2U,capabilities.minImageCount);
        uint32_t imageCount = minimumImages + 1;
        if (capabilities.maxImageCount) imageCount = std::min(imageCount,capabilities.maxImageCount);
        minimumImages = std::min(minimumImages,imageCount);
        if (minimumImages < 2) throw std::runtime_error("The window surface must support at least two swapchain images");
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (capabilities.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface; info.minImageCount = imageCount; info.imageFormat = selected.format;
        info.imageColorSpace = selected.colorSpace; info.imageExtent = extent; info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform; info.compositeAlpha = alpha; info.presentMode = mode; info.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device,&info,nullptr,&swapchain),"Creating the swapchain");
        check(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,nullptr),"Counting swapchain images");
        std::vector<VkImage> images(imageCount);
        check(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,images.data()),"Reading swapchain images");
        targets.resize(imageCount);
        createRenderPass();
        for (size_t i = 0; i < targets.size(); ++i) {
            auto& target = targets[i]; target.image = images[i];
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = target.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = colorFormat;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            check(vkCreateImageView(device,&view,nullptr,&target.view),"Creating swapchain view");
            VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            depthInfo.imageType = VK_IMAGE_TYPE_2D; depthInfo.format = depthFormat;
            depthInfo.extent = {extent.width,extent.height,1}; depthInfo.mipLevels = 1; depthInfo.arrayLayers = 1;
            depthInfo.samples = VK_SAMPLE_COUNT_1_BIT; depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; depthInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateImage(device,&depthInfo,nullptr,&target.depth),"Creating depth image");
            VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device,target.depth,&requirements);
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(device,&allocate,nullptr,&target.depthMemory),"Allocating depth memory");
            check(vkBindImageMemory(device,target.depth,target.depthMemory,0),"Binding depth memory");
            view.image = target.depth; view.format = depthFormat; view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            check(vkCreateImageView(device,&view,nullptr,&target.depthView),"Creating depth view");
            const std::array<VkImageView,2> attachments{target.view,target.depthView};
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = renderPass; framebuffer.attachmentCount = 2; framebuffer.pAttachments = attachments.data();
            framebuffer.width = extent.width; framebuffer.height = extent.height; framebuffer.layers = 1;
            check(vkCreateFramebuffer(device,&framebuffer,nullptr,&target.framebuffer),"Creating framebuffer");
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device,&semaphore,nullptr,&target.presented),"Creating presentation semaphore");
        }
        createPipelines();
        resizeNeeded = false;
    }

    void createRenderPass() {
        std::array<VkAttachmentDescription,2> attachments{};
        attachments[0].format = colorFormat; attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[1].format = depthFormat; attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color; subpass.pDepthStencilAttachment = &depth;
        VkSubpassDependency dependency{}; dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = 2; info.pAttachments = attachments.data(); info.subpassCount = 1; info.pSubpasses = &subpass;
        info.dependencyCount = 1; info.pDependencies = &dependency;
        check(vkCreateRenderPass(device,&info,nullptr,&renderPass),"Creating render pass");
    }

    VkShaderModule shader(const std::string& name) {
        auto path = assets / "shaders" / (name + ".spv");
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if (!input) throw std::runtime_error("Missing compiled shader: " + path.string());
        const auto size = input.tellg();
        if (size <= 0 || static_cast<size_t>(size) % sizeof(uint32_t)) throw std::runtime_error("Invalid shader bytecode: " + path.string());
        std::vector<uint32_t> words(static_cast<size_t>(size)/sizeof(uint32_t));
        input.seekg(0); input.read(reinterpret_cast<char*>(words.data()),size);
        if (!input) throw std::runtime_error("Could not read shader bytecode: " + path.string());
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = static_cast<size_t>(size); info.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device,&info,nullptr,&module),"Creating shader module");
        return module;
    }

    void createPipelines() {
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device,&layout,nullptr,&pipelineLayout),"Creating pipeline layout");
        for (bool sky : {false,true}) {
            const std::string name = sky ? "sky" : "terrain";
            const VkShaderModule vertex = shader(name + ".vert");
            VkShaderModule fragment = VK_NULL_HANDLE;
            try { fragment = shader(name + ".frag"); }
            catch (...) { vkDestroyShaderModule(device,vertex,nullptr); throw; }
            std::array<VkPipelineShaderStageCreateInfo,2> stages{};
            stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex; stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment; stages[1].pName = "main";
            VkVertexInputBindingDescription binding{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX};
            std::array<VkVertexInputAttributeDescription,4> attributes{{
                {0,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(Vertex,position))},
                {1,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(Vertex,normal))},
                {2,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(Vertex,color))},
                {3,0,VK_FORMAT_R32_SFLOAT,static_cast<uint32_t>(offsetof(Vertex,material))}}};
            VkPipelineVertexInputStateCreateInfo vertexInfo{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertexInfo.vertexBindingDescriptionCount = sky ? 0 : 1; vertexInfo.pVertexBindingDescriptions = &binding;
            vertexInfo.vertexAttributeDescriptionCount = sky ? 0 : static_cast<uint32_t>(attributes.size());
            vertexInfo.pVertexAttributeDescriptions = attributes.data();
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = 1; viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = sky ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable = sky ? VK_FALSE : VK_TRUE; depth.depthWriteEnable = sky ? VK_FALSE : VK_TRUE;
            depth.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1; blend.pAttachments = &attachment;
            const std::array<VkDynamicState,2> dynamic{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamicInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamicInfo.dynamicStateCount = 2; dynamicInfo.pDynamicStates = dynamic.data();
            VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipeline.stageCount = 2; pipeline.pStages = stages.data(); pipeline.pVertexInputState = &vertexInfo;
            pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster;
            pipeline.pMultisampleState = &multisample; pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &blend;
            pipeline.pDynamicState = &dynamicInfo; pipeline.layout = pipelineLayout; pipeline.renderPass = renderPass;
            VkPipeline& destination = sky ? skyPipeline : terrainPipeline;
            const VkResult result = vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipeline,nullptr,&destination);
            vkDestroyShaderModule(device,vertex,nullptr); vkDestroyShaderModule(device,fragment,nullptr);
            check(result,"Creating graphics pipeline");
        }
    }

    Buffer upload(const ChunkMesh& mesh) {
        Buffer result;
        result.revision = mesh.revision; result.min = mesh.min; result.max = mesh.max;
        result.indices = static_cast<uint32_t>(mesh.mesh.indices.size());
        result.indexOffset = mesh.mesh.vertices.size() * sizeof(Vertex);
        const VkDeviceSize usedBytes = result.indexOffset + mesh.mesh.indices.size() * sizeof(uint32_t);
        if (!usedBytes || !result.indices) return result;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = usedBytes; info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device,&info,nullptr,&result.handle),"Creating chunk buffer");
        try {
            VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device,result.handle,&requirements);
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
            // Unified-memory GPUs can directly consume this allocation. On discrete
            // GPUs it deliberately trades peak throughput for cheap, bounded uploads.
            allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            result.bytes = requirements.size;
            check(vkAllocateMemory(device,&allocate,nullptr,&result.memory),"Allocating chunk buffer");
            check(vkBindBufferMemory(device,result.handle,result.memory,0),"Binding chunk buffer");
            void* mapped = nullptr;
            check(vkMapMemory(device,result.memory,0,usedBytes,0,&mapped),"Mapping chunk buffer");
            std::memcpy(mapped,mesh.mesh.vertices.data(),static_cast<size_t>(result.indexOffset));
            std::memcpy(static_cast<char*>(mapped)+result.indexOffset,mesh.mesh.indices.data(),mesh.mesh.indices.size()*sizeof(uint32_t));
            vkUnmapMemory(device,result.memory);
        } catch (...) { destroyBuffer(result); throw; }
        return result;
    }

    void updateChunks(const std::unordered_map<ChunkKey,ChunkMesh,ChunkKeyHash>& chunks, Frame& frame, const Camera& camera) {
        for (auto it = meshes.begin(); it != meshes.end();) {
            if (!chunks.contains(it->first)) { frame.retired.push_back(it->second); it = meshes.erase(it); }
            else ++it;
        }
        std::vector<const ChunkMesh*> pending;
        for (const auto& [key,mesh] : chunks) {
            const auto found = meshes.find(key);
            if (found == meshes.end() || found->second.revision != mesh.revision) pending.push_back(&mesh);
        }
        const auto distance = [&camera](const ChunkMesh* mesh) {
            const glm::vec3 delta = (mesh->min+mesh->max)*0.5f-camera.position;
            return glm::dot(delta,delta);
        };
        const size_t count = std::min<size_t>(2,pending.size());
        std::partial_sort(pending.begin(),pending.begin()+count,pending.end(),[&](const auto* a,const auto* b) { return distance(a)<distance(b); });
        for (size_t i = 0; i < count; ++i) {
            const auto& mesh = *pending[i];
            Buffer next = upload(mesh);
            auto found = meshes.find(mesh.key);
            if (found != meshes.end()) { frame.retired.push_back(found->second); found->second = next; }
            else meshes.emplace(mesh.key,next);
        }
    }

    void draw(const Camera& camera, const std::unordered_map<ChunkKey,ChunkMesh,ChunkKeyHash>& chunks,
              float timeOfDay, float fogDistance, bool requestedVsync) {
        ImGui::Render();
        int width = 0,height = 0; glfwGetFramebufferSize(window,&width,&height);
        if (width <= 0 || height <= 0) return;
        if (requestedVsync != vsync) { vsync = requestedVsync; resizeNeeded = true; }
        if (width != static_cast<int>(extent.width) || height != static_cast<int>(extent.height)) resizeNeeded = true;
        // Recreating ImGui invalidates the font descriptor referenced by this
        // frame's draw data. Drop this frame and rebuild UI on the next iteration.
        if (resizeNeeded) { recreateSwapchain(); return; }
        Frame& frame = frames[frameNumber % frameCount];
        check(vkWaitForFences(device,1,&frame.fence,VK_TRUE,UINT64_MAX),"Waiting for a frame");
        for (auto& buffer : frame.retired) destroyBuffer(buffer);
        frame.retired.clear();
        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,frame.acquired,VK_NULL_HANDLE,&imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) { resizeNeeded = true; return; }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) check(acquire,"Acquiring swapchain image");
        if (acquire == VK_SUBOPTIMAL_KHR) resizeNeeded = true;
        updateChunks(chunks,frame,camera);
        check(vkResetCommandBuffer(frame.command,0),"Resetting frame commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(frame.command,&begin),"Beginning frame commands");
        const float yaw = glm::radians(camera.yaw), pitch = glm::radians(camera.pitch);
        const glm::vec3 forward = glm::normalize(glm::vec3(std::cos(yaw)*std::cos(pitch),std::sin(pitch),std::sin(yaw)*std::cos(pitch)));
        const glm::vec3 right = glm::normalize(glm::cross(forward,glm::vec3(0,1,0)));
        const float aspect = static_cast<float>(extent.width)/static_cast<float>(extent.height);
        const float fov = glm::radians(std::clamp(camera.fov,35.0f,110.0f));
        glm::mat4 projection = glm::perspective(fov,aspect,0.08f,std::max(120.0f,fogDistance*1.8f));
        projection[1][1] *= -1;
        PushConstants push;
        push.viewProjection = projection * glm::lookAt(camera.position,camera.position+forward,glm::vec3(0,1,0));
        push.cameraTime = glm::vec4(camera.position,timeOfDay);
        const float angle = (timeOfDay-0.25f)*6.28318530718f;
        const glm::vec3 sun = glm::normalize(glm::vec3(std::cos(angle),std::sin(angle),0.32f));
        push.sunFog = glm::vec4(sun,std::max(32.0f,fogDistance));
        push.forwardAspect = glm::vec4(forward,aspect); push.rightTan = glm::vec4(right,std::tan(fov*0.5f));
        std::array<VkClearValue,2> clear{};
        clear[0].color = {{0.15f,0.25f,0.32f,1.0f}}; clear[1].depthStencil = {1,0};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass; pass.framebuffer = targets[imageIndex].framebuffer;
        pass.renderArea.extent = extent; pass.clearValueCount = 2; pass.pClearValues = clear.data();
        vkCmdBeginRenderPass(frame.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0,0,static_cast<float>(extent.width),static_cast<float>(extent.height),0,1};
        VkRect2D scissor{{0,0},extent};
        vkCmdSetViewport(frame.command,0,1,&viewport); vkCmdSetScissor(frame.command,0,1,&scissor);
        vkCmdPushConstants(frame.command,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
        vkCmdBindPipeline(frame.command,VK_PIPELINE_BIND_POINT_GRAPHICS,skyPipeline);
        vkCmdDraw(frame.command,3,1,0,0);
        vkCmdBindPipeline(frame.command,VK_PIPELINE_BIND_POINT_GRAPHICS,terrainPipeline);
        renderStats = {}; renderStats.residentChunks = meshes.size();
        visibleMeshes.clear();
        for (const auto& [key,buffer] : meshes) {
            (void)key;
            renderStats.gpuBytes += buffer.bytes;
            if (!buffer.indices || !visibleBounds(push.viewProjection,buffer.min,buffer.max)) continue;
            visibleMeshes.push_back(&buffer);
        }
        // Approximate front-to-back order lets early depth testing reject distant
        // covered terrain before the procedural material shader runs.
        const auto distanceSquared = [&camera](const Buffer* buffer) {
            const glm::vec3 delta = (buffer->min+buffer->max)*0.5f-camera.position;
            return glm::dot(delta,delta);
        };
        std::sort(visibleMeshes.begin(),visibleMeshes.end(),[&](const Buffer* a,const Buffer* b) {
            return distanceSquared(a) < distanceSquared(b);
        });
        for (const Buffer* visible : visibleMeshes) {
            const Buffer& buffer = *visible;
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(frame.command,0,1,&buffer.handle,&offset);
            vkCmdBindIndexBuffer(frame.command,buffer.handle,buffer.indexOffset,VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.command,buffer.indices,1,0,0,0);
            ++renderStats.drawCalls; ++renderStats.visibleChunks; renderStats.triangles += buffer.indices/3;
        }
        // Counters intentionally describe terrain only; UI and fullscreen sky are excluded.
        for (const auto& slot : frames) for (const auto& buffer : slot.retired) renderStats.gpuBytes += buffer.bytes;
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),frame.command);
        vkCmdEndRenderPass(frame.command);
        check(vkEndCommandBuffer(frame.command),"Ending frame commands");
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &frame.acquired; submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &frame.command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &targets[imageIndex].presented;
        // Reset only after successful acquisition. An out-of-date early return must
        // leave a signaled fence so the next attempt cannot deadlock.
        check(vkResetFences(device,1,&frame.fence),"Resetting frame fence");
        check(vkQueueSubmit(queue,1,&submit,frame.fence),"Submitting the frame");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &targets[imageIndex].presented;
        present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &imageIndex;
        const VkResult result = vkQueuePresentKHR(queue,&present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) resizeNeeded = true;
        else check(result,"Presenting the frame");
        ++frameNumber;
    }

    void destroyBuffer(Buffer& buffer) {
        if (buffer.handle) vkDestroyBuffer(device,buffer.handle,nullptr);
        if (buffer.memory) vkFreeMemory(device,buffer.memory,nullptr);
        buffer = {};
    }
    void destroySwapchain() {
        if (terrainPipeline) vkDestroyPipeline(device,terrainPipeline,nullptr);
        if (skyPipeline) vkDestroyPipeline(device,skyPipeline,nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
        terrainPipeline = skyPipeline = VK_NULL_HANDLE; pipelineLayout = VK_NULL_HANDLE;
        for (auto& target : targets) {
            if (target.framebuffer) vkDestroyFramebuffer(device,target.framebuffer,nullptr);
            if (target.depthView) vkDestroyImageView(device,target.depthView,nullptr);
            if (target.depth) vkDestroyImage(device,target.depth,nullptr);
            if (target.depthMemory) vkFreeMemory(device,target.depthMemory,nullptr);
            if (target.view) vkDestroyImageView(device,target.view,nullptr);
            if (target.presented) vkDestroySemaphore(device,target.presented,nullptr);
        }
        targets.clear();
        if (renderPass) vkDestroyRenderPass(device,renderPass,nullptr);
        if (swapchain) vkDestroySwapchainKHR(device,swapchain,nullptr);
        renderPass = VK_NULL_HANDLE; swapchain = VK_NULL_HANDLE;
    }
    void recreateSwapchain() {
        check(vkDeviceWaitIdle(device),"Waiting to resize the swapchain");
        // The image count and renderpass format can change when moving monitors.
        // Recreate the UI backend so it cannot retain stale render buffers/pipelines.
        if (imguiVulkan) { ImGui_ImplVulkan_Shutdown(); imguiVulkan = false; }
        destroySwapchain(); createSwapchain(); initializeUiRenderer();
    }
    void clearChunks() {
        if (!device) return;
        check(vkDeviceWaitIdle(device),"Waiting to unload world buffers");
        for (auto& [key,buffer] : meshes) { (void)key; destroyBuffer(buffer); }
        meshes.clear();
        for (auto& frame : frames) { for (auto& buffer : frame.retired) destroyBuffer(buffer); frame.retired.clear(); }
        renderStats = {};
    }
    void cleanup() noexcept {
        if (device) vkDeviceWaitIdle(device);
        if (imguiVulkan) ImGui_ImplVulkan_Shutdown();
        if (imguiGlfw) ImGui_ImplGlfw_Shutdown();
        if (imguiContext) ImGui::DestroyContext();
        if (device) {
            for (auto& [key,buffer] : meshes) { (void)key; destroyBuffer(buffer); }
            for (auto& frame : frames) {
                for (auto& buffer : frame.retired) destroyBuffer(buffer);
                if (frame.acquired) vkDestroySemaphore(device,frame.acquired,nullptr);
                if (frame.fence) vkDestroyFence(device,frame.fence,nullptr);
            }
            destroySwapchain();
            if (commandPool) vkDestroyCommandPool(device,commandPool,nullptr);
            vkDestroyDevice(device,nullptr);
        }
        if (surface) vkDestroySurfaceKHR(instance,surface,nullptr);
        if (debug && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(instance,debug,nullptr);
        if (instance) vkDestroyInstance(instance,nullptr);
    }
};

Renderer::Renderer(GLFWwindow* window, const std::filesystem::path& assets) : impl_(std::make_unique<Impl>()) {
    impl_->initialize(window,assets);
}
Renderer::~Renderer() = default;
void Renderer::beginUi() {
    ImGui_ImplVulkan_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
}
void Renderer::render(const Camera& camera,const std::unordered_map<ChunkKey,ChunkMesh,ChunkKeyHash>& chunks,
    float timeOfDay,float fogDistance,bool vsync) { impl_->draw(camera,chunks,timeOfDay,fogDistance,vsync); }
RenderStats Renderer::stats() const { return impl_->renderStats; }
const std::string& Renderer::deviceName() const { return impl_->gpuName; }
void Renderer::waitIdle() { check(vkDeviceWaitIdle(impl_->device),"Waiting for the graphics device"); }
void Renderer::invalidateChunks() { impl_->clearChunks(); }
}
