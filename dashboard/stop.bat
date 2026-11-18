@echo off
rem ============================================================
rem  Quadruped Ops Console - Stop Script
rem  Stops: MQTT Broker (broker.js, TCP 1883 / WS 8083)
rem         Voice Gateway (voice-gateway.js)
rem         Dashboard   (server.js, port 8080)
rem  Keeps: Ollama (shared local LLM service, untouched)
rem ============================================================
cd /d %~dp0
title Quadruped Ops - Stop

echo.
echo  Stopping Quadruped Ops services...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0stop.ps1"

echo.
echo  Done. All dashboard services stopped (Ollama left running).
rem 带参数调用时跳过 pause，方便脚本/自动化使用
if "%~1"=="" pause
