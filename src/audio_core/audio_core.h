// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <set>

#include "audio_core/adsp/adsp.h"
#include "audio_core/audio_manager.h"
#include "audio_core/sink/sink.h"

namespace Core {
class System;
}

namespace AudioCore {

/// @brief Main audio class, stored inside the core, and holding the audio manager, all sinks, and the ADSP.
class AudioCore {
public:
    explicit AudioCore(Core::System& system);
    ~AudioCore();

    /**
     * Shutdown the audio core.
     */
    void Shutdown();

    /**
     * Get a reference to the audio manager.
     *
     * @return Ref to the audio manager.
     */
    AudioManager& GetAudioManager();

    /**
     * Get the audio output sink currently in use.
     *
     * @return Ref to the sink.
     */
    Sink::Sink& GetOutputSink();

    /**
     * Get the audio input sink currently in use.
     *
     * @return Ref to the sink.
     */
    Sink::Sink& GetInputSink();

    /// @brief Get the ADSP.
    /// @return Ref to the ADSP.
    ADSP::ADSP& ADSP();

    /// @brief Suspend or resume every audio renderer opened with an applet resource user id (am
    /// suspends the process of a title behind the HOME menu). A suspended renderer sends no
    /// commands to the ADSP, so it stops mixing and producing output, and it resumes cleanly on
    /// the next update. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
    /// @param aruid - The applet resource user id the renderers were opened with.
    /// @param suspended - True to suspend, false to resume.
    void SetSuspended(u64 aruid, bool suspended);

    /// @brief Whether the renderers of an applet resource user id are suspended.
    [[nodiscard]] bool IsSuspended(u64 aruid) const;

private:
    /// @brief Create the sinks on startup.
    void CreateSinks();

    /// Main audio manager for audio in/out
    std::optional<AudioManager> audio_manager;
    /// Sink used for audio renderer and audio out
    std::unique_ptr<Sink::Sink> output_sink;
    /// Sink used for audio input
    std::unique_ptr<Sink::Sink> input_sink;
    /// The ADSP in the sysmodule
    std::optional<ADSP::ADSP> adsp;
    /// Applet resource user ids whose renderers are suspended
    mutable std::mutex suspended_mutex;
    std::set<u64> suspended_aruids;
};

} // namespace AudioCore
