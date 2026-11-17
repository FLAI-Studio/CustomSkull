#requires -Version 5.1
# Local, offline build helper for this development machine.
# The normal build path is `xmake`; this script only exists because the
# development sandbox has no access to GitHub for xmake repositories.
param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$objDir = Join-Path $build "obj"
$preDir = Join-Path $build "prelink"
$outDll = Join-Path $build "CustomSkull.dll"

New-Item -ItemType Directory -Force $objDir, (Join-Path $preDir "lib") | Out-Null

function Find-PackageDir([string]$name, [string]$versionLike) {
    $base = Join-Path $env:LOCALAPPDATA ".xmake\packages"
    $candidates = Get-ChildItem (Join-Path $base $name) -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like $versionLike } |
        Sort-Object Name -Descending
    foreach ($candidate in $candidates) {
        $inner = Get-ChildItem $candidate.FullName -Directory -ErrorAction SilentlyContinue
        foreach ($dir in $inner) {
            if (Test-Path (Join-Path $dir.FullName "manifest.txt")) {
                return $dir.FullName
            }
        }
    }
    throw "Cannot find package $name $versionLike"
}

$ll = Find-PackageDir "l\levilamina" "26.60.0"
$bedrockData = Find-PackageDir "b\bedrockdata" "v26.60.6-server.5"
$prelink = Find-PackageDir "p\prelink" "v0.8.8"
$fmt = Find-PackageDir "f\fmt" "11.2.0"
$leveldb = Find-PackageDir "l\leveldb" "1.23"
$snappy = Find-PackageDir "s\snappy" "1.2.2"
$symbolProvider = Find-PackageDir "s\symbolprovider" "v1.3.0"

$msvcRoot = "C:\soft\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
$msvc = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkInclude = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$sdkLib = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"

$pc = Get-Content (Join-Path $ll "lib\pkgconfig\levilamina.pc") -Raw
$cflagsLine = ($pc -split "`n" | Where-Object { $_ -match "^Cflags:" }) -replace "^Cflags:\s*", ""
$pkgIncludes = [regex]::Matches($cflagsLine, "-I(\S+)") | ForEach-Object { $_.Groups[1].Value }

$includes = @((Join-Path $ll "include"), (Join-Path $root "src")) + $pkgIncludes

$compile = @(
    "/nologo", "/c", "/std:c++20", "/EHsc", "/utf-8", "/MD", "/O2", "/DNOMINMAX", "/DUNICODE",
    "/D_HAS_CXX23=1", "/DLL_PLAT_S", "/DCS_LL_MAJOR=26", "/DCS_LL_MINOR=60", "/DENTT_PACKED_PAGE=128", "/DENTT_SPARSE_PAGE=4096", "/DENTT_NO_MIXIN",
    "/imsvc", (Join-Path $msvc "include"),
    "/imsvc", (Join-Path $sdkInclude "ucrt"),
    "/imsvc", (Join-Path $sdkInclude "shared"),
    "/imsvc", (Join-Path $sdkInclude "um"),
    "/imsvc", (Join-Path $sdkInclude "winrt")
)
foreach ($include in $includes) { $compile += "/I$include" }
$compile += "/W3"

$objFiles = @()
Get-ChildItem (Join-Path $root "src") -Recurse -Filter "*.cpp" | ForEach-Object {
    $relative = $_.FullName.Substring($root.Length + 1) -replace "[\\/:]", "_"
    $obj = Join-Path $objDir ($relative + ".obj")
    $args = $compile + @("/Fo$obj", $_.FullName)
    Write-Host "clang-cl $($_.Name)"
    & "C:\Program Files\LLVM\bin\clang-cl.exe" @args
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($_.FullName)" }
    $objFiles += $obj
}

$data = Join-Path $bedrockData "bin\bedrock_runtime_data"
$prelinkExe = Join-Path $prelink "bin\prelink.exe"
$prelinkArgs = @("server-windows-x64", $preDir, $data) + $objFiles + @((Join-Path $ll "lib\LeviLamina.lib"))
Write-Host "prelink"
& $prelinkExe @prelinkArgs
if ($LASTEXITCODE -ne 0) { throw "prelink failed" }

$link = @(
    "/LD"
) + $objFiles + @(
    "/link", "/OUT:$outDll", "/DELAYLOAD:bedrock_runtime.dll",
    "/LIBPATH:$(Join-Path $preDir "lib")", "bedrock_runtime_api.lib",
    "/LIBPATH:$(Join-Path $ll "lib")", "LeviLamina.lib",
    "/LIBPATH:$(Join-Path $fmt "lib")", "fmt.lib",
    "/LIBPATH:$(Join-Path $leveldb "lib")", "leveldb.lib",
    "/LIBPATH:$(Join-Path $snappy "lib")", "snappy.lib",
    "/LIBPATH:$(Join-Path $symbolProvider "lib")", "SymbolProvider.lib",
    "winhttp.lib",
    "/LIBPATH:$(Join-Path $msvc "lib\x64")",
    "/LIBPATH:$(Join-Path $sdkLib "ucrt\x64")",
    "/LIBPATH:$(Join-Path $sdkLib "um\x64")"
)
Write-Host "link"
& "C:\Program Files\LLVM\bin\clang-cl.exe" @link
if ($LASTEXITCODE -ne 0) { throw "link failed" }

$package = Join-Path $root "bin\CustomSkull"
New-Item -ItemType Directory -Force $package | Out-Null
Copy-Item $outDll (Join-Path $package "CustomSkull.dll") -Force
$manifest = (Get-Content (Join-Path $root "manifest.json") -Raw).Replace('${modVersion}', '26.60.3')
[IO.File]::WriteAllText(
    (Join-Path $package "manifest.json"),
    $manifest,
    (New-Object Text.UTF8Encoding($false))
)
Write-Host "built $package"






