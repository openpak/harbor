// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cmath>
#include <numbers>

#include <SDL3/SDL.h>

#include "common/logging.h"
#include "harbor_setup/chime.h"

namespace HarborSetup {

struct Chime::Impl {
    SDL_AudioStream* stream = nullptr;
    bool audio_started = false;
};

std::vector<std::int16_t> Chime::Samples(int note) {
    std::vector<std::int16_t> samples(SampleRate * DurationMs / 1000);
    const double frequency = 196.0 * std::pow(2.0, std::clamp(note, 0, 10) / 12.0);
    const double count = static_cast<double>(samples.size());
    for (size_t i = 0; i < samples.size(); i++) {
        const double time = static_cast<double>(i) / SampleRate;
        const double attack = std::min(time / 0.018, 1.0);
        const double release = std::min((count - 1 - static_cast<double>(i)) / (SampleRate * 0.075), 1.0);
        const double envelope = attack * attack * release * release;
        const double phase = 2 * std::numbers::pi * frequency * time;
        const double horn = 0.62 * std::sin(phase) + 0.24 * std::sin(2 * phase) + 0.10 * std::sin(3 * phase);
        samples[i] = static_cast<std::int16_t>(32767.0 * 0.24 * envelope * horn);
    }
    return samples;
}

Chime::Chime() : impl{new Impl} {
#ifdef __ANDROID__
    // The driver Eden's own audio uses on Android, so the setup exercises the same path.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "openslES");
#endif
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_WARNING(Frontend, "harbor: setup sound unavailable: {}", SDL_GetError());
        return;
    }
    impl->audio_started = true;
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, SampleRate};
    impl->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!impl->stream) {
        LOG_WARNING(Frontend, "harbor: setup sound unavailable: {}", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(impl->stream);
    LOG_INFO(Frontend, "harbor: setup sound on SDL audio driver {}", SDL_GetCurrentAudioDriver());
}

Chime::~Chime() {
    if (impl->stream) {
        SDL_DestroyAudioStream(impl->stream);
    }
    if (impl->audio_started) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
#ifdef __ANDROID__
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
#endif
    delete impl;
}

void Chime::Play(int note) {
    if (!impl->stream) {
        return;
    }
    const auto samples = Samples(note);
    SDL_ClearAudioStream(impl->stream);
    SDL_PutAudioStreamData(impl->stream, samples.data(),
                           static_cast<int>(samples.size() * sizeof(std::int16_t)));
}

} // namespace HarborSetup
