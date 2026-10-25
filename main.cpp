// Predictive Input Buffer v0.4.2
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
//   safely (orbs, pads, triggers, portals). Dual mode and platformer mode are skipped.
//
// Not covered
//   - Swing is never touched.
//   - Orbs/pads/portals/triggers inside the look-ahead are not simulated.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
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

    // Ticks for this player's current mode: default value, or the per-mode override.
    int ticksFor(PlayerObject* p) {
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
            label->setString(
                fmt::format("Buffered: {} (total {})", g_attemptReplays, g_totalReplays).c_str()
            );
        }
    }

    void onReplay() {
        ++g_attemptReplays;
        ++g_totalReplays;
        log::debug("[InputBuffer] replayed tap (attempt {}, total {})", g_attemptReplays, g_totalReplays);
        refreshCounter(PlayLayer::get());
    }


#if PIB_ENABLE_ASSIST
    // ------------------------------------------------------- ship/wave assist
    using Clock = std::chrono::steady_clock;

    struct Delayed {
        bool down;
        double left; // seconds until it is delivered
    };

    std::vector<Delayed> g_delayed;          // inputs held back for player 1
    PlayerObject* g_dummy = nullptr;         // hidden copy used for the look-ahead
    PlayLayer* g_dummyLayer = nullptr;
    bool g_simulating = false;
    bool g_simDied = false;
    bool g_simTainted = false;               // the copy touched something we can't simulate
    double g_lastDt = kTickSeconds;          // dt of the last real physics step

    constexpr int kMaxLookAhead = 96;        // hard cap on any single simulation, in ticks
    int g_timeoutStreak = 0;                 // look-aheads in a row that ran out of time
    bool g_assistSuspended = false;          // paused for this level after too many time-outs
    int g_assistSaves = 0;                   // inputs delayed this attempt
    bool g_deathLogged = false;              // one crash line per attempt

    struct Note {
        bool valid = false;
        Clock::time_point when;
        int ticks = 0;     // saves: ticks delayed. predictions: tick the raw input would crash on
        bool down = false;
    };
    Note g_lastSave;
    Note g_lastPrediction;

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
    }

    // One line per real crash: was it predicted, did a save just happen, or was the assist idle?
    void logCrash() {
        auto now = Clock::now();
        auto since = [&](Note const& n) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - n.when).count();
        };
        if (g_lastPrediction.valid && since(g_lastPrediction) < 1500) {
            log::info(
                "[InputBuffer] CRASH: predicted {} ms earlier (raw {} would crash in {} ticks, no safe delay found). Saves this attempt: {}",
                since(g_lastPrediction), g_lastPrediction.down ? "PRESS" : "RELEASE",
                g_lastPrediction.ticks, g_assistSaves
            );
        } else if (g_lastSave.valid && since(g_lastSave) < 1500) {
            log::info(
                "[InputBuffer] CRASH: {} ms after a save (+{} ticks on a {}). Saves this attempt: {}",
                since(g_lastSave), g_lastSave.ticks, g_lastSave.down ? "PRESS" : "RELEASE", g_assistSaves
            );
        } else {
            log::info("[InputBuffer] CRASH: no recent look-ahead activity. Saves this attempt: {}", g_assistSaves);
        }
    }

    bool assistEnabled() {
        return Mod::get()->getSettingValue<bool>("assist-enabled");
    }

    int assistLimit(PlayerObject* p) {
        int64_t ticks = intSetting(p->m_isDart ? "wave-ticks" : "ship-ticks");
        return static_cast<int>(std::clamp<int64_t>(ticks, 0, 16));
    }

    // Ship or wave, single player, not platformer, not dual mode.
    bool assistApplies(PlayLayer* layer, PlayerObject* p) {
        return assistEnabled()
            && (p->m_isShip || p->m_isDart)
            && !p->m_isPlatformer
            && !layer->m_gameState.m_isDualMode;
    }

    // The hidden player. Its trails and particles go into a layer that is never
    // shown, so nothing it does is visible.
    PlayerObject* ensureDummy(PlayLayer* layer) {
        if (g_dummy && g_dummyLayer == layer) return g_dummy;
        auto* sandbox = CCLayer::create();
        sandbox->retain();
        auto* dummy = PlayerObject::create(1, 1, layer, sandbox, true);
        if (!dummy) return nullptr;
        dummy->retain();
        dummy->setVisible(false);
        g_dummy = dummy;
        g_dummyLayer = layer;
        return dummy;
    }

    struct SimResult {
        int deathTick;   // tick the copy died on, or the horizon if it survived
        bool tainted;
        bool timedOut;
    };

    // Runs the hidden player `horizon` ticks ahead. The input (`down`) is applied
    // on tick `delay`. Order per tick follows the original: update, rotation,
    // collisions, then check whether it died.
    SimResult simulate(
        PlayLayer* layer, PlayerObject* dummy, PlayerObject* real,
        bool held, bool down, int delay, int horizon, double dt,
        Clock::time_point deadline
    ) {
        SimResult out{horizon, false, false};

        g_simulating = true;
        g_simDied = false;
        g_simTainted = false;

        dummy->copyAttributes(real);
        dummy->setPosition(real->getPosition());
        dummy->m_yVelocity = real->m_yVelocity;
        dummy->m_isDead = false;
        dummy->releaseAllButtons();
        if (held) dummy->pushButton(PlayerButton::Jump);

        for (int tick = 0; tick < horizon; ++tick) {
            if (tick == delay) {
                if (down) dummy->pushButton(PlayerButton::Jump);
                else dummy->releaseButton(PlayerButton::Jump);
            }

            dummy->update(static_cast<float>(dt));
            dummy->updateRotation(static_cast<float>(dt));
            layer->checkCollisions(dummy, static_cast<float>(dt), false);

            if (g_simDied || dummy->m_isDead) {
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
        return out;
    }

    // How many ticks to hold this input back (0 = deliver it now).
    int decideDelay(PlayLayer* layer, PlayerObject* real, bool down) {
        if (g_assistSuspended) return 0;
        int limit = assistLimit(real);
        if (limit <= 0) return 0;

        auto* dummy = ensureDummy(layer);
        if (!dummy) return 0;

        bool wave = real->m_isDart;
        int horizon = wave ? 38 : 48;
        int margin = static_cast<int>(std::clamp<int64_t>(intSetting("assist-margin"), 4, 40));
        bool held = real->buttonDown(PlayerButton::Jump);
        int64_t budgetMs = std::clamp<int64_t>(intSetting("assist-budget-ms"), 0, 30);
        auto deadline = Clock::now() + std::chrono::milliseconds(budgetMs);

        // 1. What happens with the raw input?
        SimResult raw = simulate(layer, dummy, real, held, down, 0, horizon, g_lastDt, deadline);
        if (raw.timedOut) {
            noteTimeout();
            log::debug("[InputBuffer] look-ahead skipped: out of time");
            return 0;
        }
        if (raw.tainted || raw.deathTick >= horizon) {
            g_timeoutStreak = 0;
            return 0;
        }

        // 2. Try later delivery, smallest delay first. A delay only counts if the
        //    hidden player then survives `margin` ticks PAST the tick where the raw
        //    input would have crashed. The check is not capped at the look-ahead
        //    window, so a crash near the end of the window is verified properly
        //    instead of just being pushed out of view.
        int need = std::min(raw.deathTick + margin, kMaxLookAhead);
        for (int delay = 1; delay <= limit; ++delay) {
            int span = std::min(std::max(need, delay + margin), kMaxLookAhead);
            SimResult r = simulate(layer, dummy, real, held, down, delay, span, g_lastDt, deadline);
            if (r.timedOut) {
                noteTimeout();
                return 0;
            }
            if (r.tainted) return 0;
            if (r.deathTick >= span) {
                g_timeoutStreak = 0;
                ++g_assistSaves;
                g_lastSave = Note{true, Clock::now(), delay, down};
                log::info(
                    "[InputBuffer] *** SAVED +{} TICKS ({:.2f} frames @ 60fps) {}! (Frame0: {} -> checked safe for {} ticks) [Limit: {}] ***",
                    delay, delay / 4.0, down ? "PRESS" : "RELEASE", raw.deathTick, span, limit
                );
                return delay;
            }
        }

        // No safe delay exists: the raw input goes through. Remember the prediction
        // so the next crash line can say whether it was foreseen.
        g_timeoutStreak = 0;
        g_lastPrediction = Note{true, Clock::now(), raw.deathTick, down};
        return 0;
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
            if (isPlayer1) {
                // Anything arriving behind a held-back input waits behind it, to keep the order.
                if (!g_delayed.empty()) {
                    g_delayed.push_back({down, g_delayed.back().left + kTickSeconds});
                    return;
                }
                if (player && assistApplies(playLayer, player)) {
                    int ticks = decideDelay(playLayer, player, down);
                    if (ticks > 0) {
                        g_delayed.push_back({down, ticks * kTickSeconds});
                        onReplay();
                        return;
                    }
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
        if (index == 0) {
            g_lastDt = dt;
            for (auto& entry : g_delayed) entry.left -= dt;
            while (!g_delayed.empty() && g_delayed.front().left <= kEpsilon) {
                bool down = g_delayed.front().down;
                g_delayed.erase(g_delayed.begin());
                inject(down, true);
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
        if (g_simulating && this == g_dummy) return;
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
        if (g_simulating && player == g_dummy) {
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
        g_dummy = nullptr;      // a fresh hidden player is made for this level
        g_dummyLayer = nullptr;
        g_timeoutStreak = 0;
        g_assistSuspended = false;
        resetAttemptNotes();
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

    void resetLevel() {
        PlayLayer::resetLevel();
        for (auto& buffer : g_buffers) buffer.reset();
        g_attemptReplays = 0;
#if PIB_ENABLE_ASSIST
        g_delayed.clear();
        resetAttemptNotes();
#endif
        refreshCounter(this);
    }
};

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
    log::info("[InputBuffer] Loaded v0.4.2. Default window: {} ticks. Ship/wave assist: {}.",
        intSetting("ticks"), PIB_ENABLE_ASSIST ? "built in" : "not built");
}
