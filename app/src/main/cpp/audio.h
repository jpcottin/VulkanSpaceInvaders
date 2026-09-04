#pragma once
#include <memory>

// Pimpl facade — callers need no Oboe headers.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool init();
    void shutdown();
    // Activity lost / regained focus: silence the stream without closing it.
    void pause();
    void resume();

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
    // Shared, not owned outright: Oboe's error thread holds a reference while
    // it delivers onErrorAfterClose, so the Impl outlives a teardown that
    // races a route change.
    std::shared_ptr<Impl> impl_;
};
