#include "Base/Types.h"


#include <cstring>
#include <algorithm>
#include <iostream>
#include <vector>

#include "Utility/MathUtil.h"
#include "Core/Memory.h"
#include "Debug/DBGConsole.h"
#include "HLEAudio/HLEAudioInternal.h"
#include "HLEAudio/HLEAudioState.h"

extern bool isMKABI;
extern bool isZeldaABI;

void FILTER2(AudioHLECommand command) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "FILTER2");
#endif

  static int cnt = 0;
  static s16 *lutt6;
  static s16 *lutt5;
  u8 *save = (rdram + (command.cmd1 & 0xFFFFFF));
  u8 t4 = (u8)((command.cmd0 >> 0x10) & 0xFF);

  if (t4 > 1) { // Then set the cnt variable
    cnt = (command.cmd0 & 0xFFFF);
    lutt6 = (s16 *)save;
    //				memmove (dmem+0xFE0,
    // rdram+(command.cmd1&0xFFFFFF), 0x10);
    return;
  }

  if (t4 == 0) {
    //				memmove (dmem+0xFB0,
    // rdram+(command.cmd1&0xFFFFFF), 0x20);
    lutt5 = (short *)(save + 0x10);
  }

  lutt5 = (short *)(save + 0x10);

  //			lutt5 = (short *)(dmem + 0xFC0);
  //			lutt6 = (short *)(dmem + 0xFE0);
  for (int x = 0; x < 8; x++) {
    s32 a;
    a = (lutt5[x] + lutt6[x]) >> 1;
    lutt5[x] = lutt6[x] = (short)a;
  }
  short *inp1, *inp2;
  s32 out1[8];
  s16 outbuff[0x3c0], *outp;
  u32 inPtr = (u32)(command.cmd0 & 0xffff);
  inp1 = (short *)(save);
  outp = outbuff;
  inp2 = (short *)(gAudioHLEState.Buffer + inPtr);
  for (int x = 0; x < cnt; x += 0x10) {
    out1[1] = inp1[0] * lutt6[6];
    out1[1] += inp1[3] * lutt6[7];
    out1[1] += inp1[2] * lutt6[4];
    out1[1] += inp1[5] * lutt6[5];
    out1[1] += inp1[4] * lutt6[2];
    out1[1] += inp1[7] * lutt6[3];
    out1[1] += inp1[6] * lutt6[0];
    out1[1] += inp2[1] * lutt6[1]; // 1

    out1[0] = inp1[3] * lutt6[6];
    out1[0] += inp1[2] * lutt6[7];
    out1[0] += inp1[5] * lutt6[4];
    out1[0] += inp1[4] * lutt6[5];
    out1[0] += inp1[7] * lutt6[2];
    out1[0] += inp1[6] * lutt6[3];
    out1[0] += inp2[1] * lutt6[0];
    out1[0] += inp2[0] * lutt6[1];

    out1[3] = inp1[2] * lutt6[6];
    out1[3] += inp1[5] * lutt6[7];
    out1[3] += inp1[4] * lutt6[4];
    out1[3] += inp1[7] * lutt6[5];
    out1[3] += inp1[6] * lutt6[2];
    out1[3] += inp2[1] * lutt6[3];
    out1[3] += inp2[0] * lutt6[0];
    out1[3] += inp2[3] * lutt6[1];

    out1[2] = inp1[5] * lutt6[6];
    out1[2] += inp1[4] * lutt6[7];
    out1[2] += inp1[7] * lutt6[4];
    out1[2] += inp1[6] * lutt6[5];
    out1[2] += inp2[1] * lutt6[2];
    out1[2] += inp2[0] * lutt6[3];
    out1[2] += inp2[3] * lutt6[0];
    out1[2] += inp2[2] * lutt6[1];

    out1[5] = inp1[4] * lutt6[6];
    out1[5] += inp1[7] * lutt6[7];
    out1[5] += inp1[6] * lutt6[4];
    out1[5] += inp2[1] * lutt6[5];
    out1[5] += inp2[0] * lutt6[2];
    out1[5] += inp2[3] * lutt6[3];
    out1[5] += inp2[2] * lutt6[0];
    out1[5] += inp2[5] * lutt6[1];

    out1[4] = inp1[7] * lutt6[6];
    out1[4] += inp1[6] * lutt6[7];
    out1[4] += inp2[1] * lutt6[4];
    out1[4] += inp2[0] * lutt6[5];
    out1[4] += inp2[3] * lutt6[2];
    out1[4] += inp2[2] * lutt6[3];
    out1[4] += inp2[5] * lutt6[0];
    out1[4] += inp2[4] * lutt6[1];

    out1[7] = inp1[6] * lutt6[6];
    out1[7] += inp2[1] * lutt6[7];
    out1[7] += inp2[0] * lutt6[4];
    out1[7] += inp2[3] * lutt6[5];
    out1[7] += inp2[2] * lutt6[2];
    out1[7] += inp2[5] * lutt6[3];
    out1[7] += inp2[4] * lutt6[0];
    out1[7] += inp2[7] * lutt6[1];

    out1[6] = inp2[1] * lutt6[6];
    out1[6] += inp2[0] * lutt6[7];
    out1[6] += inp2[3] * lutt6[4];
    out1[6] += inp2[2] * lutt6[5];
    out1[6] += inp2[5] * lutt6[2];
    out1[6] += inp2[4] * lutt6[3];
    out1[6] += inp2[7] * lutt6[0];
    out1[6] += inp2[6] * lutt6[1];

    // XXXX correct?
    outp[1] = /*CLAMP*/ s16((out1[1] + 0x4000) >> 0xF);
    outp[0] = /*CLAMP*/ s16((out1[0] + 0x4000) >> 0xF);
    outp[3] = /*CLAMP*/ s16((out1[3] + 0x4000) >> 0xF);
    outp[2] = /*CLAMP*/ s16((out1[2] + 0x4000) >> 0xF);
    outp[5] = /*CLAMP*/ s16((out1[5] + 0x4000) >> 0xF);
    outp[4] = /*CLAMP*/ s16((out1[4] + 0x4000) >> 0xF);
    outp[7] = /*CLAMP*/ s16((out1[7] + 0x4000) >> 0xF);
    outp[6] = /*CLAMP*/ s16((out1[6] + 0x4000) >> 0xF);
    inp1 = inp2;
    inp2 += 8;
    outp += 8;
  }
  //			memmove (rdram+(command.cmd1&0xFFFFFF), dmem+0xFB0,
  // 0x20);
  // std::copy(outbuff, gAudioHLEState.Buffer + (command.cmd0 & 0xffff) );
  // std::copy(inp2 - 8, save, 0x10);
  memmove(save, inp2 - 8, 0x10);
  memmove(gAudioHLEState.Buffer + (command.cmd0 & 0xffff), outbuff, cnt);
}

//*****************************************************************************
// Integer ports of mupen64plus-rsp-hle alist_polef / alist_iirf (alist.c).
//
// gAudioHLEState.Buffer has the same byte layout as mupen's alist_buffer (compare
// AudioHLEState::Deinterleave with alist_copy_every_other_sample, both
// "*(s16 *)(buf + (a ^ 2))"), so mupen's alist_s16(a) is Buffer + (a ^ 2) and an
// s16 pointer p indexed p[i ^ S] is p[i ^ 1]. rdram likewise matches mupen's dram, so
// dram_u16(a) is rdram + (a ^ 2). Addresses wrap at 16 bits to stay inside Buffer.
// The ADPCM table is in natural N64 order (AudioHLEState::LoadADPCM), like mupen's.
//*****************************************************************************
static inline s16 *Filter_In(u16 addr) {
  return (s16 *)(gAudioHLEState.Buffer + (u16)(addr ^ 2));
}

// mupen: ((int16_t *)(alist_buffer + base))[index]
static inline s16 *Filter_Out(u16 base, u32 index) {
  return (s16 *)(gAudioHLEState.Buffer + (u16)(base + 2 * index));
}

static inline u16 *Filter_Dram16(u32 address) {
  return (u16 *)(rdram + ((address & (MAX_RAM_ADDRESS - 1)) ^ 2));
}

static inline s32 Filter_Vmulf(s16 x, s16 y) {
  return (((s32)x * (s32)y) + 0x4000) >> 15;
}

void Audio_PoleFilter(bool init, u16 dmemo, u16 dmemi, u16 count, u16 gain,
                      s16 *table, u32 address) {
  const s16 *const h1 = table;
  s16 *const h2 = table + 8;
  s16 h2_before[8];
  s16 l1, l2;
  u32 o = 0; // mupen's dst pointer, in samples from dmemo

  count = (u16)((count + 15) & ~15u);

  if (init) {
    l1 = 0;
    l2 = 0;
  } else {
    l1 = (s16)*Filter_Dram16(address + 4);
    l2 = (s16)*Filter_Dram16(address + 6);
  }

  // mupen scales the table's second half in place (it persists until the next LOADADPCM)
  for (u32 i = 0; i < 8; ++i) {
    h2_before[i] = h2[i];
    h2[i] = (s16)(((s32)h2[i] * gain) >> 14);
  }

  do {
    s16 frame[8];

    for (u32 i = 0; i < 8; ++i, dmemi += 2)
      frame[i] = *Filter_In(dmemi);

    for (u32 i = 0; i < 8; ++i) {
      s32 accu = frame[i] * gain;
      accu += h1[i] * l1 + h2_before[i] * l2;
      for (u32 j = 0; j < i; ++j) // rdot(i, h2, frame)
        accu += h2[j] * frame[i - 1 - j];
      *Filter_Out(dmemo, o + (i ^ 1)) = Saturate<s16>(accu >> 14);
    }

    l1 = *Filter_Out(dmemo, o + (6 ^ 1));
    l2 = *Filter_Out(dmemo, o + (7 ^ 1));

    o += 8;
    count -= 16;
  } while (count != 0);

  // dram_store_u32(dst - 4, address, 2)
  memcpy(rdram + (address & (MAX_RAM_ADDRESS - 1)), Filter_Out(dmemo, o - 4), 8);
}

void Audio_IIRFilter(bool init, u16 dmemo, u16 dmemi, u16 count,
                     const s16 *table, u32 address) {
  s16 frame[8] = {};
  s16 ibuf[4] = {};
  u16 index = 7;
  u32 o = 0;
  s32 prev;

  count = (u16)((count + 15) & ~15u);

  if (!init) {
    frame[6] = (s16)*Filter_Dram16(address + 4);
    frame[7] = (s16)*Filter_Dram16(address + 6);
    ibuf[1] = (s16)*Filter_Dram16(address + 8);
    ibuf[2] = (s16)*Filter_Dram16(address + 10);
  }

  prev = Filter_Vmulf(table[9], frame[6]) * 2;
  do {
    for (u32 i = 0; i < 8; ++i) {
      s32 accu;
      ibuf[index & 3] = *Filter_In(dmemi);

      accu = prev + Filter_Vmulf(table[0], ibuf[index & 3]) +
             Filter_Vmulf(table[1], ibuf[(index - 1) & 3]) +
             Filter_Vmulf(table[0], ibuf[(index - 2) & 3]);
      accu += Filter_Vmulf(table[8], frame[index]) * 2;
      prev = Filter_Vmulf(table[9], frame[index]) * 2;
      frame[i] = (s16)accu;
      *Filter_Out(dmemo, o + (i ^ 1)) = frame[i];

      index = (index + 1) & 7;
      dmemi += 2;
    }
    o += 8;
    count -= 0x10;
  } while (count > 0);

  *Filter_Dram16(address + 4) = (u16)frame[6];
  *Filter_Dram16(address + 6) = (u16)frame[7];
  *Filter_Dram16(address + 8) = (u16)ibuf[(index - 2) & 3];
  *Filter_Dram16(address + 10) = (u16)ibuf[(index - 1) & 3];
}

// ABI1 opcode 0xE (mupen alist_audio.c POLEF); the address is segmented like mupen's.
void POLEF(AudioHLECommand command) {
  u8 flags = (u8)(command.cmd0 >> 16);
  u16 gain = (u16)command.cmd0;
  u32 address = gAudioHLEState.SegmentAddress(command.cmd1);

  if (gAudioHLEState.Count == 0)
    return;

  Audio_PoleFilter((flags & A_INIT) != 0, gAudioHLEState.OutBuffer,
                   gAudioHLEState.InBuffer,
                   (u16)((gAudioHLEState.Count + 15) & ~15u), gain,
                   (s16 *)gAudioHLEState.ADPCMTable.data(), address);
}

// NEAD opcode 0xE for Mario Kart / Star Fox (mupen alist_nead.c POLEF).
void POLEF2(AudioHLECommand command) {
  u8 flags = (u8)(command.cmd0 >> 16);
  u16 gain = (u16)command.cmd0;
  u32 address = command.cmd1 & 0xffffff;

  if (gAudioHLEState.Count == 0)
    return;

  Audio_PoleFilter((flags & A_INIT) != 0, gAudioHLEState.OutBuffer,
                   gAudioHLEState.InBuffer, gAudioHLEState.Count, gain,
                   (s16 *)gAudioHLEState.ADPCMTable.data(), address);
}
