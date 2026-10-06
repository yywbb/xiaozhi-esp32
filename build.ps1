# build.ps1 - One-click build wrapper for xiaozhi-esp32 (Windows PowerShell)
#
# Usage:
#   .\build.ps1 bread-compact-wifi-s3cam          # build default variant
#   .\build.ps1 bread-compact-wifi-s3cam-dog      # build dog variant
#   .\build.ps1 bread-compact-wifi-s3cam -Flash   # build and flash ota_0
#
# Environment overrides (all optional, defaults are for this machine):
#   $env:IDF_PATH, $env:IDF_TOOLS_PATH,
#   $env:IDF_COMPONENT_CACHE_PATH, $env:CCACHE_DIR
#
# The script self-heals two environment problems that otherwise produce
# misleading errors:
#   - strips a shell-injected PYTHONPATH shim (would break asset packaging
#     with "FAILED: generated_assets.bin")
#   - restores sdkconfig from sdkconfig.old if a previous build was interrupted
#     (would break with "sdkconfig was not generated")

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [string]$Board,

    [string]$Variant,

    [switch]$Flash,

    [switch]$Clean,

    [string]$Port = "COM3",

    [int]$Baud = 230400
)

$ErrorActionPreference = "Stop"

# Native ESP-IDF tools log to stderr; treat those as non-terminating.
$script:OldEAP = $ErrorActionPreference

# ---------------------------------------------------------------------------
# Environment hygiene
# ---------------------------------------------------------------------------
# Two inherited-environment traps break this script with very misleading
# errors. Both are handled up front, before ESP-IDF is activated.

# (1) A shell-injected PYTHONPATH shim. Some terminals prepend a directory
#     containing a sitecustomize.py that guards bulk file deletion. ESP-IDF's
#     asset packer wipes build\temp_build, so the build dies with
#         [safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED]
#         FAILED: generated_assets.bin
#     which looks like a source-code bug but is not. Strip shim entries while
#     keeping anything else that was set on purpose.
if ($env:PYTHONPATH) {
    $originalPythonPath = $env:PYTHONPATH
    $keptEntries = @($originalPythonPath -split ';' |
        Where-Object { $_ -and ($_ -notmatch '[\\/]shim[\\/]*$') })
    if ($keptEntries.Count -gt 0) {
        $env:PYTHONPATH = ($keptEntries -join ';')
    }
    else {
        Remove-Item Env:\PYTHONPATH -ErrorAction SilentlyContinue
    }
    if ($env:PYTHONPATH -ne $originalPythonPath) {
        Write-Host "Stripped shell-injected PYTHONPATH shim: $originalPythonPath" -ForegroundColor Yellow
    }
}

# (2) Running from an MSYS/MinGW shell (Git Bash). ESP-IDF's idf.py refuses to
#     initialise there: it prints one "MSys/Mingw is not supported" line, exits
#     0 and does nothing -- after which build.py fails with the confusing
#     "Cannot validate board selection: sdkconfig was not generated".
#     Fail fast with an actionable message instead.
if ($env:MSYSTEM) {
    throw ("MSYSTEM='$($env:MSYSTEM)' detected - ESP-IDF's idf.py silently no-ops " +
        "under MSYS/MinGW (Git Bash), which surfaces later as 'sdkconfig was not generated'. " +
        "Run this script from PowerShell or cmd instead.")
}

# ---------------------------------------------------------------------------
# Resolve ESP-IDF environment paths (defaults for this workstation)
# ---------------------------------------------------------------------------
$script:IDF_PATH = $env:IDF_PATH
if (-not $script:IDF_PATH) { $script:IDF_PATH = "D:\Dev\Env\esp\esp-idf" }

$script:IDF_TOOLS_PATH = $env:IDF_TOOLS_PATH
if (-not $script:IDF_TOOLS_PATH) { $script:IDF_TOOLS_PATH = "D:\Dev\Env\esp\espressif" }

$script:IDF_COMPONENT_CACHE_PATH = $env:IDF_COMPONENT_CACHE_PATH
if (-not $script:IDF_COMPONENT_CACHE_PATH) { $script:IDF_COMPONENT_CACHE_PATH = "D:\Dev\Env\esp\component_cache" }

$script:CCACHE_DIR = $env:CCACHE_DIR
if (-not $script:CCACHE_DIR) { $script:CCACHE_DIR = "D:\Dev\Env\esp\ccache" }

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
if (-not (Test-Path "$script:IDF_PATH\tools\idf.py")) {
    throw "IDF_PATH invalid: $script:IDF_PATH (tools\idf.py not found)"
}
if (-not (Test-Path "$script:IDF_TOOLS_PATH\tools")) {
    throw "IDF_TOOLS_PATH invalid: $script:IDF_TOOLS_PATH (tools\ dir not found)"
}

# Locate the ESP-IDF python venv (build.py must run with idf-enabled python)
$venvRoot = Join-Path $script:IDF_TOOLS_PATH "python_env"
$venvPython = Get-ChildItem (Join-Path $venvRoot "*env*") -Recurse -Filter python.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $venvPython) {
    # Fallback: system python (works if idf-env already installed there)
    $venvPython = (Get-Command python -ErrorAction Stop).Source
    Write-Warning "No IDF python venv found under $venvRoot; falling back to $venvPython"
}

# ---------------------------------------------------------------------------
# Export environment and activate IDF
# ---------------------------------------------------------------------------
$env:IDF_PATH = $script:IDF_PATH
$env:IDF_TOOLS_PATH = $script:IDF_TOOLS_PATH
$env:IDF_COMPONENT_CACHE_PATH = $script:IDF_COMPONENT_CACHE_PATH
$env:CCACHE_DIR = $script:CCACHE_DIR

# Put the IDF venv python first on PATH so export.ps1's internal
# `python .../activate.py` call uses the correct interpreter.
$venvScripts = Split-Path $venvPython -Parent
$env:PATH = "$venvScripts;$env:PATH"

# Ensure ccache is findable before export runs
$ccacheDir = Get-ChildItem (Join-Path $script:IDF_TOOLS_PATH "tools\ccache") -Directory -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if ($ccacheDir) { $env:PATH = "$ccacheDir;$env:PATH" }

Write-Host "Activating ESP-IDF ..." -ForegroundColor Cyan
# ESP-IDF activate.py logs progress to stderr; don't let Stop mode abort on it.
$ErrorActionPreference = "Continue"
. "$script:IDF_PATH\export.ps1"
$ErrorActionPreference = $script:OldEAP

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
Push-Location $PSScriptRoot
try {
    # Clean stale ninja locks left by interrupted builds (causes
    # "ninja: error: failed recompaction: Permission denied").
    Get-Process ninja, ccache, cmake -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item build\.ninja_lock, build\.ninja_deps, build\.ninja_log -ErrorAction SilentlyContinue

    if ($Clean -and (Test-Path build)) {
        Write-Host "Cleaning build directory ..." -ForegroundColor Cyan
        Remove-Item build -Recurse -Force
    }

    # build.py skips re-configure when the target is unchanged, but still
    # requires sdkconfig to exist. ESP-IDF renames the old file to sdkconfig.old
    # before regenerating, so an interrupted build can leave the pair
    # "sdkconfig missing / sdkconfig.old present", which makes build.py fail
    # with "Cannot validate board selection: sdkconfig was not generated".
    if (-not (Test-Path sdkconfig) -and (Test-Path sdkconfig.old)) {
        Write-Host "Restoring sdkconfig from sdkconfig.old (previous build was interrupted) ..." -ForegroundColor Yellow
        Copy-Item sdkconfig.old sdkconfig -Force
    }

    $args = @("scripts\build.py", $Board)
    if ($Variant) { $args += @("--name", $Variant) }

    Write-Host "Building board '$Board' ..." -ForegroundColor Cyan
    # build.py / idf.py log to stderr; detect real failure via exit code.
    $ErrorActionPreference = "Continue"
    & $venvPython @args
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = $script:OldEAP
    if ($buildExit -ne 0) { throw "build.py exited with code $buildExit" }

    if ($Flash) {
        $appBin = "build\xiaozhi.bin"
        if (-not (Test-Path $appBin)) { throw "build\xiaozhi.bin not found" }
        Write-Host "Flashing $appBin to $Port @ ${Baud}baud ..." -ForegroundColor Cyan
        $ErrorActionPreference = "Continue"
        python -m esptool --chip esp32s3 -p $Port -b $Baud write-flash 0x20000 $appBin
        $flashExit = $LASTEXITCODE
        $ErrorActionPreference = $script:OldEAP
        if ($flashExit -ne 0) { throw "esptool exited with code $flashExit" }
    }
}
finally {
    Pop-Location
}

Write-Host "Done." -ForegroundColor Green
