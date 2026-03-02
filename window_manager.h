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
std::vector<HWND> GetTaskbarButtonOrder(bool* succeeded = nullptr);
std::vector<WindowItem> EnumerateWindowsForProcessByTaskbarOrder(DWORD pid, bool allowTaskbarSync);
bool SwapWindowsByIndex(std::vector<WindowItem>& windows, int firstIndex, int secondIndex);
void ApplyOrder(const std::vector<WindowItem>& windows, bool manualReorderEnabled);
void DebugPrintTaskbarButtons();  // Тестовая функция для отладки

}  // namespace window_manager
