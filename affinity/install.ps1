<#
.SYNOPSIS
    Installs the OfflineTags filter plug-in for Affinity.

.DESCRIPTION
    Copies affinity\build\OfflineTags.8bf into a plug-in folder and records where
    the offlinetags-bridge client is, so the plug-in can start it. Affinity's own
    settings are not touched: the folder has to be added there once, see the
    steps printed at the end.

.PARAMETER PluginDir
    Folder to copy the plug-in into. Default: %USERPROFILE%\.offlinetags-bridge\plugins

.PARAMETER Client
    Path to offlinetags-bridge.exe. Defaults to the one in this checkout's .venv.

.PARAMETER Uninstall
    Remove the plug-in and the recorded client path.
#>
param(
    [string]$PluginDir = (Join-Path $env:USERPROFILE '.offlinetags-bridge\plugins'),
    [string]$Client = (Join-Path $PSScriptRoot '..\.venv\Scripts\offlinetags-bridge.exe'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$stateDir = Join-Path $env:USERPROFILE '.offlinetags-bridge'
$clientFile = Join-Path $stateDir 'client.txt'
$target = Join-Path $PluginDir 'OfflineTags.8bf'

if ($Uninstall) {
    Remove-Item $target, $clientFile -ErrorAction SilentlyContinue
    Write-Host "Removed the plug-in. You can also remove $PluginDir from Affinity's plug-in search folders."
    return
}

$plugin = Join-Path $PSScriptRoot 'build\OfflineTags.8bf'
if (-not (Test-Path $plugin)) {
    throw "$plugin not found. Run affinity\build.ps1 first."
}
if (-not (Test-Path $Client)) {
    throw "$Client not found. Run 'pip install -e .' in the checkout first, or pass -Client."
}

New-Item -ItemType Directory -Force $PluginDir | Out-Null
try {
    Copy-Item $plugin $target -Force
} catch {
    throw "Could not replace $target. Close Affinity and run this again. ($($_.Exception.Message))"
}

# UTF-8 without a byte order mark
[IO.File]::WriteAllText($clientFile, (Resolve-Path $Client).Path, (New-Object Text.UTF8Encoding $false))

Write-Host "Installed $target"
Write-Host ''
Write-Host 'One-time setup in Affinity:'
Write-Host '  1. Open Settings and go to "Photoshop Plugins".'
Write-Host "  2. Add this plug-in search folder: $PluginDir"
Write-Host '  3. Tick "Allow unknown plugins to be used".'
Write-Host '  4. Restart Affinity.'
Write-Host 'Then select a pixel layer and choose Pixel > Filters > Plugins > OfflineTags > Analyze.'
