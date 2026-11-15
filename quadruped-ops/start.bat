@echo off
cd /d %~dp0
title Quadruped Ops Console - Local Server

rem Use the bundled portable runtime first (its version matches node_modules
rem native modules exactly); fall back to system Node.js only if missing.
set "NODE=%~dp0runtime\node.exe"
if not exist "%NODE%" set "NODE=node"
"%NODE%" --version >nul 2>&1
if errorlevel 1 (
  echo [ERROR] No usable Node.js runtime found.
  echo Bundled runtime missing ^(runtime\node.exe^) and no system Node.js in PATH.
  echo Install Node.js 18+ from https://nodejs.org or restore the runtime folder.
  pause
  exit /b 1
)
echo Node runtime: %NODE%

echo Starting MediaMTX (RTSP 8554 / WebRTC 8889)...
start "MediaMTX" cmd /k "mediamtx\mediamtx.exe mediamtx\mediamtx.yml"
timeout /t 1 /nobreak >nul
echo Starting MQTT Broker (TCP 1883 / WS 8083)...
start "MQTT Broker" cmd /k ""%NODE%" broker.js"
timeout /t 2 /nobreak >nul
echo Starting Voice Gateway (ASR + LLM)...
start "Voice Gateway" cmd /k ""%NODE%" voice-gateway.js"
timeout /t 1 /nobreak >nul
echo Starting dashboard server (8080)...
start "" http://localhost:8080
"%NODE%" server.js
pause
