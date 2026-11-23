# CleanTaskMan

CleanTaskMan is a lightweight native task manager for Windows 11. It keeps the clean look and familiar workflow of Windows Task Manager, from the process list to the Performance graphs. You can open it and go straight to the information you need.

We built it in C++ with Win32, Direct2D, and DirectWrite to draw directly with Windows APIs. The interface appears before background hardware collection starts. The app keeps its device column steady during resize and pauses collection while minimized. The goal is to preserve the feel of Windows 11 Task Manager and make common interactions feel immediate.

![CleanTaskMan Performance view showing GPU engine and memory graphs](docs/images/performance.png)

## Download

CleanTaskMan targets Windows 11 on x64. Both downloads contain the same application. No additional driver, service, or .NET runtime is required. [GitHub Releases](https://github.com/alexvojacek/CleanTaskMan/releases)

- **Installer.** Installs for the current Windows user and adds a Start menu entry. A desktop shortcut is optional. Uninstall from Settings under Installed apps.
- **Portable ZIP.** Extract the archive to a writable folder and run `CleanTaskMan.exe`.

The current binaries are unsigned. CleanTaskMan requests administrator access on launch by default. Decline the prompt to run with standard permissions, or turn off the request in Settings. The optional **Replace Task Manager** setting starts off. Turn it off before removing a copy that has it enabled.

Settings and window placement are stored in two small INI files beside the executable. Settings save when changed. On ordinary exit, the app writes only window placement if it has changed. The installer keeps these files during upgrades and uninstall.

## What works today

- Process overview and details, name and PID search, End task and Efficiency mode.
- Live CPU, memory, storage, network and GPU graphs. GPU views include engine usage, dedicated and shared memory, and temperature when the driver reports it.
- Services, startup and user session lists, along with settings for update speed and minimizing to the tray.

Run new task, Services Start and Stop, and startup item controls are still in development. Hardware readings depend on the Windows APIs and driver support available on your machine.

## Build from source

Use a Windows x64 C++20 toolchain. With Zig installed, run `.\build.ps1 -ZigPath <path-to-zig.exe>`. CMake project files are provided for Visual Studio. Build the optional installer with `.\build-installer.ps1 -IsccPath <path-to-ISCC.exe>` after building the application. Installer packaging uses [Inno Setup](https://jrsoftware.org/inno-setup).

See [README.txt](README.txt) for metric definitions, settings behavior, and verification commands. See [CONTRIBUTING.md](CONTRIBUTING.md) for project state and where to continue.

## Credits

Built by **Reed** and **Morgan** with direction and testing by **Alex**.

- **Reed** built the application and its native monitoring and interface.
- **Morgan** independently reviewed builds, tested Windows behavior, and checked regressions.
- **Alex** set the product direction, refined the design, and tested it on real hardware.

CleanTaskMan is available under the [MIT License](LICENSE).
