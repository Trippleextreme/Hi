# Predictive Input Buffer (Geode mod for Geometry Dash)

Replays jump taps made just before landing or touching an orb, and delays ship, wave and swing inputs by a few ticks when a look-ahead predicts a crash.

- **Source:** `src/main.cpp`
- **Settings:** see `mod.json` (or the in-game settings screen)
- **Logic tests:** `logic_tests.cpp` + `mock_geode.hpp`. They run the mod's logic against mock game classes on every push (see `.github/workflows/main.yml`). Run them yourself with:

```sh
mkdir -p .mock/Geode/modify
cp mock_geode.hpp .mock/Geode/Geode.hpp
for h in CCKeyboardDispatcher GJBaseGameLayer PlayLayer PlayerObject PauseLayer; do echo '#pragma once' > .mock/Geode/modify/$h.hpp; done
g++ -std=c++23 -fno-access-control -I.mock logic_tests.cpp -o logic_tests && ./logic_tests
```

The tests prove the mod's own logic only. They cannot prove that the real game behaves the same way, so check the Geode log (`InputBuffer`) when you play.

## Builds
The workflow builds Android64 (arm64-v8a). Windows and macOS are not built right now.

## Switches in `src/main.cpp`
If a build fails on one part, set that macro near the top to `0`:
`PIB_ENABLE_RINGS`, `PIB_ENABLE_ASSIST`, `PIB_ENABLE_DUAL`, `PIB_ENABLE_PAUSEUI`, `PIB_ENABLE_PRACTICE`.
