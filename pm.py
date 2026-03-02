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
import time

def sort_firefox_by_title():
    # 1. Сбор окон
    firefox_pids = [p.info['pid'] for p in psutil.process_iter(['pid', 'name'])
                    if p.info['name'] == "firefox.exe"]

    window_data = []
    def enum_cb(hwnd, _):
        if win32gui.IsWindowVisible(hwnd):
            _, pid = win32process.GetWindowThreadProcessId(hwnd)
            if pid in firefox_pids:
                title = win32gui.GetWindowText(hwnd)
                if title: window_data.append((hwnd, title))
        return True

    win32gui.EnumWindows(enum_cb, None)
    if not window_data: return

    # 2. Сортировка по заголовку (второй элемент кортежа)
    window_data.sort(key=lambda x: x[1].lower())

    # 3. Инициализация COM
    ole32 = ctypes.oledll.ole32
    ole32.CoInitialize(None)

    CLSID_Tbl = '{56FDF344-FD6D-11D0-958A-006097C9A090}'
    IID_ITbl3 = '{EA1AFB91-9E28-4B86-90E9-9E9F8A5EEFAF}'

    def get_guid(s):
        g = (ctypes.c_ubyte * 16)()
        ole32.IIDFromString(ctypes.c_wchar_p(s), g)
        return g

    clsid, iid = get_guid(CLSID_Tbl), get_guid(IID_ITbl3)
    tbl = ctypes.c_void_p()
    ole32.CoCreateInstance(ctypes.byref(clsid), None, 1, ctypes.byref(iid), ctypes.byref(tbl))

    # Получаем таблицу виртуальных функций (vtable)
    vtable_ptr = ctypes.cast(tbl, ctypes.POINTER(ctypes.c_void_p)).contents
    vtable = ctypes.cast(vtable_ptr, ctypes.POINTER(ctypes.c_void_p))

    # Определяем методы: 3-Init, 4-Add, 5-Delete
    def get_func(idx, arg_type=None):
        if arg_type:
            proto = ctypes.WINFUNCTYPE(ctypes.HRESULT, ctypes.c_void_p, arg_type)
        else:
            proto = ctypes.WINFUNCTYPE(ctypes.HRESULT, ctypes.c_void_p)
        return proto(vtable[idx])

    hr_init = get_func(3)
    add_tab = get_func(4, wintypes.HWND)
    del_tab = get_func(5, wintypes.HWND)

    hr_init(tbl)

    # 4. Перестроение
    print("Сортировка окон на панели задач...")
    for hwnd, title in window_data:
        print(f"Обработка: {title[:50]}...")
        del_tab(tbl, hwnd)
        add_tab(tbl, hwnd)
        time.sleep(0.05) # Пауза для Explorer

    ole32.CoUninitialize()
    print("Готово!")

if __name__ == "__main__":
    sort_firefox_by_title()
