# DLSS

NVIDIA DLSS Super Resolution and DLAA for Unity's built-in render pipeline on Windows x64.

The package calls DLSS through NVIDIA's Streamline SDK. Streamline records its work with the graphics API's own
objects, which C# can't reach, so the package includes a small native plugin, `Retooled.DLSS.Native.dll`, that Unity calls on
the render thread. The plugin loads Streamline's `sl.interposer.dll` from its own folder and makes every SDK call
there. `DlssContext` is the C# side.

## Requirements

- An NVIDIA RTX GPU.
- Direct3D 11 or Direct3D 12. Vulkan is not supported yet. DLSS doesn't run on OpenGL, Linux or macOS.
- On Direct3D 12, a Unity version that has the `IUnityGraphicsD3D12v8` plugin interface.

On anything else a context fails, and `DlssContext.Session` and `context.Status` say why.

## The DLSS runtime

The package doesn't include NVIDIA's DLSS runtime, `nvngx_dlss.dll`. NVIDIA's RTX SDKs license covers distributing it
as part of an application, so the application that uses this package provides it, from the Streamline SDK release
under `bin/x64`.

- In a player, Unity puts every native plugin in one folder, so a runtime that the application imports as a Windows
  x64 plugin ends up next to this package's plugin, where Streamline looks for it.
- In the editor, the runtime is somewhere else in the project. Set `DlssContext.RuntimeDirectory` to its folder before
  creating the first context.

## Use

```csharp
// Once, before the first context. NVIDIA assigns application IDs. Without one, pass zero and a GUID.
DlssContext.SetApplication(0, "834d43ef-b1af-49cd-8d72-724ba9bd2d68", Application.unityVersion);

var context = new DlssContext(outputSize, DlssMode.Dlaa, DlssOptionFlags.None);

// Each frame, once context.Status.Stage is Ready:
commands.Clear();
context.Evaluate(commands, frame);

// When done:
context.Dispose();
```

Calls are queued to the render thread and return at once. `context.Status` shows how far the render thread has got.
`Evaluate` records one frame into a command buffer. The recorded commands hold that frame's inputs, so record them
again every frame.

### Inputs

| Input | What DLSS expects |
| --- | --- |
| Color | The jittered image. Without `DlssOptionFlags.Hdr`, values from zero to one in a perceptual encoding such as sRGB. |
| Depth | Values from zero to one. Set `DlssFrameFlags.DepthInverted` if nearer surfaces have larger values. |
| Motion | Two-channel float vectors. `DlssFrame.MotionScale` turns them into the offset from a pixel to its previous position, as a fraction of the image. Unity's motion vectors need a scale of (-1, -1). |
| Jitter | The projection offset in render pixels, between -0.5 and 0.5, in a Halton sequence. NVIDIA asks for 8 samples at native resolution and more when upscaling. |
| Output | A texture with `enableRandomWrite`. |

Pixel coordinates have their origin at the top left of the texture. A Unity render texture's rows run the same way as
its UV coordinates, so Unity's jitter and motion vectors need no vertical flip.

## How it works with Unity

Streamline is designed to wrap the application's device and swap chain. A plugin can't give Unity wrapped objects,
so the plugin uses Streamline's manual hooking mode on the device that Unity created. Two things follow from that:

- Streamline counts presented frames to decide when a released resource is safe to free. Unity owns the swap chain,
  so the package reports each frame instead, by calling the present hooks of Streamline's common plugin directly. The
  hook names are part of the interface between `sl.interposer.dll` and `sl.common.dll`, so they are tied to the
  Streamline version in this package. `DlssSession.PresentHookResult` reports whether they were found.
- Streamline starts when the first context is created and stops with the graphics device. A game that never creates
  a context never loads it.

The plugin doesn't let Streamline download updated plugins or models.

## Debugging

- Streamline's warnings and errors go to the Unity log with a `[DLSS]` prefix. Set `DlssContext.LogLevel` before
  creating the first context to see more.
- NVIDIA's development build of `nvngx_dlss.dll` draws a debug overlay of the inputs when you press Ctrl+Alt+F12. It
  is in the Streamline SDK under `bin/x64/development`. Never ship it.

## Building the plugin

Needs CMake, Ninja, LLVM's `clang-cl` and the Windows SDK.

```powershell
Native~/build.ps1 -Test
```

NVIDIA's files are not in this repository. `build.ps1` first runs `Native~/fetch-sdk.ps1`, which downloads the
Streamline SDK release from GitHub, checks its SHA-256, and copies the headers into `Native~/streamline` and the
Streamline and DLSS DLLs into `Plugins/x86_64`. Run `fetch-sdk.ps1` on its own to get the DLLs without building, for
example in CI. `-Zip <path>` makes it use a release zip that was downloaded before. The DLLs' `.meta` files are kept
in `Native~/metas`, and the script copies them next to the DLLs. It also copies `nvngx_dlss.dll` into `Plugins/x86_64`
so that the harness and the package's tests can run in this repository, but releases leave it out.

When the plugin's source changes on any branch, the Native plug-in workflow builds `Retooled.DLSS.Native.dll` and commits it to
that branch. Publishing a GitHub release tagged `v<version>` packs the package with the Streamline DLLs and adds it to
the KSP1Retooled UPM registry.

The build script copies `Retooled.DLSS.Native.dll` into `Plugins/x86_64`. Close the Unity editor first if it has loaded the plugin,
because it keeps the DLL open.

`-Test` also runs `DlssHarness.exe`, which evaluates DLAA on Direct3D 11 and Direct3D 12 devices of its own, without
Unity, once with `nvngx_dlss.dll` next to Streamline and once with it in a folder of its own. It needs an NVIDIA RTX
GPU.

## Licenses

- The Streamline DLLs in `Plugins/x86_64` and the headers in `Native~/streamline` are NVIDIA's, under the MIT license
  in `Native~/streamline/license.txt`.
- `nvngx_dlss.dll` is NVIDIA's DLSS runtime, under the NVIDIA RTX SDKs license in `bin/x64/nvngx_dlss.license.txt`
  of the Streamline SDK. That license has conditions on distribution and on crediting NVIDIA in the application. Read
  it before shipping.
- The headers in `Native~/unity` are Unity's, under the Unity Companion License in `Native~/unity/LICENSE.md`.
