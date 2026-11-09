@echo off
rem Double-click wrapper for BuildAndRunStandalone.ps1 (Development, incremental). Arguments pass through, e.g. -Config Shipping -Clean.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0BuildAndRunStandalone.ps1" %*
if errorlevel 1 pause
