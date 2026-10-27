// Predictive Input Buffer v0.5.0
//
// What it does
//   If you tap jump while airborne and let go again before the tap can do
//   anything, the game drops it. This mod remembers the tap and replays it
//   when the player lands (cube, ball, robot, spider) or touches an orb/ring
//   (everything except ship, wave and swing), as long as that happens within
//   a short window after the original press. The replayed press is held for as
//   long as you originally held it.
//
//   A "tick" is one 1/240 s physics step, so 4 ticks is about 1 frame at 60 FPS.
//   Strength comes from the mod settings: a default tick value plus optional
//   per-mode overrides. F6 cycles presets on desktop.
//
// Ship and wave assist (new in 0.4.0)
//   For ship and wave there is no "lost tap" to replay, so this works the way the
//   original v4.7 did: when you press or release, a hidden copy of your player
//   (never your real one) is run a few dozen ticks ahead with that input. If the
//   copy would crash, the same input is tried 1, 2, 3 ... ticks later (up to the
//   limit in the settings), and the first delay that survives is used. The real
//   player is never rewound or modified; the input is simply delivered late.
//   The look-ahead is abandoned, and your input passes through untouched, if it
//   runs out of time, or if the copy touches anything the simulation cannot model
//   safely (orbs, pads, triggers, portals). Platformer mode is skipped. Swing copter
//   uses the same look-ahead. Dual mode is supported too (off by default): both
//   players are simulated together and their inputs are held back together.
//
//   Extras: a pause-menu button with a per-level strength profile, a "practice mode
//   only" switch, pausing the assist while chosen mods are enabled, a watchdog that
//   pauses the assist on a level where its saves keep being followed by crashes, a
//   time budget that adapts to how fast the device is, and a level summary in the log.
//
// Not covered
//   - Orbs/pads/portals/triggers inside the look-ahead are not simulated (the look-ahead
//     is abandoned there), and moving objects are not advanced.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace geode::prelude;

// Orb/ring buffering needs a hook on GJBaseGameLayer::playerTouchedRing whose
// exact signature could not be checked. If the build fails on that hook, change
// this 1 to a 0: everything else keeps working, only orb buffering is lost.
#define PIB_ENABLE_RINGS 1

// The ship/wave assist adds several hooks (death, pads, triggers, portals, ...).
// If the build fails on one of them, change this 1 to a 0: the landing/orb buffer
// keeps working and only the ship/wave assist is lost.
#define PIB_ENABLE_ASSIST 1

// Each of these can be switched off on its own (set to 0) if the build fails on it.
#define PIB_ENABLE_DUAL 1      // dual-mode support for the ship/wave assist
#define PIB_ENABLE_PAUSEUI 1   // pause-menu button and per-level profile (uses the level id)
#define PIB_ENABLE_PRACTICE 1  // "practice mode only" switch (uses the practice-mode flag)

namespace {
    // ---------------------------------------------------------------- logic
    // Pure state machine, no game types in here.

    constexpr double kTickSeconds = 1.0 / 240.0;
    constexpr double kEpsilon = 1e-6;              // absorbs float rounding in dt
    constexpr double kMinAirSeconds = 2.0 * kTickSeconds; // ignores 1-tick "air" flicker on slopes

    struct Config {
        double window = 0.0; // seconds; 0 = off
        bool landing = false; // may replay on landing
        bool ring = false;    // may replay on touching an orb
    };

    struct Step {
        bool press = false;
        bool release = false;
    };

    class PlayerBuffer {
    public:
        void reset() { *this = PlayerBuffer{}; }

        // A real jump press.
        void onPress(Config const& cfg, bool grounded) {
            injected_ = false;   // a real press takes over from a replayed one
            ringQueued_ = false;

            bool usable = cfg.window > 0.0 && (cfg.landing || cfg.ring);
            bool airborne = !grounded && airTime_ >= kMinAirSeconds - kEpsilon;
            if (usable && airborne) {
                pending_ = true;
                released_ = false;
                age_ = 0.0;
                heldFor_ = 0.0;
            } else {
                pending_ = false;
            }
        }

        // A real jump release.
        void onRelease() {
            if (pending_) released_ = true;
        }

        // The player touched an orb/ring.
        void onRing(Config const& cfg) {
            if (pending_ && released_ && cfg.ring && cfg.window > 0.0
                && age_ <= cfg.window + kEpsilon) {
                pending_ = false;
                ringQueued_ = true; // replayed on the next physics step
            }
        }

        // Once per physics step, after the game has updated the player.
        Step tick(double dt, bool grounded, Config const& cfg) {
            Step out;

            // Let go of a replayed press once the original hold time has elapsed.
            if (injected_) {
                injectedLeft_ -= dt;
                if (injectedLeft_ <= 0.0) {
                    injected_ = false;
                    out.release = true;
                }
            }

            if (ringQueued_) {
                ringQueued_ = false;
                startReplay();
                out.press = true;
            }

            if (!grounded) airTime_ += dt;
            bool landed = grounded && !wasGrounded_ && airTime_ >= kMinAirSeconds - kEpsilon;

            if (pending_) {
                age_ += dt;
                if (!released_) heldFor_ += dt;

                if (landed) {
                    // If the button is still down the game jumps on landing by itself.
                    if (released_ && cfg.landing && cfg.window > 0.0
                        && age_ <= cfg.window + kEpsilon) {
                        startReplay();
                        out.press = true;
                    }
                    pending_ = false;
                } else if (cfg.window <= 0.0 || age_ > cfg.window + kEpsilon) {
                    pending_ = false; // too early, forget it
                }
            }

            if (grounded) airTime_ = 0.0;
            wasGrounded_ = grounded;
            return out;
        }

    private:
        void startReplay() {
            injected_ = true;
            injectedLeft_ = std::max(heldFor_, kTickSeconds);
        }

        bool wasGrounded_ = true;
        double airTime_ = 0.0;
        bool pending_ = false;
        bool released_ = false;
        double age_ = 0.0;
        double heldFor_ = 0.0;
        bool injected_ = false;
        double injectedLeft_ = 0.0;
        bool ringQueued_ = false;
    };

    // ----------------------------------------------------------------- glue

    PlayerBuffer g_buffers[2]; // 0 = player 1, 1 = player 2
    bool g_injecting = false;
    int g_attemptReplays = 0;
    int g_totalReplays = 0;

    int64_t intSetting(char const* key) {
        return Mod::get()->getSettingValue<int64_t>(key);
    }

    bool boolSetting(char const* key) {
        return Mod::get()->getSettingValue<bool>(key);
    }

    // Per-level profile chosen from the pause menu: -1 = use the settings, else an index into this table.
    constexpr int kProfileTicks[] = {0, 2, 4, 8, 16};
    int g_profile = -1;

    // Assist results for the level that is running (shown in the HUD label).
    int g_levelOk = 0;
    int g_levelFailed = 0;

    // "Practice mode only" switch: everything is off outside practice mode when it is on.
    bool practiceBlocked(PlayLayer* layer) {
#if PIB_ENABLE_PRACTICE
        return layer && boolSetting("practice-only") && !layer->m_isPracticeMode;
#else
        (void)layer;
        return false;
#endif
    }

#if PIB_ENABLE_PAUSEUI
    std::string levelProfileKey(PlayLayer* layer) {
        return fmt::format("profile-{}", layer->m_level->m_levelID.value());
    }
#endif

    // Ticks for this player's current mode: default value, or the per-mode override.
    int ticksFor(PlayerObject* p) {
        if (g_profile >= 0) return kProfileTicks[g_profile];
        int64_t ticks = intSetting("ticks");
        char const* key = nullptr;
        if (p->m_isBall) key = "ball-ticks";
        else if (p->m_isRobot) key = "robot-ticks";
        else if (p->m_isSpider) key = "spider-ticks";
        else if (p->m_isBird) key = "ufo-ticks";
        if (key) {
            int64_t override = intSetting(key);
            if (override >= 0) ticks = override;
        }
        return static_cast<int>(std::clamp<int64_t>(ticks, 0, 16));
    }

    Config configFor(PlayerObject* p) {
        Config c;
        if (!p || p->m_isShip || p->m_isDart || p->m_isSwing) return c;
        if (practiceBlocked(PlayLayer::get())) return c;
        int ticks = ticksFor(p);
        if (ticks <= 0) return c;
        c.window = ticks * kTickSeconds;
        c.landing = !p->m_isBird; // UFO has no ground-jump to save
        c.ring = Mod::get()->getSettingValue<bool>("buffer-rings");
        return c;
    }

    // 0 or 1 for the two players of the running level, -1 for anything else.
    int playerIndex(PlayerObject* p) {
        auto* layer = PlayLayer::get();
        if (!layer || !p) return -1;
        if (p == layer->m_player1) return 0;
        if (p == layer->m_player2) return 1;
        return -1;
    }

    // Sends a jump press/release through the normal input path.
    void inject(bool down, bool isPlayer1) {
        auto* layer = PlayLayer::get();
        if (!layer) return;
        g_injecting = true;
        layer->handleButton(down, 1, isPlayer1);
        g_injecting = false;
    }

    void refreshCounter(PlayLayer* layer) {
        if (!layer) return;
        auto* label = typeinfo_cast<CCLabelBMFont*>(layer->getChildByID("replay-counter"_spr));
        if (!label) return;
        bool show = Mod::get()->getSettingValue<bool>("show-counter");
        label->setVisible(show);
        if (show) {
            std::string text = fmt::format("Buffered: {} (total {})", g_attemptReplays, g_totalReplays);
            if (g_levelOk + g_levelFailed > 0) {
                text += fmt::format(" | Assist: {} ok, {} failed", g_levelOk, g_levelFailed);
            }
            label->setString(text.c_str());
        }
    }

    void onReplay() {
        ++g_attemptReplays;
        ++g_totalReplays;
        log::debug("[InputBuffer] replayed tap (attempt {}, total {})", g_attemptReplays, g_totalReplays);
        refreshCounter(PlayLayer::get());
    }


#if PIB_ENABLE_ASSIST
    // --------------------------------------------------- ship/wave/swing assist
    using Clock = std::chrono::steady_clock;

    struct Delayed {
        bool down;
        double left; // seconds until it is delivered
    };

    // Dual mode: player 2's matching input arrives right after player 1's, in the same frame.
    struct Pair {
        bool valid = false;
        bool down = false;
        int ticks = 0;
    };

    std::vector<Delayed> g_delayed;          // inputs held back for player 1
    std::vector<Delayed> g_delayed2;         // ... and for player 2 (dual mode only)
    Pair g_pair;
    PlayerObject* g_dummies[2] = {nullptr, nullptr}; // hidden copies used for the look-ahead
    PlayLayer* g_dummyLayer = nullptr;
    bool g_simulating = false;
    bool g_simDied = false;
    bool g_simTainted = false;               // the copy touched something we can't simulate
    double g_lastDt = kTickSeconds;          // dt of the last real physics step
    double g_tickCostNs = 0.0;               // measured cost of one simulated tick (all players)

    constexpr int kMaxLookAhead = 96;        // hard cap on any single simulation, in ticks
    int g_timeoutStreak = 0;                 // look-aheads in a row that ran out of time
    bool g_assistSuspended = false;          // paused for this level
    bool g_conflictPause = false;            // a mod from the "pause with" list is enabled
    int g_assistSaves = 0;                   // inputs delayed this attempt
    bool g_deathLogged = false;              // one crash line per attempt

    // Per-level outcome tracking. A save "worked" if no crash follows within 1.5 s.
    int g_levelSaves = 0;
    int g_levelCrashes = 0;
    int g_levelPredicted = 0;
    bool g_lastSaveSettled = true;

    struct Note {
        bool valid = false;
        Clock::time_point when;
        int ticks = 0;     // saves: ticks delayed. predictions: tick the raw input would crash on
        bool down = false;
    };
    Note g_lastSave;
    Note g_lastPrediction;

    bool isDummy(PlayerObject* p) {
        return p && (p == g_dummies[0] || p == g_dummies[1]);
    }

    long long msSince(Note const& n) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - n.when).count();
    }

    void noteTimeout() {
        if (++g_timeoutStreak >= 5 && !g_assistSuspended) {
            g_assistSuspended = true;
            log::warn("[InputBuffer] the ship/wave look-ahead keeps running out of time and is paused for this level. Raise the time budget or lower the delay limits.");
        }
    }

    void resetAttemptNotes() {
        g_assistSaves = 0;
        g_deathLogged = false;
        g_lastSave = Note{};
        g_lastPrediction = Note{};
        g_lastSaveSettled = true;
    }

    void resetLevelStats() {
        g_levelSaves = 0;
        g_levelOk = 0;
        g_levelFailed = 0;
        g_levelCrashes = 0;
        g_levelPredicted = 0;
        resetAttemptNotes();
    }

    // Marks the last save as worked (no crash followed) or failed (a crash followed).
    void settleLastSave(bool crashed) {
        if (!g_lastSave.valid || g_lastSaveSettled) return;
        if (crashed) {
            ++g_levelFailed;
            g_lastSaveSettled = true;
        } else if (msSince(g_lastSave) >= 1500) {
            ++g_levelOk;
            g_lastSaveSettled = true;
        }
    }

    // One line per real crash: was it predicted, did a save just happen, or was the assist idle?
    // Also feeds the watchdog that pauses the assist on a level where it keeps failing.
    void logCrash() {
        ++g_levelCrashes;
        bool predicted = g_lastPrediction.valid && msSince(g_lastPrediction) < 1500;
        bool afterSave = !predicted && g_lastSave.valid && msSince(g_lastSave) < 1500;
        if (predicted) {
            ++g_levelPredicted;
            log::info(
                "[InputBuffer] CRASH: predicted {} ms earlier (raw {} would crash in {} ticks, no safe delay found). Saves this attempt: {}",
                msSince(g_lastPrediction), g_lastPrediction.down ? "PRESS" : "RELEASE",
                g_lastPrediction.ticks, g_assistSaves
            );
            settleLastSave(false);
        } else if (afterSave) {
            log::info(
                "[InputBuffer] CRASH: {} ms after a save (+{} ticks on a {}). Saves this attempt: {}",
                msSince(g_lastSave), g_lastSave.ticks, g_lastSave.down ? "PRESS" : "RELEASE", g_assistSaves
            );
            settleLastSave(true);
        } else {
            log::info("[InputBuffer] CRASH: no recent look-ahead activity. Saves this attempt: {}", g_assistSaves);
            settleLastSave(false);
        }

        if (boolSetting("assist-autopause") && !g_assistSuspended
            && g_levelFailed >= 5 && g_levelFailed * 10 >= (g_levelOk + g_levelFailed) * 7) {
            g_assistSuspended = true;
            log::warn(
                "[InputBuffer] {} of {} saves on this level were followed by a crash: the look-ahead looks unreliable here (moving objects?) and is paused for the rest of the level.",
                g_levelFailed, g_levelOk + g_levelFailed
            );
        }
        refreshCounter(PlayLayer::get());
    }

    void logLevelSummary() {
        settleLastSave(false);
        if (g_levelSaves == 0 && g_levelCrashes == 0) return;
        log::info(
            "[InputBuffer] level summary: {} saves ({} with no crash after, {} followed by a crash); {} crashes, {} of them predicted",
            g_levelSaves, g_levelOk, g_levelFailed, g_levelCrashes, g_levelPredicted
        );
    }

    // IDs in the "pause with" setting, separated by commas, semicolons or spaces.
    bool conflictingModLoaded() {
        std::string list = Mod::get()->getSettingValue<std::string>("assist-pause-with");
        std::string id;
        auto check = [&]() -> bool {
            if (id.empty()) return false;
            // getLoadedMod only returns mods that are loaded, which is what matters here
            bool hit = Loader::get()->getLoadedMod(id) != nullptr;
            if (hit) {
                log::info("[InputBuffer] ship/wave assist is paused because '{}' is enabled (see the 'Pause assist with these mods' setting).", id);
            }
            id.clear();
            return hit;
        };
        for (char c : list) {
            if (c == ',' || c == ';' || c == ' ' || c == '\n') {
                if (check()) return true;
            } else {
                id.push_back(c);
            }
        }
        return check();
    }

    bool assistEnabled() {
        return boolSetting("assist-enabled") && g_profile != 0 && !g_conflictPause;
    }

    int assistLimit(PlayerObject* p) {
        if (g_profile >= 0) return kProfileTicks[g_profile];
        char const* key = p->m_isDart ? "wave-ticks" : (p->m_isSwing ? "swing-ticks" : "ship-ticks");
        return static_cast<int>(std::clamp<int64_t>(intSetting(key), 0, 16));
    }

    bool assistMode(PlayerObject* p) {
        return p && (p->m_isShip || p->m_isDart || p->m_isSwing) && !p->m_isPlatformer;
    }

    // Ship, wave or swing; not platformer; dual mode only if switched on and both players qualify.
    bool assistApplies(PlayLayer* layer, PlayerObject* p) {
        if (!assistEnabled() || practiceBlocked(layer) || !assistMode(p)) return false;
        if (layer->m_gameState.m_isDualMode) {
#if PIB_ENABLE_DUAL
            return boolSetting("assist-dual") && assistMode(layer->m_player2);
#else
            return false;
#endif
        }
        return true;
    }

    // The hidden players. Their trails and particles go into a layer that is never
    // shown, so nothing they do is visible.
    bool ensureDummies(PlayLayer* layer, int count) {
        if (g_dummyLayer != layer) {
            g_dummies[0] = g_dummies[1] = nullptr;
            g_dummyLayer = layer;
        }
        for (int i = 0; i < count; ++i) {
            if (g_dummies[i]) continue;
            auto* sandbox = CCLayer::create();
            sandbox->retain();
            auto* dummy = PlayerObject::create(1, 1, layer, sandbox, true);
            if (!dummy) return false;
            dummy->retain();
            dummy->setVisible(false);
            g_dummies[i] = dummy;
        }
        return true;
    }

    struct SimResult {
        int deathTick;   // tick a hidden player died on, or the horizon if both survived
        bool tainted;
        bool timedOut;
    };

    // Runs the hidden players `horizon` ticks ahead. The input (`down`) is applied to
    // all of them on tick `delay`. Order per tick follows the original: update,
    // rotation, collisions, then check whether anyone died.
    SimResult simulate(
        PlayLayer* layer, int count, PlayerObject* const* reals, bool const* held,
        bool down, int delay, int horizon, double dt, Clock::time_point deadline
    ) {
        SimResult out{horizon, false, false};
        auto started = Clock::now();
        int steps = 0;

        g_simulating = true;
        g_simDied = false;
        g_simTainted = false;

        for (int i = 0; i < count; ++i) {
            auto* dummy = g_dummies[i];
            dummy->copyAttributes(reals[i]);
            dummy->setPosition(reals[i]->getPosition());
            dummy->m_yVelocity = reals[i]->m_yVelocity;
            dummy->m_isDead = false;
            dummy->releaseAllButtons();
            if (held[i]) dummy->pushButton(PlayerButton::Jump);
        }

        float step = static_cast<float>(dt);
        for (int tick = 0; tick < horizon; ++tick) {
            if (tick == delay) {
                for (int i = 0; i < count; ++i) {
                    if (down) g_dummies[i]->pushButton(PlayerButton::Jump);
                    else g_dummies[i]->releaseButton(PlayerButton::Jump);
                }
            }

            for (int i = 0; i < count; ++i) {
                auto* dummy = g_dummies[i];
                dummy->update(step);
                dummy->updateRotation(step);
                layer->checkCollisions(dummy, step, false);
                ++steps;
            }

            bool dead = g_simDied;
            for (int i = 0; i < count; ++i) dead = dead || g_dummies[i]->m_isDead;
            if (dead) {
                out.deathTick = tick;
                break;
            }
            if (g_simTainted) {
                out.tainted = true;
                break;
            }
            if (Clock::now() > deadline) {
                out.timedOut = true;
                break;
            }
        }

        g_simulating = false;

        // Learn how fast this device runs a simulated tick (moving average).
        if (steps > 0) {
            double ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count()
            );
            double perStep = ns / steps;
            g_tickCostNs = g_tickCostNs <= 0.0 ? perStep : 0.8 * g_tickCostNs + 0.2 * perStep;
        }
        return out;
    }

    // How many ticks to hold this input back (0 = deliver it now). `reals` holds the
    // players to protect: player 1, and player 2 as well in dual mode.
    int decideDelay(PlayLayer* layer, int count, PlayerObject* const* reals, bool down) {
        if (g_assistSuspended) return 0;
        int limit = assistLimit(reals[0]);
        if (limit <= 0) return 0;
        if (!ensureDummies(layer, count)) return 0;

        bool wave = reals[0]->m_isDart;
        int horizon = wave ? 38 : 48;
        int margin = static_cast<int>(std::clamp<int64_t>(intSetting("assist-margin"), 4, 40));
        bool held[2] = {false, false};
        for (int i = 0; i < count; ++i) held[i] = reals[i]->buttonDown(PlayerButton::Jump);
        int64_t budgetMs = std::clamp<int64_t>(intSetting("assist-budget-ms"), 0, 30);
        auto deadline = Clock::now() + std::chrono::milliseconds(budgetMs);

        // 1. What happens with the raw input?
        SimResult raw = simulate(layer, count, reals, held, down, 0, horizon, g_lastDt, deadline);
        if (raw.timedOut) {
            noteTimeout();
            log::debug("[InputBuffer] look-ahead skipped: out of time");
            return 0;
        }
        if (raw.tainted || raw.deathTick >= horizon) {
            g_timeoutStreak = 0;
            return 0;
        }

        // 2. How many candidate delays can this device afford in the time that is left?
        //    Skipping up front is better than starting a search and running out of time.
        int need = std::min(raw.deathTick + margin, kMaxLookAhead);
        int maxDelay = limit;
        if (g_tickCostNs > 0.0) {
            double left = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count()
            );
            double perCandidate = g_tickCostNs * need * count;
            int affordable = perCandidate > 0.0 ? static_cast<int>(left / perCandidate) : limit;
            maxDelay = std::min(limit, std::max(affordable, 0));
        }
        g_timeoutStreak = 0;
        if (maxDelay < 1) {
            log::debug("[InputBuffer] look-ahead skipped: no time left to search for a delay");
            g_lastPrediction = Note{true, Clock::now(), raw.deathTick, down};
            return 0;
        }

        // 3. Try later delivery, smallest delay first. A delay only counts if the
        //    hidden player(s) then survive `margin` ticks PAST the tick where the raw
        //    input would have crashed. The check is not capped at the look-ahead
        //    window, so a crash near the end of the window is verified properly
        //    instead of just being pushed out of view.
        for (int delay = 1; delay <= maxDelay; ++delay) {
            int span = std::min(std::max(need, delay + margin), kMaxLookAhead);
            SimResult r = simulate(layer, count, reals, held, down, delay, span, g_lastDt, deadline);
            if (r.timedOut) {
                noteTimeout();
                return 0;
            }
            if (r.tainted) return 0;
            if (r.deathTick >= span) {
                settleLastSave(false);
                ++g_assistSaves;
                ++g_levelSaves;
                g_lastSave = Note{true, Clock::now(), delay, down};
                g_lastSaveSettled = false;
                log::info(
                    "[InputBuffer] *** SAVED +{} TICKS ({:.2f} frames @ 60fps) {}! (Frame0: {} -> checked safe for {} ticks) [Limit: {}]{} ***",
                    delay, delay / 4.0, down ? "PRESS" : "RELEASE", raw.deathTick, span, limit,
                    count == 2 ? " [dual]" : ""
                );
                return delay;
            }
        }

        // No safe delay exists: the raw input goes through. Remember the prediction
        // so the next crash line can say whether it was foreseen.
        g_lastPrediction = Note{true, Clock::now(), raw.deathTick, down};
        return 0;
    }

    // Entry point from the input hook: decides for player 1, or for both players in dual mode.
    int decideDelayFor(PlayLayer* layer, bool down) {
        bool dual = PIB_ENABLE_DUAL && layer->m_gameState.m_isDualMode;
        PlayerObject* reals[2] = {layer->m_player1, dual ? layer->m_player2 : nullptr};
        return decideDelay(layer, dual ? 2 : 1, reals, down);
    }
#endif

#ifdef GEODE_IS_DESKTOP
    void nextPreset() {
        constexpr int kPresets[] = {2, 4, 8, 16, 0};
        constexpr int kCount = sizeof(kPresets) / sizeof(kPresets[0]);
        int current = static_cast<int>(intSetting("ticks"));
        int index = -1;
        for (int i = 0; i < kCount; ++i) {
            if (kPresets[i] == current) index = i;
        }
        int next = kPresets[(index + 1) % kCount];
        (void)Mod::get()->setSettingValue<int64_t>("ticks", next);

        auto text = next > 0 ? fmt::format("Input buffer: {} ticks", next)
                             : std::string("Input buffer: off");
        log::info("[InputBuffer] {}", text);
        Notification::create(text, NotificationIcon::None, 1.0f)->show();
    }
#endif
}

// Watches jump presses and releases.
class $modify(BufferGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto* playLayer = PlayLayer::get();
        bool relevant = !g_injecting
            && button == 1
            && playLayer
            && static_cast<GJBaseGameLayer*>(playLayer) == this;

        if (relevant) {
            auto* player = isPlayer1 ? m_player1 : m_player2;
#if PIB_ENABLE_ASSIST
            {
                auto& queue = isPlayer1 ? g_delayed : g_delayed2;
                // Anything arriving behind a held-back input waits behind it, to keep the order.
                if (!queue.empty()) {
                    queue.push_back({down, queue.back().left + kTickSeconds});
                    return;
                }
                if (isPlayer1) {
                    if (player && assistApplies(playLayer, player)) {
                        int ticks = decideDelayFor(playLayer, down);
                        if (ticks > 0) {
                            g_delayed.push_back({down, ticks * kTickSeconds});
                            if (playLayer->m_gameState.m_isDualMode) g_pair = Pair{true, down, ticks};
                            onReplay();
                            return;
                        }
                    }
                } else if (g_pair.valid && g_pair.down == down) {
                    // Dual mode: player 2's copy of the input waits exactly as long as player 1's.
                    g_delayed2.push_back({down, g_pair.ticks * kTickSeconds});
                    g_pair.valid = false;
                    return;
                }
            }
#endif
            auto& buffer = g_buffers[isPlayer1 ? 0 : 1];
            if (down) {
                buffer.onPress(configFor(player), player ? player->m_isOnGround : true);
            } else {
                buffer.onRelease();
            }
        }

        // The original input always goes through untouched.
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// Runs once per physics step per player: ages the buffered tap and replays it.
class $modify(BufferPlayer, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);

        int index = playerIndex(this);
        if (index < 0) return;

#if PIB_ENABLE_ASSIST
        {
            auto& queue = index == 0 ? g_delayed : g_delayed2;
            if (index == 0) {
                g_lastDt = dt;
                g_pair.valid = false; // player 2's matching input only ever follows in the same frame
            }
            for (auto& entry : queue) entry.left -= dt;
            while (!queue.empty() && queue.front().left <= kEpsilon) {
                bool down = queue.front().down;
                queue.erase(queue.begin());
                inject(down, index == 0);
            }
        }
#endif

        Step step = g_buffers[index].tick(dt, m_isOnGround, configFor(this));
        if (step.release) inject(false, index == 0);
        if (step.press) {
            inject(true, index == 0);
            onReplay();
        }
    }

#if PIB_ENABLE_ASSIST
    static void onModify(auto& self) {
        if (!self.setHookPriorityPre("PlayerObject::incrementJumps", Priority::First)) {
            log::warn("[InputBuffer] could not set the incrementJumps hook priority");
        }
    }

    // Jumps made by the hidden player must not count towards your stats.
    void incrementJumps() {
        if (g_simulating && isDummy(this)) return;
        PlayerObject::incrementJumps();
    }
#endif
};

#if PIB_ENABLE_RINGS || PIB_ENABLE_ASSIST
// Tells the buffer when the player touches an orb/ring, and stops the hidden
// player from firing real orbs.
class $modify(BufferRingLayer, GJBaseGameLayer) {
#if PIB_ENABLE_ASSIST
    static void onModify(auto& self) {
        if (!self.setHookPriorityPre("GJBaseGameLayer::playerTouchedRing", Priority::First)) {
            log::warn("[InputBuffer] could not set the playerTouchedRing hook priority");
        }
    }
#endif

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
#if PIB_ENABLE_ASSIST
        if (g_simulating) {
            g_simTainted = true;
            return;
        }
#endif
        GJBaseGameLayer::playerTouchedRing(player, ring);

#if PIB_ENABLE_RINGS
        int index = playerIndex(player);
        if (index >= 0) g_buffers[index].onRing(configFor(player));
#endif
    }
};
#endif

#if PIB_ENABLE_ASSIST
// While the hidden player is running, anything that would change the real level
// (pads, triggers, portals, mirror, dual, teleport) is skipped and the look-ahead
// is abandoned, so your input simply passes through.
class $modify(BufferSimGuards, GJBaseGameLayer) {
    // Run before other mods' hooks on the same functions, so they never see the hidden player.
    static void onModify(auto& self) {
        for (char const* name : {
            "GJBaseGameLayer::bumpPlayer",
            "GJBaseGameLayer::playerTouchedTrigger",
            "GJBaseGameLayer::playerWillSwitchMode",
            "GJBaseGameLayer::toggleDualMode",
            "GJBaseGameLayer::toggleFlipped",
            "GJBaseGameLayer::teleportPlayer",
        }) {
            if (!self.setHookPriorityPre(name, Priority::First)) {
                log::warn("[InputBuffer] could not set the hook priority of {}", name);
            }
        }
    }

    void bumpPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_simulating) { g_simTainted = true; return; }
        GJBaseGameLayer::bumpPlayer(player, object);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (g_simulating) { g_simTainted = true; return; }
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (g_simulating) { g_simTainted = true; return; }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }

    void toggleDualMode(GameObject* object, bool dual, PlayerObject* player, bool noEffects) {
        if (g_simulating) { g_simTainted = true; return; }
        GJBaseGameLayer::toggleDualMode(object, dual, player, noEffects);
    }

    void toggleFlipped(bool flip, bool noEffects) {
        if (g_simulating) { g_simTainted = true; return; }
        GJBaseGameLayer::toggleFlipped(flip, noEffects);
    }

    void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
        if (g_simulating) { g_simTainted = true; return; }
        GJBaseGameLayer::teleportPlayer(object, player);
    }
};
#endif

class $modify(BufferPlayLayer, PlayLayer) {
#if PIB_ENABLE_ASSIST
    // Run before other mods' destroyPlayer hooks. Without this, a mod that reacts
    // to deaths (for example one that shows hitboxes on death) would also see the
    // hidden player "die" during every look-ahead.
    static void onModify(auto& self) {
        if (!self.setHookPriorityPre("PlayLayer::destroyPlayer", Priority::First)) {
            log::warn("[InputBuffer] could not set the destroyPlayer hook priority");
        }
    }

    // A death of the hidden player only ends its own look-ahead.
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_simulating && isDummy(player)) {
            g_simDied = true;
            return;
        }
        if (!g_deathLogged && !g_simulating && player
            && (player == m_player1 || player == m_player2)) {
            g_deathLogged = true;
            logCrash();
        }
        PlayLayer::destroyPlayer(player, object);
    }
#endif

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        for (auto& buffer : g_buffers) buffer.reset();
        g_attemptReplays = 0;
#if PIB_ENABLE_ASSIST
        g_delayed.clear();
        g_delayed2.clear();
        g_pair = Pair{};
        g_dummies[0] = g_dummies[1] = nullptr; // fresh hidden players are made for this level
        g_dummyLayer = nullptr;
        g_timeoutStreak = 0;
        g_assistSuspended = false;
        g_tickCostNs = 0.0;
        resetLevelStats();
        g_conflictPause = conflictingModLoaded();
#endif
        g_levelOk = 0;
        g_levelFailed = 0;
#if PIB_ENABLE_PAUSEUI
        g_profile = static_cast<int>(std::clamp<int64_t>(
            Mod::get()->getSavedValue<int64_t>(levelProfileKey(this), -1), -1, 4
        ));
#endif

        auto size = CCDirector::get()->getWinSize();
        auto* label = CCLabelBMFont::create("Buffered: 0", "bigFont.fnt");
        label->setID("replay-counter"_spr);
        label->setAnchorPoint({0.5f, 0.f});
        label->setPosition({size.width / 2.f, 6.f});
        label->setScale(0.35f);
        label->setOpacity(160);
        label->setVisible(false);
        this->addChild(label, 100);
        refreshCounter(this);
    }

#if PIB_ENABLE_ASSIST
    void onQuit() {
        logLevelSummary();
        PlayLayer::onQuit();
    }
#endif

    void resetLevel() {
        PlayLayer::resetLevel();
        for (auto& buffer : g_buffers) buffer.reset();
        g_attemptReplays = 0;
#if PIB_ENABLE_ASSIST
        g_delayed.clear();
        g_delayed2.clear();
        g_pair = Pair{};
        resetAttemptNotes();
#endif
        refreshCounter(this);
    }
};

#if PIB_ENABLE_PAUSEUI
// A small button in the pause menu: cycles this level's strength profile
// (default settings -> off -> 2 -> 4 -> 8 -> 16 ticks) and remembers it for the level.
class $modify(BufferPauseLayer, PauseLayer) {
    static std::string profileText() {
        static char const* names[] = {"off", "2 ticks", "4 ticks", "8 ticks", "16 ticks"};
        return fmt::format("Input buffer: {}", g_profile < 0 ? "default" : names[g_profile]);
    }

    void customSetup() {
        PauseLayer::customSetup();

        auto size = CCDirector::get()->getWinSize();
        auto* label = CCLabelBMFont::create(profileText().c_str(), "bigFont.fnt");
        label->setScale(0.4f);
        auto* item = CCMenuItemSpriteExtra::create(label, this, menu_selector(BufferPauseLayer::onBufferProfile));
        item->setID("buffer-profile"_spr);

        auto* menu = CCMenu::create();
        menu->setPosition({size.width - 110.f, 18.f});
        menu->addChild(item);
        this->addChild(menu, 10);
    }

    void onBufferProfile(CCObject* sender) {
        g_profile = g_profile >= 4 ? -1 : g_profile + 1;
        if (auto* layer = PlayLayer::get()) {
            (void)Mod::get()->setSavedValue<int64_t>(levelProfileKey(layer), g_profile);
        }
        if (auto* item = typeinfo_cast<CCMenuItemSpriteExtra*>(sender)) {
            if (auto* label = typeinfo_cast<CCLabelBMFont*>(item->getNormalImage())) {
                label->setString(profileText().c_str());
            }
        }
        log::info("[InputBuffer] profile for this level: {}", profileText());
    }
};
#endif

#ifdef GEODE_IS_DESKTOP
class $modify(BufferKeyboard, CCKeyboardDispatcher) {
    void dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (key == enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat) {
            nextPreset();
        }
        CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
#endif

$on_mod(Loaded) {
    log::info("[InputBuffer] Loaded v0.5.0. Default window: {} ticks. Ship/wave assist: {}.",
        intSetting("ticks"), PIB_ENABLE_ASSIST ? "built in" : "not built");
}
