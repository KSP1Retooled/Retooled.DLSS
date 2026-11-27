using System;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

namespace Retooled.DLSS;

/// <summary>A DLSS Super Resolution context on Unity's graphics device, for one view at one output size.</summary>
/// <remarks>
/// The plugin calls DLSS through NVIDIA's Streamline SDK, which is not thread-safe and needs the graphics device, so
/// every call runs as a plugin event on the render thread. Methods queue or record those events and return at once.
/// <see cref="Status"/> and <see cref="Session"/> show what the render thread has done so far.
///
/// Call <see cref="SetApplication"/> once before creating the first context. Direct3D 11 and Direct3D 12 are
/// supported, on NVIDIA RTX GPUs. On anything else the context fails and <see cref="Session"/> says why.
/// </remarks>
public sealed class DlssContext : IDisposable
{
    static IntPtr eventFunction;

    readonly CommandBuffer commands = new() { name = "DLSS" };

    IntPtr native;

    int frames;

    /// <summary>Queues the creation of a context for the given output size, mode and options.</summary>
    /// <remarks>The first context also starts Streamline, which loads its DLLs and can take a moment.</remarks>
    public DlssContext(
        Vector2Int outputSize,
        DlssMode mode,
        DlssOptionFlags flags,
        DlssPreset preset = DlssPreset.Default
    )
    {
        var options = new DlssNative.Options
        {
            OutputWidth = (uint)outputSize.x,
            OutputHeight = (uint)outputSize.y,
            Mode = mode,
            Preset = preset,
            Flags = flags,
        };
        native = DlssNative.RetooledDLSS_NewContext(options);
        Issue(DlssNative.Event.Create);
        DlssFrames.AddContext();
    }

    /// <summary>The graphics API the plugin saw at device creation.</summary>
    public static GraphicsDeviceType Renderer => (GraphicsDeviceType)DlssNative.RetooledDLSS_GetRenderer();

    /// <summary>
    /// Whether Unity's Vulkan device was created with the extensions and features DLSS needs. The plugin adds them on
    /// NVIDIA GPUs when it is loaded before the device: by the editor at startup, or in a player by a Vulkan layer
    /// that calls the plugin's <c>RetooledVulkanIntercept</c> export. Without them a session on Vulkan fails with
    /// <see cref="DlssResult.ErrorVulkanDeviceNotReady"/>.
    /// </summary>
    public static bool VulkanDeviceReady => DlssNative.RetooledDLSS_IsVulkanDeviceReady() != 0;

    /// <summary>The folder that holds NVIDIA's DLSS runtime, <c>nvngx_dlss.dll</c>. Applies when Streamline starts.</summary>
    /// <remarks>
    /// The package doesn't include the runtime, because NVIDIA's license covers distributing it as part of an
    /// application. In a player, Unity puts every native plugin in one folder, so leave this unset and ship the runtime
    /// as a plugin of the application. In the editor, set it to the folder where the application keeps the runtime.
    /// </remarks>
    public static string RuntimeDirectory
    {
        set => DlssNative.RetooledDLSS_SetRuntimeDirectory(Encoding.UTF8.GetBytes(value + "\0"));
    }

    /// <summary>Which Streamline messages go to the Unity log. Applies when Streamline starts.</summary>
    public static DlssLogLevel LogLevel
    {
        set => DlssNative.RetooledDLSS_SetLogLevel(value);
    }

    /// <summary>The state of Streamline, which all contexts share.</summary>
    public static DlssSession Session
    {
        get
        {
            DlssNative.RetooledDLSS_GetSession(out DlssNative.Session session);
            return new DlssSession(session);
        }
    }

    internal static IntPtr EventFunction
    {
        get
        {
            if (eventFunction == IntPtr.Zero)
                eventFunction = DlssNative.RetooledDLSS_GetEventFunction();
            return eventFunction;
        }
    }

    public DlssStatus Status
    {
        get
        {
            ThrowIfDisposed();
            DlssNative.RetooledDLSS_GetStatus(native, out DlssNative.Status status);
            return new DlssStatus(status);
        }
    }

    /// <summary>Sets how the application identifies itself to NVIDIA's runtime. Applies when Streamline starts.</summary>
    /// <param name="applicationId">An ID that NVIDIA assigned to the application, or zero.</param>
    /// <param name="projectId">A GUID that identifies the application. Required if there is no application ID.</param>
    /// <param name="engineVersion">The version of the application's renderer, for NVIDIA's records.</param>
    public static void SetApplication(uint applicationId, string projectId, string engineVersion) =>
        DlssNative.RetooledDLSS_SetApplication(
            applicationId,
            Encoding.UTF8.GetBytes(projectId + "\0"),
            Encoding.UTF8.GetBytes(engineVersion + "\0")
        );

    /// <summary>Records the evaluation of one frame into a command buffer.</summary>
    /// <remarks>
    /// The recorded commands hold this frame's inputs, so record them again every frame and execute them once. DLSS
    /// leaves the Direct3D 11 state as it found it. On Direct3D 12, Unity restores its own state afterwards.
    /// </remarks>
    public void Evaluate(CommandBuffer target, in DlssFrame frame)
    {
        ThrowIfDisposed();
        var nativeFrame = new DlssNative.Frame
        {
            Color = frame.Color,
            Depth = frame.Depth,
            Motion = frame.Motion,
            Output = frame.Output,
            RenderWidth = (uint)frame.RenderSize.x,
            RenderHeight = (uint)frame.RenderSize.y,
            OutputWidth = (uint)frame.OutputSize.x,
            OutputHeight = (uint)frame.OutputSize.y,
            JitterX = frame.Jitter.x,
            JitterY = frame.Jitter.y,
            MotionScaleX = frame.MotionScale.x,
            MotionScaleY = frame.MotionScale.y,
            Flags = frame.Flags,
        };

        // The render thread may still be working on earlier frames, so each frame gets a slot of its own.
        IntPtr slot = DlssNative.RetooledDLSS_SetFrame(native, frames++ % DlssNative.FrameSlots, nativeFrame);
        target.IssuePluginEventAndData(EventFunction, (int)DlssNative.Event.Evaluate, slot);
    }

    /// <summary>Queues the destruction of the context. The render thread frees it.</summary>
    /// <remarks>Command buffers recorded by <see cref="Evaluate"/> must not run after this.</remarks>
    public void Dispose()
    {
        if (native == IntPtr.Zero)
            return;

        Issue(DlssNative.Event.Destroy);
        native = IntPtr.Zero;
        commands.Release();
        DlssFrames.RemoveContext();
    }

    void Issue(DlssNative.Event pluginEvent)
    {
        commands.Clear();
        commands.IssuePluginEventAndData(EventFunction, (int)pluginEvent, native);
        Graphics.ExecuteCommandBuffer(commands);
    }

    void ThrowIfDisposed()
    {
        if (native == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(DlssContext));
    }
}
