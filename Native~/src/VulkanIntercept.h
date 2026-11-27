#pragma once

#include <string>

struct IUnityInterfaces;

// DLSS needs instance and device extensions and device features that can only be enabled when the Vulkan device is
// created, so the plugin hooks Unity's instance and device creation and adds them on NVIDIA GPUs that have them.
//
// Unity runs the hooks only if they are registered from UnityPluginLoad before it creates the device, which is so in
// the editor when the plugin loads on startup. Players load plugins after the device exists, so there a Vulkan layer
// loads this module earlier and installs the same hooks through the RetooledVulkanIntercept export.
void RegisterVulkanIntercept(IUnityInterfaces* interfaces);

// Whether Unity's Vulkan device was created through the hooks, with what DLSS needs.
bool VulkanDeviceReady();

// Why the hooks left the device as Unity asked for it, or empty. Device creation happens before Unity's log can be
// reached, so the plugin logs this when a session starts.
std::string VulkanInterceptNote();
