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
bool SwapWindowsByIndex(std::vector<WindowItem>& windows, int firstIndex, int secondIndex);
void ApplyOrder(const std::vector<WindowItem>& windows, bool manualReorderEnabled);

}  // namespace window_manager
