#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include "MinHook.h"

#pragma comment(lib, "hid.lib")

// ============================================================
// 配置 / Configuration
// FALSE：关闭调试日志；TRUE：输出调试日志。
// FALSE: Disable debug logging; TRUE: Enable debug logging.
static BOOL g_EnableDebugLog = FALSE;

static HMODULE g_hOriginalDll = nullptr;
static HMODULE g_hHidDll = nullptr;

// ============================================================
// 原始 API 函数指针 / Original API function pointers
// ============================================================
using PFN_HidD_SetFeature = BOOLEAN(WINAPI*)(HANDLE, PVOID, ULONG);
using PFN_HidD_SetOutputReport = BOOLEAN(WINAPI*)(HANDLE, PVOID, ULONG);
using PFN_WriteFile = BOOL(WINAPI*)(
    HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

static PFN_HidD_SetFeature TrueHidD_SetFeature = nullptr;
static PFN_HidD_SetOutputReport TrueHidD_SetOutputReport = nullptr;
static PFN_WriteFile TrueWriteFile = nullptr;

// ============================================================
// 日志 / Logging
// 只有启用开关时才格式化并输出日志。
// Format and emit logs only when the BOOL switch is enabled.
// ============================================================
static void DebugLog(const char* format, ...)
{
    if (!g_EnableDebugLog || !format)
        return;

    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);

    OutputDebugStringA("[AW2Fix] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

// ============================================================
// HID 设备识别 / HID device identification
// ============================================================
static bool GetHidDeviceInfo(HANDLE device, USHORT& vid, USHORT& pid)
{
    if (!device || device == INVALID_HANDLE_VALUE)
        return false;

    HIDD_ATTRIBUTES attr = {};
    attr.Size = sizeof(attr);

    if (!HidD_GetAttributes(device, &attr))
        return false;

    vid = attr.VendorID;
    pid = attr.ProductID;
    return true;
}

static bool IsDs4Device(HANDLE device)
{
    USHORT vid = 0, pid = 0;
    if (!GetHidDeviceInfo(device, vid, pid))
        return false;

    // Sony DS4 V1 / V2 USB IDs.
    return vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC);
}

// ============================================================
// HID 报告日志 / HID report logging
// 最多打印 64 字节，避免日志过大。
// Log at most 64 bytes to keep output bounded.
// ============================================================
static void LogHidReport(const char* api, HANDLE device,
    const BYTE* report, ULONG length)
{
    if (!g_EnableDebugLog)
        return;

    USHORT vid = 0, pid = 0;
    const bool hasInfo = GetHidDeviceInfo(device, vid, pid);

    char buffer[512] = {};
    int offset = _snprintf_s(
        buffer, sizeof(buffer), _TRUNCATE,
        "%s VID=%04X PID=%04X HANDLE=%p LEN=%lu DATA=",
        api ? api : "HID", hasInfo ? vid : 0, hasInfo ? pid : 0,
        device, static_cast<unsigned long>(length));

    if (offset < 0)
        return;

    if (!report || length == 0)
    {
        _snprintf_s(buffer + offset, sizeof(buffer) - offset,
            _TRUNCATE, "<empty>");
    }
    else
    {
        const ULONG count = (length < 64) ? length : 64;
        for (ULONG i = 0; i < count && offset < static_cast<int>(sizeof(buffer)) - 4; ++i)
        {
            int written = _snprintf_s(
                buffer + offset, sizeof(buffer) - offset,
                _TRUNCATE, "%02X ", report[i]);
            if (written < 0)
                break;
            offset += written;
        }

        if (length > count && offset < static_cast<int>(sizeof(buffer)) - 4)
            _snprintf_s(buffer + offset, sizeof(buffer) - offset,
                _TRUNCATE, "...");
    }

    OutputDebugStringA("[AW2Fix] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

// ============================================================
// 报告过滤 / Report filtering
// 不修改报告内容；匹配后阻止该报告发送。
// Do not alter report bytes; block matching reports.
// ============================================================
static bool ShouldBlockDs4SpeakerRoute(HANDLE device,
    const BYTE* report, ULONG length)
{
    if (!report || length < 3 || !IsDs4Device(device))
        return false;

    return report[0] == 0xE0 &&
        report[1] == 0x00 &&
        report[2] == 0x03;
}

static bool ShouldBlockDs4SuspiciousReport(HANDLE device,
    const BYTE* report, ULONG length)
{
    if (!report || length != 32 || !IsDs4Device(device))
        return false;

    // 日志确认的可疑 32 字节报告前缀。
    // Suspected 32-byte report prefix observed in logs.
    return report[0] == 0x05 && report[1] == 0xF0;
}

// ============================================================
// HidD_SetFeature Hook
// ============================================================
static BOOLEAN WINAPI Hooked_HidD_SetFeature(
    HANDLE device, PVOID report, ULONG length)
{
    if (!TrueHidD_SetFeature)
    {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }

    const BYTE* bytes = static_cast<const BYTE*>(report);
    LogHidReport("HidD_SetFeature", device, bytes, length);

    if (ShouldBlockDs4SpeakerRoute(device, bytes, length))
    {
        DebugLog("Blocked DS4 speaker route via HidD_SetFeature");
        return TRUE; // 模拟成功，避免应用重试。
    }

    return TrueHidD_SetFeature(device, report, length);
}

// ============================================================
// HidD_SetOutputReport Hook
// ============================================================
static BOOLEAN WINAPI Hooked_HidD_SetOutputReport(
    HANDLE device, PVOID report, ULONG length)
{
    if (!TrueHidD_SetOutputReport)
    {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }

    const BYTE* bytes = static_cast<const BYTE*>(report);
    LogHidReport("HidD_SetOutputReport", device, bytes, length);

    if (ShouldBlockDs4SpeakerRoute(device, bytes, length))
    {
        DebugLog("Blocked DS4 speaker route via HidD_SetOutputReport");
        return TRUE;
    }

    return TrueHidD_SetOutputReport(device, report, length);
}

// ============================================================
// WriteFile Hook
// 只处理识别出的 DS4 HID 句柄；其他文件句柄原样转发。
// Only inspect recognized DS4 HID handles; pass other handles through.
// ============================================================
static BOOL WINAPI Hooked_WriteFile(
    HANDLE file, LPCVOID buffer, DWORD bytesToWrite,
    LPDWORD bytesWritten, LPOVERLAPPED overlapped)
{
    if (!TrueWriteFile)
    {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }

    USHORT vid = 0, pid = 0;
    if (!GetHidDeviceInfo(file, vid, pid))
        return TrueWriteFile(file, buffer, bytesToWrite, bytesWritten, overlapped);

    // 空缓冲区或零长度请求交由系统 API 处理。
    // Let the original API handle null/zero-length requests.
    if (!buffer || bytesToWrite == 0)
        return TrueWriteFile(file, buffer, bytesToWrite, bytesWritten, overlapped);

    const BYTE* report = static_cast<const BYTE*>(buffer);
    LogHidReport("WriteFile", file, report, bytesToWrite);

    if (ShouldBlockDs4SuspiciousReport(
        file, report, bytesToWrite))
    {
        DebugLog(
            "[AW2Fix] Blocked suspected DS4 report 05 F0\n");

        // Preserve the previous behavior that fixed the issue.
        // 保留之前已经验证有效的处理方式。
        if (bytesWritten)
            *bytesWritten = bytesToWrite;

        return TRUE;
    }

    return TrueWriteFile(file, buffer, bytesToWrite, bytesWritten, overlapped);
}

// ============================================================
// Hook 初始化 / Hook initialization
// ============================================================
static bool CreateApiHook(HMODULE module, const char* name,
    LPVOID detour, LPVOID* original)
{
    if (!module || !name || !detour || !original)
        return false;

    FARPROC target = GetProcAddress(module, name);
    if (!target)
    {
        DebugLog("GetProcAddress failed: %s (error=%lu)",
            name, GetLastError());
        return false;
    }

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<LPVOID>(target), detour, original);

    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
    {
        DebugLog("MH_CreateHook failed: %s (status=%d)", name, status);
        return false;
    }

    DebugLog("Hook created: %s", name);
    return true;
}

static void InitHooks()
{
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        DebugLog("MinHook initialization failed (status=%d)", status);
        return;
    }

    g_hHidDll = GetModuleHandleW(L"hid.dll");
    if (!g_hHidDll)
        g_hHidDll = LoadLibraryW(L"hid.dll");

    if (!g_hHidDll)
    {
        DebugLog("Failed to load hid.dll (error=%lu)", GetLastError());
        return;
    }

    CreateApiHook(g_hHidDll, "HidD_SetFeature",
        reinterpret_cast<LPVOID>(&Hooked_HidD_SetFeature),
        reinterpret_cast<LPVOID*>(&TrueHidD_SetFeature));

    CreateApiHook(g_hHidDll, "HidD_SetOutputReport",
        reinterpret_cast<LPVOID>(&Hooked_HidD_SetOutputReport),
        reinterpret_cast<LPVOID*>(&TrueHidD_SetOutputReport));

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    CreateApiHook(kernel32, "WriteFile",
        reinterpret_cast<LPVOID>(&Hooked_WriteFile),
        reinterpret_cast<LPVOID*>(&TrueWriteFile));

    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK)
        DebugLog("MH_EnableHook failed (status=%d)", status);
    else
        DebugLog("HID hooks enabled");
}

// ============================================================
// 初始化线程 / Initialization thread
// ============================================================
static DWORD WINAPI InitThread(LPVOID)
{
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        DebugLog("GetModuleFileNameW failed (error=%lu)", GetLastError());
        return 0;
    }

    wchar_t* slash = wcsrchr(path, L'\\\\');
    if (!slash)
    {
        DebugLog("Executable path has no directory separator");
        return 0;
    }
    *(slash + 1) = L'\\0';

    if (wcscat_s(path, MAX_PATH, L"xaudio2_9_original.dll") != 0)
    {
        DebugLog("Original DLL path is too long");
        return 0;
    }

    g_hOriginalDll = LoadLibraryW(path);
    if (!g_hOriginalDll)
        DebugLog("Failed to load xaudio2_9_original.dll (error=%lu)", GetLastError());
    else
        DebugLog("Original XAudio DLL loaded");

    InitHooks();
    return 0;
}

// ============================================================
// 卸载 / Uninitialization
// 注意：不要在 DllMain 的 PROCESS_DETACH 中执行复杂的 MinHook 清理，
// 因为此时可能持有 Loader Lock。应由宿主提供显式卸载入口。
// Avoid complex MinHook cleanup inside DllMain under Loader Lock.
// ============================================================
extern "C" __declspec(dllexport) void WINAPI Uninit()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_hOriginalDll)
    {
        FreeLibrary(g_hOriginalDll);
        g_hOriginalDll = nullptr;
    }

    g_hHidDll = nullptr; // hid.dll 是系统模块，不在这里释放。
}

// ============================================================
// DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
            DebugLog("CreateThread failed (error=%lu)", GetLastError());
    }

    // 不在 PROCESS_DETACH 中调用 Uninit，避免 Loader Lock 风险。
    return TRUE;
}
