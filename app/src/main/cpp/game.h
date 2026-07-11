#pragma once
#include <vector>
#include <functional>
#include "common.h"

class AudioEngine;

// All gameplay: player ship, marching invader formation, bombs, saucer,
// power-ups, 10 progressing levels, score, lives, HUD.
// World coordinates: x in [-asp, asp], y in [-1, 1], y pointing DOWN (the
// invaders descend toward +y). asp = width/height. The renderer divides x by
// asp, so shapes stay square on screen.
//
// Controls: the bottom strip of the screen (below the ship) is the control
// zone — while a finger presses there, the ship steers toward the finger's x
// and fires continuously.
class Game {
public:
    Game();
    void setViewport(int w, int h);

    // Input (pixel coordinates from the touch screen).
    void onPointerDown(int id, float x, float y);
    void onPointerMove(int id, float x, float y);
    void onPointerUp(int id);
    void onPointersCancel();

    void update(float dt);
    void render(std::vector<DrawCmd>& out);
    void clearColor(float out[3]) const;

    // True on static screens (title / game over / win) where the main loop may
    // throttle the frame rate to save battery — and on the phone for the whole
    // time the game is being played on the glasses, so the phone's swapchain
    // stops competing with the projected display's stream.
    bool isIdleScreen() const {
        return state_ == TITLE || state_ == GAME_OVER || state_ == WIN ||
               (glassesActive_ && controlMode_ == CONTROL_STRIP);
    }

public:
    // Row tiers, exposed so tests can assert per-tier scoring.
    enum AlienType { ALIEN_SQUID = 0, ALIEN_CRAB = 1, ALIEN_OCTOPUS = 2 };
    enum PowerUpType { PU_SHIELD = 0, PU_RAPID = 1, PU_TRIPLE = 2 };

    // How touch input drives the ship.
    //  CONTROL_STRIP:    phone — the strip below the ship steers + fires.
    //  CONTROL_TOUCHBAR: AI glasses — the whole touch surface is the bar:
    //                    finger x steers, contact fires. No strip, no gear,
    //                    pure-black clear (black = transparent on AR lenses).
    enum ControlMode { CONTROL_STRIP = 0, CONTROL_TOUCHBAR = 1 };
    void setControlMode(ControlMode m) { controlMode_ = m; }

    // Glasses session plumbing, fed by the platform layer (main.cpp):
    // connection state is polled from the Kotlin bridge; "active" mirrors the
    // process-wide session flag while the game runs on the glasses.
    void setGlassesConnected(bool v) { glassesConnected_ = v; }
    void setGlassesActive(bool v);
    void setGlassesLaunchCallback(std::function<bool()> fn) { glassesLaunch_ = std::move(fn); }
    void setGlassesExitCallback(std::function<void()> fn)   { glassesExit_ = std::move(fn); }

private:
    enum State { TITLE, PLAYING, LEVEL_CLEAR, GAME_OVER, WIN, SETTINGS };

    // One invader. Its world position derives from the formation origin plus
    // its (row, col) slot, so the whole wave marches as a unit.
    struct Alien {
        int  row, col;
        AlienType type;
        bool alive;
    };
    struct Bullet  { float x, y, vx, vy; bool alive; };          // player laser, flies up
    struct Bomb    { float x, y, vx, vy, wobble; bool alive; };  // alien laser, falls down
    struct Saucer  { float x, y, vx; bool alive = false; };
    // Level-10 boss: a giant mothership drifting sinusoidally across the top,
    // lobbing bombs aimed at the player. Killing it wins the game.
    struct Boss {
        float t = 0.0f, x = 0.0f;
        int   hp = 0, maxHp = 0;
        bool  alive = false;
    };
    struct PowerUp { float x, y, vy, rot, spin; PowerUpType type; bool alive; };
    struct Particle {
        float x, y, vx, vy;
        float rot, spin;
        float t, maxLife, size;
        float r, g, b;
        bool alive;
    };
    struct Explosion {
        float x, y, radius;
        float t, maxLife;
        float cr, cg, cb;
        bool alive;
    };
    struct Star { float x, y, size, phase; };
    struct Pointer { bool active; float x, y; };
    struct HighScore { long score = 0; int level = 0; };

    static const int kMaxScores = 5;
    static const int kCols = 8;      // invaders per row
    static const int kMaxRows = 5;   // formation height cap (level 1 has 3)

    // --- helpers ---
    float frand();
    float frange(float a, float b);
    void startGame();
    void startLevel(int level);
    void buildFormation();
    // PLAYING-state update steps, called in this order from update().
    void updateShip(float dt);
    void updateFormation(float dt);
    void updateBoss(float dt);
    void updateBombs(float dt);
    void updateBullets(float dt);
    void updateSaucer(float dt);
    void updatePowerUps(float dt);
    void removeDeadEntities();
    void checkLevelClear();
    void applyShipHit();
    void triggerInvasion();
    void dropBomb();
    void spawnSaucer();
    void spawnPowerUp(float x, float y);
    void applyPowerUp(PowerUpType t);
    void killAlien(Alien& a);
    float alienX(const Alien& a) const;
    float alienY(const Alien& a) const;
    float marchSpeed() const;
    int   aliveAliens() const;
    bool  moveHeld() const;
    bool  fireHeld() const;
    float controlTargetX() const;
    void loadHighScores();
    void saveHighScores();
    void mergeHighScore(long score, int level);
    void checkHighScore();
    void spawnDebris(float x, float y, float r, float cr, float cg, float cb);

    // emit one shape: world centre (wx,wy), world half-extents (sx,sy), rotation, rgba.
    // style selects the fragment-shader fill (STYLE_FLAT default, STYLE_GLOW).
    void emit(std::vector<DrawCmd>& out, int shape, float wx, float wy,
              float sx, float sy, float rot, float r, float g, float b, float a,
              float style = (float)STYLE_FLAT);
    void drawDigit(std::vector<DrawCmd>& out, int d, float cx, float cy,
                   float h, float r, float g, float b, float a);
    void drawNumber(std::vector<DrawCmd>& out, int value, float leftX, float cy,
                    float h, float r, float g, float b, float a);
    void drawLetter(std::vector<DrawCmd>& out, char ch, float cx, float cy,
                    float h, float r, float g, float b, float a);
    void drawText(std::vector<DrawCmd>& out, const char* text, float cx, float cy,
                  float h, float r, float g, float b, float a);
    void drawShip(std::vector<DrawCmd>& out, float cx, float cy, float scale,
                  float tilt, float alpha);
    void drawAlien(std::vector<DrawCmd>& out, const Alien& a, float cx, float cy,
                   float alpha);
    void drawPowerUpHUD(std::vector<DrawCmd>& out);
    void drawBossHealthBar(std::vector<DrawCmd>& out);
    void drawControlStrip(std::vector<DrawCmd>& out);
    void drawGearIcon(std::vector<DrawCmd>& out, float cx, float cy, float size,
                      float r, float g, float b, float a);
    void drawGlassesIcon(std::vector<DrawCmd>& out, float cx, float cy, float size,
                         float r, float g, float b, float a);
    void drawOnGlassesOverlay(std::vector<DrawCmd>& out);
    void drawSettingsScreen(std::vector<DrawCmd>& out);
    void loadSettings();
    void saveSettings();
    void updateAutoPlay(float dt);
    bool isGearTap(float px, float py) const;
    int numDigits(int v) const;

    // --- audio ---
    AudioEngine* audio_ = nullptr;

    // --- haptic ---
    std::function<void()> haptic_;

    // --- high scores ---
    HighScore highScores_[kMaxScores] = {};
    bool newHighScore_    = false;
    int  newHighScoreRank_= -1;
    char dataPath_[512]   = {};

public:
    void setAudioEngine(AudioEngine* a) { audio_ = a; }
    void setDataPath(const char* path);
    // Re-read highscores/settings from disk (phone <-> glasses handoff).
    void reloadFromDisk();
    void setHapticCallback(std::function<void()> fn) { haptic_ = std::move(fn); }

    // --- test / debug accessors ---
public:
    float shipX() const { return shipX_; }
    float shipY() const { return shipY_; }
    int   bulletCount() const {
        int n = 0;
        for (auto& b : bullets_) if (b.alive) n++;
        return n;
    }
    int   bombCount() const {
        int n = 0;
        for (auto& b : bombs_) if (b.alive) n++;
        return n;
    }
    int   alienCount() const { return aliveAliens(); }
    long  score() const { return score_; }
    int   lives() const { return lives_; }
    int   level() const { return level_; }

#ifndef NDEBUG
    // Test-only helpers — excluded from release builds (NDEBUG defined).
    void triggerNewGameForTest()        { startGame(); }
    void startLevelForTest(int level)   { startLevel(level); }
    int   alienRowsForTest()      const { return rows_; }
    int   alienTotalForTest()     const { return (int)aliens_.size(); }
    bool  alienAliveForTest(int i) const { return aliens_[i].alive; }
    float alienXForTest(int i)    const { return alienX(aliens_[i]); }
    float alienYForTest(int i)    const { return alienY(aliens_[i]); }
    int   alienTypeForTest(int i) const { return (int)aliens_[i].type; }
    void  killAlienForTest(int i)       { aliens_[i].alive = false; }
    int   formationDirForTest()   const { return marchDir_; }
    float formationYForTest()     const { return formationY_; }
    void  setFormationYForTest(float y) { formationY_ = y; }
    void  setFormationXForTest(float x) { formationX_ = x; }
    float marchSpeedForTest()     const { return marchSpeed(); }
    void  setShipXForTest(float x)      { shipX_ = x; }
    void  clearInvulnForTest()          { invuln_ = 0.0f; }
    void  spawnTestBomb(float x, float y, float vy) {
        bombs_.push_back({x, y, 0.0f, vy, 0.0f, true});
    }
    void  spawnTestPowerUp(float x, float y, int type) {
        powerUps_.push_back({x, y, 0.30f, 0.0f, 1.5f, (PowerUpType)type, true});
    }
    void  activatePowerUpForTest(int type) { applyPowerUp((PowerUpType)type); }
    bool  shieldActiveForTest()   const { return shieldActive_; }
    bool  rapidActiveForTest()    const { return rapidActive_; }
    bool  tripleActiveForTest()   const { return tripleActive_; }
    float bulletVxForTest(int i)  const { return bullets_[i].vx; }
    bool  bossActiveForTest()     const { return bossActive_; }
    bool  bossAliveForTest()      const { return boss_.alive; }
    int   bossHpForTest()         const { return boss_.hp; }
    float bossXForTest()          const { return boss_.x; }
    void  setBossTimeForTest(float t)   { boss_.t = t; }
    bool  saucerAliveForTest()    const { return saucer_.alive; }
    float saucerXForTest()        const { return saucer_.x; }
    void  spawnSaucerForTest()          { spawnSaucer(); }
    void  setSaucerForTest(float x, float vx) {
        saucer_.x = x; saucer_.y = -0.80f; saucer_.vx = vx; saucer_.alive = true;
    }
    long  highScoreForTest(int i) const { return highScores_[i].score; }
    bool  inTitleForTest()        const { return state_ == TITLE; }
    bool  inSettingsForTest()     const { return state_ == SETTINGS; }
    bool  isPlayingForTest()      const { return state_ == PLAYING; }
    bool  isLevelClearForTest()   const { return state_ == LEVEL_CLEAR; }
    bool  isGameOverForTest()     const { return state_ == GAME_OVER; }
    bool  isWinForTest()          const { return state_ == WIN; }
    bool  soundEnabledForTest()   const { return soundEnabled_; }
    bool  autoPlayActiveForTest() const { return autoPlayActive_; }
    int   controlModeForTest()    const { return (int)controlMode_; }
    bool  glassesActiveForTest()  const { return glassesActive_; }
    void  setAutoPlayForTest(bool v)    { autoPlayActive_ = v; }
    float aiTargetXForTest()      const { return aiTargetX_; }
    bool  aiFireForTest()         const { return aiFire_; }
    bool  aiHasTargetForTest()    const { return aiMove_; }
#endif

private:
    // --- viewport ---
    int vw_ = 1, vh_ = 1;
    float asp_ = 0.5f;

    // --- input ---
    static const int kMaxPointers = 16;
    Pointer pointers_[kMaxPointers] = {};
    bool  tapPending_ = false;
    float tapX_       = 0.0f;
    float tapY_       = 0.0f;

    // --- state ---
    State state_     = TITLE;
    State prevState_ = TITLE;
    float animTime_  = 0.0f;
    float stateTimer_ = 0.0f;

    // --- ship (fixed altitude, steers horizontally) ---
    float shipX_ = 0.0f;
    const float shipY_ = 0.58f;
    float shipTilt_ = 0.0f;
    float fireCooldown_ = 0.0f;
    const float shipScale_ = 0.065f;
    const float shipR_ = 0.050f;
    const float shipSpeed_ = 1.6f;
    float invuln_ = 0.0f;

    // --- invader formation ---
    std::vector<Alien> aliens_;
    int   rows_       = 3;
    float formationX_ = 0.0f;   // centre offset of the wave
    float formationY_ = -0.66f; // y of the TOP row
    int   marchDir_   = 1;      // +1 right, -1 left
    int   marchFrame_ = 0;      // 0/1, flips on each beat (sprite animation)
    float beatTimer_  = 0.0f;
    int   beatStep_   = 0;      // cycles 0..3 through the march bass notes

    // --- bombs (alien lasers) ---
    std::vector<Bomb> bombs_;
    float bombTimer_ = 1.2f;

    // --- saucer ---
    Saucer saucer_;
    float saucerTimer_ = 14.0f;

    // --- boss (level 10) ---
    Boss boss_;
    bool bossActive_ = false;
    float bossBombTimer_ = 0.0f;

    // --- screen shake ---
    float shakeAmt_ = 0.0f;
    float shakeX_   = 0.0f;
    float shakeY_   = 0.0f;

    // --- power-ups ---
    bool  shieldActive_ = false;         // absorbs one hit
    bool  rapidActive_  = false;
    float rapidTimer_   = 0.0f;
    bool  tripleActive_ = false;         // 3-way laser volley (±8°)
    float tripleTimer_  = 0.0f;
    static constexpr float kRapidDuration = 8.0f;

    // --- progression / scoring ---
    int level_ = 1;
    int lives_ = 3;
    long score_ = 0;
    long bonusFlash_ = 0;        // last bonus value, shown briefly where earned
    float bonusFlashTimer_ = 0.0f;
    float bonusFlashX_ = 0.0f, bonusFlashY_ = 0.0f;

    // --- settings (persisted) ---
    bool soundEnabled_   = true;
    bool autoPlayActive_ = false;
    char settingsPath_[512] = {};

    // --- control mode / glasses session (not persisted) ---
    ControlMode controlMode_ = CONTROL_STRIP;
    bool glassesConnected_ = false;
    bool glassesActive_    = false;
    std::function<bool()> glassesLaunch_;
    std::function<void()> glassesExit_;

    // --- auto-play AI (drives the same control path as a finger) ---
    bool  aiMove_    = false;
    float aiTargetX_ = 0.0f;
    bool  aiFire_    = false;

    std::vector<Bullet>    bullets_;
    std::vector<Particle>  particles_;
    std::vector<Explosion> explosions_;
    std::vector<PowerUp>   powerUps_;
    std::vector<Star>      stars_;
    std::vector<Star>      starsNear_;  // fast parallax layer
    uint32_t rng_ = 0x1234567u;
};
