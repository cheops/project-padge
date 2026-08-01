# ATtiny85 Flashing & Fuse Programming Guide

This repository includes both PlatformIO CLI commands and standalone scripts (`flash.sh`, `flash.bat`) to set fuses and upload firmware to the **ATtiny85** in a single step.

All target configuration options—including programmer settings, serial port, baud rate, fuse values, and the target `.hex` file location—are configured exclusively in `platformio.ini` as the single source of truth.

---

## Hardware Notice

> **Important:** Always disconnect your programmer from the ATtiny85 badge after flashing. The ISP lines share physical pins with I2C (SDA/SCL) and the shared button interrupt pin (`PB1`). Leaving a programmer connected will hold or glitch these lines and prevent normal operation.

---

## Method 1: PlatformIO (CLI & Integrated Workflow)

PlatformIO natively supports setting fuses and uploading sequentially without modifying your environment setup.

### Option A: Single Command Target Chaining (Recommended)

Start a `PlatformIO Core CLI` promt form the command window (CTRL + SHIFT + P)  
Run both the `fuses` and `upload` targets together in your terminal:


```bash
pio run -t fuses -t upload
```



---

## Method 2: Standalone Shell Scripts (`avrdude`)

If you are deploying a compiled binary on a system without PlatformIO installed, use the included helper scripts. They execute a unified `avrdude` command to program both fuses and flash memory in one pass.  
Download `firmware.hex` from the releases https://github.com/cheops/project-padge/releases  

### Linux / macOS (`flash.sh`)

```bash
./flash.sh
```

*(Optional: Override default port)*
```bash
./flash.sh /dev/ttyUSB1
```

### Windows (`flash.bat`)

```cmd
flash.bat
```

---

## Underlying `avrdude` Execution

Both helper scripts execute this single command under the hood:

```bash
avrdude -c stk500v1 -p t85 -P /dev/ttyUSB0 -b 9600 \
  -U lfuse:w:0xE2:m \
  -U hfuse:w:0xDF:m \
  -U efuse:w:0xFF:m \
  -U flash:w:firmware.hex:i
```