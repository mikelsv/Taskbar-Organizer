#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace debug_func {

struct TaskbarWindowInfo {
    HWND hwnd;
    std::wstring title;
    int orderIndex;
};

void CreateDebugConsole();
int DebugResortWindows();

std::vector<std::wstring> GetAllTaskbarOrder();

// Получить список окон процесса на панели задач
// processName - имя процесса (например, L"firefox" или L"chrome")
std::vector<TaskbarWindowInfo> GetProcessTaskbarWindows(const wchar_t* processName);

// Найти конкретный процесс на панели задач и вывести результаты
// processName - имя процесса (например, L"firefox")
void FindProcessOnTaskbar(const wchar_t* processName);

} // namespace debug_func
