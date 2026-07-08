#pragma once
#include <functional>

// Pimpl facade — callers need no Oboe headers.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool init();
    void shutdown();

    void triggerLaser();
    void triggerExplosion();
    void triggerPlayerHit();
    void triggerLevelClear();
    void triggerPowerUp();
    // Classic four-note invader march; step cycles 0..3 as the formation moves.
    void triggerMarch(int step);
    // Warbling siren while the bonus saucer crosses the screen.
    void setSaucer(bool active);
    void setMusicEnabled(bool enabled);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
