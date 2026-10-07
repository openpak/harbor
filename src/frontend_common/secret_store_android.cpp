// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Android: the Android Keystore, which only Java reaches. HarborSecrets.kt seals each secret with
// AES-256-GCM under a non-exportable Keystore key and keeps the ciphertext in the app's no-backup
// directory; this is its native face.

#include <jni.h>

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/logging.h"
#include "core/openpak/secret_store.h"
#include "frontend_common/secret_store.h"

namespace FrontendCommon {

namespace {

constexpr const char* SecretsClass = "org/yuzu/yuzu_emu/harbor/HarborSecrets";

bool ClearException(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

class KeystoreSecretStore final : public OpenPak::SecretStore {
public:
    KeystoreSecretStore(jclass secrets_, jmethodID load_, jmethodID store_, jmethodID erase_)
        : secrets{secrets_}, load{load_}, store{store_}, erase{erase_} {}

    // OpenPak calls these from its own native threads, which stay attached: every local
    // reference is deleted here rather than left for a frame that never pops.
    std::optional<std::string> Load(std::string_view key) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        jstring jkey = Common::Android::ToJString(env, key);
        auto value = static_cast<jstring>(env->CallStaticObjectMethod(secrets, load, jkey));
        const bool failed = ClearException(env);
        env->DeleteLocalRef(jkey);
        if (failed || !value) {
            return std::nullopt;
        }
        std::string out = Common::Android::GetJString(env, value);
        env->DeleteLocalRef(value);
        return out;
    }

    bool Store(std::string_view key, std::string_view secret) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        jstring jkey = Common::Android::ToJString(env, key);
        jstring jsecret = Common::Android::ToJString(env, secret);
        const bool ok = env->CallStaticBooleanMethod(secrets, store, jkey, jsecret) == JNI_TRUE;
        const bool failed = ClearException(env);
        env->DeleteLocalRef(jsecret);
        env->DeleteLocalRef(jkey);
        return ok && !failed;
    }

    void Erase(std::string_view key) override {
        JNIEnv* env = Common::Android::GetEnvForThread();
        jstring jkey = Common::Android::ToJString(env, key);
        env->CallStaticVoidMethod(secrets, erase, jkey);
        ClearException(env);
        env->DeleteLocalRef(jkey);
    }

private:
    jclass secrets;
    jmethodID load;
    jmethodID store;
    jmethodID erase;
};

} // namespace

std::shared_ptr<OpenPak::SecretStore> CreatePlatformSecretStore() {
    JNIEnv* env = Common::Android::GetEnvForThread();
    jclass local = env->FindClass(SecretsClass);
    if (ClearException(env) || !local) {
        LOG_WARNING(Frontend, "harbor: HarborSecrets not found (not called from a Java thread?)");
        return nullptr;
    }
    auto secrets = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    const jmethodID available = env->GetStaticMethodID(secrets, "available", "()Z");
    const jmethodID load =
        env->GetStaticMethodID(secrets, "load", "(Ljava/lang/String;)Ljava/lang/String;");
    const jmethodID store =
        env->GetStaticMethodID(secrets, "store", "(Ljava/lang/String;Ljava/lang/String;)Z");
    const jmethodID erase = env->GetStaticMethodID(secrets, "erase", "(Ljava/lang/String;)V");
    if (ClearException(env) || !available || !load || !store || !erase) {
        env->DeleteGlobalRef(secrets);
        return nullptr;
    }
    // Generates (or finds) the Keystore key; false when the Keystore will not give one.
    const bool ok = env->CallStaticBooleanMethod(secrets, available) == JNI_TRUE;
    if (ClearException(env) || !ok) {
        env->DeleteGlobalRef(secrets);
        return nullptr;
    }
    return std::make_shared<KeystoreSecretStore>(secrets, load, store, erase);
}

} // namespace FrontendCommon
