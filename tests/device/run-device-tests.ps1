param(
    [string]$Adb = 'adb',
    [string]$Serial = '',
    [string]$SuperKey = $env:KP_SUPERKEY,
    [string]$BuildDirectory = "$PSScriptRoot/../../build/device",
    [string]$Module = "$PSScriptRoot/../../build/kpm-device/kpms/wxshadow/wxshadow.kpm"
)
$ErrorActionPreference = 'Stop'
if (!$SuperKey) { throw 'Set KP_SUPERKEY or pass -SuperKey; the key is not stored by this script.' }
$wxAdbSelection = @()
if ($Serial) { $wxAdbSelection = @('-s', $Serial) }
function Invoke-Device {
    param([string[]]$Arguments)
    & $Adb @wxAdbSelection @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Device command failed (exit $LASTEXITCODE)." }
}
function Quote-Remote([string]$Value) { return "'" + $Value.Replace("'", "'\''") + "'" }
$wxStage = '/data/local/tmp/wxshadow-dobby-test'
$wxKpatch = '/data/local/tmp/kpatch ' + (Quote-Remote $SuperKey)
$wxLoaded = (Invoke-Device -Arguments @('shell', "$wxKpatch kpm list")) -join "`n"
if ($wxLoaded -match '(^|\s)wxshadow(\s|$)') {
    throw 'wxshadow is already loaded. Finish its client hooks and quiesce/unload it before running this suite.'
}
$wxFiles = @(
    (Join-Path $BuildDirectory 'wxshadow_device_static'),
    (Join-Path $BuildDirectory 'wxshadow_device_shared'),
    (Join-Path $BuildDirectory 'wxshadow_protocol_client'),
    (Join-Path $BuildDirectory 'dobby/libdobby.so'),
    $Module
)
foreach ($wxFile in $wxFiles) { if (!(Test-Path -LiteralPath $wxFile -PathType Leaf)) { throw "Missing build output: $wxFile" } }
Invoke-Device -Arguments @('shell', "mkdir -p $wxStage")
# The module is pushed separately so a differently named local file still gets
# the expected device name. Authentication arguments are never printed.
Invoke-Device -Arguments (@('push') + $wxFiles[0..3] + @("$wxStage/"))
Invoke-Device -Arguments @('push', $Module, "$wxStage/wxshadow.kpm")
Invoke-Device -Arguments @('shell', "chmod 755 $wxStage/wxshadow_device_static $wxStage/wxshadow_device_shared $wxStage/wxshadow_protocol_client")
foreach ($wxKind in @('static', 'shared')) {
    Invoke-Device -Arguments @('shell', "LD_LIBRARY_PATH=$wxStage $wxStage/wxshadow_device_$wxKind --expect-unavailable")
    Invoke-Device -Arguments @('shell', "LD_LIBRARY_PATH=$wxStage $wxStage/wxshadow_device_$wxKind --native")
}
$wxOwnedModule = $false
try {
    Invoke-Device -Arguments @('shell', "$wxKpatch kpm load $wxStage/wxshadow.kpm")
    $wxOwnedModule = $true
    Invoke-Device -Arguments @('shell', "$wxStage/wxshadow_protocol_client --require")
    foreach ($wxKind in @('static', 'shared')) {
        Invoke-Device -Arguments @('shell', "LD_LIBRARY_PATH=$wxStage $wxStage/wxshadow_device_$wxKind")
    }
} finally {
    if ($wxOwnedModule) {
        # QUIESCE must execute in an ordinary prctl process context. KPM ctl0
        # and exit run under the loader's RCU read lock and cannot drain SRCU.
        try {
            Invoke-Device -Arguments @('shell', "$wxKpatch su -c " + (Quote-Remote "$wxStage/wxshadow_protocol_client quiesce"))
        } catch {
            Write-Warning 'QUIESCE failed. The module was left loaded to preserve notifier callbacks.'
            throw
        }
        Invoke-Device -Arguments @('shell', "$wxKpatch kpm unload wxshadow")
    }
}
foreach ($wxKind in @('static', 'shared')) {
    Invoke-Device -Arguments @('shell', "LD_LIBRARY_PATH=$wxStage $wxStage/wxshadow_device_$wxKind --expect-unavailable")
}
Write-Output 'All device tests passed; wxshadow was quiesced and unloaded.'
