param(
    [string]$Adb = 'adb',
    [string]$Serial = '',
    [string]$SuperKey = $env:KP_SUPERKEY,
    [string]$BuildDirectory = "$PSScriptRoot/../../build/jni-device",
    [string]$DexFile = "$PSScriptRoot/../../build/jni-device-dex/classes.dex",
    [string]$Module = "$PSScriptRoot/../../build/kpm-device/kpms/wxshadow/wxshadow.kpm"
)
$ErrorActionPreference = 'Stop'
if (!$SuperKey) { throw 'Set KP_SUPERKEY or pass -SuperKey; credentials are never written to files.' }
$jniAdbSelection = @()
if ($Serial) { $jniAdbSelection = @('-s', $Serial) }
function Invoke-Device {
    param([string[]]$Arguments)
    & $Adb @jniAdbSelection @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Device command failed (exit $LASTEXITCODE)." }
}
function Quote-Remote([string]$Value) { return "'" + $Value.Replace("'", "'\''") + "'" }
$jniStage = '/data/local/tmp/dobby-jni-test'
$jniKpatch = '/data/local/tmp/kpatch ' + (Quote-Remote $SuperKey)
$jniLoaded = (Invoke-Device -Arguments @('shell', "$jniKpatch kpm list")) -join "`n"
$jniPreexisting = $jniLoaded -match '(^|\s)wxshadow(\s|$)'
$jniFiles = @(
    (Join-Path $BuildDirectory 'libdobby_jni_device_static.so'),
    (Join-Path $BuildDirectory 'libdobby_jni_device_shared.so'),
    (Join-Path $BuildDirectory 'dobby/libdobby.so'),
    (Join-Path $BuildDirectory 'wxshadow_protocol_client'),
    $DexFile,
    $Module
)
foreach ($jniFile in $jniFiles) {
    if (!(Test-Path -LiteralPath $jniFile -PathType Leaf)) { throw "Missing build output: $jniFile" }
}
Invoke-Device -Arguments @('shell', "mkdir -p $jniStage")
Invoke-Device -Arguments (@('push') + $jniFiles[0..3] + @("$jniStage/"))
Invoke-Device -Arguments @('push', $DexFile, "$jniStage/classes.dex")
Invoke-Device -Arguments @('push', $Module, "$jniStage/wxshadow.kpm")
Invoke-Device -Arguments @('shell', "chmod 755 $jniStage/wxshadow_protocol_client")
function Invoke-JniTest([string]$Kind, [string]$Backend) {
    $jniCommand = "LD_LIBRARY_PATH=$jniStage CLASSPATH=$jniStage/classes.dex " +
        "app_process64 -Xcheck:jni /system/bin DobbyJniFixture $jniStage/libdobby_jni_device_$Kind.so $Backend"
    Invoke-Device -Arguments @('shell', $jniCommand)
}
foreach ($jniKind in @('static', 'shared')) { Invoke-JniTest $jniKind 'native' }
$jniOwnedModule = $false
try {
    if (!$jniPreexisting) {
        Invoke-Device -Arguments @('shell', "$jniKpatch kpm load $jniStage/wxshadow.kpm")
        $jniOwnedModule = $true
    }
    Invoke-Device -Arguments @('shell', "$jniStage/wxshadow_protocol_client --require")
    foreach ($jniKind in @('static', 'shared')) { Invoke-JniTest $jniKind 'shadow' }
} finally {
    if ($jniOwnedModule) {
        # Never unload a module installed by someone else, or unload our own
        # instance before all test processes exit and QUIESCE succeeds.
        try {
            Invoke-Device -Arguments @('shell', "$jniKpatch su -c " + (Quote-Remote "$jniStage/wxshadow_protocol_client quiesce"))
        } catch {
            Write-Warning 'QUIESCE failed. The module was left loaded to preserve notifier callbacks.'
            throw
        }
        Invoke-Device -Arguments @('shell', "$jniKpatch kpm unload wxshadow")
    }
}
Write-Output 'All ART JNI tests passed for static/shared libraries and native/WxShadow backends.'
