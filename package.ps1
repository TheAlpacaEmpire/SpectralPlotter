# Builds a clean Release of Spectral Plotter and packages it as a portable zip in dist\.
# Usage: powershell -ExecutionPolicy Bypass -File package.ps1
$ErrorActionPreference = 'Stop'

$root     = $PSScriptRoot
$jucer    = Join-Path $root 'CircularSpectrumAnalyzer\CircularSpectrumAnalyzer.jucer'
$sln      = Join-Path $root 'CircularSpectrumAnalyzer\Builds\VisualStudio2026\SpectralPlotter.sln'
$exe      = Join-Path $root 'CircularSpectrumAnalyzer\Builds\VisualStudio2026\x64\Release\App\SpectralPlotter.exe'
$projucer = if ($env:PROJUCER) { $env:PROJUCER } else { 'C:\JUCE\Projucer.exe' }

$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (is Visual Studio installed?)' }

$version = ([xml] (Get-Content $jucer -Raw)).JUCERPROJECT.version
if (-not $version) { throw "No version attribute in $jucer" }

Write-Host "== Regenerating project (Projucer)"
& $projucer --resave $jucer
if ($LASTEXITCODE -ne 0) { throw 'Projucer resave failed' }

Write-Host "== Building Release x64 (clean)"
& $msbuild $sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$name  = "SpectralPlotter-$version-win64"
$dist  = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $exe $stage
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

Write-Host "== Packaged $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB)"
