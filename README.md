# DaedalusX64 for PSP — modern port for PSP-2000/3000/Go

A PSP-focused build of the [DaedalusX64](https://github.com/DaedalusX64/daedalus) Nintendo 64 emulator, brought up to date for the slim PSP models:

- **Full 64MB of RAM** on PSP-2000/3000/Go, using the current pspdev toolchain.
- **Accuracy fixes ported from [mupen64plus](https://github.com/mupen64plus/mupen64plus-core) and [mupen64plus-rsp-hle](https://github.com/mupen64plus/mupen64plus-rsp-hle)**: CPU, saves, audio and RSP tasks.
- **On-device benchmarking**: per-second speed and where the time goes, logged on the PSP itself.

It's built from current DaedalusX64 master (October 2026), not the 2019 1.1.8 release, and tested on a real PSP Go running ARK custom firmware.

**Download:** [Releases](https://github.com/jorgechavezxyz/daedalus/releases). No ROMs are included.

## Install

1. Copy `PSP/GAME/DaedalusX64` from the release zip to your PSP (Memory Stick or PSP Go internal storage).
2. Put your ROMs (`.z64` / `.n64` / `.v64`) in `PSP/GAME/DaedalusX64/Roms`.
3. **ARK users:** let homebrew use the extra RAM. In `PSP/SAVEDATA/ARK_01234/SETTINGS.TXT`, change `always, highmem, off` to `homebrew, highmem, on`, then restart the PSP. Without this, Daedalus is limited to the old 24MB layout.

PSP-1000 still works, through the original low-memory path.

## What this port changes

### Modern PSP support
- **64MB on 2000/3000/Go.** The current pspdev toolchain caps homebrew at 32MB by default, so an unmodified Daedalus built today exits at startup on these models. This build requests the full memory (`MEMSIZE=1`).
- **ROM memory sized to your PSP.** The ROM buffer is sized from the RAM actually free (about 28MB on a PSP Go), replacing a fixed 16MB buffer that crashed 16–32MB games in "ROM Buffer" mode.
- **Bigger ROM cache.** The default "File Cache" mode caches up to the whole ROM instead of 8MB, so most games read from storage only once. When memory is tight it falls back to a smaller cache instead of crashing.
- **HOME / PS button quits the emulator.** Upstream registered no exit callback, so it couldn't.

### Graphics and usability fixes
- **Scrambled dialogue text in Zelda OoT/MM.** The game reuses font slots between messages, and Daedalus showed letters left over from the previous message. Zelda's glyph textures are now re-checked every time they're drawn.
- **Pink or garbled colours in fogged scenes** (Majora's Mask, Yoshi's Story…). A December 2024 upstream change broke the PSP fog pass; the working software fog is restored.
- **Banded backgrounds in fog** (Majora's Mask's Lost Woods). Distant scenery showed as hard light-to-dark stripes and black tree spikes instead of mist, because the fog pass skipped many pixels. It now fogs every pixel the scenery drew.
- **Screenshots** (pause menu → Take Screenshot) now save to `ScreenShots/<game>/`. Before, they failed silently.

### Accuracy (from mupen64plus)
- **CPU:**
  - TLB writes and the Random register fixed.
  - Pending interrupts are taken after ERET and Status/Cause writes.
  - Nested exceptions handled correctly.
  - DIV/DDIV/DMULT edge cases fixed in both the interpreter and the PSP recompiler.
  - NaN-correct floating-point compares.
  - Count/Compare timer fixes.
  - LL/SC, trap instructions and COP0 write masks implemented.
  - The krom CPU test ROMs for DIV, DIVU, DDIV, DDIVU, DMULT and DMULTU now pass.
- **Cartridge and video timing:** cartridge (PI) DMA now takes time like on hardware (length/8 cycles for ROM, 0x1000 for saves), and the video scanline register reports the real line and interlace field instead of a counter that just ticked up on every read.
- **Floating-point rounding:** float-to-integer conversions in the interpreter follow the game's chosen rounding mode (nearest, toward zero, up, down), and no longer switch the PSP's own FPU rounding mode behind Daedalus' back.
- **Controller port:** serial (SI) DMA now takes time like on hardware: the controller reports busy and its interrupt arrives 0x900 cycles later, as in mupen64plus, instead of instantly.
- **Saves:**
  - Unknown games default to a 4Kbit EEPROM instead of crashing on save.
  - Save types for 175 more ROMs from mupen64plus' game database (SRAM, FlashRAM and 16Kbit EEPROM games that couldn't save before), plus 7 corrected entries.
  - EEPROM saves stored at their real sizes.
  - FlashRAM sector and chip erase fixed (Majora's Mask, Paper Mario, Pokémon Stadium and others).
  - CIC fallback for unknown boot chips.
  - Cartridge DMA past the end of the ROM is zero-filled.
  - Homebrew save headers are read.
- **Audio and RSP:**
  - The ADDMIXER command now mixes (Zelda OoT/MM, 1080, F-Zero X, Yoshi's Story, Star Fox).
  - Each game's audio microcode variant is identified the way mupen64plus does it, instead of guessed. GoldenEye, Blast Corps and Diddy Kong Racing get their linear-ramp envelope mixer; Mario Kart, Star Fox, Banjo-Tooie, Jet Force Gemini, Perfect Dark and Conker get the filter and overload commands they use. Donkey Kong 64's mix commands now mix (they ran an MP3 decoder before), and the HILOGAIN volume command uses the right gain.
  - Added NEAD_16 and RESAMPLE_ZOH.
  - MusyX v2 guard.
  - ADPCM and ABI1 table fixes.
  - Unknown RSP tasks complete instead of hanging the game.
- **Timing:** CPU cycles per instruction (`CountPerOp`) now default to 2 like mupen64plus, with mupen's per-game exceptions for 340 ROMs in `roms.ini`. On a PSP Go this is also faster: Mario Kart 64 93.0% vs 90.4%, Majora's Mask 81.6% vs about 80%.

### Benchmarking
Put a `benchmark.txt` file in the DaedalusX64 folder. Each game then writes a `bench.log` for 120 seconds.

Every second it records:
- emulation speed, FPS and free memory;
- a time split: HLE graphics, audio, waiting on the GPU, frame limiter, and CPU;
- ROM reads, recompiler compile time, DMA, and cache flushes (with their reasons).

At the end it adds an average.

Options in `benchmark.txt`:
- `seconds=N`: logging duration.
- `countperop=N`: CPU timing to try.
- `selective=1`: experimental selective recompiler invalidation, off by default (see [MUPEN_PORT.md](MUPEN_PORT.md)).

### Sound
Audio is **off by default** in Daedalus's PSP builds, as it is upstream. To turn it on for a game, open the pause menu with **SELECT → Edit Preferences → Audio**:
- **Async** runs audio on the Media Engine.
- **Sync** runs it on the main CPU.

The HLE audio fixes above were tested in PPSSPP with sync audio; Media Engine audio hasn't been re-tested on hardware since those fixes.

## Results on a PSP Go

Each game ran its 2-minute attract demo, with the same settings in every build.

| Game | Upstream Daedalus | This port, CountPerOp 1 (v1–v5) | CountPerOp 2 (default from v6) |
|---|---|---|---|
| Mario Kart 64 | 87.8% | 90.4% | 93.0% |
| Star Fox 64 | 96.8% | 97.0% | 97.1% |
| Majora's Mask | 80.7% | 78.5–80.7% | 81.6% |

Full details, what was tried and shelved, and known limitations are in [MUPEN_PORT.md](MUPEN_PORT.md).

## Building

Requirements:
- the [pspdev](https://github.com/pspdev/pspdev) toolchain, with `PSPDEV` set and its `bin` on `PATH`;
- `cmake`.

```bash
./build_daedalus.sh PSP
```

The output goes to `DaedalusX64/`. Copy that folder to `PSP/GAME/`.

Developer options (CMake):
- `-DDAEDALUS_STARTUP_LOG=ON` logs each startup step plus a watchdog trace, for diagnosing crashes on hardware.
- `-DDAEDALUS_AUTOLOAD=ON` is for automated PPSSPP test runs:
  - It boots the ROM named in `autoload.txt` and dumps the N64 framebuffer.
  - `autoload.txt` line 2 `interp` forces the interpreter; line 3 `audio` turns on sync audio.
  - `autoload_input.txt` scripts controller input and screenshots per frame, so a test can play through menus. Use PPSSPP's software renderer so screenshots capture the picture.
  - In the same file, `DLDUMP` writes the next frame's display list (builds with `-DDAEDALUS_DEBUG_DISPLAYLIST=ON`), and `RFLAGS=n` switches parts of the renderer off for A/B comparisons (see `RendererPSP.cpp`).
- PPSSPP can't run Daedalus' recompiler, so test runs use the interpreter.

The original project also builds for Linux, macOS, Windows, 3DS and Vita (`./build_daedalus.sh` with no argument for the host OS). This port's changes target the PSP and haven't been tested on those platforms.

## Credits
- StrmnNrmn - For bringing this project to us
- Kreationz, Howard0Su salvy, Corn, Chilly Willy, Azimer, Shinydude100 and missed folks for past contributions
- MasterFeizz for 3DS Support / ARM DynaRec
- Xerpi / Rinnegatamante / TheOfficialFloW for PS Vita and ARM Contributions
- z2442 & Wally: Compilation improvements and updating, optimizations
- TheMrIron2 & mrneo240 Optimizations, wiki maintenance
- re4thewin for continuous testing over the duration of DaedalusX64
- motolegacy for help with site and emulator itself
- fjtrujy for help with getting CI going.
- Our Valued Communities for helping making Daedalus what it is :)
- mupen64plus developers, whose core and RSP HLE code these fixes are based on

This port is not affiliated with the DaedalusX64 team. Original project: https://github.com/DaedalusX64/daedalus
