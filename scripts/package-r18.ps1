# R18 packaging: build VirtualMSSQL-1.0.0-windows.zip
# Requires: all 4 build configs + asan already built; vcvarsall in env for tools.
param(
    [string]$OutDir = "D:\projects\externals\VirtualMSSQL\build\package"
)
$ErrorActionPreference = "Stop"
$root = "D:\projects\externals\VirtualMSSQL"
$stage = Join-Path $env:TEMP "vms18_stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\Win32", "$stage\x64", "$stage\include\virtualmssql", "$stage\examples", "$stage\licenses" | Out-Null

# DLLs (release configs)
Copy-Item "$root\build\win32-release\virtualmssql.dll" "$stage\Win32\virtualmssql.dll"
Copy-Item "$root\build\x64-release\virtualmssql.dll" "$stage\x64\virtualmssql.dll"

# Headers
Copy-Item "$root\include\virtualmssql\vms_api.h" "$stage\include\virtualmssql\vms_api.h"

# README (packaging variant: project README as-is)
Copy-Item "$root\README.md" "$stage\README.md"

# Examples
Copy-Item "$root\examples\load_smoke.c" "$stage\examples\load_smoke.c"

# Licenses + third-party notices
Copy-Item "$root\licenses\THIRD-PARTY.md" "$stage\licenses\THIRD-PARTY.md"
Copy-Item "$root\licenses\THIRD-PARTY.md" "$stage\THIRD-PARTY-NOTICES"

# Manifest (provenance + toolset + versions)
$git = git -C $root log --oneline -1
$commit = ($git -split ' ')[0]
$manifest = @"
VirtualMSSQL 1.0.0 windows package
build-date: $(Get-Date -Format "yyyy-MM-dd HH:mm:ssK")
git-commit: $commit
toolset: MSVC 14.44.35207 + Windows SDK 10.0.26100 (C11, CMake+Ninja)
configs: Win32/x64 Release (/MD, /W4 /utf-8)
odbc-driver-required: Microsoft ODBC Driver 18 for SQL Server (18.6.2.1 tested)
sqlite-baseline: 3.44.0+ (tests ran against 3.53.4)
"@
Set-Content "$stage\MANIFEST.txt" $manifest -Encoding UTF8

# SBOM (simple inventory of shipped binaries + linked system DLLs)
$sbom = @"
SBOM — VirtualMSSQL 1.0.0
component: virtualmssql.dll (Win32 + x64), self-built from this repository
linked-system: ODBC32.dll (Windows), ADVAPI32.dll (Windows), KERNEL32.dll (Windows),
  VCRUNTIME140.dll (VC++ Redistributable 2015-2022), api-ms-win-crt-* (UCRT),
  sqlite3.dll (host-provided, SQLite 3.44+, NOT bundled)
driver-prereq: Microsoft ODBC Driver 18 for SQL Server (not bundled)
third-party-code: none compiled in; SQLite used via import library only
"@
Set-Content "$stage\SBOM.txt" $sbom -Encoding UTF8

# SHA-256 checksums
$hashes = Get-ChildItem "$stage\Win32\virtualmssql.dll", "$stage\x64\virtualmssql.dll", "$stage\include\virtualmssql\vms_api.h" |
    ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)  $($_.Name)" }
Set-Content "$stage\SHA256SUMS.txt" ($hashes -join "`r`n") -Encoding ASCII

# Zip
if (Test-Path $OutDir) { Remove-Item "$OutDir\VirtualMSSQL-1.0.0-windows.zip" -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Compress-Archive -Path "$stage\*" -DestinationPath "$OutDir\VirtualMSSQL-1.0.0-windows.zip"
Write-Host "package: $OutDir\VirtualMSSQL-1.0.0-windows.zip"
Get-ChildItem "$OutDir\VirtualMSSQL-1.0.0-windows.zip" | Select-Object Name, Length
