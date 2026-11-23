# DaedalusX64 (PSP) with mupen64plus fixes

Based on DaedalusX64 **master** (October 2026), not the 2019 1.1.8 release. Tested on a PSP Go with
ARK custom firmware, plus PPSSPP for interpreter/regression tests.

Install: copy `PSP/GAME/DaedalusX64` from the release zip to the PSP and put ROMs in its `Roms` folder.

## What changed vs current Daedalus

**Runs properly on a 64MB PSP with today's toolchain**
- PARAM.SFO `MEMSIZE=1` (the current pspdev default caps homebrew at 32MB; Daedalus exited at startup).
- ROM memory sized from free RAM at startup (~28MB on a PSP Go in the 64MB layout; upstream's 16MB
  buffer could not hold 16–32MB ROMs in "ROM Buffer" mode).
- On ARK, enable high memory for homebrew (`homebrew, highmem, on` in `PSP/SAVEDATA/ARK_01234/SETTINGS.TXT`);
  otherwise only the 24MB layout is available.
- PS/HOME button now quits (upstream registered no exit callback).

**Accuracy (from mupen64plus / mupen64plus-rsp-hle)**
- CPU: TLBWR/Random, interrupts after ERET/MTC0, DIV/DDIV/DMULT (interpreter + dynarec), FP compares,
  Compare/Count, LL/SC, traps, MTC0 masks. krom CPU tests: DIV, DIVU, DDIV, DDIVU, DMULT, DMULTU now pass.
- Saves: EEPROM fallback/sizes/no-response bit, FlashRAM 16KB sector + chip erase, CIC fallback,
  PI DMA past end of ROM, MI interrupt line, homebrew header save types.
- Audio/RSP: ADDMIXER (Zelda OoT/MM, 1080, F-Zero X, Star Fox…), NEAD_16, RESAMPLE_ZOH, MusyX v2 guard,
  ABI1 null slots, ADPCM3 nibble scale, unknown RSP tasks complete instead of hanging.
- Per-ROM `CountPerOp` (roms.ini) — mupen64plus CPU timing.
- SI DMA completes via a CPU_EVENT_SI 0x900 cycles later with SI_STATUS_DMA_BUSY set (mupen si_controller.c);
  falls back to immediate completion if the 4-slot event table is full. Body Harvest hack kept (untested).
- roms.ini save types from mupen64plus.ini: 7 filled, 7 conflicts resolved to mupen (Aleck64 kept), 168 ROMs added.
  Daedalus IDs are byte-reversed header CRCs; lookup falls back to CRC-only when the country byte differs.
- CountPerOp default 2 (mupen) with mupen's per-game 1/3/4 values in roms.ini (274 entries updated, 110 added).
- HLE audio: NEAD and naudio variants chosen by ucode_data signature (mupen try_audio_task_detection); HILOGAIN
  decodes a signed Q4.4 gain; DK64 opcodes 7/8 = MIXER; naudio 0xE = NAUDIO_02B0.
- Not ported: AI DMA FIFO/interrupt timing (PSP Daedalus never raises MI_INTR_AI; adding it touches the dynarec
  register-read path and OSHLE AI patches and can't be tested without hardware); POLEF/NAUDIO_14/OVERLOAD and the
  GoldenEye linear envmixer (no test ROMs).

**Graphics / usability (Daedalus bugs found while testing)**
- Zelda OoT/MM dialogue text: stale glyphs from the previous message (font slots reused, texture checks off on PSP).
  Zelda 4-bit textures are re-hashed on every use. Verified in PPSSPP with a scripted playthrough to the
  Skull Kid dialogue: before "He e,hee .You two fairi esd idgr ea t", after "I wonder if he has anything good on him…".
- Fog: upstream a9718b06 blended vertex colours instead of the fog colour (pink tint); software fog pass restored.
- Fog pass depth test GU_EQUAL -> GU_GEQUAL (GU_EQUAL kept after alpha-tested passes): many fogged pixels failed the
  equality test and stayed unfogged (MM Lost Woods bands / black canopy). Found with DLDUMP + RFLAGS in PPSSPP.
- Screenshots never saved (per-game folder not created).
- Audio is off by default on PSP; HLE audio fixes verified with sync audio in PPSSPP (MK64, SF64, MM, Yoshi).

**Tooling**
- `bench.log` (create `benchmark.txt`): per-second speed, FPS, free memory, time split
  (graphics / audio / GPU wait / limiter / CPU), ROM reads, dynarec compile, flushes.
  Options: `countperop=N`, `seconds=N`, `selective=1`.
- PPSSPP test harness (CMake `-DDAEDALUS_AUTOLOAD=ON`: boots the ROM named in `autoload.txt`, dumps the RDRAM
  framebuffer) and startup/watchdog logs (`-DDAEDALUS_STARTUP_LOG=ON`). Daedalus' dynarec doesn't run under
  PPSSPP; use `interp` as the second line of `autoload.txt`.

## Measured on the PSP Go (speed % of full N64 speed, 2 min attract demos)

| Game | Upstream (BASE) | This build | This build, countperop=2 |
|---|---|---|---|
| Mario Kart 64 | 87.8 | 90.4 | 93.0 |
| Star Fox 64 | 96.8 | 97.0 | 97.1 |
| Majora's Mask | 80.7 | 78.5–80.7 | 81.6 |

Majora's Mask time split: ~28% HLE graphics, ~64% CPU/other, <1% GPU wait. Its worst stutters (scene
changes, ~30% speed for 1–2 s) are full dynarec cache flushes after code overlays load (12 per 2 min).

## Tried and shelved
- **Selective dynarec invalidation** (only drop overwritten code). First version cut flushes 12 → 1 and
  softened scene-change dips, but steady gameplay was ~7% slower; follow-ups crashed the PSP after the
  first invalidation (bisected to the fragment-cache bookkeeping, root cause not found). Restored to the
  known-good code; available as `selective=1` for experiments. Needs real dynarec debugging to finish.
- **Dynarec in PPSSPP**: Daedalus' recompiler doesn't run under PPSSPP, so dynarec changes can only be
  verified on hardware.

## Possible next steps
- AI timing, POLEF/NAUDIO_14/OVERLOAD, GoldenEye envmixer; n64-systemtest run (needs a Rust toolchain to build).
- Performance needs Daedalus-specific work (HLE graphics path, recompiler); mupen64plus can't help there.
