# Alan Wake 2 — DualShock4 V2 Audio Fix

[English](README.md) | [简体中文](README.zh-CN.md)

A lightweight DLL-based mod for *Alan Wake 2* that fixes audio routing issues when using a Sony DualShock4 V2 (PS4) controller on Windows.

## Overview

When a headset is connected to the DualShock4 V2's 3.5 mm audio jack, *Alan Wake 2* may unexpectedly route audio to the controller's built-in speaker after controller input.

This mod helps prevent the game from switching audio output to the controller's built-in speaker, allowing audio to remain routed through the headset connected to the controller.

## Features

v1.0.0
- Fixes unexpected audio routing changes with the DualShock4 V2.
- Supports native controller input without requiring DS4Windows.
- Helps preserve audio output through the controller's headset jack.
- Lightweight DLL-based mod.

v1.0.1
- Mapped the DS4 left touchpad to M, fixing the map toggle issue.

## Requirements

- *Alan Wake 2* (Windows)
- Sony DualShock4 V2 controller connected via USB
- Headset connected to the controller's 3.5 mm audio jack
- Microsoft Visual C++ Redistributable (if required by the build)

## Installation

1. Locate the *Alan Wake 2* game installation directory.
2. Find the original `xaudio2_9redist.dll` in the game root directory.
3. Rename the original DLL to `xaudio2_9_original.dll`.
4. Download the latest mod release from the [Releases](../../releases) page.
5. Copy the mod's `xaudio2_9redist.dll` into the game root directory, alongside the renamed original DLL.
6. Connect your DualShock4 V2 controller via USB and plug your headset into its 3.5 mm audio jack.
7. Launch *Alan Wake 2* and test the audio output.

### Installation Directory

After installation, the relevant files should look like this:

```text
Alan Wake 2/
├── AlanWake2.exe
├── xaudio2_9redist.dll          # Mod DLL
└── xaudio2_9_original.dll       # Original game DLL
```

**Important:** Do not delete the original DLL. The mod relies on it to preserve the game's original audio functionality.

## Usage

1. Connect the DualShock4 V2 controller to your PC using a USB data cable.
2. Connect your headset to the controller's 3.5 mm audio jack.
3. Make sure the original DLL has been renamed and the mod DLL is in the game root directory.
4. Launch *Alan Wake 2*.
5. Use the controller normally. Audio should remain routed through the headset connected to the controller instead of unexpectedly switching to the controller's built-in speaker.

No additional configuration or DS4Windows installation is required.

## Building from Source

### Requirements

- Windows
- Visual Studio with **Desktop development with C++** installed
- CMake
- Git (if cloning the repository)

### Build Instructions

Open **Developer PowerShell for VS** in the project root directory and run:

```powershell
mkdir build
cd build
cmake -A x64 ..
cmake --build . --config Release
```

After a successful build, locate the generated mod DLL in the build output directory.

Copy the resulting `xaudio2_9redist.dll` into the *Alan Wake 2* game root directory, following the installation instructions above.

## Compatibility

This mod is intended for the Windows version of *Alan Wake 2* and a Sony DualShock4 V2 controller connected via USB.

Compatibility with other controllers, wireless connections, or future game updates has not been verified.

## Troubleshooting

- Ensure that the key for opening the map is set to M.
- Make sure the controller is connected before launching the game.
- Check that the headset is securely connected to the controller.
- Verify that the original DLL is named `xaudio2_9_original.dll`.
- Verify that the mod DLL is named `xaudio2_9redist.dll` and is located in the game root directory.
- If audio routing is still incorrect, report your Windows version, game version, controller model, and connection method in the [Issues](../../issues) section.

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) — API hooking library.

## Disclaimer

This is an unofficial community mod and is not affiliated with or endorsed by Remedy Entertainment or Sony Interactive Entertainment.

Use at your own risk.
