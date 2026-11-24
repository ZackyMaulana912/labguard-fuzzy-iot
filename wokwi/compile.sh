#!/bin/bash
# Compile sketch.ino untuk simulasi Wokwi di VS Code atau Antigravity.
# arduino-cli mewajibkan nama file .ino sama dengan nama foldernya,
# jadi sketch.ino disalin dulu ke build/sketch/ lalu di-compile dari sana.
# Hasilnya (build/sketch.ino.bin dan .elf) dibaca oleh wokwi.toml.
set -e
cd "$(dirname "$0")"
mkdir -p build/sketch
cp sketch.ino build/sketch/sketch.ino
arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir build build/sketch
echo "Compile selesai. Jalankan simulasi: tekan F1, pilih Wokwi: Start Simulator"
