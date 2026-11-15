# Quadruped Ops - stop local services started by start.bat
# Stops broker.js / voice-gateway.js / server.js / mediamtx.exe
# and closes leftover cmd windows. Ollama is untouched (shared local service).
#
# Do NOT filter by process name (services may run as node.exe or Kimi.exe).
# Match by script/exe name in CommandLine, but exclude daimon/process-runner
# supervision chains (their command lines also carry those names).
$ErrorActionPreference = 'SilentlyContinue'

$pattern = '(broker|voice-gateway|server)\.js|mediamtx\.exe'
$procs = Get-CimInstance Win32_Process |
  Where-Object {
    $_.CommandLine -match $pattern -and
    $_.CommandLine -notmatch 'daimon|process-runner|command-process' -and
    $_.ProcessId -ne $PID
  }

if ($procs) {
  foreach ($p in $procs) {
    $what = [regex]::Match($p.CommandLine, $pattern).Value
    Write-Host "   stop $what (PID $($p.ProcessId), $($p.Name))"
    Stop-Process -Id $p.ProcessId -Force
  }
} else {
  Write-Host "   no running services found"
}

# extra sweep by image name (covers instances started outside start.bat)
Get-Process mediamtx -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Host "   stop mediamtx.exe (PID $($_.Id))"
  Stop-Process -Id $_.Id -Force
}

Start-Sleep -Milliseconds 500

# Close cmd windows left by start.bat
foreach ($t in 'MQTT Broker', 'Voice Gateway', 'MediaMTX', 'Quadruped Ops Console - Local Server') {
  taskkill /F /T /FI "WINDOWTITLE eq $t" 2>&1 | Out-Null
}
