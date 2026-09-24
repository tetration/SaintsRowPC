# Builds SaintsRowPC-Setup.exe and SaintsRowPC-Updater.exe (the same program,
# starting in update mode) with the C# compiler that ships with Windows.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($target in @(@{ Name = 'SaintsRowPC-Setup.exe'; Define = '' }, @{ Name = 'SaintsRowPC-Updater.exe'; Define = '/define:UPDATER' })) {
    $flags = @('/nologo', '/target:winexe', '/optimize+', '/platform:anycpu', "/win32manifest:$here\app.manifest",
        '/reference:System.dll', '/reference:System.Drawing.dll', '/reference:System.Windows.Forms.dll',
        '/reference:System.IO.Compression.dll', '/reference:System.IO.Compression.FileSystem.dll',
        "/out:$out\$($target.Name)", "$here\SetupWizard.cs")
    if ($target.Define) { $flags += $target.Define }
    & $csc @flags
    if ($LASTEXITCODE) { throw "$($target.Name) build failed" }
}
Get-ChildItem $out -Filter *.exe | Select-Object Name, Length
