<#
.SYNOPSIS
  Prepares this PC to be the online server's storage node (server.py --role storage) - see
  vps_cutover.md. It only writes the storage key file; the firewall rule and the Project Index
  service change are printed for you to run at the switch (nothing is started or stopped).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File pc_storage_setup.ps1 -WhatIf
  powershell -ExecutionPolicy Bypass -File pc_storage_setup.ps1
  (asks for the key: paste the value of `sudo cat /etc/svr2011/storage_key` on the VPS)
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    # Where the key goes: beside Project Index's relay_secret.txt.
    [string]$KeyFile = "F:\ClaudeProjects\Project Index\Apps\SvR2011\service\storage_key.txt",
    # The key itself (else asked for, hidden).
    [string]$Key
)
$ErrorActionPreference = "Stop"

if (-not $Key) {
    $secure = Read-Host "Storage key (sudo cat /etc/svr2011/storage_key on the VPS)" -AsSecureString
    $Key = [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure))
}
$Key = $Key.Trim()
if ($Key -notmatch '^[0-9a-f]{64}$') { throw "that isn't a storage key (64 hex digits from the VPS's /etc/svr2011/storage_key)" }

$dir = Split-Path $KeyFile -Parent
if (-not (Test-Path $dir)) { throw "no folder $dir (-KeyFile <path>)" }
if (Test-Path $KeyFile) {
    $old = (Get-Content $KeyFile -Raw).Trim()
    if ($old -eq $Key) { Write-Host "$KeyFile has this key already" }
    elseif ($PSCmdlet.ShouldProcess($KeyFile, "replace the storage key")) {
        [IO.File]::WriteAllText($KeyFile, $Key + "`n", (New-Object Text.UTF8Encoding $false)); Write-Host "replaced: $KeyFile"
    }
} elseif ($PSCmdlet.ShouldProcess($KeyFile, "write the storage key")) {
    [IO.File]::WriteAllText($KeyFile, $Key + "`n", (New-Object Text.UTF8Encoding $false)); Write-Host "written: $KeyFile"
}
# only this user, SYSTEM and Administrators may read it
if ((Test-Path $KeyFile) -and $PSCmdlet.ShouldProcess($KeyFile, "limit who can read it")) {
    & icacls $KeyFile /inheritance:r /grant:r "${env:USERNAME}:R" "SYSTEM:F" "Administrators:F" | Out-Null
}

Write-Host ""
Write-Host "== At the switch (vps_cutover.md) - not done by this script =="
Write-Host ""
Write-Host "1) Windows firewall: the storage node's port, only from the VPS's tunnel address (admin PowerShell):"
Write-Host '   New-NetFirewallRule -DisplayName "SvR2011 storage node (WireGuard)" -Direction Inbound -Protocol TCP -LocalPort 8102 -RemoteAddress 10.8.0.1 -Action Allow -Profile Any'
Write-Host ""
Write-Host "2) Project Index, Apps\SvR2011\service\config.json: the storage node instead of the full server"
Write-Host '   "bind_host": "10.8.0.2",  "bind_port": 8102,  "role": "storage",'
Write-Host ('   "storage_key": "' + ($KeyFile -replace '\\', '\\') + '",')
Write-Host '   and remove "relay_udp_port" and "relay_edge" (the VPS core has the relay now).'
Write-Host ""
Write-Host "   and in service.py, add to sys.argv (after the relay lines):"
Write-Host '    "--role", CONFIG.get("role", "all"),'
Write-Host '    *(["--storage-key", CONFIG["storage_key"]] if CONFIG.get("storage_key") else []),'
Write-Host ""
Write-Host "   The node then runs: server.py --supervise --role storage --host 10.8.0.2 --port 8102"
Write-Host "   --data <Apps\SvR2011\data> --storage-key $KeyFile  (it serves data\blobs as they are)."
Write-Host "   The portal's /apps/svr2011 dashboard then has nothing on 127.0.0.1:8101: the dashboard is on"
Write-Host "   the VPS (ssh -L 8101:127.0.0.1:8101 <vps>, then http://127.0.0.1:8101/)."
