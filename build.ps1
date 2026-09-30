# Build Strikers-WiiCompiled from your own extracted copy of Mario Strikers Charged (Windows).
#
#   .\build.cmd "C:\path\to\extracted\game"     # full build
#   .\build.cmd -SkipTranslate                  # recompile the runtime only
#
# The game folder is the one you extracted the disc into (contains sys\ and files\, or DATA\).
# Everything this produces (Assets\, generated\, build-windows\) stays on your machine and is gitignored.
param(
    [Parameter(Position = 0)] [string] $Disc = "",
    [switch] $SkipTranslate,
    [int] $Jobs = [Environment]::ProcessorCount
)
$ErrorActionPreference = 'Stop'

$Repo = $PSScriptRoot
$Project = 'projects/mscharged/recomp.yml'
$TranslatorDll = 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
$BuildDir = Join-Path $Repo 'build-windows'
$AppDir = Join-Path $env:LOCALAPPDATA 'MSCRecomp'
$ExeName = 'Strikers-WiiCompiled.exe'
Set-Location $Repo

function Fail([string] $Message) { Write-Host "`nerror: $Message" -ForegroundColor Red; exit 1 }
function Step([string] $Message) { Write-Host "`n==> $Message" -ForegroundColor Cyan }
function Need([string] $Tool, [string] $Hint) {
    $cmd = Get-Command $Tool -ErrorAction SilentlyContinue
    if (-not $cmd) { Fail "$Tool not found. $Hint" }
    return $cmd.Source
}
# Runs a command with output going to a log; shows the log's tail on failure.
function Logged([string] $Log, [string] $Exe, [string[]] $Arguments) {
    $path = Join-Path $BuildDir $Log
    # Windows PowerShell turns redirected native stderr into errors; don't let that abort the build.
    $ErrorActionPreference = 'Continue'
    & $Exe @Arguments *> $path
    if ($LASTEXITCODE -ne 0) {
        Get-Content $path -Tail 25 | ForEach-Object { Write-Host "    | $_" }
        Fail "step failed (full log: $path)"
    }
}

$dotnet = Need 'dotnet' 'Install the .NET 8 SDK.'
$cmake = Need 'cmake' 'Install CMake 3.25 or newer.'
$ninja = Need 'ninja' 'Install Ninja.'
$clang = Need 'clang' 'Install LLVM-MinGW and put its bin folder first on PATH (see README).'
$clangxx = Need 'clang++' 'Install LLVM-MinGW and put its bin folder first on PATH (see README).'
$clangVersion = (& $clangxx --version) -join "`n"
if ($clangVersion -notmatch 'windows-gnu|mingw') {
    Fail "$clangxx is not an LLVM-MinGW Clang (its target should be x86_64-w64-windows-gnu). Put llvm-mingw's bin folder first on PATH (see README)."
}
New-Item -ItemType Directory -Force $BuildDir | Out-Null

$DiscRoot = $null
if ($Disc) {
    foreach ($candidate in @($Disc, (Join-Path $Disc 'DATA'))) {
        if ((Test-Path (Join-Path $candidate 'sys/main.dol') -PathType Leaf) -and (Test-Path (Join-Path $candidate 'files') -PathType Container)) {
            $DiscRoot = (Resolve-Path $candidate).Path; break
        }
    }
    if (-not $DiscRoot) { Fail "$Disc doesn't look like an extracted disc (expected sys\main.dol and files\ in it or in DATA\)" }
}

if (-not $SkipTranslate) {
    Step 'Checking main.dol'
    $dol = Join-Path $Repo 'Assets/main.dol'
    if ($DiscRoot) { $source = Join-Path $DiscRoot 'sys/main.dol' }
    elseif (Test-Path $dol) { $source = $dol }
    else { Fail 'pass the folder you extracted the game into: .\build.cmd "C:\path\to\game"' }
    $expected = ([regex]::Match((Get-Content $Project -Raw), 'sha256:\s*([0-9a-fA-F]{64})')).Groups[1].Value.ToLower()
    $actual = (Get-FileHash $source -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $expected) {
        Fail "$source has SHA-256 $actual, expected $expected.`nOnly a clean Mario Strikers Charged USA Rev 1 (R4QE01) main.dol is supported."
    }
    if ((Resolve-Path $source).Path -ne $dol) {
        New-Item -ItemType Directory -Force (Join-Path $Repo 'Assets') | Out-Null
        Copy-Item $source $dol -Force
    }
    Write-Host '    main.dol OK (R4QE01 Rev 1)'

    Step 'Building the translator'
    Logged 'translator-build.log' $dotnet @('build', 'translator/src/Translator.Cli', '-c', 'Release', '--nologo')

    Step 'Translating main.dol (this takes a few minutes)'
    Logged 'translate.log' $dotnet @($TranslatorDll, 'translate-recursive', '0x80006124', '--project', $Project,
        '--outdir', 'generated/functions', '--output-metadata', 'generated/base_translation_output.json',
        '--production-source-bundle', 'generated/base_translation_sources.bin',
        '--no-function-files', '--prune-stale', '--threads', "$Jobs")

    Step 'Generating data sections and the build graph'
    Logged 'data-init.log' $dotnet @($TranslatorDll, 'generate-data-init', '--project', $Project)
    Logged 'build-shards.log' $dotnet @($TranslatorDll, 'emit-build-shards', '--project', $Project,
        '--base-metadata', 'generated/base_translation_output.json', '--base-functions-dir', 'generated/functions',
        '--native-source-dir', 'runtime/src', '--out', 'generated/build_shards')
}

Step 'Configuring the native build'
Logged 'configure.log' $cmake @('-S', 'runtime', '-B', $BuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_C_COMPILER=$($clang -replace '\\', '/')", "-DCMAKE_CXX_COMPILER=$($clangxx -replace '\\', '/')",
    "-DCMAKE_MAKE_PROGRAM=$($ninja -replace '\\', '/')")

Step 'Compiling (the first build takes a long time)'
Logged 'build.log' $cmake @('--build', $BuildDir, '--target', 'WiiCompiled', '--parallel', "$Jobs")
$exe = Join-Path $BuildDir $ExeName
if (-not (Test-Path $exe)) { Fail "build finished but $exe is missing (see $(Join-Path $BuildDir 'build.log'))" }

if ($DiscRoot) {
    # Point Config.toml's [paths] dvd_root at the extracted disc, keeping any other settings.
    $config = Join-Path $AppDir 'Config.toml'
    New-Item -ItemType Directory -Force $AppDir | Out-Null
    # Forward slashes work on Windows and need no TOML escaping.
    $line = 'dvd_root = "' + ($DiscRoot -replace '\\', '/') + '"'
    $text = if (Test-Path $config) { Get-Content $config -Raw } else { '' }
    if ($null -eq $text) { $text = '' }
    if ($text -match '(?m)^\s*dvd_root\s*=') {
        $text = [regex]::Replace($text, '(?m)^\s*dvd_root\s*=.*$', { param($m) $line })
    } elseif ($text -match '(?m)^\[paths\]\s*$') {
        $text = [regex]::Replace($text, '(?m)^\[paths\]\s*$', { param($m) "[paths]`n$line" }, 1)
    } else {
        $text = $text.TrimEnd("`r", "`n") + $(if ($text) { "`n`n" } else { '' }) + "[paths]`n$line`n"
    }
    [IO.File]::WriteAllText($config, $text)
    Write-Host "`n    Config: $config (dvd_root -> $DiscRoot)"
}

Write-Host "`nDone. Run the game with:`n    $exe" -ForegroundColor Green
