// Stub AudioEngine for the game_tests binary — no Oboe dependency needed.
#include "audio.h"
AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() {}   // impl_ is always nullptr in stubs
bool AudioEngine::init()              { return false; }
void AudioEngine::shutdown()          {}
void AudioEngine::triggerLaser()      {}
void AudioEngine::triggerExplosion()  {}
void AudioEngine::triggerPlayerHit()  {}
void AudioEngine::triggerLevelClear() {}
void AudioEngine::triggerPowerUp()    {}
void AudioEngine::triggerMarch(int)   {}
void AudioEngine::setSaucer(bool)     {}
void AudioEngine::setMusicEnabled(bool) {}
