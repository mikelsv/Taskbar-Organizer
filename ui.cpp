#include "ui.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <set>
#include <vector>

#include "process_manager.h"
#include "window_manager.h"

namespace ui {
namespace {

enum ControlId {
    IDC_CHECK_MANUAL = 101,
    IDC_COMBO_PROCESS = 102,
    IDC_LIST_WINDOWS = 103,
    IDC_BUTTON_UP = 104,
    IDC_BUTTON_DOWN = 105,
    IDC_CHECK_APPLY_ON_FLY = 106,
    IDC_BUTTON_APPLY = 107
};

enum InternalMessage : UINT {
    WM_APP_MANUAL_REORDER = WM_APP + 1
};

enum class ReorderEvent : WPARAM {
    kLButtonDown = 1,
    kMouseMove = 2,
    kLButtonUp = 3
};

struct AppState {
    HWND hwndMain = nullptr;
    HWND checkboxManual = nullptr;
    HWND comboProcess = nullptr;
    HWND listWindows = nullptr;
    HWND buttonUp = nullptr;
    HWND buttonDown = nullptr;
    HWND checkboxApplyOnFly = nullptr;
    HWND buttonApply = nullptr;

    std::vector<process_manager::ProcessEntry> processes;
    std::vector<window_manager::WindowItem> windows;

    bool manualReorderEnabled = false;
    bool applyOnFlyEnabled = false;
    DWORD selectedPid = 0;

    bool windows10Supported = false;
    bool isDraggingTaskbarButton = false;
    int dragSourceIndex = -1;
    int dragTargetIndex = -1;
};

struct RemoteHitTest {
    int index = -1;
    bool success = false;
};

HHOOK g_mouseHook = nullptr;
HWND g_mainWindow = nullptr;

using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

AppState* GetState(HWND hwnd) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

bool IsWindows10OnlyModeSupported() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return false;
    }

    auto rtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) {
        return false;
    }

    RTL_OSVERSIONINFOW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (rtlGetVersion(&osvi) != 0) {
        return false;
    }

    // Experimental taskbar-toolbar automation is restricted to Windows 10 builds.
    // Windows 11 taskbar internals differ and are not supported by this mode.
    return osvi.dwMajorVersion == 10 && osvi.dwMinorVersion == 0 && osvi.dwBuildNumber < 22000;
}

HWND FindTaskbarToolbarWindow() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        return nullptr;
    }

    // Windows taskbar structure: Shell_TrayWnd -> MSTaskListWClass -> ToolbarWindow32
    // MSTaskListWClass is the taskbar list control, and ToolbarWindow32 inside it contains the buttons
    HWND taskList = FindWindowExW(tray, nullptr, L"MSTaskListWClass", nullptr);
    if (taskList) {
        HWND toolbar = FindWindowExW(taskList, nullptr, L"ToolbarWindow32", nullptr);
        if (toolbar) {
            return toolbar;
        }
    }

    // Fallback: try MSTaskSwWClass (older Windows versions)
    HWND rebar = FindWindowExW(tray, nullptr, L"ReBarWindow32", nullptr);
    HWND taskBand = rebar ? FindWindowExW(rebar, nullptr, L"MSTaskSwWClass", nullptr) : nullptr;
    HWND toolbar = taskBand ? FindWindowExW(taskBand, nullptr, L"ToolbarWindow32", nullptr) : nullptr;

    if (toolbar) {
        return toolbar;
    }

    // Fallback search: shell implementations vary by Windows 10 updates.
    HWND child = nullptr;
    while ((child = FindWindowExW(tray, child, nullptr, nullptr)) != nullptr) {
        if (child == tray) {
            continue;
        }

        wchar_t className[64]{};
        GetClassNameW(child, className, 64);
        if (wcscmp(className, L"ToolbarWindow32") == 0 || wcscmp(className, L"MSTaskSwWClass") == 0 || wcscmp(className, L"MSTaskListWClass") == 0) {
            // For MSTaskListWClass, we need to find ToolbarWindow32 inside it
            if (wcscmp(className, L"MSTaskListWClass") == 0) {
                HWND nestedToolbar = FindWindowExW(child, nullptr, L"ToolbarWindow32", nullptr);
                if (nestedToolbar) {
                    return nestedToolbar;
                }
            }
            return child;
        }

        HWND nestedToolbar = FindWindowExW(child, nullptr, L"ToolbarWindow32", nullptr);
        if (nestedToolbar) {
            return nestedToolbar;
        }
    }

    return nullptr;
}

bool IsCursorInsideToolbarClient(HWND toolbarHwnd, const POINT& screenPt, POINT& outClientPt) {
    if (!toolbarHwnd || !IsWindow(toolbarHwnd)) {
        return false;
    }

    outClientPt = screenPt;
    if (!ScreenToClient(toolbarHwnd, &outClientPt)) {
        return false;
    }

    RECT client{};
    if (!GetClientRect(toolbarHwnd, &client)) {
        return false;
    }

    return PtInRect(&client, outClientPt) == TRUE;
}

RemoteHitTest TaskbarToolbarHitTestCrossProcess(HWND toolbarHwnd, const POINT& toolbarClientPt) {
    RemoteHitTest result;
    if (!toolbarHwnd || !IsWindow(toolbarHwnd)) {
        return result;
    }

    DWORD explorerPid = 0;
    GetWindowThreadProcessId(toolbarHwnd, &explorerPid);
    if (explorerPid == 0) {
        return result;
    }

    HANDLE process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, explorerPid);
    if (!process) {
        return result;
    }

    LPVOID remotePoint = VirtualAllocEx(process, nullptr, sizeof(POINT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePoint) {
        CloseHandle(process);
        return result;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePoint, &toolbarClientPt, sizeof(toolbarClientPt), &written) ||
        written != sizeof(toolbarClientPt)) {
        VirtualFreeEx(process, remotePoint, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }

    LRESULT hit = SendMessageW(toolbarHwnd, TB_HITTEST, 0, reinterpret_cast<LPARAM>(remotePoint));
    if (hit >= 0) {
        POINT readback{};
        SIZE_T read = 0;
        ReadProcessMemory(process, remotePoint, &readback, sizeof(readback), &read);
        result.index = static_cast<int>(hit);
        result.success = true;
    }

    VirtualFreeEx(process, remotePoint, 0, MEM_RELEASE);
    CloseHandle(process);
    return result;
}

void UpdateMoveButtonsState(AppState& state) {
    const int selected = ListView_GetNextItem(state.listWindows, -1, LVNI_SELECTED);
    const bool hasSelection = selected >= 0 && selected < static_cast<int>(state.windows.size());

    EnableWindow(state.buttonUp, hasSelection && selected > 0);
    EnableWindow(state.buttonDown, hasSelection && selected < static_cast<int>(state.windows.size()) - 1);
}

void RefreshWindowsListView(AppState& state, int selectedIndex = -1) {
    ListView_DeleteAllItems(state.listWindows);

    std::vector<window_manager::WindowItem> filtered;
    filtered.reserve(state.windows.size());
    for (const auto& item : state.windows) {
        if (IsWindow(item.hwnd)) {
            filtered.push_back(item);
        }
    }
    state.windows = std::move(filtered);

    for (size_t i = 0; i < state.windows.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(state.windows[i].title.c_str());
        item.lParam = reinterpret_cast<LPARAM>(state.windows[i].hwnd);
        ListView_InsertItem(state.listWindows, &item);
    }

    if (!state.windows.empty()) {
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(state.windows.size())) {
            selectedIndex = 0;
        }

        ListView_SetItemState(state.listWindows, selectedIndex,
            LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED);
    }

    UpdateMoveButtonsState(state);
}

void LoadWindowsForSelectedProcess(AppState& state) {
    const int selectedIndex = static_cast<int>(SendMessageW(state.comboProcess, CB_GETCURSEL, 0, 0));
    if (selectedIndex >= 0) {
        wchar_t processName[256];
        SendMessageW(state.comboProcess, CB_GETLBTEXT, selectedIndex, reinterpret_cast<LPARAM>(processName));
        state.windows = window_manager::EnumerateWindowsForProcessByTaskbarOrder(processName);
    } else {
        state.windows.clear();
    }
    RefreshWindowsListView(state);
}

void PopulateProcessCombo(AppState& state) {
    const DWORD previousPid = state.selectedPid;

    state.processes = process_manager::EnumerateProcessesWithVisibleWindows();
    SendMessageW(state.comboProcess, CB_RESETCONTENT, 0, 0);

    // Collect unique process names to avoid duplicates
    std::set<std::wstring> addedNames;

    int selectedComboIndex = -1;
    for (size_t i = 0; i < state.processes.size(); ++i) {
        const auto& process = state.processes[i];

        // Skip duplicate process names
        if (addedNames.count(process.executableName) > 0) {
            continue;
        }
        addedNames.insert(process.executableName);

        int idx = static_cast<int>(SendMessageW(state.comboProcess, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(process.executableName.c_str())));
        if (idx >= 0) {
            SendMessageW(state.comboProcess, CB_SETITEMDATA, idx, static_cast<LPARAM>(process.pid));
            if (process.pid == previousPid) {
                selectedComboIndex = idx;
            }
        }
    }

    if (selectedComboIndex < 0 && !state.processes.empty()) {
        selectedComboIndex = 0;
    }

    state.selectedPid = 0;
    if (selectedComboIndex >= 0) {
        SendMessageW(state.comboProcess, CB_SETCURSEL, selectedComboIndex, 0);
        state.selectedPid = static_cast<DWORD>(SendMessageW(state.comboProcess, CB_GETITEMDATA, selectedComboIndex, 0));
    }

    LoadWindowsForSelectedProcess(state);
}

void ApplyCurrentOrder(AppState& state) {
    window_manager::ApplyOrder(state.windows, state.manualReorderEnabled);
}

void MoveSelectedWindow(AppState& state, int direction) {
    const int selected = ListView_GetNextItem(state.listWindows, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(state.windows.size())) {
        return;
    }

    const int newIndex = selected + direction;
    if (newIndex < 0 || newIndex >= static_cast<int>(state.windows.size())) {
        return;
    }

    window_manager::SwapWindowsByIndex(state.windows, selected, newIndex);
    RefreshWindowsListView(state, newIndex);

    if (state.applyOnFlyEnabled) {
        ApplyCurrentOrder(state);
    }
}

void DrawDragIndicator(const AppState& state, HDC hdc) {
    if (!state.isDraggingTaskbarButton || state.dragTargetIndex < 0) {
        return;
    }

    RECT listRect{};
    if (!GetWindowRect(state.listWindows, &listRect)) {
        return;
    }
    MapWindowPoints(nullptr, state.hwndMain, reinterpret_cast<POINT*>(&listRect), 2);

    int y = listRect.top + 4;
    if (state.dragTargetIndex > 0) {
        RECT itemRect{};
        if (ListView_GetItemRect(state.listWindows, state.dragTargetIndex - 1, &itemRect, LVIR_BOUNDS)) {
            POINT p{itemRect.left, itemRect.bottom};
            MapWindowPoints(state.listWindows, state.hwndMain, &p, 1);
            y = p.y;
        }
    }

    HPEN pen = CreatePen(PS_SOLID, 2, RGB(200, 32, 32));
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, listRect.left + 2, y, nullptr);
    LineTo(hdc, listRect.right - 2, y);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

bool InstallMouseHook(AppState& state) {
    if (!state.manualReorderEnabled || !state.windows10Supported) {
        return false;
    }

    if (g_mouseHook) {
        return true;
    }

    g_mainWindow = state.hwndMain;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (code == HC_ACTION && g_mainWindow != nullptr) {
            const auto* mouseInfo = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            if (mouseInfo) {
                switch (wParam) {
                case WM_LBUTTONDOWN:
                    PostMessageW(g_mainWindow, WM_APP_MANUAL_REORDER, static_cast<WPARAM>(ReorderEvent::kLButtonDown),
                        MAKELPARAM(mouseInfo->pt.x, mouseInfo->pt.y));
                    break;
                case WM_MOUSEMOVE:
                    PostMessageW(g_mainWindow, WM_APP_MANUAL_REORDER, static_cast<WPARAM>(ReorderEvent::kMouseMove),
                        MAKELPARAM(mouseInfo->pt.x, mouseInfo->pt.y));
                    break;
                case WM_LBUTTONUP:
                    PostMessageW(g_mainWindow, WM_APP_MANUAL_REORDER, static_cast<WPARAM>(ReorderEvent::kLButtonUp),
                        MAKELPARAM(mouseInfo->pt.x, mouseInfo->pt.y));
                    break;
                default:
                    break;
                }
            }
        }
        return CallNextHookEx(g_mouseHook, code, wParam, lParam);
        }, GetModuleHandleW(nullptr), 0);

    return g_mouseHook != nullptr;
}

void UninstallMouseHook() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}

void OnManualReorderMouseEvent(AppState& state, ReorderEvent eventType, const POINT& screenPt) {
    if (!state.manualReorderEnabled || !state.windows10Supported) {
        return;
    }

    HWND toolbar = FindTaskbarToolbarWindow();
    POINT toolbarClientPt{};

    if (eventType == ReorderEvent::kLButtonDown) {
        state.isDraggingTaskbarButton = false;
        state.dragSourceIndex = -1;
        state.dragTargetIndex = -1;

        if (!toolbar || !IsCursorInsideToolbarClient(toolbar, screenPt, toolbarClientPt)) {
            InvalidateRect(state.hwndMain, nullptr, TRUE);
            return;
        }

        RemoteHitTest hit = TaskbarToolbarHitTestCrossProcess(toolbar, toolbarClientPt);
        if (!hit.success || hit.index < 0) {
            InvalidateRect(state.hwndMain, nullptr, TRUE);
            return;
        }

        state.isDraggingTaskbarButton = true;
        state.dragSourceIndex = hit.index;
        state.dragTargetIndex = hit.index;
        InvalidateRect(state.hwndMain, nullptr, TRUE);
        return;
    }

    if (!state.isDraggingTaskbarButton) {
        return;
    }

    if (!toolbar || !IsCursorInsideToolbarClient(toolbar, screenPt, toolbarClientPt)) {
        if (eventType == ReorderEvent::kMouseMove) {
            state.dragTargetIndex = state.dragSourceIndex;
            InvalidateRect(state.hwndMain, nullptr, TRUE);
        }
        if (eventType == ReorderEvent::kLButtonUp) {
            state.isDraggingTaskbarButton = false;
            state.dragSourceIndex = -1;
            state.dragTargetIndex = -1;
            UninstallMouseHook();
            InstallMouseHook(state);
            InvalidateRect(state.hwndMain, nullptr, TRUE);
        }
        return;
    }

    RemoteHitTest hit = TaskbarToolbarHitTestCrossProcess(toolbar, toolbarClientPt);
    if (hit.success && hit.index >= 0) {
        state.dragTargetIndex = hit.index;
        if (eventType == ReorderEvent::kMouseMove) {
            InvalidateRect(state.hwndMain, nullptr, TRUE);
            return;
        }
    }

    if (eventType == ReorderEvent::kLButtonUp) {
        if (state.dragSourceIndex >= 0 && state.dragTargetIndex >= 0 &&
            state.dragSourceIndex != state.dragTargetIndex &&
            state.dragSourceIndex < static_cast<int>(state.windows.size()) &&
            state.dragTargetIndex < static_cast<int>(state.windows.size())) {
            window_manager::SwapWindowsByIndex(state.windows, state.dragSourceIndex, state.dragTargetIndex);
            RefreshWindowsListView(state, state.dragTargetIndex);
            ApplyCurrentOrder(state);
        }

        state.isDraggingTaskbarButton = false;
        state.dragSourceIndex = -1;
        state.dragTargetIndex = -1;
        UninstallMouseHook();
        InstallMouseHook(state);
        InvalidateRect(state.hwndMain, nullptr, TRUE);
    }
}

void CreateControls(AppState& state) {
    const int margin = 12;
    const int clientWidth = 740;

    state.checkboxManual = CreateWindowExW(0, WC_BUTTONW, L"Enable manual taskbar reorder (experimental, Windows 10 only)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        margin, margin, clientWidth - margin * 2, 24,
        state.hwndMain, reinterpret_cast<HMENU>(IDC_CHECK_MANUAL), nullptr, nullptr);

    state.comboProcess = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
        margin, 44, clientWidth - margin * 2, 240,
        state.hwndMain, reinterpret_cast<HMENU>(IDC_COMBO_PROCESS), nullptr, nullptr);

    state.listWindows = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        margin, 80, 560, 300,
        state.hwndMain, reinterpret_cast<HMENU>(IDC_LIST_WINDOWS), nullptr, nullptr);

    ListView_SetExtendedListViewStyle(state.listWindows, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<LPWSTR>(L"Window Title");
    column.cx = 540;
    ListView_InsertColumn(state.listWindows, 0, &column);

    state.buttonUp = CreateWindowExW(0, WC_BUTTONW, L"Up",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        590, 120, 120, 30,
        state.hwndMain, reinterpret_cast<HMENU>(IDC_BUTTON_UP), nullptr, nullptr);

    state.buttonDown = CreateWindowExW(0, WC_BUTTONW, L"Down",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        590, 160, 120, 30,
        state.hwndMain, reinterpret_cast<HMENU>(IDC_BUTTON_DOWN), nullptr, nullptr);

    state.checkboxApplyOnFly = CreateWindowExW(0, WC_BUTTONW, L"Apply on the fly",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        margin, 392, 170, 24,
        state.hwndMain, reinterpret_cast<HMENU>(IDC_CHECK_APPLY_ON_FLY), nullptr, nullptr);

    state.buttonApply = CreateWindowExW(0, WC_BUTTONW, L"Apply",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        200, 390, 120, 28,
        state.hwndMain, reinterpret_cast<HMENU>(IDC_BUTTON_APPLY), nullptr, nullptr);

    UpdateMoveButtonsState(state);
}

void ResizeControls(AppState& state, int width, int height) {
    const int margin = 12;
    const int controlsRightWidth = 120;
    const int gap = 10;
    const int topY = 80;
    const int bottomBarHeight = 36;

    MoveWindow(state.checkboxManual, margin, margin, width - margin * 2, 24, TRUE);
    MoveWindow(state.comboProcess, margin, 44, width - margin * 2, 240, TRUE);

    const int listWidth = width - (margin * 3 + controlsRightWidth);
    const int listHeight = height - topY - bottomBarHeight - margin * 2;
    MoveWindow(state.listWindows, margin, topY, listWidth, (std::max)(120, listHeight), TRUE);

    const int rightX = margin * 2 + listWidth;
    MoveWindow(state.buttonUp, rightX, topY + 40, controlsRightWidth, 30, TRUE);
    MoveWindow(state.buttonDown, rightX, topY + 80, controlsRightWidth, 30, TRUE);

    const int bottomY = topY + (std::max)(120, listHeight) + gap;
    MoveWindow(state.checkboxApplyOnFly, margin, bottomY, 170, 24, TRUE);
    MoveWindow(state.buttonApply, margin + 180, bottomY - 2, 120, 28, TRUE);

    RECT rc{};
    GetClientRect(state.listWindows, &rc);
    ListView_SetColumnWidth(state.listWindows, 0, rc.right - rc.left - 4);
}

}  // namespace

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    AppState* state = GetState(hwnd);

    switch (message) {
    case WM_CREATE: {
        auto appState = std::make_unique<AppState>();
        appState->hwndMain = hwnd;
        appState->windows10Supported = IsWindows10OnlyModeSupported();

        CreateControls(*appState);
        PopulateProcessCombo(*appState);

        if (!appState->windows10Supported) {
            EnableWindow(appState->checkboxManual, FALSE);
            SendMessageW(appState->checkboxManual, BM_SETCHECK, BST_UNCHECKED, 0);
            MessageBoxW(hwnd,
                L"Taskbar toolbar synchronization is only supported on Windows 10. Falling back to EnumWindows order.",
                L"Taskbar Organizer",
                MB_OK | MB_ICONINFORMATION);
        }

        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(appState.release()));
        return 0;
    }
    case WM_SIZE:
        if (state) {
            ResizeControls(*state, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_COMMAND:
        if (!state) {
            return 0;
        }

        switch (LOWORD(wParam)) {
        case IDC_CHECK_MANUAL:
            state->manualReorderEnabled = (SendMessageW(state->checkboxManual, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (state->manualReorderEnabled && state->windows10Supported) {
                InstallMouseHook(*state);
            } else {
                state->isDraggingTaskbarButton = false;
                state->dragSourceIndex = -1;
                state->dragTargetIndex = -1;
                UninstallMouseHook();
                InvalidateRect(state->hwndMain, nullptr, TRUE);
            }
            return 0;

        case IDC_CHECK_APPLY_ON_FLY:
            state->applyOnFlyEnabled = (SendMessageW(state->checkboxApplyOnFly, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;

        case IDC_COMBO_PROCESS:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int selectedIndex = static_cast<int>(SendMessageW(state->comboProcess, CB_GETCURSEL, 0, 0));
                if (selectedIndex >= 0) {
                    state->selectedPid = static_cast<DWORD>(SendMessageW(state->comboProcess, CB_GETITEMDATA, selectedIndex, 0));
                    LoadWindowsForSelectedProcess(*state);
                }
            }
            return 0;

        case IDC_BUTTON_UP:
            MoveSelectedWindow(*state, -1);
            return 0;

        case IDC_BUTTON_DOWN:
            MoveSelectedWindow(*state, +1);
            return 0;

        case IDC_BUTTON_APPLY:
            ApplyCurrentOrder(*state);
            return 0;

        default:
            break;
        }
        break;

    case WM_NOTIFY:
        if (state) {
            auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
            if (hdr && hdr->idFrom == IDC_LIST_WINDOWS && hdr->code == LVN_ITEMCHANGED) {
                UpdateMoveButtonsState(*state);
            }
        }
        return 0;

    case WM_APP_MANUAL_REORDER:
        if (state) {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            OnManualReorderMouseEvent(*state, static_cast<ReorderEvent>(wParam), pt);
        }
        return 0;

    case WM_PAINT:
        if (state) {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawDragIndicator(*state, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (state) {
            UninstallMouseHook();
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace ui
