param(
    [switch]$CleanOnly,
    [switch]$SkipNativeBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir "..\..")).Path

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )

    Write-Host "==> $Name" -ForegroundColor Cyan
    & $Action
}

function Get-MSBuildPath {
    $cmd = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    $fallbacks = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe")
    )

    foreach ($candidate in $fallbacks) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

function Build-VcxprojReleaseX64 {
    param(
        [Parameter(Mandatory = $true)][string]$MSBuildPath,
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$ProjectName
    )

    if (-not (Test-Path $ProjectPath)) {
        throw "$ProjectName project file not found: $ProjectPath"
    }

    Write-Host "Building $ProjectName => Release|x64" -ForegroundColor DarkCyan
    & $MSBuildPath $ProjectPath /m /t:Build /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) {
        throw "$ProjectName build failed with exit code $LASTEXITCODE"
    }
}

Push-Location $scriptDir
try {
    Invoke-Step "Cleaning old build output" {
        foreach ($p in @("build", "dist", "portable")) {
            $full = Join-Path $scriptDir $p
            if (Test-Path $full) {
                Remove-Item -Path $full -Recurse -Force
            }
        }
    }

    if ($CleanOnly) {
        Write-Host "Clean complete." -ForegroundColor Green
        return
    }

    $venvDir = Join-Path $scriptDir ".packenv"
    if (-not (Test-Path (Join-Path $venvDir "Scripts\python.exe"))) {
        Invoke-Step "Creating build virtual environment" {
            if (Get-Command py -ErrorAction SilentlyContinue) {
                & py -3 -m venv $venvDir
            }
            elseif (Get-Command python -ErrorAction SilentlyContinue) {
                & python -m venv $venvDir
            }
            else {
                throw "Python was not found. Install Python 3.11+ first."
            }
        }
    }

    $pythonExe = Join-Path $venvDir "Scripts\python.exe"

    Invoke-Step "Installing build dependencies" {
        & $pythonExe -m pip install --upgrade pip setuptools wheel pyinstaller
    }

    Invoke-Step "Building browser executable (onedir)" {
        & $pythonExe -m PyInstaller `
            --noconfirm `
            --clean `
            --windowed `
            --onedir `
            --name ProjectReboundBrowser `
            project_rebound_browser.py
    }

    Invoke-Step "Building UDP proxy executable (onefile)" {
        & $pythonExe -m PyInstaller `
            --noconfirm `
            --clean `
            --console `
            --onefile `
            --name ProjectReboundUdpProxy `
            project_rebound_udp_proxy.py
    }

    if (-not $SkipNativeBuild) {
        Invoke-Step "Building native helper projects (Release|x64)" {
            $msbuildPath = Get-MSBuildPath
            if (-not $msbuildPath) {
                throw "MSBuild was not found. Install Visual Studio Build Tools (Desktop development with C++) or pass -SkipNativeBuild."
            }

            Build-VcxprojReleaseX64 -MSBuildPath $msbuildPath -ProjectPath (Join-Path $repoRoot "dxgi\dxgi.vcxproj") -ProjectName "dxgi"
            Build-VcxprojReleaseX64 -MSBuildPath $msbuildPath -ProjectPath (Join-Path $repoRoot "Payload\Payload.vcxproj") -ProjectName "Payload"
            Build-VcxprojReleaseX64 -MSBuildPath $msbuildPath -ProjectPath (Join-Path $repoRoot "ServerWrapper\ProjectReboundServerWrapper\ProjectReboundServerWrapper\ProjectReboundServerWrapper.vcxproj") -ProjectName "ProjectReboundServerWrapper"
        }
    }

    $portableRoot = Join-Path $scriptDir "portable\ProjectReboundBrowserPortable"
    $runtimeDir = Join-Path $portableRoot "runtime"
    New-Item -Path $portableRoot -ItemType Directory -Force | Out-Null
    New-Item -Path $runtimeDir -ItemType Directory -Force | Out-Null

    Invoke-Step "Collecting runtime files" {
        Copy-Item -Path (Join-Path $scriptDir "dist\ProjectReboundBrowser\*") -Destination $portableRoot -Recurse -Force
        Copy-Item -Path (Join-Path $scriptDir "dist\ProjectReboundUdpProxy.exe") -Destination (Join-Path $portableRoot "ProjectReboundUdpProxy.exe") -Force
        Copy-Item -Path (Join-Path $scriptDir "README.md") -Destination (Join-Path $portableRoot "README.md") -Force
    }

    $missingArtifacts = New-Object System.Collections.Generic.List[string]

    function Copy-LatestArtifact {
        param(
            [Parameter(Mandatory = $true)][string]$ArtifactName,
            [Parameter(Mandatory = $true)][string[]]$Candidates
        )

        $existing = @()
        foreach ($candidate in $Candidates) {
            if (Test-Path $candidate) {
                $existing += Get-Item $candidate
            }
        }

        if ($existing.Count -eq 0) {
            $missingArtifacts.Add($ArtifactName)
            Write-Warning "$ArtifactName not found in local build outputs."
            return
        }

        $selected = $existing | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        Copy-Item -Path $selected.FullName -Destination (Join-Path $runtimeDir $ArtifactName) -Force
        Write-Host "Copied $ArtifactName from $($selected.FullName)" -ForegroundColor DarkGreen
    }

    Invoke-Step "Collecting optional game-side helper binaries" {
        Copy-LatestArtifact -ArtifactName "dxgi.dll" -Candidates @(
            (Join-Path $repoRoot "dxgi\x64\Release\dxgi.dll"),
            (Join-Path $repoRoot "dxgi\dxgi\x64\Release\dxgi.dll")
        )

        Copy-LatestArtifact -ArtifactName "Payload.dll" -Candidates @(
            (Join-Path $repoRoot "Payload\x64\Release\Payload.dll"),
            (Join-Path $repoRoot "Payload\Payload\x64\Release\Payload.dll")
        )

        Copy-LatestArtifact -ArtifactName "ProjectReboundServerWrapper.exe" -Candidates @(
            (Join-Path $repoRoot "ServerWrapper\ProjectReboundServerWrapper\x64\Release\ProjectReboundServerWrapper.exe"),
            (Join-Path $repoRoot "ServerWrapper\ProjectReboundServerWrapper\ProjectReboundServerWrapper\x64\Release\ProjectReboundServerWrapper.exe")
        )
    }

    $startBat = Join-Path $portableRoot "start_browser.bat"
    $startBatLines = @(
        '@echo off'
        'setlocal'
        'cd /d %~dp0'
        '%~dp0ProjectReboundBrowser.exe'
    )
    Set-Content -Path $startBat -Value $startBatLines -Encoding ASCII

    Write-Host "" 
    Write-Host "Portable package ready:" -ForegroundColor Green
    Write-Host "  $portableRoot"

    if ($missingArtifacts.Count -gt 0) {
        Write-Warning ("Missing optional artifacts: " + ($missingArtifacts -join ", "))
        Write-Host "The browser package still works; these files are only needed for automatic copy-to-game behavior."
    }
    else {
        Write-Host "Runtime helper artifacts included: dxgi.dll, Payload.dll, ProjectReboundServerWrapper.exe" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
