#include "process_manager.h"

#include <tlhelp32.h>

#include <algorithm>
#include <set>

namespace process_manager {
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

BOOL CALLBACK CollectPidsFromWindows(HWND hwnd, LPARAM lParam) {
    auto* pids = reinterpret_cast<std::set<DWORD>*>(lParam);
    if (!pids || !IsEligibleTopLevelWindow(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        pids->insert(pid);
    }

    return TRUE;
}

}  // namespace

std::vector<ProcessEntry> EnumerateProcessesWithVisibleWindows() {
    std::set<DWORD> visiblePids;
    EnumWindows(CollectPidsFromWindows, reinterpret_cast<LPARAM>(&visiblePids));

    std::vector<ProcessEntry> result;
    if (visiblePids.empty()) {
        return result;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (visiblePids.find(pe.th32ProcessID) != visiblePids.end()) {
                result.push_back(ProcessEntry{pe.th32ProcessID, pe.szExeFile});
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);

    std::sort(result.begin(), result.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
        if (a.executableName == b.executableName) {
            return a.pid < b.pid;
        }
        return a.executableName < b.executableName;
    });

    return result;
}

}  // namespace process_manager
