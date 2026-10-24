<#
.SYNOPSIS
Builds a Nebula Plus package from one exact source revision.

.DESCRIPTION
Exports the revision with `git archive` (or takes an exported source folder,
-SourceDir, as the updater's branch channel does) and records each file's
SHA-256, builds nebula-recomp and the runtime from that export, compiles
NebulaPlus.exe with the in-box C# compiler, and writes the package folder:
the prebuilt runtime and its float/DSP libraries, nebula-recomp.exe,
NebulaPlus.exe, the source zip and its manifest, the module contract and
plus-package.json (every file's SHA-256, the module and mod-tools keys).
-Zip also writes NebulaPlus-<version>.zip; -SigningKey signs it (.sig) for
the updater.

The package holds only what this repository's source builds
(docs/CONTENT_POLICY.md). The game modules, the game content and the mods
are built on each player's PC from their own files.

Requirements: Git (without -SourceDir), Rust (stable, MSVC target) and
Visual Studio 2022 or its Build Tools with the C++ tools.
#>
[CmdletBinding()]
param(
    # MAJOR.MINOR.PATCH; the default is 1.<UTC date>.<UTC hour and minute>.
    [string]$Version = '',
    [string]$Revision = 'HEAD',
    # An exported source tree to build instead of a git revision (the updater's branch channel).
    [string]$SourceDir = '',
    [string]$Commit = '',
    [string]$Repository = 'nullgeodesic0/nebula',
    [string]$Branch = 'plus',
    [string]$OutputDirectory = '',
    [string]$SigningKey = '',
    [switch]$Zip,
    # The prebuilt runtime's instruction set. SSE2 (the default) runs on every x64 CPU and is what the co-op and
    # netplay builds were tested with; BOTH adds an AVX2 build that Plus installs on CPUs that have it.
    [ValidateSet('SSE2', 'AVX2', 'BOTH')][string]$NativeIsa = 'SSE2',
    # Where the temporary build trees go (default: %TEMP%); a short path avoids MAX_PATH limits.
    [string]$WorkRoot = ''
)
$ErrorActionPreference = 'Stop'
$plusDir = Split-Path -Parent $PSCommandPath
$scriptRepo = Split-Path -Parent (Split-Path -Parent $plusDir)
if (-not $Version) { $Version = '1.' + [DateTime]::UtcNow.ToString('yyyyMMdd') + '.' + [int][DateTime]::UtcNow.ToString('HHmm') }
if ($Version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$' -and $Version -notmatch '^branch-[0-9a-f]{12}$') { throw "Version $Version is not MAJOR.MINOR.PATCH." }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $scriptRepo "dist\plus-$Version" }
$tempRoot = if ($WorkRoot) { [IO.Path]::GetFullPath($WorkRoot) } else { [IO.Path]::GetTempPath() }
$work = Join-Path $tempRoot ('nebula-plus-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
if (Test-Path -LiteralPath $OutputDirectory) { throw "$OutputDirectory exists; choose a new -OutputDirectory." }
$package = Join-Path $OutputDirectory 'package'
New-Item -ItemType Directory -Force $package | Out-Null

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
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
        $type = 'commit'
        $gitTree = 'unknown'
        [IO.Compression.ZipFile]::CreateFromDirectory($src, $sourceZip, [IO.Compression.CompressionLevel]::Optimal, $true)
    } else {
        $repo = $scriptRepo
        $resolved = (& git -C $repo rev-parse "$Revision^{commit}").Trim()
        if ($LASTEXITCODE -ne 0) { throw "Unknown revision $Revision" }
        $type = 'commit'
        $gitTree = (& git -C $repo show -s --format=%T $resolved).Trim()
        Invoke-Checked git @('-C', $repo, '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', 'archive', '--format=zip', "--prefix=nebula-$resolved/", '-o', $sourceZip, $resolved)
        $src = Join-Path $work 'src'
        [IO.Compression.ZipFile]::ExtractToDirectory($sourceZip, $src)
        $src = Join-Path $src "nebula-$resolved"
    }
    Write-Host "Building Nebula Plus $Version from $type $resolved ($Repository $Branch)"

    # 1. Source manifest: Plus checks the source it compiles the game against file by file.
    $files = [ordered]@{}
    $tree = New-Object Text.StringBuilder
    foreach ($file in (Get-ChildItem -LiteralPath $src -Recurse -File | Sort-Object { $_.FullName.Substring($src.Length + 1).Replace('\', '/') } -CaseSensitive)) {
        $relative = $file.FullName.Substring($src.Length + 1).Replace('\', '/')
        $hash = Get-Sha256 $file.FullName
        $files[$relative] = $hash
        [void]$tree.Append($relative).Append([char]0).Append($hash).Append("`n")
    }
    $treeHash = Get-TextSha256 $tree.ToString()
    $manifest = [ordered]@{ schema = 'nebula.source.v1'; repository = $Repository; commit = $resolved; version = $Version; treeSha256 = $treeHash; files = $files }
    [IO.File]::WriteAllText((Join-Path $package 'source-manifest.json'), ($manifest | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
    Copy-Item -LiteralPath $sourceZip -Destination (Join-Path $package 'nebula-source.zip')
    Write-Host ("Source manifest: {0} files, tree {1}" -f $files.Count, $treeHash)

    # Keys: the game modules depend on the generator, its metadata, the module headers and the float/DSP libraries
    # they link; the mods on their builders. An update rebuilds each only when its key changes.
    function Get-SubsetKey([string[]]$Prefixes, [string[]]$Exact) {
        $text = New-Object Text.StringBuilder
        foreach ($key in $files.Keys) {
            $match = $Exact -contains $key
            foreach ($prefix in $Prefixes) { if ($key.StartsWith($prefix, [StringComparison]::Ordinal)) { $match = $true } }
            if ($match) { [void]$text.Append($key).Append([char]0).Append($files[$key]).Append("`n") }
        }
        Get-TextSha256 $text.ToString()
    }
    $moduleKey = Get-SubsetKey @('crates/', 'metadata/', 'third_party/', 'runtime/include/') @('Cargo.lock', 'Cargo.toml', 'runtime/src/ppc_float.cpp', 'runtime/src/dsp_alu.cpp')
    $modToolsKey = Get-SubsetKey @('tools/animations/') @()

    $fmaCalls = @(Get-ChildItem -LiteralPath (Join-Path $src 'runtime') -Recurse -File -Include *.cpp, *.h, *.inl |
        Where-Object { $_.Name -notlike '*_tests.cpp' -and $_.Name -ne 'fused_multiply_add.h' } |
        Select-String -Pattern '(std::|[^\w.])fmaf?\s*\(')
    if ($fmaCalls.Count -ne 0) { throw ("std::fma/fmaf in the runtime: " + (($fmaCalls | ForEach-Object { "$($_.Path):$($_.LineNumber)" }) -join ', ')) }

    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
    . (Join-Path $src 'tools\enter_msvc_environment.ps1')

    # 2. nebula-recomp, statically linked C runtime.
    $cargo = Join-Path $env:USERPROFILE '.cargo\bin\cargo.exe'
    if (-not (Test-Path $cargo)) { $cargo = 'cargo' }
    $env:RUSTFLAGS = '-C target-feature=+crt-static'
    $env:CARGO_TARGET_DIR = Join-Path $work 'target'
    Push-Location $src
    try { Invoke-Checked $cargo @('build', '--release', '--locked', '-p', 'nebula-recomp') } finally { Pop-Location; Remove-Item Env:RUSTFLAGS }
    Copy-Item (Join-Path $env:CARGO_TARGET_DIR 'release\nebula-recomp.exe') (Join-Path $package 'nebula-recomp.exe')

    # 3. Prebuilt runtime and the libraries the modules link; the DSP comes from RMGE01_dsp.dll.
    $isaBuilds = if ($NativeIsa -eq 'BOTH') { @('SSE2', 'AVX2') } else { @($NativeIsa) }
    $effectiveIsas = @()
    foreach ($isa in $isaBuilds) {
        $rt = Join-Path $work ('runtime-' + $isa.ToLowerInvariant())
        # Windows PowerShell turns a native program's redirected stderr (CMake's warnings) into errors.
        $ErrorActionPreference = 'Continue'
        $configureLog = & cmake @('-S', $src, '-B', $rt, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
            "-DGALAXY_NATIVE_ISA=$isa", '-DNEBULA_DSP_MODULE=ON', '-DGALAXY_FIDELITYFX_SDK=', '-DGALAXY_WITH_STREAMLINE_BRIDGE=OFF',
            "-DNEBULA_SOURCE_REVISION=$resolved", "-DNEBULA_SOURCE_REVISION_KIND=$type", "-DNEBULA_SOURCE_GIT_TREE=$gitTree",
            "-DNEBULA_SOURCE_TREE_SHA256=$treeHash", '-DNEBULA_SOURCE_DIRTY=0') 2>&1
        $configureExit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        $configureLog | ForEach-Object { Write-Host $_ }
        if ($configureExit -ne 0) { throw "cmake configure failed with exit code $configureExit" }
        $effectiveIsa = $null
        foreach ($line in $configureLog) { if ("$line" -match 'Galaxy native CPU target:\s*([A-Za-z0-9]+)') { $effectiveIsa = $Matches[1].ToUpperInvariant() } }
        if ($effectiveIsa -ne $isa) { throw "The $isa runtime configured as '$effectiveIsa'." }
        $effectiveIsas += $effectiveIsa
        Invoke-Checked cmake @('--build', $rt, '--target', 'NebulaRuntime', 'galaxy_ppc_float', 'galaxy_softfloat', 'galaxy_dsp_alu')
        $prefix = if ($NativeIsa -eq 'BOTH' -and $isa -eq 'AVX2') { 'avx2' } else { '' }
        $into = if ($prefix) { Join-Path $package $prefix } else { $package }
        New-Item -ItemType Directory -Force $into | Out-Null
        foreach ($name in 'NebulaRuntime.exe', 'galaxy_ppc_float.lib', 'galaxy_softfloat.lib', 'galaxy_dsp_alu.lib') {
            $file = Get-ChildItem -LiteralPath $rt -Recurse -File -Filter $name | Select-Object -First 1
            if (-not $file) { throw "The runtime build produced no $name" }
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $into $name)
        }
    }
    $runtimeIsa = $effectiveIsas -join '+'

    # 4. NebulaPlus.exe: the installer, updater and window, built with the in-box C# compiler.
    $keyFile = Join-Path $src 'installer\plus\plus-release-public-key.xml'
    $publicKey = if (Test-Path -LiteralPath $keyFile) { (Get-Content -LiteralPath $keyFile -Raw).Trim() } else { '' }
    $buildInfo = @"
namespace Nebula
{
    internal static class BuildInfo
    {
        public const string Version = "$Version";
        public const string Commit = "$resolved";
        public const string Repository = "$Repository";
        public const string ReleasePublicKeyXml = "$($publicKey.Replace('"', '\"'))";
    }
}
"@
    $buildInfoPath = Join-Path $work 'BuildInfo.cs'
    [IO.File]::WriteAllText($buildInfoPath, $buildInfo, (New-Object Text.UTF8Encoding $false))
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $sources = @(Get-ChildItem (Join-Path $src 'installer\src\Common\*.cs') | ForEach-Object FullName) + $buildInfoPath +
        @('InstallEngine', 'Support', 'RelinkCache', 'ModuleCompatibility', 'Toolchain', 'Downloader', 'SourceTree' | ForEach-Object { Join-Path $src "installer\src\Setup\$_.cs" }) +
        @(Get-ChildItem (Join-Path $src 'installer\plus\src\*.cs') | ForEach-Object FullName)
    $references = @('/r:System.Web.Extensions.dll', '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll',
        '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.Core.dll')
    $exe = Join-Path $package 'NebulaPlus.exe'
    Invoke-Checked $csc (@('/nologo', '/nowarn:649', '/target:winexe', '/platform:x64', '/optimize+', "/win32manifest:$(Join-Path $src 'installer\app.manifest')",
        "/win32icon:$(Join-Path $src 'installer\assets\nebula.ico')", "/out:$exe") + $references + $sources)

    # 5. Module contract, toolchain pins, notices.
    $apiText = Get-Content -LiteralPath (Join-Path $src 'runtime/include/galaxy/native_api.h') -Raw
    $dspText = Get-Content -LiteralPath (Join-Path $src 'runtime/include/galaxy/dsp_context.h') -Raw
    $nativeAbi = [regex]::Match($apiText, 'kNativeAbiVersion\s*=\s*(\d+)').Groups[1].Value
    $rsoAbi = [regex]::Match($apiText, 'kNativeRsoModuleAbiVersion\s*=\s*(\d+)').Groups[1].Value
    $dspVersion = [regex]::Match($dspText, 'kDspGeneratedProbeContractVersion\s*=\s*(\d+)').Groups[1].Value
    if (-not $nativeAbi -or -not $rsoAbi -or -not $dspVersion) { throw 'Could not read the module ABI contract.' }
    [ordered]@{ nativeAbi = [uint32]$nativeAbi; rsoAbi = [uint32]$rsoAbi; dspProbeVersion = [uint32]$dspVersion; dspCapabilities = 3 } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'module-compatibility.json') -Encoding utf8
    Copy-Item (Join-Path $src 'installer\toolchain.json') (Join-Path $package 'toolchain.json')
    foreach ($name in 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'README.md') { Copy-Item (Join-Path $src $name) (Join-Path $package $name) }
    if (Test-Path (Join-Path $src 'docs\NEBULA_PLUS.md')) { Copy-Item (Join-Path $src 'docs\NEBULA_PLUS.md') (Join-Path $package 'NEBULA_PLUS.md') }

    # 6. plus-package.json with every file's hash.
    $packageFiles = [ordered]@{}
    foreach ($file in (Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName)) {
        $packageFiles[$file.FullName.Substring($package.Length + 1).Replace('\', '/')] = Get-Sha256 $file.FullName
    }
    $info = [ordered]@{
        schema = 'nebula.plus.package.v1'; version = $Version; commit = $resolved; repository = $Repository; branch = $Branch
        builtUtc = [DateTime]::UtcNow.ToString('o'); runtimeIsa = $runtimeIsa; moduleKey = $moduleKey; modToolsKey = $modToolsKey
        sourceTreeSha256 = $treeHash; files = $packageFiles
    }
    [IO.File]::WriteAllText((Join-Path $package 'plus-package.json'), ($info | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
    Write-Host "Package: $package (module key $($moduleKey.Substring(0, 16)), mod tools $($modToolsKey.Substring(0, 16)))"

    # 7. Release zip and signature.
    if ($Zip) {
        $zipPath = Join-Path $OutputDirectory "NebulaPlus-$Version.zip"
        [IO.Compression.ZipFile]::CreateFromDirectory($package, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
        Copy-Item -LiteralPath $exe -Destination (Join-Path $OutputDirectory 'NebulaPlus.exe')
        if ($SigningKey) {
            if (-not $publicKey) { throw 'installer/plus/plus-release-public-key.xml is missing; run installer/plus/new-plus-release-key.ps1 first.' }
            $rsa = New-Object Security.Cryptography.RSACryptoServiceProvider
            $rsa.PersistKeyInCsp = $false
            $rsa.FromXmlString((Get-Content -LiteralPath $SigningKey -Raw))
            if ($rsa.ToXmlString($false) -ne $publicKey) { throw 'The signing key does not match installer/plus/plus-release-public-key.xml.' }
            $signature = $rsa.SignData([IO.File]::ReadAllBytes($zipPath), [Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'))
            [IO.File]::WriteAllText("$zipPath.sig", [Convert]::ToBase64String($signature))
            Write-Host "Signed $zipPath."
        }
        $release = [ordered]@{ version = $Version; tag = "plus-v$Version"; commit = $resolved; repository = $Repository; branch = $Branch
            zipSha256 = (Get-Sha256 $zipPath); runtimeIsa = $runtimeIsa; moduleKey = $moduleKey; modToolsKey = $modToolsKey }
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'plus-release.json'), ($release | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
        Write-Host "Release zip: $zipPath $($release.zipSha256)"
    }
} finally {
    $resolvedWork = [IO.Path]::GetFullPath($work)
    $resolvedTemp = $tempRoot.TrimEnd('\') + '\'
    if ($resolvedWork.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $resolvedWork) -match '^nebula-plus-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force -ErrorAction SilentlyContinue
    }
}
