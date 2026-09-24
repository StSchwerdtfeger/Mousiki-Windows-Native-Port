#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>
#include "loudness_meter.h"

namespace muisc {

// A growing, single-producer/single-consumer PCM buffer: one decode
// thread appends to it while the audio callback (and, once decode
// finishes, the waveform pass) read whatever's been decoded so far.
//
// Capacity is reserved up front from ffprobe's duration estimate, and
// append() DELIBERATELY REFUSES to grow past that reserved capacity
// (it silently clamps instead of reallocating). That's not laziness —
// it's what makes the read side safe without a lock: once `available`
// is published (release-store), every element up to that count is
// guaranteed already written AND the vector's backing pointer is
// guaranteed to never have moved, because it never reallocates. A
// reader that does `size_t n = available.load(acquire); read data[0..n)`
// is race-free by construction. The tradeoff is a duration estimate
// that's wildly wrong (rare — ffprobe reads the container header, it's
// usually right) truncates the last bit of a track rather than risking
// a use-after-free on a mid-playback reallocation touched by another
// thread. That trade is worth it here.
struct StreamingPcm {
    // Interleaved samples: frame f, channel c lives at data[f * channels + c].
    // `channels` is 1 (mono) or 2 (stereo) and must be set (via
    // reserve_for_seconds) before decoding starts; it never changes afterwards.
    // Everything else that talks about positions -- `available`, the
    // player's cursor, seeking -- counts FRAMES, so a stereo track and a mono
    // track of the same length have the same numbers.
    std::vector<float> data;
    std::atomic<size_t> available{0};   // FRAMES safe to read right now
    std::atomic<bool> decode_done{false};
    std::atomic<bool> decode_failed{false};
    std::atomic<bool> capacity_exceeded{false}; // diagnostic only
    int sample_rate = 44100;
    int channels = 1;

    // Integrated loudness (LUFS) of the track, measured while it decodes --
    // see loudness_meter.h. NaN until enough audio has been analysed
    // (kMinAnalysisSeconds), then refreshed as decoding continues and
    // frozen by finalize_loudness(). Decode decides nothing about playback
    // gain itself; Player reads this and derives the normalisation gain.
    //
    //   loudness_lufs       -- the track as it is stored (stereo if channels==2)
    //   loudness_mono_lufs  -- the same track folded down to mono, which is
    //                          what plays when "stereo" is switched off while
    //                          a stereo track is loaded. (Identical to
    //                          loudness_lufs for a mono buffer.)
    std::atomic<float> loudness_lufs{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<float> loudness_mono_lufs{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<bool> loudness_final{false};   // decode ended -- lufs won't change any more
    static constexpr double kMinAnalysisSeconds = 15.0;
    LoudnessMeter meter;                       // decode thread only
    LoudnessMeter meter_mono;                  // decode thread only; fed only when channels == 2

    void reserve_for_seconds(double seconds, int sr, int ch = 1) {
        sample_rate = sr;
        channels = ch >= 2 ? 2 : 1;
        meter.reset(sr, channels);
        meter_mono.reset(sr, 1);
        size_t est = static_cast<size_t>(std::max(1.0, seconds) * sr * 1.25); // 25% headroom, in frames
        data.reserve(std::max<size_t>(est, static_cast<size_t>(sr) * 5) * static_cast<size_t>(channels));
    }

    // Capacity in frames (what seeking is clamped against).
    size_t capacity_frames() const { return data.capacity() / static_cast<size_t>(channels); }

    // Decode thread only. `frames` interleaved frames.
    void append(const float* samples, size_t frames) {
        const size_t ch = static_cast<size_t>(channels);
        const size_t room = (data.capacity() - data.size()) / ch;
        const size_t n = std::min(frames, room);
        if (n > 0) {
            const size_t before = data.size();
            data.insert(data.end(), samples, samples + n * ch);
            available.store(data.size() / ch, std::memory_order_release);
            // Measured from the stored copy; capacity is fixed so the
            // pointer is stable (see the class comment).
            meter.push(data.data() + before, n);
            if (ch == 2) {
                float mix[1024];
                for (size_t done = 0; done < n;) {
                    const size_t m = std::min<size_t>(1024, n - done);
                    const float* src = data.data() + before + done * 2;
                    for (size_t i = 0; i < m; ++i) mix[i] = 0.5f * (src[2 * i] + src[2 * i + 1]);
                    meter_mono.push(mix, m);
                    done += m;
                }
            }
            if (meter.take_updated() && meter.seconds_analyzed() >= kMinAnalysisSeconds) publish_loudness();
        }
        if (n < frames) capacity_exceeded.store(true, std::memory_order_relaxed);
    }

    // Decode thread, once decoding has ended (success or failure): publish
    // the final figure -- including for tracks shorter than
    // kMinAnalysisSeconds, which never got an early estimate -- and tell
    // anyone waiting on it (Player::play) there is nothing more to wait for.
    void finalize_loudness() {
        publish_loudness();
        loudness_final.store(true, std::memory_order_release);
    }

private:
    void publish_loudness() {
        const double l = meter.integrated_lufs();
        if (std::isnan(l)) return;
        loudness_lufs.store(static_cast<float>(l), std::memory_order_relaxed);
        const double lm = (channels == 2) ? meter_mono.integrated_lufs() : l;
        loudness_mono_lufs.store(static_cast<float>(std::isnan(lm) ? l : lm), std::memory_order_relaxed);
    }
};

} // namespace muisc
