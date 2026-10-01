param(
    [string]$QtPrefix = 'D:/Qt6/6.10.3/mingw_64',
    [string]$CompilerBin = 'D:/Qt6/Tools/mingw1310_64/bin',
    [string]$CMake = 'D:/Qt6/Tools/CMake_64/bin/cmake.exe',
    [string]$Ninja = 'D:/Qt6/Tools/Ninja/ninja.exe',
    [ValidateRange(3,1000)][int]$LifecycleCycles = 100
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$build = Join-Path $projectRoot 'build-quality'
$ctest = Join-Path (Split-Path $CMake -Parent) 'ctest.exe'
$oldPath = $env:PATH
$oldCycles = $env:SENSOR_LIFECYCLE_CYCLES
try {
    $env:PATH = "$CompilerBin;$QtPrefix/bin;$oldPath"
    $env:SENSOR_LIFECYCLE_CYCLES = "$LifecycleCycles"
    & $CMake -S $projectRoot -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DCMAKE_CXX_COMPILER=$CompilerBin/g++.exe" "-DCMAKE_PREFIX_PATH=$QtPrefix" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
    if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
    & $CMake --build $build --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    & $ctest --test-dir $build --output-on-failure --output-junit (Join-Path $build 'test-results.xml')
    if ($LASTEXITCODE -ne 0) {
        Get-ChildItem -LiteralPath $build -Filter '*-results.txt' | ForEach-Object { Get-Content -LiteralPath $_.FullName }
        throw 'Tests failed; inspect the per-suite reports above'
    }
    Get-ChildItem -LiteralPath $build -Filter '*-results.txt' | ForEach-Object {
        Select-String -LiteralPath $_.FullName -Pattern 'Totals:|cycles|pipeline metrics|round-robin metrics'
    }
} finally {
    $env:PATH = $oldPath
    $env:SENSOR_LIFECYCLE_CYCLES = $oldCycles
}
