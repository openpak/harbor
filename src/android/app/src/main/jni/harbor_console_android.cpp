// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The Harbor menu's console entries on Android (HarborNative.kt): the device key sealed into the
// NAND for a backup, restored from one, and a new console identity. The zip itself is Kotlin's.

#include <jni.h>

#include "common/android/android_common.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/harbor_calibration/calibration.h"

extern "C" {

JNIEXPORT jstring JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_nandDirectory(JNIEnv* env,
                                                                                    jobject) {
    return Common::Android::ToJString(
        env, Common::FS::GetEdenPathString(Common::FS::EdenPath::NANDDir));
}

JNIEXPORT jboolean JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_sealDeviceKey(
    JNIEnv* env, jobject, jstring passphrase) {
    return Harbor::WriteDeviceKeyBackup(Common::Android::GetJString(env, passphrase));
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_removeSealedDeviceKey(JNIEnv*,
                                                                                        jobject) {
    Harbor::RemoveDeviceKeyBackup();
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_reloadCalibration(JNIEnv*,
                                                                                    jobject) {
    Harbor::Calibration::Reload();
}

JNIEXPORT jboolean JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_deviceKeyRestorePending(
    JNIEnv*, jobject) {
    return Harbor::DeviceKeyRestorePending();
}

JNIEXPORT jint JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_restoreDeviceKey(
    JNIEnv* env, jobject, jstring passphrase) {
    return static_cast<jint>(
        Harbor::RestoreDeviceKey(Common::Android::GetJString(env, passphrase)));
}

JNIEXPORT jboolean JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborNative_makeNewConsole(JNIEnv*,
                                                                                     jobject) {
    return Harbor::MakeNewConsole();
}

} // extern "C"
