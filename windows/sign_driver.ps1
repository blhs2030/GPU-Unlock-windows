# SPDX-License-Identifier: GPL-2.0-only
# Self-sign gpuunlock.sys for kernel test signing. Run this elevated.
param(
	[switch]$EnableTestSigning
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$sys = Join-Path $root "build\gpuunlock.sys"
if (-not (Test-Path $sys)) {
	throw "build\gpuunlock.sys not found; run windows\build_driver.ps1 first"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
	throw "sign_driver.ps1 must run elevated"
}

$subject = "CN=GpuUnlock Test Signing"
$cert = Get-ChildItem Cert:\CurrentUser\My |
	Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey } |
	Select-Object -First 1
if (-not $cert) {
	Write-Host "creating self-signed code-signing certificate"
	$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
		-CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable `
		-NotAfter (Get-Date).AddYears(5)
}

# Test signing still validates the chain, so the certificate must be trusted.
$thumbprint = $cert.Thumbprint
$store = New-Object Security.Cryptography.X509Certificates.X509Store -ArgumentList "Root", "CurrentUser"
$store.Open("ReadWrite")
if (-not ($store.Certificates | Where-Object { $_.Thumbprint -eq $thumbprint })) {
	$store.Add($cert)
}
$store.Close()
$store = New-Object Security.Cryptography.X509Certificates.X509Store -ArgumentList "TrustedPublisher", "CurrentUser"
$store.Open("ReadWrite")
if (-not ($store.Certificates | Where-Object { $_.Thumbprint -eq $thumbprint })) {
	$store.Add($cert)
}
$store.Close()

$kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
$signtool = Get-ChildItem (Join-Path $kits "bin") -Recurse -Filter signtool.exe |
	Where-Object { $_.FullName -like "*\x64\signtool.exe" } |
	Sort-Object FullName -Descending |
	Select-Object -First 1
if (-not $signtool) {
	throw "signtool.exe was not found under $kits\bin"
}

& $signtool.FullName sign /fd SHA256 /sha1 $thumbprint $sys
if ($LASTEXITCODE -ne 0) {
	throw "signtool sign failed with exit code $LASTEXITCODE"
}
& $signtool.FullName verify /pa /v $sys
if ($LASTEXITCODE -ne 0) {
	throw "signtool verify failed with exit code $LASTEXITCODE"
}
Write-Host "signed $sys"

if ($EnableTestSigning) {
	bcdedit /set testsigning on
	if ($LASTEXITCODE -ne 0) {
		throw "bcdedit failed with exit code $LASTEXITCODE"
	}
	Write-Host "test signing enabled; reboot before loading gpuunlock.sys"
}

$testsigning = bcdedit /enum "{current}" | Select-String -Pattern "testsigning"
if ($testsigning -match "Yes") {
	Write-Host "test signing is ON"
} else {
	Write-Host "test signing is OFF; run this script with -EnableTestSigning and reboot"
}
