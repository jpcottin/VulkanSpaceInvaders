#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// android/log.h is available on-device; the test binary links liblog.
#include "game.h"

// ── helpers ───────────────────────────────────────────────────────────────────

// Portrait phone viewport used by every test.
static const int kW = 1080;
static const int kH = 2400;
static float aspect() { return (float)kW / (float)kH; }

// World -> pixel, matching Game's mapping (x in [-asp,asp], y in [-1,1]).
static float pxOf(float wx) { return (wx / aspect() + 1.0f) * 0.5f * kW; }
static float pyOf(float wy) { return (wy + 1.0f) * 0.5f * kH; }

// A pixel y inside the control strip (bottom 15% of the screen).
static float ctrlPy() { return kH * 0.95f; }

static void step(Game& g, float seconds, float dt = 1.0f / 60.0f) {
    for (float t = 0.0f; t < seconds; t += dt) g.update(dt);
}

static void startPlaying(Game& g) {
    g.setViewport(kW, kH);
    g.triggerNewGameForTest();
}

// Tap = press and release; the game consumes it on the next update().
static void tap(Game& g, float px, float py) {
    g.onPointerDown(0, px, py);
    g.onPointerUp(0);
}

// Index of the alien at (row, col) — aliens_ is built row-major, 8 per row.
static int slot(int row, int col) { return row * 8 + col; }

// ── Initial state ─────────────────────────────────────────────────────────────

TEST(InitialState, StartsOnTitle) {
    Game g;
    g.setViewport(kW, kH);
    EXPECT_TRUE(g.inTitleForTest());
}

TEST(InitialState, TapStartsGame) {
    Game g;
    g.setViewport(kW, kH);
    tap(g, kW * 0.5f, kH * 0.5f);
    g.update(0.016f);
    EXPECT_TRUE(g.isPlayingForTest());
    EXPECT_EQ(g.lives(), 3);
    EXPECT_EQ(g.score(), 0);
    EXPECT_EQ(g.level(), 1);
}

TEST(InitialState, Level1FormationIsThreeRowsOfEight) {
    Game g;
    startPlaying(g);
    EXPECT_EQ(g.alienRowsForTest(), 3);
    EXPECT_EQ(g.alienTotalForTest(), 24);
    EXPECT_EQ(g.alienCount(), 24);
}

TEST(InitialState, ShipCenteredNoShots) {
    Game g;
    startPlaying(g);
    EXPECT_FLOAT_EQ(g.shipX(), 0.0f);
    EXPECT_EQ(g.bulletCount(), 0);
    EXPECT_EQ(g.bombCount(), 0);
}

TEST(InitialState, RowTiersTopSquidBottomOctopus) {
    Game g;
    startPlaying(g);
    EXPECT_EQ(g.alienTypeForTest(slot(0, 0)), (int)Game::ALIEN_SQUID);
    EXPECT_EQ(g.alienTypeForTest(slot(1, 0)), (int)Game::ALIEN_CRAB);
    EXPECT_EQ(g.alienTypeForTest(slot(2, 0)), (int)Game::ALIEN_OCTOPUS);
}

// ── Level progression of the formation ────────────────────────────────────────

TEST(Levels, RowsGrowWithLevelAndCapAtFive) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(1);
    EXPECT_EQ(g.alienRowsForTest(), 3);
    g.startLevelForTest(3);
    EXPECT_EQ(g.alienRowsForTest(), 4);
    g.startLevelForTest(5);
    EXPECT_EQ(g.alienRowsForTest(), 5);
    g.startLevelForTest(9);
    EXPECT_EQ(g.alienRowsForTest(), 5);
    g.startLevelForTest(10);                // boss level: two-row escort only
    EXPECT_EQ(g.alienRowsForTest(), 2);
}

TEST(Levels, MarchSpeedGrowsWithLevel) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(1);
    float s1 = g.marchSpeedForTest();
    g.startLevelForTest(5);
    float s5 = g.marchSpeedForTest();
    EXPECT_GT(s5, s1);
}

TEST(Levels, MarchSpeedGrowsAsWaveThins) {
    Game g;
    startPlaying(g);
    float full = g.marchSpeedForTest();
    for (int i = 0; i < 12; i++) g.killAlienForTest(i);
    EXPECT_GT(g.marchSpeedForTest(), full);
}

// ── Marching ─────────────────────────────────────────────────────────────────

TEST(March, StartsMovingRight) {
    Game g;
    startPlaying(g);
    float x0 = g.alienXForTest(0);
    step(g, 0.5f);
    EXPECT_EQ(g.formationDirForTest(), 1);
    EXPECT_GT(g.alienXForTest(0), x0);
}

TEST(March, ReversesAndDescendsAtRightEdge) {
    Game g;
    startPlaying(g);
    // Park the wave hard against the right margin.
    g.setFormationXForTest(aspect());
    float y0 = g.formationYForTest();
    g.update(0.016f);
    EXPECT_EQ(g.formationDirForTest(), -1);
    EXPECT_GT(g.formationYForTest(), y0);
}

TEST(March, ReversesAndDescendsAtLeftEdge) {
    Game g;
    startPlaying(g);
    g.setFormationXForTest(aspect());
    g.update(0.016f);                       // now marching left
    ASSERT_EQ(g.formationDirForTest(), -1);
    g.setFormationXForTest(-aspect());
    float y0 = g.formationYForTest();
    g.update(0.016f);
    EXPECT_EQ(g.formationDirForTest(), 1);
    EXPECT_GT(g.formationYForTest(), y0);
}

TEST(March, StaysWithinSideMargins) {
    Game g;
    startPlaying(g);
    // Check continuously while the wave marches, not just at an endpoint (the
    // wave may legitimately invade before 30 s are up, which used to make
    // this test pass vacuously).
    for (int i = 0; i < 300 && g.isPlayingForTest(); i++) {
        step(g, 0.1f);
        for (int a = 0; a < g.alienTotalForTest(); a++) {
            if (!g.alienAliveForTest(a)) continue;
            // Centers reverse at asp - margin - halfWidth = asp - 0.053;
            // 0.05 leaves slack for a single frame of overshoot.
            ASSERT_LT(fabsf(g.alienXForTest(a)), aspect() - 0.05f)
                << "alien " << a << " out of margin after " << (i + 1) * 0.1f << " s";
        }
    }
}

TEST(March, InvasionEndsTheGame) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(0.72f);          // bottom row deep in the control strip
    g.update(0.016f);
    EXPECT_TRUE(g.isGameOverForTest());
    EXPECT_EQ(g.lives(), 0);
}

TEST(March, AlienCollisionCostsALifeAndKillsTheAlien) {
    Game g;
    startPlaying(g);
    g.clearInvulnForTest();
    // Put the bottom row exactly at the ship's altitude; the wave is centred so
    // columns 3/4 sit within collision range of the ship at x=0.
    g.setFormationYForTest(0.58f - 2 * 0.085f);
    g.update(0.016f);
    EXPECT_EQ(g.lives(), 2);
    EXPECT_LT(g.alienCount(), 24);
    EXPECT_TRUE(g.isPlayingForTest());
}

// ── Ship control (finger in the bottom strip) ────────────────────────────────

TEST(ShipControl, MovesTowardFingerInStrip) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, kW * 0.85f, ctrlPy());
    step(g, 0.3f);
    EXPECT_GT(g.shipX(), 0.05f);
}

TEST(ShipControl, IgnoresFingerAboveStrip) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, kW * 0.85f, kH * 0.5f);   // mid-screen, not the strip
    step(g, 0.3f);
    EXPECT_FLOAT_EQ(g.shipX(), 0.0f);
    EXPECT_EQ(g.bulletCount(), 0);               // and it doesn't fire either
}

TEST(ShipControl, StopsOnTheFingerX) {
    Game g;
    startPlaying(g);
    float targetWx = 0.2f;
    g.onPointerDown(0, pxOf(targetWx), ctrlPy());
    step(g, 1.5f);
    EXPECT_NEAR(g.shipX(), targetWx, 0.02f);
}

TEST(ShipControl, ClampedToScreenEdge) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, (float)kW, ctrlPy());
    step(g, 2.0f);
    EXPECT_LE(g.shipX(), aspect());
}

// A mid-session viewport change (fold/unfold, projected display) must clamp
// the ship back inside the new, narrower playfield.
TEST(ShipControl, ViewportShrinkClampsTheShip) {
    Game g;
    startPlaying(g);                            // 1080x2400: asp = 0.45
    g.setShipXForTest(aspect() - 0.05f);        // parked near the right edge
    g.setViewport(720, 2400);                   // asp shrinks to 0.30
    EXPECT_LT(g.shipX(), 0.30f);
}

// Dragging inside the strip is the primary steering gesture on the phone;
// the ship must follow the moved finger, not the original touch-down point.
TEST(ShipControl, DragSteersTheShip) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 0.5f);
    ASSERT_NEAR(g.shipX(), 0.0f, 0.02f);
    g.onPointerMove(0, pxOf(0.30f), ctrlPy());
    step(g, 1.5f);
    EXPECT_NEAR(g.shipX(), 0.30f, 0.02f);
}

// A system cancel (e.g. gesture-nav interception) must release all pointers:
// no phantom finger may keep steering or firing.
TEST(ShipControl, CancelReleasesAllPointers) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.30f), ctrlPy());
    step(g, 0.2f);                 // ship starts walking toward the finger
    g.onPointersCancel();
    float x = g.shipX();
    int bulletsAtCancel = g.bulletCount();
    step(g, 1.0f);
    EXPECT_FLOAT_EQ(g.shipX(), x);
    EXPECT_LE(g.bulletCount(), bulletsAtCancel);  // no new shots after cancel
}

TEST(ShipControl, NoTouchNoMovement) {
    Game g;
    startPlaying(g);
    step(g, 0.5f);
    EXPECT_FLOAT_EQ(g.shipX(), 0.0f);
}

// ── Firing ───────────────────────────────────────────────────────────────────

TEST(Firing, FingerInStripFires) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 0.1f);
    EXPECT_GE(g.bulletCount(), 1);
}

TEST(Firing, CooldownLimitsRate) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);        // park the wave far away — nothing to hit
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 0.30f);                       // cooldown is 0.42s: only one shot fits
    EXPECT_EQ(g.bulletCount(), 1);
}

TEST(Firing, AtMostThreeLasersInFlight) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    for (int i = 0; i < 300; i++) {
        g.update(1.0f / 60.0f);
        EXPECT_LE(g.bulletCount(), 3);
    }
}

TEST(Firing, LasersDespawnAtTheTop) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 0.1f);
    g.onPointerUp(0);
    ASSERT_GE(g.bulletCount(), 1);
    step(g, 1.5f);                        // plenty of time to cross the screen
    EXPECT_EQ(g.bulletCount(), 0);
}

// ── Killing invaders / scoring ────────────────────────────────────────────────

TEST(Kills, LaserKillsAnAlienAndScores) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    bool killed = false;
    for (int i = 0; i < 600 && !killed; i++) {
        g.setFormationXForTest(0.0f);     // hold the wave still over the ship
        g.update(1.0f / 60.0f);
        killed = g.alienCount() < 24;
    }
    EXPECT_TRUE(killed);
    EXPECT_GT(g.score(), 0);
}

// Regression: at the clamped worst-case dt (0.05 s) a bullet moves 0.12 per
// step — taller than an alien's whole hit box. Substepped collision must
// still land the kill instead of tunneling through the row.
TEST(Kills, LaserStillKillsAtClampedWorstCaseDt) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    bool killed = false;
    for (int i = 0; i < 200 && !killed; i++) {
        g.setFormationXForTest(0.0f);     // hold the wave still over the ship
        g.update(0.05f);                  // the dt clamp ceiling
        killed = g.alienCount() < 24;
    }
    EXPECT_TRUE(killed);
}

// Regression: bullet and bomb close by ~0.17 per clamped frame — nearly 3x
// their 0.06 collision diameter — so point-sampled collision whiffed head-on
// interceptions. Substepped bullets sample finely enough that the unsampled
// per-frame bomb move (< 0.06) can never leapfrog the hit window.
TEST(Kills, LaserInterceptsABombAtClampedWorstCaseDt) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);             // no aliens in the lane
    g.clearInvulnForTest();
    g.spawnTestBomb(0.0f, 0.10f, 0.9f);        // falling fast, dead ahead
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 1.0f, 0.05f);                      // the dt clamp ceiling
    EXPECT_EQ(g.bombCount(), 0);
    EXPECT_EQ(g.lives(), 3);                   // intercepted, not absorbed
}

TEST(Kills, BottomRowOctopusIsWorthTen) {
    Game g;
    startPlaying(g);
    // Only the octopus at (row 2, col 3) remains in its column lane; put the
    // ship right under it and hold fire.
    float colX = g.alienXForTest(slot(2, 3));
    g.setShipXForTest(colX);
    g.onPointerDown(0, pxOf(colX), ctrlPy());
    long before = g.score();
    int  aliveBefore = g.alienCount();
    for (int i = 0; i < 600 && g.alienCount() == aliveBefore; i++) {
        g.setFormationXForTest(0.0f);
        g.update(1.0f / 60.0f);
    }
    ASSERT_EQ(g.alienCount(), aliveBefore - 1);
    EXPECT_EQ(g.score() - before, 10L * g.level());
}

TEST(Kills, MiddleRowCrabIsWorthTwenty) {
    Game g;
    startPlaying(g);
    g.killAlienForTest(slot(2, 3));            // open the lane below the crab
    float colX = g.alienXForTest(slot(1, 3));
    g.setShipXForTest(colX);
    g.onPointerDown(0, pxOf(colX), ctrlPy());
    long before = g.score();
    int  aliveBefore = g.alienCount();
    for (int i = 0; i < 600 && g.alienCount() == aliveBefore; i++) {
        g.setFormationXForTest(0.0f);
        g.update(1.0f / 60.0f);
    }
    ASSERT_EQ(g.alienCount(), aliveBefore - 1);
    EXPECT_EQ(g.score() - before, 20L * g.level());
}

TEST(Kills, TopRowSquidIsWorthThirty) {
    Game g;
    startPlaying(g);
    // Clear the two aliens below the squid in column 3 so the laser lane is open.
    g.killAlienForTest(slot(1, 3));
    g.killAlienForTest(slot(2, 3));
    float colX = g.alienXForTest(slot(0, 3));
    g.setShipXForTest(colX);
    g.onPointerDown(0, pxOf(colX), ctrlPy());
    long before = g.score();
    bool killed = false;
    for (int i = 0; i < 600 && !killed; i++) {
        g.setFormationXForTest(0.0f);
        g.update(1.0f / 60.0f);
        killed = !g.alienAliveForTest(slot(0, 3));
    }
    ASSERT_TRUE(killed);
    EXPECT_EQ(g.score() - before, 30L * g.level());
}

// ── Bombs (alien lasers) ─────────────────────────────────────────────────────

TEST(Bombs, AliensEventuallyDropBombs) {
    Game g;
    startPlaying(g);
    bool seen = false;
    for (int i = 0; i < 600 && !seen; i++) {
        g.update(1.0f / 60.0f);
        seen = g.bombCount() > 0;
    }
    EXPECT_TRUE(seen);
}

TEST(Bombs, ConcurrentBombsAreCapped) {
    Game g;
    startPlaying(g);
    for (int i = 0; i < 900; i++) {
        g.update(1.0f / 60.0f);
        EXPECT_LE(g.bombCount(), 2);      // level-1 cap
        if (!g.isPlayingForTest()) break;
    }
}

TEST(Bombs, BombHitCostsALife) {
    Game g;
    startPlaying(g);
    g.clearInvulnForTest();
    g.spawnTestBomb(g.shipX(), 0.50f, 0.6f);
    step(g, 0.5f);
    EXPECT_EQ(g.lives(), 2);
}

TEST(Bombs, InvulnerabilityBlocksBackToBackHits) {
    Game g;
    startPlaying(g);
    g.clearInvulnForTest();
    g.spawnTestBomb(g.shipX(), 0.50f, 0.6f);
    step(g, 0.5f);
    ASSERT_EQ(g.lives(), 2);
    g.spawnTestBomb(g.shipX(), 0.50f, 0.6f);   // arrives well inside the window
    step(g, 0.4f);
    EXPECT_EQ(g.lives(), 2);
}

TEST(Bombs, MissedBombsDespawnOffscreen) {
    Game g;
    startPlaying(g);
    g.spawnTestBomb(0.3f, 0.90f, 1.0f);        // off to the side of the ship
    step(g, 0.5f);
    EXPECT_EQ(g.bombCount(), 0);
}

TEST(Bombs, LaserShootsDownABomb) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);             // no aliens in the lane
    g.clearInvulnForTest();
    // A slow bomb parked in the laser lane above the ship.
    g.spawnTestBomb(0.0f, 0.20f, 0.05f);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 0.6f);
    EXPECT_EQ(g.bombCount(), 0);
    EXPECT_EQ(g.lives(), 3);
}

TEST(Bombs, ThreeHitsEndTheGame) {
    Game g;
    startPlaying(g);
    for (int hit = 0; hit < 3; hit++) {
        g.clearInvulnForTest();
        g.spawnTestBomb(g.shipX(), 0.50f, 0.6f);
        step(g, 0.6f);
    }
    EXPECT_EQ(g.lives(), 0);
    EXPECT_TRUE(g.isGameOverForTest());
}

// The frame that kills the last life must not also score: updateBombs runs
// before updateBullets, and checkHighScore() saves the moment the game ends,
// so a laser landing later in the same frame would desync score from the
// saved high score. The game is deterministic (fixed RNG seed), so a control
// run pins down the exact frame the laser connects.
TEST(Bombs, DeathFrameDoesNotScore) {
    const float dt = 1.0f / 60.0f;

    // Shared choreography: burn down to one life, park the ship under a
    // bottom-row alien, and fire exactly one laser at it.
    auto prepare = [&](Game& g) {
        startPlaying(g);
        for (int hit = 0; hit < 2; hit++) {
            g.clearInvulnForTest();
            g.spawnTestBomb(g.shipX(), 0.50f, 0.6f);
            step(g, 0.6f);
        }
        float ax = g.alienXForTest(slot(2, 3));
        g.setShipXForTest(ax);
        g.onPointerDown(0, pxOf(ax), ctrlPy());
        g.update(dt);                              // one shot; cooldown blocks more
        g.onPointerUp(0);
    };

    // Control run: find the frame on which the laser connects.
    Game control;
    prepare(control);
    ASSERT_EQ(control.lives(), 1);
    ASSERT_EQ(control.bulletCount(), 1);
    long before = control.score();
    int killFrame = -1;
    for (int f = 0; f < 120; f++) {
        control.update(dt);
        if (control.score() != before) { killFrame = f; break; }
    }
    ASSERT_GE(killFrame, 1) << "control laser never connected";

    // Real run: identical inputs, but a bomb lands the fatal hit earlier in
    // the laser's kill frame.
    Game g;
    prepare(g);
    long preDeath = g.score();
    for (int f = 0; f < killFrame; f++) g.update(dt);
    ASSERT_TRUE(g.isPlayingForTest());
    g.clearInvulnForTest();
    g.spawnTestBomb(g.shipX(), g.shipY(), 0.01f);  // on the ship: hits this frame
    g.update(dt);
    ASSERT_TRUE(g.isGameOverForTest());
    EXPECT_EQ(g.lives(), 0);
    EXPECT_EQ(g.score(), preDeath) << "scored after the fatal hit";
}

// ── Saucer ───────────────────────────────────────────────────────────────────

TEST(Saucer, CrossesAndDespawns) {
    Game g;
    startPlaying(g);
    g.spawnSaucerForTest();
    ASSERT_TRUE(g.saucerAliveForTest());
    float x0 = g.saucerXForTest();
    step(g, 0.5f);
    EXPECT_NE(g.saucerXForTest(), x0);
    step(g, 8.0f);
    EXPECT_FALSE(g.saucerAliveForTest());
}

TEST(Saucer, ShootingItPaysTheBonus) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);             // clear the lane
    g.setSaucerForTest(0.0f, 0.0f);            // parked right above the ship
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    long before = g.score();
    bool killed = false;
    for (int i = 0; i < 300 && !killed; i++) {
        g.update(1.0f / 60.0f);
        killed = !g.saucerAliveForTest();
    }
    ASSERT_TRUE(killed);
    EXPECT_EQ(g.score() - before, 100L * g.level());
}

// ── Power-ups ────────────────────────────────────────────────────────────────

TEST(PowerUps, CollectingShieldActivatesIt) {
    Game g;
    startPlaying(g);
    g.spawnTestPowerUp(g.shipX(), 0.45f, (int)Game::PU_SHIELD);
    long before = g.score();
    step(g, 0.8f);
    EXPECT_TRUE(g.shieldActiveForTest());
    EXPECT_EQ(g.score() - before, 25L * g.level());
}

TEST(PowerUps, ShieldAbsorbsOneHit) {
    Game g;
    startPlaying(g);
    g.clearInvulnForTest();
    g.activatePowerUpForTest((int)Game::PU_SHIELD);
    g.spawnTestBomb(g.shipX(), 0.50f, 0.6f);
    step(g, 0.5f);
    EXPECT_EQ(g.lives(), 3);
    EXPECT_FALSE(g.shieldActiveForTest());
}

// Regression: two hits landing in the same frame (or back to back before any
// grace period) must cost only the shield, never the shield AND a life.
TEST(PowerUps, ShieldHitGrantsGracePeriod) {
    Game g;
    startPlaying(g);
    g.clearInvulnForTest();
    g.activatePowerUpForTest((int)Game::PU_SHIELD);
    g.spawnTestBomb(g.shipX(), g.shipY(), 0.01f);  // both on the ship:
    g.spawnTestBomb(g.shipX(), g.shipY(), 0.01f);  // hit the same frame
    g.update(1.0f / 60.0f);
    EXPECT_FALSE(g.shieldActiveForTest());
    EXPECT_EQ(g.lives(), 3);
}

TEST(PowerUps, ShieldPersistsUntilHit) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);             // bombs spawn too far up to arrive
    g.activatePowerUpForTest((int)Game::PU_SHIELD);
    step(g, 3.0f);
    EXPECT_TRUE(g.shieldActiveForTest());
}

TEST(PowerUps, RapidFireShortensTheCooldown) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);
    g.activatePowerUpForTest((int)Game::PU_RAPID);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 0.30f);                            // rapid cooldown 0.2s: two shots fit
    EXPECT_GE(g.bulletCount(), 2);
}

TEST(PowerUps, TripleShotFiresThreeAngledLasers) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);
    g.activatePowerUpForTest((int)Game::PU_TRIPLE);
    ASSERT_TRUE(g.tripleActiveForTest());
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 0.1f);                              // one volley within the cooldown
    ASSERT_EQ(g.bulletCount(), 3);
    // One straight, one left (+8°), one right (-8°).
    int left = 0, right = 0, straight = 0;
    for (int i = 0; i < 3; i++) {
        float vx = g.bulletVxForTest(i);
        if (vx < -0.05f) left++;
        else if (vx > 0.05f) right++;
        else straight++;
    }
    EXPECT_EQ(straight, 1);
    EXPECT_EQ(left, 1);
    EXPECT_EQ(right, 1);
}

TEST(PowerUps, TripleShotSideLasersLeaveAtEightDegrees) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);
    g.activatePowerUpForTest((int)Game::PU_TRIPLE);
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    step(g, 0.1f);
    ASSERT_EQ(g.bulletCount(), 3);
    // |vx| of the side lasers must equal sin(8°) x bullet speed (~0.334).
    const float expected = 2.40f * sinf(8.0f * 3.14159265f / 180.0f);
    float minVx = 1e9f, maxVx = -1e9f;
    for (int i = 0; i < 3; i++) {
        float vx = g.bulletVxForTest(i);
        if (vx < minVx) minVx = vx;
        if (vx > maxVx) maxVx = vx;
    }
    EXPECT_NEAR(minVx, -expected, 0.01f);
    EXPECT_NEAR(maxVx,  expected, 0.01f);
}

TEST(PowerUps, CollectingTripleActivatesIt) {
    Game g;
    startPlaying(g);
    g.spawnTestPowerUp(g.shipX(), 0.45f, (int)Game::PU_TRIPLE);
    step(g, 0.8f);
    EXPECT_TRUE(g.tripleActiveForTest());
}

TEST(PowerUps, TripleExpires) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);
    g.activatePowerUpForTest((int)Game::PU_TRIPLE);
    ASSERT_TRUE(g.tripleActiveForTest());
    step(g, 9.0f);
    EXPECT_FALSE(g.tripleActiveForTest());
}

TEST(PowerUps, RapidFireExpires) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(-3.0f);
    g.activatePowerUpForTest((int)Game::PU_RAPID);
    ASSERT_TRUE(g.rapidActiveForTest());
    step(g, 9.0f);
    EXPECT_FALSE(g.rapidActiveForTest());
}

// ── Level clear / win ────────────────────────────────────────────────────────

TEST(Progression, ClearingTheWavePaysABonusAndAdvances) {
    Game g;
    startPlaying(g);
    long before = g.score();
    for (int i = 0; i < g.alienTotalForTest(); i++) g.killAlienForTest(i);
    g.update(0.016f);
    EXPECT_TRUE(g.isLevelClearForTest());
    EXPECT_EQ(g.score() - before, 100L * 1);
    step(g, 2.0f);
    EXPECT_TRUE(g.isPlayingForTest());
    EXPECT_EQ(g.level(), 2);
    EXPECT_EQ(g.alienCount(), 24);             // level 2 is still 3 rows of 8
}

TEST(Progression, ClearingLevelNineAdvancesToTen) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(9);
    for (int i = 0; i < g.alienTotalForTest(); i++) g.killAlienForTest(i);
    g.update(0.016f);
    ASSERT_TRUE(g.isLevelClearForTest());
    step(g, 2.0f);
    EXPECT_TRUE(g.isPlayingForTest());
    EXPECT_EQ(g.level(), 10);
    EXPECT_TRUE(g.bossActiveForTest());
}

// ── Boss mothership (level 10) ───────────────────────────────────────────────

TEST(Boss, SpawnsAtLevelTenWithFullHealth) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(10);
    EXPECT_TRUE(g.bossActiveForTest());
    EXPECT_TRUE(g.bossAliveForTest());
    EXPECT_EQ(g.bossHpForTest(), 16);
    g.startLevelForTest(9);
    EXPECT_FALSE(g.bossActiveForTest());
}

TEST(Boss, DriftsAcrossTheTop) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(10);
    step(g, 1.0f);
    float x1 = g.bossXForTest();
    step(g, 1.0f);
    EXPECT_NE(g.bossXForTest(), x1);
}

TEST(Boss, ClearingTheEscortDoesNotClearTheLevel) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(10);
    for (int i = 0; i < g.alienTotalForTest(); i++) g.killAlienForTest(i);
    step(g, 0.5f);
    EXPECT_TRUE(g.isPlayingForTest());     // boss still up: no LEVEL_CLEAR
}

TEST(Boss, FiresAimedBombs) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(10);
    // Remove the escort so the only bombs left are the boss's.
    for (int i = 0; i < g.alienTotalForTest(); i++) g.killAlienForTest(i);
    bool seen = false;
    for (int i = 0; i < 300 && !seen; i++) {
        g.update(1.0f / 60.0f);
        seen = g.bombCount() > 0;
    }
    EXPECT_TRUE(seen);
}

TEST(Boss, TakesHitsAndDiesForTheWin) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(10);
    // Remove the escort so lasers have a clear lane to the mothership.
    for (int i = 0; i < g.alienTotalForTest(); i++) g.killAlienForTest(i);
    g.activatePowerUpForTest((int)Game::PU_RAPID);   // speeds the test up
    long before = g.score();
    int hp0 = g.bossHpForTest();
    g.onPointerDown(0, pxOf(0.0f), ctrlPy());
    bool killed = false;
    for (int i = 0; i < 7200 && !killed; i++) {
        g.setBossTimeForTest(0.0f);        // park the boss over the ship
        g.activatePowerUpForTest((int)Game::PU_RAPID);
        g.clearInvulnForTest();
        if (!g.isPlayingForTest() && !g.isWinForTest()) break;  // died to a bomb
        g.update(1.0f / 60.0f);
        killed = !g.bossAliveForTest();
    }
    ASSERT_TRUE(killed);
    EXPECT_LT(g.bossHpForTest(), hp0);
    EXPECT_TRUE(g.isWinForTest());
    EXPECT_GE(g.score() - before, 250L * 10);
}

TEST(Boss, AutoPlayTargetsTheBoss) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(10);
    g.setAutoPlayForTest(true);
    for (int i = 0; i < g.alienTotalForTest(); i++) g.killAlienForTest(i);
    g.update(0.016f);
    EXPECT_TRUE(g.aiHasTargetForTest());
}

TEST(Progression, GameOverTapReturnsToTitle) {
    Game g;
    startPlaying(g);
    g.setFormationYForTest(0.72f);
    g.update(0.016f);
    ASSERT_TRUE(g.isGameOverForTest());
    step(g, 1.0f);
    tap(g, kW * 0.5f, kH * 0.5f);
    g.update(0.016f);
    EXPECT_TRUE(g.inTitleForTest());
}

// ── Settings ─────────────────────────────────────────────────────────────────

TEST(Settings, GearTapOpensSettingsFromTitle) {
    Game g;
    g.setViewport(kW, kH);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));
    g.update(0.016f);
    EXPECT_TRUE(g.inSettingsForTest());
}

TEST(Settings, GearTapOpensSettingsWhilePlaying) {
    Game g;
    startPlaying(g);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));
    g.update(0.016f);
    EXPECT_TRUE(g.inSettingsForTest());
}

TEST(Settings, SoundRowToggles) {
    Game g;
    g.setViewport(kW, kH);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));
    g.update(0.016f);
    ASSERT_TRUE(g.inSettingsForTest());
    ASSERT_TRUE(g.soundEnabledForTest());
    tap(g, pxOf(0.0f), pyOf(-0.15f));           // sound row
    g.update(0.016f);
    EXPECT_FALSE(g.soundEnabledForTest());
}

TEST(Settings, AutoPlayRowToggles) {
    Game g;
    g.setViewport(kW, kH);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));
    g.update(0.016f);
    ASSERT_FALSE(g.autoPlayActiveForTest());
    tap(g, pxOf(0.0f), pyOf(0.10f));            // auto play row
    g.update(0.016f);
    EXPECT_TRUE(g.autoPlayActiveForTest());
}

TEST(Settings, BackReturnsToPreviousState) {
    Game g;
    startPlaying(g);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));
    g.update(0.016f);
    ASSERT_TRUE(g.inSettingsForTest());
    tap(g, pxOf(0.0f), pyOf(0.66f));            // back button
    g.update(0.016f);
    EXPECT_TRUE(g.isPlayingForTest());
}

// ── AI Glasses: touchbar control mode + settings hand-off ────────────────────

TEST(Glasses, TouchbarSteersAndFiresFromAnywhere) {
    Game g;
    startPlaying(g);
    g.setControlMode(Game::CONTROL_TOUCHBAR);
    g.setFormationYForTest(-3.0f);
    // Mid-screen touch — ignored by the phone strip, but the whole surface is
    // the touchbar on the glasses.
    g.onPointerDown(0, pxOf(0.25f), kH * 0.4f);
    step(g, 0.4f);
    EXPECT_GT(g.shipX(), 0.05f);
    EXPECT_GE(g.bulletCount(), 1);
}

TEST(Glasses, StripModeStillIgnoresMidScreenTouch) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.25f), kH * 0.4f);
    step(g, 0.4f);
    EXPECT_FLOAT_EQ(g.shipX(), 0.0f);
    EXPECT_EQ(g.bulletCount(), 0);
}

TEST(Glasses, NoGearOnTheGlasses) {
    Game g;
    g.setViewport(kW, kH);
    g.setControlMode(Game::CONTROL_TOUCHBAR);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));   // gear position
    g.update(0.016f);
    EXPECT_FALSE(g.inSettingsForTest());
}

TEST(Glasses, TouchbarClearColorIsPureBlack) {
    Game g;
    g.setViewport(kW, kH);
    g.setControlMode(Game::CONTROL_TOUCHBAR);
    float c[3];
    g.clearColor(c);
    EXPECT_FLOAT_EQ(c[0], 0.0f);
    EXPECT_FLOAT_EQ(c[1], 0.0f);
    EXPECT_FLOAT_EQ(c[2], 0.0f);
}

TEST(Glasses, SettingsRowLaunchesWhenConnected) {
    Game g;
    g.setViewport(kW, kH);
    bool launched = false;
    g.setGlassesLaunchCallback([&]() { launched = true; return true; });
    g.setGlassesConnected(true);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));   // open settings
    g.update(0.016f);
    ASSERT_TRUE(g.inSettingsForTest());
    tap(g, pxOf(0.0f), pyOf(0.35f));                 // glasses row
    g.update(0.016f);
    EXPECT_TRUE(launched);
}

TEST(Glasses, SettingsRowInertWhenNotConnected) {
    Game g;
    g.setViewport(kW, kH);
    bool launched = false;
    g.setGlassesLaunchCallback([&]() { launched = true; return true; });
    g.setGlassesConnected(false);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));
    g.update(0.016f);
    tap(g, pxOf(0.0f), pyOf(0.35f));
    g.update(0.016f);
    EXPECT_FALSE(launched);
    EXPECT_TRUE(g.inSettingsForTest());              // stayed put, no crash
}

TEST(Glasses, SettingsRowExitsWhenActive) {
    Game g;
    g.setViewport(kW, kH);
    bool exited = false;
    g.setGlassesExitCallback([&]() { exited = true; });
    g.setGlassesConnected(true);
    g.setGlassesActive(true);
    tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));
    g.update(0.016f);
    tap(g, pxOf(0.0f), pyOf(0.35f));
    g.update(0.016f);
    EXPECT_TRUE(exited);
}

TEST(Glasses, PhoneGameplayFreezesWhileOnGlasses) {
    Game g;
    startPlaying(g);
    g.setGlassesActive(true);
    g.spawnTestBomb(0.3f, 0.0f, 0.6f);
    long score0 = g.score();
    float alienX0 = g.alienXForTest(0);
    step(g, 1.0f);
    EXPECT_EQ(g.bombCount(), 1);                     // bomb frozen mid-air
    EXPECT_FLOAT_EQ(g.alienXForTest(0), alienX0);    // wave frozen
    EXPECT_EQ(g.score(), score0);
    // Hand the game back: everything moves again.
    g.setGlassesActive(false);
    step(g, 1.0f);
    EXPECT_NE(g.alienXForTest(0), alienX0);
}

TEST(Settings, PersistAcrossInstances) {
    const char* dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/data/local/tmp";
    char path[512];
    snprintf(path, sizeof(path), "%s/settings.bin", dir);
    remove(path);

    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        tap(g, pxOf(aspect() - 0.062f), pyOf(-0.82f));
        g.update(0.016f);
        tap(g, pxOf(0.0f), pyOf(-0.15f));       // sound off
        g.update(0.016f);
        tap(g, pxOf(0.0f), pyOf(0.10f));        // auto play on
        g.update(0.016f);
        ASSERT_FALSE(g.soundEnabledForTest());
        ASSERT_TRUE(g.autoPlayActiveForTest());
    }
    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        EXPECT_FALSE(g.soundEnabledForTest());
        EXPECT_TRUE(g.autoPlayActiveForTest());
    }
    remove(path);
}

// ── High scores ──────────────────────────────────────────────────────────────

// The table must insert by rank, displace lower entries, and cap at five —
// previously only slot 0 was ever asserted.
TEST(HighScores, TableOrdersDisplacesAndCaps) {
    Game g;
    startPlaying(g);   // dataPath unset: saves are no-ops, table is in-memory
    const long runs[6] = {300, 100, 500, 200, 400, 250};
    for (long r : runs) {
        ASSERT_TRUE(g.isPlayingForTest());
        g.setScoreForTest(r);
        g.setFormationYForTest(0.72f);         // invasion => instant game over
        g.update(1.0f / 60.0f);
        ASSERT_TRUE(g.isGameOverForTest());
        step(g, 0.7f);                         // GAME_OVER accepts taps after 0.6 s
        tap(g, kW * 0.5f, kH * 0.5f);
        g.update(1.0f / 60.0f);                // -> TITLE
        ASSERT_TRUE(g.inTitleForTest());
        tap(g, kW * 0.5f, kH * 0.5f);
        g.update(1.0f / 60.0f);                // -> fresh PLAYING run
    }
    EXPECT_EQ(g.highScoreForTest(0), 500);
    EXPECT_EQ(g.highScoreForTest(1), 400);
    EXPECT_EQ(g.highScoreForTest(2), 300);
    EXPECT_EQ(g.highScoreForTest(3), 250);
    EXPECT_EQ(g.highScoreForTest(4), 200);     // the 100 was displaced off
}

TEST(HighScores, SavedOnGameOverAndReloaded) {
    const char* dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/data/local/tmp";
    char path[512];
    snprintf(path, sizeof(path), "%s/highscores.bin", dir);
    remove(path);

    long finalScore = 0;
    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        g.triggerNewGameForTest();
        g.killAlienForTest(slot(2, 3));         // it doesn't score…
        g.spawnTestPowerUp(g.shipX(), 0.45f, (int)Game::PU_RAPID);
        step(g, 0.8f);                          // …but collecting this does
        ASSERT_GT(g.score(), 0);
        finalScore = g.score();
        g.setFormationYForTest(0.72f);          // invade -> game over -> save
        g.update(0.016f);
        ASSERT_TRUE(g.isGameOverForTest());
    }
    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        EXPECT_EQ(g.highScoreForTest(0), finalScore);
    }
    remove(path);
}

// ── Auto-play ────────────────────────────────────────────────────────────────

TEST(AutoPlay, FiresOnItsOwn) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    bool fired = false;
    for (int i = 0; i < 300 && !fired; i++) {
        g.update(1.0f / 60.0f);
        fired = g.bulletCount() > 0;
    }
    EXPECT_TRUE(fired);
}

TEST(AutoPlay, SteersAwayFromAnIncomingBomb) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setFormationYForTest(-3.0f);              // nothing else to react to
    g.spawnTestBomb(g.shipX(), 0.30f, 0.5f);
    g.update(0.016f);
    ASSERT_TRUE(g.aiHasTargetForTest());
    EXPECT_GT(fabsf(g.aiTargetXForTest() - 0.0f), 0.15f);
    // And the dodge actually works: the bomb passes without costing a life.
    g.clearInvulnForTest();
    step(g, 2.0f);
    EXPECT_EQ(g.lives(), 3);
}

TEST(AutoPlay, SteersTowardACollectiblePowerUp) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setFormationYForTest(-3.0f);              // aliens too far to matter
    g.spawnTestPowerUp(0.25f, 0.30f, (int)Game::PU_SHIELD);
    g.update(0.016f);
    ASSERT_TRUE(g.aiHasTargetForTest());
    EXPECT_NEAR(g.aiTargetXForTest(), 0.25f, 0.03f);
}

TEST(AutoPlay, LeadsTheSaucer) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setFormationYForTest(-3.0f);
    g.setSaucerForTest(-0.2f, 0.35f);
    g.update(0.016f);
    ASSERT_TRUE(g.aiHasTargetForTest());
    // The aim point must lead the saucer in its direction of travel.
    EXPECT_GT(g.aiTargetXForTest(), -0.2f);
}

TEST(AutoPlay, ClearsAWaveEventually) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    // Give the autopilot a nearly-cleared wave; it must finish the job.
    for (int i = 0; i < g.alienTotalForTest() - 2; i++) g.killAlienForTest(i);
    for (int i = 0; i < 3600 && g.isPlayingForTest(); i++)
        g.update(1.0f / 60.0f);
    // The autopilot must actually clear the wave — advancing past it is the
    // only acceptable outcome. Accepting "died trying" here would let an
    // autopilot regression that gets the ship killed instantly still pass.
    EXPECT_TRUE(g.isLevelClearForTest() || g.level() > 1)
        << "after 60 s: playing=" << g.isPlayingForTest()
        << " gameOver=" << g.isGameOverForTest()
        << " aliens=" << g.alienCount();
}

// ── Render coverage ───────────────────────────────────────────────────────────

// render() is ~600 lines of state-dependent drawing that the unit suite never
// exercised: any crash there was invisible until a device run. Drive it
// through every state reachable from the test hooks and require output.
TEST(Render, ProducesOutputInEveryReachableState) {
    Game g;
    g.setViewport(kW, kH);
    std::vector<DrawCmd> cmds;

    g.render(cmds);                                    // TITLE (+ demo wave)
    EXPECT_GT(cmds.size(), 0u) << "TITLE drew nothing";

    startPlaying(g);
    step(g, 0.2f);
    cmds.clear(); g.render(cmds);                      // PLAYING (HUD, wave)
    EXPECT_GT(cmds.size(), 0u) << "PLAYING drew nothing";

    g.startLevelForTest(10);                           // boss fight (health bar)
    step(g, 0.5f);
    cmds.clear(); g.render(cmds);
    EXPECT_GT(cmds.size(), 0u) << "BOSS level drew nothing";

    for (int hit = 0; hit < 3; hit++) {                // GAME_OVER (score text)
        g.clearInvulnForTest();
        g.spawnTestBomb(g.shipX(), 0.50f, 0.6f);
        step(g, 0.6f);
    }
    ASSERT_TRUE(g.isGameOverForTest());
    step(g, 0.5f);
    cmds.clear(); g.render(cmds);
    EXPECT_GT(cmds.size(), 0u) << "GAME_OVER drew nothing";
}
