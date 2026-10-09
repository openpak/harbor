// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/audio_core.h"
#include "audio_core/sink/sink_stream.h"
#include "core/hle/service/audio/audio_controller.h"
#include "core/hle/service/audio/audio_out_manager.h"
#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"
#include "common/redact.h"
#include "common/settings.h"
#include "common/harbor_crash.h"
#include <algorithm>

namespace Service::Audio {

IAudioController::IAudioController(Core::System& system_)
    : ServiceFramework{system_, "audctl"}, service_context{system, "audctl"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IAudioController::GetTargetVolume>, "GetTargetVolume"},
        {1, D<&IAudioController::SetTargetVolume>, "SetTargetVolume"},
        {2, D<&IAudioController::GetTargetVolumeMin>, "GetTargetVolumeMin"},
        {3, D<&IAudioController::GetTargetVolumeMax>, "GetTargetVolumeMax"},
        {4, D<&IAudioController::IsTargetMute>, "IsTargetMute"},
        {5, D<&IAudioController::SetTargetMute>, "SetTargetMute"},
        {6, nullptr, "IsTargetConnected"}, //20.0.0+
        {7, nullptr, "SetDefaultTarget"},
        {8, nullptr, "GetDefaultTarget"},
        {9, D<&IAudioController::GetAudioOutputMode>, "GetAudioOutputMode"},
        {10, D<&IAudioController::SetAudioOutputMode>, "SetAudioOutputMode"},
        {11, nullptr, "SetForceMutePolicy"},
        {12, D<&IAudioController::GetForceMutePolicy>, "GetForceMutePolicy"},
        {13, D<&IAudioController::GetOutputModeSetting>, "GetOutputModeSetting"},
        {14, D<&IAudioController::SetOutputModeSetting>, "SetOutputModeSetting"},
        {15, nullptr, "SetOutputTarget"},
        {16, nullptr, "SetInputTargetForceEnabled"},
        {17, D<&IAudioController::SetHeadphoneOutputLevelMode>, "SetHeadphoneOutputLevelMode"},
        {18, D<&IAudioController::GetHeadphoneOutputLevelMode>, "GetHeadphoneOutputLevelMode"},
        {19, nullptr, "AcquireAudioVolumeUpdateEventForPlayReport"},
        {20, nullptr, "AcquireAudioOutputDeviceUpdateEventForPlayReport"},
        {21, nullptr, "GetAudioOutputTargetForPlayReport"},
        {22, D<&IAudioController::NotifyHeadphoneVolumeWarningDisplayedEvent>, "NotifyHeadphoneVolumeWarningDisplayedEvent"},
        {23, nullptr, "SetSystemOutputMasterVolume"},
        {24, nullptr, "GetSystemOutputMasterVolume"},
        {25, nullptr, "GetAudioVolumeDataForPlayReport"},
        {26, nullptr, "UpdateHeadphoneSettings"},
        {27, nullptr, "SetVolumeMappingTableForDev"},
        {28, nullptr, "GetAudioOutputChannelCountForPlayReport"},
        {29, nullptr, "BindAudioOutputChannelCountUpdateEventForPlayReport"},
        {30, D<&IAudioController::SetSpeakerAutoMuteEnabled>, "SetSpeakerAutoMuteEnabled"},
        {31, D<&IAudioController::IsSpeakerAutoMuteEnabled>, "IsSpeakerAutoMuteEnabled"},
        {32, D<&IAudioController::GetActiveOutputTarget>, "GetActiveOutputTarget"},
        {33, nullptr, "GetTargetDeviceInfo"},
        {34, D<&IAudioController::AcquireTargetNotification>, "AcquireTargetNotification"},
        {35, nullptr, "SetHearingProtectionSafeguardTimerRemainingTimeForDebug"},
        {36, nullptr, "GetHearingProtectionSafeguardTimerRemainingTimeForDebug"},
        {37, nullptr, "SetHearingProtectionSafeguardEnabled"},
        {38, nullptr, "IsHearingProtectionSafeguardEnabled"},
        {39, nullptr, "IsHearingProtectionSafeguardMonitoringOutputForDebug"},
        {40, nullptr, "GetSystemInformationForDebug"},
        {41, nullptr, "SetVolumeButtonLongPressTime"},
        {42, nullptr, "SetNativeVolumeForDebug"},
        {43, nullptr, "Unknown43"}, //21.0.0+
        {5000, D<&IAudioController::Unknown5000>, "Unknown5000"}, //19.0.0+
        {10000, nullptr, "NotifyAudioOutputTargetForPlayReport"},
        {10001, nullptr, "NotifyAudioOutputChannelCountForPlayReport"},
        {10002, nullptr, "NotifyUnsupportedUsbOutputDeviceAttachedForPlayReport"},
        {10100, nullptr, "GetAudioVolumeDataForPlayReport"},
        {10101, nullptr, "BindAudioVolumeUpdateEventForPlayReport"},
        {10102, nullptr, "BindAudioOutputTargetUpdateEventForPlayReport"},
        {10103, nullptr, "GetAudioOutputTargetForPlayReport"},
        {10104, nullptr, "GetAudioOutputChannelCountForPlayReport"},
        {10105, nullptr, "BindAudioOutputChannelCountUpdateEventForPlayReport"}, //14.0.0-19.0.1
        {10106, nullptr, "GetDefaultAudioOutputTargetForPlayReport"}, //14.0.0-19.0.1
        {10200, nullptr, "Unknown10200"}, //20.0.0+
        {50000, nullptr, "SetAnalogInputBoostGainForPrototyping"}, //15.0.0-18.1.0
        {50001, nullptr, "OverrideDefaultTargetForDebug"}, //19.0.0-19.0.1
        {50003, nullptr, "SetForceOverrideExternalDeviceNameForDebug"}, //19.0.0+
        {50004, nullptr, "ClearForceOverrideExternalDeviceNameForDebug"} //19.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);

    HARBOR_CRUMB("audctl: IAudioController created");
    // A new session starts at the console's full volume; audctl lowers it in a HOME session.
    AudioCore::Sink::SetConsoleOutputGain(1.0f);

    m_set_sys =
        system.ServiceManager().GetService<Service::Set::ISystemSettingsServer>("set:sys", true);
    HARBOR_CRUMB_ONCE("audctl: set:sys reached");
    notification_event = service_context.CreateEvent("IAudioController:NotificationEvent");

    // Probably shouldn't do this in constructor?
    try {
        const int ui_volume = Settings::values.volume.GetValue();
        const int mapped = static_cast<int>(std::lround((static_cast<double>(ui_volume) / 100.0) * 15.0));
        const auto active_idx = static_cast<size_t>(m_active_target);
        if (active_idx < m_target_volumes.size()) {
            m_target_volumes[active_idx] = std::clamp(mapped, 0, 15);
        }

        const bool muted = Settings::values.audio_muted.GetValue();
        for (auto& m : m_target_muted) {
            m = muted;
        }

        if (!muted && active_idx < m_target_volumes.size()) {
            const float vol_f = static_cast<float>(m_target_volumes[active_idx]) / 15.0f;
            try {
                auto& sink = system.AudioCore().GetOutputSink();
                sink.SetSystemVolume(vol_f);
                sink.SetDeviceVolume(vol_f);
            } catch (...) {
                LOG_WARNING(Audio, "Failed to apply initial sink volume from settings");
            }

            if (auto audout_mgr = system.ServiceManager().GetService<Service::Audio::IAudioOutManager>("audout:u")) {
                audout_mgr->SetAllAudioOutVolume(static_cast<float>(m_target_volumes[active_idx]) / 15.0f);
            }
        }
    } catch (...) {
        // Catch if something fails, since this is constructor
    }
    HARBOR_CRUMB_ONCE("audctl: IAudioController constructed");
}

IAudioController::~IAudioController() {
    service_context.CloseEvent(notification_event);
};

Result IAudioController::GetTargetVolumeMin(Out<s32> out_target_min_volume) {
    LOG_DEBUG(Audio, "called.");

    // This service function is currently hardcoded on the
    // actual console to this value (as of 8.0.0).
    *out_target_min_volume = 0;
    R_SUCCEED();
}

Result IAudioController::GetTargetVolumeMax(Out<s32> out_target_max_volume) {
    LOG_DEBUG(Audio, "called.");

    // This service function is currently hardcoded on the
    // actual console to this value (as of 8.0.0).
    *out_target_max_volume = 15;
    R_SUCCEED();
}

Result IAudioController::GetAudioOutputMode(Out<Set::AudioOutputMode> out_output_mode,
                                            Set::AudioOutputModeTarget target) {
    const auto result = m_set_sys->GetAudioOutputMode(out_output_mode, target);

    LOG_INFO(Service_SET, "called, target={}, output_mode={}", target, *out_output_mode);
    R_RETURN(result);
}

Result IAudioController::SetAudioOutputMode(Set::AudioOutputModeTarget target,
                                            Set::AudioOutputMode output_mode) {
    LOG_INFO(Service_SET, "called, target={}, output_mode={}", target, output_mode);

    R_RETURN(m_set_sys->SetAudioOutputMode(target, output_mode));
}

Result IAudioController::GetForceMutePolicy(Out<ForceMutePolicy> out_mute_policy) {
    LOG_WARNING(Audio, "(STUBBED) called");

    // Removed on FW 13.2.1+
    *out_mute_policy = ForceMutePolicy::Disable;
    R_SUCCEED();
}

Result IAudioController::GetOutputModeSetting(Out<Set::AudioOutputMode> out_output_mode,
                                              Set::AudioOutputModeTarget target) {
    LOG_WARNING(Audio, "(STUBBED) called, target={}", target);

    *out_output_mode = Set::AudioOutputMode::ch_7_1;
    R_SUCCEED();
}

Result IAudioController::SetOutputModeSetting(Set::AudioOutputModeTarget target,
                                              Set::AudioOutputMode output_mode) {
    LOG_INFO(Service_SET, "called, target={}, output_mode={}", target, output_mode);
    R_SUCCEED();
}

Result IAudioController::SetHeadphoneOutputLevelMode(HeadphoneOutputLevelMode output_level_mode) {
    LOG_WARNING(Audio, "(STUBBED) called, output_level_mode={}", output_level_mode);
    R_SUCCEED();
}

Result IAudioController::GetHeadphoneOutputLevelMode(
    Out<HeadphoneOutputLevelMode> out_output_level_mode) {
    LOG_INFO(Audio, "called");

    *out_output_level_mode = HeadphoneOutputLevelMode::Normal;
    R_SUCCEED();
}

Result IAudioController::NotifyHeadphoneVolumeWarningDisplayedEvent() {
    LOG_WARNING(Service_Audio, "(STUBBED) called");
    R_SUCCEED();
}

Result IAudioController::SetSpeakerAutoMuteEnabled(bool is_speaker_auto_mute_enabled) {
    LOG_INFO(Audio, "called, is_speaker_auto_mute_enabled={}", is_speaker_auto_mute_enabled);

    R_RETURN(m_set_sys->SetSpeakerAutoMuteFlag(is_speaker_auto_mute_enabled));
}

Result IAudioController::IsSpeakerAutoMuteEnabled(Out<bool> out_is_speaker_auto_mute_enabled) {
    const auto result = m_set_sys->GetSpeakerAutoMuteFlag(out_is_speaker_auto_mute_enabled);

    LOG_INFO(Audio, "called, is_speaker_auto_mute_enabled={}", *out_is_speaker_auto_mute_enabled);
    R_RETURN(result);
}

Result IAudioController::AcquireTargetNotification(
    OutCopyHandle<Kernel::KReadableEvent> out_notification_event) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    // qlaunch's first audctl calls at boot: the console's stored volume applies from here, as the
    // audio sysmodule applies it at its own start.
    UseConsoleGain();

    *out_notification_event = &notification_event->GetReadableEvent();
    R_SUCCEED();
}
Result IAudioController::Unknown5000(Out<SharedPointer<IAudioController>> out_audio_controller) {
    // Undocumented (19.0.0+): another audctl object; this one is answered, as Eden did, where the
    // Ryujinx Harbor opens a new session (the two share their state either way).
    LOG_UNKNOWN_CONTRACT(Audio, "audctl 5000: answered this audctl object again");
    UseConsoleGain();

    // Return a new reference to this controller instance
    *out_audio_controller = std::static_pointer_cast<IAudioController>(shared_from_this());

    R_SUCCEED();
}

// In a HOME session HOME's volume slider sets the active target's volume (0-15) and mute, and that
// is what the console hears: a gain after the mix, linear in the step and 0 when muted, apart from
// the host's own volume. Outside a HOME session Eden's stock path (the host volume mapped onto the
// sink and audout volumes) applies unchanged.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base), AudioController.cs.
bool IAudioController::UseConsoleGain() {
    if (!m_console_gain) {
        HARBOR_CRUMB_ONCE("audctl: console volume decided");
        m_console_gain = system.GetAppletManager().IsHomeSession();
        if (*m_console_gain) {
            // The constructor seeded the targets and the sink from the host's volume: a console
            // starts at the volume its audio sysmodule stored in set:sys (full and unmuted on a
            // NAND that never stored one), and the host's volume applies on its own.
            m_target_volumes.fill(15);
            m_target_muted.fill(false);
            RestoreStoredVolume(Set::AudioOutputModeTarget::Speaker);
            RestoreStoredVolume(Set::AudioOutputModeTarget::Headphone);
            try {
                auto& sink = system.AudioCore().GetOutputSink();
                sink.SetSystemVolume(1.0f);
                sink.SetDeviceVolume(1.0f);
            } catch (...) {
                LOG_WARNING(Audio, "Failed to reset the sink volume for the console volume");
            }
            ApplyConsoleGain();
            HARBOR_CRUMB_ONCE("audctl: console volume applied");
        }
    }
    return *m_console_gain;
}

// The set:sys record of an output's volume (AudioVolumeTarget), for the targets that have one.
static std::optional<Set::AudioVolumeTarget> VolumeRecordFor(Set::AudioOutputModeTarget target) {
    switch (target) {
    case Set::AudioOutputModeTarget::Speaker:
        return Set::AudioVolumeTarget::Speaker;
    case Set::AudioOutputModeTarget::Headphone:
        return Set::AudioVolumeTarget::Headphone;
    default:
        return std::nullopt;
    }
}

void IAudioController::RestoreStoredVolume(Set::AudioOutputModeTarget target) {
    HARBOR_CRUMB("audctl: volume restore");
    const auto record = VolumeRecordFor(target);
    if (!record || !m_set_sys) {
        return;
    }
    const auto stored = m_set_sys->GetStoredAudioVolume(*record);
    if (!stored) {
        return;
    }
    const auto idx = static_cast<size_t>(target);
    m_target_volumes[idx] = std::clamp<s32>(stored->volume, 0, 15);
    m_target_muted[idx] = (stored->flags & 1) != 0;
    LOG_INFO(Audio, "audctl: target {} restored to volume {}/15{}", static_cast<u32>(target),
             m_target_volumes[idx], m_target_muted[idx] ? ", muted" : "");
}

void IAudioController::StoreVolume(Set::AudioOutputModeTarget target) {
    const auto record = VolumeRecordFor(target);
    if (!record || !m_set_sys) {
        return;
    }
    const auto idx = static_cast<size_t>(target);
    Set::AudioVolume volume{};
    if (const auto stored = m_set_sys->GetStoredAudioVolume(*record)) {
        volume.flags = stored->flags;
    }
    volume.flags = (volume.flags & ~1u) | (m_target_muted[idx] ? 1u : 0u);
    volume.volume = static_cast<u8>(m_target_volumes[idx]);
    (void)m_set_sys->SetAudioVolume(volume, *record);
}

void IAudioController::ApplyConsoleGain() {
    const auto idx = static_cast<size_t>(m_active_target);
    const bool muted = m_target_muted[idx];
    const f32 gain = muted ? 0.0f : static_cast<f32>(m_target_volumes[idx]) / 15.0f;
    AudioCore::Sink::SetConsoleOutputGain(gain);
    LOG_INFO(Audio, "audctl: console output gain {:.3f} (volume {}/15{})", gain,
             m_target_volumes[idx], muted ? ", muted" : "");
}

Result IAudioController::GetTargetVolume(Out<s32> out_target_volume, Set::AudioOutputModeTarget target) {
    LOG_DEBUG(Audio, "GetTargetVolume called, target={}", target);
    UseConsoleGain();

    const auto idx = static_cast<size_t>(target);
    if (idx >= m_target_volumes.size()) {
        LOG_ERROR(Audio, "GetTargetVolume invalid target {}", idx);
        R_THROW(ResultInvalidArgument);
    }

    *out_target_volume = m_target_volumes[idx];
    R_SUCCEED();
}

Result IAudioController::SetTargetVolume(Set::AudioOutputModeTarget target, s32 target_volume) {
    LOG_INFO(Audio, "SetTargetVolume called, target={}, volume={}", target, target_volume);
    const bool console_gain = UseConsoleGain();

    const auto idx = static_cast<size_t>(target);
    if (idx >= m_target_volumes.size()) {
        LOG_ERROR(Audio, "SetTargetVolume invalid target {}", idx);
        R_THROW(ResultInvalidArgument);
    }

    if (target_volume < 0) {
        target_volume = 0;
    } else if (target_volume > 15) {
        target_volume = 15;
    }

    m_target_volumes[idx] = target_volume;

    if (console_gain) {
        StoreVolume(target);
        if (m_active_target == target) {
            ApplyConsoleGain();
        }
        R_SUCCEED();
    }

    if (m_active_target == target && !m_target_muted[idx]) {
        const float vol = static_cast<float>(target_volume) / 15.0f;
        // try catch this, as we don't know how it handles it when no output is set.
        // we already have audio issues when no output is set, so catch.
        try {
            auto& sink = system.AudioCore().GetOutputSink();
            sink.SetSystemVolume(vol);
            sink.SetDeviceVolume(vol);
        } catch (...) {
            LOG_WARNING(Audio, "Failed to set sink system volume");
        }

        if (auto audout_mgr = system.ServiceManager().GetService<IAudioOutManager>("audout:u")) {
            audout_mgr->SetAllAudioOutVolume(vol);
        }
    }

    if (m_active_target == target) {
        const int ui_volume = static_cast<int>(std::lround((static_cast<double>(target_volume) / 15.0) * 100.0));
        Settings::values.volume.SetValue(static_cast<u8>(std::clamp(ui_volume, 0, 100)));
    }

    R_SUCCEED();
}

Result IAudioController::IsTargetMute(Out<bool> out_is_target_muted, Set::AudioOutputModeTarget target) {
    LOG_DEBUG(Audio, "called, target={}", target);
    UseConsoleGain();

    const auto idx = static_cast<size_t>(target);
    if (idx >= m_target_muted.size()) {
        LOG_ERROR(Audio, "invalid target {}", idx);
        R_THROW(ResultInvalidArgument);
    }

    *out_is_target_muted = m_target_muted[idx];
    R_SUCCEED();
}

Result IAudioController::SetTargetMute(bool is_muted, Set::AudioOutputModeTarget target) {
    LOG_INFO(Audio, "called, target={}, muted={}", target, is_muted);
    const bool console_gain = UseConsoleGain();

    const auto idx = static_cast<size_t>(target);
    if (idx >= m_target_muted.size()) {
        LOG_ERROR(Audio, "invalid target {}", idx);
        R_THROW(ResultInvalidArgument);
    }

    m_target_muted[idx] = is_muted;

    if (console_gain) {
        StoreVolume(target);
        if (m_active_target == target) {
            ApplyConsoleGain();
        }
        R_SUCCEED();
    }

    if (m_active_target == target) {
        try {
            auto& sink = system.AudioCore().GetOutputSink();
            if (is_muted) {
                sink.SetSystemVolume(0.0f);
                sink.SetDeviceVolume(0.0f);
            } else {
                const float vol = static_cast<float>(m_target_volumes[idx]) / 15.0f;
                sink.SetSystemVolume(vol);
                sink.SetDeviceVolume(vol);
            }
        } catch (...) {
            LOG_WARNING(Audio, "Failed to set sink system volume on mute change");
        }

        // Also update any open audout sessions via the audout:u service.
        if (auto audout_mgr = system.ServiceManager().GetService<IAudioOutManager>("audout:u")) {
            if (is_muted) {
                audout_mgr->SetAllAudioOutVolume(0.0f);
            } else {
                const float vol = static_cast<float>(m_target_volumes[idx]) / 15.0f;
                audout_mgr->SetAllAudioOutVolume(vol);
            }
        }
    }

    Settings::values.audio_muted.SetValue(is_muted);

    R_SUCCEED();
}

Result IAudioController::GetActiveOutputTarget(Out<Set::AudioOutputModeTarget> out_active_target) {
    LOG_DEBUG(Audio, "GetActiveOutputTarget called");
    *out_active_target = m_active_target;
    R_SUCCEED();
}
} // namespace Service::Audio
