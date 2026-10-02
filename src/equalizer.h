#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

namespace muisc {

// 10-band graphic equaliser (Mode::Equalizer, HKeyEqualizer = Shift+E).
//
// Ten peaking filters (RBJ "Audio EQ Cookbook" biquads) at the ISO octave
// centres, one octave wide each, so neighbouring bands overlap into a smooth
// curve instead of a staircase. Header-only on purpose: Player owns one
// instance and runs it inside the audio callback, App only needs the preset
// table and the band layout.
//
// Threading: the DSP object below is touched by the audio thread ONLY.
// Player hands it new gains through atomics and calls configure() from the
// callback when it notices a change (see Player::data_callback), so nothing
// here locks or allocates.

constexpr int kEqBands = 10;
constexpr float kEqMinDb = -12.0f;
constexpr float kEqMaxDb = 12.0f;

constexpr std::array<float, kEqBands> kEqFreqsHz = {
    31.25f, 62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};

// Labels drawn under the sliders (4 columns wide at most).
constexpr std::array<const char*, kEqBands> kEqLabels = {
    "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};

using EqGains = std::array<float, kEqBands>;

struct EqPreset {
    const char* name;
    EqGains gains; // dB per band
};

// The first entry is "Flat"; the preset cycle in the overlay walks this
// table in order. Values stay inside +/-12 dB (kEqMinDb/kEqMaxDb).
constexpr std::array<EqPreset, 12> kEqPresets = {{
    {"Flat",         {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0}},
    {"Bass Boost",   {  6,  5,  4,  2,  1,  0,  0,  0,  0,  0}},
    {"Treble Boost", {  0,  0,  0,  0,  0,  1,  2,  4,  5,  6}},
    {"Vocal",        { -2, -3, -3,  1,  3,  4,  3,  2,  0, -1}},
    {"Rock",         {  4,  3,  2, -1, -2, -1,  1,  3,  4,  4}},
    {"Pop",          { -1,  1,  3,  4,  3,  0, -1, -1, -1, -1}},
    {"Jazz",         {  3,  2,  1,  2, -2, -2,  0,  1,  2,  3}},
    {"Classical",    {  4,  3,  3,  2, -1, -1,  0,  2,  3,  3}},
    {"Electronic",   {  5,  4,  1,  0, -2,  2,  1,  1,  4,  5}},
    {"Hip-Hop",      {  5,  4,  2,  3, -1, -1,  2,  0,  2,  3}},
    {"Acoustic",     {  4,  4,  3,  1,  2,  2,  3,  3,  3,  2}},
    {"Loudness",     {  6,  4,  1,  0, -1, -1,  0,  1,  4,  5}},
}};

// Index of the preset whose gains match exactly, or -1 ("Custom").
inline int eq_match_preset(const EqGains& g) {
    for (size_t p = 0; p < kEqPresets.size(); ++p) {
        bool same = true;
        for (int b = 0; b < kEqBands; ++b) {
            if (std::fabs(kEqPresets[p].gains[b] - g[b]) > 0.01f) { same = false; break; }
        }
        if (same) return static_cast<int>(p);
    }
    return -1;
}

// Normalised biquad coefficients {b0,b1,b2,a1,a2} of one peaking band.
inline void eq_band_coeffs(double db, double f0, double sr, double out[5]) {
    constexpr double kQ = 1.4142; // about one octave
    const double A = std::pow(10.0, db / 40.0);
    const double w0 = 2.0 * 3.14159265358979323846 * f0 / sr;
    const double alpha = std::sin(w0) / (2.0 * kQ);
    const double cw = std::cos(w0);
    const double a0 = 1.0 + alpha / A;
    out[0] = (1.0 + alpha * A) / a0;
    out[1] = (-2.0 * cw) / a0;
    out[2] = (1.0 - alpha * A) / a0;
    out[3] = (-2.0 * cw) / a0;
    out[4] = (1.0 - alpha / A) / a0;
}

// Can band `b` run at all at this sample rate? (Zero-gain bands are identity
// filters; bands too close to Nyquist would misbehave.)
inline bool eq_band_usable(int b, float db, int sample_rate) {
    return std::fabs(db) >= 0.01f && kEqFreqsHz[b] < 0.45f * static_cast<float>(std::max(1, sample_rate));
}

// Highest point of the COMBINED response in dB (>= 0 when anything is boosted).
// Neighbouring bands overlap, so the true peak can sit above the largest single
// gain -- e.g. +6/+5 side by side peak near +7 -- which is why the preamp is
// derived from the actual curve and not from the biggest slider.
inline float eq_peak_gain_db(const EqGains& g, int sample_rate = 44100) {
    const double sr = std::max(1, sample_rate);
    double c[kEqBands][5];
    int n = 0;
    for (int b = 0; b < kEqBands; ++b) {
        if (!eq_band_usable(b, g[b], sample_rate)) continue;
        eq_band_coeffs(std::clamp(g[b], kEqMinDb, kEqMaxDb), kEqFreqsHz[b], sr, c[n++]);
    }
    if (n == 0) return 0.0f;
    double peak = -1e9;
    // 96 log-spaced points, 20 Hz .. just under Nyquist, plus the band centres.
    for (int k = 0; k < 96 + kEqBands; ++k) {
        const double f = k < 96 ? 20.0 * std::pow(std::min(0.45 * sr, 20000.0) / 20.0, k / 95.0)
                                : kEqFreqsHz[k - 96];
        if (f >= 0.5 * sr) continue;
        const double w = 2.0 * 3.14159265358979323846 * f / sr;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
        double mag_db = 0.0;
        for (int i = 0; i < n; ++i) {
            const std::complex<double> num = c[i][0] + c[i][1] * z1 + c[i][2] * z2;
            const std::complex<double> den = 1.0 + c[i][3] * z1 + c[i][4] * z2;
            mag_db += 20.0 * std::log10(std::abs(num / den));
        }
        peak = std::max(peak, mag_db);
    }
    return static_cast<float>(std::max(0.0, peak));
}

// Preamp (dB, <= 0) that brings the loudest point of the curve back to 0 dB,
// so enabling a boost does not push the signal over full scale. Applied
// automatically whenever the equaliser is active.
inline float eq_auto_preamp_db(const EqGains& g, int sample_rate = 44100) {
    return -eq_peak_gain_db(g, sample_rate);
}

class Equalizer {
public:
    // Recomputes every coefficient for `gains` at `sample_rate`. A band at
    // 0 dB is an identity filter and is skipped entirely, as is any band
    // too close to Nyquist for the biquad to behave (e.g. 16 kHz on a
    // 22.05 kHz track).
    void configure(const EqGains& gains, int sample_rate) {
        count_ = 0;
        const double sr = std::max(1, sample_rate);
        for (int b = 0; b < kEqBands; ++b) {
            if (!eq_band_usable(b, gains[b], sample_rate)) continue;
            const double db = std::clamp(gains[b], kEqMinDb, kEqMaxDb);
            double c[5];
            eq_band_coeffs(db, kEqFreqsHz[b], sr, c);
            Band& bd = bands_[count_];
            bd.b0 = c[0]; bd.b1 = c[1]; bd.b2 = c[2]; bd.a1 = c[3]; bd.a2 = c[4];
            // Keep this slot's state if it already had one (a gain tweak
            // during playback must not click); fresh slots start at zero.
            if (static_cast<int>(slot_[count_]) != b) {
                bd.z1[0] = bd.z1[1] = bd.z2[0] = bd.z2[1] = 0.0;
                slot_[count_] = static_cast<signed char>(b);
            }
            ++count_;
        }
        for (int i = count_; i < kEqBands; ++i) slot_[i] = -1; // freed slots start clean when reused
        preamp_ = static_cast<float>(std::pow(10.0, eq_auto_preamp_db(gains, sample_rate) / 20.0));
        if (count_ == 0) preamp_ = 1.0f;
    }

    // Wipes the filter memory (new track / device restart).
    void reset() {
        for (auto& b : bands_) b.z1[0] = b.z1[1] = b.z2[0] = b.z2[1] = 0.0;
        for (auto& s : slot_) s = -1;
        count_ = 0;
    }

    bool active() const { return count_ > 0; }

    // One stereo frame, in place.
    inline void process(float& l, float& r) {
        double x0 = l, x1 = r;
        for (int i = 0; i < count_; ++i) {
            Band& b = bands_[i];
            double y0 = b.b0 * x0 + b.z1[0];
            b.z1[0] = b.b1 * x0 - b.a1 * y0 + b.z2[0];
            b.z2[0] = b.b2 * x0 - b.a2 * y0;
            double y1 = b.b0 * x1 + b.z1[1];
            b.z1[1] = b.b1 * x1 - b.a1 * y1 + b.z2[1];
            b.z2[1] = b.b2 * x1 - b.a2 * y1;
            x0 = y0;
            x1 = y1;
        }
        l = static_cast<float>(x0) * preamp_;
        r = static_cast<float>(x1) * preamp_;
    }

private:
    struct Band {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] = {0, 0};
        double z2[2] = {0, 0};
    };
    std::array<Band, kEqBands> bands_{};
    std::array<signed char, kEqBands> slot_ = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int count_ = 0;
    float preamp_ = 1.0f;
};

} // namespace muisc
