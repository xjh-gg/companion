param(
    [string]$SourceRoot = 'C:\Users\x1773\AppData\Local\Temp\xiaozhi_src\xiaozhi-esp32-2.0.3'
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$companion = Join-Path $SourceRoot 'main\companion'
$cjsonDir = [IO.Path]::Combine($SourceRoot, 'managed_components', 'espressif__cjson', 'cJSON')

foreach ($f in @(
    (Join-Path $companion 'state_machine.c'),
    (Join-Path $companion 'event_queue.c'),
    (Join-Path $companion 'companion_event_log.c'),
    (Join-Path $companion 'state_machine.h'),
    (Join-Path $companion 'event_queue.h'),
    (Join-Path $companion 'companion_audio.h'),
    (Join-Path $companion 'companion_led.h'),
    (Join-Path $companion 'motion_detect.c'),
    (Join-Path $companion 'motion_detect.h')
)) {
    if (-not (Test-Path $f)) {
        throw "缺少被测源码或头文件: $f"
    }
}

$out = Join-Path $here 'host_tests.exe'
& gcc -std=gnu11 -Wall -Wextra -Werror `
    -I (Join-Path $here 'stubs') -I $companion `
    (Join-Path $here 'freertos_stub.c') `
    (Join-Path $companion 'event_queue.c') `
    (Join-Path $companion 'companion_event_log.c') `
    (Join-Path $companion 'state_machine.c') `
    (Join-Path $companion 'motion_detect.c') `
    (Join-Path $here 'test_main.c') `
    -o $out

if ($LASTEXITCODE -ne 0) {
    throw "主机测试编译失败，退出码 $LASTEXITCODE"
}

& $out
$c_result = $LASTEXITCODE

$protocolDir = Join-Path $SourceRoot 'main\protocols'
$out_cpp = Join-Path $here 'host_protocol_tests.exe'
& g++ -std=gnu++17 -Wall -Wextra -Werror `
    -I (Join-Path $here 'stubs') -I $protocolDir `
    (Join-Path $protocolDir 'protocol.cc') `
    (Join-Path $here 'test_protocol.cc') `
    -o $out_cpp

if ($LASTEXITCODE -ne 0) {
    throw "协议主机测试编译失败，退出码 $LASTEXITCODE"
}

& $out_cpp
$cpp_result = $LASTEXITCODE

# MQTT 命令协议纯校验测试（真实 cJSON）
$mqtt_result = 0
if ((Test-Path $cjsonDir) -and (Test-Path (Join-Path $companion 'companion_mqtt_codec.c'))) {
    $cjson_obj = Join-Path $here 'cJSON.o'
    & gcc -std=gnu11 -c (Join-Path $cjsonDir 'cJSON.c') -I $cjsonDir -o $cjson_obj
    if ($LASTEXITCODE -ne 0) { throw "cJSON 编译失败，退出码 $LASTEXITCODE" }

    $out_mqtt = Join-Path $here 'host_mqtt_tests.exe'
    & gcc -std=gnu11 -Wall -Wextra -Werror `
        -I $companion -I $cjsonDir `
        (Join-Path $companion 'companion_mqtt_codec.c') `
        $cjson_obj `
        (Join-Path $here 'test_companion_mqtt.c') `
        -o $out_mqtt
    if ($LASTEXITCODE -ne 0) { throw "MQTT 命令协议测试编译失败，退出码 $LASTEXITCODE" }

    & $out_mqtt
    $mqtt_result = $LASTEXITCODE
} else {
    Write-Warning "跳过 MQTT 命令协议测试：未找到 cJSON 或 companion_mqtt_codec.c"
}

if ($c_result -ne 0 -or $cpp_result -ne 0 -or $mqtt_result -ne 0) {
    exit 1
}
exit 0
