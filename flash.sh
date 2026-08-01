#!/bin/bash
set -e

PORT="/dev/ttyUSB0"
BAUD="9600"
HEX_FILE="firmware.hex"

echo "Writing fuses and flashing ATtiny85..."

avrdude -c stk500v1 -p t85 -P "$PORT" -b "$BAUD" \
  -U lfuse:w:0xE2:m \
  -U hfuse:w:0xDF:m \
  -U efuse:w:0xFF:m \
  -U flash:w:"$HEX_FILE":i

echo "Done!"