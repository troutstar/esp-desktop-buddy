# CYD ESP32 Hardware & Toolchain Reference

This file is the single reference for all CYD projects. It covers the hardware,
how to access it at the driver level, and the build/flash workflow.

Claude should handle build, flash, and serial operations directly using the
boilerplate below — do not hand these back to the user unless the operation
requires physical interaction (e.g., holding BOOT button).

---

## Hardware: ESP32-2432S028 ("Cheap Yellow Display")

### MCU
- **Chip:** ESP32-D0WD-V3 rev 3.1
- **Cores:** 2× Xtensa LX6 @ 240 MHz, hardware FPU (single-precision float)
- **RAM:** ~320 KB total SRAM; ~184 KB DMA-capable internal DRAM available at runtime
  after IDF overhead
- **Flash:** 4 MB

### Display
- **Controller:** ILI9341V
- **Size:** 2.8", 320×240 pixels, landscape orientation
- **Color format:** RGB565 (16-bit), **big-endian** for SPI DMA
  - Pixel layout in memory: bytes swapped — store as `(rgb >> 8) | (rgb << 8)`
- **Interface:** SPI (SPI2_HOST), 40 MHz — can attempt 80 MHz if signal is stable
- **Orientation:** MADCTL 0x28 — MV=1 (landscape), BGR=1 (panel colour order)
  - Portrait: MADCTL 0x00; if R/B appear swapped: MADCTL 0x20
- **Frame rate:** ~79 Hz configured (FRMCTR1 0x00, 0x18)

### Pin Assignments (confirmed, do not change)
| Signal    | GPIO |
|-----------|------|
| SPI CS    | 15   |
| SPI DC    | 2    |
| SPI MOSI  | 13   |
| SPI MISO  | 12   |
| SPI CLK   | 14   |
| Backlight | 21   |

Backlight is active-high, GPIO output. Turn off during display init to avoid a
flash of uninitialised GRAM; turn on after DISPON + 100 ms delay.

### Memory Constraints
- **Single framebuffer only.** Full-frame buffer = 320 × 240 × 2 = 153,600 bytes.
  Double-buffering (307,200 bytes) exceeds available DMA-capable DRAM.
- Allocate with: `heap_caps_malloc(ILI_W * ILI_H * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`
- After allocation, ~30 KB DMA DRAM remains for task stacks and other uses.

### USB–Serial
- **Chip:** Silicon Labs CP210x or USB-SERIAL CH340 (board-dependent)
- Identify with: `Get-WmiObject Win32_SerialPort | Select-Object Name, DeviceID, Description`
- Ignore "Standard Serial over Bluetooth link" entries — that's not the CYD.
- **Always ask the user which COM port before flashing.** Do not assume COM5 or any other port.

---

## Driver Patterns

### ILI9341 Init (ili9341.c)
Full init sequence is in `ili9341_init()`. Key steps:
1. Configure BL and DC as GPIO outputs, BL off
2. Init SPI2 bus with `max_transfer_sz = ILI_W * ILI_H * 2` (enables DMA chaining)
3. Add device at 40 MHz, mode 0, pre-transfer callback sets DC pin
4. Send ILI9341 init sequence (SWRESET → SLPOUT → power/timing → MADCTL → COLMOD → gamma)
5. DISPON, 100 ms delay, BL on

### Async DMA Blit (the render loop)
```c
while (1) {
    render_step(&state, fb);         // CPU: draw into framebuffer
    ili9341_blit_async(spi, fb);     // SPI: queue full-frame DMA (non-blocking)
    ili9341_blit_wait(spi);          // CPU: wait for DMA to finish
}
```
- `blit_async` calls `set_window(0,0,319,239)` then queues a 76,800-pixel DMA transfer
- `blit_wait` blocks until the in-flight transaction completes
- CPU computation and SPI transfer overlap: compute next frame while DMA sends current

### Color Encoding
```c
static inline uint16_t rgb_to_rgb565be(float r, float g, float b) {
    uint16_t ri = (uint16_t)(r * 31.0f) & 0x1F;
    uint16_t gi = (uint16_t)(g * 63.0f) & 0x3F;
    uint16_t bi = (uint16_t)(b * 31.0f) & 0x1F;
    uint16_t rgb = (ri << 11) | (gi << 5) | bi;
    return (rgb >> 8) | (rgb << 8);   // big-endian for SPI DMA
}
```
All pixel writes to the framebuffer must use big-endian RGB565.
`draw_fade()` in draw.c also byte-swaps in and out to operate on natural RGB565.

### Task Setup
```c
xTaskCreatePinnedToCore(render_task, "name", STACK_SIZE, &args, 5, NULL, 1);
```
- Pin to **Core 1** — leaves Core 0 for IDF system tasks
- Stack size: 4096 minimum for simple screensavers; 8192 if using nested float loops
  (e.g., ray-marchers — math library call chains eat stack)
- Priority 5 is appropriate for a graphics task

### Draw Utilities (draw.h / draw.c)
- `SCREEN_W 320`, `SCREEN_H 240` — use these constants, not raw numbers
- `draw_line(fb, x0, y0, x1, y1, color)` — Bresenham, bounds-checked
- `draw_fill(fb, color)` — memset for color==0, loop otherwise
- `draw_fade(fb)` — multiply every pixel channel by ~15/16 toward black (handles
  big-endian swap internally); use instead of clear+redraw for trail effects

---

## ESP-IDF Toolchain

### Installation (Windows — fresh system)

> If `C:\Espressif\frameworks\esp-idf-v5.5.3` does not exist, run this once.
> Requires: git and Python 3.x on PATH. ESP-IDF v5.5.3 is compatible with
> Python 3.8–3.13.

**Step 1 — Clone ESP-IDF:**
```powershell
powershell.exe -NoProfile -Command "
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path 'C:\Espressif\frameworks' | Out-Null
git clone --recursive --branch v5.5.3 --depth 1 https://github.com/espressif/esp-idf.git 'C:\Espressif\frameworks\esp-idf-v5.5.3'
"
```

**Step 2 — Install tools (downloads Xtensa GCC, OpenOCD, ninja, etc.):**
```powershell
powershell.exe -NoProfile -Command "
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
\$Env:IDF_TOOLS_PATH = 'C:\Espressif'
cd 'C:\Espressif\frameworks\esp-idf-v5.5.3'
.\install.ps1 esp32
"
```
This takes several minutes. When it finishes it prints `All done! You can now run: export.ps1`.

**Step 3 — Find your Python venv name:**
```powershell
ls C:\Espressif\python_env\
```
The directory will be named `idf5.5_py3.XX_env` where `XX` is your system Python version.
Use that name in `IDF_PYTHON_ENV_PATH` in the boilerplate below.

- **ESP-IDF:** v5.5.3 at `C:\Espressif\frameworks\esp-idf-v5.5.3`
- **Python venv:** `C:\Espressif\python_env\idf5.5_py3.XX_env` (XX = your Python version)
- **Tools root:** `C:\Espressif`

### The MSYSTEM Problem
Claude Code runs inside MSYS2/Git Bash, which sets `MSYSTEM=MINGW64`.
ESP-IDF detects this and refuses to run — including both `install.ps1` and `idf.py`.
The fix is to strip it at the top of every PowerShell invocation before sourcing export.ps1.

### Build/Flash Boilerplate
Replace `# commands` with the actual idf.py invocation(s).
Replace `py3.XX` with the version from your venv directory name.

```powershell
powershell.exe -NoProfile -Command "
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
\$Env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\python_env\idf5.5_py3.XX_env'
\$Env:IDF_TOOLS_PATH = 'C:\Espressif'
cd 'C:\Espressif\frameworks\esp-idf-v5.5.3'
. .\export.ps1 | Out-Null
cd 'C:\Users\troutstar\Desktop\<project>'
# commands
"
```

- `Remove-Item Env:MSYSTEM` — strips the MSYS flag; required for install.ps1 AND idf.py
- `IDF_PYTHON_ENV_PATH` — pins the correct venv so export.ps1 doesn't pick the wrong Python
- `IDF_TOOLS_PATH` — tells ESP-IDF where tools were installed
- `. .\export.ps1 | Out-Null` — sets IDF_PATH, PATH, etc. silently

### Common Commands

**First-time setup for a new project:**
```
idf.py set-target esp32
idf.py build
```

**Subsequent builds:**
```
idf.py build
```

**Flash (always ask user for COM port first):**
```
idf.py -p <PORT> flash
```

**Produce a merged binary for the flash tool (run after build):**
```
idf.py merge-bin -o C:\Users\Troutstar\Desktop\Ideas\flashtool\firmware\<name>_merged.bin
```
Use an **absolute path** for `-o`. esptool internally `cd`s into `build/` before running,
so a relative path like `build\name.bin` resolves to `build\build\name.bin` and fails.
An absolute path drops the merged binary directly into `flashtool/firmware/`.

This creates a single self-contained binary (bootloader + partition table + app) that
flashes at address `0x0`. Flash via the web UI at `0x0`. This is the standard workflow
for all CYD projects — always run merge-bin after build and use the merged binary for
deployment. **Never flash a raw app `.bin` at `0x0`** — it overwrites the bootloader and
bricks the device until reflashed with a proper merged binary.

**Serial monitor (only for diagnostics — do not leave running after flash):**
```
idf.py -p <PORT> monitor
# Exit with Ctrl+]
```

**Find COM port:**
```powershell
Get-WmiObject Win32_SerialPort | Select-Object Name, DeviceID, Description
```
Look for "CP210x" or "CH340". Ignore Bluetooth serial ports.

### Gotchas
- **Port busy on flash:** a monitor session is holding the port. Kill it first.
- **Wrong Python picked up:** make sure both `IDF_PYTHON_ENV_PATH` and `IDF_TOOLS_PATH`
  are set before export.ps1 runs, or it may find the wrong interpreter.
- **set-target wipes sdkconfig:** only run it once, or when switching targets.
- **MSYSTEM blocks install.ps1 too:** strip it before running the installer, not just idf.py.
- **No `&&` in PowerShell:** use `if ($LASTEXITCODE -eq 0) { next-command }` to chain commands conditionally.

---

## Project Structure Template

```
<project>/
├── CMakeLists.txt          cmake_minimum_required + project(<name>)
├── sdkconfig.defaults      CONFIG_BT_ENABLED=n, CONFIG_ESP_WIFI_ENABLED=n, CONFIG_FREERTOS_HZ=100
└── main/
    ├── CMakeLists.txt      idf_component_register(SRCS ... INCLUDE_DIRS ".")
    ├── main.c              app_main: init display, alloc fb, xTaskCreatePinnedToCore
    ├── ili9341.h / .c      display driver — copy from cydnoof/main/
    ├── draw.h / .c         framebuffer utilities — copy from cydnoof/main/
    ├── <hack>.h / .c       screensaver logic
    └── ...
```

Reference implementations live in `cydnoof/main/` (noof screensaver) and
`cydnoof/Starnest/main/` (Star Nest ray-marcher).

---

## Performance Notes

- **Float math:** ESP32 LX6 has hardware FPU; single-precision float is fast.
  `sqrtf`, `sinf`, `cosf` are hardware-accelerated but still expensive in tight loops.
- **DMA blit time:** 320×240×2 bytes at 40 MHz SPI = ~31 ms per frame.
  This is the floor — you cannot exceed ~32 FPS regardless of CPU speed.
- **Practical ceiling for per-pixel computation:** ~8–10 FPS for moderately complex
  shaders (confirmed with Star Nest: volsteps=3, iterations=5, 6×6 blocks).
  Full ray-march at 1×1 pixel blocks: ~1.5 FPS (too slow for animation).
- **Fade-to-black trick:** instead of clearing the framebuffer each frame, call
  `draw_fade()` then draw new geometry additively. Eliminates the need for a second
  buffer and creates natural motion trails. Used in noof.
