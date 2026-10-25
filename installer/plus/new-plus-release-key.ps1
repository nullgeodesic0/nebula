<#
.SYNOPSIS
Creates the Nebula Plus release signing key.

.DESCRIPTION
Writes a new 3072-bit RSA key pair: the private key to -PrivateKey (outside the
repository; default %USERPROFILE%\.nebula-plus\plus-release-key.xml) and the
public key to installer/plus/plus-release-public-key.xml, which NebulaPlus.exe
carries and checks every downloaded package against. Refuses to replace an
existing private key: packages signed with it would stop verifying for
everyone who has Plus installed.
#>
[CmdletBinding()]
param([string]$PrivateKey = (Join-Path $env:USERPROFILE '.nebula-plus\plus-release-key.xml'))
$ErrorActionPreference = 'Stop'
$publicPath = Join-Path (Split-Path -Parent $PSCommandPath) 'plus-release-public-key.xml'
if (Test-Path -LiteralPath $PrivateKey) { throw "$PrivateKey exists; keep using it (delete it only to start a new key, which old installs will not trust)." }
New-Item -ItemType Directory -Force (Split-Path -Parent $PrivateKey) | Out-Null
$rsa = New-Object Security.Cryptography.RSACryptoServiceProvider 3072
$rsa.PersistKeyInCsp = $false
[IO.File]::WriteAllText($PrivateKey, $rsa.ToXmlString($true))
[IO.File]::WriteAllText($publicPath, $rsa.ToXmlString($false) + "`n")
Write-Host "Private key: $PrivateKey (keep it private and backed up; never commit it)"
Write-Host "Public key:  $publicPath (commit this)"
