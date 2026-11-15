@echo off
cd /d %~dp0
title MQTT Broker (TCP 1883 / WS 8083)

rem Use bundled portable runtime first; fall back to system Node.js if missing.
set "NODE=%~dp0runtime\node.exe"
if not exist "%NODE%" set "NODE=node"
"%NODE%" --version >nul 2>&1
if errorlevel 1 (
  echo [ERROR] No usable Node.js runtime found.
  pause
  exit /b 1
)

"%NODE%" broker.js
pause
