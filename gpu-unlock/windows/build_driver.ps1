# SPDX-License-Identifier: GPL-2.0-only
# Build gpuunlock.sys with the MSVC x64 toolset and the WDK kernel headers/libs.
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$obj = Join-Path $build "drvobj"
New-Item -ItemType Directory -Force -Path $build, $obj | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
	$vswhere = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe"
}
if (-not (Test-Path $vswhere)) {
	throw "vswhere.exe not found; install Visual Studio Build Tools with the C/C++ workload"
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
	throw "no Visual Studio installation with the MSVC x64 toolset was found"
}
$msvc = $null
foreach ($tool in (Get-ChildItem (Join-Path $vs "VC\Tools\MSVC") | Sort-Object Name -Descending)) {
	$candidate = Join-Path $tool.FullName "bin\Hostx64\x64\cl.exe"
	if (Test-Path $candidate) {
		$msvc = $tool.FullName
		break
	}
}
if (-not $msvc) {
	throw "cl.exe was not found under $vs"
}

$kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
$sdk = $null
foreach ($dir in (Get-ChildItem (Join-Path $kits "Include") | Sort-Object Name -Descending)) {
	if (Test-Path (Join-Path $dir.FullName "km\ntddk.h")) {
		$sdk = $dir.Name
		break
	}
}
if (-not $sdk) {
	throw "WDK kernel headers were not found; install Microsoft.WindowsWDK"
}

$inc = Join-Path $kits "Include\$sdk"
$lib = Join-Path $kits "Lib\$sdk\km\x64"
$env:INCLUDE = "$inc\km;$inc\km\crt;$inc\shared;$inc\ucrt;$msvc\include"
$env:LIB = "$msvc\lib\x64;$lib"
$env:PATH = "$msvc\bin\Hostx64\x64;$env:PATH"

$source = Join-Path $root "windows\driver\gpuunlock_drv.c"
$flags = @(
	"/nologo", "/c", "/std:c11", "/O2", "/W3", "/GS-",
	"/D_KERNEL_MODE=1", "/D_AMD64_=1", "/D_WIN64", "/DUNICODE", "/D_UNICODE",
	"/I$(Join-Path $root 'windows\driver')",
	# See build.ps1: keep the builder's profile path out of the shipped binary.
	# /pathmap is silently ignored without /experimental:deterministic.
	"/experimental:deterministic",
	"/pathmap:${root}\=C:\gpu-unlock-src\"
)
$out = Join-Path $build "gpuunlock.sys"

& (Join-Path $msvc "bin\Hostx64\x64\cl.exe") @flags "/Fo$obj\" $source
if ($LASTEXITCODE -ne 0) {
	throw "cl.exe failed with exit code $LASTEXITCODE"
}

& (Join-Path $msvc "bin\Hostx64\x64\link.exe") /nologo /DRIVER /SUBSYSTEM:NATIVE `
	/MACHINE:X64 /ENTRY:DriverEntry /OUT:$out `
	"$obj\gpuunlock_drv.obj" ntoskrnl.lib hal.lib wdmsec.lib BufferOverflowK.lib
if ($LASTEXITCODE -ne 0) {
	throw "link.exe failed with exit code $LASTEXITCODE"
}
Write-Host "built $out"
