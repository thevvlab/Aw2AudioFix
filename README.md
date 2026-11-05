# Alan Wake 2 DualShock 4 Audio Fix

A lightweight proxy DLL (`xaudio2_9redist.dll`) that fixes DualShock 4 audio routing issues and global audio lockups in **Alan Wake 2** on PC.

## Features
- **Fixes Audio Routing**: Automatically intercepts and rewrites DS4 HID reports (`0xE0 00 03` -> `0xE0 00 00`), forcing game audio to stay in your headphones instead of bleeding into the controller speaker.
- **Prevents Audio Lockups**: Eliminates global sound freezes caused by Windows audio endpoint hot-swapping or controller disconnections.
- **Zero Performance Impact**: Built using lightweight inline MinHook architecture.

## How to Install
1. Go to the [Releases](../../releases) page and download `xaudio2_9redist.dll`.
2. Copy the DLL into your Alan Wake 2 game installation directory (where `AlanWake2.exe` is located).
3. Launch the game and enjoy!

## How to Build from Source
If you want to compile it yourself using Visual Studio and CMake:
```powershell
mkdir build
cd build
cmake -A x64 ..
cmake --build . --config Release
