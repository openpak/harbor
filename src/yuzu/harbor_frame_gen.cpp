// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <filesystem>
#include <system_error>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

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

constexpr int Multipliers[] = {2, 3, 4};
constexpr int TargetRates[] = {0, 60, 90, 120};
constexpr int FlowScales[] = {0, 50, 75, 100}; // 0: auto

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

template <size_t N>
int IndexOf(const int (&values)[N], int value) {
    for (size_t i = 0; i < N; i++) {
        if (values[i] == value) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

} // namespace

HarborFrameGenDialog::HarborFrameGenDialog(QWidget* parent,
                                           std::function<void(std::function<void()>)> change_dll_,
                                           std::function<void()> save_)
    : QDialog(parent), change_dll{std::move(change_dll_)}, save{std::move(save_)},
      gpu_ok{GpuCanGenerate()} {
    setWindowTitle(tr("Frame generation"));

    status = new QLabel(this);
    status->setWordWrap(true);
    choose = new QPushButton(tr("Choose Lossless.dll..."), this);
    remove = new QPushButton(tr("Remove"), this);
    auto* dll_row = new QHBoxLayout;
    dll_row->addWidget(choose);
    dll_row->addWidget(remove);
    dll_row->addStretch();

    enabled = new QCheckBox(tr("Frame generation"), this);
    multiplier = new QComboBox(this);
    for (const int value : Multipliers) {
        multiplier->addItem(QStringLiteral("%1x").arg(value));
    }
    target_rate = new QComboBox(this);
    target_rate->addItem(tr("Use a fixed multiplier"));
    for (const int value : TargetRates) {
        if (value != 0) {
            target_rate->addItem(tr("%1 FPS").arg(value));
        }
    }
    flow_scale = new QComboBox(this);
    flow_scale->addItem(tr("Auto"));
    for (const int value : FlowScales) {
        if (value != 0) {
            flow_scale->addItem(QStringLiteral("%1%").arg(value));
        }
    }

    auto* form = new QFormLayout;
    form->addRow(enabled);
    form->addRow(tr("Frame multiplier"), multiplier);
    form->addRow(tr("Target frame rate"), target_rate);
    form->addRow(tr("Motion estimation resolution"), flow_scale);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(status);
    layout->addLayout(dll_row);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(choose, &QPushButton::clicked, this, &HarborFrameGenDialog::ChooseDll);
    connect(remove, &QPushButton::clicked, this, &HarborFrameGenDialog::RemoveDll);
    connect(enabled, &QCheckBox::toggled, this, &HarborFrameGenDialog::Store);
    for (auto* box : {multiplier, target_rate, flow_scale}) {
        connect(box, &QComboBox::currentIndexChanged, this, &HarborFrameGenDialog::Store);
    }
    Refresh();
}

void HarborFrameGenDialog::Refresh() {
    refreshing = true;
    const auto installed = VideoCore::FrameGen::GetInstalledLosslessStatus();
    const bool installed_ok = installed == LosslessStatus::Ok;
    const bool ok = installed_ok && gpu_ok;
    status->setText(gpu_ok ? StatusText(installed)
                           : tr("This GPU cannot run frame generation (it needs the Vulkan memory "
                                "model, 16-bit float shaders and null descriptors)."));
    choose->setText(installed_ok ? tr("Replace Lossless.dll...") : tr("Choose Lossless.dll..."));
    choose->setEnabled(gpu_ok);
    remove->setEnabled(installed_ok);

    auto& v = Settings::values;
    enabled->setEnabled(ok);
    enabled->setChecked(ok && v.frame_gen.GetValue());
    const bool on = ok && v.frame_gen.GetValue();
    const bool fixed = v.frame_gen_target_rate.GetValue() == 0;
    multiplier->setCurrentIndex(IndexOf(Multipliers, static_cast<int>(v.frame_gen_multiplier.GetValue())));
    target_rate->setCurrentIndex(
        IndexOf(TargetRates, static_cast<int>(v.frame_gen_target_rate.GetValue())));
    flow_scale->setCurrentIndex(v.frame_gen_flow_scale_auto.GetValue()
                                    ? 0
                                    : IndexOf(FlowScales, static_cast<int>(v.frame_gen_flow_scale.GetValue())));
    multiplier->setEnabled(on && fixed);
    target_rate->setEnabled(on);
    flow_scale->setEnabled(on);
    refreshing = false;
}

void HarborFrameGenDialog::Store() {
    if (refreshing) {
        return;
    }
    auto& v = Settings::values;
    v.frame_gen.SetValue(enabled->isChecked());
    v.frame_gen_multiplier.SetValue(static_cast<u32>(Multipliers[multiplier->currentIndex()]));
    v.frame_gen_target_rate.SetValue(static_cast<u32>(TargetRates[target_rate->currentIndex()]));
    const int flow = FlowScales[flow_scale->currentIndex()];
    v.frame_gen_flow_scale_auto.SetValue(flow == 0);
    if (flow != 0) {
        v.frame_gen_flow_scale.SetValue(static_cast<u32>(flow));
    }
    LOG_INFO(Frontend, "harbor: frame generation {} ({}x, target {} fps, flow {})",
             v.frame_gen.GetValue() ? "on" : "off", v.frame_gen_multiplier.GetValue(),
             v.frame_gen_target_rate.GetValue(), flow == 0 ? std::string{"auto"} : std::to_string(flow) + "%");
    save();
    Refresh();
}

void HarborFrameGenDialog::ChooseDll() {
    const QString picked = QFileDialog::getOpenFileName(this, tr("Choose Lossless.dll"), QString{},
                                                        tr("Lossless.dll (Lossless.dll *.dll)"));
    if (picked.isEmpty()) {
        return;
    }
    // Checked before the console stops: a wrong file changes nothing.
    const std::filesystem::path source = picked.toStdU16String();
    const auto valid = VideoCore::FrameGen::ValidateLosslessDll(source);
    if (valid != LosslessStatus::Ok) {
        QMessageBox::warning(this, tr("Frame generation"), StatusText(valid));
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
        QMessageBox::warning(this, tr("Frame generation"), StatusText(result));
    }
    Refresh();
}

void HarborFrameGenDialog::RemoveDll() {
    if (QMessageBox::question(this, tr("Frame generation"),
                              tr("Remove Lossless.dll? The console restarts.")) != QMessageBox::Yes) {
        return;
    }
    change_dll([] {
        void(VideoCore::FrameGen::RemoveInstalledLosslessDll());
        LOG_INFO(Frontend, "harbor: Lossless.dll removed");
    });
    Settings::values.frame_gen.SetValue(false);
    save();
    Refresh();
}
