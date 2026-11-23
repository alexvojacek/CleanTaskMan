# Working on CleanTaskMan

The project aims to preserve the familiarity and simplicity of Windows 11 Task Manager with a responsive native interface. Start with the README and use the latest tagged release when comparing behavior. Record changes in the release notes so another contributor can resume from the repository alone.

- `src/main.cpp` owns windowing, rendering, and UI commands.
- `src/telemetry.*`, `src/activity.*`, `src/process_disk.*`, and `src/cpu_parking.*` collect native measurements.
- `src/settings.*` manages persistence. `src/integration.*` handles single-instance activation and the optional Windows Task Manager replacement.
- `tests/` includes Windows telemetry, settings, and private render fixtures. `installer/` holds the per-user setup definition.

Keep the first visible paint independent of hardware discovery. Preserve opaque native search editing, stable resize alignment, fixed GPU reading order, and placement-only exit writes. The fixture runner uses the workspace-local Zig toolchain under `.tools`. See `README.txt` for commands and what they verify. Format C++ using the included `.clang-format` configuration.

The next functional task is connecting Services Start and Stop. Run new task and startup item controls are also pending. If reporting a bug, include the Windows build, CleanTaskMan version, display scaling, the affected hardware when relevant, and the steps to reproduce it.
