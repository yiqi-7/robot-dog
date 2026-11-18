$ErrorActionPreference = 'Continue'
$wd = 'C:\Users\Administrator\Documents\kimi\tasks\2026-09-10\02-27-37-b71b13a2\quadruped-ops'

# 停掉旧的 server.js / voice-gateway.js
Get-CimInstance Win32_Process -Filter "Name='node.exe'" |
  Where-Object { $_.CommandLine -match 'voice-gateway\.js|server\.js' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Start-Sleep -Milliseconds 800

Start-Process -FilePath 'node' -ArgumentList 'server.js' -WorkingDirectory $wd -WindowStyle Hidden `
  -RedirectStandardOutput "$wd\server.log" -RedirectStandardError "$wd\server.err"
Start-Process -FilePath 'node' -ArgumentList 'voice-gateway.js' -WorkingDirectory $wd -WindowStyle Hidden `
  -RedirectStandardOutput "$wd\voice-gateway.log" -RedirectStandardError "$wd\voice-gateway.err"

Start-Sleep -Seconds 4
Get-CimInstance Win32_Process -Filter "Name='node.exe'" |
  Where-Object { $_.CommandLine -match 'voice-gateway\.js|server\.js' } |
  ForEach-Object { Write-Output ($_.ProcessId.ToString() + ' :: ' + $_.CommandLine) }
