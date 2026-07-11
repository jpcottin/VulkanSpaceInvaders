#include "game.h"
#include "audio.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// ---- level tuning (1..10, easy -> hard) ----
static int clampLevel(int L) { return L < 1 ? 1 : (L > 10 ? 10 : L); }
// Level 1 starts with 3 rows of 8; a row is added every other level, capped at 5.
static int rowsForLevel(int L) {
    int r = 3 + (clampLevel(L) - 1) / 2;
    return r > 5 ? 5 : r;
}
static float levelMarchSpeed(int L) { return 0.09f + 0.020f * (clampLevel(L) - 1); }
static float levelBombInterval(int L) {
    float s = 1.15f - 0.07f * (clampLevel(L) - 1);
    return s < 0.40f ? 0.40f : s;
}
static int levelMaxBombs(int L) {
    int n = 2 + (clampLevel(L) - 1) / 3;
    return n > 5 ? 5 : n;
}
static float levelBombSpeed(int L)  { return 0.50f + 0.05f * (clampLevel(L) - 1); }
// Higher levels start the wave a little lower — less room before the invasion.
static float levelStartY(int L) {
    int c = clampLevel(L) - 1;
    return -0.66f + 0.03f * (c > 6 ? 6 : c);
}

static const float kStarSpeed     = 0.18f;
static const float kBulletSpeed   = 2.40f;   // player laser, flies up (-y)
static const float kFireCooldown  = 0.42f;
static const float kRapidCooldown = 0.20f;
static const int   kMaxPlayerBullets = 3;

// Invader formation metrics (world units).
static const float kColStep  = 0.088f;  // horizontal slot pitch
static const float kRowStep  = 0.085f;  // vertical slot pitch
static const float kAlienHW  = 0.038f;  // alien half-width
static const float kAlienHH  = 0.030f;  // alien half-height
static const float kDescend  = 0.040f;  // drop per edge bounce
static const float kSideMargin = 0.015f;

// The invasion succeeds (instant game over) once an alien's bottom edge reaches
// the control strip. Aliens at the ship's altitude (0.58) collide first and cost
// a life each, so a wave has to fight its way down past the player.
static const float kInvadeY = 0.70f;

// Control strip: the bottom 15% of the screen, below the ship. A finger there
// steers the ship toward the finger x and holds the trigger.
static const float kCtrlZoneFrac = 0.85f;  // pixel y > vh * this => control zone
static const float kCtrlZoneTopW = 0.70f;  // same boundary in world y

// Per-tier kill points (x level): squid (top), crab (middle), octopus (bottom).
static const int kAlienScore[3] = {30, 20, 10};
static const int kSaucerScore   = 100;
static const int kPowerUpScore  = 25;
static const int kLevelClearScore = 100;
static const int kBossScore     = 250;   // x level (2500 at level 10)

// Level-10 boss mothership: drifts sinusoidally across the top, above its
// two-row escort wave, lobbing bombs aimed at the player.
static const int   kBossHP      = 16;
static const float kBossY       = -0.80f;
static const float kBossHW      = 0.150f;  // half-width
static const float kBossHH      = 0.072f;  // half-height
static const float kBossBombInterval = 1.15f;

// Triple-shot side lasers leave at ±8° off vertical.
static const float kTripleAngle = 8.0f * 3.14159265f / 180.0f;

// 7-segment masks for digits 0..9 (bit a=1,b=2,c=4,d=8,e=16,f=32,g=64).
static const int kDigitSeg[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};


Game::Game() { buildFormation(); }   // title screen shows a demo wave

float Game::frand() {
    uint32_t x = rng_;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_ = x;
    return (x & 0xFFFFFF) / float(0x1000000);
}
float Game::frange(float a, float b) { return a + (b - a) * frand(); }

void Game::setViewport(int w, int h) {
    if (w <= 0 || h <= 0) return;
    vw_ = w; vh_ = h;
    asp_ = (float)w / (float)h;
    if (stars_.empty()) {
        for (int i = 0; i < 60; i++) {
            Star s;
            s.x = frange(-1.0f, 1.0f);
            s.y = frange(-1.0f, 1.0f);
            s.size = frange(0.004f, 0.011f);
            s.phase = frange(0.0f, 6.2832f);
            stars_.push_back(s);
        }
        for (int i = 0; i < 20; i++) {
            Star s;
            s.x = frange(-1.0f, 1.0f);
            s.y = frange(-1.0f, 1.0f);
            s.size = frange(0.007f, 0.016f);
            s.phase = frange(0.0f, 6.2832f);
            starsNear_.push_back(s);
        }
    }
    float lim = asp_ - shipScale_;
    if (shipX_ > lim) shipX_ = lim;
    if (shipX_ < -lim) shipX_ = -lim;
}

// --- input helpers ---
// The control zone is the bottom strip below the ship. Any finger there both
// steers the ship (toward the finger x) and holds the fire trigger. Auto-play
// feeds the same path through aiMove_/aiTargetX_/aiFire_.

// In CONTROL_TOUCHBAR mode (AI glasses) the whole surface is the control bar;
// in CONTROL_STRIP mode only the bottom strip counts.
bool Game::moveHeld() const {
    for (auto& p : pointers_)
        if (p.active && (controlMode_ == CONTROL_TOUCHBAR || p.y > vh_ * kCtrlZoneFrac))
            return true;
    return autoPlayActive_ && aiMove_;
}

float Game::controlTargetX() const {
    for (auto& p : pointers_)
        if (p.active && (controlMode_ == CONTROL_TOUCHBAR || p.y > vh_ * kCtrlZoneFrac))
            return (2.0f * p.x / vw_ - 1.0f) * asp_;
    return aiTargetX_;   // only reached when auto-play set aiMove_
}

bool Game::fireHeld() const {
    if (autoPlayActive_ && aiFire_) return true;
    for (auto& p : pointers_)
        if (p.active && (controlMode_ == CONTROL_TOUCHBAR || p.y > vh_ * kCtrlZoneFrac))
            return true;
    return false;
}

// Entering/leaving a glasses session: the phone instance freezes its own
// gameplay and hands the speakers to the glasses instance.
void Game::setGlassesActive(bool v) {
    if (v == glassesActive_) return;
    glassesActive_ = v;
    if (audio_) {
        if (v) {
            audio_->setSaucer(false);
            audio_->setMusicEnabled(false);
        } else if (state_ == PLAYING || state_ == LEVEL_CLEAR) {
            audio_->setMusicEnabled(soundEnabled_);
        }
    }
}

void Game::onPointerDown(int id, float x, float y) {
    if (id >= 0 && id < kMaxPointers) {
        pointers_[id].active = true;
        pointers_[id].x = x;
        pointers_[id].y = y;
    }
    if (!tapPending_) { tapX_ = x; tapY_ = y; }  // first finger wins
    tapPending_ = true;
}
void Game::onPointerMove(int id, float x, float y) {
    if (id >= 0 && id < kMaxPointers && pointers_[id].active) {
        pointers_[id].x = x;
        pointers_[id].y = y;
    }
}
void Game::onPointerUp(int id) {
    if (id >= 0 && id < kMaxPointers) pointers_[id].active = false;
}
void Game::onPointersCancel() {
    for (auto& p : pointers_) p.active = false;
}

// ── High score persistence ────────────────────────────────────────────────────

static const uint32_t kHsMagic = 0x53494E56u; // "SINV"

void Game::setDataPath(const char* path) {
    if (!path || path[0] == '\0') return;
    snprintf(dataPath_,    sizeof(dataPath_),    "%s/highscores.bin", path);
    snprintf(settingsPath_, sizeof(settingsPath_), "%s/settings.bin",   path);
    loadHighScores();
    loadSettings();
}

void Game::loadHighScores() {
    if (dataPath_[0] == '\0') return;
    FILE* f = fopen(dataPath_, "rb");
    if (!f) return;
    struct { uint32_t magic, count; struct { int64_t score; int32_t level; } e[kMaxScores]; } buf;
    if (fread(&buf, sizeof(buf), 1, f) == 1 && buf.magic == kHsMagic) {
        int n = (int)buf.count < kMaxScores ? (int)buf.count : kMaxScores;
        for (int i = 0; i < n; i++) {
            highScores_[i].score = (long)buf.e[i].score;
            highScores_[i].level = buf.e[i].level;
        }
    }
    fclose(f);
}

// Insert a score at its rank unless the identical entry is already present.
// Exact-duplicate skipping matters because the phone and glasses instances
// load the same file: without it every merge would double the shared rows.
void Game::mergeHighScore(long score, int level) {
    if (score <= 0) return;
    for (int i = 0; i < kMaxScores; i++)
        if (highScores_[i].score == score && highScores_[i].level == level) return;
    int pos = kMaxScores;
    for (int i = 0; i < kMaxScores; i++)
        if (score > highScores_[i].score) { pos = i; break; }
    if (pos == kMaxScores) return;
    for (int i = kMaxScores - 1; i > pos; i--) highScores_[i] = highScores_[i - 1];
    highScores_[pos] = {score, level};
}

void Game::saveHighScores() {
    if (dataPath_[0] == '\0') return;
    // The other instance (phone vs glasses, same process, separate Game
    // objects) may have saved since we loaded: merge the disk table into
    // ours first so a stale in-memory copy never clobbers a saved score.
    FILE* rf = fopen(dataPath_, "rb");
    if (rf) {
        struct { uint32_t magic, count; struct { int64_t score; int32_t level; } e[kMaxScores]; } in;
        if (fread(&in, sizeof(in), 1, rf) == 1 && in.magic == kHsMagic) {
            int n = (int)in.count < kMaxScores ? (int)in.count : kMaxScores;
            for (int i = 0; i < n; i++)
                mergeHighScore((long)in.e[i].score, in.e[i].level);
        }
        fclose(rf);
    }

    FILE* f = fopen(dataPath_, "wb");
    if (!f) return;
    struct { uint32_t magic, count; struct { int64_t score; int32_t level; } e[kMaxScores]; } buf{};
    buf.magic = kHsMagic;
    buf.count = kMaxScores;
    for (int i = 0; i < kMaxScores; i++) {
        buf.e[i].score = (int64_t)highScores_[i].score;
        buf.e[i].level = highScores_[i].level;
    }
    fwrite(&buf, sizeof(buf), 1, f);
    fclose(f);
}

// Re-read persisted state. The phone and glasses instances share the files;
// whichever regains focus adopts what the other saved while it was away.
void Game::reloadFromDisk() {
    loadHighScores();
    loadSettings();
}

// Resume a run after process death: same level, score, and lives, but a
// fresh wave — entity positions aren't persisted. Bounds-checked because the
// saved blob comes back from the OS, not from us.
void Game::restoreSession(int level, long score, int lives) {
    if (level < 1 || level > 10 || lives < 1 || lives > 3 || score < 0) return;
    startGame();
    if (level > 1) startLevel(level);
    score_ = score;
    lives_ = lives;
}

static const uint32_t kSettingsMagic = 0x53455454u;  // "SETT"

// Gear icon world position — single source of truth for both rendering and hit-testing.
static constexpr float kGearOffsetX = 0.062f;   // distance from right edge
static constexpr float kGearWY      = -0.82f;   // y in world space (top area)

// Settings row y-positions — shared between drawSettingsScreen() and the tap handler.
static constexpr float kSettingSoundY    = -0.15f;
static constexpr float kSettingAutoPlayY =  0.10f;
static constexpr float kSettingGlassesY  =  0.35f;
static constexpr float kSettingBackY     =  0.66f;

void Game::loadSettings() {
    if (settingsPath_[0] == '\0') return;
    FILE* f = fopen(settingsPath_, "rb");
    if (!f) return;
    struct { uint32_t magic; int32_t soundOn; int32_t autoPlay; } buf = {};
    size_t n = fread(&buf, 1, sizeof(buf), f);
    fclose(f);
    if (n >= 8 && buf.magic == kSettingsMagic) {
        soundEnabled_ = (buf.soundOn != 0);
        if (n >= 12) autoPlayActive_ = (buf.autoPlay != 0);
    }
}

void Game::saveSettings() {
    if (settingsPath_[0] == '\0') return;
    FILE* f = fopen(settingsPath_, "wb");
    if (!f) return;
    struct { uint32_t magic; int32_t soundOn; int32_t autoPlay; } buf = {
        kSettingsMagic, soundEnabled_ ? 1 : 0, autoPlayActive_ ? 1 : 0
    };
    fwrite(&buf, sizeof(buf), 1, f);
    fclose(f);
}

bool Game::isGearTap(float px, float py) const {
    if (controlMode_ == CONTROL_TOUCHBAR) return false;  // no gear on the glasses
    float wy = 2.0f * py / vh_ - 1.0f;
    float wx = (2.0f * px / vw_ - 1.0f) * asp_;
    float dx = wx - (asp_ - kGearOffsetX), dy = wy - kGearWY;
    return dx*dx + dy*dy < 0.0081f;  // 0.09 world units radius
}

void Game::spawnDebris(float ax, float ay, float ar, float cr, float cg, float cb) {
    // Expanding flash ring
    Explosion e;
    e.x = ax; e.y = ay; e.radius = ar;
    e.t = 0.0f; e.maxLife = 0.22f;
    e.cr = cr; e.cg = cg; e.cb = cb;
    e.alive = true;
    explosions_.push_back(e);

    // 10 debris fragments flying outward
    for (int i = 0; i < 10; i++) {
        float angle = frange(0.0f, 6.2832f);
        float speed = frange(0.5f, 1.6f);
        Particle p;
        p.x = ax + cosf(angle) * ar * 0.4f;
        p.y = ay + sinf(angle) * ar * 0.4f;
        p.vx = cosf(angle) * speed;
        p.vy = sinf(angle) * speed;
        p.rot  = frange(0.0f, 6.2832f);
        p.spin = frange(-5.0f, 5.0f);
        p.t       = 0.0f;
        p.maxLife = frange(0.25f, 0.55f);
        p.size    = ar * frange(0.20f, 0.45f);
        p.r = cr + frange(-0.08f, 0.08f);
        p.g = cg + frange(-0.08f, 0.08f);
        p.b = cb + frange(-0.08f, 0.08f);
        p.alive = true;
        particles_.push_back(p);
    }
}

void Game::checkHighScore() {
    if (score_ <= 0) return;
    int pos = kMaxScores;
    for (int i = 0; i < kMaxScores; i++) {
        if (score_ > highScores_[i].score) { pos = i; break; }
    }
    if (pos == kMaxScores) return;
    for (int i = kMaxScores - 1; i > pos; i--) highScores_[i] = highScores_[i - 1];
    highScores_[pos] = {score_, level_};
    newHighScore_     = true;
    newHighScoreRank_ = pos;
    saveHighScores();
}

// ── formation ────────────────────────────────────────────────────────────────

// Row tier: top row is the squid, the bottom row(s) the octopus, crabs between.
static Game::AlienType rowType(int row, int rows) {
    int octoRows = rows >= 5 ? 2 : 1;
    if (row == 0) return Game::ALIEN_SQUID;
    if (row >= rows - octoRows) return Game::ALIEN_OCTOPUS;
    return Game::ALIEN_CRAB;
}

void Game::buildFormation() {
    // Level 10 is the boss fight: the mothership only brings a two-row escort.
    rows_ = (level_ == 10) ? 2 : rowsForLevel(level_);
    aliens_.clear();
    aliens_.reserve((size_t)rows_ * kCols);
    for (int r = 0; r < rows_; r++)
        for (int c = 0; c < kCols; c++)
            aliens_.push_back({r, c, rowType(r, rows_), true});
    formationX_ = 0.0f;
    formationY_ = levelStartY(level_);
    marchDir_   = 1;
    marchFrame_ = 0;
    beatTimer_  = 0.0f;
    beatStep_   = 0;
}

float Game::alienX(const Alien& a) const {
    return formationX_ + (a.col - (kCols - 1) * 0.5f) * kColStep;
}
float Game::alienY(const Alien& a) const {
    return formationY_ + a.row * kRowStep;
}

int Game::aliveAliens() const {
    int n = 0;
    for (auto& a : aliens_) if (a.alive) n++;
    return n;
}

// The fewer invaders remain, the faster the survivors march (classic).
float Game::marchSpeed() const {
    int total = (int)aliens_.size();
    if (total == 0) return levelMarchSpeed(level_);
    float aliveFrac = (float)aliveAliens() / (float)total;
    return levelMarchSpeed(level_) * (1.0f + 2.5f * (1.0f - aliveFrac));
}

void Game::startGame() {
    score_ = 0;
    lives_ = 3;
    newHighScore_     = false;
    newHighScoreRank_ = -1;
    shieldActive_ = false;
    rapidActive_  = false; rapidTimer_  = 0.0f;
    tripleActive_ = false; tripleTimer_ = 0.0f;
    bonusFlashTimer_ = 0.0f;
    level_ = 1;
    if (audio_) audio_->setMusicEnabled(soundEnabled_);
    startLevel(1);
}

void Game::startLevel(int level) {
    level_ = clampLevel(level);
    buildFormation();
    bullets_.clear();
    bombs_.clear();
    particles_.clear();
    explosions_.clear();
    powerUps_.clear();
    saucer_ = {};
    saucerTimer_ = frange(10.0f, 16.0f);
    bombTimer_   = 1.5f;
    boss_ = {};
    bossActive_ = (level_ == 10);
    if (bossActive_) {
        boss_.hp = boss_.maxHp = kBossHP;
        boss_.alive = true;
    }
    bossBombTimer_ = 2.0f;
    shipX_ = 0.0f;
    shipTilt_ = 0.0f;
    fireCooldown_ = 0.0f;
    invuln_ = 1.2f;
    shakeAmt_ = 0.0f; shakeX_ = 0.0f; shakeY_ = 0.0f;
    if (audio_) audio_->setSaucer(false);
    state_ = PLAYING;
}

void Game::spawnPowerUp(float x, float y) {
    PowerUp p;
    p.x    = x;
    p.y    = y;
    p.vy   = 0.30f;
    p.rot  = 0.0f;
    p.spin = frange(-2.5f, 2.5f);
    float roll = frand();
    p.type  = roll < 0.34f ? PU_SHIELD : (roll < 0.67f ? PU_RAPID : PU_TRIPLE);
    p.alive = true;
    powerUps_.push_back(p);
}

void Game::spawnSaucer() {
    bool fromLeft = frand() < 0.5f;
    saucer_.x  = fromLeft ? -asp_ - 0.08f : asp_ + 0.08f;
    saucer_.y  = -0.80f;
    saucer_.vx = (fromLeft ? 1.0f : -1.0f) * 0.35f;
    saucer_.alive = true;
}

void Game::update(float dt) {
    if (dt > 0.05f) dt = 0.05f;   // clamp huge hitches
    animTime_ += dt;

    // Particles and explosions animate in all states.
    // Drag is exponential decay, normalized to a 60 Hz reference so debris
    // behaves the same at any refresh rate.
    float drag = powf(0.88f, dt * 60.0f);
    for (auto& p : particles_) {
        if (!p.alive) continue;
        p.x  += p.vx * dt; p.y  += p.vy * dt;
        p.vx *= drag;      p.vy *= drag;
        p.rot += p.spin * dt;
        p.t   += dt;
        if (p.t >= p.maxLife) p.alive = false;
    }
    for (size_t i = particles_.size(); i-- > 0;)
        if (!particles_[i].alive) particles_.erase(particles_.begin() + i);

    for (auto& e : explosions_) {
        if (!e.alive) continue;
        e.t += dt;
        if (e.t >= e.maxLife) e.alive = false;
    }
    for (size_t i = explosions_.size(); i-- > 0;)
        if (!explosions_[i].alive) explosions_.erase(explosions_.begin() + i);

    // Stars scroll in every state for a sense of motion.
    for (auto& s : stars_) {
        s.y += kStarSpeed * dt;
        if (s.y > 1.05f) { s.y = -1.05f; s.x = frange(-1.0f, 1.0f); }
    }
    for (auto& s : starsNear_) {
        s.y += kStarSpeed * 3.2f * dt;
        if (s.y > 1.05f) { s.y = -1.05f; s.x = frange(-1.0f, 1.0f); }
    }

    // Screen shake decay, 60 Hz-normalized like the particle drag above.
    if (shakeAmt_ > 0.0f) {
        shakeX_ = frange(-1.0f, 1.0f) * shakeAmt_ * 0.040f;
        shakeY_ = frange(-1.0f, 1.0f) * shakeAmt_ * 0.040f;
        shakeAmt_ *= powf(0.78f, dt * 60.0f);
        if (shakeAmt_ < 0.01f) { shakeAmt_ = 0.0f; shakeX_ = 0.0f; shakeY_ = 0.0f; }
    }

    if (bonusFlashTimer_ > 0.0f) bonusFlashTimer_ -= dt;

    bool tapped = tapPending_;
    tapPending_ = false;

    // Gear tap opens Settings from any state except Settings itself.
    if (tapped && state_ != SETTINGS && isGearTap(tapX_, tapY_)) {
        prevState_ = state_;
        if (audio_) audio_->setSaucer(false);
        state_ = SETTINGS;
        tapped = false;
    }

    switch (state_) {
        case TITLE: {
            if (tapped) { startGame(); break; }
            // Demo wave slowly marching under the title.
            formationX_ += marchDir_ * 0.06f * dt;
            if (formationX_ >  0.09f) marchDir_ = -1;
            if (formationX_ < -0.09f) marchDir_ =  1;
            beatTimer_ -= dt;
            if (beatTimer_ <= 0.0f) {
                beatTimer_ = 0.60f;
                marchFrame_ ^= 1;
            }
            break;
        }
        case PLAYING: {
            // While the game runs on the glasses, the phone instance freezes
            // (Settings stays reachable via the gear to bring it back).
            if (glassesActive_ && controlMode_ == CONTROL_STRIP) break;

            // Reset AI intents; updateAutoPlay sets them when autopilot is on.
            aiMove_ = false; aiFire_ = false;
            if (autoPlayActive_) updateAutoPlay(dt);

            // One frame of gameplay, in order. State may flip to GAME_OVER or
            // WIN mid-sequence (fatal hit, invasion, boss kill); once it does,
            // skip the remaining steps — checkHighScore() has already saved,
            // so nothing may fire, score, or collect after death.
            updateShip(dt);
            if (state_ == PLAYING) updateFormation(dt);
            if (state_ == PLAYING) updateBoss(dt);
            if (state_ == PLAYING) updateBombs(dt);
            if (state_ == PLAYING) updateBullets(dt);
            if (state_ == PLAYING) updateSaucer(dt);
            if (state_ == PLAYING) updatePowerUps(dt);
            removeDeadEntities();
            checkLevelClear();
            break;
        }
        case LEVEL_CLEAR: {
            stateTimer_ += dt;
            if (stateTimer_ >= 1.8f) {
                if (level_ >= 10) { state_ = WIN; stateTimer_ = 0.0f; checkHighScore(); }
                else startLevel(level_ + 1);
            }
            break;
        }
        case GAME_OVER:
        case WIN: {
            stateTimer_ += dt;
            if (tapped && stateTimer_ > 0.6f) {
                state_ = TITLE;
                level_ = 1;
                buildFormation();       // rebuild the title demo wave
                // Clear every battlefield leftover, or it renders frozen on
                // the title screen forever (nothing updates it there).
                bombs_.clear();
                bullets_.clear();
                powerUps_.clear();
                saucer_.alive = false;
                shieldActive_ = false;
                rapidActive_  = false;
                tripleActive_ = false;
                if (audio_) audio_->setSaucer(false);
                if (audio_) audio_->setMusicEnabled(false);
            }
            break;
        }
        case SETTINGS: {
            if (tapped) {
                float wy = 2.0f * tapY_ / vh_ - 1.0f;
                float wx = (2.0f * tapX_ / vw_ - 1.0f) * asp_;
                const float kHitH = 0.12f;
                // Rows respond only across the label-to-toggle span, not the
                // full screen width (avoids accidental edge taps).
                const float kHitW = asp_ * 0.66f;
                if (fabsf(wy - kSettingSoundY) < kHitH && fabsf(wx) < kHitW) {
                    soundEnabled_ = !soundEnabled_;
                    saveSettings();
                    if (!soundEnabled_) {
                        if (audio_) audio_->setMusicEnabled(false);
                    } else if (prevState_ == PLAYING || prevState_ == LEVEL_CLEAR) {
                        if (audio_) audio_->setMusicEnabled(true);
                    }
                } else if (fabsf(wy - kSettingAutoPlayY) < kHitH && fabsf(wx) < kHitW) {
                    autoPlayActive_ = !autoPlayActive_;
                    saveSettings();
                } else if (fabsf(wy - kSettingGlassesY) < kHitH && fabsf(wx) < kHitW) {
                    if (glassesActive_) {
                        if (glassesExit_) glassesExit_();
                    } else if (glassesConnected_) {
                        if (glassesLaunch_) glassesLaunch_();
                    }
                    // Not connected: the row is informational only.
                } else if (fabsf(wy - kSettingBackY) < kHitH && fabsf(wx) < 0.15f) {
                    onPointersCancel();
                    state_ = prevState_;
                }
            }
            break;
        }
    }
}

// ---- PLAYING-state update steps ----

// Shield absorbs the hit; otherwise lose a life, flash, shake, and possibly end
// the game. Shared by bomb hits and alien collisions.
void Game::applyShipHit() {
    if (shieldActive_) {
        shieldActive_ = false;
        shakeAmt_ = 0.5f;
        // Brief grace so two overlapping hits in one frame (adjacent aliens,
        // alien + bomb) can't consume the shield and a life together.
        invuln_ = 1.0f;
        return;
    }
    lives_--;
    invuln_ = 1.5f;
    shakeAmt_ = 1.0f;
    if (audio_ && soundEnabled_) audio_->triggerPlayerHit();
    if (haptic_) haptic_();
    Explosion flash;
    flash.x = shipX_; flash.y = shipY_; flash.radius = shipScale_;
    flash.t = 0.0f; flash.maxLife = 0.18f;
    flash.cr = 0.45f; flash.cg = 0.9f; flash.cb = 1.0f;
    flash.alive = true;
    explosions_.push_back(flash);
    if (lives_ <= 0) {
        if (audio_) audio_->setSaucer(false);
        state_ = GAME_OVER; stateTimer_ = 0.0f;
        checkHighScore();
    }
}

// The wave reached the ship's line: the invasion succeeded, game over
// regardless of remaining lives (classic rule).
void Game::triggerInvasion() {
    lives_ = 0;
    shakeAmt_ = 1.0f;
    spawnDebris(shipX_, shipY_, shipScale_ * 1.5f, 0.28f, 0.72f, 0.92f);
    if (audio_ && soundEnabled_) audio_->triggerPlayerHit();
    if (haptic_) haptic_();
    if (audio_) audio_->setSaucer(false);
    state_ = GAME_OVER; stateTimer_ = 0.0f;
    checkHighScore();
}

void Game::updateShip(float dt) {
    int dir = 0;
    if (moveHeld()) {
        float target = controlTargetX();
        float dx = target - shipX_;
        const float deadband = 0.012f;
        if (fabsf(dx) > deadband) {
            dir = dx > 0 ? 1 : -1;
            float step = shipSpeed_ * dt;
            if (step > fabsf(dx)) step = fabsf(dx);   // land exactly on the finger
            shipX_ += dir * step;
        }
    }
    float lim = asp_ - shipScale_;
    if (shipX_ > lim) shipX_ = lim;
    if (shipX_ < -lim) shipX_ = -lim;
    // Smoothly lean into direction of travel (±20°)
    float tiltTarget = dir * 0.35f;
    shipTilt_ += (tiltTarget - shipTilt_) * 9.0f * dt;

    if (invuln_ > 0.0f) invuln_ -= dt;
}

void Game::updateFormation(float dt) {
    if (aliveAliens() == 0) return;

    // March sideways; speed rises as the wave thins out.
    formationX_ += marchDir_ * marchSpeed() * dt;

    // Edge bounce: reverse and step down when the outermost alive alien
    // touches a side margin.
    float minX = 1e9f, maxX = -1e9f;
    for (auto& a : aliens_) {
        if (!a.alive) continue;
        float x = alienX(a);
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
    }
    if (marchDir_ > 0 && maxX + kAlienHW >= asp_ - kSideMargin) {
        marchDir_ = -1;
        formationY_ += kDescend;
    } else if (marchDir_ < 0 && minX - kAlienHW <= -asp_ + kSideMargin) {
        marchDir_ = 1;
        formationY_ += kDescend;
    }

    // March beat: sprite animation + the four-note bass loop, faster as the
    // wave thins (interval tracks the fraction still alive).
    float aliveFrac = (float)aliveAliens() / (float)aliens_.size();
    beatTimer_ -= dt;
    if (beatTimer_ <= 0.0f) {
        beatTimer_ = 0.14f + 0.75f * aliveFrac;
        marchFrame_ ^= 1;
        beatStep_ = (beatStep_ + 1) & 3;
        if (audio_ && soundEnabled_) audio_->triggerMarch(beatStep_);
    }

    // Alien vs ship collision, and the invasion line.
    for (auto& a : aliens_) {
        if (!a.alive) continue;
        float ax = alienX(a), ay = alienY(a);
        if (ay + kAlienHH >= kInvadeY) {
            triggerInvasion();
            return;
        }
        if (invuln_ <= 0.0f &&
            fabsf(ax - shipX_) < kAlienHW + shipR_ * 0.8f &&
            fabsf(ay - shipY_) < kAlienHH + shipR_ * 0.8f) {
            killAlien(a);
            applyShipHit();
            if (state_ != PLAYING) return;
        }
    }
}

void Game::updateBoss(float dt) {
    if (!bossActive_ || !boss_.alive) return;
    boss_.t += dt;
    boss_.x  = sinf(boss_.t * 0.60f) * (asp_ - kBossHW - 0.05f);

    // Aimed bombs: lobbed toward where the player currently is.
    bossBombTimer_ -= dt;
    if (bossBombTimer_ <= 0.0f) {
        bossBombTimer_ = kBossBombInterval * frange(0.8f, 1.2f);
        Bomb b;
        b.x = boss_.x;
        b.y = kBossY + kBossHH + 0.02f;
        b.vy = levelBombSpeed(level_) * 1.25f;
        float dx = shipX_ - boss_.x;
        float tReach = (shipY_ - b.y) / b.vy;
        float vx = dx / (tReach > 0.3f ? tReach : 0.3f);
        // Cap the lateral speed so bombs stay dodgeable.
        if (vx >  0.45f) vx =  0.45f;
        if (vx < -0.45f) vx = -0.45f;
        b.vx = vx;
        b.wobble = frange(0.0f, 6.28f);
        b.alive = true;
        bombs_.push_back(b);
    }
}

void Game::dropBomb() {
    // Bottom-most alive alien of a random occupied column fires (classic).
    int cols[kCols], nCols = 0;
    for (int c = 0; c < kCols; c++) {
        for (auto& a : aliens_)
            if (a.alive && a.col == c) { cols[nCols++] = c; break; }
    }
    if (nCols == 0) return;
    int col = cols[(int)(frand() * nCols) % nCols];
    const Alien* shooter = nullptr;
    for (auto& a : aliens_)
        if (a.alive && a.col == col && (!shooter || a.row > shooter->row))
            shooter = &a;
    if (!shooter) return;
    Bomb b;
    b.x = alienX(*shooter);
    b.y = alienY(*shooter) + kAlienHH + 0.01f;
    b.vx = 0.0f;
    b.vy = levelBombSpeed(level_) * frange(0.9f, 1.1f);
    b.wobble = frange(0.0f, 6.28f);
    b.alive = true;
    bombs_.push_back(b);
}

void Game::updateBombs(float dt) {
    bombTimer_ -= dt;
    if (bombTimer_ <= 0.0f) {
        if (bombCount() < levelMaxBombs(level_) && aliveAliens() > 0) dropBomb();
        bombTimer_ = levelBombInterval(level_) * frange(0.6f, 1.4f);
    }

    for (auto& b : bombs_) {
        if (!b.alive) continue;
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        b.wobble += 6.0f * dt;
        if (b.y > 1.05f || b.x > asp_ + 0.1f || b.x < -asp_ - 0.1f) {
            b.alive = false; continue;
        }
        if (invuln_ <= 0.0f && state_ == PLAYING) {
            float dx = b.x - shipX_, dy = b.y - shipY_;
            float rad = shipR_ + 0.012f;
            if (dx*dx + dy*dy < rad * rad) {
                b.alive = false;
                applyShipHit();
            }
        }
    }
}

// Grant a power-up's effect. Shared by the pickup path and the test hook so
// tests exercise the production timers, not a copy of them.
void Game::applyPowerUp(PowerUpType t) {
    if (t == PU_SHIELD)      shieldActive_ = true;
    else if (t == PU_RAPID)  { rapidActive_  = true; rapidTimer_  = kRapidDuration; }
    else                     { tripleActive_ = true; tripleTimer_ = kRapidDuration; }
}

// Kill an alien: score by row tier, debris, possible power-up drop.
void Game::killAlien(Alien& a) {
    a.alive = false;
    float ax = alienX(a), ay = alienY(a);
    static const float kTierCol[3][3] = {
        {0.78f, 0.45f, 1.00f},   // squid: purple
        {0.30f, 0.95f, 0.55f},   // crab: green
        {1.00f, 0.70f, 0.20f},   // octopus: amber
    };
    const float* c = kTierCol[a.type];
    spawnDebris(ax, ay, kAlienHW, c[0], c[1], c[2]);
    score_ += (long)kAlienScore[a.type] * level_;
    if (audio_ && soundEnabled_) audio_->triggerExplosion();
    if (frand() < 0.08f) spawnPowerUp(ax, ay);
}

void Game::updateBullets(float dt) {
    // Fire while the trigger is held; rapid power-up shortens the cooldown and
    // the triple power-up adds two side lasers at ±8°.
    if (fireCooldown_ > 0.0f) fireCooldown_ -= dt;
    if (fireHeld() && fireCooldown_ <= 0.0f && bulletCount() < kMaxPlayerBullets) {
        auto fireLaser = [&](float angle) {
            Bullet b;
            b.x = shipX_;
            b.y = shipY_ - shipScale_ * 1.1f;
            b.vx =  kBulletSpeed * sinf(angle);
            b.vy = -kBulletSpeed * cosf(angle);
            b.alive = true;
            bullets_.push_back(b);
        };
        fireLaser(0.0f);
        if (tripleActive_) {
            fireLaser(-kTripleAngle);
            fireLaser( kTripleAngle);
        }
        fireCooldown_ = rapidActive_ ? kRapidCooldown : kFireCooldown;
        if (audio_ && soundEnabled_) audio_->triggerLaser();
    }

    for (auto& b : bullets_) {
        if (!b.alive) continue;
        // Substep the move so a fast bullet can't tunnel: a full 2.4-speed
        // step is 0.04 at 60 fps — already most of a bomb's 0.06 collision
        // diameter — and 0.12 at the clamped dt, taller than an alien's whole
        // hit box. Collisions are checked at every substep position.
        const float kMaxStep = 0.02f;
        int steps = (int)((fabsf(b.vx) + fabsf(b.vy)) * dt / kMaxStep) + 1;
        float sdt = dt / (float)steps;
        for (int s = 0; s < steps && b.alive; s++) {
            b.x += b.vx * sdt;
            b.y += b.vy * sdt;
            if (b.y < -1.05f || b.x > asp_ + 0.1f || b.x < -asp_ - 0.1f) {
                b.alive = false; continue;
            }

            // Bullet vs bomb: shooting down an incoming bomb cancels both.
            for (auto& bomb : bombs_) {
                if (!bomb.alive) continue;
                float dx = b.x - bomb.x, dy = b.y - bomb.y;
                if (dx*dx + dy*dy < 0.030f * 0.030f) {
                    b.alive = false;
                    bomb.alive = false;
                    Explosion e;
                    e.x = bomb.x; e.y = bomb.y; e.radius = 0.020f;
                    e.t = 0.0f; e.maxLife = 0.12f;
                    e.cr = 1.0f; e.cg = 0.6f; e.cb = 0.2f; e.alive = true;
                    explosions_.push_back(e);
                    break;
                }
            }
            if (!b.alive) continue;

            // Bullet vs boss mothership
            if (bossActive_ && boss_.alive &&
                fabsf(b.x - boss_.x) < kBossHW + 0.010f &&
                fabsf(b.y - kBossY) < kBossHH + 0.016f) {
                b.alive = false;
                boss_.hp--;
                Explosion hitFlash;
                hitFlash.x = b.x; hitFlash.y = kBossY + kBossHH;
                hitFlash.radius = 0.035f;
                hitFlash.t = 0.0f; hitFlash.maxLife = 0.12f;
                hitFlash.cr = 1.0f; hitFlash.cg = 0.6f; hitFlash.cb = 0.1f;
                hitFlash.alive = true;
                explosions_.push_back(hitFlash);
                if (boss_.hp <= 0) {
                    boss_.alive = false;
                    long bonus = (long)kBossScore * level_;
                    score_ += bonus;
                    bonusFlash_ = bonus;
                    bonusFlashTimer_ = 0.9f;
                    bonusFlashX_ = boss_.x; bonusFlashY_ = kBossY;
                    spawnDebris(boss_.x, kBossY, kBossHW,        1.00f, 0.35f, 0.55f);
                    spawnDebris(boss_.x, kBossY, kBossHW * 0.6f, 0.85f, 0.25f, 0.75f);
                    if (audio_) audio_->setSaucer(false);
                    if (audio_ && soundEnabled_) {
                        audio_->triggerExplosion();
                        audio_->triggerLevelClear();
                    }
                    state_ = WIN; stateTimer_ = 0.0f;
                    checkHighScore();
                }
                continue;
            }

            // Bullet vs saucer
            if (saucer_.alive &&
                fabsf(b.x - saucer_.x) < 0.055f && fabsf(b.y - saucer_.y) < 0.035f) {
                b.alive = false;
                saucer_.alive = false;
                long bonus = (long)kSaucerScore * level_;
                score_ += bonus;
                bonusFlash_ = bonus;
                bonusFlashTimer_ = 0.9f;
                bonusFlashX_ = saucer_.x; bonusFlashY_ = saucer_.y;
                spawnDebris(saucer_.x, saucer_.y, 0.05f, 1.0f, 0.35f, 0.30f);
                if (audio_) audio_->setSaucer(false);
                if (audio_ && soundEnabled_) audio_->triggerExplosion();
                continue;
            }

            // Bullet vs invaders (box test — the sprites are wide and flat).
            for (auto& a : aliens_) {
                if (!a.alive) continue;
                float dx = b.x - alienX(a), dy = b.y - alienY(a);
                if (fabsf(dx) < kAlienHW + 0.010f && fabsf(dy) < kAlienHH + 0.016f) {
                    b.alive = false;
                    killAlien(a);
                    break;
                }
            }
        }  // substeps
    }
}

void Game::updateSaucer(float dt) {
    if (!saucer_.alive) {
        // Only tease the saucer while a wave is up — and never during the boss
        // fight, whose mothership owns the top of the screen.
        if (aliveAliens() > 0 && !bossActive_) {
            saucerTimer_ -= dt;
            if (saucerTimer_ <= 0.0f) {
                spawnSaucer();
                saucerTimer_ = frange(14.0f, 22.0f);
            }
        }
    } else {
        saucer_.x += saucer_.vx * dt;
        if (saucer_.x > asp_ + 0.1f || saucer_.x < -asp_ - 0.1f)
            saucer_.alive = false;
    }
    if (audio_) audio_->setSaucer(soundEnabled_ && saucer_.alive);
}

void Game::updatePowerUps(float dt) {
    if (rapidActive_) {
        rapidTimer_ -= dt;
        if (rapidTimer_ <= 0.0f) rapidActive_ = false;
    }
    if (tripleActive_) {
        tripleTimer_ -= dt;
        if (tripleTimer_ <= 0.0f) tripleActive_ = false;
    }

    float puR = 0.045f;
    for (auto& pu : powerUps_) {
        if (!pu.alive) continue;
        pu.y   += pu.vy * dt;
        pu.rot += pu.spin * dt;
        if (pu.y > 1.1f) { pu.alive = false; continue; }
        float dx = pu.x - shipX_, dy = pu.y - shipY_;
        if (dx*dx + dy*dy < (puR + shipR_) * (puR + shipR_)) {
            pu.alive = false;
            long bonus = (long)kPowerUpScore * level_;
            score_ += bonus;
            bonusFlash_ = bonus;
            bonusFlashTimer_ = 0.9f;
            bonusFlashX_ = pu.x; bonusFlashY_ = pu.y - 0.06f;
            applyPowerUp(pu.type);
            if (audio_ && soundEnabled_) audio_->triggerPowerUp();
        }
    }
    for (size_t i = powerUps_.size(); i-- > 0;)
        if (!powerUps_[i].alive) powerUps_.erase(powerUps_.begin() + i);
}

void Game::removeDeadEntities() {
    for (size_t i = bullets_.size(); i-- > 0;)
        if (!bullets_[i].alive) bullets_.erase(bullets_.begin() + i);
    for (size_t i = bombs_.size(); i-- > 0;)
        if (!bombs_[i].alive) bombs_.erase(bombs_.begin() + i);
    // aliens_ keeps dead entries: slot positions derive from (row, col).
}

void Game::checkLevelClear() {
    if (state_ != PLAYING || aliveAliens() > 0) return;
    // The boss level only ends when the mothership dies (handled in
    // updateBullets, which jumps straight to WIN).
    if (bossActive_ && boss_.alive) return;
    score_ += (long)kLevelClearScore * level_;
    if (audio_) audio_->setSaucer(false);
    if (audio_ && soundEnabled_) audio_->triggerLevelClear();
    state_ = LEVEL_CLEAR;
    stateTimer_ = 0.0f;
    bombs_.clear();
    bullets_.clear();
    saucer_ = {};
}

// ---- auto-play AI ----
// Drives the same control path as a finger in the control strip: it sets a
// steering target (aiTargetX_/aiMove_) and holds the trigger (aiFire_).
// Priorities: dodge incoming bombs > collect a power-up > hunt the saucer >
// line up on the nearest invader column.
void Game::updateAutoPlay(float dt) {
    // Triple shot covers a wider cone, so alignment can be looser.
    const float kAlignThresh = tripleActive_ ? 0.10f : 0.030f;

    // 1) Threat scan: the bomb that will reach our altitude soonest and lands
    //    close enough to matter (boss bombs drift sideways — predict impact x).
    const Bomb* threat = nullptr;
    float threatT = 1e9f, threatX = 0.0f;
    for (auto& b : bombs_) {
        if (!b.alive || b.vy <= 0.0f) continue;
        float tHit = (shipY_ - b.y) / b.vy;
        if (tHit < 0.0f || tHit > 0.9f) continue;
        float impactX = b.x + b.vx * tHit;
        if (fabsf(impactX - shipX_) < 0.11f && tHit < threatT) {
            threatT = tHit;
            threatX = impactX;
            threat = &b;
        }
    }

    // 2) Best shoot target: the boss (lead-aimed on its sine drift) outranks
    //    the saucer, which outranks the nearest alien column.
    bool  hasTarget = false;
    float targetX   = 0.0f;
    if (bossActive_ && boss_.alive) {
        float tFly = (shipY_ - kBossY) / kBulletSpeed;
        targetX = sinf((boss_.t + tFly) * 0.60f) * (asp_ - kBossHW - 0.05f);
        hasTarget = true;
    }
    if (!hasTarget && saucer_.alive) {
        float tFly = (shipY_ - saucer_.y) / kBulletSpeed;
        float aim  = saucer_.x + saucer_.vx * tFly;
        if (fabsf(aim) < asp_ - shipScale_) { targetX = aim; hasTarget = true; }
    }
    if (!hasTarget) {
        float bestDx = 1e9f;
        for (auto& a : aliens_) {
            if (!a.alive) continue;
            float ax  = alienX(a);
            float tFly = (shipY_ - alienY(a)) / kBulletSpeed;
            float aim = ax + marchDir_ * marchSpeed() * tFly;
            float dx  = fabsf(aim - shipX_);
            if (dx < bestDx) { bestDx = dx; targetX = aim; hasTarget = true; }
        }
    }

    // 3) Power-up interception, only when nothing is shooting at us.
    const PowerUp* collect = nullptr;
    if (!threat) {
        float bestT = 1e9f;
        for (auto& pu : powerUps_) {
            if (!pu.alive) continue;
            float tFall = (shipY_ - pu.y) / pu.vy;
            if (tFall < -0.3f) continue;            // already dropped past the ship
            if (tFall < 0.0f) tFall = 0.0f;
            float tSteer = fabsf(pu.x - shipX_) / shipSpeed_;
            if (tSteer > tFall + 0.8f) continue;    // can't reach it before it escapes
            float t = tSteer > tFall ? tSteer : tFall;
            if (t < bestT) { bestT = t; collect = &pu; }
        }
    }

    // Steering priority: dodge > collect > align.
    if (threat) {
        float dir = (threatX >= shipX_) ? -1.0f : 1.0f;
        float target = shipX_ + dir * 0.24f;
        float lim = asp_ - shipScale_;
        if (target > lim || target < -lim) target = shipX_ - dir * 0.24f;
        aiTargetX_ = target;
        aiMove_ = true;
    } else if (collect) {
        aiTargetX_ = collect->x;
        aiMove_ = true;
    } else if (hasTarget) {
        aiTargetX_ = targetX;
        aiMove_ = true;
    }

    // Fire whenever we're lined up on something worth hitting.
    if (hasTarget && fabsf(targetX - shipX_) < kAlignThresh) aiFire_ = true;
    (void)dt;
}

// ---- drawing helpers ----
void Game::emit(std::vector<DrawCmd>& out, int shape, float wx, float wy,
                float sx, float sy, float rot, float r, float g, float b, float a,
                float style) {
    float c = cosf(rot), s = sinf(rot);
    DrawCmd d;
    d.mtx[0] = sx * c / asp_;
    d.mtx[1] = -sy * s / asp_;
    d.mtx[2] = sx * s;
    d.mtx[3] = sy * c;
    d.tx = wx / asp_ + shakeX_;
    d.ty = wy + shakeY_;
    d.color[0] = r; d.color[1] = g; d.color[2] = b; d.color[3] = a;
    d.style = style;
    d.seed = 0.0f;
    d.shape = shape;
    out.push_back(d);
}

void Game::drawDigit(std::vector<DrawCmd>& out, int dgt, float cx, float cy,
                     float h, float r, float g, float b, float a) {
    if (dgt < 0 || dgt > 9) return;
    int mask = kDigitSeg[dgt];
    float w = h * 0.60f;
    float t = h * 0.16f;
    float ht = t * 0.5f, hw = w * 0.5f, q = h * 0.25f;
    float hSegX = hw - ht, hSegY = ht;     // horizontal segment half-extents
    float vSegX = ht, vSegY = q - ht;      // vertical segment half-extents
    auto seg = [&](float x, float y, float ex, float ey) {
        emit(out, SHAPE_QUAD, x, y, ex, ey, 0.0f, r, g, b, a);
    };
    if (mask & 1)  seg(cx, cy - h * 0.5f + ht, hSegX, hSegY);   // a top
    if (mask & 2)  seg(cx + hw - ht, cy - q, vSegX, vSegY);     // b top-right
    if (mask & 4)  seg(cx + hw - ht, cy + q, vSegX, vSegY);     // c bottom-right
    if (mask & 8)  seg(cx, cy + h * 0.5f - ht, hSegX, hSegY);   // d bottom
    if (mask & 16) seg(cx - hw + ht, cy + q, vSegX, vSegY);     // e bottom-left
    if (mask & 32) seg(cx - hw + ht, cy - q, vSegX, vSegY);     // f top-left
    if (mask & 64) seg(cx, cy, hSegX, hSegY);                   // g middle
}

int Game::numDigits(int v) const {
    if (v <= 0) return 1;
    int n = 0;
    while (v > 0) { n++; v /= 10; }
    return n;
}

void Game::drawNumber(std::vector<DrawCmd>& out, int value, float firstCx, float cy,
                      float h, float r, float g, float b, float a) {
    if (value < 0) value = 0;
    int n = numDigits(value);
    float w = h * 0.60f;
    float step = w * 1.45f;
    int digits[12];
    int tmp = value, count = 0;
    if (value == 0) { digits[count++] = 0; }
    while (tmp > 0 && count < 12) { digits[count++] = tmp % 10; tmp /= 10; }
    // digits[] is reversed; draw most-significant first.
    for (int i = 0; i < n; i++) {
        int dgt = digits[n - 1 - i];
        drawDigit(out, dgt, firstCx + i * step, cy, h, r, g, b, a);
    }
}

void Game::drawLetter(std::vector<DrawCmd>& out, char ch, float cx, float cy,
                      float h, float r, float g, float b, float a) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= '0' && ch <= '9') { drawDigit(out, ch - '0', cx, cy, h, r, g, b, a); return; }
    if (ch < 'A' || ch > 'Z') return;

    // Stroke font: each letter built from line segments.
    // Coords are in letter-local space: x in [-1,1], y in [-1,1] (top=-1, bot=+1).
    float hw = h * 0.38f;  // half-width
    float hh = h * 0.50f;  // half-height
    float th = h * 0.07f;  // stroke thickness

    auto stroke = [&](float x1, float y1, float x2, float y2) {
        float wx1 = cx + x1 * hw,  wy1 = cy + y1 * hh;
        float wx2 = cx + x2 * hw,  wy2 = cy + y2 * hh;
        float dx = wx2 - wx1, dy = wy2 - wy1;
        float len = sqrtf(dx*dx + dy*dy);
        if (len < 0.001f) return;
        float rot = atan2f(wx1 - wx2, wy2 - wy1);
        emit(out, SHAPE_QUAD, (wx1+wx2)*0.5f, (wy1+wy2)*0.5f, th, len*0.5f, rot, r, g, b, a);
    };

    switch (ch) {
        case 'A': stroke(-1,1,0,-1); stroke(1,1,0,-1); stroke(-0.5f,0.1f,0.5f,0.1f); break;
        case 'B': stroke(-1,-1,-1,1); stroke(-1,-1,0.6f,-1); stroke(0.6f,-1,1,-0.5f); stroke(1,-0.5f,0.6f,0); stroke(0.6f,0,-1,0); stroke(-1,0,0.8f,0); stroke(0.8f,0,1,0.5f); stroke(1,0.5f,0.8f,1); stroke(0.8f,1,-1,1); break;
        case 'C': stroke(1,-0.7f,0,-1); stroke(0,-1,-1,-0.3f); stroke(-1,-0.3f,-1,0.3f); stroke(-1,0.3f,0,1); stroke(0,1,1,0.7f); break;
        case 'D': stroke(-1,-1,-1,1); stroke(-1,-1,0.3f,-1); stroke(0.3f,-1,1,-0.4f); stroke(1,-0.4f,1,0.4f); stroke(1,0.4f,0.3f,1); stroke(0.3f,1,-1,1); break;
        case 'E': stroke(-1,-1,-1,1); stroke(-1,-1,1,-1); stroke(-1,0,0.6f,0); stroke(-1,1,1,1); break;
        case 'F': stroke(-1,-1,-1,1); stroke(-1,-1,1,-1); stroke(-1,0,0.6f,0); break;
        case 'G': stroke(1,-0.7f,0,-1); stroke(0,-1,-1,-0.3f); stroke(-1,-0.3f,-1,0.3f); stroke(-1,0.3f,0,1); stroke(0,1,1,0.7f); stroke(1,0.7f,1,0); stroke(1,0,0.2f,0); break;
        case 'H': stroke(-1,-1,-1,1); stroke(1,-1,1,1); stroke(-1,0,1,0); break;
        case 'I': stroke(-0.5f,-1,0.5f,-1); stroke(0,-1,0,1); stroke(-0.5f,1,0.5f,1); break;
        case 'J': stroke(0.5f,-1,0.5f,0.6f); stroke(0.5f,0.6f,0,1); stroke(0,1,-0.5f,0.7f); break;
        case 'K': stroke(-1,-1,-1,1); stroke(-1,0,1,-1); stroke(-1,0,1,1); break;
        case 'L': stroke(-1,-1,-1,1); stroke(-1,1,1,1); break;
        case 'M': stroke(-1,1,-1,-1); stroke(-1,-1,0,0.3f); stroke(0,0.3f,1,-1); stroke(1,-1,1,1); break;
        case 'N': stroke(-1,1,-1,-1); stroke(-1,-1,1,1); stroke(1,1,1,-1); break;
        case 'O': stroke(-1,-1,1,-1); stroke(1,-1,1,1); stroke(1,1,-1,1); stroke(-1,1,-1,-1); break;
        case 'P': stroke(-1,-1,-1,1); stroke(-1,-1,0.7f,-1); stroke(0.7f,-1,1,-0.5f); stroke(1,-0.5f,0.7f,0); stroke(0.7f,0,-1,0); break;
        case 'Q': stroke(-1,-1,1,-1); stroke(1,-1,1,1); stroke(1,1,-1,1); stroke(-1,1,-1,-1); stroke(0.2f,0.4f,1,1); break;
        case 'R': stroke(-1,-1,-1,1); stroke(-1,-1,0.7f,-1); stroke(0.7f,-1,1,-0.5f); stroke(1,-0.5f,0.7f,0); stroke(0.7f,0,-1,0); stroke(-0.1f,0,1,1); break;
        case 'S': stroke(1,-0.8f,-1,-1); stroke(-1,-1,-1,0); stroke(-1,0,1,0); stroke(1,0,1,1); stroke(1,1,-1,0.8f); break;
        case 'T': stroke(-1,-1,1,-1); stroke(0,-1,0,1); break;
        case 'U': stroke(-1,-1,-1,0.7f); stroke(-1,0.7f,0,1); stroke(0,1,1,0.7f); stroke(1,0.7f,1,-1); break;
        case 'V': stroke(-1,-1,0,1); stroke(1,-1,0,1); break;
        case 'W': stroke(-1,-1,-0.5f,1); stroke(-0.5f,1,0,0); stroke(0,0,0.5f,1); stroke(0.5f,1,1,-1); break;
        case 'X': stroke(-1,-1,1,1); stroke(1,-1,-1,1); break;
        case 'Y': stroke(-1,-1,0,0); stroke(1,-1,0,0); stroke(0,0,0,1); break;
        case 'Z': stroke(-1,-1,1,-1); stroke(1,-1,-1,1); stroke(-1,1,1,1); break;
        default: break;
    }
}

void Game::drawText(std::vector<DrawCmd>& out, const char* text, float cx, float cy,
                    float h, float r, float g, float b, float a) {
    int total = 0;
    for (const char* p = text; *p; p++) total++;
    if (total == 0) return;
    float step = h * 0.95f;  // stroke font: wider spacing than 7-seg digits
    float startX = cx - (total - 1) * step * 0.5f;
    for (int i = 0; text[i]; i++) {
        if (text[i] != ' ')
            drawLetter(out, text[i], startX + i * step, cy, h, r, g, b, a);
    }
}

// The three-layer delta-wing fighter — same colors as the launcher icon.
void Game::drawShip(std::vector<DrawCmd>& out, float cx, float cy, float scale,
                    float tilt, float alpha) {
    emit(out, SHAPE_SHIP_WINGS, cx, cy, scale, scale, tilt,
         0.22f, 0.42f, 0.65f, alpha);
    emit(out, SHAPE_SHIP_BODY, cx, cy, scale, scale, tilt,
         0.28f, 0.72f, 0.92f, alpha);
    emit(out, SHAPE_SHIP_NOSE, cx, cy, scale, scale, tilt,
         0.88f, 0.97f, 1.00f, alpha);
}

void Game::drawAlien(std::vector<DrawCmd>& out, const Alien& a, float cx, float cy,
                     float alpha) {
    static const int kFrame0[3] = {SHAPE_INVADER_A0, SHAPE_INVADER_B0, SHAPE_INVADER_C0};
    static const int kFrame1[3] = {SHAPE_INVADER_A1, SHAPE_INVADER_B1, SHAPE_INVADER_C1};
    static const float kTierCol[3][3] = {
        {0.78f, 0.45f, 1.00f},   // squid: purple
        {0.30f, 0.95f, 0.55f},   // crab: green
        {1.00f, 0.70f, 0.20f},   // octopus: amber
    };
    int shape = (marchFrame_ ? kFrame1 : kFrame0)[a.type];
    const float* c = kTierCol[a.type];
    emit(out, shape, cx, cy, kAlienHW, kAlienHH, 0.0f, c[0], c[1], c[2], alpha);
}

void Game::drawPowerUpHUD(std::vector<DrawCmd>& out) {
    // Left-side HUD: active bonus indicators. Shield has no timer (it lasts
    // until hit); rapid fire and triple shot show remaining time as a bar.
    float iconX = -asp_ + 0.055f;
    float barX0 = -asp_ + 0.095f;
    float barMaxW = 0.16f;
    int row = 0;
    if (shieldActive_) {
        float pulse = 0.75f + 0.25f * sinf(animTime_ * 4.0f);
        emit(out, SHAPE_DISC, iconX, -0.70f, 0.020f, 0.020f, 0.0f,
             0.30f, 0.55f, 1.00f, pulse);
        row++;
    }
    auto timedRow = [&](float timer, float cr, float cg, float cb) {
        float y = -0.70f + row * 0.11f;
        float progress = timer / kRapidDuration;
        float pulse = (timer < 2.0f) ? (0.5f + 0.5f * sinf(animTime_ * 18.0f)) : 1.0f;
        emit(out, SHAPE_QUAD, iconX, y, 0.020f, 0.020f, 0.785f, cr, cg, cb, pulse);
        emit(out, SHAPE_QUAD, barX0 + barMaxW * 0.5f, y, barMaxW * 0.5f, 0.006f, 0.0f,
             cr * 0.3f, cg * 0.3f, cb * 0.3f, 0.5f);
        float fillW = barMaxW * progress;
        if (fillW > 0.002f)
            emit(out, SHAPE_QUAD, barX0 + fillW * 0.5f, y, fillW * 0.5f, 0.006f, 0.0f,
                 cr, cg, cb, 0.85f);
        row++;
    };
    if (rapidActive_)  timedRow(rapidTimer_,  1.00f, 0.85f, 0.10f);
    if (tripleActive_) timedRow(tripleTimer_, 0.25f, 1.00f, 0.40f);
}

void Game::drawBossHealthBar(std::vector<DrawCmd>& out) {
    if (!bossActive_ || !boss_.alive) return;
    float progress = (float)boss_.hp / (float)boss_.maxHp;
    float barW = asp_ * 0.80f;
    // Sits in the gap between the mothership's flight line and its escort rows.
    float y    = -0.66f;
    // Background
    emit(out, SHAPE_QUAD, 0.0f, y, barW, 0.010f, 0.0f, 0.25f, 0.05f, 0.10f, 0.7f);
    // Fill (magenta → red as health drops)
    float fr = 1.0f, fg = 0.15f, fb = 0.25f + 0.45f * progress;
    emit(out, SHAPE_QUAD, -barW + barW * progress, y, barW * progress, 0.010f, 0.0f, fr, fg, fb, 0.9f);
    drawText(out, "BOSS", 0.0f, y + 0.040f, 0.032f, fr, fg, fb, 0.85f);
}

// The touch strip below the ship: a faint backdrop, a divider line, and a
// steering marker under the ship so the player knows where their finger acts.
void Game::drawControlStrip(std::vector<DrawCmd>& out) {
    bool held = false;
    for (auto& p : pointers_)
        if (p.active && p.y > vh_ * kCtrlZoneFrac) { held = true; break; }

    float cy = (kCtrlZoneTopW + 1.0f) * 0.5f;
    float hh = (1.0f - kCtrlZoneTopW) * 0.5f;
    emit(out, SHAPE_QUAD, 0.0f, cy, asp_, hh, 0.0f,
         0.30f, 0.60f, 1.00f, held ? 0.10f : 0.05f);
    // Divider line at the strip's top edge
    emit(out, SHAPE_QUAD, 0.0f, kCtrlZoneTopW, asp_, 0.0025f, 0.0f,
         0.40f, 0.65f, 0.95f, 0.30f);
    // Steering marker tracks the ship
    emit(out, SHAPE_SHIP_NOSE, shipX_, cy, 0.030f, 0.030f, 0.0f,
         0.40f, 0.75f, 1.00f, held ? 0.90f : 0.40f);
}

void Game::drawGearIcon(std::vector<DrawCmd>& out, float cx, float cy, float size,
                         float r, float g, float b, float a) {
    // Circular body
    const float bodyR = size * 0.68f;
    emit(out, SHAPE_DISC, cx, cy, bodyR, bodyR, 0.0f, r, g, b, a);

    // 6 rectangular teeth protruding clearly beyond the body
    const float toothCR = size * 0.94f;   // center of tooth from gear center
    const float toothHW = size * 0.20f;   // tooth half-width  (tangential)
    const float toothHH = size * 0.30f;   // tooth half-height (radial)
    for (int i = 0; i < 6; i++) {
        float ang = i * 1.0472f;  // 60° apart
        emit(out, SHAPE_QUAD,
             cx + cosf(ang) * toothCR,
             cy + sinf(ang) * toothCR,
             toothHW, toothHH, ang, r, g, b, a);
    }

    // Centre hole punched through in background colour
    emit(out, SHAPE_DISC, cx, cy, size * 0.30f, size * 0.30f, 0.0f,
         0.03f, 0.04f, 0.09f, a);
}

// A pair of glasses: two lens rings joined by a bridge, with short temples.
void Game::drawGlassesIcon(std::vector<DrawCmd>& out, float cx, float cy, float size,
                           float r, float g, float b, float a) {
    const float lensR  = size * 0.55f;
    const float lensDX = size * 0.72f;
    for (int s = -1; s <= 1; s += 2) {
        emit(out, SHAPE_DISC, cx + s * lensDX, cy, lensR, lensR, 0.0f, r, g, b, a);
        emit(out, SHAPE_DISC, cx + s * lensDX, cy, lensR * 0.62f, lensR * 0.62f, 0.0f,
             0.04f, 0.06f, 0.14f, a);   // punch the lens hole in overlay colour
    }
    // Bridge
    emit(out, SHAPE_QUAD, cx, cy - lensR * 0.35f, lensDX - lensR * 0.8f, size * 0.10f,
         0.0f, r, g, b, a);
    // Temples
    for (int s = -1; s <= 1; s += 2)
        emit(out, SHAPE_QUAD, cx + s * (lensDX + lensR * 1.15f), cy - lensR * 0.25f,
             lensR * 0.55f, size * 0.09f, 0.0f, r, g, b, a);
}

// Phone-side banner while the game runs on the glasses.
void Game::drawOnGlassesOverlay(std::vector<DrawCmd>& out) {
    emit(out, SHAPE_QUAD, 0.0f, 0.0f, asp_, 1.0f, 0.0f, 0.03f, 0.05f, 0.12f, 0.80f);
    float pulse = 0.6f + 0.4f * sinf(animTime_ * 3.0f);
    drawGlassesIcon(out, 0.0f, -0.22f, 0.10f, 0.35f, 0.95f, 0.55f, pulse);
    drawText(out, "ON GLASSES", 0.0f, 0.0f, 0.070f, 0.35f, 0.95f, 0.55f, 1.0f);
    drawText(out, "OPEN SETTINGS", 0.0f, 0.16f, 0.038f, 0.65f, 0.72f, 0.85f, 0.9f);
    drawText(out, "TO PLAY ON PHONE", 0.0f, 0.24f, 0.038f, 0.65f, 0.72f, 0.85f, 0.9f);
}

void Game::drawSettingsScreen(std::vector<DrawCmd>& out) {
    // Full-screen dark overlay
    emit(out, SHAPE_QUAD, 0.0f, 0.0f, asp_, 1.0f, 0.0f, 0.04f, 0.06f, 0.14f, 0.93f);

    drawText(out, "SETTINGS", 0.0f, -0.52f, 0.078f, 0.55f, 0.78f, 1.00f, 1.0f);

    const float labelX  = -asp_ * 0.48f;
    const float toggleX =  asp_ * 0.45f;

    // Sound row
    drawText(out, "SOUND", labelX, kSettingSoundY, 0.055f, 0.80f, 0.85f, 0.90f, 0.9f);
    emit(out, SHAPE_QUAD, toggleX, kSettingSoundY, 0.090f, 0.040f, 0.0f,
         soundEnabled_ ? 0.08f : 0.20f,
         soundEnabled_ ? 0.48f : 0.18f,
         soundEnabled_ ? 0.08f : 0.18f, 0.75f);
    drawText(out, soundEnabled_ ? "ON" : "OFF", toggleX, kSettingSoundY, 0.048f,
             soundEnabled_ ? 0.35f : 0.65f,
             soundEnabled_ ? 1.00f : 0.45f,
             soundEnabled_ ? 0.35f : 0.45f, 1.0f);

    // Auto Play row
    drawText(out, "AUTO PLAY", labelX, kSettingAutoPlayY, 0.055f, 0.80f, 0.85f, 0.90f, 0.9f);
    emit(out, SHAPE_QUAD, toggleX, kSettingAutoPlayY, 0.090f, 0.040f, 0.0f,
         autoPlayActive_ ? 0.08f : 0.20f,
         autoPlayActive_ ? 0.48f : 0.18f,
         autoPlayActive_ ? 0.08f : 0.18f, 0.75f);
    drawText(out, autoPlayActive_ ? "ON" : "OFF", toggleX, kSettingAutoPlayY, 0.048f,
             autoPlayActive_ ? 0.35f : 0.65f,
             autoPlayActive_ ? 1.00f : 0.45f,
             autoPlayActive_ ? 0.35f : 0.45f, 1.0f);

    // Glasses row — same label + chip pattern as the toggles above. The chip
    // is the action: PLAY hands the game to the glasses, PHONE brings it
    // back, NONE means nothing is paired. Short chip words fit every screen
    // width (the old free-form text collided with the icon on narrow folds).
    const char* chip;
    float gr, gg, gb, ga;          // icon + chip text colour
    float br, bg, bb, ba;          // chip background
    if (glassesActive_) {
        chip = "PHONE";
        gr = 1.00f; gg = 0.85f; gb = 0.20f; ga = 1.0f;
        br = 0.40f; bg = 0.32f; bb = 0.05f; ba = 0.75f;
    } else if (glassesConnected_) {
        chip = "PLAY";
        gr = 0.35f; gg = 1.00f; gb = 0.55f; ga = 1.0f;
        br = 0.08f; bg = 0.48f; bb = 0.08f; ba = 0.75f;
    } else {
        chip = "NONE";
        gr = 0.55f; gg = 0.58f; gb = 0.65f; ga = 0.7f;
        br = 0.20f; bg = 0.20f; bb = 0.22f; ba = 0.60f;
    }
    drawText(out, "GLASSES", labelX, kSettingGlassesY, 0.055f, 0.80f, 0.85f, 0.90f, 0.9f);
    // Small glasses icon centred in whatever gap the screen width leaves
    // between the label and the chip (narrow folds leave very little).
    const float labelHalf = 7 * 0.055f * 0.95f * 0.5f;
    const float gapLeft   = labelX + labelHalf;
    const float gapRight  = toggleX - 0.105f;
    drawGlassesIcon(out, (gapLeft + gapRight) * 0.5f, kSettingGlassesY, 0.030f,
                    gr, gg, gb, ga);
    emit(out, SHAPE_QUAD, toggleX, kSettingGlassesY, 0.100f, 0.040f, 0.0f, br, bg, bb, ba);
    drawText(out, chip, toggleX, kSettingGlassesY, 0.040f, gr, gg, gb, 1.0f);

    // Back button
    emit(out, SHAPE_QUAD, 0.0f, kSettingBackY, 0.12f, 0.052f, 0.0f, 0.22f, 0.22f, 0.25f, 0.82f);
    drawText(out, "BACK", 0.0f, kSettingBackY, 0.055f, 0.85f, 0.85f, 0.92f, 1.0f);
}

void Game::render(std::vector<DrawCmd>& out) {
    // Stars are soft twinkling glow dots so they read as scenery, never as
    // collidable objects — everything dangerous stays sharp-edged. The glow
    // falloff eats the disc's edge, so sizes are scaled up to compensate.
    // far stars (dim, slow)
    for (auto& s : stars_) {
        float tw = 0.35f + 0.15f * sinf(animTime_ * (0.8f + s.phase * 0.2f) + s.phase);
        emit(out, SHAPE_DISC, s.x * asp_, s.y, s.size * 1.7f, s.size * 1.7f, 0.0f,
             0.55f, 0.60f, 0.78f, tw, (float)STYLE_GLOW);
    }
    // near stars (brighter, fast parallax layer)
    for (auto& s : starsNear_) {
        float tw = 0.50f + 0.20f * sinf(animTime_ * (1.2f + s.phase * 0.3f) + s.phase);
        emit(out, SHAPE_DISC, s.x * asp_, s.y, s.size * 1.7f, s.size * 1.7f, 0.0f,
             0.85f, 0.88f, 1.00f, tw, (float)STYLE_GLOW);
    }

    // Invader wave. On the title screen it doubles as the demo formation,
    // drawn dimmer under the title text.
    bool titleDemo = (state_ == TITLE);
    float waveAlpha = titleDemo ? 0.85f : 1.0f;
    float waveShiftY = titleDemo ? 0.30f : 0.0f;  // demo sits below the title
    for (auto& a : aliens_) {
        if (!a.alive) continue;
        drawAlien(out, a, alienX(a), alienY(a) + waveShiftY, waveAlpha);
    }

    // Boss mothership: a giant saucer sprite with an angry pulsing aura.
    if (bossActive_ && boss_.alive &&
        (state_ == PLAYING || state_ == LEVEL_CLEAR || state_ == SETTINGS)) {
        float bpulse = 0.25f + 0.15f * sinf(animTime_ * 5.0f);
        float hurt = 1.0f - (float)boss_.hp / (float)boss_.maxHp;  // redder as it weakens
        emit(out, SHAPE_DISC, boss_.x, kBossY, kBossHW * 1.5f, kBossHH * 2.2f, 0.0f,
             1.00f, 0.20f, 0.55f, bpulse, (float)STYLE_GLOW);
        emit(out, SHAPE_SAUCER, boss_.x, kBossY, kBossHW, kBossHH, 0.0f,
             0.80f + 0.20f * hurt, 0.30f - 0.10f * hurt, 0.70f - 0.30f * hurt, 1.0f);
    }

    // Saucer: pixel sprite plus a pulsing glow so it reads as the bonus target.
    if (saucer_.alive) {
        float pulse = 0.30f + 0.15f * sinf(animTime_ * 9.0f);
        emit(out, SHAPE_DISC, saucer_.x, saucer_.y, 0.075f, 0.045f, 0.0f,
             1.00f, 0.35f, 0.30f, pulse, (float)STYLE_GLOW);
        emit(out, SHAPE_SAUCER, saucer_.x, saucer_.y, 0.055f, 0.026f, 0.0f,
             1.00f, 0.40f, 0.35f, 1.0f);
    }

    // power-ups: rotating diamond with glow halo
    for (auto& pu : powerUps_) {
        if (!pu.alive) continue;
        float pr, pg, pb;
        if      (pu.type == PU_SHIELD) { pr=0.30f; pg=0.55f; pb=1.00f; }
        else if (pu.type == PU_RAPID)  { pr=1.00f; pg=0.85f; pb=0.10f; }
        else                           { pr=0.25f; pg=1.00f; pb=0.40f; }
        float sz   = 0.035f;
        float glow = sz * (1.35f + 0.20f * sinf(animTime_ * 5.0f));
        emit(out, SHAPE_QUAD, pu.x, pu.y, glow, glow, pu.rot,      pr,   pg,   pb,   0.28f);
        emit(out, SHAPE_QUAD, pu.x, pu.y, sz,   sz,   pu.rot,      pr,   pg,   pb,   1.00f);
        emit(out, SHAPE_QUAD, pu.x, pu.y, sz*0.38f, sz*0.38f, pu.rot + 0.785f, 1.0f, 1.0f, 1.0f, 0.85f);
    }

    // player lasers: two-layer bolt (outer glow + bright core), tilted along
    // their flight path for triple-shot side lasers
    for (auto& b : bullets_) {
        if (!b.alive) continue;
        float rot = b.vx != 0.0f ? atan2f(b.vx, -b.vy) : 0.0f;
        emit(out, SHAPE_QUAD, b.x, b.y, 0.018f, 0.034f, rot, 0.40f, 0.85f, 1.00f, 0.45f);
        emit(out, SHAPE_QUAD, b.x, b.y, 0.008f, 0.025f, rot, 1.00f, 1.00f, 0.80f, 1.00f);
    }

    // alien bombs: red bolt, wobbling like the classic zig-zag shot
    for (auto& b : bombs_) {
        if (!b.alive) continue;
        float rot = sinf(b.wobble) * 0.45f;
        emit(out, SHAPE_QUAD, b.x, b.y, 0.016f, 0.032f, rot, 1.00f, 0.35f, 0.15f, 0.45f);
        emit(out, SHAPE_QUAD, b.x, b.y, 0.007f, 0.024f, rot, 1.00f, 0.75f, 0.35f, 1.00f);
    }

    // explosion flash rings (expanding, fading, glow falloff)
    for (auto& e : explosions_) {
        float t = e.t / e.maxLife;                // 0→1
        float s = e.radius * (1.0f + t * 3.5f);   // expand outward
        float a = (1.0f - t) * 0.85f;
        float fr = 1.0f, fg = 0.65f + e.cr * 0.35f, fb = 0.10f;
        emit(out, SHAPE_DISC, e.x, e.y, s, s, 0.0f, fr, fg, fb, a, (float)STYLE_GLOW);
    }

    // debris fragments
    for (auto& p : particles_) {
        float life = 1.0f - p.t / p.maxLife;     // 1→0
        float sz   = p.size * life;
        emit(out, SHAPE_QUAD, p.x, p.y, sz, sz, p.rot, p.r, p.g, p.b, life);
    }

    // ship (PLAYING + LEVEL_CLEAR + TITLE). Blink while invulnerable.
    bool showShip = (state_ == PLAYING || state_ == LEVEL_CLEAR || state_ == TITLE);
    bool blinkOn = invuln_ <= 0.0f || fmodf(animTime_ * 12.0f, 1.0f) < 0.6f;
    if (showShip && blinkOn) {
        float bob = (state_ == TITLE) ? 0.02f * sinf(animTime_ * 2.0f) : 0.0f;
        float tilt = (state_ == TITLE) ? 0.0f : shipTilt_;
        drawShip(out, shipX_, shipY_ + bob, shipScale_, tilt, 1.0f);

        // Shield bubble when active
        if (shieldActive_) {
            float pulse = 0.60f + 0.40f * sinf(animTime_ * 9.0f);
            float sr = shipScale_ * 1.85f;
            emit(out, SHAPE_DISC, shipX_, shipY_ + bob, sr, sr, 0.0f,
                 0.30f, 0.55f, 1.00f, pulse * 0.55f, (float)STYLE_GLOW);
        }
    }

    // Bonus flash: the points just earned, fading where they were earned.
    if (bonusFlashTimer_ > 0.0f) {
        float alpha = bonusFlashTimer_ / 0.9f;
        int n = numDigits((int)bonusFlash_);
        float h = 0.055f, step = h * 0.60f * 1.45f;
        drawNumber(out, (int)bonusFlash_, bonusFlashX_ - (n - 1) * step * 0.5f,
                   bonusFlashY_, h, 1.0f, 0.85f, 0.10f, alpha);
    }

    // HUD during gameplay — drawn without screen shake so it stays readable on hit.
    if (state_ == PLAYING || state_ == LEVEL_CLEAR) {
        float savedShakeX = shakeX_, savedShakeY = shakeY_;
        shakeX_ = 0.0f; shakeY_ = 0.0f;

        float h = 0.085f;
        float w = h * 0.60f;
        float step = w * 1.45f;
        // score top-left
        drawNumber(out, (int)score_, -asp_ + 0.06f + w * 0.5f, -0.90f, h,
                   1.0f, 1.0f, 1.0f, 1.0f);
        // level number top-right (yellow), right-aligned — supports 2 digits at level 10
        int nd = numDigits(level_);
        float levelFirstCx = asp_ - 0.06f - w * 0.5f - (nd - 1) * step;
        drawNumber(out, level_, levelFirstCx, -0.90f, h,
                   1.0f, 0.85f, 0.2f, 1.0f);
        // lives as small ship icons, top-center
        float ls = 0.03f, gap = 0.085f;
        float startX = -(lives_ - 1) * gap * 0.5f;
        for (int i = 0; i < lives_; i++)
            drawShip(out, startX + i * gap, -0.90f, ls, 0.0f, 1.0f);

        drawPowerUpHUD(out);
        drawBossHealthBar(out);
        shakeX_ = savedShakeX; shakeY_ = savedShakeY;
    }

    // Control strip — shake-free, active gameplay only, phone mode only (the
    // glasses touchbar has no on-screen counterpart).
    if (state_ == PLAYING && controlMode_ == CONTROL_STRIP) {
        float savedShakeX = shakeX_, savedShakeY = shakeY_;
        shakeX_ = 0.0f; shakeY_ = 0.0f;
        drawControlStrip(out);
        shakeX_ = savedShakeX; shakeY_ = savedShakeY;
    }

    float pulse = 0.5f + 0.5f * sinf(animTime_ * 4.0f);

    if (state_ == TITLE) {
        // Title text
        drawText(out, "VULKAN",   0.0f, -0.86f, 0.062f, 0.35f, 0.78f, 1.00f, 1.0f);
        drawText(out, "SPACE",    0.0f, -0.73f, 0.100f, 0.35f, 0.95f, 0.55f, 1.0f);
        drawText(out, "INVADERS", 0.0f, -0.58f, 0.092f, 0.30f, 0.85f, 0.48f, 1.0f);

        // High score podium (top 3, gold/silver/bronze)
        static const float kPodR[3] = {1.00f, 0.78f, 0.72f};
        static const float kPodG[3] = {0.85f, 0.82f, 0.47f};
        static const float kPodB[3] = {0.20f, 0.92f, 0.22f};
        const float hh = 0.048f, rowY[3] = {0.06f, 0.16f, 0.26f};
        for (int i = 0; i < 3; i++) {
            if (highScores_[i].score <= 0) break;
            float pr = kPodR[i], pg = kPodG[i], pb = kPodB[i];
            // Rank ship icon
            drawShip(out, -asp_*0.72f, rowY[i], 0.020f, 0.0f, 1.0f);
            // Score
            drawNumber(out, (int)highScores_[i].score, -asp_*0.50f, rowY[i], hh, pr, pg, pb, 1.0f);
            // Level digit (right-aligned)
            drawNumber(out, highScores_[i].level, asp_*0.62f, rowY[i], hh, pr, pg, pb, 0.80f);
        }

        // pulsing tap hint
        drawShip(out, 0.0f, 0.40f, 0.05f + 0.012f * pulse, 0.0f, 0.4f + 0.6f * pulse);
    } else if (state_ == LEVEL_CLEAR) {
        // big green level number just cleared
        drawText(out, "LEVEL", 0.0f, -0.28f, 0.075f, 0.4f, 1.0f, 0.5f, 1.0f);
        drawNumber(out, level_, 0.0f, -0.02f, 0.42f, 0.4f, 1.0f, 0.5f, 1.0f);
    } else if (state_ == GAME_OVER) {
        emit(out, SHAPE_QUAD, 0.0f, 0.0f, asp_, 1.0f, 0.0f,
             0.6f, 0.05f, 0.08f, 0.32f + 0.10f * pulse);
        int n = numDigits((int)score_);
        float fh = 0.30f, fw = fh * 0.6f * 1.45f;
        float firstCx = -(n - 1) * fw * 0.5f;
        // Gold pulsing score if new high score, white otherwise
        float sr = 1.0f, sg = newHighScore_ ? (0.75f + 0.20f*pulse) : 1.0f, sb = newHighScore_ ? 0.10f : 1.0f;
        drawNumber(out, (int)score_, firstCx, -0.05f, fh, sr, sg, sb, 1.0f);
        // Rank digit above score when it's a new high score
        if (newHighScore_ && newHighScoreRank_ >= 0 && newHighScoreRank_ < 3) {
            static const float kPodR[3] = {1.00f, 0.78f, 0.72f};
            static const float kPodG[3] = {0.85f, 0.82f, 0.47f};
            static const float kPodB[3] = {0.20f, 0.92f, 0.22f};
            int ri = newHighScoreRank_;
            drawDigit(out, ri + 1, 0.0f, -0.42f, 0.14f,
                      kPodR[ri], kPodG[ri], kPodB[ri], 0.65f + 0.35f * pulse);
        }
        drawShip(out, 0.0f, 0.5f, 0.05f, 0.0f, 0.3f + 0.6f * pulse);
    } else if (state_ == WIN) {
        emit(out, SHAPE_QUAD, 0.0f, 0.0f, asp_, 1.0f, 0.0f,
             0.1f, 0.5f, 0.15f, 0.30f + 0.10f * pulse);
        int n = numDigits((int)score_);
        float fh = 0.30f, fw = fh * 0.6f * 1.45f;
        float firstCx = -(n - 1) * fw * 0.5f;
        float sr = 1.0f, sg = newHighScore_ ? (0.80f + 0.15f*pulse) : 0.9f, sb = newHighScore_ ? 0.10f : 0.3f;
        drawNumber(out, (int)score_, firstCx, -0.05f, fh, sr, sg, sb, 1.0f);
        if (newHighScore_ && newHighScoreRank_ >= 0 && newHighScoreRank_ < 3) {
            static const float kPodR[3] = {1.00f, 0.78f, 0.72f};
            static const float kPodG[3] = {0.85f, 0.82f, 0.47f};
            static const float kPodB[3] = {0.20f, 0.92f, 0.22f};
            int ri = newHighScoreRank_;
            drawDigit(out, ri + 1, 0.0f, -0.42f, 0.14f,
                      kPodR[ri], kPodG[ri], kPodB[ri], 0.65f + 0.35f * pulse);
        }
        drawShip(out, 0.0f, 0.5f, 0.05f, 0.0f, 0.3f + 0.6f * pulse);
    }

    // ── "On glasses" banner — phone instance only, under the gear so
    //    Settings stays reachable to bring the game back ────────────────────
    if (glassesActive_ && controlMode_ == CONTROL_STRIP && state_ != SETTINGS) {
        float svX = shakeX_, svY = shakeY_;
        shakeX_ = shakeY_ = 0.0f;
        drawOnGlassesOverlay(out);
        shakeX_ = svX; shakeY_ = svY;
    }

    // ── Gear button — shake-free, top-right corner, every state but SETTINGS
    //    (and never on the glasses: Settings lives on the phone) ────────────
    if (state_ != SETTINGS && controlMode_ == CONTROL_STRIP) {
        float svX = shakeX_, svY = shakeY_;
        shakeX_ = shakeY_ = 0.0f;
        float gearWX = asp_ - kGearOffsetX, gearWY = kGearWY;
        float gearA  = 0.55f + 0.12f * sinf(animTime_ * 2.0f);
        float gr = autoPlayActive_ ? 0.25f : 0.55f;
        float gg = autoPlayActive_ ? 1.00f : 0.62f;
        float gb = autoPlayActive_ ? 0.35f : 0.78f;
        drawGearIcon(out, gearWX, gearWY, 0.046f, gr, gg, gb, gearA);
        if (autoPlayActive_ && (state_ == PLAYING || state_ == LEVEL_CLEAR)) {
            float autoA = 0.38f + 0.28f * sinf(animTime_ * 3.5f);
            drawText(out, "AUTO", gearWX - 0.18f, gearWY, 0.030f,
                     0.25f, 1.00f, 0.38f, autoA);
        }
        shakeX_ = svX; shakeY_ = svY;
    }

    // ── Settings overlay — drawn last so it covers everything ────────────────
    if (state_ == SETTINGS) {
        float svX = shakeX_, svY = shakeY_;
        shakeX_ = shakeY_ = 0.0f;
        drawSettingsScreen(out);
        shakeX_ = svX; shakeY_ = svY;
    }
}

void Game::clearColor(float out[3]) const {
    if (controlMode_ == CONTROL_TOUCHBAR) {
        // Pure black renders transparent on additive AR lenses.
        out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f;
        return;
    }
    out[0] = 0.03f; out[1] = 0.04f; out[2] = 0.09f;
}
