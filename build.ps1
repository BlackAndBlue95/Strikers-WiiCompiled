# Build Strikers-WiiCompiled from your own copy of Mario Strikers Charged (Windows).
#
#   build.cmd                                   # asks for your game (or double-click build.cmd)
#   build.cmd "C:\path\to\game.wbfs"            # disc image: ISO, RVZ, WBFS, WIA, CISO, GCZ, ...
#   build.cmd "C:\path\to\extracted\game"       # or a folder extracted with Dolphin (sys\ + files\)
#   build.cmd -SkipTranslate                    # recompile the runtime only
#
# The game files are installed into the app's data folder (next to Config.toml and saves), so the
# original image/folder isn't needed afterwards. Build output (Assets\, generated\, build-windows\)
# stays in the repo, gitignored.
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
# Like WiiCompiled's installer, the game files are installed next to the config and saves, so the
# original disc image or folder isn't needed once the build is done.
$GameDir = Join-Path $AppDir 'Game'
# nodtool (https://github.com/encounter/nod) reads Wii disc images; pinned by version and hash.
$NodVersion = 'v2.0.0-alpha.10'
$NodBuilds = @{
    'AMD64' = @('nodtool-windows-x86_64.exe', '1e150e33b88157527f96caea89ad12267aed8b117180c9ebaa89ebe89c1d948d')
    'ARM64' = @('nodtool-windows-arm64.exe', 'a2cf9a1ab153fb1252e9676a60d9dd6f68d8f192387bbfbf571e7ba19b8047aa')
}
# LLVM-MinGW ships no C++/WinRT headers (music_attenuation.cpp needs them), so they're generated
# with cppwinrt.exe from the same NuGet package Launcher/Prepare-Dependencies.ps1 pins.
$CppWinRtVersion = '3.0.260818.1'
$CppWinRtSha = 'a8993608ae9263a8288e1a1d9ee2684a5632f4acc59e1e89ddd86b3b889a7367'
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

$ExpectedDolSha = ([regex]::Match((Get-Content $Project -Raw), 'sha256:\s*([0-9a-fA-F]{64})')).Groups[1].Value.ToLower()
function Get-Sha([string] $Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }
function Assert-Dol([string] $Path) {
    $actual = Get-Sha $Path
    if ($actual -ne $ExpectedDolSha) {
        Fail "$Path has SHA-256 $actual, expected $ExpectedDolSha.`nOnly a clean Mario Strikers Charged USA Rev 1 (R4QE01) disc is supported."
    }
}
# The folder that directly contains sys\main.dol and files\ (the folder itself or DATA\), or $null.
function Get-DiscRoot([string] $Folder) {
    foreach ($candidate in @($Folder, (Join-Path $Folder 'DATA'))) {
        if ((Test-Path -LiteralPath (Join-Path $candidate 'sys/main.dol') -PathType Leaf) -and (Test-Path -LiteralPath (Join-Path $candidate 'files') -PathType Container)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}
function Get-NodTool {
    $arch = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
    if (-not $NodBuilds.ContainsKey($arch)) { Fail "no prebuilt nodtool for $arch; extract the disc with Dolphin and pass the folder instead" }
    $asset, $sha = $NodBuilds[$arch]
    $tool = Join-Path $BuildDir 'tools/nodtool.exe'
    if ((Test-Path $tool) -and ((Get-Sha $tool) -eq $sha)) { return $tool }
    New-Item -ItemType Directory -Force (Split-Path $tool) | Out-Null
    Write-Host "    downloading nodtool $NodVersion"
    $part = "$tool.part"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/encounter/nod/releases/download/$NodVersion/$asset" -OutFile $part
    if ((Get-Sha $part) -ne $sha) { Remove-Item $part -Force; Fail 'nodtool download failed its checksum' }
    Move-Item $part $tool -Force
    return $tool
}
# Generates the C++/WinRT projection headers from this machine's Windows metadata, once.
function Get-CppWinRtHeaders {
    $headers = Join-Path $BuildDir 'cppwinrt'
    if (Test-Path (Join-Path $headers 'winrt/base.h') -PathType Leaf) { return $headers }
    Step 'Generating C++/WinRT headers'
    $toolRoot = Join-Path $BuildDir 'tools/cppwinrt'
    $compiler = Join-Path $toolRoot 'bin/cppwinrt.exe'
    if (-not (Test-Path $compiler -PathType Leaf)) {
        New-Item -ItemType Directory -Force (Join-Path $BuildDir 'tools') | Out-Null
        Write-Host "    downloading cppwinrt $CppWinRtVersion"
        # Expand-Archive in Windows PowerShell only accepts a .zip extension (a .nupkg is a zip).
        $part = Join-Path $BuildDir 'tools/cppwinrt.zip'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -UseBasicParsing -Uri "https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/$CppWinRtVersion" -OutFile $part
        if ((Get-Sha $part) -ne $CppWinRtSha) { Remove-Item $part -Force; Fail 'cppwinrt download failed its checksum' }
        if (Test-Path $toolRoot) { Remove-Item $toolRoot -Recurse -Force }
        Expand-Archive $part $toolRoot
        Remove-Item $part -Force
    }
    if (Test-Path $headers) { Remove-Item $headers -Recurse -Force }
    New-Item -ItemType Directory -Force $headers | Out-Null
    Logged 'cppwinrt.log' $compiler @('-input', 'local', '-output', $headers)
    if (-not (Test-Path (Join-Path $headers 'winrt/base.h') -PathType Leaf)) { Fail "cppwinrt produced no winrt\base.h (see $(Join-Path $BuildDir 'cppwinrt.log'))" }
    return $headers
}
function Expand-DiscImage([string] $Image) {
    Step 'Extracting your disc image (a few minutes)'
    $nodtool = Get-NodTool
    $stage = Join-Path $AppDir ".extract-$PID"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force $AppDir | Out-Null
    try {
        Logged 'extract.log' $nodtool @('extract', $Image, $stage)
        $dol = Get-ChildItem $stage -Recurse -File -Filter 'main.dol' | Where-Object { $_.Directory.Name -eq 'sys' } | Select-Object -First 1
        if (-not $dol) { Fail "that image doesn't contain sys\main.dol (is it a Wii disc?)" }
        $root = $dol.Directory.Parent.FullName
        if (-not (Test-Path (Join-Path $root 'files') -PathType Container)) { Fail "that image doesn't contain a files\ folder" }
        Assert-Dol $dol.FullName
        if (Test-Path $GameDir) { Remove-Item $GameDir -Recurse -Force }
        Move-Item $root $GameDir
    } finally {
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    }
    Write-Host "    extracted to $GameDir"
}

if (-not $Disc -and -not $SkipTranslate -and -not (Test-Path (Join-Path $Repo 'Assets/main.dol'))) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Choose your Mario Strikers Charged (USA) disc image'
    $dialog.Filter = 'Wii disc images (*.iso;*.rvz;*.wbfs;*.wia;*.ciso;*.gcz;*.nfs)|*.iso;*.rvz;*.wbfs;*.wia;*.ciso;*.gcz;*.nfs|All files (*.*)|*.*'
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'no game selected' }
    $Disc = $dialog.FileName
}

$DiscRoot = $null
if ($Disc) {
    # -LiteralPath throughout: image names like "Game [R4QE01].wbfs" are not wildcards.
    if (Test-Path -LiteralPath $Disc -PathType Container) {
        $sourceRoot = Get-DiscRoot $Disc
        if (-not $sourceRoot) { Fail "$Disc doesn't look like an extracted disc (expected sys\main.dol and files\ in it or in DATA\)" }
        Assert-Dol (Join-Path $sourceRoot 'sys/main.dol')
        if ([IO.Path]::GetFullPath($sourceRoot).TrimEnd('\') -ne [IO.Path]::GetFullPath($GameDir).TrimEnd('\')) {
            Step 'Installing the game files'
            New-Item -ItemType Directory -Force $AppDir | Out-Null
            $tmp = "$GameDir.tmp"
            if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
            Copy-Item -LiteralPath $sourceRoot $tmp -Recurse
            if (Test-Path $GameDir) { Remove-Item $GameDir -Recurse -Force }
            Move-Item $tmp $GameDir
            Write-Host "    installed to $GameDir"
        }
        $DiscRoot = $GameDir
    } elseif (Test-Path -LiteralPath $Disc -PathType Leaf) {
        $existing = Get-DiscRoot $GameDir
        if ($existing -and ((Get-Sha (Join-Path $existing 'sys/main.dol')) -eq $ExpectedDolSha)) {
            Write-Host "    using the already-extracted game in $GameDir"
            $DiscRoot = $existing
        } else {
            Expand-DiscImage (Resolve-Path -LiteralPath $Disc).Path
            $DiscRoot = $GameDir
        }
    } else {
        Fail "$Disc not found"
    }
}

if (-not $SkipTranslate) {
    Step 'Checking main.dol'
    $dol = Join-Path $Repo 'Assets/main.dol'
    if ($DiscRoot) { $source = Join-Path $DiscRoot 'sys/main.dol' } else { $source = $dol }
    Assert-Dol $source
    if ([IO.Path]::GetFullPath($source) -ne [IO.Path]::GetFullPath($dol)) {
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

$cppWinRt = Get-CppWinRtHeaders

Step 'Configuring the native build'
# SDL is built from source (as WiiCompiled's own Windows build does): the official prebuilt MinGW
# SDL3 has no libusb, so it can't see the Wii U GameCube adapter (WUP-028, or a third-party adapter
# in Wii U mode), which isn't a HID device.
Logged 'configure.log' $cmake @('-S', 'runtime', '-B', $BuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_C_COMPILER=$($clang -replace '\\', '/')", "-DCMAKE_CXX_COMPILER=$($clangxx -replace '\\', '/')",
    "-DCMAKE_MAKE_PROGRAM=$($ninja -replace '\\', '/')", "-DMKW_CPPWINRT_INCLUDE_DIR=$($cppWinRt -replace '\\', '/')",
    '-DAURORA_SDL3_PROVIDER=vendor')

Step 'Compiling (the first build takes a long time)'
Logged 'build.log' $cmake @('--build', $BuildDir, '--target', 'WiiCompiled', '--parallel', "$Jobs")
$exe = Join-Path $BuildDir $ExeName
if (-not (Test-Path $exe)) { Fail "build finished but $exe is missing (see $(Join-Path $BuildDir 'build.log'))" }

if ($DiscRoot) {
    # Point Config.toml's [paths] dvd_root at the game files, keeping any other settings.
    $config = Join-Path $AppDir 'Config.toml'
    New-Item -ItemType Directory -Force $AppDir | Out-Null
    # Forward slashes work on Windows and need no TOML escaping.
    $line = 'dvd_root = "' + ($DiscRoot -replace '\\', '/') + '"'
    $text = if (Test-Path $config) { Get-Content $config -Raw } else { '' }
    if ($null -eq $text) { $text = '' }
    $dvdRe = [regex]'(?m)^[ \t]*dvd_root[ \t]*=[^\r\n]*'
    $pathsRe = [regex]'(?m)^\[paths\][ \t]*\r?$'
    if ($dvdRe.IsMatch($text)) {
        $text = $dvdRe.Replace($text, [Text.RegularExpressions.MatchEvaluator] { param($m) $line }, 1)
    } elseif ($pathsRe.IsMatch($text)) {
        $text = $pathsRe.Replace($text, [Text.RegularExpressions.MatchEvaluator] { param($m) "[paths]`n$line" }, 1)
    } else {
        $text = $text.TrimEnd("`r", "`n") + $(if ($text) { "`n`n" } else { '' }) + "[paths]`n$line`n"
    }
    [IO.File]::WriteAllText($config, $text)
    Write-Host "`n    Config: $config (dvd_root -> $DiscRoot)"
}

Write-Host "`nDone. Run the game with:`n    $exe" -ForegroundColor Green
