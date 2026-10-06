@echo off
REM build.bat - Convenience wrapper for build.ps1
REM Usage: build.bat bread-compact-wifi-s3cam
powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
