<#
.SYNOPSIS
    Builds and installs QScintilla 2.14.1 for Qt 6 (MSVC) against qmake on PATH.

.DESCRIPTION
    Downloads QScintilla 2.14.1 source, verifies its SHA-256 digest, compiles the
    Qt 6 release library with MSVC/nmake, and installs the Qsci headers,
    qscintilla2_qt6.lib import library, and qscintilla2_qt6.dll runtime library into
    the active Qt 6 prefix reported by qmake.

    Prerequisites:
      1. Run from an x64 Native Tools / Developer PowerShell prompt for Visual Studio 2022
         (cl.exe and nmake.exe on PATH).
      2. Ensure Qt 6's bin directory (e.g. C:\Qt\6.7.3\msvc2019_64\bin) is on PATH so
         qmake resolves to Qt 6.

.PARAMETER CacheDir
    Optional directory to cache and restore prebuilt QScintilla 2.14.1 artifacts across
    CI runs (for example, C:\qscintilla-cache).

.PARAMETER ForceRebuild
    Compile from source even when prebuilt artifacts already exist in CacheDir or Qt.
#>
[CmdletBinding()]
param(
    [string]$CacheDir = "",
    [switch]$ForceRebuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Find-QMake6 {
    $Cmd = Get-Command qmake6, qmake -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $Cmd) {
        return $Cmd.Source
    }
    foreach ($EnvPrefix in @($env:Qt6_DIR, $env:QT_ROOT_DIR)) {
        if (-not [string]::IsNullOrWhiteSpace($EnvPrefix)) {
            foreach ($BinName in @("qmake6.exe", "qmake.exe")) {
                $Candidate = Join-Path $EnvPrefix "bin\$BinName"
                if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
                    return $Candidate
                }
            }
        }
    }
    throw "qmake (Qt 6) was not found on PATH or under Qt6_DIR / QT_ROOT_DIR. Add your Qt 6 msvc2019_64/bin (or msvc2022_64/bin) directory to PATH."
}

$QMakeExe = Find-QMake6
$QtVersion = (& $QMakeExe -query QT_VERSION).Trim()
if (-not $QtVersion.StartsWith("6.")) {
    throw "Expected Qt 6 qmake on PATH, but '$QMakeExe' reported QT_VERSION=$QtVersion."
}

$QtLibDir = (& $QMakeExe -query QT_INSTALL_LIBS).Trim()
$QtBinDir = (& $QMakeExe -query QT_INSTALL_BINS).Trim()
$QtIncDir = (& $QMakeExe -query QT_INSTALL_HEADERS).Trim()

Write-Host "==> Target Qt $QtVersion installation:"
Write-Host "    qmake:   $QMakeExe"
Write-Host "    headers: $QtIncDir"
Write-Host "    libs:    $QtLibDir"
Write-Host "    bins:    $QtBinDir"

if (-not $ForceRebuild -and -not [string]::IsNullOrWhiteSpace($CacheDir)) {
    $CachedLib = Join-Path $CacheDir "qscintilla2_qt6.lib"
    $CachedDll = Join-Path $CacheDir "qscintilla2_qt6.dll"
    $CachedHdr = Join-Path $CacheDir "Qsci\qsciscintilla.h"

    if ((Test-Path -LiteralPath $CachedLib -PathType Leaf) -and
        (Test-Path -LiteralPath $CachedDll -PathType Leaf) -and
        (Test-Path -LiteralPath $CachedHdr -PathType Leaf)) {
        Write-Host "==> Restoring prebuilt QScintilla 2.14.1 from cache ($CacheDir)"
        Copy-Item -LiteralPath $CachedLib -Destination (Join-Path $QtLibDir "qscintilla2_qt6.lib") -Force
        Copy-Item -LiteralPath $CachedDll -Destination (Join-Path $QtLibDir "qscintilla2_qt6.dll") -Force
        Copy-Item -LiteralPath $CachedDll -Destination (Join-Path $QtBinDir "qscintilla2_qt6.dll") -Force
        Copy-Item -LiteralPath (Join-Path $CacheDir "Qsci") -Destination $QtIncDir -Recurse -Force
        exit 0
    }
}

if (-not $ForceRebuild -and [string]::IsNullOrWhiteSpace($CacheDir)) {
    if ((Test-Path -LiteralPath (Join-Path $QtLibDir "qscintilla2_qt6.lib") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $QtBinDir "qscintilla2_qt6.dll") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $QtIncDir "Qsci\qsciscintilla.h") -PathType Leaf)) {
        Write-Host "==> QScintilla 2.14.1 is already installed in Qt $QtVersion ($QtLibDir). Pass -ForceRebuild to recompile."
        exit 0
    }
}

if ($null -eq (Get-Command nmake -ErrorAction SilentlyContinue) -or
    $null -eq (Get-Command cl -ErrorAction SilentlyContinue)) {
    throw "MSVC build tools (cl.exe and nmake.exe) were not found on PATH. Run this script from 'x64 Native Tools Command Prompt for VS 2022' or 'Developer PowerShell for VS 2022'."
}

$ExpectedSha256 = "dfe13c6acc9d85dfcba76ccc8061e71a223957a6c02f3c343b30a9d43a4cdd4d"
$Urls = @(
    "https://www.riverbankcomputing.com/static/Downloads/QScintilla/2.14.1/QScintilla_src-2.14.1.tar.gz",
    "https://files.pythonhosted.org/packages/a9/f6/a7aa4b495dcee4c521b87205de9363fb62ee5fdc8eab91d4ddb97257c85b/QScintilla-2.14.1.tar.gz"
)

$TempBase = if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    $env:RUNNER_TEMP
} else {
    [System.IO.Path]::GetTempPath()
}
$WorkDir = Join-Path $TempBase "qscintilla-2.14.1-build-$PID"
if (Test-Path -LiteralPath $WorkDir) {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force
}
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

try {
    $TarPath = Join-Path $WorkDir "QScintilla_src-2.14.1.tar.gz"
    $DownloadedUrl = ""
    foreach ($CandUrl in $Urls) {
        Write-Host "==> Downloading QScintilla 2.14.1 from $CandUrl"
        & curl.exe --fail --location --retry 3 --retry-delay 2 -o $TarPath $CandUrl
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $TarPath -PathType Leaf)) {
            $DownloadedUrl = $CandUrl
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($DownloadedUrl)) {
        throw "Failed to download QScintilla 2.14.1 source archive."
    }

    $ActualSha256 = (Get-FileHash -LiteralPath $TarPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($DownloadedUrl -like "*QScintilla_src-2.14.1.tar.gz" -and $ActualSha256 -ne $ExpectedSha256) {
        throw "SHA256 mismatch for QScintilla_src-2.14.1.tar.gz: expected $ExpectedSha256, got $ActualSha256"
    }

    & tar -xzf $TarPath -C $WorkDir
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $SrcDirCand = Get-ChildItem -Path $WorkDir -Directory | Select-Object -First 1
    if ($null -eq $SrcDirCand) {
        throw "Extracted QScintilla 2.14.1 archive contained no source directory."
    }
    $QsciSrcDir = Join-Path $SrcDirCand.FullName "src"

    Push-Location $QsciSrcDir
    try {
        & $QMakeExe "CONFIG+=release" "CONFIG-=debug_and_release" "QMAKE_CXXFLAGS+=/MP" qscintilla.pro
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        & nmake
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        & nmake install
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        if (-not [string]::IsNullOrWhiteSpace($CacheDir)) {
            New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $QsciSrcDir "Qsci") -Destination $CacheDir -Recurse -Force
        }

        foreach ($BuiltLib in @(Get-ChildItem -Path $QsciSrcDir -Recurse -Filter "qscintilla2_qt6.lib" -File -ErrorAction SilentlyContinue)) {
            if ($QtLibDir -and (Test-Path -LiteralPath $QtLibDir -PathType Container)) {
                Copy-Item -LiteralPath $BuiltLib.FullName -Destination (Join-Path $QtLibDir "qscintilla2_qt6.lib") -Force
            }
            if (-not [string]::IsNullOrWhiteSpace($CacheDir)) {
                Copy-Item -LiteralPath $BuiltLib.FullName -Destination (Join-Path $CacheDir "qscintilla2_qt6.lib") -Force
            }
        }

        foreach ($BuiltDll in @(Get-ChildItem -Path $QsciSrcDir -Recurse -Filter "qscintilla2_qt6.dll" -File -ErrorAction SilentlyContinue)) {
            if ($QtLibDir -and (Test-Path -LiteralPath $QtLibDir -PathType Container)) {
                Copy-Item -LiteralPath $BuiltDll.FullName -Destination (Join-Path $QtLibDir "qscintilla2_qt6.dll") -Force
            }
            if ($QtBinDir -and (Test-Path -LiteralPath $QtBinDir -PathType Container)) {
                Copy-Item -LiteralPath $BuiltDll.FullName -Destination (Join-Path $QtBinDir "qscintilla2_qt6.dll") -Force
            }
            if (-not [string]::IsNullOrWhiteSpace($CacheDir)) {
                Copy-Item -LiteralPath $BuiltDll.FullName -Destination (Join-Path $CacheDir "qscintilla2_qt6.dll") -Force
            }
        }
    } finally {
        Pop-Location
    }

    if ($QtLibDir -and $QtBinDir) {
        $LibDll = Join-Path $QtLibDir "qscintilla2_qt6.dll"
        if ((Test-Path -LiteralPath $LibDll -PathType Leaf) -and (Test-Path -LiteralPath $QtBinDir -PathType Container)) {
            Copy-Item -LiteralPath $LibDll -Destination (Join-Path $QtBinDir "qscintilla2_qt6.dll") -Force
        }
    }

    Write-Host "==> QScintilla 2.14.1 installed into Qt $QtVersion ($QtLibDir)"
} finally {
    if (Test-Path -LiteralPath $WorkDir) {
        Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
