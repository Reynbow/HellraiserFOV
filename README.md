# Hellraiser FOV - Gameplay Field of View for Hellraiser: Revival

A mod for Clive Barker's Hellraiser: Revival: your own field of view for gameplay only, a separate FOV for aiming down sights and for your arms and held items, all set live in a menu in the game.

Download and install instructions are on Nexus Mods (search for Hellraiser FOV in the Clive Barker's Hellraiser: Revival section). This repository is the full source.

## Features

- Gameplay only: only the player's own camera changes. Cutscenes and other cameras keep the game's FOV, and camera blends between them and the player stay smooth.
- In-game menu (F6) that works with the mouse or keyboard. Changes show live.
- Aim FOV: your own FOV while aiming down sights, or the game's own aiming FOV (the default).
- Viewmodel FOV: the FOV of your arms and held items, set separately (the game draws them with a FOV of their own).
- Keeps the game's FOV effects: the game's own FOV changes, such as sprinting and climbing ladders, are kept, scaled around your FOV. Can be turned off.
- Smooth transitions: the blend time between your FOV and the game's is adjustable, from instant up to 2 seconds.
- Hotkeys: Page Up / Page Down change the FOV at any time (with a small popup), End toggles the mod for a quick comparison.
- Any resolution and aspect ratio, ultrawide included; the menu stays sharp at any resolution and Windows display scaling.
- Update-friendly: finds everything it needs by name in the game's own reflection data instead of hard-coded addresses, so it adapts to most game patches.

## Requirements (to play)

None. It's a standalone `winmm.dll`: no UE4SS, ReShade or other loader needed.

## Building

Requires Windows and the Visual Studio 2022 Build Tools (C++ workload). Run `build.bat`; it builds `build\winmm.dll` and `build\offline_check.exe`. To install your build, put `winmm.dll` in `Hellraiser\Binaries\Win64\` in the game folder, next to `Hellraiser-Win64-Shipping.exe`.

`offline_check.exe <path to Hellraiser-Win64-Shipping.exe> build\winmm.dll` maps the game executable without running it and checks the code searches that find the hooked functions, the vtable hooking and the winmm forwarding against the installed game. `tools/gen_proxy_exports.py` regenerates `src/proxy_exports.h` from the system `winmm.dll`.

## How it works

The game loads `winmm.dll` from its own folder first. This one forwards every real winmm function to `C:\Windows\System32\winmm.dll` and starts the mod inside the game process.

- **Finding things:** the game ships without debug symbols, so nothing is hard-coded. Once the game window opens, the mod finds the engine's global object array and name table in the executable's data, checks the core engine layouts against well-known objects (`FVector`, `AActor`), and then looks up every class, property offset and function it uses by name, through the engine's own reflection data. The three functions reflection doesn't describe are found in the game's code: `UGameViewportClient::PostRender` (the function before `DrawTransition`, found by its "LoadingMessage" text), `APlayerCameraManager::ApplyCameraModifiers` (the last call in `UpdateViewTarget` that's passed the view, found by its "FreeCam_Default" camera style name) and `UObject::ProcessEvent` (by its function flag tests).
- **The FOV:** the player camera manager's `ApplyCameraModifiers` runs once per view target every frame; the mod rescales the FOV only when that view target is the player's own view. The game renders the player through `BP_PlayerCamera`, the child actor of the character's `FPCameraActor` component; cutscene cameras are separate view targets, so they're never touched, and blends between them and the player interpolate naturally. Changes blend in and out over the configurable time, in tan(FOV/2) space so they're proportional on screen. With **Keep the game's FOV effects** on, your FOV scales around the game's own gameplay FOV (the player Blueprint's `GetDefaultCameraFov`), so the game's sprint and ladder FOV changes are kept.
- **Aiming:** while `IsAimingDownSight` says the player is aiming, the FOV eases towards the Aim FOV, or by default towards the game's own aiming FOV, not scaled by your gameplay FOV.
- **Viewmodel FOV:** the game uses the engine's first-person rendering, which draws the arms and held items with a FOV of their own (`FMinimalViewInfo::FirstPersonFOV`). With a viewmodel FOV set, the mod changes that value in the same hook, blended like the main FOV, and keeps any change the game makes to it (such as aiming) scaled around its usual value.
- **The menu:** drawn with the engine's own `UCanvas` from `UGameViewportClient::PostRender` (no D3D12 hooking), through the canvas's `K2_DrawTexture`, `K2_DrawText` and `K2_TextSize` functions called with `ProcessEvent`. Text is rasterized at the exact size needed (the runtime font's `LegacyFontSize` is set for each call), so it stays sharp at any resolution and Windows display scaling. Keyboard and mouse input come from the game window's messages; keys the mod uses, and the mouse while the menu is open, aren't passed on to the game.

There is no network code; the mod writes only its own settings and log files in the game's `Binaries\Win64` folder.

## Antivirus false positives

Some antivirus tools may flag `winmm.dll` because it is an unsigned proxy DLL that hooks into the game. This is a false positive. The full source is here, so you can check it or build the DLL yourself with `build.bat`.

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

## License

MIT, see [LICENSE](LICENSE). Clive Barker's Hellraiser: Revival is published by Saber Interactive; this project isn't affiliated with or endorsed by its developers or publisher.
