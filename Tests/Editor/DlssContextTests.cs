using System.Collections;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.TestTools;

namespace Retooled.DLSS.Tests;

public class DlssContextTests
{
    const float TimeoutSeconds = 60f;

    const int NvidiaVendorId = 0x10DE;

    static readonly Vector2Int Size = new(640, 360);

    static bool ExpectDlss =>
        SystemInfo.graphicsDeviceVendorID == NvidiaVendorId
        && SystemInfo.graphicsDeviceType is GraphicsDeviceType.Direct3D11 or GraphicsDeviceType.Direct3D12;

    [Test]
    public void StructsMatchTheNativeLayout()
    {
        Assert.That(Marshal.SizeOf<DlssNative.Session>(), Is.EqualTo(11 * sizeof(int)));
        Assert.That(Marshal.SizeOf<DlssNative.Options>(), Is.EqualTo(5 * sizeof(int)));
        Assert.That(Marshal.SizeOf<DlssNative.Frame>(), Is.EqualTo(4 * 8 + 13 * sizeof(int) + 4));
        Assert.That(Marshal.SizeOf<DlssNative.Status>(), Is.EqualTo(6 * sizeof(int) + sizeof(ulong)));
    }

    [Test]
    public void ThePluginSeesUnitysGraphicsApi()
    {
        RequirePlugin();
        Assert.That(DlssContext.Renderer, Is.EqualTo(SystemInfo.graphicsDeviceType));
    }

    // DLSS runs on NVIDIA RTX GPUs under Direct3D 11 and 12. Anywhere else the context has to fail with a reason.
    [UnityTest]
    public IEnumerator CreatesAContextOrReportsWhyNot()
    {
        RequirePlugin();
        using DlssContext context = NewContext();
        yield return WaitWhile(() => context.Status.Stage == DlssStage.Pending);
        RequireStreamline();

        DlssStatus status = context.Status;
        DlssSession session = DlssContext.Session;
        if (ExpectDlss)
        {
            Assert.That(session.Started, Is.True, session.ToString());
            Assert.That(session.PresentHookResult, Is.EqualTo(DlssResult.Ok));
            Assert.That(status.Stage, Is.EqualTo(DlssStage.Ready), status.ToString());
            Assert.That(status.RenderSize, Is.EqualTo(Size), "DLAA renders at the output size");
        }
        else
        {
            Assert.That(session.Started, Is.False, session.ToString());
            Assert.That(status.Stage, Is.EqualTo(DlssStage.Failed), status.ToString());
        }
    }

    // A flat image with no motion has one right answer, so the output has to match the input.
    [UnityTest]
    public IEnumerator DlaaReproducesAFlatImage()
    {
        RequirePlugin();
        if (!ExpectDlss)
            Assert.Ignore("DLSS needs an NVIDIA GPU on Direct3D 11 or 12");

        const int frames = 8;
        var input = new Color(0.25f, 0.5f, 0.75f, 1f);
        RenderTexture color = NewTexture(RenderTextureFormat.ARGB32, input, randomWrite: false);
        RenderTexture depth = NewTexture(RenderTextureFormat.RFloat, new Color(0.5f, 0f, 0f, 0f), randomWrite: false);
        RenderTexture motion = NewTexture(RenderTextureFormat.RGHalf, Color.clear, randomWrite: false);
        RenderTexture output = NewTexture(RenderTextureFormat.ARGB32, Color.black, randomWrite: true);
        var commands = new CommandBuffer { name = "DLSS test" };
        try
        {
            using DlssContext context = NewContext();
            yield return WaitWhile(() => context.Status.Stage == DlssStage.Pending);
            RequireStreamline();
            Assert.That(context.Status.Stage, Is.EqualTo(DlssStage.Ready), DlssContext.Session.ToString());

            var frame = new DlssFrame
            {
                Color = color.GetNativeTexturePtr(),
                Depth = depth.GetNativeTexturePtr(),
                Motion = motion.GetNativeTexturePtr(),
                Output = output.GetNativeTexturePtr(),
                RenderSize = Size,
                OutputSize = Size,
                MotionScale = Vector2.one,
            };
            for (int i = 0; i < frames; i++)
            {
                frame.Flags = DlssFrameFlags.DepthInverted | (i == 0 ? DlssFrameFlags.Reset : DlssFrameFlags.None);
                commands.Clear();
                context.Evaluate(commands, frame);
                Graphics.ExecuteCommandBuffer(commands);
                yield return null;
            }
            yield return WaitWhile(() => context.Status.Evaluations < frames);

            DlssStatus status = context.Status;
            Assert.That(status.EvaluateResult, Is.EqualTo(DlssResult.Ok), status.ToString());
            Assert.That(status.VideoMemory, Is.GreaterThan(0));

            Color result = ReadCenter(output);
            Assert.That(result.r, Is.EqualTo(input.r).Within(0.02f), result.ToString());
            Assert.That(result.g, Is.EqualTo(input.g).Within(0.02f), result.ToString());
            Assert.That(result.b, Is.EqualTo(input.b).Within(0.02f), result.ToString());
        }
        finally
        {
            commands.Release();
            foreach (RenderTexture texture in new[] { color, depth, motion, output })
            {
                texture.Release();
                Object.DestroyImmediate(texture);
            }
        }
    }

    // The plugin is only built for Windows.
    static void RequirePlugin()
    {
        if (Application.platform != RuntimePlatform.WindowsEditor)
            Assert.Ignore("DLSS only runs on Windows");
    }

    // A checkout of this package may not include NVIDIA's DLLs. The plugin looks for them when the first context is
    // created, so call this after a context has left the pending stage.
    static void RequireStreamline()
    {
        if (DlssContext.Session.InitResult == DlssResult.ErrorLibraryMissing)
            Assert.Ignore("sl.interposer.dll isn't in Plugins/x86_64");
    }

    // The package leaves out NVIDIA's runtime, so the project provides it: in this repo fetch-sdk.ps1 puts it in
    // Plugins/x86_64, and an application keeps it with its own plugins.
    static string RuntimeFolder()
    {
        string path = AssetDatabase.FindAssets("nvngx_dlss")
            .Select(AssetDatabase.GUIDToAssetPath)
            .FirstOrDefault(asset => Path.GetFileName(asset) == "nvngx_dlss.dll");
        return path == null ? null : Path.GetFullPath(Path.GetDirectoryName(path));
    }

    static DlssContext NewContext()
    {
        string runtime = RuntimeFolder();
        if (ExpectDlss && runtime == null)
            Assert.Ignore("nvngx_dlss.dll isn't in the project");
        // Any GUID identifies a development build to NVIDIA's runtime.
        DlssContext.SetApplication(0, "5a0ad5c3-9d0b-4d6f-9f3b-3e7f0d1c8a11", Application.unityVersion);
        DlssContext.RuntimeDirectory = runtime;
        return new DlssContext(Size, DlssMode.Dlaa, DlssOptionFlags.None);
    }

    static IEnumerator WaitWhile(System.Func<bool> waiting)
    {
        float deadline = Time.realtimeSinceStartup + TimeoutSeconds;
        while (waiting() && Time.realtimeSinceStartup < deadline)
            yield return null;
    }

    static RenderTexture NewTexture(RenderTextureFormat format, Color fill, bool randomWrite)
    {
        var texture = new RenderTexture(Size.x, Size.y, 0, format, RenderTextureReadWrite.Linear)
        {
            enableRandomWrite = randomWrite,
            hideFlags = HideFlags.HideAndDontSave,
        };
        texture.Create();
        RenderTexture previous = RenderTexture.active;
        RenderTexture.active = texture;
        GL.Clear(clearDepth: false, clearColor: true, fill);
        RenderTexture.active = previous;
        return texture;
    }

    static Color ReadCenter(RenderTexture texture)
    {
        var pixel = new Texture2D(1, 1, TextureFormat.RGBA32, mipChain: false, linear: true);
        RenderTexture previous = RenderTexture.active;
        RenderTexture.active = texture;
        pixel.ReadPixels(new Rect(Size.x / 2, Size.y / 2, 1, 1), 0, 0);
        RenderTexture.active = previous;
        Color result = pixel.GetPixel(0, 0);
        Object.DestroyImmediate(pixel);
        return result;
    }
}
