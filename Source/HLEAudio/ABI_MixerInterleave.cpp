#include "Base/Types.h"


#include <string.h>

#include "Utility/MathUtil.h"
#include "Core/Memory.h"
#include "Debug/DBGConsole.h"
#include "HLEAudio/HLEAudioInternal.h"
#include "HLEAudio/HLEAudioState.h"

extern bool isMKABI;
extern bool isZeldaABI;

// Saturating add of one DMEM buffer into another (mupen64plus-rsp-hle alist_add).
// This was a no-op before, so mixes using it were silently dropped (Zelda OoT/MM, 1080,
// F-Zero X, Yoshi's Story, Star Fox...).
void ADDMIXER(AudioHLECommand command) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "ADDMIXER");
#endif
  u32 count = ((command.cmd0 >> 12) & 0xff0) >> 1;
  u16 dmemi = (command.cmd1 >> 16);
  u16 dmemo = command.cmd1 & 0xffff;

  s16 *dst = (s16 *)(gAudioHLEState.Buffer + dmemo);
  const s16 *src = (const s16 *)(gAudioHLEState.Buffer + dmemi);
  while (count--) {
    *dst = Saturate<s16>((s32)*dst + (s32)*src);
    ++dst;
    ++src;
  }
}

// NEAD opcode 0x10: copy count blocks of block_size bytes in 0x20 byte steps
// (mupen64plus-rsp-hle alist_copy_blocks, d85e978). Fixes garbled race start in Wave Race.
void NEAD_16(AudioHLECommand command) {
  u32 count = (command.cmd0 >> 16) & 0xff;
  u16 dmemi = command.cmd0 & 0xffff;
  u16 dmemo = (command.cmd1 >> 16);
  u32 block_size = command.cmd1 & 0xffff;

  s32 blocks_left = count;
  do {
    s32 bytes_left = block_size;
    do {
      memcpy(gAudioHLEState.Buffer + dmemo, gAudioHLEState.Buffer + dmemi, 0x20);
      bytes_left -= 0x20;
      dmemi += 0x20;
      dmemo += 0x20;
    } while (bytes_left > 0);
    --blocks_left;
  } while (blocks_left > 0);
}

// NEAD opcode 0x06: zero-order-hold resampling (mupen64plus-rsp-hle alist_resample_zoh, db692a3).
// Fixes garbled audio when switching headset mode in 1080 Snowboarding.
void RESAMPLE_ZOH(AudioHLECommand command) {
  u32 pitch = (command.cmd0 & 0xffff) << 1;
  u32 pitch_accu = command.cmd1 & 0xffff;

  u16 ipos = gAudioHLEState.InBuffer >> 1;
  u16 opos = gAudioHLEState.OutBuffer >> 1;
  u32 count = gAudioHLEState.Count >> 1;
  s16 *samples = (s16 *)gAudioHLEState.Buffer;

  while (count != 0) {
    samples[(opos++) ^ 1] = samples[ipos ^ 1];
    pitch_accu += pitch;
    ipos += (pitch_accu >> 16);
    pitch_accu &= 0xffff;
    --count;
  }
}

void HILOGAIN(AudioHLECommand command) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "HILOAGAIN");
#endif
  u32 count = command.cmd0 & 0xffff;
  s32 hi = (s16)((command.cmd0 >> 4) & 0xf000);
  u32 lo = (command.cmd0 >> 20) & 0xf;

  u32 out = (command.cmd1 >> 16) & 0xffff;
  s16 *src = (s16 *)(gAudioHLEState.Buffer + out);

  while (count) {
    s32 val = *src;
    s32 tmp = ((val * hi) >> 16) + (u32)(val * lo);
    *src++ = Saturate<s16>(tmp);
    count -= 2;
  }
}

void INTERLEAVE(AudioHLECommand command) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "INTERLEAVE");
#endif
  u16 inL = command.Abi1Interleave.LAddr;
  u16 inR = command.Abi1Interleave.RAddr;

  gAudioHLEState.Interleave(inL, inR);
}

void DEINTERLEAVE2(AudioHLECommand command) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "DEINTERLEAVE2");
#endif
  u16 count = command.Abi2Deinterleave.Count;
  u16 out = command.Abi2Deinterleave.Out;
  u16 in = command.Abi2Deinterleave.In;

  gAudioHLEState.Deinterleave(out, in, count);
}

void INTERLEAVE2(AudioHLECommand command) {

#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "INTERLEAVE2");
#endif
  u16 inR = command.Abi2Interleave.RAddr;
  u16 inL = command.Abi2Interleave.LAddr;
  u16 out = command.Abi2Interleave.OutAddr;
  u16 count = command.Abi2Interleave.Count;

  if (count != 0) {
    gAudioHLEState.Interleave(out, inL, inR, count);
  } else {
    gAudioHLEState.Interleave(inL, inR);
  }
}

void INTERLEAVE3(AudioHLECommand command [[maybe_unused]]) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "INTERLEAVE3");
#endif
  // Needs accuracy verification...
  // inR = command.cmd1 & 0xFFFF;
  // inL = (command.cmd1 >> 16) & 0xFFFF;

  gAudioHLEState.Interleave(0x4f0, 0x9d0, 0xb40, 0x170);
}

void MIXER(AudioHLECommand command) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "MIXER");
#endif
  u16 dmemin = command.Abi1Mixer.DmemIn;
  u16 dmemout = command.Abi1Mixer.DmemOut;
  s32 gain = command.Abi1Mixer.Gain;

  gAudioHLEState.Mixer(dmemout, dmemin, gain);
}

void MIXER2(AudioHLECommand command) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "MIXER2");
#endif
  // Needs accuracy verification
  u16 dmemin = command.Abi2Mixer.DmemIn;
  u16 dmemout = command.Abi2Mixer.DmemOut;
  s32 gain = command.Abi2Mixer.Gain;
  u16 count = command.Abi2Mixer.Count * 16;

  // printf( "Mixer: i:%04x o:%04x g:%08x (%d) c:%04x - %08x%08x\n", dmemin,
  // dmemout, gain, s16(gain), count, command.cmd0, command.cmd1 );

  gAudioHLEState.Mixer(
      dmemout, dmemin, gain,
      count); // NB - did mult gain by 2 above, then shifted by 16 inside mixer.
}

void MIXER3(AudioHLECommand command) {
#ifdef DEBUG_AUDIO
  DBGConsole_Msg(0, "MIXER3");
#endif
  // Needs accuracy verification...
  u16 dmemin = (u16)(command.cmd1 >> 0x10) + 0x4f0;
  u16 dmemout = (u16)(command.cmd1 & 0xFFFF) + 0x4f0;
  // u8  flags   = (u8)((command.cmd0 >> 16) & 0xff);
  s32 gain = (s16)(command.cmd0 & 0xFFFF);

  gAudioHLEState.Mixer(
      dmemout, dmemin, gain,
      0x170); // NB - did mult gain by 2 above, then shifted by 16 inside mixer.
}
