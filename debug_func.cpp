#include "debug_func.h"

#include <windows.h>
#include <CommCtrl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>

#include <shobjidl.h>  // ITaskbarList3
#include <uiautomation.h>
#include <fcntl.h>   // для _O_U16TEXT
#include <io.h>      // для _setmode и _fileno
#include <iostream>
#include <thread>
#include <chrono>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace debug_func {

struct WindowInfo {
    HWND hwnd;
    std::wstring title;
};

namespace {
// Получить PID процесса по имени (без расширения)
DWORD GetProcessIdByName(const wchar_t* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(pe32);

    if (Process32FirstW(snapshot, &pe32)) {
        do {
            // Сравниваем без учета расширения .exe
            std::wstring exeName = pe32.szExeFile;
            size_t dotPos = exeName.find(L'.');
            if (dotPos != std::wstring::npos) {
                exeName = exeName.substr(0, dotPos);
            }

            std::wstring targetName = processName;
            // Убираем .exe если есть
            size_t targetDotPos = targetName.find(L'.');
            if (targetDotPos != std::wstring::npos) {
                targetName = targetName.substr(0, targetDotPos);
            }

            // Нечувствительное к регистру сравнение
            std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::towlower);
            std::transform(targetName.begin(), targetName.end(), targetName.begin(), ::towlower);

            if (exeName == targetName) {
                CloseHandle(snapshot);
                return pe32.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);
    return 0;
}

// Найти хэндл тулбара панели задач
HWND FindTaskbarToolbarWindow() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        return nullptr;
    }

    // Windows taskbar structure: Shell_TrayWnd -> MSTaskListWClass -> ToolbarWindow32
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

    // Fallback search
    HWND child = nullptr;
    while ((child = FindWindowExW(tray, child, nullptr, nullptr)) != nullptr) {
        if (child == tray) {
            continue;
        }

        wchar_t className[64]{};
        GetClassNameW(child, className, 64);
        if (wcscmp(className, L"ToolbarWindow32") == 0 ||
            wcscmp(className, L"MSTaskSwWClass") == 0 ||
            wcscmp(className, L"MSTaskListWClass") == 0) {
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

// Попытаться извлечь HWND из данных кнопки
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

    std::uintptr_t bytes[8]{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(explorerProcess, reinterpret_cast<LPCVOID>(button.dwData),
                           bytes, sizeof(bytes), &read) ||
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

// Получить список HWND кнопок на панели задач в порядке их отображения
std::vector<HWND> GetTaskbarButtonOrder() {
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

    const LRESULT buttonCount = SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0);
    if (buttonCount <= 0) {
        CloseHandle(explorerProcess);
        return ordered;
    }

    ordered.reserve(static_cast<size_t>(buttonCount));

    for (int i = 0; i < buttonCount; ++i) {
        LPVOID remoteButton = VirtualAllocEx(explorerProcess, nullptr, sizeof(TBBUTTON),
                                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteButton) {
            ordered.clear();
            CloseHandle(explorerProcess);
            return ordered;
        }

        if (!SendMessageW(toolbar, TB_GETBUTTON, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(remoteButton))) {
            VirtualFreeEx(explorerProcess, remoteButton, 0, MEM_RELEASE);
            ordered.clear();
            CloseHandle(explorerProcess);
            return ordered;
        }

        TBBUTTON localButton{};
        SIZE_T read = 0;
        if (!ReadProcessMemory(explorerProcess, remoteButton, &localButton,
                              sizeof(localButton), &read) || read != sizeof(localButton)) {
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
    return ordered;
}

// Получить текст окна
std::wstring GetWindowTitle(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    if (length == 0) {
        return L"";
    }

    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return buffer;
}

} // anonymous namespace

void CreateDebugConsole(){
    // 1. Создаем консоль
    if (AllocConsole()) {
        FILE* fDummy;
        // 1. Открываем как обычный текст
        freopen_s(&fDummy, "CONOUT$", "w", stdout);
        freopen_s(&fDummy, "CONOUT$", "w", stderr);
        freopen_s(&fDummy, "CONIN$", "r", stdin);

        // 1. Устанавливаем кодировку вывода в саму консоль (UTF-8)
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        // 2. Переключаем поток в режим UTF-8 (это уберет мусор и позволит писать по-русски)
        _setmode(_fileno(stdout), _O_U8TEXT);

        // Опционально: переназвать окно консоли
        SetConsoleTitle(L"Debug Console");
    }    
}

// Основная функция: получить список окон процесса на панели задач
std::vector<TaskbarWindowInfo> GetProcessTaskbarWindows(const wchar_t* processName) {
    std::vector<TaskbarWindowInfo> result;

    // 1. Получаем PID процесса
    DWORD pid = GetProcessIdByName(processName);
    if (pid == 0) {
        wprintf(L"Процесс '%s' не найден\n", processName);
        return result;
    }
    wprintf(L"PID процесса %s: %u\n", processName, pid);

    // 2. Получаем порядок кнопок на панели задач
    std::vector<HWND> taskbarButtons = GetTaskbarButtonOrder();
    if (taskbarButtons.empty()) {
        wprintf(L"Не удалось получить кнопки панели задач\n");
        return result;
    }

    wprintf(L"Найдено %zu кнопок на панели задач\n", taskbarButtons.size());

    // 3. Фильтруем по PID процесса и собираем информацию
    for (size_t i = 0; i < taskbarButtons.size(); ++i) {
        HWND hwnd = taskbarButtons[i];
        if (!IsWindow(hwnd)) {
            continue;
        }

        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);

        if (windowPid == pid) {
            std::wstring title = GetWindowTitle(hwnd);
            result.push_back({hwnd, title, static_cast<int>(i)});
            wprintf(L"  [%zu] HWND: %p | Title: %s\n", i, hwnd, title.c_str());
        }
    }

    return result;
}

// Функция для поиска конкретного процесса (например, firefox) на панели задач
void FindProcessOnTaskbar(const wchar_t* processName) {
    wprintf(L"\n=== Поиск окон '%s' на панели задач ===\n", processName);

    auto windows = GetProcessTaskbarWindows(processName);

    if (windows.empty()) {
        wprintf(L"Окна не найдены (возможно, они сгруппированы в одну иконку без текста).\n");
    } else {
        wprintf(L"\n=== Результаты поиска ===\n");
        for (size_t i = 0; i < windows.size(); ++i) {
            wprintf(L"%zu. %s\n", i + 1, windows[i].title.c_str());
        }
    }
}

std::vector<std::wstring> GetAllTaskbarOrder()
{
    std::vector<std::wstring> result;

    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr))
        return result;

    IUIAutomation* automation = nullptr;
    hr = CoCreateInstance(
        CLSID_CUIAutomation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IUIAutomation,
        (void**)&automation
    );

    if (FAILED(hr) || !automation)
    {
        CoUninitialize();
        return result;
    }

    // 1️⃣ Найти окно панели задач (Shell_TrayWnd)
    HWND trayHwnd = FindWindow(L"Shell_TrayWnd", nullptr);
    if (!trayHwnd)
    {
        automation->Release();
        CoUninitialize();
        return result;
    }

    IUIAutomationElement* taskbarElement = nullptr;
    hr = automation->ElementFromHandle(trayHwnd, &taskbarElement);
    if (FAILED(hr) || !taskbarElement)
    {
        automation->Release();
        CoUninitialize();
        return result;
    }

    // 2️⃣ Найти MSTaskListWClass
    VARIANT varProp;
    varProp.vt = VT_BSTR;
    varProp.bstrVal = SysAllocString(L"MSTaskListWClass");

    IUIAutomationCondition* classCondition = nullptr;
    automation->CreatePropertyCondition(
        UIA_ClassNamePropertyId,
        varProp,
        &classCondition
    );

    IUIAutomationElement* appList = nullptr;
    taskbarElement->FindFirst(
        TreeScope_Subtree,
        classCondition,
        &appList
    );

    SysFreeString(varProp.bstrVal);
    classCondition->Release();

    if (!appList)
    {
        // fallback — ищем первый ToolBar
        VARIANT varType;
        varType.vt = VT_I4;
        varType.lVal = UIA_ToolBarControlTypeId;

        IUIAutomationCondition* toolbarCondition = nullptr;
        automation->CreatePropertyCondition(
            UIA_ControlTypePropertyId,
            varType,
            &toolbarCondition
        );

        taskbarElement->FindFirst(
            TreeScope_Subtree,
            toolbarCondition,
            &appList
        );

        toolbarCondition->Release();
    }

    if (!appList)
    {
        taskbarElement->Release();
        automation->Release();
        CoUninitialize();
        return result;
    }

    // 3️⃣ Получить дочерние элементы (кнопки)
    IUIAutomationCondition* trueCondition = nullptr;
    hr = automation->CreateTrueCondition(&trueCondition);

    IUIAutomationElementArray* children = nullptr;
    if (SUCCEEDED(hr))
    {
        hr = appList->FindAll(TreeScope_Children, trueCondition, &children);
    }

    if (trueCondition)
        trueCondition->Release();

    if (children)
    {
        int length = 0;
        children->get_Length(&length);

        for (int i = 0; i < length; ++i)
        {
            IUIAutomationElement* child = nullptr;
            if (SUCCEEDED(children->GetElement(i, &child)) && child)
            {
                BSTR name;
                if (SUCCEEDED(child->get_CurrentName(&name)) && name)
                {
                    std::wstring title(name);
                    SysFreeString(name);

                    if (!title.empty()// &&
                       ) //title.find(L"Firefox") != std::wstring::npos)
                    {
                        result.push_back(title);
                    }
                }
                child->Release();
            }
        }
        children->Release();
    }

    appList->Release();
    taskbarElement->Release();
    automation->Release();
    CoUninitialize();

    return result;
}

// Callback для EnumWindows
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto& data = *reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;

    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return TRUE;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snapshot, &pe))
    {
        do {
            if (pe.th32ProcessID == pid &&
                _wcsicmp(pe.szExeFile, L"firefox.exe") == 0)
            {
                wchar_t buf[512];
                GetWindowText(hwnd, buf, _countof(buf));
                if (wcslen(buf) > 0)
                {
                    data.push_back({ hwnd, buf });
                }
            }
        } while (Process32Next(snapshot, &pe));
    }
    CloseHandle(snapshot);

    return TRUE;
}

int DebugResortWindows() {
    // 1️⃣ Собираем окна Firefox
    std::vector<WindowInfo> windows;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    if (windows.empty())
    {
        std::wcout << L"Окна Firefox не найдены.\n";
        return 0;
    }

    // 2️⃣ Сортируем по заголовку
    std::sort(windows.begin(), windows.end(),
        [](const WindowInfo& a, const WindowInfo& b) {
            std::wstring ta = a.title;
            std::wstring tb = b.title;
            std::transform(ta.begin(), ta.end(), ta.begin(), ::towlower);
            std::transform(tb.begin(), tb.end(), tb.begin(), ::towlower);
            return ta < tb;
        });

    // 3️⃣ Инициализация COM
    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr))
    {
        std::wcout << L"CoInitialize failed\n";
        return 1;
    }

    ITaskbarList3* pTaskbar = nullptr;
    hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pTaskbar));
    if (FAILED(hr) || !pTaskbar)
    {
        std::wcout << L"CoCreateInstance failed\n";
        CoUninitialize();
        return 1;
    }

    hr = pTaskbar->HrInit();
    if (FAILED(hr))
    {
        std::wcout << L"HrInit failed\n";
        pTaskbar->Release();
        CoUninitialize();
        return 1;
    }

    // 4️⃣ Перестроение
    std::wcout << L"Сортировка окон на панели задач...\n";
    for (const auto& wi : windows)
    {
        std::wcout << L"Обработка: " << wi.title.substr(0, 50) << L"...\n";

        pTaskbar->DeleteTab(wi.hwnd);
        pTaskbar->AddTab(wi.hwnd);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    pTaskbar->Release();
    CoUninitialize();

    std::wcout << L"Готово!\n";
    return 0;
}

} // namespace debug_func
