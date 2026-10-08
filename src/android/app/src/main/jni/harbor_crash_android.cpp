// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Harbor's crash reports on Android (common/harbor_crash.h): consent, the fatal-signal handler and
// the reports the next launch offers (HarborCrashReports.kt).

#include <jni.h>

#include "common/android/android_common.h"
#include "common/harbor_crash.h"

namespace {

namespace HC = Common::HarborCrash;

jobjectArray ToArray(JNIEnv* env, const HC::Report* report) {
    jobjectArray out =
        env->NewObjectArray(report ? 4 : 0, env->FindClass("java/lang/String"), nullptr);
    if (report != nullptr) {
        const std::string fields[4] = {report->path.string(), report->name, report->summary,
                                       report->when};
        for (jsize i = 0; i < 4; ++i) {
            env->SetObjectArrayElement(out, i, Common::Android::ToJString(env, fields[i]));
        }
    }
    return out;
}

HC::Report FromPath(const std::string& path) {
    const std::filesystem::path file{path};
    return HC::Report{.path = file, .name = file.filename().string(), .summary = {}, .when = {}};
}

} // namespace

extern "C" {

/// Before the console boots, so the JIT's fault handler chains to Harbor's.
JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborCrashReports_install(JNIEnv*, jobject) {
    HC::Install();
}

/// 0 not asked yet, 1 kept, 2 not kept.
JNIEXPORT jint JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborCrashReports_consent(JNIEnv*, jobject) {
    switch (HC::GetConsent()) {
    case HC::Consent::Accepted:
        return 1;
    case HC::Consent::Declined:
        return 2;
    case HC::Consent::Unasked:
        break;
    }
    return 0;
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborCrashReports_setConsent(JNIEnv*, jobject,
                                                                                    jboolean accepted) {
    HC::SetConsent(accepted);
}

/// The newest report not offered yet: path, name, summary, when; empty when there is none.
JNIEXPORT jobjectArray JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborCrashReports_takeOffer(JNIEnv* env,
                                                                                           jobject) {
    const auto report = HC::TakeOffer();
    return ToArray(env, report ? &*report : nullptr);
}

/// The newest kept report, offered or not; empty when there is none.
JNIEXPORT jobjectArray JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborCrashReports_latest(JNIEnv* env,
                                                                                        jobject) {
    const auto reports = HC::List();
    return ToArray(env, reports.empty() ? nullptr : &reports.front());
}

JNIEXPORT jstring JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborCrashReports_read(JNIEnv* env, jobject,
                                                                                 jstring path) {
    return Common::Android::ToJString(env, HC::Read(FromPath(Common::Android::GetJString(env, path))));
}

JNIEXPORT void JNICALL Java_org_yuzu_yuzu_1emu_harbor_HarborCrashReports_delete(JNIEnv* env, jobject,
                                                                                jstring path) {
    HC::Delete(FromPath(Common::Android::GetJString(env, path)));
}

} // extern "C"
