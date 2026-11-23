$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$taskZig=Join-Path $taskRoot '.tools\zig-x86_64-windows-0.15.2\zig.exe'
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $taskRoot '.cache\zig-global'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $taskRoot '.cache\zig-local'
$env:ZIG_LIB_DIR=Join-Path (Split-Path $taskZig -Parent) 'lib'
New-Item -ItemType Directory -Force -Path (Join-Path $taskRoot 'build\disk-fix') | Out-Null
& $taskZig c++ -std=c++20 -O2 -DPROCESS_DISK_DIAGNOSTICS -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0a00 -Wall -Wextra (Join-Path $PSScriptRoot 'native_disk_probe.cpp') (Join-Path $taskRoot 'src\process_disk.cpp') -o (Join-Path $taskRoot 'build\disk-fix\NativeDiskProbe.exe') -ladvapi32
if($LASTEXITCODE -ne 0){throw 'Disk probe did not compile'}
