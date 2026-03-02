#include "window_manager.h"

#include <algorithm>

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
