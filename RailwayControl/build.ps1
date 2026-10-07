# ============================================================================
#  build.ps1 - Build RailControl and place the executables in output/.
#
#  The Makefile is the primary build; this script exists because `make` is not
#  always present on Windows, and because copying the finished binaries into
#  one predictable folder is a step worth automating rather than doing by hand.
#
#  Usage:
#     .\build.ps1                 build everything into output/
#     .\build.ps1 -Console        only rcp.exe
#     .\build.ps1 -Gui            only rcp-gui.exe
#     .\build.ps1 -Tools          only names_tool.exe and auth_console.exe
#     .\build.ps1 -Clean          delete build/ and output/*.exe first
#     .\build.ps1 -Optimise Debug build with symbols, no optimisations
#
#  What it does:
#     1. compiles every engine translation unit into build/obj
#     2. links the front ends against those objects
#     3. copies the finished binaries into output/
#
#  Objects go to build/, binaries to output/. Nothing intermediate is left in
#  output/ - everything there is a finished, runnable executable.
# ============================================================================

[CmdletBinding()]
param(
    [switch]$Console,
    [switch]$Gui,
    [switch]$Tools,
    [switch]$Clean,
    [ValidateSet('Release', 'Debug')]
    [string]$Optimise = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

# --- Toolchain ---------------------------------------------------------------
# Prefer gcc; fall back to clang. Both are MinGW targets, so the platform
# libraries below are the Windows ones.
$cc = $null
foreach ($candidate in @('gcc', 'clang')) {
    $found = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($found) { $cc = $found.Source; break }
}
if (-not $cc) {
    Write-Error "No C compiler found. Install MinGW-w64 or LLVM-MinGW and ensure gcc or clang is on PATH."
    exit 1
}

$std    = '-std=c17'
$warn   = @('-Wall', '-Wextra', '-Wshadow')
$opt    = if ($Optimise -eq 'Debug') { @('-O0', '-g') } else { @('-O2') }
$inc    = '-Iinclude'

# Static runtime: the produced .exe must run without MinGW DLLs beside it.
# -static-libstdc++ is deliberately not passed: this is a C project, the flag is
# inert, and clang warns about it on every link.
$linkCommon = @('-static', '-static-libgcc')

$platformLibs = @('-lwinmm', '-lws2_32')
$guiLibs      = @('-lgdi32', '-luser32', '-lcomdlg32', '-lshell32')

# --- Directories -------------------------------------------------------------
$objDir = Join-Path $root 'build\obj'
$outDir = Join-Path $root 'output'

if ($Clean) {
    Write-Host "Cleaning..." -ForegroundColor Yellow
    Remove-Item (Join-Path $root 'build') -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem $outDir -Filter '*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force
}

New-Item -ItemType Directory -Path $objDir -Force | Out-Null
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# Capture the build banner that goes into every binary, so a shipped exe can
# say when it was made.
#
# The macro must reach the compiler as a QUOTED C STRING, because it is pasted
# into string literals (RC_VERSION_FULL, the log header). Two details matter:
#
#   1. The quoting is built by concatenation, not backtick escapes - nesting
#      escapes inside an interpolated string silently produced ""20261007"".
#   2. The quotes are BACKSLASH-escaped. PowerShell strips quotes when it
#      splats an argument array to a native command, so "20261007" arrives at
#      gcc as a bare 20261007 unless the quotes are escaped here.
$buildStamp  = (Get-Date).ToUniversalTime().ToString("yyyyMMdd")
$buildDefine = '-DRC_VERSION_BUILD=\"' + $buildStamp + '\"'

# --- Source sets -------------------------------------------------------------
# The shared engine. win32_* files are the GUI only and main.c has its own
# entry point used by the console link, so both are excluded here.
$engineSources = Get-ChildItem (Join-Path $root 'src\*.c') |
    Where-Object { $_.Name -ne 'main.c' -and $_.Name -notlike 'win32_*' } |
    Sort-Object Name

$guiSources = @(
    'src\win32_entry.c', 'src\win32_main.c', 'src\win32_panel.c',
    'src\win32_status.c', 'src\win32_terminal.c', 'src\win32_actions.c'
)

if ($engineSources.Count -eq 0) {
    Write-Error "No engine sources found in src\."
    exit 1
}

# --- Compile the engine once -------------------------------------------------
# Every front end links these same objects, so there is one implementation of
# the railway and several ways to drive it. Compiling them once rather than per
# target is what keeps the build honest - a front end that failed to compile
# against the engine would fail here, not silently build its own copy.
Write-Host ""
Write-Host "Compiling engine ($($engineSources.Count) translation units)..." -ForegroundColor Cyan

$engineObjects = @()
foreach ($src in $engineSources) {
    $obj = Join-Path $objDir ($src.BaseName + '.o')
    $engineObjects += $obj

    # Recompile only when the source is newer than its object.
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -gt $src.LastWriteTime)) {
        continue
    }

    $compileArgs = @($std) + $warn + $opt + @($inc, $buildDefine, '-c', $src.FullName, '-o', $obj)
    & $cc @compileArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to compile $($src.Name)"
        exit 1
    }
}
Write-Host "  engine objects up to date" -ForegroundColor DarkGray

# --- Link helper -------------------------------------------------------------
$built = @()

function Invoke-Link {
    param(
        [string]   $Name,
        [string[]] $Sources,
        [string[]] $Libs,
        [switch]   $Windowed
    )

    $out = Join-Path $outDir "$Name.exe"
    Write-Host "Linking $Name.exe ..." -ForegroundColor Cyan

    $extra = @()
    if ($Windowed) {
        # -mwindows  -> WINDOWS subsystem, no console window
        # -municode  -> the wWinMain entry point
        $extra = @('-mwindows', '-municode')
    }

    $sourcePaths = $Sources | ForEach-Object { Join-Path $root $_ }

    $linkArgs = @($std) + $opt + $extra + @($inc, $buildDefine) +
            $sourcePaths + $engineObjects + @('-o', $out) +
            $linkCommon + $Libs

    & $cc @linkArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to link $Name.exe"
        exit 1
    }

    $size = [math]::Round((Get-Item $out).Length / 1KB)
    Write-Host "  -> output\$Name.exe  ($size KB)" -ForegroundColor Green
    $script:built += $Name
}

# --- Node project ------------------------------------------------------------
# The repository also carries a Node front end. It is not compiled, but the
# script reports its test status so a distribution build notices a regression
# in the other half of the tree.
function Test-NodeProject {
    $pkg = Join-Path (Split-Path $root -Parent) 'package.json'
    if (-not (Test-Path $pkg)) { return }

    $nodeModules = Join-Path (Split-Path $root -Parent) 'node_modules'
    if (-not (Test-Path $nodeModules)) {
        Write-Host "Node: dependencies not installed (run npm install)" -ForegroundColor DarkYellow
        return
    }
}

# --- Build selection ---------------------------------------------------------
$doAll = -not ($Console -or $Gui -or $Tools)

if ($doAll -or $Console) {
    Invoke-Link -Name 'rcp' -Sources @('src\main.c') -Libs $platformLibs
}

if ($doAll -or $Gui) {
    Invoke-Link -Name 'rcp-gui' -Sources $guiSources -Libs ($platformLibs + $guiLibs) -Windowed
}

if ($doAll -or $Tools) {
    Invoke-Link -Name 'names_tool'   -Sources @('tools\names_tool.c')   -Libs $platformLibs
    Invoke-Link -Name 'auth_console' -Sources @('tools\auth_console.c') -Libs $platformLibs
}

# --- Report ------------------------------------------------------------------
Write-Host ""
Write-Host "Build complete ($Optimise). Binaries in output\:" -ForegroundColor Green
Get-ChildItem $outDir -Filter '*.exe' |
    Sort-Object Name |
    Select-Object Name, @{Name = 'KB'; Expression = { [math]::Round($_.Length / 1KB) } }, LastWriteTime |
    Format-Table -AutoSize | Out-String | Write-Host

Write-Host "Verify with:" -ForegroundColor DarkGray
Write-Host "  .\output\rcp.exe --version" -ForegroundColor DarkGray
Write-Host "  .\output\names_tool.exe --check" -ForegroundColor DarkGray
Write-Host ""
Write-Host "These are SIMULATOR binaries - not certified for real signalling." -ForegroundColor DarkYellow

Test-NodeProject
