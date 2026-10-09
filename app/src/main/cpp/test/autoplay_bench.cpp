// Auto Play bench: plays seeded, headless campaigns with the autopilot and
// reports how often it clears each level, how long it takes and how it dies.
// Every Auto Play change is measured here before it ships (the numbers in the
// commit messages come from this program).
//
// Builds on the host, no Android SDK needed: scripts/autoplay-bench.sh.
//
//   autoplay_bench [--runs N] [--width W --height H] [--levels] [--campaigns]
//                  [--min-campaign-win PCT]
//
// --levels    plays each level 1..10 on its own, N seeds each, starting with
//             three lives (the default when nothing is selected).
// --campaigns plays N full campaigns from level 1.
// --min-campaign-win PCT exits 1 when fewer than PCT% of campaigns are won:
//             the CI regression guard.
// --trace     prints the scene (ship, autopilot intent, bombs) at every death.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include "game.h"

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kLevelTimeout = 300.0f;     // seconds before a level counts as stuck
bool gTrace = false;                        // --trace: dump the scene at every death

struct Stats {
    int runs = 0, cleared = 0;
    int deathsBomb = 0, deathsCollision = 0, invasions = 0, timeouts = 0;
    double seconds = 0.0;     // summed over cleared runs
    long score = 0;
    long shots = 0, kills = 0, lost = 0, bombsShot = 0;
    double idle = 0.0;        // seconds the trigger was ready but not pulled
};

void dumpScene(const Game& g, const char* cause, float t) {
    float floorY = -9.0f;
    int alive = 0;
    for (int i = 0; i < g.alienTotalForTest(); i++) {
        if (!g.alienAliveForTest(i)) continue;
        alive++;
        if (g.alienYForTest(i) > floorY) floorY = g.alienYForTest(i);
    }
    printf("  death[%s] L%d t=%.1f ship x=%.3f aiTarget=%.3f move=%d fire=%d alive=%d floorY=%.2f shield=%d\n",
           cause, g.level(), t, g.shipX(), g.aiTargetXForTest(), g.aiHasTargetForTest(),
           g.aiFireForTest(), alive, floorY, g.shieldActiveForTest());
    for (int i = 0; i < g.bombsForTest(); i++)
        printf("    bomb x=%.3f y=%.3f vx=%.2f vy=%.2f (dx=%.3f, tHit=%.2f)\n",
               g.bombXForTest(i), g.bombYForTest(i), g.bombVxForTest(i), g.bombVyForTest(i),
               g.bombXForTest(i) - g.shipX(), (g.shipY() - g.bombYForTest(i)) / g.bombVyForTest(i));
}

// A life lost to an invader rather than a bomb: an alien sat inside the
// ship's collision box just before the frame that took the life.
bool alienOnShip(const Game& g) {
    for (int i = 0; i < g.alienTotalForTest(); i++) {
        if (!g.alienAliveForTest(i)) continue;
        if (fabsf(g.alienXForTest(i) - g.shipX()) < 0.038f + 0.050f * 0.8f + 0.02f &&
            fabsf(g.alienYForTest(i) - g.shipY()) < 0.030f + 0.050f * 0.8f + 0.02f)
            return true;
    }
    return false;
}

// Plays from the current PLAYING state until the level ends or the game does.
// Returns true when the level was cleared (LEVEL_CLEAR or WIN).
bool playLevel(Game& g, Stats& s, float& seconds) {
    seconds = 0.0f;
    int lives = g.lives();
    int shots0 = g.shotsFiredForTest(), aliens0 = g.alienCount();
    int lost0 = g.bulletsLostForTest(), bombsShot0 = g.bombsShotForTest();
    std::optional<Game> before;        // the frame before a death, for --trace
    while (g.isPlayingForTest()) {
        bool touching = alienOnShip(g);
        if (gTrace) before.emplace(g);
        g.update(kDt);
        if (g.fireReadyForTest() && !g.aiFireForTest()) s.idle += kDt;
        seconds += kDt;
        if (g.lives() < lives) {
            const char* cause;
            if (g.isGameOverForTest() && lives - g.lives() > 1) { s.invasions++; cause = "invasion"; }
            else if (touching) { s.deathsCollision++; cause = "collision"; }
            else { s.deathsBomb++; cause = "bomb"; }
            if (gTrace) dumpScene(*before, cause, seconds);
            lives = g.lives();
        }
        if (seconds > kLevelTimeout) { s.timeouts++; return false; }
    }
    s.shots += g.shotsFiredForTest() - shots0;
    s.kills += aliens0 - g.alienCount();
    s.lost += g.bulletsLostForTest() - lost0;
    s.bombsShot += g.bombsShotForTest() - bombsShot0;
    return g.isLevelClearForTest() || g.isWinForTest();
}

void printStats(const char* label, const Stats& s) {
    double avg = s.cleared ? s.seconds / s.cleared : 0.0;
    printf("%-10s cleared %5.1f%%  deaths/run bomb %.2f collision %.2f  invasions %3d"
           "  timeouts %2d  avg %6.1f s  idle %4.1f s  shots/kill %.2f  lost/run %.1f"
           "  bombs shot/run %.1f  avg score %7ld\n",
           label, 100.0 * s.cleared / s.runs,
           (double)s.deathsBomb / s.runs, (double)s.deathsCollision / s.runs,
           s.invasions, s.timeouts, avg, s.idle / s.runs,
           s.kills ? (double)s.shots / s.kills : 0.0,
           (double)s.lost / s.runs, (double)s.bombsShot / s.runs,
           s.runs ? s.score / s.runs : 0L);
}

}  // namespace

int main(int argc, char** argv) {
    int runs = 200, w = 1080, h = 2400;
    bool levels = false, campaigns = false;
    double minCampaignWin = -1.0;
    for (int i = 1; i < argc; i++) {
        auto next = [&](int& dst) { if (i + 1 < argc) dst = atoi(argv[++i]); };
        if      (!strcmp(argv[i], "--runs"))      next(runs);
        else if (!strcmp(argv[i], "--width"))     next(w);
        else if (!strcmp(argv[i], "--height"))    next(h);
        else if (!strcmp(argv[i], "--levels"))    levels = true;
        else if (!strcmp(argv[i], "--campaigns")) campaigns = true;
        else if (!strcmp(argv[i], "--trace"))     gTrace = true;
        else if (!strcmp(argv[i], "--min-campaign-win") && i + 1 < argc)
            minCampaignWin = atof(argv[++i]);
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (minCampaignWin >= 0.0) campaigns = true;
    if (!levels && !campaigns) levels = true;
    if (runs <= 0 || w <= 0 || h <= 0) { fprintf(stderr, "bad size or run count\n"); return 2; }
    printf("Auto Play bench: %dx%d, %d seeded runs\n", w, h, runs);

    if (levels) {
        Stats total;
        for (int L = 1; L <= 10; L++) {
            Stats s;
            for (int seed = 1; seed <= runs; seed++) {
                Game g;
                g.setViewport(w, h);
                g.seedRng((uint32_t)seed * 2654435761u);
                g.triggerNewGameForTest();
                if (L > 1) g.startLevelForTest(L);
                g.setAutoPlayForTest(true);
                float sec;
                s.runs++;
                if (playLevel(g, s, sec)) { s.cleared++; s.seconds += sec; }
                s.score += g.score();
            }
            char label[16];
            snprintf(label, sizeof(label), "level %2d", L);
            printStats(label, s);
            total.runs += s.runs; total.cleared += s.cleared;
            total.deathsBomb += s.deathsBomb; total.deathsCollision += s.deathsCollision;
            total.invasions += s.invasions; total.timeouts += s.timeouts;
            total.seconds += s.seconds; total.score += s.score;
            total.shots += s.shots; total.kills += s.kills;
            total.lost += s.lost; total.bombsShot += s.bombsShot; total.idle += s.idle;
        }
        printStats("all", total);
    }

    int won = 0;
    if (campaigns) {
        Stats s;
        double wonSeconds = 0.0;
        int reached[11] = {};
        for (int seed = 1; seed <= runs; seed++) {
            Game g;
            g.setViewport(w, h);
            g.seedRng((uint32_t)seed * 2654435761u);
            g.triggerNewGameForTest();
            g.setAutoPlayForTest(true);
            float total = 0.0f;
            s.runs++;
            bool alive = true;
            while (alive) {
                float sec;
                reached[g.level()]++;
                bool ok = playLevel(g, s, sec);
                total += sec;
                if (!ok) { alive = false; break; }
                if (g.isWinForTest()) break;
                // LEVEL_CLEAR: let the interlude run into the next level.
                while (g.isLevelClearForTest()) g.update(kDt);
            }
            if (g.isWinForTest()) { won++; wonSeconds += total; }
            s.score += g.score();
        }
        printf("campaigns  won %5.1f%%  deaths/run bomb %.2f collision %.2f  invasions %3d"
               "  timeouts %2d  avg won %6.1f s  shots/kill %.2f  avg score %7ld\n",
               100.0 * won / s.runs,
               (double)s.deathsBomb / s.runs, (double)s.deathsCollision / s.runs,
               s.invasions, s.timeouts, won ? wonSeconds / won : 0.0,
               s.kills ? (double)s.shots / s.kills : 0.0, s.score / s.runs);
        printf("reached    ");
        for (int L = 1; L <= 10; L++) printf("L%d %3d  ", L, reached[L]);
        printf("\n");
        if (minCampaignWin >= 0.0 && 100.0 * won / s.runs < minCampaignWin) {
            printf("FAIL: campaign win rate below %.1f%%\n", minCampaignWin);
            return 1;
        }
    }
    return 0;
}
