#include "oscilloscope_visualizer.h"
#include <algorithm>
#include <cmath>

namespace muisc {
namespace {

// kBrailleMap[y][x] = bit of that subpixel inside a 2x4 braille cell.
const uint8_t kBrailleMap[4][2] = {
    {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}
};

// Mono detection: energy ratio (L-R)^2 / (L+R)^2 over the drawn window.
// Below kMonoFull the scope is fully in phase-portrait mode, above
// kMonoNone it is a true XY scope, in between the two are cross-faded.
constexpr float kMonoFull = 0.003f; // about -25 dB of side energy
constexpr float kMonoNone = 0.020f; // about -17 dB
constexpr int kDeriv = 3;           // half-width of the central difference for ds/dt

} // namespace

void OscilloscopeVisualizer::push_frames(const float* interleaved_lr, size_t frames) {
    if (!interleaved_lr || frames == 0) return;
    if (frames > static_cast<size_t>(kRingFrames)) {
        interleaved_lr += 2 * (frames - static_cast<size_t>(kRingFrames));
        frames = static_cast<size_t>(kRingFrames);
    }
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < frames; ++i) {
        ch0_[write_] = interleaved_lr[2 * i];
        ch1_[write_] = interleaved_lr[2 * i + 1];
        write_ = (write_ + 1) % kRingFrames;
    }
}

void OscilloscopeVisualizer::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    ch0_.fill(0.0f);
    ch1_.fill(0.0f);
    write_ = 0;
    peak_ = 0.05f;
    cleared_ = true; // glow_ belongs to the render thread; it wipes it on its next frame
}

std::vector<std::vector<OscilloscopeVisualizer::Cell>>
OscilloscopeVisualizer::render(int cols, int rows, const Params& params) {
    const float decay = std::clamp(params.decay, 0.0f, 0.99f);
    const float dot_threshold = std::clamp(params.dot_threshold, 0.01f, 1.0f);
    const float tail = std::clamp(params.tail_brightness, 0.0f, 1.0f);
    std::vector<std::vector<Cell>> out(std::max(0, rows));
    for (auto& r : out) r.assign(std::max(0, cols), Cell{});
    if (cols <= 0 || rows <= 0) return out;

    const int pw = cols * 2, ph = rows * 4;

    // --- snapshot the newest kWindow frames, oldest -> newest ----------
    std::vector<float> xs(kWindow), ys(kWindow);
    float peak;
    bool wipe;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int i = 0; i < kWindow; ++i) {
            const int idx = (write_ - kWindow + i + kRingFrames) % kRingFrames;
            xs[i] = ch0_[idx];
            ys[i] = ch1_[idx];
        }
        peak = peak_;
        wipe = cleared_;
        cleared_ = false;
    }

    // --- phosphor buffer: resize / wipe / decay --------------------------
    if (wipe) { mono_ = 0.0f; scale_ = 0.0f; }
    if (glow_w_ != pw || glow_h_ != ph || wipe) {
        glow_.assign(static_cast<size_t>(pw) * ph, 0.0f);
        glow_w_ = pw;
        glow_h_ = ph;
    } else {
        for (float& v : glow_) v *= decay;
    }

    // --- mono detection + phase-portrait cross-fade ---------------------
    {
        double em = 0.0, es = 0.0;
        for (int i = 0; i < kWindow; ++i) {
            const double m = xs[i] + ys[i], s = xs[i] - ys[i];
            em += m * m;
            es += s * s;
        }
        const float ratio = static_cast<float>(es / (em + 1e-9));
        const float target = std::clamp((kMonoNone - ratio) / (kMonoNone - kMonoFull), 0.0f, 1.0f);
        mono_ += (target - mono_) * 0.25f;

        if (mono_ > 0.001f) {
            std::vector<float> mid(kWindow), q(kWindow);
            for (int i = 0; i < kWindow; ++i) mid[i] = 0.5f * (xs[i] + ys[i]);
            double rs = 0.0, rq = 0.0;
            for (int i = 0; i < kWindow; ++i) {
                const int a = std::max(i - kDeriv, 0), b = std::min(i + kDeriv, kWindow - 1);
                q[i] = mid[b] - mid[a];
                rs += static_cast<double>(mid[i]) * mid[i];
                rq += static_cast<double>(q[i]) * q[i];
            }
            // Scale ds/dt so its rms equals the signal's rms: a pure tone
            // then draws a circle whatever its pitch.
            const float want = static_cast<float>(std::sqrt(rs / kWindow) / std::max(std::sqrt(rq / kWindow), 1e-6));
            const float clamped = std::min(want, 400.0f);
            scale_ = scale_ <= 0.0f ? clamped : scale_ + (clamped - scale_) * 0.2f;
            for (int i = 0; i < kWindow; ++i) {
                xs[i] = xs[i] + (mid[i] - xs[i]) * mono_;
                ys[i] = ys[i] + (q[i] * scale_ - ys[i]) * mono_;
            }
        }
    }

    // --- auto-gain (fast attack, slow release) ---------------------------
    float frame_peak = 0.0f;
    for (int i = 0; i < kWindow; ++i)
        frame_peak = std::max(frame_peak, std::max(std::fabs(xs[i]), std::fabs(ys[i])));
    const float gain = 1.0f / std::max(peak, 0.05f);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        peak_ = frame_peak > peak ? frame_peak : peak * 0.97f + frame_peak * 0.03f;
    }

    // --- draw the beam ---------------------------------------------------
    const int side = std::min(pw, ph);
    const float cx = (pw - 1) * 0.5f, cy = (ph - 1) * 0.5f;
    const float amp = (side - 1) * 0.5f * 0.94f;

    auto plot = [&](int x, int y, float v) {
        if (x < 0 || x >= pw || y < 0 || y >= ph || v <= 0.0f) return;
        float& g = glow_[static_cast<size_t>(y) * pw + x];
        g = std::max(g, std::min(v, 1.0f));
    };
    // Xiaolin Wu anti-aliased line; `i0`/`i1` = beam brightness at the ends.
    auto wu = [&](float x0, float y0, float x1, float y1, float i0, float i1) {
        const bool steep = std::fabs(y1 - y0) > std::fabs(x1 - x0);
        if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
        if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); std::swap(i0, i1); }
        auto put = [&](int a, int b, float cov, float inten) {
            if (steep) plot(b, a, cov * inten); else plot(a, b, cov * inten);
        };
        const float dx = x1 - x0, dy = y1 - y0;
        if (dx < 1e-4f) { // a single point (silence / stationary beam)
            put(static_cast<int>(std::lround(x0)), static_cast<int>(std::lround(y0)), 1.0f, i1);
            return;
        }
        const float grad = dy / dx;
        const int xa = static_cast<int>(std::lround(x0));
        const int xb = static_cast<int>(std::lround(x1));
        for (int x = xa; x <= xb; ++x) {
            const float t = (x1 > x0) ? std::clamp((x - x0) / dx, 0.0f, 1.0f) : 0.0f;
            const float inten = i0 + (i1 - i0) * t;
            const float y = y0 + grad * (x - x0);
            const int yi = static_cast<int>(std::floor(y));
            const float f = y - yi;
            put(x, yi, 1.0f - f, inten);
            put(x, yi + 1, f, inten);
        }
    };

    float px = cx, py = cy, pi = tail;
    for (int i = 0; i < kWindow; ++i) {
        const float vx = std::clamp(xs[i] * gain, -1.0f, 1.0f);
        const float vy = std::clamp(ys[i] * gain, -1.0f, 1.0f);
        const float x = cx + vx * amp;
        const float y = cy - vy * amp; // +R is up
        const float inten = tail + (1.0f - tail) * (static_cast<float>(i) / (kWindow - 1));
        if (i > 0) wu(px, py, x, y, pi, inten);
        else wu(x, y, x, y, inten, inten);
        px = x; py = y; pi = inten;
    }

    // --- pack the buffer into cells -------------------------------------
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            uint8_t pattern = 0;
            float level = 0.0f;
            for (int yy = 0; yy < 4; ++yy) {
                for (int xx = 0; xx < 2; ++xx) {
                    const float v = glow_[static_cast<size_t>(ty * 4 + yy) * pw + tx * 2 + xx];
                    if (v >= dot_threshold) {
                        pattern |= kBrailleMap[yy][xx];
                        level = std::max(level, v);
                    }
                }
            }
            out[ty][tx].braille = pattern;
            out[ty][tx].level = static_cast<uint8_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

} // namespace muisc
