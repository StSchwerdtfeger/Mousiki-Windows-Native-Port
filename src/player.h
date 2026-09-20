#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "miniaudio.h"
#include "streaming_pcm.h"
#include "fft_visualizer.h"

namespace muisc {

// Plays back a StreamingPcm buffer through a real audio device via
// miniaudio, using the backend pinned in audio_backend.h
// (PulseAudio/ALSA -> PipeWire on Linux, WASAPI on Windows, OpenSL ES on
// Android).
//
// This reads from a buffer that may STILL BE FILLING IN — play() can be
// called the moment decode starts (as soon as duration is known from
// ffprobe and the buffer's capacity is reserved), and the callback below
// just plays silence for any frame past what's been decoded so far,
// self-correcting once the decode thread catches up. That's what lets
// playback start almost immediately instead of waiting for the whole
// track to decode first.
//
// This replaced an earlier ffplay-subprocess design. ffplay's audio
// output goes through SDL, and SDL's Android backend expects to be
// running inside a proper Activity with its Java glue — a plain Termux
// CLI process has neither, so SDL_OpenAudioDevice effectively never
// succeeds there and nothing plays, silently. Talking to OpenSL ES
// directly through miniaudio sidesteps that entirely.
class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // `pcm` must stay alive for as long as playback is active — App holds
    // it via shared_ptr and only replaces it once stop() has fully torn
    // the device down. `fft_sink`, if given, gets push_samples() called
    // from the audio callback with each chunk actually played (nullptr
    // to disable — e.g. not needed for a plain smoke test).
    bool play(std::shared_ptr<StreamingPcm> pcm, double start_sec, int volume_pct,
              FftVisualizer* fft_sink = nullptr);

    void pause();
    void resume();
    bool is_paused() const { return paused_; }

    void seek_relative(double delta_sec);
    void set_volume(int volume_pct);
    int volume() const;

    double poll_elapsed() const;
    bool finished() const { return finished_.load(); }
    // Synchronously clears a stale finished flag left over from the
    // previous track. play() itself resets this too, but play() now
    // runs on a detached background thread (device init can genuinely
    // stall) — without this, there's a window where has_track_ is
    // already true for the NEW track but finished_ is still true from
    // the OLD one, and the main loop's "if (has_track_ && finished())
    // advance_track()" check fires again immediately, skipping straight
    // past the track that was just supposed to start.
    void clear_finished() { finished_.store(false); }

    void stop();

private:
    // Guards play()/stop() and every getter/setter below against each
    // other. Now load-bearing rather than a nice-to-have: play()/stop() run
    // on App's persistent device-worker thread for the whole session, while
    // seek/volume/pause hotkeys and the shutdown path's stop() run on the
    // main thread -- two long-lived threads genuinely calling into the same
    // Player concurrently, not a short-lived ad-hoc thread that mostly
    // didn't overlap anything. NOT taken inside data_callback(): that runs
    // on miniaudio's own real-time audio thread, and ma_device_uninit() is
    // documented to block until that callback thread has fully stopped
    // before returning -- by the time stop_locked() reassigns pcm_, the
    // callback that used to read it is already provably not running, so
    // there is nothing there for this mutex to protect, and taking it in
    // the callback would risk an audible stall if it ever had to wait on a
    // slow device_init() elsewhere.
    mutable std::mutex mutex_;

    // stop()'s actual work, factored out so play() can call it without
    // re-locking mutex_ (std::mutex isn't recursive -- play() calling the
    // public stop() from inside its own already-held lock would deadlock).
    void stop_locked();

    ma_context context_{};
    bool context_ready_ = false;
    ma_device device_{};
    bool device_ready_ = false;

    std::shared_ptr<StreamingPcm> pcm_;
    FftVisualizer* fft_sink_ = nullptr;
    int sample_rate_ = 44100;
    std::atomic<long long> cursor_frames_{0};
    std::atomic<bool> finished_{false};
    std::atomic<float> gain_{0.7f};
    std::atomic<bool> paused_{false};
    int volume_pct_ = 70;

    static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count);
};

} // namespace muisc
