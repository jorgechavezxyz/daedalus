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
- **Saves:**
  - Unknown games default to a 4Kbit EEPROM instead of crashing on save.
  - EEPROM saves stored at their real sizes.
  - FlashRAM sector and chip erase fixed (Majora's Mask, Paper Mario, Pokémon Stadium and others).
  - CIC fallback for unknown boot chips.
  - Cartridge DMA past the end of the ROM is zero-filled.
  - Homebrew save headers are read.
- **Audio and RSP:**
  - The ADDMIXER command now mixes (Zelda OoT/MM, 1080, F-Zero X, Yoshi's Story, Star Fox).
  - Added NEAD_16 and RESAMPLE_ZOH.
  - MusyX v2 guard.
  - ADPCM and ABI1 table fixes.
  - Unknown RSP tasks complete instead of hanging the game.
- **Timing:** per-game `CountPerOp` (cycles per instruction), set in `roms.ini`.

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

## Results on a PSP Go

Each game ran its 2-minute attract demo, with the same settings in every build.

| Game | Upstream Daedalus | This port | This port, `countperop=2` |
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
- `-DDAEDALUS_AUTOLOAD=ON` boots the ROM named in `autoload.txt` and dumps the N64 framebuffer, for PPSSPP test runs.
- PPSSPP can't run Daedalus' recompiler, so put `interp` on the second line of `autoload.txt`.

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
