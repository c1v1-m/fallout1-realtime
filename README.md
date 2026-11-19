# Fallout 1 Realtime

**Fallout 1 in realtime for Steam Deck and PC.**

<!-- TODO: a screenshot or a short GIF of a fight goes here -->

Fallout 1 where nobody waits for their turn. You walk with the stick, hold the left trigger to lock onto an enemy and pull the right one to shoot, while the raiders and mutants come at you at the same time. Action points are still there. They refill as you fight, faster when the enemy is close, so kiting does not pay.

It is made for a controller, from the first screen to the ending, and you can play it from start to finish.

You need your own copy of Fallout from [Steam](https://store.steampowered.com/app/38400) or [GOG](https://www.gog.com/game/fallout).

> [!CAUTION]
> **BACK UP YOUR SAVES BEFORE YOU PLAY.** The game uses save slots 1 to 5 for its quick save and autosaves and **overwrites whatever is in them**. If you have your own saves there, save them again into slot 6 or later, or copy the `SAVEGAME` folder somewhere safe. See [Saves](#saves).

## What is new

- Realtime combat. Everyone moves and attacks at the same time, and you can shoot and keep moving.
- Every shot flies. Bullets travel at their real speed, each calibre its own, with tracers in a burst. Rockets and plasma you can see coming and step out of, lasers hit at once.
- Enemies fight smarter. They wait for each other and come round a corner together, cover the door you are holding, shoot from where they can see you, reload out of your sight, jump clear of grenades and step out of your line of fire.
- Enemies see you only with a clear line of sight. When they lose you, they search where they last saw you.
- Your stats decide how fast you swing, aim and reload. Agility brings your action points back faster.
- A stimpack heals over the next few seconds in a fight, so hits still hurt while it works. Hold the button and you inject one after another.
- With the Awareness perk, red dots on the ground show how far a grenade or a rocket will blast, while you aim and while it flies.
- The whole game plays on a gamepad: fighting, looting, barter, dialogue, the Pip-Boy, the character screen.
- With a pad, the screens show which pad button does what: the interface bar, the use icon, dialogue, the inventory, character selection and the character screen.
- Two quick slots: LB for a stimpack or any drug or tool, RB for a grenade. Radial menus for skills, weapons, ammo and drugs.
- Autosaves when you reach a new place or a quest moves on, and a quick save on the controller.
- On the Steam Deck it starts straight from the Steam library, in Game Mode.
- Widescreen at the original scale, smooth movement and VSync.
- A MODS page in the options, where most of this can be switched back toward the original.

## Install

Download the latest zip from [Releases](../../releases) and unzip it.

Before it changes anything, the installer copies `fallout.cfg`, `f1_res.ini` and your whole `SAVEGAME` folder into a new dated folder in `Fallout1-Realtime-backup`, inside the game folder. It does this on every install and every update, and if the copy fails, nothing is installed. To get your saves back, copy `SAVEGAME` from the oldest dated folder into the game's `DATA` folder.

### Steam on a PC

1. Run `Fallout1-Realtime-Setup.exe` and click **Install** (**Update** over an older version). It finds your Steam copy of Fallout by itself.
2. Press **Play** on Fallout in Steam.

### Steam Deck

1. Switch to Desktop Mode and unzip the download.
2. Open the `steamdeck` folder, right-click inside it, choose **Open Terminal Here** and type `bash install-steamdeck.sh`.
3. Go back to Game Mode. On Fallout's page open the controller settings, pick **Templates** and choose **Gamepad with Mouse Trackpad**. Steam's default layout for Fallout turns the buttons into keyboard keys, and the game then never sees a controller.
4. Press **Play** on Fallout.

### GOG or a CD copy

Run `Fallout1-Realtime-Setup.exe`, click **Browse...**, choose your Fallout folder and click **Install**. Then start `fallout1-realtime.exe` from that folder.

### Uninstall

Run the installer again and click **Uninstall**. On the Deck it is `bash install-steamdeck.sh uninstall`. Steam's **Verify integrity of game files** brings the original back as well. Your saves, `fallout.cfg`, `f1_res.ini` and the backups stay. **Screen resolution** and **Internal resolution** on the MODS page write to the High Resolution Patch's `f1_res.ini`, so to undo them after you uninstall, copy `f1_res.ini` back from the oldest backup.

## Controls

| Button | What it does |
|---|---|
| Left stick | Walk |
| Right stick | Look, aim |
| D-pad | Next action, scroll |
| Hold Up | Pip-Boy |
| Hold Down | Character |
| Hold Left | Automap |
| Hold Right | End combat |
| LB / RB | Quick slots |
| LT | Aim |
| RT | Shoot |
| L3 | Skills |
| R3 | Called shot |
| A | Confirm, action |
| B | Cancel |
| B hold | Drugs, items |
| X hold | Run |
| Y | Swap hands |
| Y hold | Weapons, ammo |
| LT + X | Reload |
| LT + Y | Firing mode |
| BACK | Inventory |
| BACK hold | Run to exit |
| START | Pause |
| START hold | Quick save |

The same list is in the game: press Y on the pause menu (START), or open the Pip-Boy's Archives, CONTROLS. On a Steam Deck that list calls the buttons L1, R1, L2, R2, VIEW and MENU.

Other screens show their pad buttons on them. In the inventory B closes it when the pointer is not on an item, in a talk LB opens REVIEW, and LB / RB pick a character on New Game and turn the tabs of the character screen.

## Saves

The save format is unchanged, so saves from the original game and from Fallout Community Edition should load and you can carry on where you left off.

The first five slots on the save screen are used by the game itself:

| Slot | What goes there |
|---|---|
| 1 | Quick save, when you hold START |
| 2, 3, 4 | Autosave each time you enter a new place, taking turns, so the last three places are kept |
| 5 | Autosave each time a quest moves on |

An autosave waits until the fight is over. Holding START quick-saves whenever you are out in the world, in a fight too.

> [!CAUTION]
> **Your own saves in slots 1 to 5 WILL BE OVERWRITTEN.** Save them again into slot 6 or later before you start, or back up the `SAVEGAME` folder.

## Status

Version 0.2.0, an early release, so expect some rough edges. Most of the playtesting was done on the Rough combat difficulty. If something breaks, please open an [issue](../../issues). It helps a lot if you put `trace_log=1` under `[preferences]` in `fallout.cfg`, play until it happens again and attach the newest `trace_*.log` from the game folder.

Known: on some PCs (seen with an AMD graphics card) the Steam overlay does not show. On the Steam Deck it works.

## What's next

- Polish, everywhere it still feels rough.
- New hand-made animations, starting with reloading.
- The Steam overlay on the PCs where it does not show yet.

## Building

Windows, with Visual Studio 2026 and CMake:

```
cmake -S . -B build-release -G "Visual Studio 18 2026" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DFALLOUT_RELEASE=ON
cmake --build build-release --config Release --target fallout1-realtime-setup
```

That builds `fallout1-realtime.exe` and the installer. Leave out `-DFALLOUT_RELEASE=ON` for the development build, which keeps every diagnostic on.

## Credits

Fallout 1 Realtime is a modified version of [Fallout Community Edition](https://github.com/alexbatalov/fallout1-ce) by Alexander Batalov, which re-implements the original engine. Everything changed after upstream commit `0609bcf` is the work of c1v1-m.

It uses [SDL2](https://www.libsdl.org) under the zlib license, the YUV conversion code in SDL2 under a BSD license, and adecode by Alexander Batalov under the MIT License. Their notices are in `NOTICE.txt` in the release zip.

Fallout is a trademark of Bethesda Softworks. This project contains no game data.

## License

The source code is available under the [Sustainable Use License](LICENSE.md), the same as Fallout Community Edition: free, non-commercial use and distribution only.
