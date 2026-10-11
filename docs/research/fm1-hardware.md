# M-VAVE FM-1 hardware, toolchain and flashing

A developer-facing reference for the FM-1 hardware. Researched 2026-10-10. Each claim cites a numbered source listed at the bottom. Where sources disagree, both versions are given. Anything unconfirmed is marked **Unknown**.

Related: [firmware landscape](firmware-landscape.md) · [dev tooling](dev-tooling.md) · [architecture](../engineering/architecture.md)

## Summary

- **Device.** The FM-1 is a Game Boy-sized battery FM synth released in July 2026 for about $67–100 [1][2][3][4]. The stock firmware is a DX7-style 6-operator FM synth.
- **SoC.** It runs on a JieLi **AC791N / WL82** with a **pi32v2** CPU, which is a proprietary Blackfin-like core and **not ARM**. The only toolchain is **JieLi clang 4.0.1 (pi32v2)**, which runs on Linux x86-64 only (use Docker on macOS) [5][6][7]. arm-none-eabi-gcc, PlatformIO, Arduino and Rust are all out.
- **Specs.** The CPU runs at 240 MHz from a 24 MHz crystal [7]. SRAM is about **512 KiB** [8] (one secondary source says 578 KB [10]). Flash is **1 MiB SPI NOR**, with **no SD card** [6][7][9].
- **Prior art.** **Felucca** (GPL-3.0, bare-metal) already provides:
  - a working HAL;
  - a 4-track × 64-step sequencer;
  - a song chain;
  - p-locks and automation;
  - MIDI clock in;
  - USB-MIDI and USB audio;
  - a web installer and a browser emulator.

  The fm1-x0x firmware already forked Felucca's HAL [5][11].
- **Hard limits:**
  - There is no TRS MIDI out.
  - USB works as a device only. No known firmware does USB host.
  - Felucca has 8 voices; the stock firmware has 12 FM voices.
  - The keys are not velocity-sensitive.

## 1. Device

| Item | Detail | Src |
|---|---|---|
| Size | 161.5 × 96.5 × 28.6 mm, 251 g | [12] |
| Keys | **27 silicone keys**, F3–G5, each with an LED | [12][13] |
| Velocity | **No velocity sensing found.** Felucca scans the keys as plain switches and gives hand-entered steps a fixed velocity of 96. The stock firmware's fixed velocity value is Unknown. | [13][14][12] |
| Knobs | 1 analog MASTER pot (ADC PB6) and **7 endless detented encoders**: SELECT, ALGORITHM, PRESETS, KNOB 1–4 | [12][13][15] |
| Buttons | **14**: FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC, OCT−, OCT+. Plus a rear power switch. | [15][12] |
| Display | **Colour TFT, 240×240, ST7789-class controller on SPI1** (12 MHz), RGB565 | [2][7][16] |
| LEDs | Per-key and per-button LEDs, multiplexed through the scan matrix. PLAY can show green. Felucca adds dim, breathe and mid levels with PWM. | [13][5] |
| Audio | 3.5 mm TRS out and a speaker. The stock engine and FX are mono. | [12][4] |
| USB | USB-C: power, charge, MIDI, firmware update and audio. Felucca enumerates as USB-MIDI plus a stereo UAC input, so the computer can record the FM-1, and adds an optional CDC serial console. | [7][5][12] |
| MIDI | **3.5 mm TRS MIDI IN only** (UART1 PH8). USB-MIDI in and out. BLE-MIDI works on stock firmware only. | [12][17][7] |
| Battery | 2000 mAh Li-po, about 12 h; voltage read on ADC PB1 | [12][15] |
| Stock engine | msfa (Dexed) DX7-compatible, 32 algorithms, 12 voices, 44.1 kHz, 64-sample blocks, 128 presets | [7][4][12] |
| Stock FX | Mono chain: filter, chorus, phaser, distortion, delay, reverb | [4][12] |
| Stock sequencer | 16 steps, 16 patterns, step entry only. No p-locks. The arpeggiator has 7 modes. ARP and SEQ can't run at the same time. | [4][12] |

The FM-1-RE "pitch/mod wheel" ADC notes are SDK leftovers. Felucca's HAL, tested on hardware, identifies those ADC channels as the MASTER pot and the battery divider [7][15][18].

## 2. Internals

- **CPU.**
  - pi32v2, 32-bit, 16-bit LE instruction words [7][19].
  - Stock firmware uses `CPU_CORE_NUM 1` [18].
  - The chip family is advertised as dual-core with an FPU up to 320 MHz [10][20]. **Whether the second core can be used on the FM-1 is Unknown.**
- **SRAM.**
  - Felucca maps 0x01C00000–0x01C80000. That splits into 24 KiB of RAM code, 72 KiB of .data/.bss, a 360 KiB pool, stacks, and a no-init area. A boot mailbox sits at 0x01C7FD50 [8].
  - There is no external SDRAM [7].
- **Code** executes in place (XIP) from flash at `0x02000000`, through a cache [8][16].
- **Audio codec.**
  - **The sources disagree.** Felucca drives ALNK0 I2S into an external codec, with control on PC0, PC1, PC2 and PC6, at 24-bit stereo, 128-frame half-buffers and 44.1 kHz [16][13]. FM-1-RE says the codec is the on-chip ADDA [7].
  - The part number is Unknown. Treat Felucca's working audio path as the reference.

### Flash maps

**Stock** [7]:

| Offset | Contents |
|---|---|
| 0x00000 | Header |
| 0xA0 | SPL |
| 0x38D0 | isd_config |
| 0x4000–0x93FFF | App (encrypted JLFS) |
| 0x94000 | VM config |
| 0xE9000 | BTIF |
| 0xEA000 | USR patches |
| 0xFF000 | Key / MAC |

**Felucca** [9][22]:

| Range | Contents |
|---|---|
| 0x4000–0x93000 | App |
| 0x97000–0x9EFFF | 4 projects × A/B, 4 KiB each, 3840-byte payload |
| 0x9F000 | User FM6 patches |
| 0xA0000–0xDBFFF | **User samples, 240 KiB** |
| 0xDC000–0xDFFFF | User preset banks |
| 0xE0000–0xE4FFF | OTA staging |
| 0xE5000–0xE6FFF | Autosave A/B |
| 0xE7000 | FM6 voice bank |
| 0xFC000–0xFEFFF | Settings |

- Felucca never writes 0xE8000, the stock VM backup.
- Its storage uses A/B sector pairs, and a CRC'd commit header is written last, so a power cut mid-write is safe [22].
- **Flash writes stall XIP.** Writes run from RAM in short IRQ-off windows, with the audio silenced during erases [9][23].

### Pinout (from Felucca's HAL; no schematic exists)

| Function | Pins |
|---|---|
| Key / button / encoder matrix | 11 columns × 6 rows via two 74HC595s. SER PA4, SRCLK PA3, RCLK PA1. Rows on PA0, PA5–PA8, PB7. Scanned from a ~10 kHz TIMER5 ISR [13]. |
| LEDs | PH6, PH9, PA9, PA10, on the same column scan [13] |
| LCD | SPI1: CLK PC9, DO PC10, CS PC7, D/C PC8. Backlight PA2, active low [16]. |
| MIDI IN | UART1 RX PH8, 31250 baud, DMA ring buffer [17] |
| ADC | PB6 MASTER pot, PB1 battery [15] |
| Flash | SPI0 / SFC, CS# on PD0 [9] |
| USB | USB0 full-speed device, MUSB-like [23] |

### Debug

- No SWD or JTAG probe exists for pi32v2.
- The stock firmware has no UART console [24].
- Felucca adds a **USB CDC serial console** (build flags `FELUCCA_CDC` and `FELUCCA_UART`) [6][13].
- No teardown photos or schematics were found.

## 3. Flashing and recovery

1. **Normal path: USB-MIDI SysEx carrying a `.fwsc` package.**
   - Tools: Felucca's **web installer** (Chrome/Edge Web MIDI, https://hugelton.github.io/Felucca/) or `python3 tools/fm1_install.py build/felucca.fwsc` [5][6].
   - The installer can revert to "official V15".
   - BAUDGIRL's installer rejects packages whose first 16 KB differs from M-VAVE's boot code [25].
2. **Recovery path: mask-ROM UBOOT.**
   - The chip enumerates as `WL80UBOOT1.00`, 4C4A:8057, USB mass storage with vendor SCSI commands [17].
   - Entry methods: a USB_KEY waveform on D+/D− at power-on, or the vendor SysEx `F0 22 24 35 7D F7` sent to running V15 [17][26].
   - **FM-1 Transporter** (MIT) is a XIAO RP2040 running Pico-PIO-USB, wired GP0 to D+, GP1 to D− and GND, **with no VBUS**. It loads `wl82loader.bin` and dumps the whole 1 MiB in about 3 s. It refuses to write outside 0x4000–0x93000 [9][26].
3. **Brick risk.**
   - There is a single firmware bank and no recovery button [21][27].
   - The SPL in the first 16 KiB and the ROM boot are never overwritten, so a bad app can be recovered with the Transporter dongle [9][26].
   - Felucca adds a staged loader, a watchdog, `fm1_enter_uboot` (mailbox at 0x01C7FD80) and a crash safe mode [5][8].
4. **SDK.**
   - Jieli-Tech fw-AC79_AIoT_SDK, Apache-2.0, tag `AC79NN_SDK_V1.2.1_2023-12-13` [6][28].
   - Felucca uses only `uboot.boot`, `cfg_tool.bin` and `eq_cfg_hw.bin` from it, for packaging. The firmware is otherwise bare-metal with no vendor code [6][8].
   - M-VAVE has published no SDK or source.

## 4. Toolchain

- The compiler is **JieLi clang/LLVM 4.0.1** (`clang -target pi32v2`), with prebuilt libc, libm and compiler-rt, producing ELF machine type 0xF1 [19].
- The language level is C11, or C++14 at most.
- Felucca is plain C with its own `libc.c`, `app.ld` and `loader.ld` [8].
- Felucca builds with `./build.sh` and fetches the toolchain with `tools/get_toolchain.sh`. On macOS it builds in Docker `linux/amd64`. It also needs Python 3 with Pillow and fontTools, and Emscripten for the emulator [6].

## 5. Constraints for a clip-launcher firmware

- **Storage.**
  - About 292 KiB of user flash in Felucca's layout.
  - **One Felucca project is 3840 bytes**: 4 tracks × 64 steps, parameters, a 16-row song chain and 128 automation records [22].
  - Shrinking the 240 KiB sample area would make room for dozens of clips.
  - There is no removable storage, so backup has to go over USB [5].
- **Never write** 0x0000–0x3FFF, 0xE8000, 0xFF000 or the BTIF region [9][17].
- **Saves stall audio.** Save only when stopped and idle, or in small silenced chunks [5][9][23].
- **CPU.**
  - One core at 240 MHz.
  - Felucca's budgets are host instructions per sample (about 300–1600 per patch), not target cycles.
  - On-device ISR load appears in MENU > SYSTEM > INFO [5][14][32].
  - **Absolute headroom is Unknown.**
- **Audio.**
  - 128-frame half-buffers, 2.9 ms each [14][13].
  - **8 voices shared across 4 tracks** in Felucca, with voice stealing [14].
- **MIDI.**
  - Felucca follows clock in over USB or TRS [5][33].
  - **Clock out is not implemented.** It could only ever go over USB, because there is no physical MIDI out [12].
  - BLE-MIDI needs JieLi's closed BT stack [7].
- **External keyboard.**
  - The FM-1 is a **USB device only, so a USB MIDI keyboard cannot plug in directly.**
  - Options: a keyboard with TRS or DIN MIDI out into the **TRS MIDI IN** jack (adapter type A or B is Unknown), a computer, or a USB-MIDI host box.
  - Whether USB host is possible on this board is Unknown [7][9][23].
- **LEDs.** The key LEDs are single-colour, but Felucca gets dim, breathe and mid levels out of them [13]. Combine them with the 240×240 screen for clip-state feedback.

## 6. Unknowns

- The audio codec part number, and whether the codec is internal or external.
- The flash part (ID 0x856014).
- A schematic.
- On-target CPU headroom.
- Whether the second core is reliable. Some units misread RAM on core 1 (x0x#10), so treat core 1 as optional headroom. *Resolved:* the FPU exists. It is single precision, and X0X uses it ([fork evaluation](fork-evaluation.md#5-engines-cpu-second-core)).
- Whether USB host and VBUS switching are possible.
- The stock keyboard's fixed velocity value.
- MIDI clock out in any firmware.
- Groove OS internals.

## Sources
1. https://synthanatomy.com/2026/06/m-vave-fm-1-a-budget-friendly-dx-7-style-desktop-fm-polysynth.html
2. https://www.synthtopia.com/content/2026/07/13/m-vave-introduces-fm-1-fm-pocket-synthesizer/
3. https://www.geeky-gadgets.com/m-vave-fm1-synthesizer-review/
4. https://synthanatomy.com/2026/07/m-vave-fm-1-review-low-budget-pocket-fm-ynthesizer-with-iconic-sounds.html
5. https://github.com/hugelton/Felucca
6. https://github.com/hugelton/Felucca/blob/main/BUILDING.md
7. https://github.com/AL-255/FM-1-RE/blob/main/docs/architecture.md
8. Felucca `firmware/app.ld`, `firmware/hal/fm1_sys.h`, `firmware/loader/loader.ld`
9. https://github.com/kurogedelic/FM-1-transporter ; Felucca `firmware/hal/fm1_flash.h`
10. https://gitcode.csdn.net/69e9857554b52172bc6bb671.html (secondary source)
11. https://github.com/charlesvestal/fm1-x0x
12. M-VAVE FM-1 manual: https://m.media-amazon.com/images/I/A1WOydif9HL.pdf
13. Felucca `firmware/hal/fm1_input.h`
14. Felucca `firmware/src/core.h`, `voice.c`, `seq.c`
15. Felucca `firmware/hal/fm1_adc.h`, `firmware/src/panel.c`
16. Felucca `firmware/hal/fm1_audio.h`, `fm1_lcd_hw.h`, `firmware/src/lcd.c`, `fm1_xip.h`
17. https://github.com/kurogedelic/FM-1-transporter/blob/main/docs/PROTOCOL.md ; Felucca `firmware/hal/fm1_uart.h`
18. FM-1-RE `docs/reversing/01-hardware-map.md`, `docs/io/07-input.md`
19. https://github.com/AL-255/FM-1-RE/blob/main/docs/04-toolchain-and-vendoring.md
20. AC7911B datasheet: https://atta.szlcsc.com/upload/public/pdf/source/20241209/7B2C0C261932781E1CA5530337098F4C.pdf
21. https://github.com/ip2k/lunar-modulator
22. Felucca `firmware/src/storage.c`, `project.c`, `song_chain.c`
23. Felucca `firmware/src/storage_hw.c`, `firmware/hal/fm1_usb.h`
24. https://github.com/AL-255/FM-1-RE/blob/main/analysis/device/debug-surfaces.md
25. https://sonicstate.com/news/2026/09/29/free-custom-firmware-for-mvave-fm-1-/
26. https://github.com/kurogedelic/FM-1-transporter/blob/main/docs/DEVLOG.md
27. https://github.com/AL-255/FM-1-RE ; https://news.ycombinator.com/item?id=49994065
28. https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK
29. https://github.com/kagaimiq/jl-uboot-tool ; https://kagaimiq.github.io/jielie/isp/usb/usb-key.html
30. https://www.synthtopia.com/?p=155069
31. https://www.matrixsynth.com/2026/10/m-vave-fm-1-now-is-pocket-daw-groove-os.html
32. Felucca `tests/cpu_baseline.txt`, `tests/target_budget.txt`
33. Felucca `firmware/src/midi_clock.c`
34. https://www.elektronauts.com/t/m-vave-fm-1/252170
