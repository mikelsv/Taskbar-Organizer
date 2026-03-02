#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace process_manager {

struct ProcessEntry {
    DWORD pid;
    std::wstring executableName;
};

std::vector<ProcessEntry> EnumerateProcessesWithVisibleWindows();

}  // namespace process_manager
