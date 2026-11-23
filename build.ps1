param(
    [string]$ZigPath,
    [switch]$Run,
    [string]$OutputDirectory = 'build'
)
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskBuild = Join-Path $taskRoot $OutputDirectory
$taskCache = Join-Path $taskRoot '.cache'
New-Item -ItemType Directory -Force -Path $taskBuild, $taskCache | Out-Null
if (-not $ZigPath) {
    if ($env:ZIG_EXE) { $ZigPath = $env:ZIG_EXE }
    elseif (Get-Command zig -ErrorAction SilentlyContinue) { $ZigPath = (Get-Command zig).Source }
    else {
        $taskLocalZig = Join-Path $taskRoot '.tools\zig-x86_64-windows-0.15.2\zig.exe'
        if (Test-Path -LiteralPath $taskLocalZig) { $ZigPath = $taskLocalZig }
        else { throw 'Pass -ZigPath <zig.exe>, set ZIG_EXE, or build with CMake and the Visual Studio C++ toolchain.' }
    }
}
$ZigPath = (Resolve-Path -LiteralPath $ZigPath).Path
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $taskCache 'zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $taskCache 'zig-local'
$env:ZIG_LIB_DIR = Join-Path (Split-Path $ZigPath -Parent) 'lib'
Push-Location (Join-Path $taskRoot 'resources')
try {
    & $ZigPath rc /fo (Join-Path $taskBuild 'app.res') app.rc
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
} finally { Pop-Location }
$taskArgs = @(
    'c++', '-std=c++20', '-O2', '-DNDEBUG', '-DUNICODE', '-D_UNICODE',
    '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0a00', '-DWINVER=0x0a00', '-Wall', '-Wextra',
    '-municode', '-Wl,--subsystem,windows', (Join-Path $taskRoot 'src\main.cpp'),
    (Join-Path $taskRoot 'src\telemetry.cpp'),
    (Join-Path $taskRoot 'src\settings.cpp'),
    (Join-Path $taskRoot 'src\process_actions.cpp'),
    (Join-Path $taskRoot 'src\activity.cpp'),
    (Join-Path $taskRoot 'src\process_disk.cpp'),
    (Join-Path $taskRoot 'src\cpu_parking.cpp'),
    (Join-Path $taskRoot 'src\integration.cpp'),
    (Join-Path $taskBuild 'app.res'),
    '-o', (Join-Path $taskBuild 'CleanTaskMan.exe'),
    '-ld2d1', '-ldwrite', '-ldwmapi', '-luser32', '-lgdi32',
    '-lcomctl32', '-lole32', '-lshell32', '-ladvapi32', '-lpsapi', '-lpowrprof',
    '-lpdh', '-liphlpapi', '-ldxgi', '-lwtsapi32', '-lsetupapi', '-lws2_32', '-ltdh'
)
& $ZigPath @taskArgs
if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed.' }
Get-Item -LiteralPath (Join-Path $taskBuild 'CleanTaskMan.exe') | Select-Object FullName, Length
if ($Run) { Start-Process -FilePath (Join-Path $taskBuild 'CleanTaskMan.exe') -WindowStyle Hidden }
