#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace window_manager {

struct WindowItem {
    HWND hwnd;
    std::wstring title;
};

std::vector<WindowItem> EnumerateWindowsForProcess(DWORD pid);
void ApplyOrder(const std::vector<WindowItem>& windows, bool manualReorderEnabled);

}  // namespace window_manager
