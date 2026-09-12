$ErrorActionPreference = "Stop"
Write-Host "Removing old full-screen blink frames..."
if (Test-Path "main\koyoda_half.c") { Remove-Item -Force "main\koyoda_half.c" }
if (Test-Path "main\koyoda_closed.c") { Remove-Item -Force "main\koyoda_closed.c" }
Write-Host "Done."
