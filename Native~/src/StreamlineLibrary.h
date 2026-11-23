#pragma once

#include <windows.h>

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"

// The functions of sl.interposer.dll that the plugin calls.
//
// The interposer is loaded by path at first use, so that games which never select DLSS don't load it, and so that it is
// found next to this plugin rather than through the process's DLL search path. Nothing links against
// sl.interposer.lib, so the plugin never routes Direct3D or DXGI calls through Streamline.
struct StreamlineLibrary
{
    decltype(&slInit) Init;
    decltype(&slShutdown) Shutdown;
    decltype(&slSetD3DDevice) SetD3DDevice;
    decltype(&slIsFeatureSupported) IsFeatureSupported;
    decltype(&slGetFeatureVersion) GetFeatureVersion;
    decltype(&slGetFeatureFunction) GetFeatureFunction;
    decltype(&slGetNewFrameToken) GetNewFrameToken;
    decltype(&slSetConstants) SetConstants;
    decltype(&slEvaluateFeature) EvaluateFeature;
    decltype(&slFreeResources) FreeResources;
};

enum class StreamlineLoad
{
    Loaded,
    Missing,
    NotSigned,
};

// The folder that holds this module, with a trailing separator. Streamline's plugins are loaded from here too.
const wchar_t* ModuleDirectory();

// Loads sl.interposer.dll from the module's folder after checking NVIDIA's signature on it. Safe to call from any
// thread. The library pointer is set only when the result is Loaded.
StreamlineLoad LoadStreamlineLibrary(const StreamlineLibrary** library);
