/*
Copyright (C) 2003 Azimer
Copyright (C) 2001,2006 StrmnNrmn

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//
//	N.B. This source code is derived from Azimer's Audio plugin (v0.55?)
//	and modified by StrmnNrmn to work with Daedalus PSP. Thanks Azimer!
//	Drop me a line if you get chance :)
//
//

#include "Base/Types.h"
#include <cstring>

#include "HLEAudio/HLEAudioInternal.h"
#include "HLEAudio/HLEAudioState.h"
#include "Ultra/ultra_sptask.h"
#include "Utility/Profiler.h"

// Audio UCode lists
// Dummy UCode Handler
//
static void SPU(AudioHLECommand) {}
//
//     ABI ? : Unknown or unsupported UCode
//
// std::array<AudioHLEInstruction, 0x20> ABIUnknown = {
AudioHLEInstruction ABIUnknown[0x20] = { // Unknown ABI
    SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU,
    SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU,
    SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU, SPU};
//---------------------------------------------------------------------------------------------
//
//     ABI 1 : Mario64, WaveRace USA, Golden Eye 007, Quest64, SF Rush
//				 60% of all games use this.  Distributed 3rd Party
//ABI
//
extern AudioHLEInstruction ABI1[0x20];
extern AudioHLEInstruction ABI1_GE[0x20]; // GoldenEye, Blast Corps, Diddy Kong Racing
// extern std::array<AudioHLEInstruction, 0x20> ABI1;
//---------------------------------------------------------------------------------------------
//
//     ABI 2 : WaveRace JAP, MarioKart 64, Mario64 JAP RumbleEdition,
//				 Yoshi Story, Pokemon Games, Zelda64, Zelda MoM
//(miyamoto) 				 Most NCL or NOA games (Most commands)
extern AudioHLEInstruction ABI2[0x20];

// extern std::array<AudioHLEInstruction, 0x20> ABI2;
//---------------------------------------------------------------------------------------------
//
//     ABI 3 : DK64, Perfect Dark, Banjo Kazooie, Banjo Tooie
//				 All RARE games except Golden Eye 007
//
extern AudioHLEInstruction ABI3[0x20];
AudioHLEInstruction *Audio_SelectNaudioVariant(u32 sig);
// extern std::array<AudioHLEInstruction, 0x20> ABI3;
//---------------------------------------------------------------------------------------------
//
//     ABI 5 : Factor 5 - MoSys/MusyX
//				 Rogue Squadron, Tarzan, Hydro Thunder, and
//TWINE 				 Indiana Jones and Battle for Naboo (?)
//---------------------------------------------------------------------------------------------
//
// Below functions were updated
//

AudioHLEInstruction *ABI = ABIUnknown;
bool bAudioChanged = false;
static u32 gMusyXVersion = 0; // 1/2: MusyX v1/v2 task (ABI_MusyX.cpp), not an alist
extern bool isMKABI;
extern bool isZeldaABI;

//*****************************************************************************
//
//*****************************************************************************
void Audio_Reset() {
  bAudioChanged = false;
  gMusyXVersion = 0;
  isMKABI = false;
  isZeldaABI = false;
}

//*****************************************************************************
//
//*****************************************************************************
#ifdef DAEDALUS_AUTOLOAD
// Test harness: audio ucode signature words and the alist opcodes seen (logged with each vi= line)
u32 gAutoloadAudioSig[3] = {};
u32 gAutoloadAudioOps = 0;
#endif

//*****************************************************************************
// NEAD (ABI2) variants, identified by the same ucode_data word mupen64plus-rsp-hle
// uses (hle.c try_audio_task_detection). Opcode 7 differs between them: Zelda-style
// ucodes use it for FILTER, the Mario Kart / Star Fox / F-Zero / Wave Race (J) ones
// ignore it. Daedalus used to guess from the first opcode 7 it saw (SEGMENT2), which
// could put Mario Kart in Zelda mode. Unknown signatures keep that heuristic.
//*****************************************************************************
static AudioHLEInstruction ABI2_Variant[0x20];

static AudioHLEInstruction *Audio_SelectNeadVariant(u32 sig) {
  enum { NEAD_ZELDA, NEAD_MK, NEAD_SF, NEAD_FZ, NEAD_WRJB, NEAD_UNKNOWN } kind;
  switch (sig) {
  case 0x1f08122c: /* Yoshi's Story */
  case 0x1f38122c: /* 1080 Snowboarding */
  case 0x1f681230: /* Zelda OoT, Zelda MM (J) */
  case 0x1f801250: /* Zelda MM, Pokemon Stadium 2 */
  case 0x109411f8: /* Zelda MM (E Beta) */
  case 0x1eac11b8: /* Animal Crossing */
    kind = NEAD_ZELDA; break;
  case 0x11181350: /* Mario Kart 64, Wave Race (E) */
    kind = NEAD_MK; break;
  case 0x111812e0: /* Star Fox (J) */
  case 0x110412cc: /* Star Fox / Lylat Wars */
    kind = NEAD_SF; break;
  case 0x1cd01250: /* F-Zero X */
  case 0x1f4c1230: /* F-Zero X Expansion */
    kind = NEAD_FZ; break;
  case 0x110412ac: /* Wave Race (J Rev B) */
    kind = NEAD_WRJB; break;
  default:
    kind = NEAD_UNKNOWN; break;
  }
  if (kind == NEAD_UNKNOWN)
    return ABI2;

  memcpy(ABI2_Variant, ABI2, sizeof(ABI2_Variant));
  isMKABI = (kind == NEAD_MK);
  isZeldaABI = (kind == NEAD_ZELDA);
  if (kind == NEAD_ZELDA) {
    ABI2_Variant[0x07] = FILTER2;
  } else {
    ABI2_Variant[0x07] = SPNOOP;
  }
  if (kind == NEAD_MK || kind == NEAD_SF) {
    ABI2_Variant[0x0e] = POLEF2;   // mupen alist_nead.c: POLEF, not HILOGAIN
  }
  if (kind == NEAD_FZ || kind == NEAD_WRJB) {
    ABI2_Variant[0x0e] = SPNOOP;   // mupen: no HILOGAIN in these ucodes
  }
  if (kind == NEAD_WRJB) {
    ABI2_Variant[0x1b] = FILTER2;
  }
  return ABI2_Variant;
}

inline void Audio_Ucode_Detect(OSTask *pTask) {
  u8 *p_base = g_pu8RamBase + (uintptr_t)pTask->t.ucode_data;
#ifdef DAEDALUS_AUTOLOAD
  gAutoloadAudioSig[0] = *(u32 *)(p_base + 0); gAutoloadAudioSig[1] = *(u32 *)(p_base + 0x10); gAutoloadAudioSig[2] = *(u32 *)(p_base + 0x28);
#endif
  gMusyXVersion = 0;
  if (*(u32 *)(p_base + 0) != 0x01) {
    if (*(u32 *)(p_base + 0x10) == 0x00000001) {
      ABI = ABIUnknown; // MusyX v1 (Rogue Squadron, RE2, Hydro Thunder, Tarzan, ...)
      gMusyXVersion = 1;
    }
    else
      ABI = Audio_SelectNaudioVariant(*(u32 *)(p_base + 0x10));
  } else {
    if (*(u32 *)(p_base + 0x30) == 0xF0000F00) {
      const u32 sig = *(u32 *)(p_base + 0x28);
      // mupen64plus-rsp-hle: GoldenEye (0x1dc8138c) and Blast Corps / DKR (0x1e3c1390)
      // use ENVMIXER_GE; the common ABI1 (0x1e24138c) and unknown ones keep ENVMIXER.
      ABI = (sig == 0x1dc8138c || sig == 0x1e3c1390) ? ABI1_GE : ABI1;
    }
    else if (*(u32 *)(p_base + 0x10) == 0x00010010) {
      ABI = ABIUnknown; // MusyX v2 (Indiana Jones, Battle for Naboo): not an alist, don't parse it as ABI2
      gMusyXVersion = 2;
    }
    else
      ABI = Audio_SelectNeadVariant(*(u32 *)(p_base + 0x10));
  }
}

//*****************************************************************************
//
//*****************************************************************************
void Audio_Ucode() {
#ifdef DAEDALUS_PROFILE
  DAEDALUS_PROFILE("HLEMain::Audio_Ucode");
#endif
  OSTask *pTask = (OSTask *)(g_pu8SpMemBase + 0x0FC0);

  // Only detect ABI once per game
  if (!bAudioChanged) {
    bAudioChanged = true;
    Audio_Ucode_Detect(pTask);
  }

  // MusyX: the task data is a list of sound frames in RDRAM, not an alist.
  if (gMusyXVersion == 1) {
    Audio_MusyX_v1((u32)(uintptr_t)pTask->t.data_ptr, pTask->t.data_size);
    return;
  }
  if (gMusyXVersion == 2) {
    Audio_MusyX_v2((u32)(uintptr_t)pTask->t.data_ptr, pTask->t.data_size);
    return;
  }

  // Other unsupported audio ucodes: their task data is not an alist.
  if (ABI == ABIUnknown)
    return;

  gAudioHLEState.LoopVal = 0;
  memset( gAudioHLEState.Segments, 0, sizeof( gAudioHLEState.Segments ) );

  u32 *p_alist = (u32 *)(g_pu8RamBase + (uintptr_t)pTask->t.data_ptr);
  u32 ucode_size = (pTask->t.data_size >> 3); // ABI5 can return 0 here!!!

  while (ucode_size) {
    AudioHLECommand command;
    command.cmd0 = *p_alist++;
    command.cmd1 = *p_alist++;

#ifdef DAEDALUS_AUTOLOAD
    gAutoloadAudioOps |= 1u << command.cmd;
#endif
    ABI[command.cmd](command);

    --ucode_size;

    // printf("%08X %08X\n",command.cmd0,command.cmd1);
  }
}
