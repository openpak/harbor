// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <vector>

namespace HarborSetup {

/// Harbor's original short horn cue: softly enveloped harmonics, one semitone higher per setup page.
/// Not a sampled console sound. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base),
/// SetupChime.cs.
class Chime {
public:
    static constexpr int SampleRate = 44100;
    static constexpr int DurationMs = 180;

    /// 16-bit mono PCM of the cue for a page note (0 = Welcome).
    static std::vector<std::int16_t> Samples(int note);

    Chime();
    ~Chime();

    Chime(const Chime&) = delete;
    Chime& operator=(const Chime&) = delete;

    /// Plays the note, replacing one still playing. Sound failing never stops setup.
    void Play(int note);

private:
    struct Impl;
    Impl* impl = nullptr;
};

} // namespace HarborSetup
