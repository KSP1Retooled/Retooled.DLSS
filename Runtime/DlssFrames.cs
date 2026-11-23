using System;
using UnityEngine;
using UnityEngine.Rendering;

namespace Retooled.DLSS;

/// <summary>Tells Streamline where one frame ends and the next begins.</summary>
/// <remarks>
/// Streamline counts frames to decide when a released resource is safe to free, and it normally counts them by
/// watching the swap chain. Unity owns the swap chain, so the plugin reports each frame instead, just before the
/// frame's cameras render. The reports continue for a few frames after the last context is disposed, so that
/// Streamline frees that context's resources.
/// </remarks>
internal static class DlssFrames
{
    const int FramesAfterLastContext = 16;

    static CommandBuffer commands;

    static int contexts;

    static int remaining;

    static bool reporting;

    internal static void AddContext()
    {
        contexts++;
        if (reporting)
            return;

        commands ??= new CommandBuffer { name = "DLSS frame end" };
        commands.Clear();
        commands.IssuePluginEventAndData(DlssContext.EventFunction, (int)DlssNative.Event.EndFrame, IntPtr.Zero);
        Application.onBeforeRender += ReportFrame;
        reporting = true;
    }

    internal static void RemoveContext()
    {
        contexts--;
        remaining = FramesAfterLastContext;
    }

    static void ReportFrame()
    {
        Graphics.ExecuteCommandBuffer(commands);
        if (contexts > 0 || --remaining > 0)
            return;

        Application.onBeforeRender -= ReportFrame;
        reporting = false;
    }
}
