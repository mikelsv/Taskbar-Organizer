import win32gui
import win32process
import psutil

def get_windows_by_process_name(process_name):
    # 1. Находим PID процесса по имени
    pids = [p.info['pid'] for p in psutil.process_iter(['pid', 'name'])
            if p.info['name'] == process_name]

    if not pids:
        return []

    windows = []

    # 2. Функция обратного вызова для перебора окон
    def callback(hwnd, extra):
        # Получаем PID окна
        _, win_pid = win32process.GetWindowThreadProcessId(hwnd)
        if win_pid in pids and win32gui.IsWindowVisible(hwnd):
            title = win32gui.GetWindowText(hwnd)
            if title:  # Отсеиваем пустые заголовки
                windows.append((hwnd, title))
        return True

    # 3. Запуск перебора всех верхнеуровневых окон
    win32gui.EnumWindows(callback, None)
    return windows

# Использование
# res = get_windows_by_process_name("firefox.exe")
# for hwnd, title in res:
#     print(f"HWND: {hwnd} | Title: {title}")



"""
### Нюансы
/*
*   **Видимость:** Процесс может иметь множество служебных невидимых окон. Функция `IsWindowVisible` помогает найти только те, что видит пользователь.
*   **Доступ:** Для некоторых системных процессов могут потребоваться права администратора.
*   **Альтернатива:** Библиотека `pyvda` удобнее, если вам нужны только главные окна приложений на рабочем столе.
"""


from pywinauto import Desktop

def get_firefox_taskbar_order():
    desktop = Desktop(backend="uia")

    # 1. Находим панель задач по классу (Shell_TrayWnd — стандарт для всех Windows)
    taskbar = desktop.window(class_name="Shell_TrayWnd")

    # 2. Ищем внутри неё список запущенных приложений по классу 'MSTaskListWClass'
    # Это работает надежнее, чем поиск по текстовому заголовку
    try:
        app_list = taskbar.child_window(class_name="MSTaskListWClass")
    except Exception:
        # Если класс не найден, попробуем найти через глубинный поиск первой попавшейся панели инструментов
        print("MSTaskListWClass не найден.")
        app_list = taskbar.descendants(control_type="ToolBar")[0]

    firefox_windows = []

    # 3. Перебираем детей (кнопки на панели)
    for btn in app_list.children():
        name = btn.window_text()
        # Проверяем наличие Firefox в названии кнопки
        if name and "Firefox" in name:
            firefox_windows.append(name)

    return firefox_windows

try:
    print("Поиск окон Firefox на панели задач...")
    results = get_firefox_taskbar_order()
    if not results:
        print("Окна не найдены (возможно, они сгруппированы в одну иконку без текста).")
    else:
        for i, title in enumerate(results, 1):
            print(f"{i}. {title}")
except Exception as e:
    print(f"Ошибка при поиске: {e}")


"""
import ctypes
from ctypes import wintypes
import psutil
import win32gui
import win32process

def reorder_windows():
    # 1. Сбор окон Firefox
    firefox_pids = [p.info['pid'] for p in psutil.process_iter(['pid', 'name'])
                    if p.info['name'] == "firefox.exe"]
    hwnds = []
    win32gui.EnumWindows(lambda h, _: (hwnds.append(h) if win32gui.IsWindowVisible(h) and
                         win32process.GetWindowThreadProcessId(h)[1] in firefox_pids and
                         win32gui.GetWindowText(h) else None), None)

    if len(hwnds) < 2:
        print("Нужно хотя бы 2 окна.")
        return

    # Меняем два последних окна местами в списке
    hwnds[-1], hwnds[-2] = hwnds[-2], hwnds[-1]

    # 2. Подготовка COM интерфейса ITaskbarList3
    ole32 = ctypes.oledll.ole32
    ole32.CoInitialize(None)

    CLSID_TaskbarList = '{56FDF344-FD6D-11d0-958A-006097C9A090}'
    IID_ITaskbarList3 = '{EA1AFB91-9E28-4B86-90E9-9E9F8A5EEFAF}'

    def get_guid(s):
        g = (ctypes.c_ubyte * 16)()
        ole32.IIDFromString(ctypes.c_wchar_p(s), g)
        return g

    clsid = get_guid(CLSID_TaskbarList)
    iid = get_guid(IID_ITaskbarList3)
    tbl = ctypes.c_void_p()

    if ole32.CoCreateInstance(ctypes.byref(clsid), None, 1, ctypes.byref(iid), ctypes.byref(tbl)) != 0:
        print("Ошибка COM")
        return

    # Определяем прототипы функций для vtable
    # Index 3: HrInit(), 4: AddTab(HWND), 5: DeleteTab(HWND)
    vtable = ctypes.cast(tbl, ctypes.POINTER(ctypes.c_void_p)).contents

    def get_method(index):
        proto = ctypes.WINFUNCTYPE(ctypes.HRESULT, ctypes.c_void_p, wintypes.HWND)
        return proto(ctypes.cast(vtable, ctypes.POINTER(ctypes.c_void_p))[index])

    hr_init = ctypes.WINFUNCTYPE(ctypes.HRESULT, ctypes.c_void_p)(
        ctypes.cast(vtable, ctypes.POINTER(ctypes.c_void_p))[3]
    )
    add_tab = get_method(4)
    del_tab = get_method(5)

    # 3. Выполнение
    hr_init(tbl)
    print("Перестраиваем панель задач...")

    for hwnd in hwnds:
        del_tab(tbl, hwnd)
        add_tab(tbl, hwnd)

    ole32.CoUninitialize()
    print("Готово!")

if __name__ == "__main__":
    reorder_windows()

"""