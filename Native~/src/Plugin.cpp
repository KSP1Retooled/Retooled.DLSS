#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Unity's D3D11 and D3D12 headers expect the Direct3D and DXGI types to be declared already.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include "Dlss.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityGraphicsVulkan.h"
#include "IUnityInterface.h"
#include "IUnityLog.h"
#include "VulkanIntercept.h"

namespace
{
    // Frames that the main thread may prepare before the render thread has evaluated the oldest of them.
    constexpr int FrameSlots = 8;

    enum class Stage : int32_t
    {
        Pending = 0,
        Ready = 1,
        Failed = -1,
    };

    enum Event
    {
        CreateEvent = 0,
        EvaluateEvent = 1,
        EndFrameEvent = 2,
        DestroyEvent = 3,
    };

    // Shared with C# as DlssLogLevel.
    enum LogLevel : int32_t
    {
        LogOff = 0,
        LogWarnings = 1,
        LogInformation = 2,
        LogVerbose = 3,
    };

    // Shared with C# as DlssNative.Status, field for field.
    struct Status
    {
        Stage stage = Stage::Pending;
        int32_t configureResult = dlss::NotRun;
        int32_t evaluateResult = dlss::NotRun;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t evaluations = 0;
        uint64_t videoMemory = 0;
    };

    struct Context;

    // The data of an evaluate event. The main thread fills a slot each frame and the render thread reads it when it
    // reaches that frame's commands.
    struct FrameSlot
    {
        Context* context;
        dlss::Frame frame;
    };

    // The main thread writes the options and frame slots and reads the status. The render thread does the opposite.
    struct Context
    {
        std::mutex mutex;
        Status status;
        dlss::Options options{};
        uint32_t viewport = 0;
        FrameSlot slots[FrameSlots]{};
    };

    IUnityInterfaces* interfaces;
    IUnityGraphics* graphics;
    IUnityLog* unityLog;
    IUnityGraphicsD3D11* d3d11;
    IUnityGraphicsD3D12v8* d3d12;
    IUnityGraphicsVulkanV2* vulkan;
    UnityGfxRenderer renderer = kUnityGfxRendererNull;

    // An image view that a context's destruction left behind, with the frame that Unity was recording then. Frames in
    // flight may still use the view, so it waits until Unity says that frame is done.
    struct RetiredView
    {
        VkImageView view;
        unsigned long long frame;
    };

    // Streamline wants a view of each Vulkan texture, and Unity only hands out the images. Used on the render thread
    // only.
    UnityVulkanInstance vulkanInstance{};
    PFN_vkCreateImageView vulkanCreateImageView;
    PFN_vkDestroyImageView vulkanDestroyImageView;
    std::unordered_map<VkImage, VkImageView> vulkanViews;
    std::vector<RetiredView> vulkanRetiredViews;
    unsigned long long vulkanCurrentFrame;

    std::mutex sessionMutex;
    dlss::Session session;
    uint32_t applicationId;
    std::string projectId;
    std::string engineVersion;
    std::wstring runtimeDirectory;
    std::atomic<int32_t> logLevel{LogWarnings};
    std::atomic<uint32_t> nextViewport{1};

    // Streamline may call this from its own threads. Unity's log accepts messages from any thread.
    void OnLog(int32_t type, const char* message)
    {
        // Streamline reports every plugin, hook and model that it loads as information.
        bool information = type == 0;
        if (unityLog == nullptr || (information && logLevel < LogInformation))
            return;

        std::string text = std::string("[DLSS] ") + message;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        UnityLogType unityType = type == 2 ? kUnityLogTypeError : type == 1 ? kUnityLogTypeWarning : kUnityLogTypeLog;
        unityLog->Log(unityType, text.c_str(), "", 0);
    }

    void SetUpVulkan()
    {
        vulkanInstance = vulkan->Instance();
        auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            vulkanInstance.getInstanceProcAddr(vulkanInstance.instance, "vkGetDeviceProcAddr"));
        vulkanCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
            getDeviceProcAddr(vulkanInstance.device, "vkCreateImageView"));
        vulkanDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
            getDeviceProcAddr(vulkanInstance.device, "vkDestroyImageView"));
        vulkanCurrentFrame = 0;

        // DLSS dispatches compute work, which can't happen inside a render pass, and binds its own state.
        UnityVulkanPluginEventConfig config{};
        config.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
        config.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
        config.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission
            | kUnityVulkanEventConfigFlag_ModifiesCommandBuffersState;
        vulkan->ConfigureEvent(EvaluateEvent, &config);
    }

    void DestroyVulkanViews()
    {
        for (auto& [image, view] : vulkanViews)
            vulkanDestroyImageView(vulkanInstance.device, view, nullptr);
        for (const RetiredView& retired : vulkanRetiredViews)
            vulkanDestroyImageView(vulkanInstance.device, retired.view, nullptr);
        vulkanViews.clear();
        vulkanRetiredViews.clear();
    }

    void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType type)
    {
        if (type == kUnityGfxDeviceEventInitialize)
        {
            renderer = graphics->GetRenderer();
            d3d11 = nullptr;
            d3d12 = nullptr;
            vulkan = nullptr;
            if (renderer == kUnityGfxRendererVulkan)
                vulkan = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsVulkanV2);
            if (vulkan != nullptr)
                SetUpVulkan();
            // Version 8 is the first that lets a plugin ask Unity for a resource state.
            if (renderer == kUnityGfxRendererD3D11)
                d3d11 = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsD3D11);
            if (renderer == kUnityGfxRendererD3D12)
                d3d12 = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsD3D12v8);
        }
        else if (type == kUnityGfxDeviceEventShutdown)
        {
            // Streamline must shut down before the device is destroyed.
            dlss::Stop();
            if (vulkan != nullptr)
                DestroyVulkanViews();
            std::lock_guard lock(sessionMutex);
            session = dlss::Session{};
            renderer = kUnityGfxRendererNull;
            d3d11 = nullptr;
            d3d12 = nullptr;
            vulkan = nullptr;
        }
    }

    // Starts Streamline on Unity's device at the first use, so that a game which never selects DLSS never loads it.
    dlss::Session StartSession()
    {
        dlss::Session started;
        if (vulkan != nullptr && !VulkanDeviceReady())
        {
            std::string note = VulkanInterceptNote();
            if (note.empty())
                note = "the plugin loaded after the Vulkan device was created";
            OnLog(1, ("DLSS is unavailable on Vulkan: " + note).c_str());
            started.initResult = dlss::ErrorVulkanDeviceNotReady;
        }
        else if (d3d11 != nullptr || d3d12 != nullptr || vulkan != nullptr)
        {
            std::string project;
            std::string version;
            std::wstring runtime;
            dlss::Application application{};
            {
                std::lock_guard lock(sessionMutex);
                project = projectId;
                version = engineVersion;
                runtime = runtimeDirectory;
                application.applicationId = applicationId;
            }
            application.projectId = project.c_str();
            application.engineVersion = version.c_str();
            application.runtimeDirectory = runtime.c_str();

            dlss::VulkanDevice vulkanDevice{vulkanInstance.instance, vulkanInstance.physicalDevice,
                vulkanInstance.device, vulkanInstance.queueFamilyIndex};
            void* device = vulkan != nullptr ? static_cast<void*>(&vulkanDevice)
                : d3d12 != nullptr ? static_cast<void*>(d3d12->GetDevice())
                : d3d11->GetDevice();
            dlss::GraphicsApi api = vulkan != nullptr ? dlss::GraphicsApi::Vulkan
                : d3d12 != nullptr ? dlss::GraphicsApi::Direct3D12
                : dlss::GraphicsApi::Direct3D11;
            // An sl::LogLevel: off, default or verbose.
            int32_t streamlineLevel = logLevel == LogOff ? 0 : logLevel == LogVerbose ? 2 : 1;
            started = dlss::Start(device, api, application, streamlineLevel, OnLog);
        }
        else
        {
            started.initResult = dlss::ErrorUnsupportedGraphicsApi;
        }

        std::lock_guard lock(sessionMutex);
        session = started;
        return started;
    }

    void Create(Context& context)
    {
        dlss::Session started = StartSession();
        dlss::Options options;
        {
            std::lock_guard lock(context.mutex);
            options = context.options;
        }

        int32_t result = dlss::NotRun;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        if (started.Started())
            result = dlss::Configure(context.viewport, options, &renderWidth, &renderHeight);

        std::lock_guard lock(context.mutex);
        context.status.configureResult = result;
        context.status.renderWidth = renderWidth;
        context.status.renderHeight = renderHeight;
        context.status.stage = result == dlss::Ok ? Stage::Ready : Stage::Failed;
    }

    VkImageView VulkanView(const UnityVulkanImage& image)
    {
        auto found = vulkanViews.find(image.image);
        if (found != vulkanViews.end())
            return found->second;

        VkImageViewCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        create.image = image.image;
        create.viewType = VK_IMAGE_VIEW_TYPE_2D;
        create.format = image.format;
        create.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vulkanCreateImageView(vulkanInstance.device, &create, nullptr, &view);
        vulkanViews.emplace(image.image, view);
        return view;
    }

    // Has Unity move a texture into the layout that DLSS is told about, with a barrier in the command buffer it is
    // recording.
    bool AccessVulkanImage(void* texture, VkImageLayout layout, VkAccessFlags access, dlss::VulkanImage& image)
    {
        UnityVulkanImage unityImage{};
        VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        if (!vulkan->AccessTexture(texture, &subresource, layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, access,
            kUnityVulkanResourceAccess_PipelineBarrier, &unityImage))
            return false;

        image.image = unityImage.image;
        image.view = VulkanView(unityImage);
        image.layout = static_cast<uint32_t>(layout);
        image.format = static_cast<uint32_t>(unityImage.format);
        image.usage = unityImage.usage;
        image.width = unityImage.extent.width;
        image.height = unityImage.extent.height;
        image.mipLevels = static_cast<uint32_t>(unityImage.mipCount);
        image.layers = static_cast<uint32_t>(unityImage.layers);
        return true;
    }

    void DestroyRetiredVulkanViews(unsigned long long safeFrame)
    {
        std::erase_if(vulkanRetiredViews, [&](const RetiredView& retired)
        {
            if (retired.frame > safeFrame)
                return false;
            vulkanDestroyImageView(vulkanInstance.device, retired.view, nullptr);
            return true;
        });
    }

    int32_t EvaluateVulkan(uint32_t viewport, const dlss::Frame& frame)
    {
        const VkImageLayout readLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkAccessFlags read = VK_ACCESS_SHADER_READ_BIT;
        dlss::VulkanImage color{};
        dlss::VulkanImage depth{};
        dlss::VulkanImage motion{};
        dlss::VulkanImage output{};
        if (!AccessVulkanImage(frame.color, readLayout, read, color)
            || !AccessVulkanImage(frame.depth, readLayout, read, depth)
            || !AccessVulkanImage(frame.motion, readLayout, read, motion)
            || !AccessVulkanImage(frame.output, VK_IMAGE_LAYOUT_GENERAL, read | VK_ACCESS_SHADER_WRITE_BIT, output))
            return dlss::ErrorTextureAccess;

        // Accessing textures may record barriers, so the recording state is read afterwards.
        UnityVulkanRecordingState recording{};
        if (!vulkan->CommandRecordingState(&recording, kUnityVulkanGraphicsQueueAccess_DontCare))
            return dlss::NotRun;
        DestroyRetiredVulkanViews(recording.safeFrameNumber);
        vulkanCurrentFrame = recording.currentFrameNumber;
        return dlss::EvaluateVulkan(viewport, frame, color, depth, motion, output, recording.commandBuffer);
    }

    void Evaluate(const FrameSlot& slot)
    {
        Context& context = *slot.context;
        dlss::Frame frame;
        {
            std::lock_guard lock(context.mutex);
            if (context.status.stage != Stage::Ready)
                return;
            frame = slot.frame;
        }

        if (vulkan != nullptr)
        {
            int32_t vulkanResult = EvaluateVulkan(context.viewport, frame);
            if (vulkanResult == dlss::NotRun)
                return;

            uint64_t vulkanMemory = dlss::VideoMemory(context.viewport);
            std::lock_guard lock(context.mutex);
            context.status.evaluateResult = vulkanResult;
            context.status.videoMemory = vulkanMemory;
            context.status.evaluations++;
            return;
        }

        void* commandList = nullptr;
        if (d3d12 != nullptr)
        {
            UnityGraphicsD3D12RecordingState recording{};
            if (!d3d12->CommandRecordingState(&recording))
                return;
            commandList = recording.commandList;

            // Unity adds the barriers to its command list and tracks the new states. DLSS returns the textures in
            // the states that it is told about here.
            frame.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            frame.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            frame.motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            frame.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.color), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.depth), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.motion), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.output), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        int32_t result = dlss::Evaluate(context.viewport, frame, commandList);
        uint64_t videoMemory = dlss::VideoMemory(context.viewport);

        std::lock_guard lock(context.mutex);
        context.status.evaluateResult = result;
        context.status.videoMemory = videoMemory;
        context.status.evaluations++;
    }

    void Destroy(Context* context)
    {
        if (context->status.stage == Stage::Ready)
            dlss::Free(context->viewport);

        // The context's textures are about to go, and a later texture may get the handle of one of them.
        for (auto& [image, view] : vulkanViews)
            vulkanRetiredViews.push_back({view, vulkanCurrentFrame});
        vulkanViews.clear();
        delete context;
    }

    void UNITY_INTERFACE_API OnRenderEvent(int event, void* data)
    {
        switch (event)
        {
        case CreateEvent:
            Create(*static_cast<Context*>(data));
            break;
        case EvaluateEvent:
            Evaluate(*static_cast<FrameSlot*>(data));
            break;
        case EndFrameEvent:
            dlss::EndFrame();
            break;
        case DestroyEvent:
            Destroy(static_cast<Context*>(data));
            break;
        default:
            break;
        }
    }
}

extern "C"
{
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
    {
        interfaces = unityInterfaces;
        graphics = UNITY_GET_INTERFACE(interfaces, IUnityGraphics);
        unityLog = UNITY_GET_INTERFACE(interfaces, IUnityLog);
        graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
        RegisterVulkanIntercept(interfaces);

        // A plugin loaded after the device was created misses the initialize event.
        OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
    {
        graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    }

    UNITY_INTERFACE_EXPORT int RetooledDLSS_GetRenderer()
    {
        return renderer;
    }

    // Whether DLSS can run on Unity's Vulkan device. Meaningful once Unity has created the device.
    UNITY_INTERFACE_EXPORT int RetooledDLSS_IsVulkanDeviceReady()
    {
        return VulkanDeviceReady() ? 1 : 0;
    }

    // Applies when Streamline starts, which is when the first context is created.
    UNITY_INTERFACE_EXPORT void RetooledDLSS_SetApplication(uint32_t id, const char* project, const char* version)
    {
        std::lock_guard lock(sessionMutex);
        applicationId = id;
        projectId = project != nullptr ? project : "";
        engineVersion = version != nullptr ? version : "";
    }

    // Applies when Streamline starts, which is when the first context is created. The path is UTF-8.
    UNITY_INTERFACE_EXPORT void RetooledDLSS_SetRuntimeDirectory(const char* directory)
    {
        std::wstring wide;
        int length = directory != nullptr ? MultiByteToWideChar(CP_UTF8, 0, directory, -1, nullptr, 0) : 0;
        if (length > 1)
        {
            wide.resize(length);
            MultiByteToWideChar(CP_UTF8, 0, directory, -1, wide.data(), length);
            wide.resize(length - 1);
        }
        std::lock_guard lock(sessionMutex);
        runtimeDirectory = wide;
    }

    // Applies when Streamline starts, which is when the first context is created.
    UNITY_INTERFACE_EXPORT void RetooledDLSS_SetLogLevel(int level)
    {
        logLevel = level;
    }

    UNITY_INTERFACE_EXPORT void RetooledDLSS_GetSession(dlss::Session* result)
    {
        std::lock_guard lock(sessionMutex);
        *result = session;
    }

    UNITY_INTERFACE_EXPORT UnityRenderingEventAndData RetooledDLSS_GetEventFunction()
    {
        return OnRenderEvent;
    }

    // The context is freed by its destroy event on the render thread.
    UNITY_INTERFACE_EXPORT Context* RetooledDLSS_NewContext(const dlss::Options* options)
    {
        auto* context = new Context();
        context->options = *options;
        context->viewport = nextViewport++;
        for (FrameSlot& slot : context->slots)
            slot.context = context;
        return context;
    }

    UNITY_INTERFACE_EXPORT void RetooledDLSS_GetStatus(Context* context, Status* status)
    {
        std::lock_guard lock(context->mutex);
        *status = context->status;
    }

    // Stores a frame's inputs in a slot and returns the slot, which is the data of that frame's evaluate event.
    UNITY_INTERFACE_EXPORT FrameSlot* RetooledDLSS_SetFrame(Context* context, int index, const dlss::Frame* frame)
    {
        FrameSlot& slot = context->slots[static_cast<uint32_t>(index) % FrameSlots];
        std::lock_guard lock(context->mutex);
        slot.frame = *frame;
        return &slot;
    }
}
