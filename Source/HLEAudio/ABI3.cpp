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
#include "Base/Types.h"


#include <string.h>

#include "Utility/MathUtil.h"
#include "HLEAudio/HLEAudioInternal.h"
#include "HLEAudio/HLEAudioState.h"

// Disables the command because it's not used?
static void DISABLE(AudioHLECommand) {}
static void WHATISTHIS(AudioHLECommand) {}

static void MP3ADDY(AudioHLECommand) {
  //	setaddr = (command.cmd1 & 0xffffff);
}

AudioHLEInstruction ABI3[0x20] = {
// std::array<AudioHLEInstruction, 0x20> ABI3 = {
    DISABLE,   ADPCM3,      CLEARBUFF3, ENVMIXER3, LOADBUFF3, RESAMPLE3,
    SAVEBUFF3, MP3,         MP3ADDY,    SETVOL3,   DMEMMOVE3, LOADADPCM3,
    MIXER3,    INTERLEAVE3, WHATISTHIS, SETLOOP3,  SPNOOP,    SPNOOP,
    SPNOOP,    SPNOOP,      SPNOOP,     SPNOOP,    SPNOOP,    SPNOOP,
    SPNOOP,    SPNOOP,      SPNOOP,     SPNOOP,    SPNOOP,    SPNOOP,
    SPNOOP,    SPNOOP,
};

// naudio opcode 0xE (mupen64plus-rsp-hle NAUDIO_02B0): emulates code inside SETVOL that the RSP
// reaches through this opcode; sets the low 16 bits of the right volume ramp rate.
static void NAUDIO_02B0(AudioHLECommand command) {
  gAudioHLEState.VolRampRight = (gAudioHLEState.VolRampRight & ~0xffff) | (command.cmd1 & 0xffff);
}

// naudio MP3 ucodes, opcode 0xE (mupen64plus-rsp-hle NAUDIO_14): pole filter when the first
// two ADPCM table entries are zero, otherwise IIR filter, on 0x170 bytes of main or main2.
static void NAUDIO_14(AudioHLECommand command) {
  u8 flags = (u8)(command.cmd0 >> 16);
  u16 gain = (u16)command.cmd0;
  u8 select_main = (u8)(command.cmd1 >> 24);
  u32 address = command.cmd1 & 0xffffff;
  u16 addr = (select_main == 0) ? 0x4f0 : 0x660;
  s16 *table = (s16 *)gAudioHLEState.ADPCMTable.data();

  if (table[0] == 0 && table[1] == 0)
    Audio_PoleFilter((flags & A_INIT) != 0, addr, addr, 0x170, gain, table, address);
  else
    Audio_IIRFilter((flags & A_INIT) != 0, addr, addr, 0x170, table, address);
}

// naudio MP3 ucodes, opcode 0 (mupen64plus-rsp-hle OVERLOAD / alist_overload): Conker's
// overload distortion. Clamped gain, then attenuation, over 0x170 samples.
static void OVERLOAD(AudioHLECommand command) {
  u16 addr = (u16)((command.cmd0 & 0xfff) + 0x4f0);
  s16 gain = (s16)(u16)command.cmd1;
  u16 attenuation = (u16)(command.cmd1 >> 16);
  s16 *sample = (s16 *)(gAudioHLEState.Buffer + addr);

  for (u32 count = 0x170; count != 0; --count) {
    s16 accu = Saturate<s16>(*sample * gain);
    *sample = (s16)((accu * attenuation) >> 16);
    sample++;
  }
}

// naudio variants by ucode_data word (mupen64plus-rsp-hle hle.c try_audio_task_detection).
// Plain naudio / Banjo-Kazooie: opcodes 7/8 are no-ops (Daedalus ran its MP3 decoder there),
// 0xE is NAUDIO_02B0. Donkey Kong 64: 7/8 are MIXER (its mixes were lost). MP3 ucodes
// (Banjo-Tooie, Jet Force Gemini, Mickey, Perfect Dark; Conker): 0 is OVERLOAD, 0xE is
// NAUDIO_14. Unknown ones keep ABI3.
static AudioHLEInstruction ABI3_Variant[0x20];

AudioHLEInstruction *Audio_SelectNaudioVariant(u32 sig) {
  const bool plain = (sig == 0x0000127c || sig == 0x00001280);
  const bool dk = (sig == 0x1c58126c);
  const bool mp3 = (sig == 0x1ae8143c || sig == 0x1ab0140c);
  if (!plain && !dk && !mp3)
    return ABI3;
  memcpy(ABI3_Variant, ABI3, sizeof(ABI3_Variant));
  if (mp3) {
    ABI3_Variant[0x00] = OVERLOAD;
    ABI3_Variant[0x0e] = NAUDIO_14;
    return ABI3_Variant;
  }
  ABI3_Variant[0x07] = dk ? MIXER3 : SPNOOP;
  ABI3_Variant[0x08] = dk ? MIXER3 : SPNOOP;
  ABI3_Variant[0x0e] = NAUDIO_02B0;
  return ABI3_Variant;
}
