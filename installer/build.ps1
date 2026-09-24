# Builds "SaintsRowPC-Setup.exe" with the C# compiler that ships with Windows.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force $out | Out-Null
& $csc /nologo /target:winexe /optimize+ /platform:anycpu "/win32manifest:$here\app.manifest" `
    /reference:System.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll `
    /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll `
    "/out:$out\SaintsRowPC-Setup.exe" "$here\SetupWizard.cs"
if ($LASTEXITCODE) { throw 'Setup build failed' }
Get-Item "$out\SaintsRowPC-Setup.exe" | Select-Object Name, Length
