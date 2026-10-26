#include "src/main.cpp"
#include <algorithm>
#include <cstdio>
#include <cmath>
static int fails = 0;
#define CHECK(c, msg) do { if(!(c)) { std::printf("FAIL: %s\n", msg); ++fails; } else std::printf("ok:   %s\n", msg);} while(0)
static const float DT = 1.f/240.f;

struct World {
    BufferPlayer P, P2; BufferGameLayer L; BufferRingLayer R; BufferSimGuards G; BufferPlayLayer PL; BufferPauseLayer PZ;
    GJGameLevel level;
    World(int ticks = 8) {
        g_ints["ticks"] = ticks; for (auto k : {"ball-ticks","robot-ticks","spider-ticks","ufo-ticks"}) g_ints[k] = -1;
        g_ints["ship-ticks"] = 4; g_ints["wave-ticks"] = 4; g_ints["swing-ticks"] = 4; g_ints["assist-budget-ms"] = 50; g_ints["assist-margin"] = 12;
        g_bools["buffer-rings"] = true; g_bools["show-counter"] = true; g_bools["assist-enabled"] = true;
        g_bools["assist-dual"] = false; g_bools["assist-autopause"] = true; g_bools["practice-only"] = false;
        g_strings["assist-pause-with"] = ""; g_saved.clear(); g_loadedMods.clear();
        g_calls.clear(); g_layer = &L; L.m_player1 = &P; L.m_player2 = nullptr; L.m_gameState.m_isDualMode = false; L.m_isPracticeMode = false;
        L.m_level = &level; PL.m_level = &level; PL.m_player1 = &P; PL.m_player2 = &P2;
        g_world = nullptr; g_realDeaths = 0; g_createCount = 0;
        for (auto& b : g_buffers) b.reset(); g_attemptReplays = 0; g_totalReplays = 0;
        g_delayed.clear(); g_delayed2.clear(); g_pair = Pair{}; g_dummies[0] = g_dummies[1] = nullptr; g_dummyLayer = nullptr; g_simulating = false;
        g_timeoutStreak = 0; g_assistSuspended = false; g_conflictPause = false; g_tickCostNs = 0; g_profile = -1;
        resetLevelStats(); g_logs.clear();
        P.m_isOnGround = false;
    }
    void tick(int n = 1) { for (int i = 0; i < n; ++i) P.update(DT); }
    void press()   { L.handleButton(true,  1, true); }
    void release() { L.handleButton(false, 1, true); }
    void land()    { P.m_isOnGround = true; }
    void ring()    { R.playerTouchedRing(&P, nullptr); }
    // wave with a "spike" at x 5..6, y >= 4: a raw press at tick 0 crashes, a press 2+ ticks later survives
    void spike() { g_world = [this](PlayerObject* p){ if (p->pos.x >= 5 && p->pos.x <= 6 && p->pos.y >= 4) PL.destroyPlayer(p, nullptr); }; }
    void wave(float y = 0) { P.m_isDart = true; P.pos = {0, y}; P.held = false; }
};

int main() {
    // =============== v0.3 behaviour (cube landing / orbs / per-mode) must be unchanged ===============
    { World w(8); w.tick(3); w.press(); w.tick(1); w.release(); w.tick(2); w.land(); w.tick(1);
      CHECK(g_calls.size() == 3 && g_calls[2] == true, "cube: landing within window => replayed press");
      w.tick(3); CHECK(g_calls.size() == 4 && g_calls[3] == false, "cube: replayed release comes later");
      CHECK(g_totalReplays == 1, "cube: counter 1"); }
    { World w(8); w.tick(3); w.press(); w.release(); w.tick(20); w.land(); w.tick(1);
      CHECK(g_calls.size() == 2, "cube: outside window => no replay"); }
    { World w(0); w.tick(3); w.press(); w.release(); w.tick(2); w.land(); w.tick(2);
      CHECK(g_calls.size() == 2, "cube: 0 ticks = off"); }
    { World w(8); w.tick(3); w.press(); w.tick(1); w.release(); w.tick(2); w.ring(); w.tick(1);
      CHECK(g_calls.size() == 3 && g_calls[2] == true, "cube: orb touched within window => replayed"); }
    { World w(8); w.P.m_isBird = true; w.tick(3); w.press(); w.release(); w.tick(2); w.ring(); w.tick(1);
      CHECK(g_calls.size() == 3, "UFO: orb tap replayed"); }
    { World w(2); w.P.m_isBall = true; g_ints["ball-ticks"] = 12; w.tick(3); w.press(); w.release(); w.tick(9); w.land(); w.tick(1);
      CHECK(g_calls.size() == 3, "ball override wider than default => replayed"); }
    { World w(8); w.P.m_isSwing = true; w.tick(3); w.press(); w.release(); w.tick(2); w.ring(); w.land(); w.tick(3);
      CHECK(g_calls.size() == 2, "swing: untouched"); }
    { World w(8); w.tick(3); w.press(); w.release(); w.tick(1); w.PL.resetLevel(); w.land(); w.tick(2);
      CHECK(g_calls.size() == 2 && g_attemptReplays == 0, "resetLevel clears pending tap and attempt counter"); }

    // =============== ship / wave assist ===============
    { World w; w.wave(); w.spike(); w.press();
      CHECK(g_calls.empty(), "wave: crashing press is held back (not delivered yet)");
      CHECK(g_delayed.size() == 1, "wave: one input queued");
      CHECK(std::fabs(g_delayed[0].left - 2 * kTickSeconds) < 1e-9, "wave: smallest surviving delay found = 2 ticks");
      CHECK(w.P.pos.x == 0 && w.P.pos.y == 0 && !w.P.held, "real player untouched by the look-ahead");
      CHECK(g_dummies[0] != nullptr && g_dummies[0] != &w.P, "look-ahead ran on a separate hidden player");
      CHECK(g_realDeaths == 0, "hidden player's crashes never reach the real death handler");
      w.tick(1); CHECK(g_calls.empty(), "wave: still held back after 1 tick");
      w.tick(1); CHECK(g_calls.size() == 1 && g_calls[0] == true, "wave: delivered after exactly 2 ticks");
      CHECK(g_delayed.empty(), "wave: queue empty after delivery"); }

    { World w; w.wave(-20); w.spike(); w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "wave: safe raw input passes straight through"); }

    { World w; g_ints["wave-ticks"] = 1; w.wave(); w.spike(); w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "wave: needed delay (2) above limit (1) => input passes through"); }
    { World w; g_ints["wave-ticks"] = 0; w.wave(); w.spike(); w.press();
      CHECK(g_calls.size() == 1, "wave limit 0 => off"); }

    { World w; w.P.m_isShip = true; g_ints["wave-ticks"] = 0; g_ints["ship-ticks"] = 4; w.P.pos = {0,0}; w.spike(); w.press();
      CHECK(g_calls.empty() && g_delayed.size() == 1, "ship: uses the ship limit and is delayed");
      World v; v.P.m_isShip = true; g_ints["ship-ticks"] = 0; v.P.pos = {0,0}; v.spike(); v.press();
      CHECK(g_calls.size() == 1, "ship limit 0 => off"); }

    { World w; w.wave(); w.spike(); w.release();   // release while held, same logic
      w.P.held = true; g_calls.clear(); g_delayed.clear();
      w.release();
      CHECK(true, "release path runs without error"); }

    // ---- safety fallbacks: every one must leave the player's input untouched ----
    { World w; w.wave(); w.spike();
      auto old = g_world; g_world = [&, old](PlayerObject* p){ if (p->pos.x == 3) w.R.playerTouchedRing(p, nullptr); old(p); };
      w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "orb touched inside look-ahead => abandoned, input untouched"); }
    { World w; w.wave(); w.spike();
      auto old = g_world; g_world = [&, old](PlayerObject* p){ if (p->pos.x == 3) w.G.bumpPlayer(p, nullptr); old(p); };
      w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "pad touched inside look-ahead => abandoned"); }
    { World w; w.wave(); w.spike();
      auto old = g_world; g_world = [&, old](PlayerObject* p){ if (p->pos.x == 3) w.G.playerTouchedTrigger(p, nullptr); old(p); };
      w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "trigger touched inside look-ahead => abandoned"); }
    { World w; w.wave(); w.spike();
      auto old = g_world; g_world = [&, old](PlayerObject* p){ if (p->pos.x == 3) w.G.toggleDualMode(nullptr, true, p, false); old(p); };
      w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "dual portal inside look-ahead => abandoned"); }
    { World w; w.wave(); w.spike();
      auto old = g_world; g_world = [&, old](PlayerObject* p){ if (p->pos.x == 3) w.G.playerWillSwitchMode(p, nullptr); old(p); };
      w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "mode portal inside look-ahead => abandoned"); }
    { World w; g_ints["assist-budget-ms"] = 0; w.wave(); w.spike();
      auto old = g_world; g_world = [old](PlayerObject* p){ auto t = Clock::now(); while (Clock::now() - t < std::chrono::microseconds(200)) {} old(p); };
      w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "out of time budget => abandoned, input untouched"); }
    { World w; w.wave(); w.spike(); w.L.m_gameState.m_isDualMode = true; w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "dual mode => assist skipped"); }
    { World w; w.wave(); w.spike(); w.P.m_isPlatformer = true; w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "platformer => assist skipped"); }
    { World w; w.wave(); w.spike(); g_bools["assist-enabled"] = false; w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "setting off => assist skipped"); }
    { World w; w.wave(); w.spike(); w.L.handleButton(true, 1, false);
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "player 2 input is never delayed"); }
    { World w; w.wave(); w.spike(); w.L.handleButton(true, 2, true);
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "left/right buttons are never delayed"); }

    // ---- ordering, reset, reuse ----
    { World w; w.wave(); w.spike(); w.press(); w.release();
      CHECK(g_calls.empty() && g_delayed.size() == 2, "input arriving behind a held-back one waits behind it");
      w.tick(2); CHECK(g_calls.size() == 1 && g_calls[0] == true, "order: press delivered first");
      w.tick(1); CHECK(g_calls.size() == 2 && g_calls[1] == false, "order: release delivered after it"); }
    { World w; w.wave(); w.spike(); w.press(); w.PL.resetLevel();
      CHECK(g_delayed.empty(), "resetLevel drops held-back inputs"); }
    { World w; w.wave(); w.spike(); w.press(); w.tick(2); w.P.pos = {0,0}; w.P.held = false; w.release(); (void)0;
      w.wave(); w.spike(); w.P.held = false; g_calls.clear(); g_delayed.clear(); w.press();
      CHECK(g_createCount == 1, "hidden player is created once and reused"); }
    { World w; w.wave(); w.spike(); w.press(); w.PL.setupHasCompleted();
      CHECK(g_dummies[0] == nullptr && g_delayed.empty(), "new level => fresh hidden player, queue cleared"); }


    // =============== v0.4.2: stricter verification ===============
    // diagonal wall: raw input crashes on tick 37 (window is 38). Delaying by k only moves the crash to tick 37+k.
    // The old rule (cap at the window) wrongly called that a save; the new rule must not.
    { World w; w.wave(); g_world = [&w](PlayerObject* p){ if (p->pos.y >= 75 - p->pos.x) w.PL.destroyPlayer(p, nullptr); };
      w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "crash near the window edge that a delay only postpones => NOT a save (input untouched)");
      CHECK(g_lastPrediction.valid && g_lastPrediction.ticks == 37, "that unfixable crash was remembered as a prediction (tick 37)");
      bool saved = false; for (auto& l : g_logs) if (l.find("SAVED") != std::string::npos) saved = true;
      CHECK(!saved, "no SAVED line is logged for it"); }

    // spike at x 5..6 is fixable by a delay of 2; a second wall at x==30 kills everything afterwards
    { World w; w.wave(); w.spike();
      auto old = g_world; g_world = [old, &w](PlayerObject* p){ old(p); if (p->pos.x >= 30) w.PL.destroyPlayer(p, nullptr); };
      g_ints["assist-margin"] = 12; w.press();
      CHECK(g_delayed.size() == 1, "margin 12: delayed input survives 12 ticks past the crash point => accepted");
      World v; v.wave(); v.spike();
      auto old2 = g_world; g_world = [old2, &v](PlayerObject* p){ old2(p); if (p->pos.x >= 30) v.PL.destroyPlayer(p, nullptr); };
      g_ints["assist-margin"] = 40; v.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "margin 40: same situation is no longer enough => input untouched"); }

    // a delay larger than the margin must still be checked with the input actually applied
    { World w; g_ints["wave-ticks"] = 16; g_ints["assist-margin"] = 4; w.wave(); w.spike(); w.press();
      CHECK(g_delayed.size() == 1 && std::fabs(g_delayed[0].left - 2 * kTickSeconds) < 1e-9, "small margin still finds the smallest working delay (2), never an unapplied large one"); }

    // =============== v0.4.2: timeouts pause the assist ===============
    { World w; g_ints["assist-budget-ms"] = 0; w.wave(); w.spike();
      int worldCalls = 0;
      auto old = g_world; g_world = [&, old](PlayerObject* p){ ++worldCalls; auto t = Clock::now(); while (Clock::now() - t < std::chrono::microseconds(200)) {} old(p); };
      for (int i = 0; i < 5; ++i) w.press();
      CHECK(g_assistSuspended, "5 time-outs in a row pause the assist for the level");
      int before = worldCalls; size_t callsBefore = g_calls.size();
      w.press();
      CHECK(worldCalls == before && g_calls.size() == callsBefore + 1, "while paused: no look-ahead runs and the input passes straight through");
      bool warned = false; (void)warned;
      w.PL.setupHasCompleted();
      CHECK(!g_assistSuspended && g_timeoutStreak == 0, "a new level lifts the pause"); }
    { World w; g_ints["assist-budget-ms"] = 0; w.wave(); w.spike();
      auto old = g_world; g_world = [old](PlayerObject* p){ auto t = Clock::now(); while (Clock::now() - t < std::chrono::microseconds(200)) {} old(p); };
      for (int i = 0; i < 3; ++i) w.press();
      g_ints["assist-budget-ms"] = 50; g_world = nullptr; w.wave(-20); w.press();   // a normal decision resets the streak
      CHECK(g_timeoutStreak == 0 && !g_assistSuspended, "a completed look-ahead resets the time-out streak"); }

    // =============== v0.4.2: crash log ===============
    auto hasLog = [](const char* needle) { for (auto& l : g_logs) if (l.find(needle) != std::string::npos) return true; return false; };
    { World w; w.wave(); g_world = [&w](PlayerObject* p){ if (p->pos.y >= 75 - p->pos.x) w.PL.destroyPlayer(p, nullptr); };
      w.press();                                   // unfixable prediction
      w.PL.destroyPlayer(&w.P, nullptr);           // the real crash
      CHECK(hasLog("CRASH: predicted"), "real crash right after an unfixable prediction says it was predicted");
      size_t n = g_logs.size(); w.PL.destroyPlayer(&w.P, nullptr);
      CHECK(g_logs.size() == n, "only one crash line per attempt");
      w.PL.resetLevel(); w.PL.destroyPlayer(&w.P, nullptr);
      CHECK(hasLog("CRASH: no recent look-ahead activity"), "after a reset the next crash is logged again (now with no recent activity)"); }
    { World w; w.wave(); w.spike(); w.press();     // a save
      w.PL.destroyPlayer(&w.P, nullptr);
      CHECK(hasLog("CRASH:") && hasLog("after a save"), "real crash right after a save says so");
      CHECK(g_assistSaves == 1, "saves are counted per attempt"); }
    { World w; w.wave(); w.spike(); w.press();
      w.PL.destroyPlayer(g_dummies[0], nullptr);        // hidden player's death must not produce a crash line
      CHECK(!hasLog("CRASH"), "the hidden player's death is never logged as a crash"); }


    // =============== v0.5.0: swing ===============
    { World w; w.P.m_isSwing = true; w.P.pos = {0,0}; w.spike(); w.press();
      CHECK(g_calls.empty() && g_delayed.size() == 1, "swing: same look-ahead as ship/wave (input held back)");
      World v; g_ints["swing-ticks"] = 0; v.P.m_isSwing = true; v.P.pos = {0,0}; v.spike(); v.press();
      CHECK(g_calls.size() == 1, "swing limit 0 => off"); }

    // =============== v0.5.0: dual mode ===============
    { World w; g_bools["assist-dual"] = true; w.L.m_gameState.m_isDualMode = true; w.L.m_player2 = &w.P2;
      w.wave(); w.P2.m_isDart = true; w.P2.pos = {0, 3}; w.P2.held = false; w.spike();
      w.press();
      CHECK(g_calls.empty() && g_delayed.size() == 1, "dual: player 1's input is held back");
      CHECK(std::fabs(g_delayed[0].left - 3 * kTickSeconds) < 1e-9, "dual: delay chosen so BOTH players survive (3 ticks; player 1 alone would need only 2)");
      CHECK(g_dummies[1] != nullptr && g_dummies[1] != &w.P2 && g_dummies[0] != g_dummies[1], "dual: a second, separate hidden player is used");
      w.L.handleButton(true, 1, false);
      CHECK(g_calls.empty() && g_delayed2.size() == 1 && std::fabs(g_delayed2[0].left - 3 * kTickSeconds) < 1e-9, "dual: player 2's matching input waits exactly as long");
      w.tick(3); CHECK(g_calls.size() == 1 && g_calls[0] == true, "dual: player 1's input delivered after 3 ticks");
      for (int i = 0; i < 3; ++i) w.P2.update(DT);
      CHECK(g_calls.size() == 2 && g_calls[1] == true, "dual: player 2's input delivered after 3 ticks"); }
    { World w; g_bools["assist-dual"] = true; w.L.m_gameState.m_isDualMode = true; w.L.m_player2 = &w.P2;
      w.wave(); w.P2.m_isDart = true; w.P2.pos = {0, 3}; w.spike();
      w.press(); w.tick(1); w.L.handleButton(true, 1, false);
      CHECK(g_calls.size() == 1 && g_delayed2.empty(), "dual: a player 2 input that is NOT in the same frame is never delayed"); }
    { World w; w.L.m_gameState.m_isDualMode = true; w.L.m_player2 = &w.P2; w.wave(); w.P2.m_isDart = true; w.spike(); w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "dual mode with the setting off => assist skipped"); }
    { World w; g_bools["assist-dual"] = true; w.L.m_gameState.m_isDualMode = true; w.L.m_player2 = &w.P2;
      w.wave(); w.P2.m_isDart = false; w.spike(); w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "dual mode with a non-ship/wave second player => assist skipped"); }

    // =============== v0.5.0: accuracy tracking, watchdog, summary, HUD ===============
    { World w; w.wave(); w.spike(); w.press(); w.tick(2);
      CHECK(g_levelSaves == 1 && g_levelOk == 0 && g_levelFailed == 0, "a fresh save is not judged yet");
      g_lastSave.when -= std::chrono::seconds(2);
      w.wave(); w.press();
      CHECK(g_levelOk == 1 && g_levelSaves == 2, "a save with no crash within 1.5 s counts as worked"); }
    { World w; w.wave(); w.spike(); w.press(); w.PL.destroyPlayer(&w.P, nullptr);
      CHECK(g_levelFailed == 1 && g_levelOk == 0, "a crash right after a save counts it as failed"); }
    { World w; w.wave(); w.spike();
      for (int i = 0; i < 5; ++i) { w.PL.resetLevel(); w.wave(); w.press(); w.PL.destroyPlayer(&w.P, nullptr); }
      CHECK(g_levelFailed == 5 && g_assistSuspended, "5 failed saves in a row pause the assist for the level");
      bool warned = false; for (auto& l : g_logs) if (l.find("unreliable") != std::string::npos) warned = true;
      CHECK(warned, "the watchdog says why it paused");
      size_t calls = g_calls.size(); w.PL.resetLevel(); w.wave(); w.press();
      CHECK(g_calls.size() == calls + 1 && g_delayed.empty(), "while paused the input passes straight through"); }
    { World w; g_bools["assist-autopause"] = false; w.wave(); w.spike();
      for (int i = 0; i < 6; ++i) { w.PL.resetLevel(); w.wave(); w.press(); w.PL.destroyPlayer(&w.P, nullptr); }
      CHECK(g_levelFailed >= 5 && !g_assistSuspended, "watchdog switched off => assist keeps running"); }
    { World w; w.wave(); g_world = [&w](PlayerObject* p){ if (p->pos.y >= 75 - p->pos.x) w.PL.destroyPlayer(p, nullptr); };
      w.press(); w.PL.destroyPlayer(&w.P, nullptr);
      CHECK(g_levelCrashes == 1 && g_levelPredicted == 1, "crash statistics: one crash, predicted"); }
    { World w; w.wave(); w.spike(); w.press(); w.PL.onQuit();
      bool s = false; for (auto& l : g_logs) if (l.find("level summary") != std::string::npos) s = true;
      CHECK(s, "leaving the level logs a summary"); }
    { World w; w.PL.setupHasCompleted(); g_levelOk = 2; g_levelFailed = 1; refreshCounter(&w.PL);
      auto* label = typeinfo_cast<CCLabelBMFont*>(w.PL.getChildByID("replay-counter"_spr));
      CHECK(label && label->text.find("Assist: 2 ok, 1 failed") != std::string::npos, "HUD label shows the assist results"); }

    // =============== v0.5.0: other mods, practice-only ===============
    { World w; g_strings["assist-pause-with"] = "foo.bar, baz.qux"; g_loadedMods["baz.qux"] = true; w.PL.setupHasCompleted();
      CHECK(g_conflictPause, "a listed mod that is enabled pauses the assist");
      w.wave(); w.spike(); w.press(); CHECK(g_calls.size() == 1 && g_delayed.empty(), "...and the input passes straight through");
      g_loadedMods["baz.qux"] = false; w.PL.setupHasCompleted();
      CHECK(!g_conflictPause, "a listed mod that is not enabled does not"); }
    { World w; g_strings["assist-pause-with"] = ""; w.PL.setupHasCompleted();
      CHECK(!g_conflictPause, "empty list => never paused"); }
    { World w; g_bools["practice-only"] = true; w.wave(); w.spike(); w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "practice-only: assist is off outside practice mode");
      World v; g_bools["practice-only"] = true; v.L.m_isPracticeMode = true; v.wave(); v.spike(); v.press();
      CHECK(g_delayed.size() == 1, "practice-only: assist works in practice mode"); }
    { World w(8); g_bools["practice-only"] = true; w.tick(3); w.press(); w.release(); w.tick(2); w.land(); w.tick(2);
      CHECK(g_calls.size() == 2, "practice-only: the cube/orb buffer is off outside practice mode too");
      World v(8); g_bools["practice-only"] = true; v.L.m_isPracticeMode = true; v.tick(3); v.press(); v.release(); v.tick(2); v.land(); v.tick(1);
      CHECK(g_calls.size() == 3, "practice-only: the cube/orb buffer works in practice mode"); }

    // =============== v0.5.0: profiles and the pause-menu button ===============
    { World w(8); g_profile = 0; w.tick(3); w.press(); w.release(); w.tick(2); w.land(); w.tick(2);
      CHECK(g_calls.size() == 2, "profile off: cube buffer off");
      World v; g_profile = 0; v.wave(); v.spike(); v.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty(), "profile off: assist off"); }
    { World w(2); g_profile = 4; w.tick(3); w.press(); w.release(); w.tick(9); w.land(); w.tick(1);
      CHECK(g_calls.size() == 3, "profile 16 ticks overrides the default of 2 for the cube");
      World v; g_profile = 2; g_ints["wave-ticks"] = 0; v.wave(); v.spike(); v.press();
      CHECK(g_delayed.size() == 1, "profile 4 ticks overrides a wave limit of 0"); }
    { World w; w.PZ.customSetup();
      CHECK(g_profile == -1, "pause button starts on the default settings");
      w.PZ.onBufferProfile(nullptr); CHECK(g_profile == 0 && g_saved["profile-0"] == 0, "pause button: default -> off, saved for the level");
      for (int i = 0; i < 4; ++i) w.PZ.onBufferProfile(nullptr);
      CHECK(g_profile == 4 && g_saved["profile-0"] == 4, "pause button: ... -> 16 ticks");
      w.PZ.onBufferProfile(nullptr); CHECK(g_profile == -1, "pause button wraps back to default");
      g_saved["profile-0"] = 3; w.PL.setupHasCompleted();
      CHECK(g_profile == 3, "opening the level again restores its saved profile"); }

    // =============== v0.5.0: adaptive time budget ===============
    { World w; w.wave(); w.spike(); g_tickCostNs = 1e12; w.press();
      CHECK(g_calls.size() == 1 && g_delayed.empty() && g_timeoutStreak == 0, "no time to search => skipped up front, not counted as a time-out");
      CHECK(g_lastPrediction.valid, "...and the predicted crash is still remembered"); }
    { World w; w.wave(); w.spike(); CHECK(g_tickCostNs == 0.0, "no cost measured yet"); w.press();
      CHECK(g_tickCostNs > 0.0 && g_delayed.size() == 1, "the cost per simulated tick is learned, and a normal search still succeeds"); }

    std::printf("\n%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fails ? 1 : 0;
}
