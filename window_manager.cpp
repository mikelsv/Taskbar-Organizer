#include "window_manager.h"

#include <CommCtrl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>    // для freopen_s
#include <fcntl.h>   // для _O_U16TEXT
#include <io.h>      // для _setmode и _fileno
#include <clocale>   // для setlocale и LC_ALL

#include <shobjidl.h>  // ITaskbarList3
#include <uiautomation.h>
#include <tlhelp32.h>

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

    // On Windows 10 taskbar, TBBUTTON::dwData points to an Explorer-owned structure.
    // Probe its first bytes and treat pointer-sized fields as potential HWND candidates.
    std::uintptr_t bytes[8]{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(explorerProcess, reinterpret_cast<LPCVOID>(button.dwData), bytes, sizeof(bytes), &read) ||
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

std::vector<HWND> GetTaskbarButtonOrder(bool* succeeded) {
    if (succeeded) {
        *succeeded = false;
    }

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

    // TB_BUTTONCOUNT returns how many taskbar buttons are in the toolbar.
    const LRESULT buttonCount = SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0);
    if (buttonCount <= 0) {
        CloseHandle(explorerProcess);
        return ordered;
    }

    ordered.reserve(static_cast<size_t>(buttonCount));

    for (int i = 0; i < buttonCount; ++i) {
        // TB_GETBUTTON writes into memory of the toolbar owner process (explorer.exe),
        // so we must allocate a TBBUTTON buffer in explorer using VirtualAllocEx.
        LPVOID remoteButton = VirtualAllocEx(explorerProcess, nullptr, sizeof(TBBUTTON), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteButton) {
            ordered.clear();
            CloseHandle(explorerProcess);
            return ordered;
        }

        // TB_GETBUTTON fills a TBBUTTON at the remote pointer for the current index.
        if (!SendMessageW(toolbar, TB_GETBUTTON, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(remoteButton))) {
            VirtualFreeEx(explorerProcess, remoteButton, 0, MEM_RELEASE);
            ordered.clear();
            CloseHandle(explorerProcess);
            return ordered;
        }

        TBBUTTON localButton{};
        SIZE_T read = 0;
        if (!ReadProcessMemory(explorerProcess, remoteButton, &localButton, sizeof(localButton), &read) || read != sizeof(localButton)) {
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

    if (succeeded) {
        *succeeded = !ordered.empty();
    }
    return ordered;
}

std::vector<WindowItem> EnumerateWindowsForProcessByTaskbarOrder(DWORD pid, bool allowTaskbarSync) {
    auto fallback = EnumerateWindowsForProcess(pid);
    if (!allowTaskbarSync || pid == 0) {
        return fallback;
    }

    bool gotTaskbar = false;
    const auto taskbarOrder = GetTaskbarButtonOrder(&gotTaskbar);
    if (!gotTaskbar || taskbarOrder.empty()) {
        return fallback;
    }

    std::vector<WindowItem> ordered;
    ordered.reserve(fallback.size());

    for (HWND hwnd : taskbarOrder) {
        if (!IsWindow(hwnd) || !IsEligibleTopLevelWindow(hwnd)) {
            continue;
        }

        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid != pid) {
            continue;
        }

        auto it = std::find_if(fallback.begin(), fallback.end(), [hwnd](const WindowItem& item) {
            return item.hwnd == hwnd;
        });
        if (it != fallback.end() &&
            std::none_of(ordered.begin(), ordered.end(), [hwnd](const WindowItem& item) { return item.hwnd == hwnd; })) {
            ordered.push_back(*it);
        }
    }

    for (const auto& item : fallback) {
        if (std::none_of(ordered.begin(), ordered.end(), [h = item.hwnd](const WindowItem& current) { return current.hwnd == h; })) {
            ordered.push_back(item);
        }
    }

    return ordered;
}


std::vector<DWORD> GetProcessIdsByName(const std::wstring& processName)
{
    std::vector<DWORD> pids;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return pids;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (Process32First(snapshot, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0)
            {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return pids;
}

BOOL CALLBACK EnumWindowsProcGetHwndByIds(HWND hwnd, LPARAM lParam){
    EnumData* data = reinterpret_cast<EnumData*>(lParam);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    // Проверяем, принадлежит ли окно нужному процессу
    if (std::find(data->targetPids->begin(),
        data->targetPids->end(),
        pid) == data->targetPids->end())
        return TRUE;

    // Фильтрация "нормальных" окон
    if (!IsWindowVisible(hwnd))
        return TRUE;

    if (GetWindow(hwnd, GW_OWNER) != nullptr) // исключаем дочерние/всплывающие
        return TRUE;

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if (!(style & WS_OVERLAPPEDWINDOW))
        return TRUE;

    wchar_t title[512];
    GetWindowText(hwnd, title, 512);

    if (wcslen(title) == 0)
        return TRUE;

    data->result->push_back({ hwnd, title });

    return TRUE;
}

std::vector<WindowItem> GetWindowsForProcesses(
    const std::vector<DWORD>& targetPids)
{
    std::vector<WindowItem> result;

    EnumData data;
    data.targetPids = &targetPids;
    data.result = &result;

    EnumWindows(EnumWindowsProcGetHwndByIds, reinterpret_cast<LPARAM>(&data));

    return result;
}

std::vector<WindowItem> EnumerateWindowsForProcessByTaskbarOrder(std::wstring processName){
    auto targetPids = GetProcessIdsByName(processName);
    auto windows = GetWindowsForProcesses(targetPids);
    std::vector<WindowItem> result;

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
                // 2️⃣ Получаем имя кнопки панели задач
                BSTR name = nullptr;
                std::wstring taskbarTitle;

                if (SUCCEEDED(child->get_CurrentName(&name)) && name)
                {
                    taskbarTitle = name;
                    SysFreeString(name);
                }

                if (taskbarTitle.empty())
                {
                    child->Release();
                    continue;
                }

                // 3️⃣ Ищем соответствующее реальное окно
                auto it = std::find_if(
                    windows.begin(),
                    windows.end(),
                    [&](const WindowItem& w)
                    {
                        // Сравнение с учётом того,
                        // что в taskbarTitle может быть дописан текст
                        return taskbarTitle.find(w.title) != std::wstring::npos;
                    });

                if (it != windows.end())
                {
                    result.push_back(*it);
                    windows.erase(it); // чтобы не матчить повторно
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


bool SwapWindowsByIndex(std::vector<WindowItem>& windows, int firstIndex, int secondIndex) {
    if (firstIndex < 0 || secondIndex < 0 ||
        firstIndex >= static_cast<int>(windows.size()) ||
        secondIndex >= static_cast<int>(windows.size()) ||
        firstIndex == secondIndex) {
        return false;
    }

    std::iter_swap(windows.begin() + firstIndex, windows.begin() + secondIndex);
    return true;
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

void ApplyTaskbarOrder(const std::vector<WindowItem>& windows) {
    if (windows.empty()) {
        return;
    }

    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        return;
    }

    ITaskbarList3* pTaskbar = nullptr;
    hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pTaskbar));
    if (FAILED(hr) || !pTaskbar) {
        CoUninitialize();
        return;
    }

    hr = pTaskbar->HrInit();
    if (FAILED(hr)) {
        pTaskbar->Release();
        CoUninitialize();
        return;
    }

    for (const auto& item : windows) {
        if (!IsWindow(item.hwnd)) {
            continue;
        }

        pTaskbar->DeleteTab(item.hwnd);
        pTaskbar->AddTab(item.hwnd);

        Sleep(100);  // Пауза 100мс чтобы окна успели отобразиться в нужной последовательности
    }

    pTaskbar->Release();
    CoUninitialize();
}

// Универсальная замена wprintf
int console_log(const wchar_t* format, ...) {
    wchar_t buffer[1024]; // Буфер для текста
    va_list args;
    va_start(args, format);
    int result = vswprintf_s(buffer, format, args);
    va_end(args);

    if (result > 0) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written;
        WriteConsoleW(hOut, buffer, (DWORD)result, &written, NULL);
    }
    return result;
}

// Маскируем под wprintf (теперь можно просто сменить название или оставить это)
#define wprintf console_log

// Тестовая функция для отладки - выводит список кнопок на панели задач
void DebugPrintTaskbarButtons() {
    // Настраиваем консоль для вывода Unicode
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    //CreateDebugConsole();

    wprintf(L"=== Тест получения кнопок панели задач ===\n\n");

    // 1. Находим Shell_TrayWnd
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        wprintf(L"Ошибка: Shell_TrayWnd не найден\n");
        return;
    }
    wprintf(L"Shell_TrayWnd найден: %p\n\n", tray);

    // 2. Ищем MSTaskListWClass внутри Shell_TrayWnd
    HWND taskList = FindWindowExW(tray, nullptr, L"MSTaskListWClass", nullptr);
    if (taskList) {
        wprintf(L"MSTaskListWClass найден: %p\n", taskList);
    } else {
        wprintf(L"MSTaskListWClass не найден, пробуем MSTaskSwWClass...\n");
        HWND rebar = FindWindowExW(tray, nullptr, L"ReBarWindow32", nullptr);
        if (rebar) {
            wprintf(L"ReBarWindow32 найден: %p\n", rebar);
            taskList = FindWindowExW(rebar, nullptr, L"MSTaskSwWClass", nullptr);
            if (taskList) {
                wprintf(L"MSTaskSwWClass найден: %p\n", taskList);
            }
        }
    }

    if (!taskList) {
        wprintf(L"Ошибка: список задач не найден\n");
        return;
    }

    // 3. Перебираем дочерние окна
    struct ButtonInfo {
        HWND hwnd;
        std::wstring text;
    };
    std::vector<ButtonInfo> buttons;

    auto enumChildProc = [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* buttons = reinterpret_cast<std::vector<ButtonInfo>*>(lParam);
        if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
            return TRUE;
        }

        int length = GetWindowTextLengthW(hwnd);
        if (length > 0) {
            std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(hwnd, buffer.data(), length + 1);
            buffer.resize(static_cast<size_t>(length));

            if (!buffer.empty()) {
                buttons->push_back({hwnd, buffer});
                wprintf(L"HWND: %p | Text: %s\n", hwnd, buffer.c_str());
            }
        }
        return TRUE;
    };

    wprintf(L"\n=== Кнопки на панели задач ===\n");
    EnumChildWindows(taskList, enumChildProc, reinterpret_cast<LPARAM>(&buttons));

    wprintf(L"\n=== Всего найдено кнопок: %zu ===\n", buttons.size());
    return;
}

}  // namespace window_manager
