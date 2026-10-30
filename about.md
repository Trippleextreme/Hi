# Predictive Input Buffer

Never lose a tap you made a hair too early, and get a little help with tight ship and wave sections.

## Cube, ball, robot, spider, UFO
If you tap just before you land or touch an orb and let go again before it counts, the tap is replayed at the right moment. It is only replayed if it happens within a short window (set in ticks; 4 ticks is about one frame at 60 FPS).

## Ship, wave and swing
When you press or release, a hidden copy of your player is run a few dozen ticks ahead. If that copy would crash, the same input is tried 1, 2, 3 ... ticks later (up to the limit you choose) and delivered late if that survives. Your real player is never touched, and if the look-ahead is unsure, runs out of time, or meets an orb, pad, trigger or portal, your input goes through unchanged.

## One slider
"Buffer ticks" sets the strength for every game mode. 0 turns the mod off.

## Pause menu
Use the button at the bottom of the pause menu to choose a strength for the current level.

## Accuracy test
Turn on "Accuracy test (log only)" in the settings, play some ship or wave, and look for `drift` in the Geode log. It compares the look-ahead's predicted path with where your player really went and never changes your inputs. The summary line at the end of a level tells you how close the match was.

## Good to know
- It is a timing assist. Think about whether you want it on for runs you plan to submit anywhere; the **Practice mode only** setting turns it off outside practice.
- The look-ahead does not see moving objects. If the assist keeps failing on a level it pauses itself.
- Look for `InputBuffer` in the Geode log to see what it did.
- Other mods can react to the hidden player the ship/wave look-ahead uses. The mod switches hitbox displays that were turned on by it back off. If another mod still misbehaves, add its mod ID to "Pause assist with these mods".
