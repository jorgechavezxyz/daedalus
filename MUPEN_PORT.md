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
- Audio/RSP: ADDMIXER (Zelda OoT/MM, 1080, F-Zero X, Star Fox…), NEAD_16, RESAMPLE_ZOH,
  ABI1 null slots, ADPCM3 nibble scale, unknown RSP tasks complete instead of hanging.
- Per-ROM `CountPerOp` (roms.ini) — mupen64plus CPU timing.
- SI DMA completes via a CPU_EVENT_SI 0x900 cycles later with SI_STATUS_DMA_BUSY set (mupen si_controller.c);
  falls back to immediate completion if the 4-slot event table is full. Body Harvest hack kept (untested).
- roms.ini save types from mupen64plus.ini: 7 filled, 7 conflicts resolved to mupen (Aleck64 kept), 168 ROMs added.
  Daedalus IDs are byte-reversed header CRCs; lookup falls back to CRC-only when the country byte differs.
- CountPerOp default 2 (mupen) with mupen's per-game 1/3/4 values in roms.ini (274 entries updated, 110 added).
- HLE audio: NEAD and naudio variants chosen by ucode_data signature (mupen try_audio_task_detection); HILOGAIN
  decodes a signed Q4.4 gain; DK64 opcodes 7/8 = MIXER; naudio 0xE = NAUDIO_02B0.
- PI DMA via CPU_EVENT_PI (len/8 ROM, 0x1000 SRAM/Flash/writes); VI_CURRENT from cycles left to the queued VBL + field bit.
- Interpreter CVT.W/CVT.L honour FCR31 RM without changing the host FPU mode (dynarec unchanged).
- HLE audio: integer POLEF/IIRF, ENVMIXER_GE (GoldenEye/Blast Corps/DKR), NEAD MK/SF POLEF, naudio MP3 OVERLOAD/NAUDIO_14;
  host-tested byte-identical against mupen's alist code on 20,000 random cases.
- n64-systemtest (IS-Viewer output in autoload builds -> isviewer.log): 639/947 pass (interpreter). Out of scope: RSP LLE,
  doubles/FR=1, xkphys, reverse-endian, cache tags, MI repeat mode. Not added: 64-bit address and privilege checks in
  load/store handlers shared with the dynarec. Fixed a linker clash: the interpreter was running Dynamo's CPU_EXECUTE_OP.
- MusyX v1/v2 audio (Factor 5 ucodes) ported from mupen's musyx.c (`ABI_MusyX.cpp`, Media-Engine safe, host-tested
  byte-identical on 8,000 random tasks). The MusyX games only send audio tasks if the AI interrupt fires, which
  Daedalus never did: roms.ini `AIInterrupt=yes` turns on a mupen-style AI DMA FIFO (BUSY/FULL, MI_INTR_AI at the end
  of each buffer). It is opt-in because it changes interrupt timing for every game and hasn't been verified widely (with it
  on for all games the PPSSPP suite was still pixel-identical, audio off); enabled and checked (sound in PPSSPP, still
  runs with audio off) for Rogue Squadron (also no longer stuck at VI 320), Tarzan, Resident Evil 2, Rugrats in
  Paris, Rush 2049, The World is Not Enough, Polaris SnoCross. Without it the MusyX HLE alone gives no sound.
  Unverified: Hydro Thunder / NBA Showtime (audio stops at their intro screens), Indiana Jones / Battle for Naboo
  (MusyX v2: no voices within 200 s), Gauntlet Legends (crashes at VI 448 either way). AI_LEN still reads back the
  written length, not mupen's remaining length.
- roms.ini `MemoryAccessOptimisation=0` now turns the dynarec memory optimisation off (it used to be OR'd with the
  default-on preference, so it could only force it on).

**Dynarec bugs found by running the recompiler in JPCSP** (upstream Daedalus bugs, affect real PSPs)
- A load/store (or interpreter-called op) in the delay slot of a direct branch that leaves the recompiler
  (e.g. a PI_WR_LEN write raising the PI interrupt) resumed at a stale `TargetPC`: native direct branches never
  set it. Majora's Mask froze after a fragment-cache flush (PI manager thread resumed inside `__osException`).
  The slow paths of such delay-slot ops now store the known continuation address first.
- `CACHE` Index_Invalidate_I (osInvalICache for >= 16KB ranges loops over 0x80000000-0x80003FFF) only dropped
  fragments at those addresses, so CPU-decompressed overlays ran stale code unless the osInvalICache OS patch
  matched (MM with HLE OS patches off: thread jumped to 0). It now flushes all compiled code.
- Loop fragments (Dynarec Loop Optimisation, on by default) whose side exit goes back to the loop top branched
  there with a conditional branch from the branch-handler code buffer, megabytes away: the 16-bit offset was
  truncated and the CPU jumped into unrelated generated code. Now a branch over a `J` when out of range. Found in
  Conker's Bad Fur Day (0x150a5000: `gp++; beq a3,a2; ...; beq gp,t9,top`); any game with such a loop was affected.
- Conker's Bad Fur Day stalls on its "for mature audiences" screen with its TLB-mapped code (0x10000000-0x15xxxxxx)
  compiled, interpreter fine. roms.ini `DynarecSkipMappedCode=yes` (set for the Conker entries) leaves code fetched
  through the TLB to the interpreter; Conker then reaches its N64-logo intro on the dynarec (JPCSP). Narrowed down
  so far: 0x150a5000-0x150a5080 (the loop above, fixed), then the pair 0x150a565c + 0x150a5668.

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
- **Dynarec in PPSSPP**: Daedalus' recompiler doesn't run under PPSSPP. It does run in JPCSP (x86 Linux build in
  a VM, with a one-line JPCSP compiler patch), which is how the dynarec bugs above were found.

## Possible next steps
- AI timing for all games (AIInterrupt is per-ROM; AI_LEN remaining length); MusyX v2 in-game check; n64-systemtest run (needs a Rust toolchain to build).
- Conker's Bad Fur Day on the dynarec: works with roms.ini `DynarecSkipMappedCode=yes` (see above), at
  interpreter speed for its TLB-mapped code. Root cause of the stall with that code compiled not found: the
  loop-exit bug above was one cause; after fixing it, compiling the fragments at 0x150a565c (stack-clearing loop)
  and 0x150a5668 (FPU registers loaded with DMTC1, Status.FR=1) still makes the game wait forever on its warning
  screen, although the CPU state at the next block (0x150a579c) matches the interpreter for the first 18 visits and
  the result doesn't depend on where the code buffers are. Leads: FR=1 (Daedalus treats DMTC1/LDC1 to odd FPU
  registers as FR=0 pairs), and the dynarec never raises Coprocessor Unusable (libultra's lazy FPU switching; the
  interpreter does, Conker hits it twice early on).
- Performance needs Daedalus-specific work (HLE graphics path, recompiler); mupen64plus can't help there.
