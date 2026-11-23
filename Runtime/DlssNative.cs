using System;
using System.Runtime.InteropServices;

namespace Retooled.DLSS;

/// <summary>Declarations for RetooledDLSS.dll, the native plugin in <c>Native~</c>.</summary>
internal static class DlssNative
{
    const string Plugin = "RetooledDLSS";

    /// <summary>The number of frame slots in a native context, <c>FrameSlots</c> in Plugin.cpp.</summary>
    internal const int FrameSlots = 8;

    /// <summary>Render-thread events, handled by <c>OnRenderEvent</c> in Plugin.cpp.</summary>
    internal enum Event
    {
        Create = 0,
        Evaluate = 1,
        EndFrame = 2,
        Destroy = 3,
    }

    /// <summary>Dlss.h's <c>Session</c>, field for field.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Session
    {
        public DlssResult InitResult;
        public DlssResult DeviceResult;
        public DlssResult SupportResult;
        public DlssResult FunctionsResult;
        public DlssResult PresentHookResult;
        public uint StreamlineMajor;
        public uint StreamlineMinor;
        public uint StreamlineBuild;
        public uint NgxMajor;
        public uint NgxMinor;
        public uint NgxBuild;
    }

    /// <summary>Dlss.h's <c>Options</c>, field for field.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Options
    {
        public uint OutputWidth;
        public uint OutputHeight;
        public DlssMode Mode;
        public DlssPreset Preset;
        public DlssOptionFlags Flags;
    }

    /// <summary>Dlss.h's <c>Frame</c>, field for field. The plugin fills in the resource states.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Frame
    {
        public IntPtr Color;
        public IntPtr Depth;
        public IntPtr Motion;
        public IntPtr Output;
        public uint ColorState;
        public uint DepthState;
        public uint MotionState;
        public uint OutputState;
        public uint RenderWidth;
        public uint RenderHeight;
        public uint OutputWidth;
        public uint OutputHeight;
        public float JitterX;
        public float JitterY;
        public float MotionScaleX;
        public float MotionScaleY;
        public DlssFrameFlags Flags;
    }

    /// <summary>Plugin.cpp's <c>Status</c>, field for field.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Status
    {
        public DlssStage Stage;
        public DlssResult ConfigureResult;
        public DlssResult EvaluateResult;
        public uint RenderWidth;
        public uint RenderHeight;
        public uint Evaluations;
        public ulong VideoMemory;
    }

    [DllImport(Plugin)]
    internal static extern int RetooledDLSS_GetRenderer();

    [DllImport(Plugin)]
    internal static extern void RetooledDLSS_SetApplication(
        uint applicationId,
        byte[] nullTerminatedProjectId,
        byte[] nullTerminatedEngineVersion
    );

    [DllImport(Plugin)]
    internal static extern void RetooledDLSS_SetLogLevel(DlssLogLevel level);

    [DllImport(Plugin)]
    internal static extern void RetooledDLSS_GetSession(out Session session);

    [DllImport(Plugin)]
    internal static extern IntPtr RetooledDLSS_GetEventFunction();

    [DllImport(Plugin)]
    internal static extern IntPtr RetooledDLSS_NewContext(in Options options);

    [DllImport(Plugin)]
    internal static extern void RetooledDLSS_GetStatus(IntPtr context, out Status status);

    /// <summary>Returns the slot that holds the frame, which is the data of that frame's evaluate event.</summary>
    [DllImport(Plugin)]
    internal static extern IntPtr RetooledDLSS_SetFrame(IntPtr context, int index, in Frame frame);
}
