@echo off
cd /d %~dp0
echo Starting PHF-RDP Server (Native C Engine)...
echo Access the dashboard in your browser: http://localhost:8080
server\phf-rdp-server.exe %*
