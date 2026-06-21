# Build and run the portable-core unit tests on the PC (no Flipper needed).
# Uses clang if present, else gcc (msys2). Run from the repo root:  ./host_test/run.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$srcs = @(
    "host_test\test_updi.c", "host_test\updi_emu.c",
    "updi\updi_link.c", "updi\updi_nvm.c", "updi\updi_device.c", "updi\updi_session.c",
    "hex\intel_hex.c"
) | ForEach-Object { Join-Path $root $_ }

$cc = $null
foreach ($cand in @("C:\Program Files\LLVM\bin\clang.exe", "C:\msys64\mingw64\bin\gcc.exe", "gcc", "clang")) {
    if (Get-Command $cand -ErrorAction SilentlyContinue) { $cc = $cand; break }
}
if (-not $cc) { throw "No host C compiler (clang/gcc) found." }

$out = Join-Path $root "host_test\run_tests.exe"
& $cc -Wall -Wextra -std=c11 -O1 -o $out @srcs
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
& $out
exit $LASTEXITCODE
