using System;
using UnityEngine;

namespace Retooled.DLSS;

/// <summary>Streamline's <c>sl::Result</c>, plus results for steps that never reached Streamline.</summary>
public enum DlssResult
{
    Ok = 0,
    ErrorIO = 1,
    ErrorDriverOutOfDate = 2,
    ErrorOSOutOfDate = 3,
    ErrorOSDisabledHWS = 4,
    ErrorDeviceNotCreated = 5,
    ErrorNoSupportedAdapterFound = 6,
    ErrorAdapterNotSupported = 7,
    ErrorNoPlugins = 8,
    ErrorVulkanAPI = 9,
    ErrorDXGIAPI = 10,
    ErrorD3DAPI = 11,
    ErrorNRDAPI = 12,
    ErrorNVAPI = 13,
    ErrorReflexAPI = 14,
    ErrorNGXFailed = 15,
    ErrorJSONParsing = 16,
    ErrorMissingProxy = 17,
    ErrorMissingResourceState = 18,
    ErrorInvalidIntegration = 19,
    ErrorMissingInputParameter = 20,
    ErrorNotInitialized = 21,
    ErrorComputeFailed = 22,
    ErrorInitNotCalled = 23,
    ErrorExceptionHandler = 24,
    ErrorInvalidParameter = 25,
    ErrorMissingConstants = 26,
    ErrorDuplicatedConstants = 27,
    ErrorMissingOrInvalidAPI = 28,
    ErrorCommonConstantsMissing = 29,
    ErrorUnsupportedInterface = 30,
    ErrorFeatureMissing = 31,
    ErrorFeatureNotSupported = 32,
    ErrorFeatureMissingHooks = 33,
    ErrorFeatureFailedToLoad = 34,
    ErrorFeatureWrongPriority = 35,
    ErrorFeatureMissingDependency = 36,
    ErrorFeatureManagerInvalidState = 37,
    ErrorInvalidState = 38,
    WarnOutOfVRAM = 39,

    /// <summary>sl.interposer.dll is not in the plugin's folder, or cannot be loaded.</summary>
    ErrorLibraryMissing = 1000,

    /// <summary>sl.interposer.dll does not carry NVIDIA's signature.</summary>
    ErrorLibraryNotSigned = 1001,

    /// <summary>The graphics API is not Direct3D 11, Direct3D 12 or Vulkan, or Unity lacks the plugin interface for it.</summary>
    ErrorUnsupportedGraphicsApi = 1002,

    ErrorNotStarted = 1003,

    /// <summary><see cref="DlssContext.SetApplication"/> was not called before the first context.</summary>
    ErrorNoApplicationIdentity = 1004,

    /// <summary>The Vulkan device was created without what DLSS needs. See <see cref="DlssContext.VulkanDeviceReady"/>.</summary>
    ErrorVulkanDeviceNotReady = 1005,

    /// <summary>On Vulkan, Unity could not hand over one of a frame's textures.</summary>
    ErrorTextureAccess = 1006,

    /// <summary>The step has not happened yet.</summary>
    NotRun = int.MaxValue,
}

/// <summary>Streamline's <c>sl::DLSSMode</c>.</summary>
/// <remarks>DLAA renders at the output resolution. The other modes render below it and upscale.</remarks>
public enum DlssMode
{
    Off = 0,
    MaxPerformance = 1,
    Balanced = 2,
    MaxQuality = 3,
    UltraPerformance = 4,
    UltraQuality = 5,
    Dlaa = 6,
}

/// <summary>Streamline's <c>sl::DLSSPreset</c>, which selects the model.</summary>
public enum DlssPreset
{
    /// <summary>The SDK's choice for the mode.</summary>
    Default = 0,
    J = 10,
    K = 11,
    L = 12,
    M = 13,
}

[Flags]
public enum DlssOptionFlags : uint
{
    None = 0,

    /// <summary>The color textures hold linear values that may exceed one.</summary>
    /// <remarks>Without this flag, DLSS expects values from zero to one in a perceptual encoding such as sRGB.</remarks>
    Hdr = 1 << 0,

    AutoExposure = 1 << 1,
}

[Flags]
public enum DlssFrameFlags : uint
{
    None = 0,

    /// <summary>The previous frame has no connection to this one, so DLSS drops its history.</summary>
    Reset = 1 << 0,

    /// <summary>Nearer surfaces have larger depth values.</summary>
    DepthInverted = 1 << 1,

    /// <summary>The motion vectors include the projection jitter.</summary>
    MotionJittered = 1 << 2,
}

/// <summary>Which of Streamline's messages go to the Unity log.</summary>
public enum DlssLogLevel
{
    Off = 0,
    Warnings = 1,

    /// <summary>Also every plugin, hook and model that Streamline loads.</summary>
    Information = 2,
    Verbose = 3,
}

/// <summary>How far a context has come on the render thread.</summary>
public enum DlssStage
{
    Pending = 0,
    Ready = 1,
    Failed = -1,
}

/// <summary>The inputs of one evaluated frame.</summary>
/// <remarks>
/// Textures are native pointers, from <see cref="Texture.GetNativeTexturePtr"/>. Depth values run from zero to one.
/// The output texture needs <see cref="RenderTexture.enableRandomWrite"/>.
/// </remarks>
public struct DlssFrame
{
    /// <summary>The jittered image, at the render size.</summary>
    public IntPtr Color;

    public IntPtr Depth;

    /// <summary>Two-channel float motion vectors, at the render size.</summary>
    public IntPtr Motion;

    public IntPtr Output;

    public Vector2Int RenderSize;

    public Vector2Int OutputSize;

    /// <summary>The projection offset in render pixels, with the origin at the top left of the image.</summary>
    /// <remarks>Each component is between -0.5 and 0.5.</remarks>
    public Vector2 Jitter;

    /// <summary>
    /// Multiplying a motion vector by this scale gives the offset from a pixel to its previous position, as a fraction
    /// of the image, with the origin at the top left.
    /// </summary>
    public Vector2 MotionScale;

    public DlssFrameFlags Flags;
}

/// <summary>The state of Streamline in this process, as last written by the render thread.</summary>
/// <remarks>Streamline starts when the first context is created and stops with the graphics device.</remarks>
public readonly struct DlssSession
{
    public readonly DlssResult InitResult;

    public readonly DlssResult DeviceResult;

    /// <summary>Whether DLSS can run on the GPU and driver of Unity's device.</summary>
    public readonly DlssResult SupportResult;

    public readonly DlssResult FunctionsResult;

    /// <summary>
    /// Whether the plugin can tell Streamline about presented frames. Without it, Streamline keeps released resources.
    /// </summary>
    public readonly DlssResult PresentHookResult;

    public readonly Version StreamlineVersion;

    /// <summary>The version of the DLSS runtime, nvngx_dlss.dll.</summary>
    public readonly Version NgxVersion;

    internal DlssSession(in DlssNative.Session session)
    {
        InitResult = session.InitResult;
        DeviceResult = session.DeviceResult;
        SupportResult = session.SupportResult;
        FunctionsResult = session.FunctionsResult;
        PresentHookResult = session.PresentHookResult;
        StreamlineVersion = new Version((int)session.StreamlineMajor, (int)session.StreamlineMinor, (int)session.StreamlineBuild);
        NgxVersion = new Version((int)session.NgxMajor, (int)session.NgxMinor, (int)session.NgxBuild);
    }

    /// <summary>Whether Streamline started and DLSS is supported.</summary>
    public bool Started =>
        InitResult == DlssResult.Ok
        && DeviceResult == DlssResult.Ok
        && SupportResult == DlssResult.Ok
        && FunctionsResult == DlssResult.Ok;

    public override string ToString() =>
        $"init {InitResult}, device {DeviceResult}, support {SupportResult}, functions {FunctionsResult}, "
        + $"present hook {PresentHookResult}, Streamline {StreamlineVersion}, NGX {NgxVersion}";
}

/// <summary>A snapshot of a context's state, as last written by the render thread.</summary>
public readonly struct DlssStatus
{
    public readonly DlssStage Stage;

    /// <summary>The result of setting the context's options.</summary>
    public readonly DlssResult ConfigureResult;

    /// <summary>The result of the last evaluated frame.</summary>
    public readonly DlssResult EvaluateResult;

    /// <summary>The render size that DLSS recommends for the context's mode and output size.</summary>
    public readonly Vector2Int RenderSize;

    /// <summary>The number of frames evaluated so far.</summary>
    public readonly int Evaluations;

    /// <summary>The video memory that DLSS uses for this context, in bytes.</summary>
    public readonly long VideoMemory;

    internal DlssStatus(in DlssNative.Status status)
    {
        Stage = status.Stage;
        ConfigureResult = status.ConfigureResult;
        EvaluateResult = status.EvaluateResult;
        RenderSize = new Vector2Int((int)status.RenderWidth, (int)status.RenderHeight);
        Evaluations = (int)status.Evaluations;
        VideoMemory = (long)status.VideoMemory;
    }

    public override string ToString() =>
        $"stage {Stage}, configure {ConfigureResult}, evaluate {EvaluateResult}, render size {RenderSize.x}x{RenderSize.y}, "
        + $"{Evaluations} evaluations, {VideoMemory / (1024.0 * 1024.0):F1} MB";
}
