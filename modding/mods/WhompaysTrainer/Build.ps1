param([switch]$Test, [switch]$BuildHost)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$compiler = Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang++.exe'
$env:PATH = (Split-Path $compiler) + ';' + $env:PATH
$out = Join-Path $root 'build/trainer'
New-Item -ItemType Directory -Force $out | Out-Null
$common = @('-std=c++23','-O2','-msse4.1','-D_CRT_SECURE_NO_WARNINGS',
    "-I$root/build/sdk/include", "-I$root/modding/include", '-luser32','-lgdi32')
if ($Test) {
    & $compiler @common (Join-Path $root 'modding/tests/trainer_test.cpp') '-o' (Join-Path $out 'trainer_test.exe')
    if ($LASTEXITCODE) { throw 'Trainer tests failed to compile.' }
    & (Join-Path $out 'trainer_test.exe')
    if ($LASTEXITCODE) { throw 'Trainer tests failed.' }
} else {
    if ($BuildHost) {
        $env:REXSDK = Join-Path $root 'build/sdk'
        & cmake -S (Join-Path $root 'project') -B (Join-Path $root 'build/game') '-DFETCHCONTENT_FULLY_DISCONNECTED=ON'
        if ($LASTEXITCODE) { throw 'Host configuration failed.' }
        & cmake --build (Join-Path $root 'build/game') --target saintsrow whompays_trainer -j 6
        if ($LASTEXITCODE) { throw 'Host build failed.' }
        Copy-Item (Join-Path $root 'build/game/saintsrow.exe') (Join-Path $root 'dist/saintsrow.exe') -Force
    }
    & $compiler @common '-shared' (Join-Path $PSScriptRoot 'trainer.cpp') '-o' (Join-Path $out 'WhompaysTrainer.dll')
    if ($LASTEXITCODE) { throw 'Trainer build failed.' }
    $dest = Join-Path $root 'dist/mods/WhompaysTrainer'
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item (Join-Path $out 'WhompaysTrainer.dll') $dest -Force
    Copy-Item (Join-Path $PSScriptRoot 'mod.ini'),(Join-Path $PSScriptRoot 'README.md') $dest -Force
    Write-Host "Built and installed: $dest"
}
