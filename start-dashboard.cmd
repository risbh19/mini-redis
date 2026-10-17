@echo off
setlocal
cd /d "%~dp0"
set "MINI_REDIS_BINARY=build/mini-redis"
if exist "build-clang\mini-redis" set "MINI_REDIS_BINARY=build-clang/mini-redis"
echo Starting Mini Redis Data Studio in WSL Ubuntu...
echo Open http://localhost:8080 when the ready message appears.
echo Keep this window open. Press Ctrl+C to stop.
wsl -d Ubuntu -- python3 web/server.py --binary "%MINI_REDIS_BINARY%"
if errorlevel 1 pause
