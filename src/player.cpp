#include "player.h"
#include "audio_backend.h"
#include "console_log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace muisc {

namespace {

// Transparent below the knee, then eases into +/-1.0 with a continuous
// slope instead of hard-clipping. Only used while normalisation is on, where
// boosting a quiet track could otherwise push its peaks over full scale.
inline float soft_limit(float x) {
    constexpr float kKnee = 0.891f; // -1 dBFS
    const float a = std::fabs(x);
    if (a <= kKnee) return x;
    const float y = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
    return x < 0.0f ? -y : y;
}

} // namespace

Player::Player() = default;
Player::~Player() { stop(); }

void Player::data_callback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
    Player* self = static_cast<Player*>(device->pUserData);
    float* out = static_cast<float*>(output);

    if (!self || !self->pcm_ || self->paused_.load()) {
        std::memset(out, 0, frame_count * sizeof(float));
        return;
    }

    StreamingPcm& pcm = *self->pcm_;
    long long cur = self->cursor_frames_.load();
    float gain = self->gain_.load();

    // Loudness normalisation: target gain from this track's measured LUFS.
    // Until the measurement exists (or with normalisation off) the target is
    // unity. The applied gain follows the target with a ~1 s time constant,
    // so toggling it, or the estimate being refined as decoding proceeds,
    // never produces a click or a sudden jump.
    const bool norm_on = self->norm_enabled_.load();
    const float lufs = pcm.loudness_lufs.load(std::memory_order_relaxed);
    float norm_target = 1.0f;
    float norm_db = 0.0f;
    if (norm_on && !std::isnan(lufs)) {
        norm_db = std::clamp(self->norm_target_lufs_.load() - lufs, -30.0f, self->norm_max_boost_db_.load());
        norm_target = std::pow(10.0f, norm_db / 20.0f);
    }
    self->track_lufs_.store(lufs);
    self->norm_gain_db_.store(norm_db);
    float norm_cur = self->norm_cur_ < 0.0f ? norm_target : self->norm_cur_;
    const int sr_now = std::max(1, self->sample_rate_.load());
    const float norm_alpha = 1.0f - std::exp(-1.0f / (1.0f * static_cast<float>(sr_now)));
    // Acquire-load: pairs with the release-store in StreamingPcm::append(),
    // guaranteeing every index below `avail` was fully written by the
    // decode thread before we read it here.
    size_t avail = pcm.available.load(std::memory_order_acquire);

    for (ma_uint32 i = 0; i < frame_count; ++i) {
        long long idx = cur + static_cast<long long>(i);
        norm_cur += (norm_target - norm_cur) * norm_alpha;
        float s = (idx >= 0 && static_cast<size_t>(idx) < avail) ? pcm.data[static_cast<size_t>(idx)] * norm_cur * gain : 0.0f;
        out[i] = (norm_on || norm_cur > 1.001f) ? soft_limit(s) : s;
    }
    self->norm_cur_ = norm_cur;

    if (self->fft_sink_) self->fft_sink_->push_samples(out, frame_count, self->sample_rate_.load());

    long long new_cur = cur + static_cast<long long>(frame_count);
    // Only truly "finished" once decode is done AND playback has caught
    // all the way up to everything it ever produced — not just the
    // current available count, which may still be growing while we play.
    if (pcm.decode_done.load() &&
        new_cur >= 0 && static_cast<size_t>(new_cur) >= pcm.available.load(std::memory_order_acquire)) {
        self->finished_.store(true);
    }
    self->cursor_frames_.store(new_cur);
}

bool Player::play(std::shared_ptr<StreamingPcm> pcm, double start_sec, int volume_pct,
                   FftVisualizer* fft_sink) {
    // Give the loudness measurement a moment to exist so the track starts at
    // its final level instead of gliding into it. Decoding runs far faster
    // than real time, so this normally returns immediately; capped so a slow
    // source can't delay playback noticeably. Done BEFORE taking mutex_ so
    // the main thread's stop() is never held up behind it.
    if (pcm && norm_enabled_.load()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        while (std::isnan(pcm->loudness_lufs.load(std::memory_order_relaxed)) &&
               !pcm->loudness_final.load(std::memory_order_acquire) &&
               !pcm->decode_failed.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::lock_guard<std::mutex> lk(mutex_);
    stop_locked();
    if (!pcm) return false;

    if (!context_ready_) {
        context_ready_ = init_platform_audio_context(context_);
        // Not fatal if this fails — ma_device_init(nullptr, ...) below
        // falls back to miniaudio's own default backend selection.
    }

    pcm_ = std::move(pcm);
    fft_sink_ = fft_sink;
    sample_rate_.store(pcm_->sample_rate > 0 ? pcm_->sample_rate : 44100);
    volume_pct_.store(std::clamp(volume_pct, 0, 100));
    gain_.store(volume_pct_.load() / 100.0f);
    finished_.store(false);
    paused_.store(false);
    norm_cur_ = -1.0f; // audio device isn't running yet: safe to reset; first callback snaps to the target gain
    track_lufs_.store(std::numeric_limits<float>::quiet_NaN());
    norm_gain_db_.store(0.0f);
    cursor_frames_.store(static_cast<long long>(std::max(0.0, start_sec) * sample_rate_.load()));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.sampleRate = static_cast<ma_uint32>(sample_rate_.load());
    cfg.dataCallback = data_callback;
    cfg.pUserData = this;

    ma_context* ctx = context_ready_ ? &context_ : nullptr;
    ma_result init_res = ma_device_init(ctx, &cfg, &device_);
    if (init_res != MA_SUCCESS) {
        pcm_.reset();
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_init failed: ") + ma_result_description(init_res));
        return false;
    }
    ma_result start_res = ma_device_start(&device_);
    if (start_res != MA_SUCCESS) {
        ma_device_uninit(&device_);
        pcm_.reset();
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_start failed: ") + ma_result_description(start_res));
        return false;
    }
    ConsoleLog::instance().log_verbose(
        std::string("audio: device started, backend=") + ma_get_backend_name(device_.pContext->backend) +
        ", rate=" + std::to_string(sample_rate_.load()) + "Hz");

    device_ready_ = true;
    return true;
}

void Player::pause() { paused_.store(true); }
void Player::resume() { paused_.store(false); }

int Player::volume() const {
    // Lock-free on purpose: called every frame by the UI, and mutex_ can be
    // held for hundreds of ms by play() during a device swap.
    return volume_pct_.load();
}

void Player::seek_relative(double delta_sec) {
    // try_lock, not lock: if play()/stop() is mid device swap the old pcm_ is
    // being torn down anyway, so dropping the seek is right -- and blocking
    // the main thread on it would freeze the UI.
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock() || !pcm_) return;
    long long delta_frames = static_cast<long long>(delta_sec * sample_rate_.load());
    long long cur = cursor_frames_.load();
    // Clamp against reserved capacity (the eventual max), not the
    // currently-decoded amount — seeking a bit ahead of what's decoded
    // so far is fine, it just plays silence until decode catches up.
    long long cap = static_cast<long long>(pcm_->data.capacity());
    long long next = std::clamp<long long>(cur + delta_frames, 0, cap);
    cursor_frames_.store(next);
    if (next < cap) finished_.store(false);
}

void Player::set_normalization(bool enabled, float target_lufs, float max_boost_db) {
    norm_target_lufs_.store(std::clamp(target_lufs, -40.0f, 0.0f));
    norm_max_boost_db_.store(std::clamp(max_boost_db, 0.0f, 24.0f));
    norm_enabled_.store(enabled);
}

void Player::set_volume(int volume_pct) {
    // Atomics only -- see volume().
    const int v = std::clamp(volume_pct, 0, 100);
    volume_pct_.store(v);
    gain_.store(v / 100.0f);
}

double Player::poll_elapsed() const {
    // Lock-free on purpose -- see volume().
    const int sr = sample_rate_.load();
    if (sr <= 0) return 0.0;
    return static_cast<double>(cursor_frames_.load()) / sr;
}

void Player::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stop_locked();
}

void Player::stop_locked() {
    if (device_ready_) {
        ma_device_uninit(&device_);
        device_ready_ = false;
    }
    pcm_.reset();
    fft_sink_ = nullptr;
}

} // namespace muisc
