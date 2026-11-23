#pragma once

#include <cstdint>

// DLSS Super Resolution through Streamline, without anything from Unity. Plugin.cpp calls this from Unity's render
// thread, and the test harness calls it on devices that it creates itself.
//
// Streamline is not thread-safe here: every function must be called from the thread that owns the graphics device.
namespace dlss
{
    // Results are sl::Result values, plus the codes below for steps that never reached Streamline.
    constexpr int32_t Ok = 0;
    constexpr int32_t NotRun = INT32_MAX;
    constexpr int32_t ErrorLibraryMissing = 1000;
    constexpr int32_t ErrorLibraryNotSigned = 1001;
    constexpr int32_t ErrorUnsupportedGraphicsApi = 1002;
    constexpr int32_t ErrorNotStarted = 1003;
    constexpr int32_t ErrorNoApplicationIdentity = 1004;

    enum class GraphicsApi : int32_t
    {
        Direct3D11 = 0,
        Direct3D12 = 1,
    };

    // The type is an sl::LogType: 0 for information, 1 for a warning, 2 for an error. May be called from any thread.
    using LogFunction = void(int32_t type, const char* message);

    // How the application identifies itself to NGX. NVIDIA assigns application IDs. An application without one passes
    // zero and a project ID, which must look like a GUID.
    struct Application
    {
        uint32_t applicationId;
        const char* projectId;
        const char* engineVersion;
    };

    // Shared with C# as DlssNative.Session, field for field.
    struct Session
    {
        int32_t initResult = NotRun;
        int32_t deviceResult = NotRun;
        int32_t supportResult = NotRun;
        int32_t functionsResult = NotRun;
        int32_t presentHookResult = NotRun;
        uint32_t streamlineVersion[3] = {};
        uint32_t ngxVersion[3] = {};

        bool Started() const
        {
            return initResult == Ok && deviceResult == Ok && supportResult == Ok && functionsResult == Ok;
        }
    };

    enum OptionFlags : uint32_t
    {
        // The color buffers hold linear values that may exceed one. Without this flag, DLSS expects values from zero
        // to one in a perceptual encoding such as sRGB.
        OptionHdr = 1 << 0,
        OptionAutoExposure = 1 << 1,
    };

    // Shared with C# as DlssNative.Options, field for field.
    struct Options
    {
        uint32_t outputWidth;
        uint32_t outputHeight;
        // An sl::DLSSMode. DLAA is 6.
        uint32_t mode;
        // An sl::DLSSPreset, used for every mode. Zero is the SDK's default.
        uint32_t preset;
        uint32_t flags;
    };

    enum FrameFlags : uint32_t
    {
        // The previous frame has no connection to this one, so DLSS drops its history.
        FrameReset = 1 << 0,
        // Nearer surfaces have larger depth values.
        FrameDepthInverted = 1 << 1,
        // The motion vectors include the projection jitter.
        FrameMotionJittered = 1 << 2,
    };

    // Shared with C# as DlssNative.Frame, field for field.
    //
    // Textures are ID3D11Resource or ID3D12Resource pointers. The states are D3D12_RESOURCE_STATES that the textures
    // are in when the frame is evaluated, and are ignored on Direct3D 11. DLSS returns the textures in the same
    // states.
    struct Frame
    {
        void* color;
        void* depth;
        void* motion;
        void* output;
        uint32_t colorState;
        uint32_t depthState;
        uint32_t motionState;
        uint32_t outputState;
        uint32_t renderWidth;
        uint32_t renderHeight;
        uint32_t outputWidth;
        uint32_t outputHeight;
        // The projection offset in render pixels, with the origin at the top left of the image.
        float jitterX;
        float jitterY;
        // Multiplying a motion vector by this scale gives the offset to the previous position as a fraction of the
        // image, with the origin at the top left.
        float motionScaleX;
        float motionScaleY;
        uint32_t flags;
    };

    // Starts Streamline on the device and loads DLSS. Later calls return the first call's result until Stop.
    const Session& Start(void* device, GraphicsApi api, const Application& application, int32_t logLevel, LogFunction* log);

    const Session& Current();

    // Shuts Streamline down. Every viewport must have been freed.
    void Stop();

    // Sets the DLSS options of a viewport and returns the render size that DLSS recommends for them.
    int32_t Configure(uint32_t viewport, const Options& options, uint32_t* renderWidth, uint32_t* renderHeight);

    // Records the DLSS passes for a viewport into the command buffer: an ID3D12GraphicsCommandList, or null on
    // Direct3D 11 to use the immediate context. On Direct3D 12 the caller restores the command list's state afterwards.
    int32_t Evaluate(uint32_t viewport, const Frame& frame, void* commandBuffer);

    // Tells Streamline that the frame is about to be presented. Call once per frame.
    int32_t EndFrame();

    // Releases the DLSS resources of a viewport.
    int32_t Free(uint32_t viewport);

    // The video memory that DLSS uses for a viewport, in bytes, or zero if it is unknown.
    uint64_t VideoMemory(uint32_t viewport);
}
