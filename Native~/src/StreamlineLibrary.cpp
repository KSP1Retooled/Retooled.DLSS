#include "StreamlineLibrary.h"

#include <mutex>
#include <string>

#include "sl_security.h"

namespace
{
    std::wstring FindModuleDirectory()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&FindModuleDirectory),
            &self);

        std::wstring path(MAX_PATH, L'\0');
        while (true)
        {
            DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
            if (length < path.size())
            {
                path.resize(length);
                break;
            }
            path.resize(path.size() * 2);
        }
        return path.substr(0, path.find_last_of(L"\\/") + 1);
    }

    template <typename T>
    bool Resolve(HMODULE module, const char* name, T& function)
    {
        function = reinterpret_cast<T>(GetProcAddress(module, name));
        return function != nullptr;
    }

    std::wstring directory;
    StreamlineLibrary library;
    StreamlineLoad result = StreamlineLoad::Missing;
    std::once_flag directoryOnce;
    std::once_flag loadOnce;

    void Load()
    {
        std::wstring path = std::wstring(ModuleDirectory()) + L"sl.interposer.dll";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return;

        // The interposer checks the signatures of the plugins that it loads, so this check covers the whole chain.
        if (!sl::security::verifyEmbeddedSignature(path.c_str()))
        {
            result = StreamlineLoad::NotSigned;
            return;
        }

        // The altered search path lets the interposer find the DLLs it depends on in its own folder.
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr)
            return;

        bool resolved = Resolve(module, "slInit", library.Init)
            && Resolve(module, "slShutdown", library.Shutdown)
            && Resolve(module, "slSetD3DDevice", library.SetD3DDevice)
            && Resolve(module, "slSetVulkanInfo", library.SetVulkanInfo)
            && Resolve(module, "slIsFeatureSupported", library.IsFeatureSupported)
            && Resolve(module, "slGetFeatureVersion", library.GetFeatureVersion)
            && Resolve(module, "slGetFeatureFunction", library.GetFeatureFunction)
            && Resolve(module, "slGetNewFrameToken", library.GetNewFrameToken)
            && Resolve(module, "slSetConstants", library.SetConstants)
            && Resolve(module, "slEvaluateFeature", library.EvaluateFeature)
            && Resolve(module, "slFreeResources", library.FreeResources);
        if (resolved)
            result = StreamlineLoad::Loaded;
    }
}

const wchar_t* ModuleDirectory()
{
    std::call_once(directoryOnce, [] { directory = FindModuleDirectory(); });
    return directory.c_str();
}

StreamlineLoad LoadStreamlineLibrary(const StreamlineLibrary** loaded)
{
    std::call_once(loadOnce, Load);
    if (result == StreamlineLoad::Loaded)
        *loaded = &library;
    return result;
}
