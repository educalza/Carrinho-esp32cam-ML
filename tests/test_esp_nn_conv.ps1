param(
    [string]$TflmSource = 'C:/Users/eduar/Documents/Arduino/libraries/tflm_esp32/src',
    [string]$CompilerDirectory = 'C:/Strawberry/c/bin',
    [ValidateSet('Os','O2')][string]$Optimization = 'Os',
    [ValidateSet(0,1)][int]$HotPathOptimization = 1
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo '.build/esp_nn_test'
$vendor = Join-Path $repo 'firmware/esp32cam_autonomous/src/esp_nn'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$nativeFlags = @("-$Optimization", "-DCAR_ML_OPTIMIZE_HOT_PATHS=$HotPathOptimization")
foreach ($source in @('esp_nn_conv_opt','esp_nn_conv_ansi')) {
    & "$CompilerDirectory/gcc.exe" -std=gnu11 @nativeFlags -I $vendor -c "$vendor/$source.c" -o "$output/$source.o"
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile $source" }
}
& "$CompilerDirectory/g++.exe" -std=c++17 @nativeFlags -I $TflmSource `
    "$PSScriptRoot/test_esp_nn_conv.cpp" "$TflmSource/tensorflow/lite/kernels/internal/common.cpp" `
    "$repo/firmware/esp32cam_autonomous/optimized_conv.cpp" `
    "$output/esp_nn_conv_opt.o" "$output/esp_nn_conv_ansi.o" -o "$output/test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Failed to compile reference comparison' }
& "$output/test.exe"
if ($LASTEXITCODE -ne 0) { throw 'ESP-NN/reference comparison failed' }
