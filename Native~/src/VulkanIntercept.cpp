#include "VulkanIntercept.h"

#include <cstring>
#include <mutex>
#include <vector>

#include <windows.h>

#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"
#include "IUnityInterface.h"

namespace
{
    constexpr uint32_t NvidiaVendorId = 0x10DE;

    // What Streamline's DLSS plugin asks of a Vulkan instance and device. Streamline can only say so after it has
    // started, and it starts with the first context, long after the device exists. The lists are therefore tied to
    // the Streamline version that ships with this plugin.
    const char* const InstanceExtensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    };
    const char* const DeviceExtensions[] = {
        VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
        VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
        VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    };

    // Streamline also asks for the Vulkan 1.2 features timelineSemaphore, descriptorIndexing and bufferDeviceAddress.
    // A device that doesn't enable them through VkPhysicalDeviceVulkan12Features gets them through the extensions
    // they came from, which is valid whatever API version the instance was created for.
    const char* const FeatureExtensions[] = {
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    };

    PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr;
    PFN_vkCreateInstance nextCreateInstance;
    PFN_vkCreateDevice nextCreateDevice;
    VkInstance createdInstance;
    bool deviceReady;
    // Who installed the hooks: Unity, when the plugin loaded before Unity set up Vulkan, or else the game's Vulkan
    // layer. Each takes the hooks only if the other hasn't, because they hold one chain of functions to call on to.
    bool hookedByUnity;
    bool hookedByLayer;

    std::mutex noteMutex;
    std::string note;

    void SetNote(const char* text)
    {
        std::lock_guard lock(noteMutex);
        note = text;
    }

    bool Contains(const std::vector<VkExtensionProperties>& available, const char* name)
    {
        for (const VkExtensionProperties& extension : available)
        {
            if (std::strcmp(extension.extensionName, name) == 0)
                return true;
        }

        return false;
    }

    void AddMissing(std::vector<const char*>& names, const char* added)
    {
        for (const char* name : names)
        {
            if (std::strcmp(name, added) == 0)
                return;
        }

        names.push_back(added);
    }

    std::vector<VkExtensionProperties> InstanceExtensionsAvailable()
    {
        std::vector<VkExtensionProperties> available;
        // Unity's hook gets the loader's vkGetInstanceProcAddr, which knows the functions that need no instance.
        // Inside a layer chain the next one may know only the functions of an instance, and a layer need not accept
        // a call without one. The loader, which is what called this module, exports the function itself.
        PFN_vkEnumerateInstanceExtensionProperties enumerate = nullptr;
        HMODULE loader = hookedByLayer ? GetModuleHandleW(L"vulkan-1.dll") : nullptr;
        if (loader != nullptr)
        {
            enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                GetProcAddress(loader, "vkEnumerateInstanceExtensionProperties"));
        }
        else if (!hookedByLayer)
        {
            enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
        }

        uint32_t count = 0;
        if (enumerate == nullptr || enumerate(nullptr, &count, nullptr) != VK_SUCCESS)
            return available;

        available.resize(count);
        if (enumerate(nullptr, &count, available.data()) < VK_SUCCESS)
            count = 0;
        available.resize(count);
        return available;
    }

    std::vector<VkExtensionProperties> DeviceExtensionsAvailable(VkPhysicalDevice physicalDevice)
    {
        std::vector<VkExtensionProperties> available;
        auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            nextGetInstanceProcAddr(createdInstance, "vkEnumerateDeviceExtensionProperties"));
        uint32_t count = 0;
        if (enumerate == nullptr || enumerate(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS)
            return available;

        available.resize(count);
        if (enumerate(physicalDevice, nullptr, &count, available.data()) < VK_SUCCESS)
            count = 0;
        available.resize(count);
        return available;
    }

    bool IsNvidia(VkPhysicalDevice physicalDevice)
    {
        auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            nextGetInstanceProcAddr(createdInstance, "vkGetPhysicalDeviceProperties"));
        if (getProperties == nullptr)
            return false;

        VkPhysicalDeviceProperties properties{};
        getProperties(physicalDevice, &properties);
        return properties.vendorID == NvidiaVendorId;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
        const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance)
    {
        // The GPU isn't known yet. The three extensions are part of Vulkan 1.1 and cost nothing on other GPUs.
        std::vector<const char*> extensions(info->ppEnabledExtensionNames,
            info->ppEnabledExtensionNames + info->enabledExtensionCount);
        std::vector<VkExtensionProperties> available = InstanceExtensionsAvailable();
        for (const char* name : InstanceExtensions)
        {
            if (Contains(available, name))
                AddMissing(extensions, name);
        }

        VkInstanceCreateInfo patched = *info;
        patched.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        patched.ppEnabledExtensionNames = extensions.data();
        VkResult result = nextCreateInstance(&patched, allocator, instance);
        if (result != VK_SUCCESS)
        {
            SetNote("the Vulkan instance refused DLSS's extensions");
            result = nextCreateInstance(info, allocator, instance);
        }
        if (result == VK_SUCCESS)
            createdInstance = *instance;
        return result;
    }

    // A feature that the hook switched on in one of Unity's own structures, to switch back if the device refuses.
    struct Enabled
    {
        VkBool32* feature;
        VkBool32 previous;
    };

    void Enable(VkBool32& feature, std::vector<Enabled>& enabled)
    {
        enabled.push_back({&feature, feature});
        feature = VK_TRUE;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkDevice* device)
    {
        if (createdInstance == VK_NULL_HANDLE)
        {
            SetNote("the Vulkan instance was not created through the plugin's hooks");
            return nextCreateDevice(physicalDevice, info, allocator, device);
        }
        if (!IsNvidia(physicalDevice))
        {
            SetNote("the GPU is not an NVIDIA GPU");
            return nextCreateDevice(physicalDevice, info, allocator, device);
        }

        std::vector<VkExtensionProperties> available = DeviceExtensionsAvailable(physicalDevice);
        std::vector<const char*> extensions(info->ppEnabledExtensionNames,
            info->ppEnabledExtensionNames + info->enabledExtensionCount);
        for (const char* name : DeviceExtensions)
        {
            if (!Contains(available, name))
            {
                SetNote("the NVIDIA GPU or its driver lacks a Vulkan extension that DLSS needs");
                return nextCreateDevice(physicalDevice, info, allocator, device);
            }

            AddMissing(extensions, name);
        }

        VkPhysicalDeviceVulkan12Features* vulkan12 = nullptr;
        VkPhysicalDeviceTimelineSemaphoreFeatures* timeline = nullptr;
        VkPhysicalDeviceBufferDeviceAddressFeatures* address = nullptr;
        for (auto* next = static_cast<VkBaseOutStructure*>(const_cast<void*>(info->pNext)); next != nullptr;
            next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
                vulkan12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(next);
            else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
                timeline = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(next);
            else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)
                address = reinterpret_cast<VkPhysicalDeviceBufferDeviceAddressFeatures*>(next);
        }

        // A chain may hold VkPhysicalDeviceVulkan12Features or the structures of the single features, never both.
        std::vector<Enabled> enabled;
        VkPhysicalDeviceTimelineSemaphoreFeatures ownTimeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceBufferDeviceAddressFeatures ownAddress{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        const void* chain = info->pNext;
        if (vulkan12 != nullptr)
        {
            Enable(vulkan12->timelineSemaphore, enabled);
            Enable(vulkan12->descriptorIndexing, enabled);
            Enable(vulkan12->bufferDeviceAddress, enabled);
        }
        else
        {
            if (timeline != nullptr)
            {
                Enable(timeline->timelineSemaphore, enabled);
            }
            else
            {
                ownTimeline.timelineSemaphore = VK_TRUE;
                ownTimeline.pNext = const_cast<void*>(chain);
                chain = &ownTimeline;
            }

            if (address != nullptr)
            {
                Enable(address->bufferDeviceAddress, enabled);
            }
            else
            {
                ownAddress.bufferDeviceAddress = VK_TRUE;
                ownAddress.pNext = const_cast<void*>(chain);
                chain = &ownAddress;
            }

            for (const char* name : FeatureExtensions)
            {
                if (Contains(available, name))
                    AddMissing(extensions, name);
            }
        }

        VkDeviceCreateInfo patched = *info;
        patched.pNext = chain;
        patched.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        patched.ppEnabledExtensionNames = extensions.data();
        VkResult result = nextCreateDevice(physicalDevice, &patched, allocator, device);
        if (result != VK_SUCCESS)
        {
            for (const Enabled& entry : enabled)
                *entry.feature = entry.previous;
            SetNote("the Vulkan device refused DLSS's extensions or features");
            return nextCreateDevice(physicalDevice, info, allocator, device);
        }

        deviceReady = true;
        return result;
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name)
    {
        if (std::strcmp(name, "vkCreateInstance") == 0)
        {
            nextCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(nextGetInstanceProcAddr(instance, name));
            return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
        }
        if (std::strcmp(name, "vkCreateDevice") == 0)
        {
            nextCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(nextGetInstanceProcAddr(instance, name));
            return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
        }
        return nextGetInstanceProcAddr(instance, name);
    }

    PFN_vkGetInstanceProcAddr Intercept(PFN_vkGetInstanceProcAddr getInstanceProcAddr)
    {
        SetNote("the plugin's hooks were installed, but the Vulkan device was not created through them");
        nextGetInstanceProcAddr = getInstanceProcAddr;
        return &GetInstanceProcAddr;
    }

    PFN_vkGetInstanceProcAddr UNITY_INTERFACE_API InterceptInitialization(
        PFN_vkGetInstanceProcAddr getInstanceProcAddr, void*)
    {
        if (hookedByLayer)
            return getInstanceProcAddr;

        hookedByUnity = true;
        return Intercept(getInstanceProcAddr);
    }
}

void RegisterVulkanIntercept(IUnityInterfaces* interfaces)
{
    if (hookedByLayer)
        return;

    // Registering fails once Unity has created its device, whatever the API. Starting a session reports that case.
    auto* unity = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsVulkanV2);
    if (unity != nullptr)
        unity->AddInterceptInitialization(&InterceptInitialization, nullptr, 1);
}

bool VulkanDeviceReady()
{
    return deviceReady;
}

std::string VulkanInterceptNote()
{
    std::lock_guard lock(noteMutex);
    return note;
}

extern "C"
{
    // Called by a Vulkan layer with the next vkGetInstanceProcAddr in its chain. The layer creates the instance and
    // the device through the function returned. Null means that Unity has the hooks already and the layer should
    // leave this module alone.
    UNITY_INTERFACE_EXPORT PFN_vkGetInstanceProcAddr RetooledVulkanIntercept(PFN_vkGetInstanceProcAddr next)
    {
        if (hookedByUnity)
            return nullptr;

        hookedByLayer = true;
        return Intercept(next);
    }
}
