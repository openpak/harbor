// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"
#include "core/core.h"
#include "core/core_timing.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {

/**
 * A stream that plays nothing, but consumes its queued buffers at the stream's sample rate against
 * core time, as a real backend's callback would. Without that the played-sample counters never
 * move and AudioOut buffers are never released (the guest then waits on them for ever).
 */
class NullSinkStreamImpl final : public SinkStream {
public:
    explicit NullSinkStreamImpl(Core::System& system_, StreamType type_)
        : SinkStream{system_, type_} {}

    ~NullSinkStreamImpl() override {
        Finalize();
    }

    void Finalize() override {
        Stop();
    }

    void Start(bool resume = false) override {
        if (!paused) {
            return;
        }
        paused = false;
        last_time = system.CoreTiming().GetGlobalTimeNs();
        remainder = std::chrono::nanoseconds{0};
        thread = std::jthread([this](std::stop_token token) { Run(token); });
    }

    void Stop() override {
        if (paused) {
            return;
        }
        SignalPause();
        thread.request_stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Input streams have nothing to record; ProcessAudioIn consumes the buffers with silence.
    std::vector<s16> ReleaseBuffer(u64 num_samples) override {
        return std::vector<s16>(num_samples, 0);
    }

private:
    void Run(std::stop_token token) {
        // The same size as a real backend's callback (TargetSampleCount frames, 5 ms).
        constexpr auto period = std::chrono::milliseconds{5};
        constexpr std::size_t max_frames = TargetSampleRate / 10;
        std::vector<s16> scratch;

        while (!token.stop_requested()) {
            std::this_thread::sleep_for(period);
            if (token.stop_requested()) {
                break;
            }

            const auto now = system.CoreTiming().GetGlobalTimeNs();
            const auto elapsed = now - last_time + remainder;
            if (elapsed <= std::chrono::nanoseconds{0}) {
                last_time = now;
                continue;
            }
            auto frames = static_cast<std::size_t>((elapsed * TargetSampleRate) /
                                                   std::chrono::seconds{1});
            if (frames == 0) {
                continue;
            }
            if (frames > max_frames) {
                // Core time ran far ahead (a long pause or fast-forward): count one slice only.
                frames = max_frames;
                remainder = std::chrono::nanoseconds{0};
            } else {
                remainder = elapsed - std::chrono::nanoseconds{
                                          std::chrono::seconds{1} * static_cast<s64>(frames) /
                                          TargetSampleRate};
            }
            last_time = now;

            scratch.assign(frames * GetDeviceChannels(), 0);
            if (type == StreamType::In) {
                ProcessAudioIn(scratch, frames);
            } else {
                ProcessAudioOutAndRender(scratch, frames);
            }
        }
    }

    std::jthread thread;
    std::chrono::nanoseconds last_time{};
    std::chrono::nanoseconds remainder{};
};

/**
 * A sink for when no audio out is wanted. Each session gets its own stream.
 */
class NullSink final : public Sink {
public:
    explicit NullSink(std::string_view) {}
    ~NullSink() override = default;

    SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels, const std::string&,
                                  StreamType type) override {
        SinkStreamPtr& stream =
            streams.emplace_back(std::make_unique<NullSinkStreamImpl>(system, type));
        stream->SetSystemChannels(system_channels);
        stream->SetDeviceVolume(device_volume);
        return stream.get();
    }

    void CloseStream(SinkStream* stream) override {
        std::erase_if(streams, [stream](const SinkStreamPtr& s) { return s.get() == stream; });
    }
    void CloseStreams() override {
        streams.clear();
    }
    f32 GetDeviceVolume() const override {
        return device_volume;
    }
    void SetDeviceVolume(f32 volume) override {
        device_volume = volume;
        for (auto& stream : streams) {
            stream->SetDeviceVolume(volume);
        }
    }
    void SetSystemVolume(f32 volume) override {
        for (auto& stream : streams) {
            stream->SetSystemVolume(volume);
        }
    }

private:
    std::vector<SinkStreamPtr> streams;
};

} // namespace AudioCore::Sink
