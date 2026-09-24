param([string]$Port = 'COM24')
$ErrorActionPreference = 'Stop'
$firmwareDir = Join-Path $PSScriptRoot 'firmware'
python -m esptool --chip esp32c6 --port $Port --baud 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 4MB 0x0 (Join-Path $firmwareDir 'bootloader.bin') 0x8000 (Join-Path $firmwareDir 'partitions.bin') 0xf000 (Join-Path $firmwareDir 'ota_data_initial.bin') 0x20000 (Join-Path $firmwareDir 'firmware.bin')
if ($LASTEXITCODE -ne 0) { throw 'Firmware upload failed' }
