[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$InstallPrefix = "",
    [switch]$Test,
    [switch]$VerifyLoader,
    [switch]$DeployQt,
    [switch]$Run,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RunArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-WorkspacePath {
    param([string]$InputPath)
    if ([System.IO.Path]::IsPathRooted($InputPath)) {
        return $InputPath
    }
    if ($InputPath.StartsWith(".\") -or $InputPath.StartsWith("..\") -or
        $InputPath.StartsWith("./") -or $InputPath.StartsWith("../")) {
        return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $InputPath))
    }
    return [System.IO.Path]::GetFullPath((Join-Path $RootDir $InputPath))
}

function Find-BuiltExecutable {
    param([string]$BaseBuildDir, [string]$Config)
    $Candidates = @(
        (Join-Path $BaseBuildDir "$Config\BeatrixLeslieIDE.exe"),
        (Join-Path $BaseBuildDir "BeatrixLeslieIDE.exe")
    )
    foreach ($Candidate in $Candidates) {
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
            return $Candidate
        }
    }
    throw "BeatrixLeslieIDE.exe was not found under '$BaseBuildDir' (checked $($Candidates -join ', '))."
}

$ResolvedBuildDir = Resolve-WorkspacePath -InputPath $BuildDir

Write-Host "==> Configuring BeatrixLeslie IDE ($Configuration) in $ResolvedBuildDir"
& cmake -S $RootDir -B $ResolvedBuildDir "-DCMAKE_BUILD_TYPE=$Configuration" "-DSPELL_BUILD_RUST=ON"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "==> Building BeatrixLeslie IDE ($Configuration)"
& cmake --build $ResolvedBuildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$AppExe = Find-BuiltExecutable -BaseBuildDir $ResolvedBuildDir -Config $Configuration
$AppDir = Split-Path -Parent $AppExe

if ($DeployQt) {
    $WinDeployQt = Get-Command windeployqt6, windeployqt -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $WinDeployQt) {
        Write-Host "==> Running $($WinDeployQt.Name) on $AppExe"
        & $WinDeployQt.Source --no-translations $AppExe
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    } else {
        Write-Warning "windeployqt was not found on PATH; skipping Qt DLL deployment. Add your Qt6 bin directory to PATH to bundle Qt runtime DLLs."
    }
}

if ($Test) {
    Write-Host "==> Running spell_core Rust tests (SPELL_EPHEMERAL_KEYRING=1)"
    $PrevEphemeral = $env:SPELL_EPHEMERAL_KEYRING
    try {
        $env:SPELL_EPHEMERAL_KEYRING = "1"
        & cargo test --manifest-path (Join-Path $RootDir "rust\spell_core\Cargo.toml") --target-dir (Join-Path $RootDir "rust\spell_core\target")
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    } finally {
        if ($null -eq $PrevEphemeral) {
            Remove-Item Env:\SPELL_EPHEMERAL_KEYRING -ErrorAction SilentlyContinue
        } else {
            $env:SPELL_EPHEMERAL_KEYRING = $PrevEphemeral
        }
    }

    Write-Host "==> Running SpellQtTests via CTest"
    & ctest --test-dir $ResolvedBuildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if (-not [string]::IsNullOrWhiteSpace($InstallPrefix)) {
    $ResolvedInstallPrefix = Resolve-WorkspacePath -InputPath $InstallPrefix
    Write-Host "==> Installing BeatrixLeslie IDE to $ResolvedInstallPrefix"
    & cmake --install $ResolvedBuildDir --config $Configuration --prefix $ResolvedInstallPrefix
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if ($VerifyLoader) {
    Write-Host "==> Verifying Windows runtime layout in $AppDir"
    $CoreDll = Join-Path $AppDir "spell_core.dll"
    $LicenseFile = Join-Path $AppDir "Resources\THIRD_PARTY_LICENSES.txt"
    $LogoFile = Join-Path $AppDir "Resources\AppLogo.jpg"

    if (-not (Test-Path -LiteralPath $CoreDll -PathType Leaf)) {
        throw "Loader verification failed: missing '$CoreDll' alongside '$AppExe'."
    }
    if (-not (Test-Path -LiteralPath $LicenseFile -PathType Leaf)) {
        throw "Loader verification failed: missing '$LicenseFile'."
    }
    if (-not (Test-Path -LiteralPath $LogoFile -PathType Leaf)) {
        throw "Loader verification failed: missing '$LogoFile'."
    }

    $DumpBin = Get-Command dumpbin -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $DumpBin) {
        $Imports = & $DumpBin.Source /DEPENDENTS $AppExe
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        Write-Host ($Imports | Out-String)
        if (-not (($Imports | Out-String) -match "spell_core\.dll")) {
            throw "Loader verification failed: '$AppExe' PE import table does not reference spell_core.dll."
        }
    } else {
        $ObjDump = Get-Command objdump -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $ObjDump) {
            $Imports = & $ObjDump.Source -p $AppExe | Select-String "DLL Name:"
            Write-Host ($Imports | Out-String)
            if (-not (($Imports | Out-String) -match "spell_core\.dll")) {
                throw "Loader verification failed: '$AppExe' PE import table does not reference spell_core.dll."
            }
        }
    }

    Write-Host "==> Windows loader verification passed for $AppExe"
}

if ($Run) {
    Write-Host "==> Launching $AppExe"
    if ($null -ne $RunArgs -and $RunArgs.Count -gt 0) {
        & $AppExe @RunArgs
    } else {
        & $AppExe
    }
    exit $LASTEXITCODE
}
