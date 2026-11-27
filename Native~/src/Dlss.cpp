#include "Dlss.h"

#include <unordered_map>

#include "StreamlineLibrary.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

namespace
{
    // The hooks that Streamline's common plugin runs around IDXGISwapChain::Present.
    using PresentHook = HRESULT(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, bool& skip);
    using AfterPresentHook = HRESULT(UINT flags);

    // The hooks that it runs around vkQueuePresentKHR.
    using VulkanPresentHook = VkResult(VkQueue queue, const VkPresentInfoKHR* presentInfo, bool& skip);
    using VulkanAfterPresentHook = VkResult();

    struct State
    {
        const StreamlineLibrary* sl = nullptr;
        dlss::Session session;
        dlss::GraphicsApi api = dlss::GraphicsApi::Direct3D11;
        dlss::LogFunction* log = nullptr;
        ID3D11DeviceContext* immediateContext = nullptr;
        PFun_slDLSSSetOptions* setOptions = nullptr;
        PFun_slDLSSGetOptimalSettings* getOptimalSettings = nullptr;
        PFun_slDLSSGetState* getState = nullptr;
        PresentHook* present = nullptr;
        AfterPresentHook* afterPresent = nullptr;
        VulkanPresentHook* vulkanPresent = nullptr;
        VulkanAfterPresentHook* vulkanAfterPresent = nullptr;
        uint32_t frameIndex = 0;
        // The frame in which each viewport was last evaluated.
        std::unordered_map<uint32_t, uint32_t> evaluated;
        bool started = false;
        bool initialized = false;
    };

    State state;

    void OnStreamlineLog(sl::LogType type, const char* message)
    {
        if (state.log != nullptr)
            state.log(static_cast<int32_t>(type), message);
    }

    bool FindAdapter(void* device, dlss::GraphicsApi api, LUID& luid)
    {
        if (api == dlss::GraphicsApi::Direct3D12)
        {
            luid = static_cast<ID3D12Device*>(device)->GetAdapterLuid();
            return true;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        if (FAILED(static_cast<ID3D11Device*>(device)->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
            return false;

        IDXGIAdapter* adapter = nullptr;
        DXGI_ADAPTER_DESC description{};
        bool found = SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&description));
        if (adapter != nullptr)
            adapter->Release();
        dxgiDevice->Release();
        luid = description.AdapterLuid;
        return found;
    }

    template <typename T>
    int32_t Import(sl::Feature feature, const char* name, T*& function)
    {
        void* address = nullptr;
        sl::Result result = state.sl->GetFeatureFunction(feature, name, address);
        function = static_cast<T*>(address);
        return static_cast<int32_t>(result);
    }

    int32_t ImportDlssFunctions()
    {
        int32_t result = Import(sl::kFeatureDLSS, "slDLSSSetOptions", state.setOptions);
        if (result == dlss::Ok)
            result = Import(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", state.getOptimalSettings);
        if (result == dlss::Ok)
            result = Import(sl::kFeatureDLSS, "slDLSSGetState", state.getState);
        return result;
    }

    // Streamline counts frames, frees resources that were released a few frames ago, and tracks its video memory
    // budget when a frame is presented. It normally learns of a present by wrapping the application's swap chain, or
    // on Vulkan by standing in for vkQueuePresentKHR. A plugin cannot give Unity either, so EndFrame calls the
    // present hooks of Streamline's common plugin directly. The hook names are part of the plugin interface between
    // sl.interposer.dll and sl.common.dll, so they are tied to the Streamline version that ships with this plugin.
    int32_t ImportPresentHooks()
    {
        int32_t result;
        if (state.api == dlss::GraphicsApi::Vulkan)
        {
            result = Import(sl::kFeatureCommon, "slHookVkPresent", state.vulkanPresent);
            if (result == dlss::Ok)
                result = Import(sl::kFeatureCommon, "slHookVkAfterPresent", state.vulkanAfterPresent);
        }
        else
        {
            result = Import(sl::kFeatureCommon, "slHookPresent", state.present);
            if (result == dlss::Ok)
                result = Import(sl::kFeatureCommon, "slHookAfterPresent", state.afterPresent);
        }

        if (result != dlss::Ok)
        {
            state.present = nullptr;
            state.afterPresent = nullptr;
            state.vulkanPresent = nullptr;
            state.vulkanAfterPresent = nullptr;
        }
        return result;
    }

    sl::float4x4 Identity()
    {
        sl::float4x4 matrix;
        matrix.setRow(0, {1.0f, 0.0f, 0.0f, 0.0f});
        matrix.setRow(1, {0.0f, 1.0f, 0.0f, 0.0f});
        matrix.setRow(2, {0.0f, 0.0f, 1.0f, 0.0f});
        matrix.setRow(3, {0.0f, 0.0f, 0.0f, 1.0f});
        return matrix;
    }

    sl::Boolean ToBoolean(bool value)
    {
        return value ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    }

    // Streamline wants every constant set. DLSS Super Resolution reads the jitter, the motion scale and the flags. It
    // reads the camera only when it has to add camera motion to the motion vectors, and the caller's vectors already
    // include it. The camera values here are placeholders for that reason.
    sl::Constants ToConstants(const dlss::Frame& frame)
    {
        sl::Constants constants{};
        constants.cameraViewToClip = Identity();
        constants.clipToCameraView = Identity();
        constants.clipToLensClip = Identity();
        constants.clipToPrevClip = Identity();
        constants.prevClipToClip = Identity();
        constants.jitterOffset = {frame.jitterX, frame.jitterY};
        constants.mvecScale = {frame.motionScaleX, frame.motionScaleY};
        constants.cameraPinholeOffset = {0.0f, 0.0f};
        constants.cameraPos = {0.0f, 0.0f, 0.0f};
        constants.cameraUp = {0.0f, 1.0f, 0.0f};
        constants.cameraRight = {1.0f, 0.0f, 0.0f};
        constants.cameraFwd = {0.0f, 0.0f, 1.0f};
        constants.cameraNear = 0.1f;
        constants.cameraFar = 1000.0f;
        constants.cameraFOV = 1.0f;
        constants.cameraAspectRatio = static_cast<float>(frame.renderWidth) / static_cast<float>(frame.renderHeight);
        constants.motionVectorsInvalidValue = 0.0f;
        constants.depthInverted = ToBoolean((frame.flags & dlss::FrameDepthInverted) != 0);
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        constants.reset = ToBoolean((frame.flags & dlss::FrameReset) != 0);
        constants.motionVectorsJittered = ToBoolean((frame.flags & dlss::FrameMotionJittered) != 0);
        return constants;
    }

    sl::DLSSOptions ToOptions(const dlss::Options& options)
    {
        auto preset = static_cast<sl::DLSSPreset>(options.preset);
        sl::DLSSOptions result{};
        result.mode = static_cast<sl::DLSSMode>(options.mode);
        result.outputWidth = options.outputWidth;
        result.outputHeight = options.outputHeight;
        result.colorBuffersHDR = ToBoolean((options.flags & dlss::OptionHdr) != 0);
        result.useAutoExposure = ToBoolean((options.flags & dlss::OptionAutoExposure) != 0);
        result.dlaaPreset = preset;
        result.qualityPreset = preset;
        result.balancedPreset = preset;
        result.performancePreset = preset;
        result.ultraPerformancePreset = preset;
        result.ultraQualityPreset = preset;
        return result;
    }

    sl::Resource ToResource(void* texture, uint32_t d3d12State)
    {
        // Streamline wants state zero for every Direct3D 11 resource.
        uint32_t resourceState = state.api == dlss::GraphicsApi::Direct3D12 ? d3d12State : 0;
        return sl::Resource(sl::ResourceType::eTex2d, texture, nullptr, nullptr, resourceState);
    }

    // Streamline can't ask Vulkan what an image is like, so the resource carries its description.
    sl::Resource ToResource(const dlss::VulkanImage& image)
    {
        sl::Resource resource(sl::ResourceType::eTex2d, image.image, nullptr, image.view, image.layout);
        resource.width = image.width;
        resource.height = image.height;
        resource.nativeFormat = image.format;
        resource.mipLevels = image.mipLevels;
        resource.arrayLayers = image.layers;
        resource.flags = 0;
        resource.usage = image.usage;
        return resource;
    }

    sl::RenderAPI ToRenderApi(dlss::GraphicsApi api)
    {
        switch (api)
        {
        case dlss::GraphicsApi::Direct3D12:
            return sl::RenderAPI::eD3D12;
        case dlss::GraphicsApi::Vulkan:
            return sl::RenderAPI::eVulkan;
        default:
            return sl::RenderAPI::eD3D11;
        }
    }

    // Gives Streamline the device and returns whether DLSS runs on its GPU.
    void SetDevice(void* device, dlss::GraphicsApi api, dlss::Session& session)
    {
        sl::AdapterInfo adapter{};
        LUID luid{};
        if (api == dlss::GraphicsApi::Vulkan)
        {
            // DLSS asks Streamline for no queues of its own, so the indices only say where such queues would start.
            auto* vulkan = static_cast<const dlss::VulkanDevice*>(device);
            sl::VulkanInfo info{};
            info.device = static_cast<VkDevice>(vulkan->device);
            info.instance = static_cast<VkInstance>(vulkan->instance);
            info.physicalDevice = static_cast<VkPhysicalDevice>(vulkan->physicalDevice);
            info.graphicsQueueFamily = vulkan->queueFamily;
            info.computeQueueFamily = vulkan->queueFamily;
            session.deviceResult = static_cast<int32_t>(state.sl->SetVulkanInfo(info));
            adapter.vkPhysicalDevice = vulkan->physicalDevice;
        }
        else
        {
            session.deviceResult = static_cast<int32_t>(state.sl->SetD3DDevice(device));
            if (session.deviceResult == dlss::Ok && !FindAdapter(device, api, luid))
            {
                session.supportResult = static_cast<int32_t>(sl::Result::eErrorNoSupportedAdapterFound);
                return;
            }
            adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
            adapter.deviceLUIDSizeInBytes = sizeof(LUID);
        }

        if (session.deviceResult == dlss::Ok)
            session.supportResult = static_cast<int32_t>(state.sl->IsFeatureSupported(sl::kFeatureDLSS, adapter));
    }

    int32_t EvaluateResources(uint32_t viewport, const dlss::Frame& frame, sl::Resource& color, sl::Resource& depth,
        sl::Resource& motion, sl::Resource& output, void* commands)
    {
        if (!state.session.Started())
            return dlss::ErrorNotStarted;

        // Streamline accepts one set of constants per viewport and frame. A second evaluation without a present in
        // between, such as an editor view that repaints twice, counts as a new frame.
        auto last = state.evaluated.find(viewport);
        if (last != state.evaluated.end() && last->second == state.frameIndex)
            dlss::EndFrame();
        state.evaluated[viewport] = state.frameIndex;

        sl::FrameToken* token = nullptr;
        sl::Result result = state.sl->GetNewFrameToken(token, &state.frameIndex);
        if (result != sl::Result::eOk)
            return static_cast<int32_t>(result);

        sl::ViewportHandle handle(viewport);
        result = state.sl->SetConstants(ToConstants(frame), *token, handle);
        if (result != sl::Result::eOk)
            return static_cast<int32_t>(result);

        sl::Extent renderExtent{0, 0, frame.renderWidth, frame.renderHeight};
        sl::Extent outputExtent{0, 0, frame.outputWidth, frame.outputHeight};

        // The tags are passed with the call, so they apply to this evaluation only.
        const sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eValidUntilEvaluate;
        sl::ResourceTag colorTag(&color, sl::kBufferTypeScalingInputColor, lifecycle, &renderExtent);
        sl::ResourceTag depthTag(&depth, sl::kBufferTypeDepth, lifecycle, &renderExtent);
        sl::ResourceTag motionTag(&motion, sl::kBufferTypeMotionVectors, lifecycle, &renderExtent);
        sl::ResourceTag outputTag(&output, sl::kBufferTypeScalingOutputColor, lifecycle, &outputExtent);
        const sl::BaseStructure* inputs[] = {&handle, &colorTag, &depthTag, &motionTag, &outputTag};

        result = state.sl->EvaluateFeature(sl::kFeatureDLSS, *token, inputs, 5, commands);
        return static_cast<int32_t>(result);
    }
}

namespace dlss
{
    const Session& Start(void* device, GraphicsApi api, const Application& application, int32_t logLevel, LogFunction* log)
    {
        Session& session = state.session;
        if (state.started)
            return session;

        state.started = true;
        state.api = api;
        state.log = log;

        bool hasProject = application.projectId != nullptr && application.projectId[0] != '\0';
        if (application.applicationId == 0 && !hasProject)
        {
            session.initResult = ErrorNoApplicationIdentity;
            return session;
        }

        StreamlineLoad load = LoadStreamlineLibrary(&state.sl);
        if (load != StreamlineLoad::Loaded)
        {
            session.initResult = load == StreamlineLoad::NotSigned ? ErrorLibraryNotSigned : ErrorLibraryMissing;
            return session;
        }

        // Streamline loads its plugins from the first folder that has them, and NGX looks for nvngx_dlss.dll in each
        // folder in turn.
        bool hasRuntimeDirectory = application.runtimeDirectory != nullptr && application.runtimeDirectory[0] != L'\0';
        const wchar_t* pluginPaths[] = {ModuleDirectory(), application.runtimeDirectory};
        const sl::Feature features[] = {sl::kFeatureDLSS};
        sl::Preferences preferences{};
        preferences.logLevel = static_cast<sl::LogLevel>(logLevel);
        preferences.pathsToPlugins = pluginPaths;
        preferences.numPathsToPlugins = hasRuntimeDirectory ? 2 : 1;
        preferences.logMessageCallback = OnStreamlineLog;
        // Manual hooking keeps Streamline out of the device and swap chain that the engine created. The default flags
        // also download updated plugins and models, which the application has not asked for.
        preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
        preferences.featuresToLoad = features;
        preferences.numFeaturesToLoad = 1;
        preferences.applicationId = application.applicationId;
        preferences.engine = sl::EngineType::eCustom;
        preferences.engineVersion = application.engineVersion;
        preferences.projectId = application.projectId;
        preferences.renderAPI = ToRenderApi(api);

        session.initResult = static_cast<int32_t>(state.sl->Init(preferences, sl::kSDKVersion));
        if (session.initResult != Ok)
            return session;
        state.initialized = true;

        SetDevice(device, api, session);
        if (session.deviceResult != Ok || session.supportResult != Ok)
            return session;

        sl::FeatureVersion version{};
        if (state.sl->GetFeatureVersion(sl::kFeatureDLSS, version) == sl::Result::eOk)
        {
            session.streamlineVersion[0] = version.versionSL.major;
            session.streamlineVersion[1] = version.versionSL.minor;
            session.streamlineVersion[2] = version.versionSL.build;
            session.ngxVersion[0] = version.versionNGX.major;
            session.ngxVersion[1] = version.versionNGX.minor;
            session.ngxVersion[2] = version.versionNGX.build;
        }

        session.functionsResult = ImportDlssFunctions();
        if (session.functionsResult != Ok)
            return session;

        session.presentHookResult = ImportPresentHooks();
        if (api == GraphicsApi::Direct3D11)
            static_cast<ID3D11Device*>(device)->GetImmediateContext(&state.immediateContext);
        return session;
    }

    const Session& Current()
    {
        return state.session;
    }

    void Stop()
    {
        if (state.immediateContext != nullptr)
            state.immediateContext->Release();
        if (state.initialized)
            state.sl->Shutdown();
        state = State{};
    }

    int32_t Configure(uint32_t viewport, const Options& options, uint32_t* renderWidth, uint32_t* renderHeight)
    {
        if (!state.session.Started())
            return ErrorNotStarted;

        sl::DLSSOptions dlssOptions = ToOptions(options);
        sl::DLSSOptimalSettings optimal{};
        sl::Result result = state.getOptimalSettings(dlssOptions, optimal);
        if (result != sl::Result::eOk)
            return static_cast<int32_t>(result);

        *renderWidth = optimal.optimalRenderWidth;
        *renderHeight = optimal.optimalRenderHeight;
        return static_cast<int32_t>(state.setOptions(sl::ViewportHandle(viewport), dlssOptions));
    }

    int32_t Evaluate(uint32_t viewport, const Frame& frame, void* commandBuffer)
    {
        sl::Resource color = ToResource(frame.color, frame.colorState);
        sl::Resource depth = ToResource(frame.depth, frame.depthState);
        sl::Resource motion = ToResource(frame.motion, frame.motionState);
        sl::Resource output = ToResource(frame.output, frame.outputState);
        void* commands = state.api == GraphicsApi::Direct3D11 ? state.immediateContext : commandBuffer;
        return EvaluateResources(viewport, frame, color, depth, motion, output, commands);
    }

    int32_t EvaluateVulkan(uint32_t viewport, const Frame& frame, const VulkanImage& color, const VulkanImage& depth,
        const VulkanImage& motion, const VulkanImage& output, void* commandBuffer)
    {
        sl::Resource colorResource = ToResource(color);
        sl::Resource depthResource = ToResource(depth);
        sl::Resource motionResource = ToResource(motion);
        sl::Resource outputResource = ToResource(output);
        return EvaluateResources(
            viewport, frame, colorResource, depthResource, motionResource, outputResource, commandBuffer);
    }

    int32_t EndFrame()
    {
        if (!state.session.Started())
            return ErrorNotStarted;

        state.frameIndex++;
        if (state.api == GraphicsApi::Vulkan)
        {
            if (state.vulkanPresent == nullptr)
                return state.session.presentHookResult;

            bool skipped = false;
            VkResult presented = state.vulkanPresent(VK_NULL_HANDLE, nullptr, skipped);
            VkResult after = state.vulkanAfterPresent();
            bool vulkanFailed = presented != VK_SUCCESS || after != VK_SUCCESS;
            return vulkanFailed ? static_cast<int32_t>(sl::Result::eErrorVulkanAPI) : Ok;
        }

        if (state.present == nullptr)
            return state.session.presentHookResult;

        bool skip = false;
        HRESULT presented = state.present(nullptr, 0, 0, skip);
        HRESULT after = state.afterPresent(0);
        bool failed = FAILED(presented) || FAILED(after);
        return failed ? static_cast<int32_t>(sl::Result::eErrorDXGIAPI) : Ok;
    }

    int32_t Free(uint32_t viewport)
    {
        if (!state.session.Started())
            return ErrorNotStarted;

        state.evaluated.erase(viewport);
        return static_cast<int32_t>(state.sl->FreeResources(sl::kFeatureDLSS, sl::ViewportHandle(viewport)));
    }

    uint64_t VideoMemory(uint32_t viewport)
    {
        if (!state.session.Started())
            return 0;

        sl::DLSSState dlssState{};
        if (state.getState(sl::ViewportHandle(viewport), dlssState) != sl::Result::eOk)
            return 0;
        return dlssState.estimatedVRAMUsageInBytes;
    }
}
