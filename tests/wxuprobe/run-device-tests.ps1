# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [string]$Adb = 'adb',
    [string]$Serial = '',
    [string]$SuperKey = $env:KP_SUPERKEY,
    [string]$BuildDirectory = "$PSScriptRoot/../../build/wxuprobe-device",
    [string]$Module = "$PSScriptRoot/../../build/wxuprobe-kpm/kpms/wxuprobe/wxuprobe.kpm",
    [string]$LogDirectory = "$PSScriptRoot/../../build/wxuprobe-device/logs"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$SuperKey) { throw 'Set KP_SUPERKEY or pass -SuperKey; credentials are never written to files.' }
$wuSelection = @()
if ($Serial) { $wuSelection = @('-s', $Serial) }
function Quote-Remote([string]$Value) { return "'" + $Value.Replace("'", "'\''") + "'" }
function Redact([string]$Value) { return $Value.Replace($SuperKey, '<redacted>') }
function Invoke-Device {
    param([string[]]$Arguments)
    $wuResult = (& $Adb @wuSelection @Arguments 2>&1 | ForEach-Object { $_.ToString() }) -join "`n"
    $wuExit = $LASTEXITCODE
    $wuResult = Redact $wuResult
    if ($wuExit -ne 0) { throw "Device command failed (exit $wuExit): $wuResult" }
    return $wuResult.TrimEnd()
}
$wuStage = '/data/local/tmp/wxuprobe-test'
$wuKpatch = '/data/local/tmp/kpatch ' + (Quote-Remote $SuperKey)
function Invoke-Root([string]$Command) {
    return Invoke-Device -Arguments @('shell', "$wuKpatch su -c " + (Quote-Remote $Command))
}
function Record([string]$Value) {
    $wuSafe = Redact $Value
    Write-Host $wuSafe
    Add-Content -LiteralPath (Join-Path $LogDirectory 'device.log') -Value $wuSafe -Encoding UTF8
}
$wuTests = Join-Path $BuildDirectory 'wxuprobe_device_tests'
$wuClient = Join-Path $BuildDirectory 'wxuprobe_client'
foreach ($wuFile in @($wuTests, $wuClient, $Module)) {
    if (!(Test-Path -LiteralPath $wuFile -PathType Leaf)) { throw "Missing build output: $wuFile" }
}
if ((Invoke-Device -Arguments @('get-state')) -ne 'device') { throw 'ADB device is not ready.' }
$wuExisting = Invoke-Device -Arguments @('shell', "$wuKpatch kpm list")
if ($wuExisting -match 'wxuprobe|wxshadow|kernel_trace') {
    throw 'A hook module is already loaded. Finish its hooks and QUIESCE/unload it before running this suite.'
}
if ((Invoke-Device -Arguments @('shell', 'id -u')) -ne '2000') {
    throw 'This suite requires the ordinary ADB shell uid 2000 to test the unprivileged path.'
}
if ((Invoke-Root 'id -u') -ne '0') { throw 'kpatch root is unavailable.' }
$wuTraceState = Invoke-Root 'cat /sys/kernel/tracing/tracing_on'
if ($wuTraceState -notin @('0', '1')) { throw 'tracefs tracing_on is unavailable.' }
# printk's clock can exclude suspend time while /proc/uptime includes it.
# Use the last actual dmesg timestamp as the boundary, on that same clock.
$wuBeforeDmesg = Invoke-Root 'dmesg'
$wuTimestamps = [regex]::Matches($wuBeforeDmesg, '(?m)^\[\s*(\d+(?:\.\d+)?)\]')
if (!$wuTimestamps.Count) { throw 'Cannot establish the kernel log boundary.' }
$wuStartDmesg = [double]::Parse($wuTimestamps[$wuTimestamps.Count - 1].Groups[1].Value,
    [Globalization.CultureInfo]::InvariantCulture)
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
Set-Content -LiteralPath (Join-Path $LogDirectory 'device.log') -Value "wxuprobe device run: $(Get-Date -Format o)" -Encoding UTF8
Record (Invoke-Device -Arguments @('shell', 'uname -a'))
Record ("KPM SHA256=" + (Get-FileHash -LiteralPath $Module -Algorithm SHA256).Hash)
Record (Invoke-Device -Arguments @('shell', "mkdir -p $wuStage"))
Record (Invoke-Device -Arguments @('push', $wuTests, "$wuStage/wxuprobe_device_tests"))
Record (Invoke-Device -Arguments @('push', $wuClient, "$wuStage/wxuprobe_client"))
Record (Invoke-Device -Arguments @('push', $Module, "$wuStage/wxuprobe.kpm"))
Invoke-Device -Arguments @('shell', "chmod 755 $wuStage/wxuprobe_device_tests $wuStage/wxuprobe_client") | Out-Null
Record (Invoke-Device -Arguments @('shell', "$wuStage/wxuprobe_device_tests --expect-unavailable"))
$wuOwned = $false
$wuTraceChanged = $false
try {
    Invoke-Device -Arguments @('shell', "$wuKpatch kpm load $wuStage/wxuprobe.kpm") | Out-Null
    $wuOwned = $true
    if ($wuTraceState -eq '0') {
        Invoke-Root 'echo 1 > /sys/kernel/tracing/tracing_on' | Out-Null
        $wuTraceChanged = $true
    }
    $wuBasic = Invoke-Device -Arguments @('shell', "$wuStage/wxuprobe_device_tests --basic")
    Record $wuBasic
    Record (Invoke-Device -Arguments @('shell', "$wuStage/wxuprobe_device_tests --remote"))
    $wuFull = Invoke-Root "$wuStage/wxuprobe_device_tests"
    Record $wuFull
    Record (Invoke-Root "$wuStage/wxuprobe_device_tests --leave-active")
    # Read the snapshot, never consume trace_pipe or clear the global buffer.
    $wuTrace = Invoke-Root 'cat /sys/kernel/tracing/trace'
    $wuTraceLines = @($wuTrace -split "`n" | Where-Object { $_ -match '\+Test-Log\+ wxuprobe ' })
    Set-Content -LiteralPath (Join-Path $LogDirectory 'trace.log') -Value $wuTraceLines -Encoding UTF8
    foreach ($wuMatch in [regex]::Matches("$wuBasic`n$wuFull", 'trace_name=(\S+)')) {
        if (!$wuTrace.Contains("fun_name:$($wuMatch.Groups[1].Value),")) {
            throw "Missing tracefs entry for $($wuMatch.Groups[1].Value)."
        }
    }
    if ([regex]::Matches("$wuBasic`n$wuFull", 'trace_name=(\S+)').Count -ne 2) {
        throw 'Tests did not emit both expected trace labels.'
    }
    Record 'PASS tracefs: both current test processes emitted +Test-Log+ records'
} finally {
    try {
        if ($wuOwned) {
            try {
                # prctl runs outside the KPM loader's RCU critical section.
                Record (Invoke-Root "$wuStage/wxuprobe_client quiesce")
            } catch {
                Write-Warning 'QUIESCE failed. The module was left loaded to preserve its callbacks.'
                throw
            }
            Invoke-Device -Arguments @('shell', "$wuKpatch kpm unload wxuprobe") | Out-Null
            $wuOwned = $false
            Record 'PASS module QUIESCE and unload'
        }
    } finally {
        if ($wuTraceChanged) { Invoke-Root "echo $wuTraceState > /sys/kernel/tracing/tracing_on" | Out-Null }
    }
}
Record (Invoke-Device -Arguments @('shell', "$wuStage/wxuprobe_device_tests --expect-unavailable"))
$wuDmesg = Invoke-Root 'dmesg'
$wuNewDmesg = @($wuDmesg -split "`n" | Where-Object {
    if ($_ -match '^\[\s*(\d+(?:\.\d+)?)\]') {
        [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) -gt $wuStartDmesg
    }
})
Set-Content -LiteralPath (Join-Path $LogDirectory 'dmesg.log') -Value $wuNewDmesg -Encoding UTF8
if (!(($wuNewDmesg -join "`n").Contains('wxuprobe: ready')) -or
    !(($wuNewDmesg -join "`n").Contains('wxuprobe: unloaded'))) {
    throw 'Kernel log does not cover both load and unload; inspect dmesg.log.'
}
if ($wuNewDmesg -match 'BUG:|WARNING:|Call trace:|Internal error:|UNSAFE UNLOAD|KASAN:|KCSAN:') {
    throw 'Kernel diagnostics appeared during this run; inspect dmesg.log.'
}
if ((Invoke-Device -Arguments @('shell', "$wuKpatch kpm list")) -match 'wxuprobe') {
    throw 'wxuprobe is still in the loaded module list.'
}
Record 'ALL WXUPROBE DEVICE CHECKS PASSED; tracing_on restored and module unloaded'
