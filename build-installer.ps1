param(
    [string]$IsccPath,
    [string]$Executable = 'build\CleanTaskMan.exe',
    [string]$OutputDirectory = 'build\installer'
)

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not $IsccPath) {
        $IsccPath = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
    }
    if (-not $IsccPath) {
        throw 'Install Inno Setup and pass -IsccPath with the path to ISCC.exe.'
    }

    $compiler = (Resolve-Path -LiteralPath $IsccPath).Path
    $app = Get-Item -LiteralPath $Executable
    $version = $app.VersionInfo.FileVersion
    if ($app.Name -ne 'CleanTaskMan.exe' -or $version -notmatch '^\d+\.\d+\.\d+$') {
        throw 'Pass a built, versioned CleanTaskMan.exe to -Executable.'
    }

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $output = (Resolve-Path -LiteralPath $OutputDirectory).Path
    $readme = Join-Path $PSScriptRoot 'README.txt'
    $license = Join-Path $PSScriptRoot 'LICENSE'
    $script = Join-Path $PSScriptRoot 'installer\CleanTaskMan.iss'
    & $compiler "/DAppVersion=$version" "/DAppExecutable=$($app.FullName)" "/DAppReadme=$readme" "/DAppLicense=$license" "/O$output" $script
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup compilation failed with exit code $LASTEXITCODE."
    }

    Get-Item -LiteralPath (Join-Path $output "CleanTaskMan-$version-win-x64-setup.exe")
} finally {
    Pop-Location
}
