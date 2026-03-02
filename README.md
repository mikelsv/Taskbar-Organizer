# Taskbar Organizer

A Windows desktop application that provides advanced control over the order of taskbar buttons. Reorder windows of specific applications on your Windows taskbar with ease.

## Features

- **Process Selection**: Choose any running process to manage its windows on the taskbar
- **Window Reordering**: Drag and drop windows in the list to change their order on the taskbar
- **Manual Reorder Mode**: Enable manual dragging of items to reorder them
- **Apply on Fly**: Automatically apply changes as you reorder windows
- **Manual Apply**: Apply changes manually using the Apply button
- **Taskbar Sync**: Synchronize window order with taskbar in real-time

## Requirements

- Windows 10 or later
- Visual Studio 2019 or later (for building from source)
- Windows SDK

## Building

1. Open `Taskbar Organizer.sln` in Visual Studio
2. Select your target configuration (Debug/Release)
3. Build the solution (Ctrl+Shift+B)
4. Run the executable from the output directory

## Usage

1. Launch Taskbar Organizer
2. Select a process from the dropdown (e.g., Firefox, Chrome, Explorer)
3. The list will show all windows of that process on the taskbar
4. Enable "Manual Reorder" to drag and drop items in the list
5. Use "Up" and "Down" buttons to move selected windows
6. Click "Apply" to apply changes to the taskbar, or enable "Apply on Fly" for automatic application

## Architecture

The application consists of several key components:

- **main.cpp**: Application entry point and window class registration
- **ui.cpp/ui.h**: User interface implementation with ListView controls
- **window_manager.cpp/window_manager.h**: Taskbar button manipulation and window enumeration
- **process_manager.cpp/process_manager.h**: Process enumeration with visible windows
- **debug_func.cpp/debug_func.h**: Debug utilities

## Technical Details

- Built with native Win32 API
- Uses ListView control (common controls)
- Direct manipulation of Windows taskbar button order via COM interfaces
- Supports both manual and automatic reordering modes

## License

MIT License
