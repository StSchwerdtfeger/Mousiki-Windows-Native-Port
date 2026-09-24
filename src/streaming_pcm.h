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
    std::vector<float> data;
    std::atomic<size_t> available{0};   // frames safe to read right now
    std::atomic<bool> decode_done{false};
    std::atomic<bool> decode_failed{false};
    std::atomic<bool> capacity_exceeded{false}; // diagnostic only
    int sample_rate = 44100;

    // Integrated loudness (LUFS) of the track, measured while it decodes --
    // see loudness_meter.h. NaN until enough audio has been analysed
    // (kMinAnalysisSeconds), then refreshed as decoding continues and
    // frozen by finalize_loudness(). Decode decides nothing about playback
    // gain itself; Player reads this and derives the normalisation gain.
    std::atomic<float> loudness_lufs{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<bool> loudness_final{false};   // decode ended -- lufs won't change any more
    static constexpr double kMinAnalysisSeconds = 15.0;
    LoudnessMeter meter;                       // decode thread only

    void reserve_for_seconds(double seconds, int sr) {
        sample_rate = sr;
        meter.reset(sr);
        size_t est = static_cast<size_t>(std::max(1.0, seconds) * sr * 1.25); // 25% headroom
        data.reserve(std::max<size_t>(est, static_cast<size_t>(sr) * 5)); // at least 5s worth
    }

    // Decode thread only.
    void append(const float* samples, size_t count) {
        size_t room = data.capacity() - data.size();
        size_t n = std::min(count, room);
        if (n > 0) {
            const size_t before = data.size();
            data.insert(data.end(), samples, samples + n);
            available.store(data.size(), std::memory_order_release);
            // Measured from the stored copy; capacity is fixed so the
            // pointer is stable (see the class comment).
            meter.push(data.data() + before, n);
            if (meter.take_updated() && meter.seconds_analyzed() >= kMinAnalysisSeconds) {
                const double l = meter.integrated_lufs();
                if (!std::isnan(l)) loudness_lufs.store(static_cast<float>(l), std::memory_order_relaxed);
            }
        }
        if (n < count) capacity_exceeded.store(true, std::memory_order_relaxed);
    }

    // Decode thread, once decoding has ended (success or failure): publish
    // the final figure -- including for tracks shorter than
    // kMinAnalysisSeconds, which never got an early estimate -- and tell
    // anyone waiting on it (Player::play) there is nothing more to wait for.
    void finalize_loudness() {
        const double l = meter.integrated_lufs();
        if (!std::isnan(l)) loudness_lufs.store(static_cast<float>(l), std::memory_order_relaxed);
        loudness_final.store(true, std::memory_order_release);
    }
};

} // namespace muisc
