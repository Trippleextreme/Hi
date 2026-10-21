// Predictive Input Buffer
//
// What it does
//   If you tap jump while airborne and let go again before touching down, the
//   game normally drops that tap. This mod remembers it and replays the press
//   on the landing tick, as long as the landing happens within a short window
//   after the original press. The replayed press is held for as long as you
//   originally held it.
//
//   A "tick" is one 1/240 s physics step, so 4 ticks is about 1 frame at
//   60 FPS. The strength is the "Buffer stage" setting (1 to 5), which F6
//   also cycles on desktop:
//     1: 2 ticks   2: 4 ticks   3: 8 ticks   4: 16 ticks   5: off
//
// What it does not do
//   - Only ground-based modes (cube, ball, robot, spider) are buffered.
//     Ship, UFO, wave and swing are left alone.
//   - Orbs and rings are not buffered yet.
//   - There is no trajectory prediction and no special wave handling.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>

using namespace geode::prelude;

namespace {
    constexpr double kTickSeconds = 1.0 / 240.0;
    constexpr double kEpsilon = 1e-6; // absorbs float rounding in dt at window edges

    struct Stage {
        int ticks;
        bool enabled;
    };

    constexpr Stage kStages[] = {
        {2, true},
        {4, true},
        {8, true},
        {16, true},
        {0, false},
    };
    constexpr int kStageCount = sizeof(kStages) / sizeof(kStages[0]);

    // 0-based index into kStages, read from the "stage" setting (1 to 5).
    int stageIndex() {
        auto value = Mod::get()->getSettingValue<int64_t>("stage");
        return std::clamp(static_cast<int>(value), 1, kStageCount) - 1;
    }

    // Buffer window in seconds, or 0 when the buffer is off.
    double windowSeconds() {
        auto const& stage = kStages[stageIndex()];
        return stage.enabled ? stage.ticks * kTickSeconds : 0.0;
    }

    void logStage(int index) {
        auto const& stage = kStages[index];
        if (!stage.enabled) {
            log::info("[InputBuffer] Stage {}: OFF (100% raw input)", index + 1);
            return;
        }
        log::info(
            "[InputBuffer] Stage {}: {} ticks ({:.2f} frames @ 60 FPS)",
            index + 1,
            stage.ticks,
            stage.ticks / 4.0
        );
    }

    // One entry per player (0 = player 1, 1 = player 2).
    struct PlayerState {
        bool wasGrounded = true;
        bool pending = false;      // jump pressed while airborne, still waiting for a landing
        bool released = false;     // ...and already let go of again
        double age = 0.0;          // seconds since that press
        double heldFor = 0.0;      // how long the press was held
        bool injected = false;     // we pressed jump on the player's behalf
        double injectedLeft = 0.0; // seconds until we let go again

        void reset() { *this = PlayerState{}; }
    };

    PlayerState g_state[2];
    bool g_injecting = false;

    bool needsGroundToJump(PlayerObject* player) {
        return !(player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing);
    }

    // Sends a jump press/release through the normal input path.
    void inject(bool down, bool isPlayer1) {
        auto* layer = PlayLayer::get();
        if (!layer) return;
        g_injecting = true;
        layer->handleButton(down, 1, isPlayer1);
        g_injecting = false;
    }

#ifdef GEODE_IS_DESKTOP
    void nextStage() {
        int next = (stageIndex() + 1) % kStageCount;
        (void)Mod::get()->setSettingValue<int64_t>("stage", next + 1);
        logStage(next);

        auto const& stage = kStages[next];
        auto text = stage.enabled
            ? fmt::format("Input buffer: {} ticks", stage.ticks)
            : std::string("Input buffer: off");
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
            auto& s = g_state[isPlayer1 ? 0 : 1];

            if (down) {
                // A real press takes over from anything we pressed ourselves.
                s.injected = false;

                if (player && windowSeconds() > 0.0
                    && needsGroundToJump(player) && !player->m_isOnGround) {
                    s.pending = true;
                    s.released = false;
                    s.age = 0.0;
                    s.heldFor = 0.0;
                } else {
                    s.pending = false;
                }
            } else if (s.pending) {
                s.released = true;
            }
        }

        // The original input always goes through untouched.
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// Runs once per physics step per player: ages the buffered tap and replays it on landing.
class $modify(BufferPlayer, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);

        auto* playLayer = PlayLayer::get();
        if (!playLayer) return;

        int index = -1;
        if (this == playLayer->m_player1) index = 0;
        else if (this == playLayer->m_player2) index = 1;
        if (index < 0) return;

        auto& s = g_state[index];
        bool grounded = m_isOnGround;
        bool landed = grounded && !s.wasGrounded;
        double window = windowSeconds();

        // Let go of a replayed press once the original hold time has elapsed.
        if (s.injected) {
            s.injectedLeft -= dt;
            if (s.injectedLeft <= 0.0) {
                s.injected = false;
                inject(false, index == 0);
            }
        }

        if (s.pending) {
            s.age += dt;
            if (!s.released) s.heldFor += dt;

            if (landed) {
                // Tap already finished and landing is inside the window: replay it.
                // If the button is still down the game jumps on landing by itself.
                if (s.released && window > 0.0 && s.age <= window + kEpsilon) {
                    inject(true, index == 0);
                    s.injected = true;
                    s.injectedLeft = std::max(s.heldFor, kTickSeconds);
                    log::debug("[InputBuffer] replayed tap after {:.1f} ticks", s.age / kTickSeconds);
                }
                s.pending = false;
            } else if (window <= 0.0 || s.age > window + kEpsilon) {
                s.pending = false; // too early, forget it
            }
        }

        s.wasGrounded = grounded;
    }
};

// Clear any buffered state when the level restarts.
class $modify(BufferPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        for (auto& s : g_state) s.reset();
    }
};

#ifdef GEODE_IS_DESKTOP
class $modify(BufferKeyboard, CCKeyboardDispatcher) {
    void dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (key == enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat) {
            nextStage();
        }
        CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
#endif

$on_mod(Loaded) {
    log::info("[InputBuffer] Loaded. Strength is the \"Buffer stage\" setting.");
    logStage(stageIndex());
}
