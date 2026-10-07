// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/hle/service/btm/btm_system_core.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"

namespace Service::BTM {

IBtmSystemCore::IBtmSystemCore(Core::System& system_)
    : ServiceFramework{system_, "IBtmSystemCore"}, service_context{system_, "IBtmSystemCore"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, C<&IBtmSystemCore::StartGamepadPairing>, "StartGamepadPairing"},
        {1, C<&IBtmSystemCore::CancelGamepadPairing>, "CancelGamepadPairing"},
        {2, C<&IBtmSystemCore::ClearGamepadPairingDatabase>, "ClearGamepadPairingDatabase"},
        {3, C<&IBtmSystemCore::GetPairedGamepadCount>, "GetPairedGamepadCount"},
        {4, C<&IBtmSystemCore::EnableRadio>, "EnableRadio"},
        {5, C<&IBtmSystemCore::DisableRadio>, "DisableRadio"},
        {6, C<&IBtmSystemCore::IsRadioEnabled>, "IsRadioEnabled"},
        {7, C<&IBtmSystemCore::AcquireRadioEvent>, "AcquireRadioEvent"},
        {8, C<&IBtmSystemCore::AcquireGamepadPairingEvent>, "AcquireGamepadPairingEvent"},
        {9, C<&IBtmSystemCore::IsGamepadPairingStarted>, "IsGamepadPairingStarted"},
        {10, C<&IBtmSystemCore::StartAudioDeviceDiscovery>, "StartAudioDeviceDiscovery"},
        {11, C<&IBtmSystemCore::StopAudioDeviceDiscovery>, "StopAudioDeviceDiscovery"},
        {12, C<&IBtmSystemCore::IsDiscoveryingAudioDevice>, "IsDiscoveryingAudioDevice"},
        {13, C<&IBtmSystemCore::GetDiscoveredAudioDevice>, "GetDiscoveredAudioDevice"},
        {14, C<&IBtmSystemCore::AcquireAudioDeviceConnectionEvent>, "AcquireAudioDeviceConnectionEvent"},
        {15, C<&IBtmSystemCore::ConnectAudioDevice>, "ConnectAudioDevice"},
        {16, C<&IBtmSystemCore::IsConnectingAudioDevice>, "IsConnectingAudioDevice"},
        {17, C<&IBtmSystemCore::GetConnectedAudioDevices>, "GetConnectedAudioDevices"},
        {18, C<&IBtmSystemCore::DisconnectAudioDevice>, "DisconnectAudioDevice"},
        {19, C<&IBtmSystemCore::AcquirePairedAudioDeviceInfoChangedEvent>, "AcquirePairedAudioDeviceInfoChangedEvent"},
        {20, C<&IBtmSystemCore::GetPairedAudioDevices>, "GetPairedAudioDevices"},
        {21, C<&IBtmSystemCore::RemoveAudioDevicePairing>, "RemoveAudioDevicePairing"},
        {22, C<&IBtmSystemCore::RequestAudioDeviceConnectionRejection>, "RequestAudioDeviceConnectionRejection"},
        {23, C<&IBtmSystemCore::CancelAudioDeviceConnectionRejection>, "CancelAudioDeviceConnectionRejection"}
    };
    // clang-format on

    RegisterHandlers(functions);
    radio_event = service_context.CreateEvent("IBtmSystemCore::RadioEvent");
    audio_device_connection_event =
        service_context.CreateEvent("IBtmSystemCore::AudioDeviceConnectionEvent");
    gamepad_pairing_event = service_context.CreateEvent("IBtmSystemCore::GamepadPairingEvent");
    paired_audio_device_info_changed_event =
        service_context.CreateEvent("IBtmSystemCore::PairedAudioDeviceInfoChangedEvent");

    m_set_sys =
        system.ServiceManager().GetService<Service::Set::ISystemSettingsServer>("set:sys", true);
}

IBtmSystemCore::~IBtmSystemCore() {
    service_context.CloseEvent(radio_event);
    service_context.CloseEvent(audio_device_connection_event);
    service_context.CloseEvent(gamepad_pairing_event);
    service_context.CloseEvent(paired_audio_device_info_changed_event);
}

Result IBtmSystemCore::StartGamepadPairing() {
    LOG_WARNING(Service_BTM, "(STUBBED) called");
    gamepad_pairing_started = true;
    R_SUCCEED();
}

Result IBtmSystemCore::CancelGamepadPairing() {
    LOG_WARNING(Service_BTM, "(STUBBED) called");
    gamepad_pairing_started = false;
    R_SUCCEED();
}

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): there is no Bluetooth radio to
// drive. No gamepad is paired over Bluetooth (controllers come from the host through hid), pairing
// never completes, and with nothing to discover, connect or pair every audio device list is empty.
Result IBtmSystemCore::ClearGamepadPairingDatabase() {
    LOG_DEBUG(Service_BTM, "called");
    R_SUCCEED();
}

Result IBtmSystemCore::GetPairedGamepadCount(Out<u8> out_count) {
    LOG_DEBUG(Service_BTM, "called");
    *out_count = 0;
    R_SUCCEED();
}

Result IBtmSystemCore::AcquireGamepadPairingEvent(
    Out<bool> out_is_valid, OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_BTM, "called");
    *out_is_valid = true;
    *out_event = &gamepad_pairing_event->GetReadableEvent();
    R_SUCCEED();
}

Result IBtmSystemCore::IsGamepadPairingStarted(Out<bool> out_is_started) {
    LOG_DEBUG(Service_BTM, "called");
    *out_is_started = gamepad_pairing_started;
    R_SUCCEED();
}

Result IBtmSystemCore::StartAudioDeviceDiscovery() {
    LOG_INFO(Service_BTM, "audio device discovery requested; no Bluetooth radio, nothing will be found");
    R_SUCCEED();
}

Result IBtmSystemCore::StopAudioDeviceDiscovery() {
    LOG_DEBUG(Service_BTM, "called");
    R_SUCCEED();
}

Result IBtmSystemCore::IsDiscoveryingAudioDevice(Out<bool> out_is_discovering) {
    LOG_DEBUG(Service_BTM, "called");
    *out_is_discovering = false;
    R_SUCCEED();
}

Result IBtmSystemCore::ConnectAudioDevice() {
    // Unreachable from the menu while the discovered and paired lists are empty.
    LOG_WARNING(Service_BTM, "ConnectAudioDevice with no Bluetooth radio; nothing connects");
    R_SUCCEED();
}

Result IBtmSystemCore::IsConnectingAudioDevice(Out<bool> out_is_connecting) {
    LOG_DEBUG(Service_BTM, "called");
    *out_is_connecting = false;
    R_SUCCEED();
}

Result IBtmSystemCore::DisconnectAudioDevice() {
    LOG_WARNING(Service_BTM, "DisconnectAudioDevice with no Bluetooth radio");
    R_SUCCEED();
}

Result IBtmSystemCore::AcquirePairedAudioDeviceInfoChangedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_BTM, "called");
    *out_event = &paired_audio_device_info_changed_event->GetReadableEvent();
    R_SUCCEED();
}

Result IBtmSystemCore::RemoveAudioDevicePairing() {
    LOG_WARNING(Service_BTM, "RemoveAudioDevicePairing with no Bluetooth radio");
    R_SUCCEED();
}

Result IBtmSystemCore::EnableRadio() {
    LOG_DEBUG(Service_BTM, "called");

    R_RETURN(m_set_sys->SetBluetoothEnableFlag(true));
}
Result IBtmSystemCore::DisableRadio() {
    LOG_DEBUG(Service_BTM, "called");

    R_RETURN(m_set_sys->SetBluetoothEnableFlag(false));
}

Result IBtmSystemCore::IsRadioEnabled(Out<bool> out_is_enabled) {
    LOG_DEBUG(Service_BTM, "called");

    R_RETURN(m_set_sys->GetBluetoothEnableFlag(out_is_enabled));
}

Result IBtmSystemCore::AcquireRadioEvent(Out<bool> out_is_valid,
                                         OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_BTM, "(STUBBED) called");

    *out_is_valid = true;
    *out_event = &radio_event->GetReadableEvent();
    R_SUCCEED();
}

Result IBtmSystemCore::GetDiscoveredAudioDevice(OutArray<std::array<u8, 0xFF>, BufferAttr_HipcPointer> out_audio_devices, s32 count, Out<s32> out_total) {
    LOG_WARNING(Service_BTM, "(STUBBED) called");

    *out_total = 0;
    R_SUCCEED();
}

Result IBtmSystemCore::AcquireAudioDeviceConnectionEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_BTM, "(STUBBED) called");

    *out_event = &audio_device_connection_event->GetReadableEvent();
    R_SUCCEED();
}

Result IBtmSystemCore::GetConnectedAudioDevices(
    Out<s32> out_count, OutArray<std::array<u8, 0xFF>, BufferAttr_HipcPointer> out_audio_devices) {
    LOG_WARNING(Service_BTM, "(STUBBED) called");

    *out_count = 0;
    R_SUCCEED();
}

Result IBtmSystemCore::GetPairedAudioDevices(
    Out<s32> out_count, OutArray<std::array<u8, 0xFF>, BufferAttr_HipcPointer> out_audio_devices) {
    LOG_WARNING(Service_BTM, "(STUBBED) called");

    *out_count = 0;
    R_SUCCEED();
}

Result IBtmSystemCore::RequestAudioDeviceConnectionRejection(ClientAppletResourceUserId aruid) {
    LOG_WARNING(Service_BTM, "(STUBBED) called, applet_resource_user_id={}", aruid.pid);
    R_SUCCEED();
}

Result IBtmSystemCore::CancelAudioDeviceConnectionRejection(ClientAppletResourceUserId aruid) {
    LOG_WARNING(Service_BTM, "(STUBBED) called, applet_resource_user_id={}", aruid.pid);
    R_SUCCEED();
}

} // namespace Service::BTM
