// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <filesystem>
#include <system_error>

#include <QFileDialog>
#include <QMessageBox>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/frame_gen/lossless_dll.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "yuzu/harbor_frame_gen.h"

namespace {

using VideoCore::FrameGen::LosslessStatus;

QString StatusText(LosslessStatus status) {
    switch (status) {
    case LosslessStatus::Ok:
        return QObject::tr("Lossless.dll is installed.");
    case LosslessStatus::NotInstalled:
        return QObject::tr("Frame generation uses the shaders in your own copy of Lossless.dll, "
                           "from Lossless Scaling. Choose it to turn frame generation on.");
    case LosslessStatus::UnreadableFile:
        return QObject::tr("The file could not be read.");
    case LosslessStatus::NotPortableExecutable:
        return QObject::tr("The file is not Lossless.dll.");
    case LosslessStatus::MissingShaders:
        return QObject::tr("This Lossless.dll lacks the shaders frame generation needs.");
    case LosslessStatus::TranslationFailed:
        return QObject::tr("This Lossless.dll's shaders could not be translated.");
    case LosslessStatus::CacheUnusable:
        return QObject::tr("The shader cache could not be written.");
    }
    return {};
}

/// Whether the GPU Harbor renders with has what the frame generation shaders need (the renderer
/// checks the same three features and presents without generated frames when one is missing).
bool GpuCanGenerate() try {
    Vulkan::vk::InstanceDispatch dld;
    const auto library = Vulkan::OpenLibrary();
    const auto instance = Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1);
    const auto devices = instance.EnumeratePhysicalDevices();
    const size_t index = Settings::values.vulkan_device.GetValue();
    if (devices.empty() || index >= devices.size()) {
        return false;
    }
    const Vulkan::vk::PhysicalDevice device(devices[index], dld);
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .pNext = nullptr,
    };
    VkPhysicalDeviceShaderFloat16Int8Features float16_int8{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
        .pNext = &robustness2,
    };
    VkPhysicalDeviceVulkanMemoryModelFeatures memory_model{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
        .pNext = &float16_int8,
    };
    VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &memory_model,
    };
    device.GetFeatures2(features);
    const bool ok = memory_model.vulkanMemoryModel && float16_int8.shaderFloat16 &&
                    robustness2.nullDescriptor;
    LOG_INFO(Frontend, "harbor: frame generation on this GPU: {} (memory model {}, float16 {}, "
                       "null descriptor {})",
             ok ? "yes" : "no", memory_model.vulkanMemoryModel == VK_TRUE,
             float16_int8.shaderFloat16 == VK_TRUE, robustness2.nullDescriptor == VK_TRUE);
    return ok;
} catch (const Vulkan::vk::Exception& e) {
    LOG_WARNING(Frontend, "harbor: the GPU could not be checked for frame generation: {}", e.what());
    return false;
}

} // namespace

namespace HarborFrameGen {

bool GpuSupported() {
    static const bool ok = GpuCanGenerate();
    return ok;
}

void ChooseDll(QWidget* parent, std::function<void(std::function<void()>)> change_dll) {
    const QString picked = QFileDialog::getOpenFileName(parent, QObject::tr("Choose Lossless.dll"), QString{},
                                                        QObject::tr("Lossless.dll (Lossless.dll *.dll)"));
    if (picked.isEmpty()) {
        return;
    }
    // Checked before the console stops: a wrong file changes nothing.
    const std::filesystem::path source = picked.toStdU16String();
    const auto valid = VideoCore::FrameGen::ValidateLosslessDll(source);
    if (valid != LosslessStatus::Ok) {
        QMessageBox::warning(parent, QObject::tr("Frame generation"), StatusText(valid));
        return;
    }
    LosslessStatus result = LosslessStatus::Ok;
    change_dll([source, &result] {
        const auto destination = VideoCore::FrameGen::GetLosslessDllPath();
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing,
                                   ec);
        result = ec ? LosslessStatus::UnreadableFile : VideoCore::FrameGen::BuildShaderCache();
        if (result != LosslessStatus::Ok) {
            void(VideoCore::FrameGen::RemoveInstalledLosslessDll());
        }
        LOG_INFO(Frontend, "harbor: Lossless.dll {}", result == LosslessStatus::Ok ? "installed" : "rejected");
    });
    if (result != LosslessStatus::Ok) {
        QMessageBox::warning(parent, QObject::tr("Frame generation"), StatusText(result));
    }
}

void RemoveDll(std::function<void(std::function<void()>)> change_dll, std::function<void()> save) {
    change_dll([] {
        void(VideoCore::FrameGen::RemoveInstalledLosslessDll());
        LOG_INFO(Frontend, "harbor: Lossless.dll removed");
    });
    Settings::values.frame_gen.SetValue(false);
    save();
}

} // namespace HarborFrameGen
