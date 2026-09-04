#include "audio.h"
#include "common.h"
#include <oboe/Oboe.h>
#include <atomic>
#include <array>
#include <cmath>
#include <mutex>

// ── Impl ──────────────────────────────────────────────────────────────────────

struct AudioEngine::Impl : public oboe::AudioStreamDataCallback,
                           public oboe::AudioStreamErrorCallback,
                           public std::enable_shared_from_this<AudioEngine::Impl> {

    static constexpr int   kVoices = 8;
    static constexpr float kTau    = 6.28318530f;

    enum class ST : uint8_t { LASER, EXPLOSION, HIT, CLEAR, POWERUP, MARCH };

    struct Voice {
        ST    type   = ST::LASER;
        float t      = 0.0f;
        float phase  = 0.0f;
        float freq   = 0.0f;   // MARCH: note frequency chosen at trigger time
        bool  active = false;
    };

    // `stream` is touched by the game thread (open/close/pause/resume) and by
    // Oboe's error thread (onErrorAfterClose); `streamMutex` guards it. Oboe
    // calls are made outside the lock so lifecycle calls never wait on
    // audioserver.
    std::mutex                         streamMutex;
    std::shared_ptr<oboe::AudioStream> stream;
    bool                               closing = false;
    bool                               paused  = false;
    std::array<Voice, kVoices>         voices{};

    // The stream opens at the device's native rate (no resampler in the
    // path); the synth steps every oscillator by this. Published together
    // with the stream it describes, before that stream starts; the audio
    // thread reads it per sample.
    std::atomic<float> sampleDt{1.0f / 48000.0f};

    // Bits: 0=LASER 1=EXPLOSION 2=HIT 3=CLEAR 4=POWERUP – game thread writes.
    std::atomic<uint32_t> pending{0};
    // -1 = none; 0..3 = play that march step. Game thread writes.
    std::atomic<int>      marchPending{-1};
    std::atomic<bool>     saucer{false};
    std::atomic<bool>     musicEnabled{false};

    float saucerPhase  = 0.0f;
    float saucerWobble = 0.0f;
    float saucerEnv    = 0.0f;
    uint32_t rng = 0xDEADBEEFu;

    // The classic descending four-note bass loop the formation marches to.
    static constexpr float kMarchFreqs[4] = {110.0f, 98.0f, 87.3f, 82.4f}; // A2 G2 F2 E2

    // ── Background music state (audio thread only) ────────────────────────────
    // 80 BPM ambient space track in A-minor: pad + bass + arpeggio.
    static constexpr float kTempo       = 80.0f / 60.0f;   // beats/s
    static constexpr float kArpInterval = 60.0f / 80.0f / 2.0f; // 8th note = 0.375s
    static constexpr float kPadFreqs[3] = {110.0f, 130.8f, 164.8f}; // A2 C3 E3
    static constexpr float kArpFreqs[4] = {220.0f, 261.6f, 329.6f, 440.0f}; // A3 C4 E4 A4

    // Wrapping phases instead of an ever-growing time accumulator: a float
    // that keeps counting seconds loses the per-sample step after ~8 minutes.
    float tremPhase_   = 0.0f;   // [0,1) at 0.28 Hz
    float barPhase_    = 0.0f;   // [0,4) beats
    float padPhase_[3] = {};
    float bassPhase_   = 0.0f;
    float bassEnv_     = 0.0f;
    float arpPhase_    = 0.0f;
    float arpEnv_      = 0.55f;  // start audible on step 0 (A3) immediately
    float arpTimer_    = 0.0f;
    int   arpStep_     = 0;
    int   lastBeat_    = -1;

    // ── helpers ──────────────────────────────────────────────────────────────

    float white() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return static_cast<float>(rng & 0xFFFFu) / 32768.0f - 1.0f;
    }

    void activate(ST type, float freq = 0.0f) {
        for (auto& v : voices) {
            if (!v.active) { v = {type, 0.0f, 0.0f, freq, true}; return; }
        }
        voices[0] = {type, 0.0f, 0.0f, freq, true};
    }

    float genVoice(Voice& v) {
        const float dt = sampleDt.load(std::memory_order_relaxed);
        float s = 0.0f;

        switch (v.type) {
            case ST::LASER: {
                constexpr float dur = 0.12f;
                if (v.t > dur) { v.active = false; break; }
                float prog  = v.t / dur;
                float freq  = 1100.0f - 750.0f * prog;
                v.phase    += dt * freq;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                s = sinf(v.phase * kTau) * (1.0f - prog) * 0.42f;
                break;
            }
            case ST::EXPLOSION: {
                constexpr float dur = 0.50f;
                if (v.t > dur) { v.active = false; break; }
                // Gains sum to ~0.9 so a single explosion never hits the clipper.
                float crack  = white() * expf(-v.t / 0.010f) * 0.32f;
                v.phase      = v.phase * 0.48f + white() * 0.52f;
                float rumble = v.phase * expf(-v.t / 0.09f) * 0.36f;
                float boom   = sinf(v.t * kTau * 55.0f) * expf(-v.t / 0.045f) * 0.22f;
                s = crack + rumble + boom;
                break;
            }
            case ST::HIT: {
                constexpr float dur = 0.20f;
                if (v.t > dur) { v.active = false; break; }
                float thud  = sinf(v.t * kTau * 90.0f) * expf(-v.t / 0.05f);
                float noise = white() * expf(-v.t / 0.025f);
                s = (thud * 0.6f + noise * 0.4f) * 0.50f;
                break;
            }
            case ST::CLEAR: {
                constexpr float dur     = 0.55f;
                constexpr float segDur  = dur / 3.0f;
                constexpr float freqs[3]= {523.25f, 659.25f, 783.99f};
                if (v.t > dur) { v.active = false; break; }
                int   ni   = static_cast<int>(v.t / segDur);
                if (ni > 2) ni = 2;
                float nt   = fmodf(v.t, segDur);
                float env  = sinf(nt / segDur * 3.14159265f);
                v.phase   += dt * freqs[ni];
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                s = sinf(v.phase * kTau) * env * 0.45f;
                break;
            }
            case ST::POWERUP: {
                // Rising sparkle arpeggio: four notes ascending over 0.28s
                constexpr float dur      = 0.28f;
                constexpr float noteDur  = dur / 4.0f;
                constexpr float noteFreq[4] = {440.0f, 554.4f, 659.3f, 880.0f};
                if (v.t > dur) { v.active = false; break; }
                int   ni  = (int)(v.t / noteDur);
                if (ni > 3) ni = 3;
                float nt  = fmodf(v.t, noteDur);
                float env = sinf(nt / noteDur * 3.14159265f);
                v.phase  += dt * noteFreq[ni];
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                s = sinf(v.phase * kTau) * env * 0.38f;
                break;
            }
            case ST::MARCH: {
                // Short square-wave bass thump — one step of the invader march.
                constexpr float dur = 0.09f;
                if (v.t > dur) { v.active = false; break; }
                v.phase += dt * v.freq;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                float sq  = v.phase < 0.5f ? 1.0f : -1.0f;
                float env = expf(-v.t / 0.035f);
                s = sq * env * 0.30f;
                break;
            }
        }
        v.t += dt;
        return s;
    }

    float genMusic() {
        const float dt = sampleDt.load(std::memory_order_relaxed);
        float s = 0.0f;

        // Slow tremolo LFO at 0.28 Hz
        tremPhase_ += dt * 0.28f;
        if (tremPhase_ >= 1.0f) tremPhase_ -= 1.0f;
        float tremolo = 0.72f + 0.28f * sinf(tremPhase_ * kTau);

        // Pad chord: A2 + C3 + E3 sustained
        for (int i = 0; i < 3; i++) {
            padPhase_[i] += dt * kPadFreqs[i];
            if (padPhase_[i] >= 1.0f) padPhase_[i] -= 1.0f;
            s += sinf(padPhase_[i] * kTau) * 0.022f * tremolo;
        }

        // Bass: A1 (55 Hz) triggered on beats 1 and 3 of a 4-beat bar
        barPhase_ += dt * kTempo;
        if (barPhase_ >= 4.0f) barPhase_ -= 4.0f;
        int   beat     = (int)barPhase_;
        if (beat != lastBeat_) {
            lastBeat_ = beat;
            if (beat == 0 || beat == 2) bassEnv_ = 1.0f;
        }
        bassPhase_ += dt * 55.0f;
        if (bassPhase_ >= 1.0f) bassPhase_ -= 1.0f;
        bassEnv_  *= 0.9989f;
        s += sinf(bassPhase_ * kTau) * bassEnv_ * 0.065f;

        // Melody arpeggio: A3 C4 E4 A4 cycling on 8th notes
        arpTimer_ += dt;
        if (arpTimer_ >= kArpInterval) {
            arpTimer_ -= kArpInterval;
            arpStep_   = (arpStep_ + 1) & 3;
            arpEnv_    = 0.55f;
        }
        arpPhase_ += dt * kArpFreqs[arpStep_];
        if (arpPhase_ >= 1.0f) arpPhase_ -= 1.0f;
        arpEnv_  *= 0.9988f;
        s += sinf(arpPhase_ * kTau) * arpEnv_ * 0.018f;

        return s;
    }

    // ── Oboe callback ─────────────────────────────────────────────────────────

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream*, void* data, int32_t frames) override {

        float* out = static_cast<float*>(data);

        uint32_t mask = pending.exchange(0, std::memory_order_relaxed);
        if (mask &  1u) activate(ST::LASER);
        if (mask &  2u) activate(ST::EXPLOSION);
        if (mask &  4u) activate(ST::HIT);
        if (mask &  8u) activate(ST::CLEAR);
        if (mask & 16u) activate(ST::POWERUP);

        int step = marchPending.exchange(-1, std::memory_order_relaxed);
        if (step >= 0) activate(ST::MARCH, kMarchFreqs[step & 3]);

        const bool sau   = saucer.load(std::memory_order_relaxed);
        const bool music = musicEnabled.load(std::memory_order_relaxed);
        const float dt   = sampleDt.load(std::memory_order_relaxed);

        for (int i = 0; i < frames; i++) {
            float s = 0.0f;

            for (auto& v : voices)
                if (v.active) s += genVoice(v);

            // Saucer siren: pitch warbles between ~550 and ~900 Hz. The envelope
            // eases in/out so toggling never clicks.
            saucerEnv += ((sau ? 1.0f : 0.0f) - saucerEnv) * 0.0006f;
            if (saucerEnv > 0.001f) {
                saucerWobble += dt * 6.0f;
                if (saucerWobble >= 1.0f) saucerWobble -= 1.0f;
                float freq = 725.0f + 175.0f * sinf(saucerWobble * kTau);
                saucerPhase += dt * freq;
                if (saucerPhase >= 1.0f) saucerPhase -= 1.0f;
                s += sinf(saucerPhase * kTau) * saucerEnv * 0.10f;
            }

            if (music) s += genMusic();

            // Soft clip: stacked explosions + march + siren compress instead
            // of turning into a square wave.
            out[i] = tanhf(s);
        }
        return oboe::DataCallbackResult::Continue;
    }

    // ── stream lifecycle ──────────────────────────────────────────────────────

    // Open (but don't start) an output stream at the device's native rate.
    // No lock held: this talks to audioserver.
    std::shared_ptr<oboe::AudioStream> openStream() {
        std::shared_ptr<oboe::AudioStream> s;
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
               ->setFormat(oboe::AudioFormat::Float)
               ->setChannelCount(oboe::ChannelCount::Mono)
               // onAudioReady writes mono float unconditionally; let Oboe
               // convert when the device stream differs, instead of the
               // callback misinterpreting the buffer (garbage audio).
               ->setFormatConversionAllowed(true)
               ->setChannelConversionAllowed(true)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               // Shared: exclusive endpoints are the ones AAudio takes away
               // when another app wants one, and this synth gains nothing
               // from the extra few ms.
               ->setSharingMode(oboe::SharingMode::Shared)
               ->setDataCallback(shared_from_this())
               ->setErrorCallback(shared_from_this());

        oboe::Result r = builder.openStream(s);
        if (r != oboe::Result::OK) {
            LOGW("Oboe openStream: %s", oboe::convertToText(r));
            return nullptr;
        }
        LOGI("Oboe stream: %d Hz, %s, %s", s->getSampleRate(),
             oboe::convertToText(s->getSharingMode()),
             oboe::convertToText(s->getPerformanceMode()));
        return s;
    }

    static void start(const std::shared_ptr<oboe::AudioStream>& s) {
        oboe::Result r = s->requestStart();
        if (r != oboe::Result::OK) LOGW("Oboe requestStart: %s", oboe::convertToText(r));
    }

    bool open() {
        std::shared_ptr<oboe::AudioStream> s = openStream();
        if (!s) return false;
        bool startIt;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            closing = false;
            stream = s;
            sampleDt.store(1.0f / (float)s->getSampleRate(), std::memory_order_relaxed);
            startIt = !paused;
        }
        if (startIt) start(s);
        return true;
    }

    // Route changes (headphones unplugged, Bluetooth switch, the endpoint
    // taken by another app) disconnect the stream; Oboe has already stopped
    // and closed it. Open a fresh one so audio comes back. Runs on Oboe's
    // error thread; the open itself happens outside the lock.
    void onErrorAfterClose(oboe::AudioStream* errored, oboe::Result error) override {
        LOGW("Audio stream closed (%s), reopening", oboe::convertToText(error));
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            if (closing || stream.get() != errored) return;
            stream.reset();
        }
        std::shared_ptr<oboe::AudioStream> fresh = openStream();
        if (!fresh) return;
        bool install, startIt = false;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            install = !closing && !stream;   // shutdown() may have raced us
            if (install) {
                stream = fresh;
                sampleDt.store(1.0f / (float)fresh->getSampleRate(), std::memory_order_relaxed);
                startIt = !paused;
            }
        }
        if (!install) { fresh->close(); return; }
        if (startIt) start(fresh);
    }

    void close() {
        std::shared_ptr<oboe::AudioStream> s;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            closing = true;
            s = std::move(stream);
        }
        if (s) {
            s->requestStop();
            s->close();
        }
    }

    void setPaused(bool p) {
        std::shared_ptr<oboe::AudioStream> s;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            if (paused == p) return;   // GAINED_FOCUS right after open(): already running
            paused = p;
            s = stream;
        }
        if (!s) return;
        oboe::Result r = p ? s->requestPause() : s->requestStart();
        if (r != oboe::Result::OK)
            LOGW("Oboe %s: %s", p ? "requestPause" : "requestStart", oboe::convertToText(r));
    }
};

// ── AudioEngine public API ────────────────────────────────────────────────────

AudioEngine::AudioEngine()  : impl_(std::make_shared<Impl>()) {}
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init()     { return impl_->open(); }
void AudioEngine::shutdown() { impl_->close(); }
void AudioEngine::pause()    { impl_->setPaused(true); }
void AudioEngine::resume()   { impl_->setPaused(false); }

void AudioEngine::triggerLaser()     { impl_->pending.fetch_or( 1u, std::memory_order_relaxed); }
void AudioEngine::triggerExplosion() { impl_->pending.fetch_or( 2u, std::memory_order_relaxed); }
void AudioEngine::triggerPlayerHit() { impl_->pending.fetch_or( 4u, std::memory_order_relaxed); }
void AudioEngine::triggerLevelClear(){ impl_->pending.fetch_or( 8u, std::memory_order_relaxed); }
void AudioEngine::triggerPowerUp()   { impl_->pending.fetch_or(16u, std::memory_order_relaxed); }
void AudioEngine::triggerMarch(int step) { impl_->marchPending.store(step & 3, std::memory_order_relaxed); }
void AudioEngine::setSaucer(bool a)  { impl_->saucer.store(a,       std::memory_order_relaxed); }
void AudioEngine::setMusicEnabled(bool e) { impl_->musicEnabled.store(e, std::memory_order_relaxed); }
