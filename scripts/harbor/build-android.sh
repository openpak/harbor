#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Build one Android flavour as a release APK: build-android.sh <legacy|mainline>
# The APK is signed with the committed debug keystore unless ANDROID_KEYSTORE_FILE,
# ANDROID_KEYSTORE_PASS and ANDROID_KEY_ALIAS are set. Test builds only.
set -euo pipefail
. "$(dirname "$0")/common.sh"

flavor="${1:-}"
case "$flavor" in
    legacy) task=assembleLegacyRelease ;;
    mainline) task=assembleMainlineRelease ;;
    *) echo "usage: $0 <legacy|mainline>" >&2; exit 2 ;;
esac

refuse_while_emulator_runs

export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
export PATH="$JAVA_HOME/bin:$PATH"
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export GRADLE_USER_HOME="$SCR/gradle-home"
export GRADLE_RO_DEP_CACHE=$HOME/.gradle/caches
JOBS="${NUM_JOBS:-5}"

# The Android Gradle plugin runs ninja without -j, which exhausts memory here, so the
# compile and link steps are limited with CMake job pools instead.

# Reuse the Gradle distribution already on this machine instead of downloading it again.
dist=gradle-8.13-bin
if [ ! -d "$GRADLE_USER_HOME/wrapper/dists/$dist" ] && [ -d "$HOME/.gradle/wrapper/dists/$dist" ]; then
    mkdir -p "$GRADLE_USER_HOME/wrapper/dists"
    cp -r "$HOME/.gradle/wrapper/dists/$dist" "$GRADLE_USER_HOME/wrapper/dists/"
fi

mkdir -p "$SCR/artifacts"
cd "$SRC/src/android"
locked ./gradlew "$task" \
    -Dorg.gradle.daemon=false \
    -Dorg.gradle.workers.max="$JOBS" \
    -PYUZU_ANDROID_ARGS="-DYUZU_DOWNLOAD_ANDROID_VVL=OFF -DCMAKE_JOB_POOLS=compile=$JOBS;link=2 -DCMAKE_JOB_POOL_COMPILE=compile -DCMAKE_JOB_POOL_LINK=link" \
    -Pnightly=false

apk="$SRC/src/android/app/build/outputs/apk/$flavor/release/app-$flavor-release.apk"
out="$SCR/artifacts/harbor-eden-$flavor-$SHORT_SHA.apk"
cp "$apk" "$out"

print_elapsed
ls -l "$out"
