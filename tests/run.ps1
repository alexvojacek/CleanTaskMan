$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskZig = Join-Path $taskRoot '.tools\zig-x86_64-windows-0.15.2\zig.exe'
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $taskRoot '.cache\zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $taskRoot '.cache\zig-local'
$env:ZIG_LIB_DIR = Join-Path (Split-Path $taskZig -Parent) 'lib'
$taskOutput = Join-Path $taskRoot 'build\native\telemetry-tests.exe'
& $taskZig c++ -std=c++20 -O2 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0a00 -Wall -Wextra (Join-Path $PSScriptRoot 'telemetry_tests.cpp') (Join-Path $taskRoot 'src\telemetry.cpp') (Join-Path $taskRoot 'src\activity.cpp') (Join-Path $taskRoot 'src\process_disk.cpp') (Join-Path $taskRoot 'src\cpu_parking.cpp') -o $taskOutput -lpsapi -lpowrprof -lpdh -liphlpapi -ldxgi -lwtsapi32 -lole32 -ladvapi32 -luser32 -lgdi32 -lsetupapi -lws2_32 -ltdh
if ($LASTEXITCODE -ne 0) { throw 'Telemetry test compilation failed.' }
& $taskOutput
if ($LASTEXITCODE -ne 0) { throw 'Telemetry tests failed.' }
