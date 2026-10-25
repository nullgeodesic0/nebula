<#
.SYNOPSIS
Builds, signs and publishes a Nebula Plus release on GitHub.

.DESCRIPTION
Builds the package from -Revision (default: the fork's nebula2 branch as fetched),
signs it with the private release key, and creates the GitHub release
plus2-v<version> on -Repository with NebulaPlus-<version>.zip, its .sig,
plus-release.json and NebulaPlus.exe (the download for a first install).
Installed copies of Nebula Plus find it with Check for updates.

The revision must already be pushed: the release's tag points at it. Needs
the GitHub CLI (gh auth login) and what build-plus-package.ps1 needs.
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$Revision = 'fork/nebula2',
    [string]$Repository = 'nullgeodesic0/nebula',
    [string]$Branch = 'nebula2',
    [string]$SigningKey = (Join-Path $env:USERPROFILE '.nebula-plus\plus-release-key.xml'),
    [string]$Notes = '',
    [string]$WorkRoot = '',
    [switch]$Draft
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSCommandPath))
if (-not (Test-Path -LiteralPath $SigningKey)) { throw "No signing key at $SigningKey (installer/plus/new-plus-release-key.ps1 makes one)." }
& git -C $repo fetch (($Revision -split '/')[0]) 2>$null
$commit = (& git -C $repo rev-parse "$Revision^{commit}").Trim()
if ($LASTEXITCODE -ne 0) { throw "Unknown revision $Revision" }
if (-not $Version) { $Version = '2.' + [DateTime]::UtcNow.ToString('yyyyMMdd') + '.' + [int][DateTime]::UtcNow.ToString('HHmm') }
$out = Join-Path $repo "dist\plus-release-$Version"
& (Join-Path $repo 'installer\plus\build-plus-package.ps1') -Version $Version -Revision $commit -Repository $Repository -Branch $Branch `
    -OutputDirectory $out -SigningKey $SigningKey -Zip -WorkRoot $WorkRoot
$zip = Join-Path $out "NebulaPlus-$Version.zip"
if (-not $Notes) { $Notes = (& git -C $repo log -1 --format=%s $commit) }
$body = "Nebula Plus $Version for Nebula 2 (the native game), built from $commit.`n`n$Notes`n`nInstall: download NebulaPlus-Setup-$Version.zip, unzip it, run Install Nebula Plus.cmd and choose your own Super Mario Galaxy (USA) disc image. Update: Check for updates.cmd in the Nebula Plus folder. Saves and settings are never touched by updates. Custom models, animations and voices can be added as mods."
$ghArgs = @('release', 'create', "plus2-v$Version", '--repo', $Repository, '--target', $commit, '--title', "Nebula Plus $Version", '--notes', $body,
    (Join-Path $out "NebulaPlus-Setup-$Version.zip"), $zip, "$zip.sig", (Join-Path $out 'plus-release.json'))
if ($Draft) { $ghArgs += '--draft' }
& gh @ghArgs
if ($LASTEXITCODE -ne 0) { throw "gh release create failed ($LASTEXITCODE)" }
Write-Host "Published plus2-v$Version ($commit) on $Repository."
