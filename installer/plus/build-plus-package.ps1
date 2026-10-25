<#
.SYNOPSIS
Builds a Nebula Plus package for Nebula 2 (the native game) from one exact source revision.

.DESCRIPTION
Exports the revision with `git archive` (or takes an exported source folder, -SourceDir, as the updater's branch
channel does) and records each file's SHA-256, then builds from that export: nebula-import (Cargo), NebulaNative.exe
(MSVC), smg_game.dll (clang, from the Petari decompilation in native/petari) and NebulaPlus.exe / NebulaPlusCli.exe
(the in-box C# compiler). It writes the package folder with plus-package.json (every file's SHA-256 and the mod
tools' key); -Zip also writes NebulaPlus-<version>.zip, and -SigningKey signs it (.sig) for the updater.

The package holds only what this repository's source builds (docs/CONTENT_POLICY.md). game.pak and the Odyssey mod
are made on each player's PC from their own disc and files.

Requirements: Git (without -SourceDir), Rust (stable, MSVC target), Visual Studio 2022 or its Build Tools with the C++
tools and Windows SDK, LLVM/clang 19+ (-Llvm), CMake and Ninja.
#>
[CmdletBinding()]
param(
    # MAJOR.MINOR.PATCH; the default is 2.<UTC date>.<UTC hour and minute>.
    [string]$Version = '',
    [string]$Revision = 'HEAD',
    # An exported source tree to build instead of a git revision (the updater's branch channel).
    [string]$SourceDir = '',
    [string]$Commit = '',
    [string]$Repository = 'nullgeodesic0/nebula',
    [string]$Branch = 'nebula2',
    [string]$OutputDirectory = '',
    [string]$SigningKey = '',
    [switch]$Zip,
    [string]$Llvm = 'C:\Program Files\LLVM\bin',
    # Where the temporary build trees go (default: %TEMP%); a short path avoids MAX_PATH limits.
    [string]$WorkRoot = ''
)
$ErrorActionPreference = 'Stop'
$plusDir = Split-Path -Parent $PSCommandPath
$scriptRepo = Split-Path -Parent (Split-Path -Parent $plusDir)
if (-not $Version) { $Version = '2.' + [DateTime]::UtcNow.ToString('yyyyMMdd') + '.' + [int][DateTime]::UtcNow.ToString('HHmm') }
if ($Version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$' -and $Version -notmatch '^branch-[0-9a-f]{12}$') { throw "Version $Version is not MAJOR.MINOR.PATCH." }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $scriptRepo "dist\plus-$Version" }
$tempRoot = if ($WorkRoot) { [IO.Path]::GetFullPath($WorkRoot) } else { [IO.Path]::GetTempPath() }
$work = Join-Path $tempRoot ('nebula-plus-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
if (Test-Path -LiteralPath $OutputDirectory) { throw "$OutputDirectory exists; choose a new -OutputDirectory." }
$package = Join-Path $OutputDirectory 'package'
New-Item -ItemType Directory -Force $package | Out-Null

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    # Windows PowerShell turns a native program's stderr into errors when it is redirected: let it through.
    $ErrorActionPreference = 'Continue'
    & $File @Arguments
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "$File failed with exit code $code" }
}
function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-TextSha256([string]$Text) {
    [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-', '').ToLowerInvariant()
}

try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $sourceZip = Join-Path $work 'nebula-source.zip'
    if ($SourceDir) {
        $src = (Resolve-Path -LiteralPath $SourceDir).Path
        if (-not $Commit) { throw '-SourceDir needs -Commit.' }
        $resolved = $Commit
        [IO.Compression.ZipFile]::CreateFromDirectory($src, $sourceZip, [IO.Compression.CompressionLevel]::Optimal, $true)
    } else {
        $resolved = (& git -C $scriptRepo rev-parse "$Revision^{commit}").Trim()
        if ($LASTEXITCODE -ne 0) { throw "Unknown revision $Revision" }
        Invoke-Checked git @('-C', $scriptRepo, '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', 'archive', '--format=zip', "--prefix=nebula-$resolved/", '-o', $sourceZip, $resolved)
        $src = Join-Path $work 'src'
        [IO.Compression.ZipFile]::ExtractToDirectory($sourceZip, $src)
        $src = Join-Path $src "nebula-$resolved"
    }
    Write-Host "Building Nebula Plus $Version (Nebula 2) from commit $resolved ($Repository $Branch)"

    # 1. Source manifest: Plus checks the source the mod builders run from, file by file.
    $files = [ordered]@{}
    foreach ($file in (Get-ChildItem -LiteralPath $src -Recurse -File | Sort-Object { $_.FullName.Substring($src.Length + 1).Replace('\', '/') } -CaseSensitive)) {
        $files[$file.FullName.Substring($src.Length + 1).Replace('\', '/')] = Get-Sha256 $file.FullName
    }
    $manifest = [ordered]@{ schema = 'nebula.source.v1'; repository = $Repository; commit = $resolved; version = $Version; files = $files }
    [IO.File]::WriteAllText((Join-Path $package 'source-manifest.json'), ($manifest | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
    Copy-Item -LiteralPath $sourceZip -Destination (Join-Path $package 'nebula-source.zip')
    $toolsText = New-Object Text.StringBuilder
    foreach ($key in $files.Keys) { if ($key.StartsWith('tools/animations/', [StringComparison]::Ordinal)) { [void]$toolsText.Append($key).Append([char]0).Append($files[$key]).Append("`n") } }
    $modToolsKey = Get-TextSha256 $toolsText.ToString()

    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
    . (Join-Path $src 'tools\enter_msvc_environment.ps1')

    # 2. nebula-import, the C runtime linked in (it runs on PCs without the Visual C++ runtime).
    $cargo = Join-Path $env:USERPROFILE '.cargo\bin\cargo.exe'
    if (-not (Test-Path $cargo)) { $cargo = 'cargo' }
    $env:CARGO_TARGET_DIR = Join-Path $work 'target'
    Push-Location $src
    try {
        Invoke-Checked $cargo @('build', '--release', '--locked', '-p', 'nebula-import', '--bin', 'nebula-import',
            '--config', "target.x86_64-pc-windows-msvc.rustflags=['-C','target-feature=+crt-static']")
    } finally { Pop-Location }
    Copy-Item (Join-Path $env:CARGO_TARGET_DIR 'release\nebula-import.exe') (Join-Path $package 'nebula-import.exe')

    # 3. NebulaNative.exe (MSVC; AVX2, as upstream's Setup ships it).
    $host_ = Join-Path $work 'host'
    Invoke-Checked cmake @('-S', $src, '-B', $host_, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DGALAXY_NATIVE_ISA=AVX2')
    Invoke-Checked cmake @('--build', $host_, '--target', 'NebulaNative')
    Copy-Item (Join-Path $host_ 'NebulaNative.exe') (Join-Path $package 'NebulaNative.exe')

    # 4. smg_game.dll (clang, from the decompilation; no AVX or FMA, so every PC computes the same game).
    $game = Join-Path $work 'game'
    $l = $Llvm.Replace('\', '/')
    Invoke-Checked cmake @('-S', (Join-Path $src 'native').Replace('\', '/'), '-B', $game.Replace('\', '/'), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_C_COMPILER=$l/clang.exe", "-DCMAKE_CXX_COMPILER=$l/clang++.exe", "-DCMAKE_RC_COMPILER=$l/llvm-rc.exe",
        "-DNEBULA_PETARI_DIR=$((Join-Path $src 'native\petari').Replace('\', '/'))")
    Invoke-Checked cmake @('--build', $game, '--target', 'smg_game')
    Copy-Item (Join-Path $game 'smg_game.dll') (Join-Path $package 'smg_game.dll')

    # 5. NebulaPlus.exe and NebulaPlusCli.exe.
    $plusKeyFile = Join-Path $src 'installer\plus\plus-release-public-key.xml'
    $plusKey = if (Test-Path -LiteralPath $plusKeyFile) { (Get-Content -LiteralPath $plusKeyFile -Raw).Trim() } else { '' }
    $upstreamKey = (Get-Content -LiteralPath (Join-Path $src 'installer\release-public-key.xml') -Raw).Trim()
    $buildInfoPath = Join-Path $work 'BuildInfo.cs'
    [IO.File]::WriteAllText($buildInfoPath, @"
namespace Nebula
{
    internal static class BuildInfo
    {
        public const string Version = "$Version";
        public const string Commit = "$resolved";
        public const string Repository = "$Repository";
        public const string ReleasePublicKeyXml = "$($upstreamKey.Replace('"', '\"'))";
        public const string PlusReleasePublicKeyXml = "$($plusKey.Replace('"', '\"'))";
    }
}
"@, (New-Object Text.UTF8Encoding $false))
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $sources = @(Get-ChildItem (Join-Path $src 'installer\src\Common\*.cs') | ForEach-Object FullName) + $buildInfoPath +
        @(Get-ChildItem (Join-Path $src 'installer\plus\src\*.cs') | ForEach-Object FullName)
    $references = @('/r:System.Web.Extensions.dll', '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll',
        '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.Core.dll')
    $appManifest = Join-Path $src 'installer\app.manifest'
    $exe = Join-Path $package 'NebulaPlus.exe'
    Invoke-Checked $csc (@('/nologo', '/nowarn:649', '/target:winexe', '/platform:x64', '/optimize+', "/win32manifest:$appManifest",
        "/win32icon:$(Join-Path $src 'installer\assets\nebula.ico')", "/out:$exe") + $references + $sources)
    Invoke-Checked $csc (@('/nologo', '/nowarn:649', '/target:exe', '/platform:x64', '/optimize+', '/define:NEBULA_PLUS_CLI',
        "/win32manifest:$appManifest", "/out:$(Join-Path $package 'NebulaPlusCli.exe')") + $references + $sources)

    foreach ($name in 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'README.md') { Copy-Item (Join-Path $src $name) (Join-Path $package $name) }
    if (Test-Path (Join-Path $src 'docs\NEBULA_PLUS.md')) { Copy-Item (Join-Path $src 'docs\NEBULA_PLUS.md') (Join-Path $package 'NEBULA_PLUS.md') }

    # 6. plus-package.json with every file's hash.
    $packageFiles = [ordered]@{}
    foreach ($file in (Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName)) {
        $packageFiles[$file.FullName.Substring($package.Length + 1).Replace('\', '/')] = Get-Sha256 $file.FullName
    }
    $info = [ordered]@{
        schema = 'nebula.plus.package.v2'; game = 'native'; version = $Version; commit = $resolved; repository = $Repository; branch = $Branch
        builtUtc = [DateTime]::UtcNow.ToString('o'); modToolsKey = $modToolsKey; files = $packageFiles
    }
    [IO.File]::WriteAllText((Join-Path $package 'plus-package.json'), ($info | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
    Write-Host "Package: $package (smg_game.dll $($packageFiles['smg_game.dll'].Substring(0, 16)))"

    # 7. Release zip and signature.
    if ($Zip) {
        $zipPath = Join-Path $OutputDirectory "NebulaPlus-$Version.zip"
        [IO.Compression.ZipFile]::CreateFromDirectory($package, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
        Copy-Item -LiteralPath $exe -Destination (Join-Path $OutputDirectory 'NebulaPlus.exe')
        if ($SigningKey) {
            if (-not $plusKey) { throw 'installer/plus/plus-release-public-key.xml is missing; run installer/plus/new-plus-release-key.ps1 first.' }
            $rsa = New-Object Security.Cryptography.RSACryptoServiceProvider
            $rsa.PersistKeyInCsp = $false
            $rsa.FromXmlString((Get-Content -LiteralPath $SigningKey -Raw))
            if ($rsa.ToXmlString($false) -ne $plusKey) { throw 'The signing key does not match installer/plus/plus-release-public-key.xml.' }
            $signature = $rsa.SignData([IO.File]::ReadAllBytes($zipPath), [Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'))
            [IO.File]::WriteAllText("$zipPath.sig", [Convert]::ToBase64String($signature))
            Write-Host "Signed $zipPath."
        }
        $release = [ordered]@{ version = $Version; tag = "plus2-v$Version"; game = 'native'; commit = $resolved; repository = $Repository; branch = $Branch
            zipSha256 = (Get-Sha256 $zipPath); smgGameSha256 = $packageFiles['smg_game.dll']; modToolsKey = $modToolsKey }
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'plus-release.json'), ($release | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
        Write-Host "Release zip: $zipPath $($release.zipSha256)"
    }
} finally {
    $resolvedWork = [IO.Path]::GetFullPath($work)
    if ($resolvedWork.StartsWith($tempRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $resolvedWork) -match '^nebula-plus-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force -ErrorAction SilentlyContinue
    }
}
