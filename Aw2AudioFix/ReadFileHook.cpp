#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winternl.h>
#include <hidsdi.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <cstddef>

#include "MinHook.h"

#ifndef _countof
#define _countof(_Array) (sizeof(_Array) / sizeof((_Array)[0]))
#endif

// ============================================================
// 新版输入监控：
// 1. GetRawInputData
// 2. GetRawInputDeviceList / GetRawInputDeviceInfoW
// 3. HidD_GetInputReport
// 4. NtCreateFile / NtOpenFile：建立 HID HANDLE -> 设备路径映射
// 5. NtDeviceIoControlFile：只解析 DS4 输入报告 IOCTL
// 6. NtReadFile：捕获 DS4 连续输入报告（同步完成 + Event 等待完成）
// 7. NtWaitForSingleObject / NtWaitForMultipleObjects：读取已完成的异步报告
// 8. NtClose：清理 HANDLE 映射和未完成读取记录
//
// 不包含 Win32 ReadFile Hook。原有 HID 输出逻辑不在本文件中修改。
// 异步 IOCP/APC 完成路径不由 NtWaitForSingleObject 覆盖，详见说明。
// ============================================================

using PFN_GetRawInputData = UINT(WINAPI*)(
    HRAWINPUT, UINT, LPVOID, PUINT, UINT);

using PFN_GetRawInputDeviceList = UINT(WINAPI*)(
    PRAWINPUTDEVICELIST, PUINT, UINT);

using PFN_GetRawInputDeviceInfoW = UINT(WINAPI*)(
    HANDLE, UINT, LPVOID, PUINT);

using PFN_HidD_GetInputReport = BOOLEAN(WINAPI*)(
    HANDLE, PVOID, ULONG);

using AW2_IO_APC_ROUTINE = VOID(NTAPI*)(PVOID, PIO_STATUS_BLOCK, ULONG);

using PFN_NtDeviceIoControlFile = NTSTATUS(NTAPI*)(
    HANDLE, HANDLE, AW2_IO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
    ULONG, PVOID, ULONG, PVOID, ULONG);

using PFN_NtCreateFile = NTSTATUS(NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

using PFN_NtOpenFile = NTSTATUS(NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    ULONG, ULONG);

using PFN_NtReadFile = NTSTATUS(NTAPI*)(
    HANDLE, HANDLE, AW2_IO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
    PVOID, ULONG, PLARGE_INTEGER, PULONG);

using PFN_NtWaitForSingleObject = NTSTATUS(NTAPI*)(
    HANDLE, BOOLEAN, PLARGE_INTEGER);

using PFN_NtWaitForMultipleObjects = NTSTATUS(NTAPI*)(
    ULONG, PHANDLE, ULONG, BOOLEAN, PLARGE_INTEGER);

using PFN_NtClose = NTSTATUS(NTAPI*)(HANDLE);

static PFN_GetRawInputData TrueGetRawInputData = nullptr;
static PFN_GetRawInputDeviceList TrueGetRawInputDeviceList = nullptr;
static PFN_GetRawInputDeviceInfoW TrueGetRawInputDeviceInfoW = nullptr;
static PFN_HidD_GetInputReport TrueHidD_GetInputReport = nullptr;
static PFN_NtDeviceIoControlFile TrueNtDeviceIoControlFile = nullptr;
static PFN_NtCreateFile TrueNtCreateFile = nullptr;
static PFN_NtOpenFile TrueNtOpenFile = nullptr;
static PFN_NtReadFile TrueNtReadFile = nullptr;
static PFN_NtWaitForSingleObject TrueNtWaitForSingleObject = nullptr;
static PFN_NtWaitForMultipleObjects TrueNtWaitForMultipleObjects = nullptr;
static PFN_NtClose TrueNtClose = nullptr;

// Hook 索引
enum HookIndex
{
    HOOK_RAW_DATA = 0,
    HOOK_RAW_DEVICE_LIST,
    HOOK_RAW_DEVICE_INFO,
    HOOK_HID_GET_INPUT_REPORT,
    HOOK_NT_DEVICE_IO_CONTROL,
    HOOK_NT_CREATE_FILE,
    HOOK_NT_OPEN_FILE,
    HOOK_NT_READ_FILE,
    HOOK_NT_WAIT_SINGLE,
    HOOK_NT_WAIT_MULTIPLE,
    HOOK_NT_CLOSE,
    HOOK_COUNT
};

static LPVOID g_HookTargets[HOOK_COUNT] = {};
static bool g_InputHooksReady = false;

// ============================================================
// 日志
// ============================================================

static void ReadLog(const char* format, ...)
{
    char buffer[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);

    OutputDebugStringA("[AW2-INPUT] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

static void WideToUtf8(
    const wchar_t* source,
    char* destination,
    size_t destinationSize)
{
    if (!destination || destinationSize == 0)
        return;

    destination[0] = '\0';
    if (!source)
        return;

    int n = WideCharToMultiByte(
        CP_UTF8, 0, source, -1,
        destination, static_cast<int>(destinationSize),
        nullptr, nullptr);

    if (n <= 0)
        strcpy_s(destination, destinationSize, "?");
}

static bool IsDs4Path(const wchar_t* path)
{
    if (!path)
        return false;

    wchar_t upper[1024] = {};
    wcsncpy_s(upper, _countof(upper), path, _TRUNCATE);
    _wcsupr_s(upper);

    return wcsstr(upper, L"VID_054C") != nullptr &&
        (wcsstr(upper, L"PID_05C4") != nullptr ||
            wcsstr(upper, L"PID_09CC") != nullptr);
}

static bool IsHidPath(const wchar_t* path)
{
    if (!path)
        return false;

    wchar_t upper[1024] = {};
    wcsncpy_s(upper, _countof(upper), path, _TRUNCATE);
    _wcsupr_s(upper);

    return wcsstr(upper, L"HID#") != nullptr ||
        wcsstr(upper, L"VID_") != nullptr;
}

static void DumpBytes(
    const char* label,
    const BYTE* data,
    ULONG length,
    ULONG maximum = 64)
{
    if (!data || length == 0)
        return;

    ULONG count = (length < maximum) ? length : maximum;
    char line[1200] = {};

    int offset = _snprintf_s(
        line, sizeof(line), _TRUNCATE,
        "%s LEN=%lu DATA=", label,
        static_cast<unsigned long>(length));

    if (offset < 0)
        return;

    for (ULONG i = 0; i < count; ++i)
    {
        int written = _snprintf_s(
            line + offset, sizeof(line) - offset, _TRUNCATE,
            "%02X ", data[i]);

        if (written < 0)
            break;

        offset += written;
    }

    if (length > count && offset > 0)
    {
        _snprintf_s(
            line + offset, sizeof(line) - offset,
            _TRUNCATE, "...");
    }

    ReadLog("%s", line);
}

// ============================================================
// Hook 安装
// ============================================================

static bool InstallHook(
    LPVOID target,
    LPVOID detour,
    LPVOID* original,
    int index,
    const char* name)
{
    if (!target || !detour || !original ||
        index < 0 || index >= HOOK_COUNT)
    {
        ReadLog("%s invalid parameter", name);
        return false;
    }

    if (g_HookTargets[index])
        return true;

    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK)
    {
        ReadLog("CreateHook %s failed: %d", name, status);
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        ReadLog("EnableHook %s failed: %d", name, status);
        MH_RemoveHook(target);
        *original = nullptr;
        return false;
    }

    g_HookTargets[index] = target;
    ReadLog("Hook enabled: %s TARGET=%p", name, target);
    return true;
}

// ============================================================
// HANDLE -> HID 路径映射
// 只记录 HID/VID 路径，避免把普通文件句柄塞入表。
// ============================================================

struct HandlePathEntry
{
    HANDLE handle;
    bool isDs4;
    wchar_t path[1024];
};

static HandlePathEntry g_HandlePaths[256] = {};
static SRWLOCK g_HandlePathLock = SRWLOCK_INIT;

// Forward declaration: async NtReadFile completion path feeds the same decoder.
static void LogDs4InputReport(const char* source, const BYTE* report, ULONG length);

static void RememberHandle(HANDLE handle, const wchar_t* path)
{
    if (!handle || handle == INVALID_HANDLE_VALUE || !IsHidPath(path))
        return;

    AcquireSRWLockExclusive(&g_HandlePathLock);

    int slot = -1;
    for (int i = 0; i < _countof(g_HandlePaths); ++i)
    {
        if (g_HandlePaths[i].handle == handle)
        {
            slot = i;
            break;
        }
        if (slot < 0 && !g_HandlePaths[i].handle)
            slot = i;
    }

    if (slot >= 0)
    {
        g_HandlePaths[slot].handle = handle;
        g_HandlePaths[slot].isDs4 = IsDs4Path(path);
        wcsncpy_s(
            g_HandlePaths[slot].path,
            _countof(g_HandlePaths[slot].path),
            path,
            _TRUNCATE);
    }

    ReleaseSRWLockExclusive(&g_HandlePathLock);

}

static void ForgetHandle(HANDLE handle)
{
    if (!handle)
        return;

    AcquireSRWLockExclusive(&g_HandlePathLock);

    for (int i = 0; i < _countof(g_HandlePaths); ++i)
    {
        if (g_HandlePaths[i].handle == handle)
        {
            g_HandlePaths[i].handle = nullptr;
            g_HandlePaths[i].path[0] = L'\0';
            break;
        }
    }

    ReleaseSRWLockExclusive(&g_HandlePathLock);
}

static bool GetHandlePath(HANDLE handle, wchar_t* path, size_t pathCount)
{
    if (!path || pathCount == 0)
        return false;

    path[0] = L'\0';
    bool found = false;

    AcquireSRWLockShared(&g_HandlePathLock);

    for (int i = 0; i < _countof(g_HandlePaths); ++i)
    {
        if (g_HandlePaths[i].handle == handle)
        {
            wcsncpy_s(path, pathCount, g_HandlePaths[i].path, _TRUNCATE);
            found = true;
            break;
        }
    }

    ReleaseSRWLockShared(&g_HandlePathLock);
    return found;
}

static bool IsDs4Handle(HANDLE handle)
{
    bool result = false;
    AcquireSRWLockShared(&g_HandlePathLock);
    for (int i = 0; i < _countof(g_HandlePaths); ++i)
    {
        if (g_HandlePaths[i].handle == handle)
        {
            result = g_HandlePaths[i].isDs4;
            break;
        }
    }
    ReleaseSRWLockShared(&g_HandlePathLock);
    return result;
}

static bool MarkDs4Handle(HANDLE handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return false;

    bool newlyIdentified = false;
    AcquireSRWLockExclusive(&g_HandlePathLock);
    int slot = -1;
    for (int i = 0; i < _countof(g_HandlePaths); ++i)
    {
        if (g_HandlePaths[i].handle == handle)
        {
            slot = i;
            break;
        }
        if (slot < 0 && !g_HandlePaths[i].handle)
            slot = i;
    }
    if (slot >= 0)
    {
        newlyIdentified = !g_HandlePaths[slot].isDs4;
        g_HandlePaths[slot].handle = handle;
        g_HandlePaths[slot].isDs4 = true;
    }
    ReleaseSRWLockExclusive(&g_HandlePathLock);
    return newlyIdentified;
}

// NtReadFile may complete asynchronously. Track only DS4 reads and only
// inspect the buffer after the associated event/file object has signaled.
struct PendingDs4Read
{
    bool used;
    HANDLE fileHandle;
    HANDLE eventHandle;
    PIO_STATUS_BLOCK ioStatusBlock;
    BYTE* buffer;
    ULONG length;
    NTSTATUS completedStatus;
    ULONG_PTR completedBytes;
};

static PendingDs4Read g_PendingDs4Reads[128] = {};
static SRWLOCK g_PendingReadLock = SRWLOCK_INIT;

static void TrackPendingDs4Read(
    HANDLE fileHandle, HANDLE eventHandle, PIO_STATUS_BLOCK iosb,
    PVOID buffer, ULONG length)
{
    if (!fileHandle || !iosb || !buffer || length == 0)
        return;

    AcquireSRWLockExclusive(&g_PendingReadLock);
    int slot = -1;
    for (int i = 0; i < _countof(g_PendingDs4Reads); ++i)
    {
        if (!g_PendingDs4Reads[i].used)
        {
            slot = i;
            break;
        }
    }

    if (slot >= 0)
    {
        g_PendingDs4Reads[slot].used = true;
        g_PendingDs4Reads[slot].fileHandle = fileHandle;
        g_PendingDs4Reads[slot].eventHandle = eventHandle;
        g_PendingDs4Reads[slot].ioStatusBlock = iosb;
        g_PendingDs4Reads[slot].buffer = static_cast<BYTE*>(buffer);
        g_PendingDs4Reads[slot].length = length;
    }
    else
    {
        ReadLog("DS4 pending-read table full; async read not tracked");
    }
    ReleaseSRWLockExclusive(&g_PendingReadLock);
}

static void CompletePendingDs4ReadsForWait(HANDLE waitedHandle)
{
    if (!waitedHandle)
        return;

    PendingDs4Read completed[128] = {};
    int completedCount = 0;

    AcquireSRWLockExclusive(&g_PendingReadLock);
    for (int i = 0; i < _countof(g_PendingDs4Reads); ++i)
    {
        PendingDs4Read& item = g_PendingDs4Reads[i];
        if (!item.used ||
            (item.eventHandle != waitedHandle && item.fileHandle != waitedHandle))
            continue;

        if (!item.ioStatusBlock ||
            item.ioStatusBlock->Status == static_cast<NTSTATUS>(0x00000103L))
            continue;

        completed[completedCount] = item;
        completed[completedCount].completedStatus = item.ioStatusBlock->Status;
        completed[completedCount].completedBytes = item.ioStatusBlock->Information;
        ++completedCount;
        ZeroMemory(&item, sizeof(item));
    }
    ReleaseSRWLockExclusive(&g_PendingReadLock);

    for (int i = 0; i < completedCount; ++i)
    {
        const PendingDs4Read& item = completed[i];
        NTSTATUS ioStatus = item.completedStatus;
        ULONG_PTR bytes = item.completedBytes;
        if (ioStatus >= 0 && bytes > 0 && item.buffer)
        {
            ULONG safeLength = item.length;
            if (bytes < safeLength)
                safeLength = static_cast<ULONG>(bytes);
            LogDs4InputReport("NtReadFile", item.buffer, safeLength);
        }
    }
}

static void ForgetPendingReadsForHandle(HANDLE handle)
{
    AcquireSRWLockExclusive(&g_PendingReadLock);
    for (int i = 0; i < _countof(g_PendingDs4Reads); ++i)
    {
        PendingDs4Read& item = g_PendingDs4Reads[i];
        if (item.used && (item.fileHandle == handle || item.eventHandle == handle))
            ZeroMemory(&item, sizeof(item));
    }
    ReleaseSRWLockExclusive(&g_PendingReadLock);
}

static void GetObjectName(
    POBJECT_ATTRIBUTES attributes,
    wchar_t* path,
    size_t pathCount)
{
    if (!path || pathCount == 0)
        return;

    path[0] = L'\0';

    if (!attributes || !attributes->ObjectName ||
        !attributes->ObjectName->Buffer)
        return;

    size_t chars = attributes->ObjectName->Length / sizeof(WCHAR);
    if (chars >= pathCount)
        chars = pathCount - 1;

    if (chars)
    {
        wmemcpy(path, attributes->ObjectName->Buffer, chars);
        path[chars] = L'\0';
    }
}

// ============================================================
// Raw Input 设备路径查询
// 必须调用 TrueGetRawInputDeviceInfoW，避免再次进入 Hook。
// ============================================================

static bool QueryRawDevicePath(
    HANDLE device,
    wchar_t* path,
    UINT pathCount)
{
    if (!TrueGetRawInputDeviceInfoW || !path || pathCount == 0)
        return false;

    path[0] = L'\0';
    UINT size = pathCount;

    UINT result = TrueGetRawInputDeviceInfoW(
        device, RIDI_DEVICENAME, path, &size);

    if (result == static_cast<UINT>(-1))
        return false;

    path[pathCount - 1] = L'\0';
    return path[0] != L'\0';
}

// ============================================================
// Raw Input HID 报告解析
// ============================================================

// ============================================================
// DS4 USB input report decoder
// USB report 0x01 / 64 bytes. Only emit button transitions and
// touch contacts/movement; do not dump axes, IMU or descriptor data.
// ============================================================

struct Ds4TouchState
{
    bool active;
    BYTE id;
    USHORT x;
    USHORT y;
};

static SRWLOCK g_Ds4StateLock = SRWLOCK_INIT;
static bool g_Ds4StateInitialized = false;
static volatile LONG g_Ds4NtReadSeen = 0;
static BYTE g_LastButtons[3] = { 0xFF, 0xFF, 0xFF };
static Ds4TouchState g_LastTouches[2] = {};

static const char* DpadName(BYTE value)
{
    static const char* names[9] = {
        "UP", "UP_RIGHT", "RIGHT", "DOWN_RIGHT",
        "DOWN", "DOWN_LEFT", "LEFT", "UP_LEFT", "NEUTRAL"
    };
    return value <= 8 ? names[value] : "INVALID";
}

static void LogDs4InputReport(const char* source, const BYTE* r, ULONG length)
{
    if (!r || length < 64 || r[0] != 0x01)
        return; // 当前只解析 DS4 USB 标准输入报告

    // DS4 USB report 0x01 layout:
    // byte 0 report ID; bytes 1..4 sticks; bytes 5..7 buttons;
    // bytes 8..9 analog triggers; byte 33 touch report count.
    BYTE buttons[3] = { r[5], r[6], r[7] };
    BYTE touchCount = r[33];

    Ds4TouchState touches[2] = {};
    bool hasTouchSample = touchCount > 0 && touchCount <= 3;

    if (hasTouchSample)
    {
        // 一帧可能包含多个触摸采样，取最后一组作为当前触摸状态。
        ULONG base = 35 + static_cast<ULONG>(touchCount - 1) * 9;
        if (base + 7 >= length)
        {
            hasTouchSample = false;
        }
        else
        {
            for (int finger = 0; finger < 2; ++finger)
            {
                ULONG off = base + static_cast<ULONG>(finger) * 4;
                BYTE idAndState = r[off];
                touches[finger].active = (idAndState & 0x80) == 0;
                touches[finger].id = idAndState & 0x7F;
                touches[finger].x = static_cast<USHORT>(r[off + 1] | ((r[off + 2] & 0x0F) << 8));
                touches[finger].y = static_cast<USHORT>((r[off + 2] >> 4) | (r[off + 3] << 4));
            }
        }
    }

    AcquireSRWLockExclusive(&g_Ds4StateLock);
    bool first = !g_Ds4StateInitialized;
    if (first)
    {
        g_Ds4StateInitialized = true;
        g_LastButtons[0] = buttons[0];
        g_LastButtons[1] = buttons[1];
        g_LastButtons[2] = buttons[2];
        g_LastTouches[0] = touches[0];
        g_LastTouches[1] = touches[1];
        ReleaseSRWLockExclusive(&g_Ds4StateLock);
        ReadLog("DS4 %s BASELINE buttons=%02X %02X %02X touch=%d,%d",
            source, buttons[0], buttons[1], buttons[2],
            touches[0].active ? 1 : 0, touches[1].active ? 1 : 0);
        return;
    }

    BYTE changed[3] = {
        static_cast<BYTE>(buttons[0] ^ g_LastButtons[0]),
        static_cast<BYTE>(buttons[1] ^ g_LastButtons[1]),
        static_cast<BYTE>(buttons[2] ^ g_LastButtons[2])
    };

    bool anyButton = changed[0] || changed[1] || changed[2];
    if (anyButton)
    {
        // Byte 5: d-pad low nibble, Square/Cross/Circle/Triangle high nibble.
        if ((changed[0] & 0x0F) != 0 && (buttons[0] & 0x0F) <= 8)
            ReadLog("DS4 BUTTON %s DPAD=%s", source, DpadName(buttons[0] & 0x0F));

        static const char* faceNames[4] = { "SQUARE", "CROSS", "CIRCLE", "TRIANGLE" };
        for (int bit = 0; bit < 4; ++bit)
            if (changed[0] & (1 << (bit + 4)))
                ReadLog("DS4 BUTTON %s %s %s", source, faceNames[bit],
                    (buttons[0] & (1 << (bit + 4))) ? "DOWN" : "UP");

        static const char* byte1Names[8] = { "L1", "R1", "L2_DIGITAL", "R2_DIGITAL", "SHARE", "OPTIONS", "L3", "R3" };
        for (int bit = 0; bit < 8; ++bit)
            if (changed[1] & (1 << bit))
                ReadLog("DS4 BUTTON %s %s %s", source, byte1Names[bit],
                    (buttons[1] & (1 << bit)) ? "DOWN" : "UP");

        if (changed[2] & 0x01)
            ReadLog("DS4 BUTTON %s PS %s", source, (buttons[2] & 0x01) ? "DOWN" : "UP");

        // ==========================================
        // 2. 左右触控板独立映射逻辑
        // ==========================================
        if (changed[2] & 0x02)
        {
            bool touchpadClickedNow = (buttons[2] & 0x02) != 0;
            ReadLog("DS4 BUTTON %s TOUCHPAD_CLICK %s", source, touchpadClickedNow ? "DOWN" : "UP");

            // 静态状态变量：记录当前注入的键位及防止连续触发
            static bool s_InjectedClicked = false;
            static WORD s_InjectedKey = 0;

            if (touchpadClickedNow && !s_InjectedClicked)
            {
                // 获取当前手指的 X 坐标（默认取 finger 1，其次 finger 2）
                USHORT xCoord = 960; // 默认居中
                if (touches[0].active)
                    xCoord = touches[0].x;
                else if (touches[1].active)
                    xCoord = touches[1].x;

                // DS4 触控板 X 坐标范围约为 0 ~ 1920，中线在 960 左右
                if (xCoord < 960)
                {
                    // 左侧触控板 -> 映射为 M 键 (打开地图)
                    s_InjectedKey = 'M';
                    keybd_event('M', MapVirtualKeyA('M', MAPVK_VK_TO_VSC), 0, 0);

                    BYTE* mutableReport = const_cast<BYTE*>(r);
                    mutableReport[7] &= ~0x02; // 将 r[7] 中的 TOUCHPAD_CLICK 位强制清零

                    ReadLog("[AW2Fix] Left Touchpad Clicked (X=%u) -> Injecting 'M' Down", xCoord);
                }
                else
                {
                    // 右侧触控板 -> 不丢弃直接 打开房间/案件板
                    s_InjectedKey = VK_TAB;
                    // keybd_event(VK_TAB, MapVirtualKeyA(VK_TAB, MAPVK_VK_TO_VSC), 0, 0);

                    ReadLog("[AW2Fix] Right Touchpad Clicked (X=%u) -> Injecting 'TAB' Down", xCoord);
                }
                s_InjectedClicked = true;
            }
            else if (!touchpadClickedNow && s_InjectedClicked)
            {
                // 松开时释放对应的键盘按键
                if (s_InjectedKey != 0 && s_InjectedKey != VK_TAB)
                {
                    keybd_event(
                        static_cast<BYTE>(s_InjectedKey),
                        MapVirtualKeyA(static_cast<BYTE>(s_InjectedKey), MAPVK_VK_TO_VSC),
                        KEYEVENTF_KEYUP,
                        0
                    );
                    ReadLog("[AW2Fix] Touchpad Released -> Injecting Key Up");
                    s_InjectedKey = 0;
                }
                s_InjectedClicked = false;
            }
        }
    }

    if (hasTouchSample)
        for (int finger = 0; finger < 2; ++finger)
        {
            const Ds4TouchState& now = touches[finger];
            const Ds4TouchState& old = g_LastTouches[finger];
            if (now.active != old.active || (now.active && (!old.active || now.id != old.id)))
            {
                ReadLog("DS4 TOUCH %s FINGER%d %s ID=%u X=%u Y=%u",
                    source, finger + 1, now.active ? "DOWN" : "UP",
                    now.id, now.x, now.y);
            }
            else if (now.active && old.active)
            {
                int dx = static_cast<int>(now.x) - static_cast<int>(old.x);
                int dy = static_cast<int>(now.y) - static_cast<int>(old.y);
                if (dx >= 8 || dx <= -8 || dy >= 8 || dy <= -8)
                    ReadLog("DS4 TOUCH %s FINGER%d MOVE ID=%u X=%u Y=%u",
                        source, finger + 1, now.id, now.x, now.y);
            }
        }

    g_LastButtons[0] = buttons[0];
    g_LastButtons[1] = buttons[1];
    g_LastButtons[2] = buttons[2];
    if (hasTouchSample)
    {
        g_LastTouches[0] = touches[0];
        g_LastTouches[1] = touches[1];
    }
    ReleaseSRWLockExclusive(&g_Ds4StateLock);
}

static void LogRawHid(
    const char* api,
    HANDLE device,
    const RAWHID* hid,
    DWORD availableSize)
{
    if (!hid || hid->dwSizeHid == 0 || hid->dwCount == 0)
        return;

    const size_t payloadOffset =
        offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
    if (availableSize < payloadOffset)
        return;

    size_t remaining = availableSize - payloadOffset;
    size_t reportSize = hid->dwSizeHid;
    if (hid->dwCount > remaining / reportSize)
        return;

    wchar_t path[1024] = {};
    if (!QueryRawDevicePath(device, path, _countof(path)) || !IsDs4Path(path))
        return;

    for (DWORD i = 0; i < hid->dwCount; ++i)
    {
        const BYTE* report = hid->bRawData + static_cast<size_t>(i) * reportSize;
        LogDs4InputReport(api, report, hid->dwSizeHid);
    }
}

static void InspectRawInput(
    const char* api,
    const RAWINPUT* raw,
    DWORD availableSize)
{
    if (!raw || availableSize < sizeof(RAWINPUTHEADER))
        return;

    DWORD blockSize = raw->header.dwSize;
    if (blockSize < sizeof(RAWINPUTHEADER) || blockSize > availableSize)
        return;

    if (raw->header.dwType == RIM_TYPEHID)
        LogRawHid(api, raw->header.hDevice, &raw->data.hid, blockSize);
}

// ============================================================
// Hook: GetRawInputData
// ============================================================

static UINT WINAPI Hooked_GetRawInputData(
    HRAWINPUT rawInput,
    UINT command,
    LPVOID data,
    PUINT size,
    UINT headerSize)
{
    if (!TrueGetRawInputData)
        return static_cast<UINT>(-1);

    UINT result = TrueGetRawInputData(
        rawInput, command, data, size, headerSize);

    if (result == static_cast<UINT>(-1))
        return result;

    if (command == RID_INPUT && data &&
        result >= sizeof(RAWINPUTHEADER))
    {
        InspectRawInput(
            "GetRawInputData",
            static_cast<const RAWINPUT*>(data),
            result);
    }

    return result;
}

// ============================================================
// Hook: GetRawInputDeviceList
// ============================================================

static UINT WINAPI Hooked_GetRawInputDeviceList(
    PRAWINPUTDEVICELIST list,
    PUINT count,
    UINT entrySize)
{
    if (!TrueGetRawInputDeviceList)
        return static_cast<UINT>(-1);
    return TrueGetRawInputDeviceList(list, count, entrySize);
}

// ============================================================
// Hook: GetRawInputDeviceInfoW
// ============================================================

static UINT WINAPI Hooked_GetRawInputDeviceInfoW(
    HANDLE device,
    UINT command,
    LPVOID data,
    PUINT size)
{
    if (!TrueGetRawInputDeviceInfoW)
        return static_cast<UINT>(-1);
    return TrueGetRawInputDeviceInfoW(device, command, data, size);
}

// ============================================================
// Hook: HidD_GetInputReport
// ============================================================

static BOOLEAN WINAPI Hooked_HidD_GetInputReport(
    HANDLE device,
    PVOID reportBuffer,
    ULONG reportBufferLength)
{
    if (!TrueHidD_GetInputReport)
        return FALSE;
    BOOLEAN result = TrueHidD_GetInputReport(device, reportBuffer, reportBufferLength);
    if (result && reportBuffer && IsDs4Handle(device))
        LogDs4InputReport("HidD_GetInputReport", static_cast<const BYTE*>(reportBuffer), reportBufferLength);
    return result;
}

// ============================================================
// Hook: NtCreateFile / NtOpenFile
// ============================================================

static NTSTATUS NTAPI Hooked_NtCreateFile(
    PHANDLE fileHandle,
    ACCESS_MASK desiredAccess,
    POBJECT_ATTRIBUTES objectAttributes,
    PIO_STATUS_BLOCK ioStatusBlock,
    PLARGE_INTEGER allocationSize,
    ULONG fileAttributes,
    ULONG shareAccess,
    ULONG createDisposition,
    ULONG createOptions,
    PVOID eaBuffer,
    ULONG eaLength)
{
    if (!TrueNtCreateFile)
        return static_cast<NTSTATUS>(0xC0000001L);

    wchar_t path[1024] = {};
    GetObjectName(objectAttributes, path, _countof(path));

    NTSTATUS status = TrueNtCreateFile(
        fileHandle, desiredAccess, objectAttributes, ioStatusBlock,
        allocationSize, fileAttributes, shareAccess, createDisposition,
        createOptions, eaBuffer, eaLength);

    if (status >= 0 && fileHandle && *fileHandle &&
        IsHidPath(path))
    {
        RememberHandle(*fileHandle, path);
    }

    return status;
}

static NTSTATUS NTAPI Hooked_NtOpenFile(
    PHANDLE fileHandle,
    ACCESS_MASK desiredAccess,
    POBJECT_ATTRIBUTES objectAttributes,
    PIO_STATUS_BLOCK ioStatusBlock,
    ULONG shareAccess,
    ULONG openOptions)
{
    if (!TrueNtOpenFile)
        return static_cast<NTSTATUS>(0xC0000001L);

    wchar_t path[1024] = {};
    GetObjectName(objectAttributes, path, _countof(path));

    NTSTATUS status = TrueNtOpenFile(
        fileHandle, desiredAccess, objectAttributes,
        ioStatusBlock, shareAccess, openOptions);

    if (status >= 0 && fileHandle && *fileHandle &&
        IsHidPath(path))
    {
        RememberHandle(*fileHandle, path);
    }

    return status;
}

// ============================================================
// Hook: NtDeviceIoControlFile
// 记录同步完成的输出数据；STATUS_PENDING 时不读取尚未完成的缓冲区。
// ============================================================

static NTSTATUS NTAPI Hooked_NtDeviceIoControlFile(
    HANDLE fileHandle,
    HANDLE event,
    AW2_IO_APC_ROUTINE apcRoutine,
    PVOID apcContext,
    PIO_STATUS_BLOCK ioStatusBlock,
    ULONG ioControlCode,
    PVOID inputBuffer,
    ULONG inputBufferLength,
    PVOID outputBuffer,
    ULONG outputBufferLength)
{
    if (!TrueNtDeviceIoControlFile)
        return static_cast<NTSTATUS>(0xC0000001L);

    NTSTATUS status = TrueNtDeviceIoControlFile(
        fileHandle, event, apcRoutine, apcContext, ioStatusBlock,
        ioControlCode, inputBuffer, inputBufferLength,
        outputBuffer, outputBufferLength);

    const NTSTATUS pending = static_cast<NTSTATUS>(0x00000103L);
    NTSTATUS finalStatus = status;
    ULONG_PTR information = 0;
    if (ioStatusBlock && status != pending)
    {
        finalStatus = ioStatusBlock->Status;
        information = ioStatusBlock->Information;
    }

    // 用 GET_COLLECTION_INFORMATION 的返回值识别 DS4：
    // HID_COLLECTION_INFORMATION: DescriptorSize[0..3], Polled[4],
    // VendorID[6..7], ProductID[8..9], VersionNumber[10..11].
    if (ioControlCode == 0x000B01A8 && status >= 0 && status != pending &&
        finalStatus >= 0 && outputBuffer && information >= 12 && outputBufferLength >= 12)
    {
        const BYTE* b = static_cast<const BYTE*>(outputBuffer);
        USHORT vid = static_cast<USHORT>(b[6] | (b[7] << 8));
        USHORT pid = static_cast<USHORT>(b[8] | (b[9] << 8));
        if (vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC))
        {
            if (MarkDs4Handle(fileHandle))
                ReadLog("DS4 HANDLE IDENTIFIED HANDLE=%p VID=%04X PID=%04X", fileHandle, vid, pid);
        }
    }

    if (IsDs4Handle(fileHandle) && status >= 0 && status != pending &&
        finalStatus >= 0 && outputBuffer && information > 0)
    {
        ULONG safeLength = outputBufferLength;
        if (information < safeLength) safeLength = static_cast<ULONG>(information);

        // 只处理输入报告类 IOCTL，完全忽略集合信息/描述符/其他 IOCTL。
        if (ioControlCode == 0x000B01A2 || ioControlCode == 0x000B000B)
            LogDs4InputReport("NtDeviceIoControlFile", static_cast<const BYTE*>(outputBuffer), safeLength);
    }

    return status;
}

// ============================================================
// Hook: NtReadFile
// ============================================================

static NTSTATUS NTAPI Hooked_NtReadFile(
    HANDLE fileHandle,
    HANDLE event,
    AW2_IO_APC_ROUTINE apcRoutine,
    PVOID apcContext,
    PIO_STATUS_BLOCK ioStatusBlock,
    PVOID buffer,
    ULONG length,
    PLARGE_INTEGER byteOffset,
    PULONG key)
{
    if (!TrueNtReadFile)
        return static_cast<NTSTATUS>(0xC0000001L);

    const bool isDs4 = IsDs4Handle(fileHandle);
    NTSTATUS status = TrueNtReadFile(
        fileHandle, event, apcRoutine, apcContext, ioStatusBlock,
        buffer, length, byteOffset, key);

    if (!isDs4 || !ioStatusBlock || !buffer || length == 0)
        return status;

    if (InterlockedCompareExchange(&g_Ds4NtReadSeen, 1, 0) == 0)
    {
        ReadLog("DS4 NtReadFile observed HANDLE=%p REQUEST=%lu STATUS=%08X EVENT=%p",
            fileHandle, static_cast<unsigned long>(length),
            static_cast<unsigned int>(status), event);
    }

    const NTSTATUS pending = static_cast<NTSTATUS>(0x00000103L);
    if (status != pending)
    {
        NTSTATUS ioStatus = ioStatusBlock->Status;
        ULONG_PTR bytes = ioStatusBlock->Information;
        if (ioStatus >= 0 && bytes > 0)
        {
            ULONG safeLength = length;
            if (bytes < safeLength)
                safeLength = static_cast<ULONG>(bytes);
            LogDs4InputReport("NtReadFile", static_cast<const BYTE*>(buffer), safeLength);
        }
    }
    else
    {
        TrackPendingDs4Read(fileHandle, event, ioStatusBlock, buffer, length);
        // Close the small race where I/O completes just before it is tracked.
        if (ioStatusBlock->Status != pending)
            CompletePendingDs4ReadsForWait(event ? event : fileHandle);
    }

    return status;
}

// ============================================================
// Hook: NtWaitForSingleObject
// ============================================================

static NTSTATUS NTAPI Hooked_NtWaitForSingleObject(
    HANDLE handle,
    BOOLEAN alertable,
    PLARGE_INTEGER timeout)
{
    if (!TrueNtWaitForSingleObject)
        return static_cast<NTSTATUS>(0xC0000001L);

    NTSTATUS status = TrueNtWaitForSingleObject(handle, alertable, timeout);
    if (status == 0)
        CompletePendingDs4ReadsForWait(handle);
    return status;
}

static NTSTATUS NTAPI Hooked_NtWaitForMultipleObjects(
    ULONG count,
    PHANDLE handles,
    ULONG waitType,
    BOOLEAN alertable,
    PLARGE_INTEGER timeout)
{
    if (!TrueNtWaitForMultipleObjects)
        return static_cast<NTSTATUS>(0xC0000001L);

    NTSTATUS status = TrueNtWaitForMultipleObjects(
        count, handles, waitType, alertable, timeout);

    // STATUS_WAIT_0 + index is returned for a successful wait-any;
    // wait-all returns STATUS_WAIT_0. Other positive statuses are not success.
    if (status >= 0 && static_cast<ULONG>(status) < count && handles)
    {
        for (ULONG i = 0; i < count; ++i)
            CompletePendingDs4ReadsForWait(handles[i]);
    }
    return status;
}

// ============================================================
// Hook: NtClose
// ============================================================

static NTSTATUS NTAPI Hooked_NtClose(HANDLE handle)
{
    if (!TrueNtClose)
        return static_cast<NTSTATUS>(0xC0000001L);

    NTSTATUS status = TrueNtClose(handle);
    if (status >= 0)
    {
        ForgetPendingReadsForHandle(handle);
        ForgetHandle(handle);
    }

    return status;
}

// ============================================================
// 卸载
// ============================================================

static void RemoveAllInputHooks()
{
    for (int i = HOOK_COUNT - 1; i >= 0; --i)
    {
        LPVOID target = g_HookTargets[i];
        if (!target)
            continue;

        MH_DisableHook(target);
        MH_RemoveHook(target);
        g_HookTargets[i] = nullptr;
    }

    TrueGetRawInputData = nullptr;
    TrueGetRawInputDeviceList = nullptr;
    TrueGetRawInputDeviceInfoW = nullptr;
    TrueHidD_GetInputReport = nullptr;
    TrueNtDeviceIoControlFile = nullptr;
    TrueNtCreateFile = nullptr;
    TrueNtOpenFile = nullptr;
    TrueNtReadFile = nullptr;
    TrueNtWaitForSingleObject = nullptr;
    TrueNtWaitForMultipleObjects = nullptr;
    TrueNtClose = nullptr;

    AcquireSRWLockExclusive(&g_HandlePathLock);
    ZeroMemory(g_HandlePaths, sizeof(g_HandlePaths));
    ReleaseSRWLockExclusive(&g_HandlePathLock);

    AcquireSRWLockExclusive(&g_PendingReadLock);
    ZeroMemory(g_PendingDs4Reads, sizeof(g_PendingDs4Reads));
    ReleaseSRWLockExclusive(&g_PendingReadLock);
    InterlockedExchange(&g_Ds4NtReadSeen, 0);

    g_InputHooksReady = false;
}

// ============================================================
// 初始化
// 保留旧导出名，兼容主 DLL。
// ============================================================

extern "C" __declspec(dllexport)
BOOL WINAPI InitReadFileHooks()
{
    if (g_InputHooksReady)
        return TRUE;

    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
    {
        ReadLog("MH_Initialize failed: %d", mh);
        return FALSE;
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
    {
        ReadLog("user32.dll not found");
        return FALSE;
    }

    FARPROC pRawData = GetProcAddress(user32, "GetRawInputData");
    FARPROC pRawList = GetProcAddress(user32, "GetRawInputDeviceList");
    FARPROC pRawInfo = GetProcAddress(user32, "GetRawInputDeviceInfoW");

    if (!pRawData || !pRawList || !pRawInfo)
    {
        ReadLog("Raw Input API missing");
        return FALSE;
    }

    HMODULE hid = GetModuleHandleW(L"hid.dll");
    if (!hid)
        hid = LoadLibraryW(L"hid.dll");

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

    if (!hid || !ntdll)
    {
        ReadLog("hid.dll or ntdll.dll not found");
        return FALSE;
    }

    FARPROC pHidGetInput = GetProcAddress(hid, "HidD_GetInputReport");
    FARPROC pNtIoctl = GetProcAddress(ntdll, "NtDeviceIoControlFile");
    FARPROC pNtCreate = GetProcAddress(ntdll, "NtCreateFile");
    FARPROC pNtOpen = GetProcAddress(ntdll, "NtOpenFile");
    FARPROC pNtRead = GetProcAddress(ntdll, "NtReadFile");
    FARPROC pNtWaitSingle = GetProcAddress(ntdll, "NtWaitForSingleObject");
    FARPROC pNtWaitMultiple = GetProcAddress(ntdll, "NtWaitForMultipleObjects");
    FARPROC pNtClose = GetProcAddress(ntdll, "NtClose");

    if (!pHidGetInput || !pNtIoctl || !pNtCreate || !pNtOpen ||
        !pNtRead || !pNtWaitSingle || !pNtWaitMultiple || !pNtClose)
    {
        ReadLog("HID/NT API missing");
        return FALSE;
    }

    if (!InstallHook(reinterpret_cast<LPVOID>(pRawData),
        reinterpret_cast<LPVOID>(&Hooked_GetRawInputData),
        reinterpret_cast<LPVOID*>(&TrueGetRawInputData),
        HOOK_RAW_DATA, "GetRawInputData"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pRawList),
        reinterpret_cast<LPVOID>(&Hooked_GetRawInputDeviceList),
        reinterpret_cast<LPVOID*>(&TrueGetRawInputDeviceList),
        HOOK_RAW_DEVICE_LIST, "GetRawInputDeviceList"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pRawInfo),
        reinterpret_cast<LPVOID>(&Hooked_GetRawInputDeviceInfoW),
        reinterpret_cast<LPVOID*>(&TrueGetRawInputDeviceInfoW),
        HOOK_RAW_DEVICE_INFO, "GetRawInputDeviceInfoW"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pHidGetInput),
        reinterpret_cast<LPVOID>(&Hooked_HidD_GetInputReport),
        reinterpret_cast<LPVOID*>(&TrueHidD_GetInputReport),
        HOOK_HID_GET_INPUT_REPORT, "HidD_GetInputReport"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pNtCreate),
        reinterpret_cast<LPVOID>(&Hooked_NtCreateFile),
        reinterpret_cast<LPVOID*>(&TrueNtCreateFile),
        HOOK_NT_CREATE_FILE, "NtCreateFile"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pNtOpen),
        reinterpret_cast<LPVOID>(&Hooked_NtOpenFile),
        reinterpret_cast<LPVOID*>(&TrueNtOpenFile),
        HOOK_NT_OPEN_FILE, "NtOpenFile"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pNtIoctl),
        reinterpret_cast<LPVOID>(&Hooked_NtDeviceIoControlFile),
        reinterpret_cast<LPVOID*>(&TrueNtDeviceIoControlFile),
        HOOK_NT_DEVICE_IO_CONTROL, "NtDeviceIoControlFile"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pNtRead),
        reinterpret_cast<LPVOID>(&Hooked_NtReadFile),
        reinterpret_cast<LPVOID*>(&TrueNtReadFile),
        HOOK_NT_READ_FILE, "NtReadFile"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pNtWaitSingle),
        reinterpret_cast<LPVOID>(&Hooked_NtWaitForSingleObject),
        reinterpret_cast<LPVOID*>(&TrueNtWaitForSingleObject),
        HOOK_NT_WAIT_SINGLE, "NtWaitForSingleObject"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pNtWaitMultiple),
        reinterpret_cast<LPVOID>(&Hooked_NtWaitForMultipleObjects),
        reinterpret_cast<LPVOID*>(&TrueNtWaitForMultipleObjects),
        HOOK_NT_WAIT_MULTIPLE, "NtWaitForMultipleObjects"))
        goto fail;

    if (!InstallHook(reinterpret_cast<LPVOID>(pNtClose),
        reinterpret_cast<LPVOID>(&Hooked_NtClose),
        reinterpret_cast<LPVOID*>(&TrueNtClose),
        HOOK_NT_CLOSE, "NtClose"))
        goto fail;

    g_InputHooksReady = true;
    ReadLog("HID + RawInput + NtReadFile monitoring hooks enabled");
    return TRUE;

fail:
    RemoveAllInputHooks();
    ReadLog("Input hook initialization failed; installed hooks removed");
    return FALSE;
}

// ============================================================
// 卸载
// ============================================================

extern "C" __declspec(dllexport)
void WINAPI UninitReadFileHooks()
{
    if (!g_InputHooksReady)
        return;

    RemoveAllInputHooks();
    ReadLog("HID + RawInput + NtReadFile monitoring hooks disabled");
}
