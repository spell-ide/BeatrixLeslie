[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$InstallPrefix = "",
    [switch]$StrictWarnings,
    [switch]$NoWerror,
    [string]$SignPfxPath = $env:SPELL_WINDOWS_PFX_PATH,
    [string]$SignPfxPassword = $env:SPELL_WINDOWS_PFX_PASSWORD,
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
    param([string]$BaseBuildDir, [string]$Config, [string]$ExeName = "BeatrixLeslieIDE.exe")
    $Candidates = @(
        (Join-Path $BaseBuildDir "$Config\$ExeName"),
        (Join-Path $BaseBuildDir $ExeName)
    )
    foreach ($Candidate in $Candidates) {
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
            return $Candidate
        }
    }
    throw "$ExeName was not found under '$BaseBuildDir' (checked $($Candidates -join ', '))."
}

function Find-WinDeployQt {
    $Cmd = Get-Command windeployqt6, windeployqt -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $Cmd) {
        return $Cmd.Source
    }
    foreach ($EnvPrefix in @($env:Qt6_DIR, $env:QT_ROOT_DIR)) {
        if (-not [string]::IsNullOrWhiteSpace($EnvPrefix)) {
            foreach ($BinName in @("windeployqt6.exe", "windeployqt.exe")) {
                $Candidate = Join-Path $EnvPrefix "bin\$BinName"
                if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
                    return $Candidate
                }
            }
        }
    }
    return $null
}

function Find-SignTool {
    $Cmd = Get-Command signtool.exe, signtool -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $Cmd) {
        return $Cmd.Source
    }
    $ProgFilesX86 = ${env:ProgramFiles(x86)}
    if (-not [string]::IsNullOrWhiteSpace($ProgFilesX86)) {
        $KitsRoot = Join-Path $ProgFilesX86 "Windows Kits\10\bin"
        if (Test-Path -LiteralPath $KitsRoot -PathType Container) {
            $Found = Get-ChildItem -Path $KitsRoot -Recurse -Filter "signtool.exe" -File -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } |
                Sort-Object FullName -Descending |
                Select-Object -First 1
            if ($null -ne $Found) {
                return $Found.FullName
            }
        }
    }
    throw "signtool.exe was not found on PATH or under Windows Kits\10\bin. Install the Windows 10/11 SDK to sign binaries."
}

function Invoke-AuthenticodeSignOnTargetDir {
    param([string]$TargetExe, [string]$PfxPath, [string]$PfxPassword)
    $ResolvedPfx = Resolve-WorkspacePath -InputPath $PfxPath
    if (-not (Test-Path -LiteralPath $ResolvedPfx -PathType Leaf)) {
        throw "Windows signing PFX file was not found at '$ResolvedPfx'."
    }
    $SignToolExe = Find-SignTool
    $TargetDir = Split-Path -Parent $TargetExe

    $Targets = @()
    $CoreDll = Join-Path $TargetDir "spell_core.dll"
    if (Test-Path -LiteralPath $CoreDll -PathType Leaf) {
        $Targets += $CoreDll
    }
    foreach ($QsciDll in @(Get-ChildItem -Path $TargetDir -Filter "qscintilla2*.dll" -File -ErrorAction SilentlyContinue)) {
        $Targets += $QsciDll.FullName
    }
    if (Test-Path -LiteralPath $TargetExe -PathType Leaf) {
        $Targets += $TargetExe
    }

    if ($Targets.Count -eq 0) {
        throw "No signable PE targets found in '$TargetDir'."
    }

    Write-Host "==> Signing Windows binaries in $TargetDir with signtool.exe ($($Targets.Count) file(s))"
    $SignArgs = @(
        "sign",
        "/fd", "SHA256",
        "/tr", "http://timestamp.digicert.com",
        "/td", "SHA256",
        "/f", $ResolvedPfx
    )
    if (-not [string]::IsNullOrWhiteSpace($PfxPassword)) {
        $SignArgs += @("/p", $PfxPassword)
    }
    $SignArgs += $Targets

    & $SignToolExe @SignArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Invoke-WinDeployQtOnTarget {
    param([string]$WinDeployQtExe, [string]$TargetExe, [string]$Config)
    $ModeFlag = if ($Config -eq "Debug") { "--debug" } else { "--release" }
    $TargetDir = Split-Path -Parent $TargetExe
    $QsciDlls = @(Get-ChildItem -Path $TargetDir -Filter "qscintilla2*.dll" -File -ErrorAction SilentlyContinue)
    if ($QsciDlls.Count -eq 0) {
        $WinDeployBinDir = Split-Path -Parent $WinDeployQtExe
        $WinDeployRootDir = Split-Path -Parent $WinDeployBinDir
        $SearchDirs = @(
            $WinDeployBinDir,
            (Join-Path $WinDeployRootDir "lib")
        )
        foreach ($EnvPrefix in @($env:Qt6_DIR, $env:QT_ROOT_DIR)) {
            if (-not [string]::IsNullOrWhiteSpace($EnvPrefix)) {
                $SearchDirs += (Join-Path $EnvPrefix "bin")
                $SearchDirs += (Join-Path $EnvPrefix "lib")
            }
        }
        foreach ($SearchDir in $SearchDirs) {
            if (Test-Path -LiteralPath $SearchDir -PathType Container) {
                $FoundQsci = @(Get-ChildItem -Path $SearchDir -Filter "qscintilla2*.dll" -File -ErrorAction SilentlyContinue)
                if ($FoundQsci.Count -gt 0) {
                    Copy-Item -LiteralPath $FoundQsci[0].FullName -Destination (Join-Path $TargetDir $FoundQsci[0].Name) -Force
                    $QsciDlls = @(Get-ChildItem -Path $TargetDir -Filter "qscintilla2*.dll" -File -ErrorAction SilentlyContinue)
                    break
                }
            }
        }
    }
    $DeployInputs = @($TargetExe)
    foreach ($QsciDll in $QsciDlls) {
        $DeployInputs += $QsciDll.FullName
    }
    Write-Host "==> Running windeployqt ($ModeFlag) on $($DeployInputs -join ', ')"
    & $WinDeployQtExe $ModeFlag --no-translations --compiler-runtime @DeployInputs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Test-WindowsRuntimeLayout {
    param([string]$TargetExe, [bool]$RequireQtDlls)
    $TargetDir = Split-Path -Parent $TargetExe
    Write-Host "==> Verifying Windows runtime layout in $TargetDir"

    if (-not (Test-Path -LiteralPath $TargetExe -PathType Leaf)) {
        throw "Loader verification failed: missing executable '$TargetExe'."
    }

    $CoreDll = Join-Path $TargetDir "spell_core.dll"
    $LicenseFile = Join-Path $TargetDir "Resources\THIRD_PARTY_LICENSES.txt"
    $LogoFile = Join-Path $TargetDir "Resources\AppLogo.jpg"

    if (-not (Test-Path -LiteralPath $CoreDll -PathType Leaf)) {
        throw "Loader verification failed: missing '$CoreDll' alongside '$TargetExe'."
    }

    $QsciMatches = @(Get-ChildItem -Path $TargetDir -Filter "qscintilla2*.dll" -File -ErrorAction SilentlyContinue)
    if ($QsciMatches.Count -eq 0) {
        throw "Loader verification failed: missing QScintilla2 DLL (qscintilla2_qt6.dll / qscintilla2*.dll) alongside '$TargetExe'."
    }

    if ($RequireQtDlls) {
        $QtWidgetsCandidates = @(
            (Join-Path $TargetDir "Qt6Widgets.dll"),
            (Join-Path $TargetDir "Qt6Widgetsd.dll")
        )
        $FoundQtWidgets = $false
        foreach ($QtCand in $QtWidgetsCandidates) {
            if (Test-Path -LiteralPath $QtCand -PathType Leaf) {
                $FoundQtWidgets = $true
                break
            }
        }
        if (-not $FoundQtWidgets) {
            throw "Loader verification failed: missing Qt6Widgets.dll alongside '$TargetExe' after -DeployQt."
        }

        $QtPrintCandidates = @(
            (Join-Path $TargetDir "Qt6PrintSupport.dll"),
            (Join-Path $TargetDir "Qt6PrintSupportd.dll")
        )
        $FoundQtPrint = $false
        foreach ($QtPrintCand in $QtPrintCandidates) {
            if (Test-Path -LiteralPath $QtPrintCand -PathType Leaf) {
                $FoundQtPrint = $true
                break
            }
        }
        if (-not $FoundQtPrint) {
            throw "Loader verification failed: missing Qt6PrintSupport.dll (required by QScintilla2) alongside '$TargetExe' after -DeployQt."
        }
    }

    if (-not (Test-Path -LiteralPath $LicenseFile -PathType Leaf)) {
        throw "Loader verification failed: missing '$LicenseFile'."
    }
    if (-not (Test-Path -LiteralPath $LogoFile -PathType Leaf)) {
        throw "Loader verification failed: missing '$LogoFile'."
    }

    $DumpBin = Get-Command dumpbin -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $DumpBin) {
        $Imports = & $DumpBin.Source /DEPENDENTS $TargetExe
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        Write-Host ($Imports | Out-String)
        if (-not (($Imports | Out-String) -match "spell_core\.dll")) {
            throw "Loader verification failed: '$TargetExe' PE import table does not reference spell_core.dll."
        }
    } else {
        $ObjDump = Get-Command objdump -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $ObjDump) {
            $Imports = & $ObjDump.Source -p $TargetExe | Select-String "DLL Name:"
            Write-Host ($Imports | Out-String)
            if (-not (($Imports | Out-String) -match "spell_core\.dll")) {
                throw "Loader verification failed: '$TargetExe' PE import table does not reference spell_core.dll."
            }
        }
    }

    Write-Host "==> Windows loader verification passed for $TargetExe"
}

$ResolvedBuildDir = Resolve-WorkspacePath -InputPath $BuildDir
$WerrorValue = if ($NoWerror) { "OFF" } else { "ON" }

$PrevRustFlags = $env:RUSTFLAGS
try {
    if ($StrictWarnings) {
        $env:RUSTFLAGS = if ([string]::IsNullOrWhiteSpace($PrevRustFlags)) {
            "-D warnings"
        } else {
            "$PrevRustFlags -D warnings"
        }
        Write-Host "==> Strict warnings enabled (SPELL_WARNINGS_AS_ERRORS=ON, RUSTFLAGS='$($env:RUSTFLAGS)')"
    }

    Write-Host "==> Configuring BeatrixLeslie IDE ($Configuration) in $ResolvedBuildDir"
    & cmake -S $RootDir -B $ResolvedBuildDir `
        "-DCMAKE_BUILD_TYPE=$Configuration" `
        "-DSPELL_BUILD_RUST=ON" `
        "-DSPELL_WARNINGS_AS_ERRORS=$WerrorValue"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $ParallelJobs = [Math]::Max(1, [Environment]::ProcessorCount)
    Write-Host "==> Building BeatrixLeslie IDE ($Configuration, parallel jobs: $ParallelJobs)"
    & cmake --build $ResolvedBuildDir --config $Configuration --parallel $ParallelJobs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $AppExe = Find-BuiltExecutable -BaseBuildDir $ResolvedBuildDir -Config $Configuration

    $WinDeployQtExe = $null
    if ($DeployQt) {
        $WinDeployQtExe = Find-WinDeployQt
        if ($null -eq $WinDeployQtExe) {
            throw "windeployqt was not found on PATH or under Qt6_DIR / QT_ROOT_DIR. Add your Qt6 bin directory to PATH when passing -DeployQt."
        }
        Invoke-WinDeployQtOnTarget -WinDeployQtExe $WinDeployQtExe -TargetExe $AppExe -Config $Configuration

        if ($Test) {
            $TestExe = Find-BuiltExecutable -BaseBuildDir $ResolvedBuildDir -Config $Configuration -ExeName "SpellQtTests.exe"
            Invoke-WinDeployQtOnTarget -WinDeployQtExe $WinDeployQtExe -TargetExe $TestExe -Config $Configuration
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

    $InstalledAppExe = ""
    if (-not [string]::IsNullOrWhiteSpace($InstallPrefix)) {
        $ResolvedInstallPrefix = Resolve-WorkspacePath -InputPath $InstallPrefix
        Write-Host "==> Installing BeatrixLeslie IDE to $ResolvedInstallPrefix"
        & cmake --install $ResolvedBuildDir --config $Configuration --prefix $ResolvedInstallPrefix
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }

        $InstalledAppExe = Join-Path $ResolvedInstallPrefix "bin\BeatrixLeslieIDE.exe"
        if ($DeployQt) {
            Invoke-WinDeployQtOnTarget -WinDeployQtExe $WinDeployQtExe -TargetExe $InstalledAppExe -Config $Configuration
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($SignPfxPath)) {
        $SignTargetExe = if (-not [string]::IsNullOrWhiteSpace($InstalledAppExe)) { $InstalledAppExe } else { $AppExe }
        Invoke-AuthenticodeSignOnTargetDir -TargetExe $SignTargetExe -PfxPath $SignPfxPath -PfxPassword $SignPfxPassword
    }

    if ($VerifyLoader) {
        Test-WindowsRuntimeLayout -TargetExe $AppExe -RequireQtDlls $DeployQt.IsPresent
        if (-not [string]::IsNullOrWhiteSpace($InstalledAppExe)) {
            Test-WindowsRuntimeLayout -TargetExe $InstalledAppExe -RequireQtDlls $DeployQt.IsPresent
        }
    }
} finally {
    if ($null -eq $PrevRustFlags) {
        Remove-Item Env:\RUSTFLAGS -ErrorAction SilentlyContinue
    } else {
        $env:RUSTFLAGS = $PrevRustFlags
    }
}

if ($Run) {
    $LaunchExe = if (-not [string]::IsNullOrWhiteSpace($InstalledAppExe) -and $DeployQt) { $InstalledAppExe } else { $AppExe }
    Write-Host "==> Launching $LaunchExe"
    if ($null -ne $RunArgs -and $RunArgs.Count -gt 0) {
        & $LaunchExe @RunArgs
    } else {
        & $LaunchExe
    }
    exit $LASTEXITCODE
}
