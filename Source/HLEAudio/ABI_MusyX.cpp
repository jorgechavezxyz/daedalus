/*
 * MusyX v1/v2 audio ucode HLE (Factor 5): Rogue Squadron, Resident Evil 2, Hydro Thunder,
 * Tarzan, Indiana Jones, Battle for Naboo, ...
 *
 * Port of mupen64plus-rsp-hle musyx.c (Copyright (C) 2013 Bobby Smiles, GPL v2 or later).
 * Unlike the alist ucodes, MusyX works on RDRAM only (one task = a list of "SFD" sound
 * frames). Changes from mupen:
 *  - may run on the PSP Media Engine: no heap, no printf, no float; large work buffers are
 *    static instead of on the stack;
 *  - RDRAM is addressed like the rest of Daedalus' HLE audio (8MB, native-endian words,
 *    halfwords at (a>>1)^1, bytes at a^3); every access is masked to 8MB so bad pointers
 *    can't touch host memory (mupen masks to 16MB);
 *  - arithmetic that overflows int32 in mupen (signed overflow) is done with explicit
 *    32-bit wrap-around, which is what mupen compiles to on x86/ARM.
 * Host-tested byte-identical to mupen's musyx_v1_task/musyx_v2_task on random tasks.
 */

#include "Base/Types.h"
#include "Core/Memory.h"
#include "HLEAudio/HLEAudioInternal.h"

#include <cstring>

namespace {

/* various constants */
enum { SUBFRAME_SIZE = 192 };
enum { MAX_VOICES = 32 };
enum { SAMPLE_BUFFER_SIZE = 0x200 };

enum {
  SFD_VOICE_COUNT = 0x0,
  SFD_SFX_INDEX = 0x2,
  SFD_VOICE_BITMASK = 0x4,
  SFD_STATE_PTR = 0x8,
  SFD_SFX_PTR = 0xc,
  SFD_VOICES = 0x10,

  /* v2 only */
  SFD2_10_PTR = 0x10,
  SFD2_14_BITMASK = 0x14,
  SFD2_15_BITMASK = 0x15,
  SFD2_16_BITMASK = 0x16,
  SFD2_18_PTR = 0x18,
  SFD2_1C_PTR = 0x1c,
  SFD2_20_PTR = 0x20,
  SFD2_24_PTR = 0x24,
  SFD2_VOICES = 0x28
};

enum {
  VOICE_ENV_BEGIN = 0x00,
  VOICE_ENV_STEP = 0x10,
  VOICE_PITCH_Q16 = 0x20,
  VOICE_PITCH_SHIFT = 0x22,
  VOICE_CATSRC_0 = 0x24,
  VOICE_CATSRC_1 = 0x30,
  VOICE_ADPCM_FRAMES = 0x3c,
  VOICE_SKIP_SAMPLES = 0x3e,

  /* for PCM16 */
  VOICE_U16_40 = 0x40,
  VOICE_U16_42 = 0x42,

  /* for ADPCM */
  VOICE_ADPCM_TABLE_PTR = 0x40,

  VOICE_INTERLEAVED_PTR = 0x44,
  VOICE_END_POINT = 0x48,
  VOICE_RESTART_POINT = 0x4a,
  VOICE_U16_4C = 0x4c,
  VOICE_U16_4E = 0x4e,

  VOICE_SIZE = 0x50
};

enum {
  CATSRC_PTR1 = 0x00,
  CATSRC_PTR2 = 0x04,
  CATSRC_SIZE1 = 0x08,
  CATSRC_SIZE2 = 0x0a
};

enum {
  STATE_LAST_SAMPLE = 0x0,
  STATE_BASE_VOL = 0x100,
  STATE_CC0 = 0x110,
  STATE_740_LAST4_V1 = 0x290,

  STATE_740_LAST4_V2 = 0x110
};

enum {
  SFX_CBUFFER_PTR = 0x00,
  SFX_CBUFFER_LENGTH = 0x04,
  SFX_TAP_COUNT = 0x08,
  SFX_FIR4_HGAIN = 0x0a,
  SFX_TAP_DELAYS = 0x0c,
  SFX_TAP_GAINS = 0x2c,
  SFX_U16_3C = 0x3c,
  SFX_U16_3E = 0x3e,
  SFX_FIR4_HCOEFFS = 0x40
};

struct musyx_t {
  /* internal subframes */
  s16 left[SUBFRAME_SIZE];
  s16 right[SUBFRAME_SIZE];
  s16 cc0[SUBFRAME_SIZE];
  s16 e50[SUBFRAME_SIZE];

  /* internal subframes base volumes */
  s32 base_vol[4];

  s16 subframe_740_last4[4];
};

typedef void (*mix_sfx_with_main_subframes_t)(musyx_t *musyx, const s16 *subframe, const u16 *gains);

/* Work buffers (static: the Media Engine stack is small). One task runs at a time. */
musyx_t gMusyx;
s16 gSamples[SAMPLE_BUFFER_SIZE];
u8 gAdpcmBuffer[SAMPLE_BUFFER_SIZE * 2 * 5 / 16];
s16 gAdpcmTable[256]; /* 128 loaded; codebook index can reach 255 (mupen reads past its table) */
s16 gSfxBuffer[SUBFRAME_SIZE + 4];
s16 gSfxDelayed[SUBFRAME_SIZE];
s16 gInterleaveSubframe[SUBFRAME_SIZE];

/* mupen64plus-rsp-hle audio.c RESAMPLE_LUT */
const s16 RESAMPLE_LUT[64 * 4] = {
  (s16)0x0c39, (s16)0x66ad, (s16)0x0d46, (s16)0xffdf, (s16)0x0b39, (s16)0x6696, (s16)0x0e5f, (s16)0xffd8,
  (s16)0x0a44, (s16)0x6669, (s16)0x0f83, (s16)0xffd0, (s16)0x095a, (s16)0x6626, (s16)0x10b4, (s16)0xffc8,
  (s16)0x087d, (s16)0x65cd, (s16)0x11f0, (s16)0xffbf, (s16)0x07ab, (s16)0x655e, (s16)0x1338, (s16)0xffb6,
  (s16)0x06e4, (s16)0x64d9, (s16)0x148c, (s16)0xffac, (s16)0x0628, (s16)0x643f, (s16)0x15eb, (s16)0xffa1,
  (s16)0x0577, (s16)0x638f, (s16)0x1756, (s16)0xff96, (s16)0x04d1, (s16)0x62cb, (s16)0x18cb, (s16)0xff8a,
  (s16)0x0435, (s16)0x61f3, (s16)0x1a4c, (s16)0xff7e, (s16)0x03a4, (s16)0x6106, (s16)0x1bd7, (s16)0xff71,
  (s16)0x031c, (s16)0x6007, (s16)0x1d6c, (s16)0xff64, (s16)0x029f, (s16)0x5ef5, (s16)0x1f0b, (s16)0xff56,
  (s16)0x022a, (s16)0x5dd0, (s16)0x20b3, (s16)0xff48, (s16)0x01be, (s16)0x5c9a, (s16)0x2264, (s16)0xff3a,
  (s16)0x015b, (s16)0x5b53, (s16)0x241e, (s16)0xff2c, (s16)0x0101, (s16)0x59fc, (s16)0x25e0, (s16)0xff1e,
  (s16)0x00ae, (s16)0x5896, (s16)0x27a9, (s16)0xff10, (s16)0x0063, (s16)0x5720, (s16)0x297a, (s16)0xff02,
  (s16)0x001f, (s16)0x559d, (s16)0x2b50, (s16)0xfef4, (s16)0xffe2, (s16)0x540d, (s16)0x2d2c, (s16)0xfee8,
  (s16)0xffac, (s16)0x5270, (s16)0x2f0d, (s16)0xfedb, (s16)0xff7c, (s16)0x50c7, (s16)0x30f3, (s16)0xfed0,
  (s16)0xff53, (s16)0x4f14, (s16)0x32dc, (s16)0xfec6, (s16)0xff2e, (s16)0x4d57, (s16)0x34c8, (s16)0xfebd,
  (s16)0xff0f, (s16)0x4b91, (s16)0x36b6, (s16)0xfeb6, (s16)0xfef5, (s16)0x49c2, (s16)0x38a5, (s16)0xfeb0,
  (s16)0xfedf, (s16)0x47ed, (s16)0x3a95, (s16)0xfeac, (s16)0xfece, (s16)0x4611, (s16)0x3c85, (s16)0xfeab,
  (s16)0xfec0, (s16)0x4430, (s16)0x3e74, (s16)0xfeac, (s16)0xfeb6, (s16)0x424a, (s16)0x4060, (s16)0xfeaf,
  (s16)0xfeaf, (s16)0x4060, (s16)0x424a, (s16)0xfeb6, (s16)0xfeac, (s16)0x3e74, (s16)0x4430, (s16)0xfec0,
  (s16)0xfeab, (s16)0x3c85, (s16)0x4611, (s16)0xfece, (s16)0xfeac, (s16)0x3a95, (s16)0x47ed, (s16)0xfedf,
  (s16)0xfeb0, (s16)0x38a5, (s16)0x49c2, (s16)0xfef5, (s16)0xfeb6, (s16)0x36b6, (s16)0x4b91, (s16)0xff0f,
  (s16)0xfebd, (s16)0x34c8, (s16)0x4d57, (s16)0xff2e, (s16)0xfec6, (s16)0x32dc, (s16)0x4f14, (s16)0xff53,
  (s16)0xfed0, (s16)0x30f3, (s16)0x50c7, (s16)0xff7c, (s16)0xfedb, (s16)0x2f0d, (s16)0x5270, (s16)0xffac,
  (s16)0xfee8, (s16)0x2d2c, (s16)0x540d, (s16)0xffe2, (s16)0xfef4, (s16)0x2b50, (s16)0x559d, (s16)0x001f,
  (s16)0xff02, (s16)0x297a, (s16)0x5720, (s16)0x0063, (s16)0xff10, (s16)0x27a9, (s16)0x5896, (s16)0x00ae,
  (s16)0xff1e, (s16)0x25e0, (s16)0x59fc, (s16)0x0101, (s16)0xff2c, (s16)0x241e, (s16)0x5b53, (s16)0x015b,
  (s16)0xff3a, (s16)0x2264, (s16)0x5c9a, (s16)0x01be, (s16)0xff48, (s16)0x20b3, (s16)0x5dd0, (s16)0x022a,
  (s16)0xff56, (s16)0x1f0b, (s16)0x5ef5, (s16)0x029f, (s16)0xff64, (s16)0x1d6c, (s16)0x6007, (s16)0x031c,
  (s16)0xff71, (s16)0x1bd7, (s16)0x6106, (s16)0x03a4, (s16)0xff7e, (s16)0x1a4c, (s16)0x61f3, (s16)0x0435,
  (s16)0xff8a, (s16)0x18cb, (s16)0x62cb, (s16)0x04d1, (s16)0xff96, (s16)0x1756, (s16)0x638f, (s16)0x0577,
  (s16)0xffa1, (s16)0x15eb, (s16)0x643f, (s16)0x0628, (s16)0xffac, (s16)0x148c, (s16)0x64d9, (s16)0x06e4,
  (s16)0xffb6, (s16)0x1338, (s16)0x655e, (s16)0x07ab, (s16)0xffbf, (s16)0x11f0, (s16)0x65cd, (s16)0x087d,
  (s16)0xffc8, (s16)0x10b4, (s16)0x6626, (s16)0x095a, (s16)0xffd0, (s16)0x0f83, (s16)0x6669, (s16)0x0a44,
  (s16)0xffd8, (s16)0x0e5f, (s16)0x6696, (s16)0x0b39, (s16)0xffdf, (s16)0x0d46, (s16)0x66ad, (s16)0x0c39
};

/* RDRAM access (8MB, same byte order as mupen's dram_uXX on a little-endian host) */
const u32 DRAM_MASK = 0x7fffff;

inline u8 dram_u8(u32 address) { return g_pu8RamBase[(address & DRAM_MASK) ^ 3]; }
inline u16 dram_u16(u32 address) { return ((u16 *)g_pu8RamBase)[((address & DRAM_MASK) >> 1) ^ 1]; }
inline u32 dram_u32(u32 address) { return ((u32 *)g_pu8RamBase)[(address & DRAM_MASK) >> 2]; }
inline void dram_w16(u32 address, u16 v) { ((u16 *)g_pu8RamBase)[((address & DRAM_MASK) >> 1) ^ 1] = v; }
inline void dram_w32(u32 address, u32 v) { ((u32 *)g_pu8RamBase)[(address & DRAM_MASK) >> 2] = v; }

void dram_load_u8(u8 *dst, u32 address, u32 count) {
  while (count != 0) { *(dst++) = dram_u8(address); address += 1; --count; }
}
void dram_load_u16(u16 *dst, u32 address, u32 count) {
  while (count != 0) { *(dst++) = dram_u16(address); address += 2; --count; }
}
void dram_load_u32(u32 *dst, u32 address, u32 count) {
  while (count != 0) { *(dst++) = dram_u32(address); address += 4; --count; }
}
void dram_store_u16(const u16 *src, u32 address, u32 count) {
  while (count != 0) { dram_w16(address, *(src++)); address += 2; --count; }
}

inline s16 clamp_s16(s32 x) {
  x = (x < -32768) ? -32768 : x;
  x = (x > 32767) ? 32767 : x;
  return (s16)x;
}

inline u32 align(u32 x, u32 amount) {
  --amount;
  return (x + amount) & ~amount;
}

inline s16 adpcm_predict_sample(u8 byte, u8 mask, unsigned lshift, unsigned rshift) {
  s16 sample = (s16)(u16)((u16)(byte & mask) << lshift);
  sample >>= rshift; /* signed */
  return sample;
}

/* mupen audio.c; sums wrap at 32 bits like mupen's int32 arithmetic */
u32 rdot(u32 n, const s16 *x, const s16 *y) {
  u32 accu = 0;
  y += n;
  while (n != 0) {
    accu += (u32)(*(x++) * *(--y));
    --n;
  }
  return accu;
}

void adpcm_compute_residuals(s16 *dst, const s16 *src, const s16 *cb_entry, const s16 *last_samples, u32 count) {
  const s16 *const book1 = cb_entry;
  const s16 *const book2 = cb_entry + 8;
  const s16 l1 = last_samples[0];
  const s16 l2 = last_samples[1];

  for (u32 i = 0; i < count; ++i) {
    u32 accu = (u32)(s32)src[i] << 11;
    accu += (u32)(book1[i] * l1) + (u32)(book2[i] * l2) + rdot(i, book2, src);
    dst[i] = clamp_s16((s32)accu >> 11);
  }
}

s32 dot4(const s16 *x, const s16 *y) {
  s32 accu = 0;
  for (u32 i = 0; i < 4; ++i)
    accu = clamp_s16(accu + (((s32)x[i] * (s32)y[i]) >> 15));
  return accu;
}

void load_base_vol(s32 *base_vol, u32 address) {
  base_vol[0] = (s32)(((u32)dram_u16(address) << 16) | dram_u16(address + 8));
  base_vol[1] = (s32)(((u32)dram_u16(address + 2) << 16) | dram_u16(address + 10));
  base_vol[2] = (s32)(((u32)dram_u16(address + 4) << 16) | dram_u16(address + 12));
  base_vol[3] = (s32)(((u32)dram_u16(address + 6) << 16) | dram_u16(address + 14));
}

void save_base_vol(const s32 *base_vol, u32 address) {
  for (u32 k = 0; k < 4; ++k) {
    dram_w16(address, (u16)(base_vol[k] >> 16));
    address += 2;
  }
  for (u32 k = 0; k < 4; ++k) {
    dram_w16(address, (u16)base_vol[k]);
    address += 2;
  }
}

void update_base_vol(s32 *base_vol, u32 voice_mask, u32 last_sample_ptr, u8 mask_15, u32 ptr_24) {
  u32 i, k, mask;

  if (voice_mask != 0) {
    for (i = 0, mask = 1; i < MAX_VOICES; ++i, mask <<= 1, last_sample_ptr += 8) {
      if ((voice_mask & mask) == 0)
        continue;
      for (k = 0; k < 4; ++k)
        base_vol[k] = (s32)((u32)base_vol[k] + (u32)(s32)(s16)dram_u16(last_sample_ptr + k * 2));
    }
  }

  if (mask_15 != 0) {
    for (i = 0, mask = 1; i < 4; ++i, mask <<= 1, ptr_24 += 8) {
      if ((mask_15 & mask) == 0)
        continue;
      for (k = 0; k < 4; ++k)
        base_vol[k] = (s32)((u32)base_vol[k] + (u32)(s32)(s16)dram_u16(ptr_24 + k * 2));
    }
  }

  /* apply 3% decay */
  for (k = 0; k < 4; ++k)
    base_vol[k] = (s32)((u32)base_vol[k] * 0x0000f850u) >> 16;
}

void init_subframes_v1(musyx_t *musyx) {
  s16 base_cc0 = clamp_s16(musyx->base_vol[2]);
  s16 base_e50 = clamp_s16(musyx->base_vol[3]);

  s16 *left = musyx->left;
  s16 *right = musyx->right;
  s16 *cc0 = musyx->cc0;
  s16 *e50 = musyx->e50;

  for (u32 i = 0; i < SUBFRAME_SIZE; ++i) {
    *(e50++) = base_e50;
    *(left++) = clamp_s16(*cc0 + base_cc0);
    *(right++) = clamp_s16(-*cc0 - base_cc0);
    *(cc0++) = 0;
  }
}

void init_subframes_v2(musyx_t *musyx) {
  s16 values[4];
  s16 *subframes[4];

  for (u32 k = 0; k < 4; ++k)
    values[k] = clamp_s16(musyx->base_vol[k]);

  subframes[0] = musyx->left;
  subframes[1] = musyx->right;
  subframes[2] = musyx->cc0;
  subframes[3] = musyx->e50;

  for (u32 i = 0; i < SUBFRAME_SIZE; ++i)
    for (u32 k = 0; k < 4; ++k)
      *(subframes[k]++) = values[k];
}

void dma_cat8(u8 *dst, u32 catsrc_ptr) {
  u32 ptr1 = dram_u32(catsrc_ptr + CATSRC_PTR1);
  u32 ptr2 = dram_u32(catsrc_ptr + CATSRC_PTR2);
  u16 size1 = dram_u16(catsrc_ptr + CATSRC_SIZE1);
  u16 size2 = dram_u16(catsrc_ptr + CATSRC_SIZE2);

  /* stay inside the ADPCM buffer (mupen doesn't check) */
  u32 count1 = size1;
  u32 count2 = size2;
  if (count1 > sizeof(gAdpcmBuffer)) count1 = sizeof(gAdpcmBuffer);
  if (count2 > sizeof(gAdpcmBuffer) - count1) count2 = sizeof(gAdpcmBuffer) - count1;

  dram_load_u8(dst, ptr1, count1);

  if (size2 == 0)
    return;

  dram_load_u8(dst + count1, ptr2, count2);
}

void dma_cat16(u16 *dst, u32 catsrc_ptr, u32 room) {
  u32 ptr1 = dram_u32(catsrc_ptr + CATSRC_PTR1);
  u32 ptr2 = dram_u32(catsrc_ptr + CATSRC_PTR2);
  u16 size1 = dram_u16(catsrc_ptr + CATSRC_SIZE1);
  u16 size2 = dram_u16(catsrc_ptr + CATSRC_SIZE2);

  /* stay inside the sample buffer (mupen doesn't check) */
  u32 count1 = size1 >> 1;
  u32 count2 = size2 >> 1;
  if (count1 > room) count1 = room;
  if (count2 > room - count1) count2 = room - count1;

  dram_load_u16(dst, ptr1, count1);

  if (size2 == 0)
    return;

  dram_load_u16(dst + count1, ptr2, count2);
}

void load_samples_PCM16(u32 voice_ptr, s16 *samples, u32 *segbase, u32 *offset) {
  u8 u8_3e = dram_u8(voice_ptr + VOICE_SKIP_SAMPLES);
  u16 u16_40 = dram_u16(voice_ptr + VOICE_U16_40);
  u16 u16_42 = dram_u16(voice_ptr + VOICE_U16_42);

  u32 count = align(u16_40 + u8_3e, 4);
  if (count > SAMPLE_BUFFER_SIZE) count = SAMPLE_BUFFER_SIZE;  /* not in mupen: buffer bound */

  *segbase = SAMPLE_BUFFER_SIZE - count;
  *offset = u8_3e;

  dma_cat16((u16 *)samples + *segbase, voice_ptr + VOICE_CATSRC_0, count);

  if (u16_42 != 0)
    dma_cat16((u16 *)samples, voice_ptr + VOICE_CATSRC_1, SAMPLE_BUFFER_SIZE);
}

void adpcm_predict_frame(s16 *dst, const u8 *src, const u8 *nibbles, unsigned rshift) {
  *(dst++) = (s16)((src[0] << 8) | src[1]);
  *(dst++) = (s16)((src[2] << 8) | src[3]);

  for (u32 i = 1; i < 16; ++i) {
    u8 byte = nibbles[i];
    *(dst++) = adpcm_predict_sample(byte, 0xf0, 8, rshift);
    *(dst++) = adpcm_predict_sample(byte, 0x0f, 12, rshift);
  }
}

void adpcm_decode_frames(s16 *dst, const u8 *src, const s16 *table, u8 count, u8 skip_samples) {
  s16 frame[32];
  const u8 *nibbles = src + 8;
  const u8 *const src_end = gAdpcmBuffer + sizeof(gAdpcmBuffer);
  bool jump_gap = false;

  if (skip_samples >= 32) {
    jump_gap = true;
    nibbles += 16;
    src += 4;
  }

  for (u32 i = 0; i < count; ++i) {
    /* not in mupen: stop at the end of the ADPCM / sample buffers */
    if (nibbles + 16 > src_end || src + 4 > src_end || dst + 32 > gSamples + SAMPLE_BUFFER_SIZE)
      break;

    u8 c2 = nibbles[0];

    const s16 *book = (c2 & 0xf0) + table;
    unsigned rshift = (c2 & 0x0f);

    adpcm_predict_frame(frame, src, nibbles, rshift);

    dst[0] = frame[0];
    dst[1] = frame[1];
    adpcm_compute_residuals(dst + 2, frame + 2, book, dst, 6);
    adpcm_compute_residuals(dst + 8, frame + 8, book, dst + 6, 8);
    adpcm_compute_residuals(dst + 16, frame + 16, book, dst + 14, 8);
    adpcm_compute_residuals(dst + 24, frame + 24, book, dst + 22, 8);

    if (jump_gap) {
      nibbles += 8;
      src += 32;
    }

    jump_gap = !jump_gap;
    nibbles += 16;
    src += 4;
    dst += 32;
  }
}

void load_samples_ADPCM(u32 voice_ptr, s16 *samples, u32 *segbase, u32 *offset) {
  u8 *const buffer = gAdpcmBuffer;
  s16 *const adpcm_table = gAdpcmTable;

  u8 u8_3c = dram_u8(voice_ptr + VOICE_ADPCM_FRAMES);
  u8 u8_3d = dram_u8(voice_ptr + VOICE_ADPCM_FRAMES + 1);
  u8 u8_3e = dram_u8(voice_ptr + VOICE_SKIP_SAMPLES);
  u8 u8_3f = dram_u8(voice_ptr + VOICE_SKIP_SAMPLES + 1);
  u32 adpcm_table_ptr = dram_u32(voice_ptr + VOICE_ADPCM_TABLE_PTR);
  u32 count;

  dram_load_u16((u16 *)adpcm_table, adpcm_table_ptr, 128);

  count = u8_3c << 5;
  if (count > SAMPLE_BUFFER_SIZE) count = SAMPLE_BUFFER_SIZE;  /* not in mupen: buffer bound */

  *segbase = SAMPLE_BUFFER_SIZE - count;
  *offset = u8_3e & 0x1f;

  dma_cat8(buffer, voice_ptr + VOICE_CATSRC_0);
  adpcm_decode_frames(samples + *segbase, buffer, adpcm_table, u8_3c, u8_3e);

  if (u8_3d != 0) {
    dma_cat8(buffer, voice_ptr + VOICE_CATSRC_1);
    adpcm_decode_frames(samples, buffer, adpcm_table, u8_3d, u8_3f);
  }
}

/* Keeps a sample pointer inside gSamples (dot4 reads 4): mupen doesn't check */
inline const s16 *clamp_sample_ptr(const s16 *p) {
  if (p < gSamples) return gSamples;
  if (p > gSamples + SAMPLE_BUFFER_SIZE - 4) return gSamples + SAMPLE_BUFFER_SIZE - 4;
  return p;
}

void mix_voice_samples(musyx_t *musyx, u32 voice_ptr, const s16 *samples, u32 segbase, u32 offset,
                       u32 last_sample_ptr) {
  /* parse VOICE structure */
  const u16 pitch_q16 = dram_u16(voice_ptr + VOICE_PITCH_Q16);
  const u16 pitch_shift = dram_u16(voice_ptr + VOICE_PITCH_SHIFT); /* Q4.12 */

  const u16 end_point = dram_u16(voice_ptr + VOICE_END_POINT);
  const u16 restart_point = dram_u16(voice_ptr + VOICE_RESTART_POINT);

  const u16 u16_4e = dram_u16(voice_ptr + VOICE_U16_4E);

  /* init values and pointers */
  const s16 *sample = samples + segbase + offset + u16_4e;
  const s16 *const sample_end = samples + segbase + end_point;
  const s16 *const sample_restart =
      samples + (restart_point & 0x7fff) + (((restart_point & 0x8000) != 0) ? 0x000 : segbase);

  u32 pitch_accu = pitch_q16;
  u32 pitch_step = pitch_shift << 4;

  u32 v4_env[4];
  u32 v4_env_step[4];
  s16 *v4_dst[4];
  s16 v4[4];

  dram_load_u32(v4_env, voice_ptr + VOICE_ENV_BEGIN, 4);
  dram_load_u32(v4_env_step, voice_ptr + VOICE_ENV_STEP, 4);

  v4_dst[0] = musyx->left;
  v4_dst[1] = musyx->right;
  v4_dst[2] = musyx->cc0;
  v4_dst[3] = musyx->e50;

  for (u32 i = 0; i < SUBFRAME_SIZE; ++i) {
    /* update sample and lut pointers and then pitch_accu */
    const s16 *lut = (RESAMPLE_LUT + ((pitch_accu & 0xfc00) >> 8));
    s32 dist;
    s16 v;

    sample += (pitch_accu >> 16);
    pitch_accu &= 0xffff;
    pitch_accu += pitch_step;

    /* handle end/restart points */
    dist = (s32)(sample - sample_end);
    if (dist >= 0)
      sample = sample_restart + dist;

    /* apply resample filter */
    v = clamp_s16(dot4(clamp_sample_ptr(sample), lut));

    for (u32 k = 0; k < 4; ++k) {
      /* envmix */
      s32 accu = (v * ((s32)v4_env[k] >> 16)) >> 15;
      v4[k] = clamp_s16(accu);
      *(v4_dst[k]) = clamp_s16(accu + *(v4_dst[k]));

      /* update envelopes and dst pointers */
      ++(v4_dst[k]);
      v4_env[k] += v4_env_step[k];
    }
  }

  /* save last resampled sample */
  dram_store_u16((u16 *)v4, last_sample_ptr, 4);
}

/* Process voices, and returns interleaved subframe destination address */
u32 voice_stage(musyx_t *musyx, u32 voice_ptr, u32 last_sample_ptr) {
  u32 output_ptr;
  u32 i = 0;

  /* voice stage can be skipped if first voice has no samples */
  if (dram_u16(voice_ptr + VOICE_CATSRC_0 + CATSRC_SIZE1) == 0) {
    output_ptr = dram_u32(voice_ptr + VOICE_INTERLEAVED_PTR);
  } else {
    /* otherwise process voices until a non null output_ptr is encountered */
    for (;;) {
      /* load voice samples (PCM16 or APDCM) */
      u32 segbase;
      u32 offset;

      if (dram_u8(voice_ptr + VOICE_ADPCM_FRAMES) == 0)
        load_samples_PCM16(voice_ptr, gSamples, &segbase, &offset);
      else
        load_samples_ADPCM(voice_ptr, gSamples, &segbase, &offset);

      /* mix them with each internal subframes */
      mix_voice_samples(musyx, voice_ptr, gSamples, segbase, offset, last_sample_ptr + i * 8);

      /* check break condition */
      output_ptr = dram_u32(voice_ptr + VOICE_INTERLEAVED_PTR);
      if (output_ptr != 0)
        break;

      /* next voice (not in mupen: give up after MAX_VOICES) */
      ++i;
      if (i >= MAX_VOICES)
        break;
      voice_ptr += VOICE_SIZE;
    }
  }

  return output_ptr;
}

void mix_samples(s16 *y, s16 x, s16 hgain) {
  *y = clamp_s16(*y + ((x * hgain + 0x4000) >> 15));
}

void mix_subframes(s16 *y, const s16 *x, s16 hgain) {
  for (u32 i = 0; i < SUBFRAME_SIZE; ++i)
    mix_samples(&y[i], x[i], hgain);
}

void mix_fir4(s16 *y, const s16 *x, s16 hgain, const s16 *hcoeffs) {
  s32 h[4];

  h[0] = (hgain * hcoeffs[0]) >> 15;
  h[1] = (hgain * hcoeffs[1]) >> 15;
  h[2] = (hgain * hcoeffs[2]) >> 15;
  h[3] = (hgain * hcoeffs[3]) >> 15;

  for (u32 i = 0; i < SUBFRAME_SIZE; ++i) {
    u32 sum = (u32)(h[0] * x[i]) + (u32)(h[1] * x[i + 1]) + (u32)(h[2] * x[i + 2]) + (u32)(h[3] * x[i + 3]);
    s32 v = (s32)sum >> 15;
    y[i] = clamp_s16(y[i] + v);
  }
}

void mix_sfx_with_main_subframes_v1(musyx_t *musyx, const s16 *subframe, const u16 * /*gains*/) {
  for (u32 i = 0; i < SUBFRAME_SIZE; ++i) {
    s16 v = subframe[i];
    musyx->left[i] = clamp_s16(musyx->left[i] + v);
    musyx->right[i] = clamp_s16(musyx->right[i] + v);
  }
}

void mix_sfx_with_main_subframes_v2(musyx_t *musyx, const s16 *subframe, const u16 *gains) {
  for (u32 i = 0; i < SUBFRAME_SIZE; ++i) {
    s16 v = subframe[i];
    s16 v1 = (s16)((s32)(v * gains[0]) >> 16);
    s16 v2 = (s16)((s32)(v * gains[1]) >> 16);

    musyx->left[i] = clamp_s16(musyx->left[i] + v1);
    musyx->right[i] = clamp_s16(musyx->right[i] + v1);
    musyx->cc0[i] = clamp_s16(musyx->cc0[i] + v2);
  }
}

void sfx_stage(mix_sfx_with_main_subframes_t mix_sfx_with_main_subframes, musyx_t *musyx, u32 sfx_ptr, u16 idx) {
  s16 *const buffer = gSfxBuffer;
  s16 *const subframe = buffer + 4;
  s16 *const delayed = gSfxDelayed;

  u32 tap_delays[8];
  s16 tap_gains[8];
  s16 fir4_hcoeffs[4];

  s32 dpos, dlength;

  const u32 pos = idx * SUBFRAME_SIZE;

  u32 cbuffer_ptr;
  u32 cbuffer_length;
  u16 tap_count;
  s16 fir4_hgain;
  u16 sfx_gains[2];

  if (sfx_ptr == 0)
    return;

  /* load sfx  parameters */
  cbuffer_ptr = dram_u32(sfx_ptr + SFX_CBUFFER_PTR);
  cbuffer_length = dram_u32(sfx_ptr + SFX_CBUFFER_LENGTH);

  tap_count = dram_u16(sfx_ptr + SFX_TAP_COUNT);
  if (tap_count > 8) tap_count = 8;  /* not in mupen: tap arrays hold 8 */

  dram_load_u32(tap_delays, sfx_ptr + SFX_TAP_DELAYS, 8);
  dram_load_u16((u16 *)tap_gains, sfx_ptr + SFX_TAP_GAINS, 8);

  fir4_hgain = (s16)dram_u16(sfx_ptr + SFX_FIR4_HGAIN);
  dram_load_u16((u16 *)fir4_hcoeffs, sfx_ptr + SFX_FIR4_HCOEFFS, 4);

  sfx_gains[0] = dram_u16(sfx_ptr + SFX_U16_3C);
  sfx_gains[1] = dram_u16(sfx_ptr + SFX_U16_3E);

  /* mix up to 8 delayed subframes */
  memset(subframe, 0, SUBFRAME_SIZE * sizeof(subframe[0]));
  for (u32 i = 0; i < tap_count; ++i) {
    dpos = (s32)(pos - tap_delays[i]);
    if (dpos <= 0)
      dpos = (s32)((u32)dpos + cbuffer_length);
    dlength = SUBFRAME_SIZE;

    if ((u32)dpos + SUBFRAME_SIZE > cbuffer_length) {
      dlength = (s32)(cbuffer_length - dpos);
      if (dlength < 0) dlength = 0;  /* not in mupen: bad delay/length */
      if (dlength > SUBFRAME_SIZE) dlength = SUBFRAME_SIZE;
      dram_load_u16((u16 *)delayed + dlength, cbuffer_ptr, SUBFRAME_SIZE - dlength);
    }

    dram_load_u16((u16 *)delayed, cbuffer_ptr + (u32)dpos * 2, dlength);

    mix_subframes(subframe, delayed, tap_gains[i]);
  }

  /* add resulting subframe to main subframes */
  mix_sfx_with_main_subframes(musyx, subframe, sfx_gains);

  /* apply FIR4 filter and writeback filtered result */
  memcpy(buffer, musyx->subframe_740_last4, 4 * sizeof(s16));
  memcpy(musyx->subframe_740_last4, subframe + SUBFRAME_SIZE - 4, 4 * sizeof(s16));
  mix_fir4(musyx->e50, buffer + 1, fir4_hgain, fir4_hcoeffs);
  dram_store_u16((u16 *)musyx->e50, cbuffer_ptr + pos * 2, SUBFRAME_SIZE);
}

void interleave_stage_v1(musyx_t *musyx, u32 output_ptr) {
  s16 base_left = clamp_s16(musyx->base_vol[0]);
  s16 base_right = clamp_s16(musyx->base_vol[1]);

  const s16 *left = musyx->left;
  const s16 *right = musyx->right;

  for (u32 i = 0; i < SUBFRAME_SIZE; ++i) {
    u16 l = clamp_s16(*(left++) + base_left);
    u16 r = clamp_s16(*(right++) + base_right);

    dram_w32(output_ptr, ((u32)l << 16) | r);
    output_ptr += 4;
  }
}

void interleave_stage_v2(musyx_t *musyx, u16 mask_16, u32 ptr_18, u32 ptr_1c, u32 output_ptr) {
  u32 i, k;
  s16 *const subframe = gInterleaveSubframe;
  u16 mask;

  /* compute L_total, R_total and update subframe @ptr_1c */
  memset(subframe, 0, SUBFRAME_SIZE * sizeof(subframe[0]));

  for (i = 0; i < SUBFRAME_SIZE; ++i) {
    s16 v = (s16)dram_u16(ptr_1c + i * 2);
    musyx->left[i] = v;
    musyx->right[i] = clamp_s16(-v);
  }

  for (k = 0, mask = 1; k < 8; ++k, mask <<= 1, ptr_18 += 8) {
    s16 hgain;
    u32 address;

    if ((mask_16 & mask) == 0)
      continue;

    address = dram_u32(ptr_18);
    hgain = (s16)dram_u16(ptr_18 + 4);

    for (i = 0; i < SUBFRAME_SIZE; ++i, address += 2) {
      mix_samples(&musyx->left[i], (s16)dram_u16(address), hgain);
      mix_samples(&musyx->right[i], (s16)dram_u16(address + 2 * SUBFRAME_SIZE), hgain);
      mix_samples(&subframe[i], (s16)dram_u16(address + 4 * SUBFRAME_SIZE), hgain);
    }
  }

  /* interleave L_total and R_total */
  for (i = 0; i < SUBFRAME_SIZE; ++i) {
    u16 l = musyx->left[i];
    u16 r = musyx->right[i];
    dram_w32(output_ptr, ((u32)l << 16) | r);
    output_ptr += 4;
  }

  /* writeback subframe @ptr_1c */
  dram_store_u16((u16 *)subframe, ptr_1c, SUBFRAME_SIZE);
}

} // namespace

/**************************************************************************
 * MusyX v1 audio ucode
 **************************************************************************/
void Audio_MusyX_v1(u32 sfd_ptr, u32 sfd_count) {
  musyx_t *const musyx = &gMusyx;
  u32 state_ptr;

  if (sfd_count == 0)  /* not in mupen (would loop over 4G frames) */
    return;

  state_ptr = dram_u32(sfd_ptr + SFD_STATE_PTR);

  /* load initial state */
  load_base_vol(musyx->base_vol, state_ptr + STATE_BASE_VOL);
  dram_load_u16((u16 *)musyx->cc0, state_ptr + STATE_CC0, SUBFRAME_SIZE);
  dram_load_u16((u16 *)musyx->subframe_740_last4, state_ptr + STATE_740_LAST4_V1, 4);

  for (;;) {
    /* parse SFD structure */
    u16 sfx_index = dram_u16(sfd_ptr + SFD_SFX_INDEX);
    u32 voice_mask = dram_u32(sfd_ptr + SFD_VOICE_BITMASK);
    u32 sfx_ptr = dram_u32(sfd_ptr + SFD_SFX_PTR);
    u32 voice_ptr = sfd_ptr + SFD_VOICES;
    u32 last_sample_ptr = state_ptr + STATE_LAST_SAMPLE;
    u32 output_ptr;

    /* initialize internal subframes using updated base volumes */
    update_base_vol(musyx->base_vol, voice_mask, last_sample_ptr, 0, 0);
    init_subframes_v1(musyx);

    /* active voices get mixed into L,R,cc0,e50 subframes (optional) */
    output_ptr = voice_stage(musyx, voice_ptr, last_sample_ptr);

    /* apply delay-based effects (optional) */
    sfx_stage(mix_sfx_with_main_subframes_v1, musyx, sfx_ptr, sfx_index);

    /* emit interleaved L,R subframes */
    interleave_stage_v1(musyx, output_ptr);

    --sfd_count;
    if (sfd_count == 0)
      break;

    sfd_ptr += (u32)SFD_VOICES + (u32)MAX_VOICES * (u32)VOICE_SIZE;
    state_ptr = dram_u32(sfd_ptr + SFD_STATE_PTR);
  }

  /* writeback updated state */
  save_base_vol(musyx->base_vol, state_ptr + STATE_BASE_VOL);
  dram_store_u16((u16 *)musyx->cc0, state_ptr + STATE_CC0, SUBFRAME_SIZE);
  dram_store_u16((u16 *)musyx->subframe_740_last4, state_ptr + STATE_740_LAST4_V1, 4);
}

/**************************************************************************
 * MusyX v2 audio ucode
 **************************************************************************/
void Audio_MusyX_v2(u32 sfd_ptr, u32 sfd_count) {
  musyx_t *const musyx = &gMusyx;

  if (sfd_count == 0)  /* not in mupen (would loop over 4G frames) */
    return;

  for (;;) {
    /* parse SFD structure */
    u16 sfx_index = dram_u16(sfd_ptr + SFD_SFX_INDEX);
    u32 voice_mask = dram_u32(sfd_ptr + SFD_VOICE_BITMASK);
    u32 state_ptr = dram_u32(sfd_ptr + SFD_STATE_PTR);
    u32 sfx_ptr = dram_u32(sfd_ptr + SFD_SFX_PTR);
    u32 voice_ptr = sfd_ptr + SFD2_VOICES;

    u8 mask_15 = dram_u8(sfd_ptr + SFD2_15_BITMASK);
    u16 mask_16 = dram_u16(sfd_ptr + SFD2_16_BITMASK);
    u32 ptr_18 = dram_u32(sfd_ptr + SFD2_18_PTR);
    u32 ptr_1c = dram_u32(sfd_ptr + SFD2_1C_PTR);
    u32 ptr_20 = dram_u32(sfd_ptr + SFD2_20_PTR);
    u32 ptr_24 = dram_u32(sfd_ptr + SFD2_24_PTR);

    u32 last_sample_ptr = state_ptr + STATE_LAST_SAMPLE;
    u32 output_ptr;

    /* load state */
    load_base_vol(musyx->base_vol, state_ptr + STATE_BASE_VOL);
    dram_load_u16((u16 *)musyx->subframe_740_last4, state_ptr + STATE_740_LAST4_V2, 4);

    /* initialize internal subframes using updated base volumes */
    update_base_vol(musyx->base_vol, voice_mask, last_sample_ptr, mask_15, ptr_24);
    init_subframes_v2(musyx);

    /* ptr_10 / mask_14: unknown, ignored (mupen: TODO) */

    /* active voices get mixed into L,R,cc0,e50 subframes (optional) */
    output_ptr = voice_stage(musyx, voice_ptr, last_sample_ptr);

    /* apply delay-based effects (optional) */
    sfx_stage(mix_sfx_with_main_subframes_v2, musyx, sfx_ptr, sfx_index);

    dram_store_u16((u16 *)musyx->left, output_ptr, SUBFRAME_SIZE);
    dram_store_u16((u16 *)musyx->right, output_ptr + 2 * SUBFRAME_SIZE, SUBFRAME_SIZE);
    dram_store_u16((u16 *)musyx->cc0, output_ptr + 4 * SUBFRAME_SIZE, SUBFRAME_SIZE);

    /* store state */
    save_base_vol(musyx->base_vol, state_ptr + STATE_BASE_VOL);
    dram_store_u16((u16 *)musyx->subframe_740_last4, state_ptr + STATE_740_LAST4_V2, 4);

    if (mask_16)
      interleave_stage_v2(musyx, mask_16, ptr_18, ptr_1c, ptr_20);

    --sfd_count;
    if (sfd_count == 0)
      break;

    sfd_ptr += (u32)SFD2_VOICES + (u32)MAX_VOICES * (u32)VOICE_SIZE;
  }
}
