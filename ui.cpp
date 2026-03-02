#include "ui.h"

#include <commctrl.h>

#include <algorithm>
#include <memory>
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
};

AppState* GetState(HWND hwnd) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
    state.windows = window_manager::EnumerateWindowsForProcess(state.selectedPid);
    RefreshWindowsListView(state);
}

void PopulateProcessCombo(AppState& state) {
    const DWORD previousPid = state.selectedPid;

    state.processes = process_manager::EnumerateProcessesWithVisibleWindows();
    SendMessageW(state.comboProcess, CB_RESETCONTENT, 0, 0);

    int selectedComboIndex = -1;
    for (size_t i = 0; i < state.processes.size(); ++i) {
        const auto& process = state.processes[i];
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

    std::iter_swap(state.windows.begin() + selected, state.windows.begin() + newIndex);
    RefreshWindowsListView(state, newIndex);

    if (state.applyOnFlyEnabled) {
        ApplyCurrentOrder(state);
    }
}

void CreateControls(AppState& state) {
    const int margin = 12;
    const int clientWidth = 740;

    state.checkboxManual = CreateWindowExW(0, WC_BUTTONW, L"Enable manual taskbar window reordering",
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
        CreateControls(*appState);
        PopulateProcessCombo(*appState);

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

    case WM_DESTROY:
        if (state) {
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
