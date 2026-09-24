#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace muisc {

// Streaming "integrated loudness" meter following ITU-R BS.1770-4 / EBU R128
// (the same measurement behind LUFS, ReplayGain 2.0 and the loudness
// normalisation of YouTube/Spotify):
//
//   1. K-weighting: a high-shelf (models the head) followed by a 38 Hz
//      high-pass, so the number tracks *perceived* loudness rather than
//      raw sample level.
//   2. Mean-square over 400 ms blocks that overlap by 75 % (a new block
//      every 100 ms).
//   3. Gating: blocks quieter than -70 LUFS are ignored (silence), then
//      blocks more than 10 LU below the average of what's left are
//      ignored too (quiet passages/intros shouldn't drag the result down).
//
// Feed it mono samples with push(); read integrated_lufs() at any time --
// it is the value for everything pushed so far, so it converges as more of
// a track is decoded. Not thread-safe: use it from the decode thread only
// and publish the result through an atomic (see StreamingPcm).
//
// Mousiki plays a mono buffer that the audio device then duplicates into
// both stereo channels (miniaudio copies mono -> each output channel), which
// is 2x the energy of one channel, i.e. +3.01 LU. kChannelEnergy accounts
// for that so the reported LUFS is what the speakers actually emit and
// lines up with the usual -14/-16/-23 LUFS reference levels.
class LoudnessMeter {
public:
    static constexpr double kChannelEnergy = 2.0;

    explicit LoudnessMeter(int sample_rate = 44100) { reset(sample_rate); }

    void reset(int sample_rate) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 44100;
        sub_len_ = std::max(1, static_cast<int>(std::lround(sample_rate_ * 0.1)));
        design_filters();
        s1_ = {};
        s2_ = {};
        sub_sum_ = 0.0;
        sub_fill_ = 0;
        ring_.fill(0.0);
        ring_pos_ = 0;
        subblocks_ = 0;
        hist_count_.fill(0);
        hist_energy_.fill(0.0);
        total_samples_ = 0;
        updated_ = false;
    }

    void push(const float* samples, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(samples[i]);
            // Stage 1: high-shelf. Direct form II transposed.
            double y = b1_[0] * x + s1_[0];
            s1_[0] = b1_[1] * x - a1_[1] * y + s1_[1];
            s1_[1] = b1_[2] * x - a1_[2] * y;
            // Stage 2: RLB high-pass.
            double z = b2_[0] * y + s2_[0];
            s2_[0] = b2_[1] * y - a2_[1] * z + s2_[1];
            s2_[1] = b2_[2] * y - a2_[2] * z;

            sub_sum_ += z * z;
            if (++sub_fill_ == sub_len_) finish_subblock();
        }
        total_samples_ += n;
    }

    // Seconds of audio measured so far.
    double seconds_analyzed() const {
        return static_cast<double>(total_samples_) / sample_rate_;
    }

    // True (once) if a new 100 ms step completed since the last call -- lets
    // the caller skip recomputing integrated_lufs() when nothing changed.
    bool take_updated() {
        const bool u = updated_;
        updated_ = false;
        return u;
    }

    // Gated integrated loudness in LUFS of everything pushed so far, or NaN
    // if there is no block above the absolute gate yet (silence / < 400 ms).
    double integrated_lufs() const {
        double total_e = 0.0;
        uint64_t n = 0;
        for (int i = 0; i < kBins; ++i) { total_e += hist_energy_[i]; n += hist_count_[i]; }
        if (n == 0) return std::numeric_limits<double>::quiet_NaN();

        const double rel_gate = to_lufs(total_e / static_cast<double>(n)) - 10.0;
        int start = static_cast<int>(std::ceil((rel_gate - kMinLufs) / kBinWidth));
        start = std::clamp(start, 0, kBins - 1);

        double e2 = 0.0;
        uint64_t n2 = 0;
        for (int i = start; i < kBins; ++i) { e2 += hist_energy_[i]; n2 += hist_count_[i]; }
        if (n2 == 0) return std::numeric_limits<double>::quiet_NaN();
        return to_lufs(e2 / static_cast<double>(n2));
    }

private:
    static constexpr double kMinLufs = -70.0;   // absolute gate
    static constexpr double kBinWidth = 0.1;    // LU per histogram bin
    static constexpr int    kBins = 1000;       // -70 ... +30 LUFS

    static double to_lufs(double energy) { return -0.691 + 10.0 * std::log10(energy); }

    // BS.1770 K-weighting, designed for any sample rate with the bilinear
    // transform (the standard's tabulated coefficients are only for 48 kHz).
    void design_filters() {
        const double pi = 3.14159265358979323846;
        {   // Stage 1 -- high shelf
            const double f0 = 1681.974450955533;
            const double G  = 3.999843853973347;
            const double Q  = 0.7071752369554196;
            const double K  = std::tan(pi * f0 / sample_rate_);
            const double Vh = std::pow(10.0, G / 20.0);
            const double Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            b1_ = { (Vh + Vb * K / Q + K * K) / a0,
                    2.0 * (K * K - Vh) / a0,
                    (Vh - Vb * K / Q + K * K) / a0 };
            a1_ = { 1.0,
                    2.0 * (K * K - 1.0) / a0,
                    (1.0 - K / Q + K * K) / a0 };
        }
        {   // Stage 2 -- RLB high-pass
            const double f0 = 38.13547087602444;
            const double Q  = 0.5003270373238773;
            const double K  = std::tan(pi * f0 / sample_rate_);
            const double a0 = 1.0 + K / Q + K * K;
            b2_ = { 1.0, -2.0, 1.0 };
            a2_ = { 1.0,
                    2.0 * (K * K - 1.0) / a0,
                    (1.0 - K / Q + K * K) / a0 };
        }
    }

    void finish_subblock() {
        ring_[ring_pos_] = sub_sum_;
        ring_pos_ = (ring_pos_ + 1) % 4;
        sub_sum_ = 0.0;
        sub_fill_ = 0;
        ++subblocks_;
        updated_ = true;
        if (subblocks_ < 4) return; // need a full 400 ms window

        const double mean_sq = (ring_[0] + ring_[1] + ring_[2] + ring_[3]) / (4.0 * sub_len_);
        const double energy = mean_sq * kChannelEnergy;
        if (energy <= 0.0) return;
        const double lufs = to_lufs(energy);
        if (lufs < kMinLufs) return; // absolute gate
        int bin = static_cast<int>((lufs - kMinLufs) / kBinWidth);
        bin = std::clamp(bin, 0, kBins - 1);
        ++hist_count_[bin];
        hist_energy_[bin] += energy;
    }

    int sample_rate_ = 44100;
    int sub_len_ = 4410;
    std::array<double, 3> b1_{}, a1_{}, b2_{}, a2_{};
    std::array<double, 2> s1_{}, s2_{};
    double sub_sum_ = 0.0;
    int sub_fill_ = 0;
    std::array<double, 4> ring_{};
    int ring_pos_ = 0;
    uint64_t subblocks_ = 0;
    std::array<uint64_t, kBins> hist_count_{};
    std::array<double, kBins> hist_energy_{};
    uint64_t total_samples_ = 0;
    bool updated_ = false;
};

} // namespace muisc
