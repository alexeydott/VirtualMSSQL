# one-liners for build + test per configuration (R1.2 gate)
# x64: run from "vcvarsall x64" prompt, Win32: from "vcvarsall x86" prompt.

param(
    [Parameter(Mandatory=$true)][ValidateSet('x64','x86')][string]$Arch,
    [Parameter(Mandatory=$true)][ValidateSet('debug','release')][string]$Config
)

$vcvars = Join-Path $env:VMS_VS_ROOT 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not $env:VMS_VS_ROOT) {
    # default location used in this workspace
    $vcvars = 'D:\Visual Studio2022\VC\Auxiliary\Build\vcvarsall.bat'
}
$preset = "$arch-$config"

cmd /c "`"$vcvars`" $arch >nul 2>nul && cmake --preset $preset && cmake --build --preset $preset && ctest --preset $preset"
exit $LASTEXITCODE
