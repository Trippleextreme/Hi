# v0.5.0
- Ship, wave and **swing** assist: a hidden copy of your player is run ahead, and an input that would crash is delivered a few ticks late.
- **Dual mode** support for the assist (off by default, turn on "Assist in dual mode").
- **Pause-menu button** to pick a strength profile per level (default / off / 2 / 4 / 8 / 16 ticks). It is remembered for each level.
- **Practice mode only** switch.
- **Pause assist with these mods**: list mod IDs that should switch the assist off while they are enabled.
- The assist now **verifies** a delay properly: it must keep the hidden player alive a set number of ticks past the point where the raw input would have crashed ("Required safe ticks").
- **Watchdog**: if most of the saves on a level are followed by a crash, the assist pauses itself for that level.
- The time budget **adapts** to how fast your device runs the look-ahead.
- The log gets a line for every real crash (was it predicted?) and a level summary; the replay counter can show assist results.
- Logic tests now live in the repository and run on every build.

# v0.4.2
- Stricter verification of delays, "Required safe ticks" setting, crash log, time-out auto-pause, other mods' hooks run after ours.

# v0.4.1
- The hidden player's simulated crashes no longer reach other mods' death hooks.

# v0.4.0
- First ship and wave assist.

# v0.3.0
- Replay counter, orb/ring buffering, per-mode ticks.

# v0.2.0
- Landing buffer for cube, ball, robot and spider.
