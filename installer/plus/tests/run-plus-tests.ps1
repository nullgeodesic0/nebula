# Nebula Plus checks without a game, a network or an install (installer/plus/tests/PlusTests.cs). The "official"
# data root is a scratch folder (NEBULA_DATA_ROOT), so the user's Nebula files are never touched.
param([string]$Scratch = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
if (-not $Scratch) { $Scratch = Join-Path $env:TEMP ('nebula-plus-tests-' + [guid]::NewGuid().ToString('N')) }
New-Item -ItemType Directory -Force $Scratch | Out-Null
$stub = Join-Path $Scratch 'BuildInfo.cs'
@'
namespace Nebula { internal static class BuildInfo {
 public const string Version = "0.0.0-test.1";
 public const string Commit = "tests";
 public const string Repository = "nullgeodesic0/nebula";
 public const string ReleasePublicKeyXml = "";
}}
'@ | Set-Content -LiteralPath $stub
$setup = @('InstallEngine', 'Support', 'RelinkCache', 'ModuleCompatibility', 'Toolchain', 'Downloader', 'SourceTree' | ForEach-Object { Join-Path $repo "installer\src\Setup\$_.cs" })
$sources = @(Get-ChildItem -LiteralPath (Join-Path $repo 'installer\src\Common') -Filter *.cs | ForEach-Object FullName) + $setup +
    @(Get-ChildItem -LiteralPath (Join-Path $repo 'installer\plus\src') -Filter *.cs | ForEach-Object FullName) +
    @($stub, (Join-Path $PSScriptRoot 'PlusTests.cs'))
$exe = Join-Path $Scratch 'PlusTests.exe'
& (Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe') /nologo /nowarn:649 /target:exe /platform:x64 /main:Nebula.Plus.PlusTests "/out:$exe" `
    /r:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Core.dll @sources
if ($LASTEXITCODE -ne 0) { throw 'Nebula Plus test compilation failed.' }
& $exe (Join-Path $Scratch 'work')
$code = $LASTEXITCODE
Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue
if ($code -ne 0) { throw 'Nebula Plus checks failed.' }
