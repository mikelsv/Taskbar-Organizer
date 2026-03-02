#include "window_manager.h"

#include <CommCtrl.h>

#include <algorithm>
#include <cstdint>

namespace window_manager {
namespace {

bool IsEligibleTopLevelWindow(HWND hwnd) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }

    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }

    return true;
}

struct EnumContext {
    DWORD pid;
    std::vector<WindowItem>* output;
};

BOOL CALLBACK EnumProcessWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);
    if (!ctx || !ctx->output || !IsEligibleTopLevelWindow(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) {
        return TRUE;
    }

    const int length = GetWindowTextLengthW(hwnd);
    std::wstring title;
    if (length > 0) {
        std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, buffer.data(), length + 1);
        if (copied > 0) {
            title.assign(buffer.c_str(), static_cast<size_t>(copied));
        }
    }

    if (title.empty()) {
        title = L"<Untitled window>";
    }

    ctx->output->push_back(WindowItem{hwnd, std::move(title)});
    return TRUE;
}

HWND FindTaskbarToolbarWindow() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        return nullptr;
    }

    // Recursively walk descendants by repeatedly calling FindWindowEx to locate ToolbarWindow32.
    std::vector<HWND> stack;
    stack.push_back(tray);

    while (!stack.empty()) {
        HWND parent = stack.back();
        stack.pop_back();

        HWND child = nullptr;
        while ((child = FindWindowExW(parent, child, nullptr, nullptr)) != nullptr) {
            wchar_t className[64]{};
            GetClassNameW(child, className, 64);
            if (lstrcmpW(className, L"ToolbarWindow32") == 0) {
                return child;
            }
            stack.push_back(child);
        }
    }

    return nullptr;
}

bool TryExtractHwndFromButtonData(HANDLE explorerProcess, const TBBUTTON& button, HWND* hwndOut) {
    if (!hwndOut) {
        return false;
    }

    const auto direct = reinterpret_cast<HWND>(button.dwData);
    if (direct && IsWindow(direct)) {
        *hwndOut = direct;
        return true;
    }

    if (button.dwData == 0) {
        return false;
    }

    // On Windows 10 taskbar, TBBUTTON::dwData points to an Explorer-owned structure.
    // Probe its first bytes and treat pointer-sized fields as potential HWND candidates.
    std::uintptr_t bytes[8]{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(explorerProcess, reinterpret_cast<LPCVOID>(button.dwData), bytes, sizeof(bytes), &read) ||
        read < sizeof(std::uintptr_t)) {
        return false;
    }

    const size_t count = read / sizeof(std::uintptr_t);
    for (size_t i = 0; i < count; ++i) {
        HWND candidate = reinterpret_cast<HWND>(bytes[i]);
        if (candidate && IsWindow(candidate)) {
            *hwndOut = candidate;
            return true;
        }
    }

    return false;
}

}  // namespace

std::vector<WindowItem> EnumerateWindowsForProcess(DWORD pid) {
    std::vector<WindowItem> items;
    if (pid == 0) {
        return items;
    }

    EnumContext ctx{pid, &items};
    EnumWindows(EnumProcessWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return items;
}

std::vector<HWND> GetTaskbarButtonOrder(bool* succeeded) {
    if (succeeded) {
        *succeeded = false;
    }

    std::vector<HWND> ordered;

    HWND toolbar = FindTaskbarToolbarWindow();
    if (!toolbar) {
        return ordered;
    }

    DWORD explorerPid = 0;
    GetWindowThreadProcessId(toolbar, &explorerPid);
    if (explorerPid == 0) {
        return ordered;
    }

    HANDLE explorerProcess = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, explorerPid);
    if (!explorerProcess) {
        return ordered;
    }

    // TB_BUTTONCOUNT returns how many taskbar buttons are in the toolbar.
    const LRESULT buttonCount = SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0);
    if (buttonCount <= 0) {
        CloseHandle(explorerProcess);
        return ordered;
    }

    ordered.reserve(static_cast<size_t>(buttonCount));

    for (int i = 0; i < buttonCount; ++i) {
        // TB_GETBUTTON writes into memory of the toolbar owner process (explorer.exe),
        // so we must allocate a TBBUTTON buffer in explorer using VirtualAllocEx.
        LPVOID remoteButton = VirtualAllocEx(explorerProcess, nullptr, sizeof(TBBUTTON), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteButton) {
            ordered.clear();
            CloseHandle(explorerProcess);
            return ordered;
        }

        // TB_GETBUTTON fills a TBBUTTON at the remote pointer for the current index.
        if (!SendMessageW(toolbar, TB_GETBUTTON, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(remoteButton))) {
            VirtualFreeEx(explorerProcess, remoteButton, 0, MEM_RELEASE);
            ordered.clear();
            CloseHandle(explorerProcess);
            return ordered;
        }

        TBBUTTON localButton{};
        SIZE_T read = 0;
        if (!ReadProcessMemory(explorerProcess, remoteButton, &localButton, sizeof(localButton), &read) || read != sizeof(localButton)) {
            VirtualFreeEx(explorerProcess, remoteButton, 0, MEM_RELEASE);
            ordered.clear();
            CloseHandle(explorerProcess);
            return ordered;
        }

        HWND hwnd = nullptr;
        if (TryExtractHwndFromButtonData(explorerProcess, localButton, &hwnd)) {
            ordered.push_back(hwnd);
        }

        VirtualFreeEx(explorerProcess, remoteButton, 0, MEM_RELEASE);
    }

    CloseHandle(explorerProcess);

    if (succeeded) {
        *succeeded = !ordered.empty();
    }
    return ordered;
}

std::vector<WindowItem> EnumerateWindowsForProcessByTaskbarOrder(DWORD pid, bool allowTaskbarSync) {
    auto fallback = EnumerateWindowsForProcess(pid);
    if (!allowTaskbarSync || pid == 0) {
        return fallback;
    }

    bool gotTaskbar = false;
    const auto taskbarOrder = GetTaskbarButtonOrder(&gotTaskbar);
    if (!gotTaskbar || taskbarOrder.empty()) {
        return fallback;
    }

    std::vector<WindowItem> ordered;
    ordered.reserve(fallback.size());

    for (HWND hwnd : taskbarOrder) {
        if (!IsWindow(hwnd) || !IsEligibleTopLevelWindow(hwnd)) {
            continue;
        }

        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid != pid) {
            continue;
        }

        auto it = std::find_if(fallback.begin(), fallback.end(), [hwnd](const WindowItem& item) {
            return item.hwnd == hwnd;
        });
        if (it != fallback.end() &&
            std::none_of(ordered.begin(), ordered.end(), [hwnd](const WindowItem& item) { return item.hwnd == hwnd; })) {
            ordered.push_back(*it);
        }
    }

    for (const auto& item : fallback) {
        if (std::none_of(ordered.begin(), ordered.end(), [h = item.hwnd](const WindowItem& current) { return current.hwnd == h; })) {
            ordered.push_back(item);
        }
    }

    return ordered;
}

bool SwapWindowsByIndex(std::vector<WindowItem>& windows, int firstIndex, int secondIndex) {
    if (firstIndex < 0 || secondIndex < 0 ||
        firstIndex >= static_cast<int>(windows.size()) ||
        secondIndex >= static_cast<int>(windows.size()) ||
        firstIndex == secondIndex) {
        return false;
    }

    std::iter_swap(windows.begin() + firstIndex, windows.begin() + secondIndex);
    return true;
}

void ApplyOrder(const std::vector<WindowItem>& windows, bool manualReorderEnabled) {
    if (!manualReorderEnabled) {
        return;
    }

    for (const auto& item : windows) {
        if (!IsWindow(item.hwnd)) {
            continue;
        }

        SetWindowPos(item.hwnd, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(item.hwnd);
        Sleep(50);
    }
}

}  // namespace window_manager
