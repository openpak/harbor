// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Reads the device for the device check: every value comes from the running system.

#include <cstdlib>
#include <sstream>

#include <dlfcn.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "harbor_setup/device_check.h"

namespace HarborSetup {

namespace {

#if defined(__ANDROID__)
std::string Property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}
#endif

// A Vulkan 1.0 instance on the system loader; the first device's version and name.
void ProbeVulkan(DeviceInfo& d) {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    d.vulkan_major = 0;
    d.vulkan_minor = 0;
    if (!lib) {
        return;
    }

    const auto get_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    const auto create =
        get_proc ? reinterpret_cast<PFN_vkCreateInstance>(get_proc(nullptr, "vkCreateInstance"))
                 : nullptr;
    if (!create) {
        return;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Harbor setup";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    if (create(&info, nullptr, &instance) != VK_SUCCESS) {
        return;
    }

    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get_proc(instance, "vkEnumeratePhysicalDevices"));
    const auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        get_proc(instance, "vkGetPhysicalDeviceProperties"));
    const auto destroy =
        reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));

    uint32_t count = 1;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    const VkResult result =
        enumerate ? enumerate(instance, &count, &device) : VK_ERROR_INITIALIZATION_FAILED;
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count > 0 && props) {
        VkPhysicalDeviceProperties p{};
        props(device, &p);
        d.vulkan_major = static_cast<int>(VK_API_VERSION_MAJOR(p.apiVersion));
        d.vulkan_minor = static_cast<int>(VK_API_VERSION_MINOR(p.apiVersion));
        d.gpu = std::string(p.deviceName);
    }
    if (destroy) {
        destroy(instance, nullptr);
    }
    // The loader stays loaded: unloading it while driver threads may still run is not worth the bytes.
}

} // namespace

DeviceInfo ProbeDevice(const std::string& data_dir, bool probe_vulkan) {
    DeviceInfo d;

#if defined(__ANDROID__)
    // Build.SUPPORTED_ABIS comes from this property.
    std::vector<std::string> abis;
    std::stringstream list(Property("ro.product.cpu.abilist"));
    for (std::string abi; std::getline(list, abi, ',');) {
        abis.push_back(abi);
    }
    d.abis = abis;
#elif defined(__aarch64__)
    d.abis = std::vector<std::string>{"arm64"};
#elif defined(__x86_64__)
    d.abis = std::vector<std::string>{"x64"};
#else
    d.abis = std::vector<std::string>{"unknown"};
#endif

    // The same figure as ActivityManager.MemoryInfo.totalMem (MemTotal).
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        d.total_memory = static_cast<long long>(pages) * page_size;
    }

    struct statvfs fs {};
    if (statvfs(data_dir.c_str(), &fs) == 0) {
        d.free_storage = static_cast<long long>(fs.f_bavail) * static_cast<long long>(fs.f_frsize);
    }

#if defined(__ANDROID__)
    ProbeVulkan(d);
    const std::string vulkan_hw = Property("ro.hardware.vulkan");
    const std::string egl_hw = Property("ro.hardware.egl");
    d.adreno = vulkan_hw == "adreno" || egl_hw == "adreno";
    d.android_api = std::atoi(Property("ro.build.version.sdk").c_str());
    d.android_release = Property("ro.build.version.release");
#else
    // The desktop rows are processor, memory and storage, as in the design.
    if (probe_vulkan) {
        ProbeVulkan(d);
    }
#endif
    return d;
}

} // namespace HarborSetup
