# Copies built drs_*.ofx.bundle folders into the OFX plugin folder (or removes them).
# Called elevated by `task ofx:install` / `task ofx:uninstall`. Keep this file ASCII.
param(
    [Parameter(Mandatory)][string]$Dest,
    [string]$Src = "",
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
try {
    New-Item -ItemType Directory -Force $Dest | Out-Null
    Get-ChildItem $Dest -Directory -Filter "drs_*.ofx.bundle" | Remove-Item -Recurse -Force
    if (-not $Uninstall) {
        Get-ChildItem $Src -Directory -Filter "drs_*.ofx.bundle" | Copy-Item -Destination $Dest -Recurse -Force
    }
} catch {
    Write-Host $_
    Read-Host "Failed. Press Enter to close"
    exit 1
}
