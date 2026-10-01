#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace muisc {

// XY (Lissajous / vectorscope) oscilloscope for the lyrics panel -- the
// time-domain alternative to SphereVisualizer, picked with Settings ->
// ON/OFF -> "Lyric Viz".
//
// XY mode: the left channel drives the horizontal deflection, the right
// channel the vertical one, exactly like a scope in X-Y mode. A mono
// signal (L == R) therefore draws a diagonal line, a wide stereo signal
// fills out a cloud, out-of-phase material leans the other way.
//
// Rendering: instead of one-bit braille dots, the beam is drawn with
// anti-aliased (Xiaolin Wu) lines into a float "phosphor" buffer on the
// 2x4-subpixel-per-cell grid. The buffer decays a little every frame, so
// the trace leaves a short fading afterglow like a real CRT, and the
// newest part of each frame's trace is brighter than its oldest part.
// Every output cell carries the braille dot pattern (subpixels above a
// threshold) AND a brightness level (the brightest subpixel in the cell),
// which the caller uses to dim the cell's colour -- that is what gives
// the line its smooth, fine-grained look in a text terminal.
//
// Mono material (L == R, e.g. old mono recordings or mono-decoded files)
// would only ever draw a diagonal line in a true XY scope. When the side
// signal (L - R) is almost absent, the scope therefore cross-fades to a
// phase portrait of the mid signal: x = s(t), y = ds/dt (scaled so a pure
// tone of any pitch draws a circle). Stereo content stays a true XY scope;
// the cross-fade is smoothed over a few frames so nothing flickers when a
// track hovers around the threshold.
//
// Audio plumbing is unchanged: Player::data_callback() calls push_frames()
// with every block it sends to the device (one short lock, nothing ever
// allocated on the audio thread); render() snapshots that ring on the
// render thread.
class OscilloscopeVisualizer {
public:
    struct Cell {
        uint8_t braille = 0; // U+2800 dot pattern, 0 = empty cell
        uint8_t level = 0;   // 0..255 brightness of the brightest subpixel
    };

    // The three look-and-feel knobs, adjustable live from the main UI's
    // oscilloscope overlay (SHIFT+O). Passed into every render() call
    // instead of being stored here, so no thread ever races with a setter.
    struct Params {
        // Afterglow: the phosphor buffer is multiplied by this every
        // frame. Higher = longer trails (0.00 .. 0.99; 1.0 would never fade).
        float decay = 0.80f;
        // Brightness (0..1) a subpixel needs to light a braille dot.
        // Lower = thicker/softer line, higher = thin, sharp core.
        float dot_threshold = 0.28f;
        // Brightness (0..1) of the OLDEST sample in a frame's trace
        // relative to the newest (1.0). Low = comet-like fading tail.
        float tail_brightness = 0.45f;
    };

    // AUDIO CALLBACK thread. `interleaved_lr` is the exact stereo block
    // just written to the device (L,R,L,R,... -- post gain, post
    // normalisation, post limiter). Fixed-size ring, never allocates; a
    // block larger than the ring keeps only its tail.
    void push_frames(const float* interleaved_lr, size_t frames);

    // Clears the ring, the auto-gain state and the phosphor afterglow --
    // call when a new track starts (same contract as FftVisualizer::reset()).
    void reset();

    // RENDER thread (single caller). Returns `rows` rows of `cols` cells.
    // Every row has exactly `cols` entries, empty cells included, so the
    // caller can pad/colour without any width bookkeeping. The drawing area
    // is the largest centred square of the grid (a braille subpixel is
    // square on screen), so circles stay circles.
    std::vector<std::vector<Cell>> render(int cols, int rows, const Params& params);

private:
    static constexpr int kRingFrames = 2048;
    static constexpr int kWindow = 1024; // newest samples drawn per frame

    std::mutex mtx_; // guards ring_/write_/peak_/reset flag across audio + render threads
    std::array<float, kRingFrames> ch0_{};
    std::array<float, kRingFrames> ch1_{};
    int write_ = 0;
    float peak_ = 0.05f;
    bool cleared_ = false; // set by reset(), consumed by render() to wipe the afterglow

    // Render-thread-only state (never touched by the audio thread).
    std::vector<float> glow_; // phosphor buffer, pw*ph, 0..1
    int glow_w_ = 0, glow_h_ = 0;
    float mono_ = 0.0f;  // smoothed 0 (true XY) .. 1 (phase portrait) cross-fade
    float scale_ = 0.0f; // smoothed ds/dt -> amplitude scale for the phase portrait
};

} // namespace muisc
