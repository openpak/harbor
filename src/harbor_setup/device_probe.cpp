// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Reads the device for the device check: every value comes from the running system.

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <system_error>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <unistd.h>
#else
#include "common/memory_detect.h"
#include "video_core/vulkan_common/vulkan_library.h"
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
// The loader stays loaded: unloading it while driver threads may still run is not worth the bytes.
PFN_vkGetInstanceProcAddr VulkanLoader() {
#if defined(__ANDROID__)
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    return lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"))
               : nullptr;
#else
    // Eden's own search: vulkan-1.dll, libvulkan.so.1, or the bundle's loader or MoltenVK on macOS.
    static const auto* const library =
        new std::shared_ptr<Common::DynamicLibrary>(Vulkan::OpenLibrary(nullptr));
    PFN_vkGetInstanceProcAddr get_proc = nullptr;
    if (*library && (*library)->IsOpen()) {
        void((*library)->GetSymbol("vkGetInstanceProcAddr", &get_proc));
    }
    return get_proc;
#endif
}

void ProbeVulkan(DeviceInfo& d) {
    d.vulkan_major = 0;
    d.vulkan_minor = 0;
    const auto get_proc = VulkanLoader();
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
#elif defined(ARCHITECTURE_arm64)
    d.abis = std::vector<std::string>{"arm64"};
#elif defined(ARCHITECTURE_x86_64)
    // (MSVC defines neither __aarch64__ nor __x86_64__; Eden's build names the architecture.)
    d.abis = std::vector<std::string>{"x64"};
#else
    d.abis = std::vector<std::string>{"unknown"};
#endif

#if defined(__ANDROID__)
    // The same figure as ActivityManager.MemoryInfo.totalMem (MemTotal).
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        d.total_memory = static_cast<long long>(pages) * page_size;
    }
#else
    d.total_memory = static_cast<long long>(Common::GetMemInfo().TotalPhysicalMemory);
#endif

    // What an unprivileged user may still write (statvfs' f_bavail on POSIX).
    std::error_code ec;
    const auto space = std::filesystem::space(data_dir, ec);
    if (!ec) {
        d.free_storage = static_cast<long long>(space.available);
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
