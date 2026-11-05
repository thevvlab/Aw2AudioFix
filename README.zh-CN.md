# 心灵杀手2 — DualShock 4 音频修复

[English](README.md) | [简体中文](README.zh-CN.md)

一款轻量级 DLL 模组，用于修复 Windows 平台上使用 Sony DualShock 4（PS4）手柄时，《心灵杀手2》出现的音频输出异常问题。

## 概述

当耳机连接到 DualShock 4 手柄的 3.5 毫米音频接口时，《心灵杀手2》可能会在玩家操作手柄后，意外地将音频输出切换到手柄内置扬声器。

本模组可以帮助防止游戏将音频输出切换到手柄内置扬声器，使音频保持通过连接在手柄上的耳机输出。

## 功能特性

- 修复 DualShock 4 手柄导致的意外音频输出切换问题。
- 支持原生手柄输入，无需安装 DS4Windows。
- 帮助保持音频通过手柄耳机接口输出。
- 轻量级 DLL 模组。

## 环境要求

- Windows 版《心灵杀手2》
- 通过 USB 连接的 Sony DualShock 4 手柄
- 连接到手柄 3.5 毫米音频接口的耳机
- Microsoft Visual C++ Redistributable（如果构建版本需要）

## 安装方法

1. 找到《心灵杀手2》的游戏安装目录。
2. 在游戏根目录中找到原版 `xaudio2_9redist.dll`。
3. 将原版 DLL 重命名为 `xaudio2_9_original.dll`。
4. 从 [Releases](https://github.com/thevvlab/Aw2AudioFix/releases) 下载最新版本的模组。
5. 将模组的 `xaudio2_9redist.dll` 放入游戏根目录，与重命名后的原版 DLL 放在一起。
6. 通过 USB 连接 DualShock 4 手柄，并将耳机插入手柄的 3.5 毫米音频接口。
7. 启动《心灵杀手2》，测试音频输出是否正常。

### 安装后的目录结构

```text
Alan Wake 2/
├── AlanWake2.exe
├── xaudio2_9redist.dll          # 模组 DLL
└── xaudio2_9_original.dll       # 游戏原版 DLL
```

**重要提示：** 不要删除原版 DLL。模组依赖它来保留游戏原有的音频功能。

## 使用方法

1. 使用 USB 数据线将 DualShock 4 手柄连接到电脑。
2. 将耳机连接到手柄的 3.5 毫米音频接口。
3. 确认原版 DLL 已重命名，且模组 DLL 已放入游戏根目录。
4. 启动《心灵杀手2》。
5. 正常使用手柄。音频应保持通过连接在手柄上的耳机输出，而不会意外切换到手柄内置扬声器。

无需额外配置，也不需要安装 DS4Windows。

## 从源代码构建

### 环境要求

- Windows
- 安装了 **使用 C++ 的桌面开发** 工作负载的 Visual Studio
- CMake
- Git（如果通过克隆仓库获取源代码）

### 构建步骤

在项目根目录中打开 ***Developer PowerShell for VS*，然后运行：

```powershell
mkdir build
cd build
cmake -A x64 ..
cmake --build . --config Release
```

构建成功后，在构建输出目录中找到生成的模组 DLL。

按照上面的安装说明，将生成的 `xaudio2_9redist.dll` 复制到《心灵杀手2》的游戏根目录。

## 兼容性

本模组适用于 Windows 版《心灵杀手2》以及通过 USB 连接的 Sony DualShock 4 手柄。

尚未验证其他手柄、无线连接方式或未来游戏更新的兼容性。

## 故障排除

- 确保在启动游戏之前连接手柄。
- 检查耳机是否牢固地连接到手柄。
- 确认原版 DLL 已重命名为 `xaudio2_9_original.dll`。
- 确认模组 DLL 名称为 `xaudio2_9redist.dll`，并且位于游戏根目录。
- 如果音频输出问题仍然存在，请在 [Issues](https://github.com/thevvlab/Aw2AudioFix/issues) 中报告 Windows 版本、游戏版本、手柄型号和连接方式。

## 致谢

- [MinHook](https://github.com/TsudaKageyu/minhook) — API Hook库。

## 免责声明

本模组是非官方社区模组，与 Remedy Entertainment 或 Sony Interactive Entertainment 没有任何关联，也未获得其认可或授权。

使用风险由用户自行承担。

