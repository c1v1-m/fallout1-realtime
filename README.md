# Fallout 1 Realtime

**Fallout 1 in realtime for Steam Deck and PC.**

<!-- TODO: a screenshot or a short GIF of a fight goes here -->

Fallout 1 where nobody waits for their turn. You walk with the stick, hold the left trigger to lock onto an enemy and pull the right one to shoot, while the raiders and mutants come at you at the same time. Action points are still there. They refill as you fight, faster when the enemy is close, so standing back and kiting does not pay.

It is made for a controller, from the first screen to the ending, and you can play it from start to finish. Mouse and keyboard still work.

You need your own copy of Fallout from [Steam](https://store.steampowered.com/app/38400) or [GOG](https://www.gog.com/game/fallout).

## What is new

- Realtime combat. Everyone moves and attacks at the same time.
- Your stats decide how fast you swing, aim and reload. Agility brings your action points back faster.
- The whole game plays on a gamepad: fighting, looting, barter, dialogue, the Pip-Boy, the character screen.
- Radial menus for skills, weapons, ammo and drugs. A stimpack is one press of RB.
- On the Steam Deck it starts straight from the Steam library, in Game Mode.
- Widescreen at the original scale, smooth movement and VSync.
- A MODS page in the options, where most of this can be switched back toward the original.

## Install

Download the latest zip from [Releases](../../releases) and unzip it.

### Steam on a PC

1. Run `Fallout1-Realtime-Setup.exe` and click **Install**. It finds your Steam copy of Fallout by itself.
2. Press **Play** on Fallout in Steam.

### Steam Deck

1. Switch to Desktop Mode and unzip the download.
2. Open the `steamdeck` folder, right-click inside it, choose **Open Terminal Here** and type `bash install-steamdeck.sh`.
3. Go back to Game Mode and press **Play** on Fallout.

If the buttons act like a mouse, open Fallout's controller settings in Steam and pick the **Gamepad** layout.

### GOG or a CD copy

Run `Fallout1-Realtime-Setup.exe`, click **Browse...**, choose your Fallout folder and click **Install**. Then start `fallout1-realtime.exe` from that folder.

### Uninstall

Run the installer again and click **Uninstall**. On the Deck it is `bash install-steamdeck.sh uninstall`. Steam's **Verify integrity of game files** brings the original back as well. Your saves and settings are never touched.

## Controls

| Button | What it does |
|---|---|
| Left stick | Walk, hold X to run |
| LT (hold) | Lock onto the nearest enemy |
| RT | Attack, hold to keep attacking |
| R3 | Called shot |
| LB | Reload |
| RB | Stimpack |
| A | Use what is lit up |
| L3, Y, B | Skill, weapon and item wheels |
| BACK | Inventory, hold to run to the nearest exit |
| START (hold) | Quick save |

Every screen has its own list in the game: Pip-Boy, Archives, CONTROLS.

## Status

Version 0.1.0, the first public release, so expect some rough edges. If something breaks, please open an [issue](../../issues). It helps a lot if you put `trace_log=1` under `[preferences]` in `fallout.cfg`, play until it happens again and attach the newest `trace_*.log` from the game folder.

Known: on some PCs (seen with an AMD graphics card) the Steam overlay does not show. On the Steam Deck it works.

## Building

Windows, with Visual Studio 2026 and CMake:

```
cmake -S . -B build-release -G "Visual Studio 18 2026" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DFALLOUT_RELEASE=ON
cmake --build build-release --config Release --target fallout1-realtime-setup
```

That builds `fallout1-realtime.exe` and the installer. Leave out `-DFALLOUT_RELEASE=ON` for the development build, which keeps every diagnostic on.

## Credits

Fallout 1 Realtime is a modified version of [Fallout Community Edition](https://github.com/alexbatalov/fallout1-ce) by Alexander Batalov, which re-implements the original engine. Everything changed after upstream commit `0609bcf` is the work of c1v1-m.

Fallout is a trademark of Bethesda Softworks. This project contains no game data.

## License

The source code is available under the [Sustainable Use License](LICENSE.md), the same as Fallout Community Edition: free, non-commercial use and distribution only.
