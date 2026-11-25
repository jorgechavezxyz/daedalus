/*
Copyright (C) 2001 StrmnNrmn

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


#include "Base/Types.h"

#include <random>
#include <limits>
#include <cmath>
#include <utility>


#include "Base/Macros.h"
#include "Core/CPU.h"
#include "Core/Interrupt.h"
#include "Core/DMA.h"
#include "Core/Dynamo.h"
#include "Core/R4300.h"
#include "Core/ROM.h"
#include "Interface/ConfigOptions.h"
#include "Debug/Registers.h"			// For REG_?? defines
#include "Debug/DBGConsole.h"
#include "Debug/DebugLog.h"
#include "DynaRec/TraceRecorder.h"

#include "Ultra/ultra_R4300.h"

#ifdef DAEDALUS_PSP
#include <pspfpu.h>
#include "SysPSP/Math/Math.h"	// VFPU Math

#define SIM_DOUBLES
#else
#include <float.h>
#endif

#if defined(DAEDALUS_POSIX) || defined(DAEDALUS_W32)
#include <fenv.h>
#endif

#define SPEEDHACK_INTERPRETER // Probably can disalbe this on the PSP?


#define	R4300_CALL_MAKE_OP( var )	OpCode	var;	var._u32 = op_code_bits

std::default_random_engine R4300_Rand();
static u32 R4300_GetRandom();

#if defined(DAEDALUS_PSP) && defined(SIM_DOUBLES)
#define R4300_IsNaN(x) 		isnanf((x))
#define R4300_Sqrt(x)		sqrtf((x))
#define R4300_SqrtD(x)		sqrtf((x))
#define R4300_AbsS(x) 		fabsf((x))
#define R4300_AbsD(x) 		fabsf((x))
#else
#define R4300_IsNaN(x)		std::isnan((x))
#define R4300_Sqrt(x)		sqrtf((x))
#define R4300_SqrtD(x)		sqrt((x))
#define R4300_AbsS(x) 		fabsf((x))
#define R4300_AbsD(x) 		fabs((x))
#endif

//	Abstract away the different rounding modes between targets
enum ERoundingMode
{
	RM_ROUND = 0,
	RM_TRUNC,
	RM_CEIL,
	RM_FLOOR,
	RM_NUM_MODES,
};
static ERoundingMode	gRoundingMode( RM_ROUND );

#if defined(DAEDALUS_PSP)

static const PspFpuRoundMode		gNativeRoundingModes[ RM_NUM_MODES ] =
{
	PSP_FPU_RN,	// RM_ROUND,
	PSP_FPU_RZ,	// RM_TRUNC,
	PSP_FPU_RP,	// RM_CEIL,
	PSP_FPU_RM,	// RM_FLOOR,
};

inline void SET_ROUND_MODE( ERoundingMode mode )
{
	//This is very expensive on the PSP, so is disabled, also this is skipt in the dynarec as well.
	//Note: Do not enable this, since it causes conflicts in the dynarec because we dont set rounding mode there. See Mario Party 3
	//TODO: Map 1:1 N64 and PSP round mode? perphaps write this in asm and just fiddle with the PSP CTC1 opcode?
	//pspFpuSetRoundmode( gNativeRoundingModes[ mode ] );
}


#elif DAEDALUS_POSIX || defined(DAEDALUS_W32)

static const int		gNativeRoundingModes[ RM_NUM_MODES ] =
{
	FE_TONEAREST,	// RM_ROUND,
	FE_TOWARDZERO,	// RM_TRUNC,
	FE_UPWARD,		// RM_CEIL,
	FE_DOWNWARD,	// RM_FLOOR,
};

inline void SET_ROUND_MODE( ERoundingMode mode )
{
	fesetround( gNativeRoundingModes[ mode ] );
}

#else

// Need defining
void SET_ROUND_MODE( ERoundingMode mode )
{
	#ifdef DAEDALUS_DEBUG_CONSOLE
	DAEDALUS_ERROR( "Floating point rounding modes not implemented on this platform" );
	#endif
}

#endif

// If the hardware doesn't support doubles in hardware - use 32 bits floats and accept the loss in precision
#ifdef SIM_DOUBLES
using d64 = f32;

#else
using d64 = f64;
#endif


inline void SpeedHack(u32 pc, u32 new_pc)
{
#ifdef SPEEDHACK_INTERPRETER
	// If jumping to the same address, this might be a busy-wait
	if (pc == new_pc)
	{
#ifdef DAEDALUS_ENABLE_DYNAREC
		if (gTraceRecorder.IsTraceActive())
			return;
#endif
		// TODO: Should maybe use some internal function, so we can account
		// for things like Branch/DelaySlot pair straddling a page boundary.
		u32 next_op = *(u32 *)(gLastAddress + 4);

		// If nop, then this is a busy-wait for an interrupt
		if (next_op == 0)
		{
			// XXXX if we leave the counter at 1, then we always terminate traces with a delay slot active.
			// Need a more permenant fix to for this - i.e. making tracing more robust.
			CPU_SkipToNextEvent();
		}
		// XXXX check this....need to update count....
		// This is:
		// 0x7f0d01e8: BNEL      v0 != v1 --> 0x7f0d01e8
		// 0x7f0d01ec: ADDIU     v0 = v0 + 0x0004
		/*else if (op._u32 == 0x5443ffff && next_op == 0x24420004)
		{
			gGPR[REG_v0]._u64 = gGPR[REG_v1]._u64 - 4;
		}*/
		/*else
		{
			static bool warned = false;

			if (!warned)
			{
				DBGConsole_Msg(0, "Missed Speedhack 0x%08x", gCPUState.CurrentPC);
				warned = true;
			}
		}*/
	}
#endif
}
//
//	A bit on FPU exceptions.
//		LWC1/LDC1/SWC1/SDC1		are unformatted, so no exceptions raised
//		MTC1/MFC1/DMTC1/DMFC1	are unformatted, so no exceptions raised
//		Word accessed must be 4 byte aligned, DWord accesses must be 8 byte aligned
//

//	Status.FR selects the FPU register model. FR=0 (libultra): 32-bit registers, a double or
//	64-bit value in an even/odd pair. FR=1 (Conker's Bad Fur Day): 32 registers of 64 bits, the
//	single-precision value in the low half. FPU[n] always holds what MTC1/LWC1/single ops on
//	register n access; the upper half of a 64-bit value is in FPU[n+1] (FR=0) or gFPUHi[n] (FR=1),
//	through gFPRHi[n] (mupen64plus set_fpr_pointers). On a mode switch the odd registers'
//	FPU[] entries swap with gFPUHi[n-1], so the 32 x 64-bit register file is kept intact.
bool			gDynarecFR0DoublesCompiled = false;
static u32		gFPUHi[32];
static u32 *	gFPRHi[32];
static bool		gFPRModeFR = false;

static void R4300_SetFPRPointers()
{
	for( u32 i = 0; i < 32; ++i )
	{
		gFPRHi[i] = gFPRModeFR ? &gFPUHi[i] : &gCPUState.FPU[i]._u32 + 1;	// FR=0: as before, FPU[i+1]
	}
}

static void R4300_UpdateFPRMode()
{
	const bool fr = ( gCPUState.CPUControl[C0_SR]._u32 & SR_FR ) != 0;
	if( fr == gFPRModeFR && gFPRHi[0] != nullptr )
		return;

	if( fr != gFPRModeFR )
	{
		for( u32 i = 1; i < 32; i += 2 )
		{
			std::swap( gCPUState.FPU[i]._u32, gFPUHi[i-1] );
		}
		gFPRModeFR = fr;

		// Recompiled double-precision code for register pairs is wrong now: drop it
		if( fr && gDynarecFR0DoublesCompiled )
		{
			gDynarecFR0DoublesCompiled = false;
			CPU_ResetFragmentCache();
		}
	}
	R4300_SetFPRPointers();
}

#define FPR_HI( reg )	( *gFPRHi[reg] )

inline void StoreFPR_Long( u32 reg, u64 value )
{
	REG64	r;
	r._u64 = value;

	gCPUState.FPU[reg+0]._u32 = r._u32_0;
	FPR_HI( reg ) = r._u32_1;
}


#define SIMULATESIG 0x1234	//Reduce signature to load value with one OP

inline u64 LoadFPR_Long( u32 reg )
{
	REG64 res;
#ifdef SIM_DOUBLES
	if (gCPUState.FPU[reg+0]._u32 == SIMULATESIG)
	{
		REG32 f; f._u32 = FPR_HI( reg );
		res._f64 = (f64)f._f32;	//Convert f32 -> f64
	}
	else
#endif
	{
		res._u32_0 = gCPUState.FPU[reg+0]._u32;
		res._u32_1 = FPR_HI( reg );
	}
	return res._u64;
}

inline d64 LoadFPR_Double( u32 reg )
{
#ifdef SIM_DOUBLES
	if (gCPUState.FPU[reg+0]._u32 == SIMULATESIG)
	{
		REG32 f; f._u32 = FPR_HI( reg );
		return (d64)f._f32;
	}
	else
#endif
	{
		REG64 res;
		res._u32_0 = gCPUState.FPU[reg+0]._u32;
		res._u32_1 = FPR_HI( reg );
		return (d64)res._f64;	//Converted f64 -> f32
	}
}

inline void StoreFPR_Double( u32 reg, d64 value )
{
#ifdef SIM_DOUBLES
	REG32 f; f._f32 = f32( value );	//No Coversion
	gCPUState.FPU[reg+0]._u32 = SIMULATESIG;
	FPR_HI( reg ) = f._u32;
#else
	REG64 r;
	r._f64 = value;

	gCPUState.FPU[reg+0]._u32 = r._u32_0;
	FPR_HI( reg ) = r._u32_1;
#endif
}

inline s32 LoadFPR_Word( u32 reg ) { 	return gCPUState.FPU[reg]._s32; }
inline void StoreFPR_Word( u32 reg, s32 value ) { gCPUState.FPU[reg]._s32 = value;}
inline f32 LoadFPR_Single( u32 reg ) { return gCPUState.FPU[reg]._f32; }
inline void StoreFPR_Single( u32 reg, f32 value ) { gCPUState.FPU[reg]._f32 = value; }

//	int -> float conversion routines
inline f32 s32_to_f32( s32 x ) { return (f32)x; }
inline d64 s32_to_d64( s32 x ) { return (d64)x; }
inline f32 s64_to_f32( s64 x ) { return (f32)x; }
inline d64 s64_to_d64( s64 x ) { return (d64)x; }


//	float -> float conversion routines
inline d64 f32_to_d64( f32 x ) { return (d64)x; }
inline f32 d64_to_f32( d64 x ) { return (f32)x; }


//	Float -> int conversion routines
#ifdef DAEDALUS_PSP

//These ASM routines convert float to int and puts the value in CPU to sign extend, rather than FPU since the PSP doesn't have 64bit instructions //Corn
//These can be risky since the N64 is expecting float to int64 and thus float can be larger than int, this happens with trunc_w_s on the 4th level of DK64..
#ifdef DAEDALUS_ENABLE_ASSERTS
inline s32 cvt_w_s( f32 x )							{ DAEDALUS_ASSERT( x >= std::numeric_limits<long>::lowest() && x <= std::numeric_limits<long>::max(), "Float too large, can't convert with 32bit PSP instruction" );s32 r; asm volatile ( "cvt.w.s %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
inline s32 trunc_w_s( f32 x )						{ DAEDALUS_ASSERT( x >= std::numeric_limits<long>::lowest() && x <= std::numeric_limits<long>::max(), "Float too large, can't convert with 32bit PSP instruction" );s32 r; asm volatile ( "trunc.w.s %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
inline s32 round_w_s( f32 x )						{ DAEDALUS_ASSERT( x >= std::numeric_limits<long>::lowest() && x <= std::numeric_limits<long>::max(), "Float too large, can't convert with 32bit PSP instruction" );s32 r; asm volatile ( "round.w.s %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
inline s32 ceil_w_s( f32 x )						{ DAEDALUS_ASSERT( x >= std::numeric_limits<long>::lowest() && x <= std::numeric_limits<long>::max(), "Float too large, can't convert with 32bit PSP instruction" );s32 r; asm volatile ( "ceil.w.s  %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
inline s32 floor_w_s( f32 x )						{ DAEDALUS_ASSERT( x >= std::numeric_limits<long>::lowest() && x <= std::numeric_limits<long>::max(), "Float too large, can't convert with 32bit PSP instruction" );s32 r; asm volatile ( "floor.w.s %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
#else
inline s32 cvt_w_s( f32 x )							{s32 r; asm volatile ( "cvt.w.s %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
inline s32 trunc_w_s( f32 x )						{s32 r; asm volatile ( "trunc.w.s %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
inline s32 round_w_s( f32 x )						{s32 r; asm volatile ( "round.w.s %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
inline s32 ceil_w_s( f32 x )						{s32 r; asm volatile ( "ceil.w.s  %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
inline s32 floor_w_s( f32 x )						{s32 r; asm volatile ( "floor.w.s %1, %1\nmfc1 %0,%1\n" : "=r"(r) : "f"(x) ); return r; }
#endif

inline s32 f32_to_s32_trunc( f32 x )				{ return pspFpuTrunc(x); }
inline s32 f32_to_s32_round( f32 x )				{ return pspFpuRound(x); }
inline s32 f32_to_s32_ceil( f32 x )					{ return pspFpuCeil(x); }
inline s32 f32_to_s32_floor( f32 x )				{ return pspFpuFloor(x); }
// CVT.W/CVT.L honour FCR31.RM (as mupen64plus cvt_w_s/cvt_l_s do), but the rounding is done
// explicitly here so the host FPU rounding mode is never changed (HLE runs on the same FPU).
// Single precision only. For |x| < 2^23, t = trunc(x) and (f32)t are exact, so frac = x - t
// is exact and in (-1, 1); RN is round-half-to-even, i.e. what cvt.w.s gives in PSP_FPU_RN.
// |x| >= 2^23 (and Inf/NaN) is already integral, so every mode equals truncation.
inline s32 f32_to_s32_rm( f32 x, u32 rm )
{
	if ( !( x > -8388608.0f && x < 8388608.0f ) )
		return trunc_w_s( x );	// last use of x: the asm overwrites its input register
	s32 t = (s32)x;
	f32 frac = x - (f32)t;
	switch ( rm & FPCSR_RM_MASK )
	{
	case FPCSR_RM_RZ:	return t;
	case FPCSR_RM_RP:	return frac > 0.0f ? t + 1 : t;
	case FPCSR_RM_RM:	return frac < 0.0f ? t - 1 : t;
	default:
		if ( frac > 0.5f || ( frac == 0.5f && ( t & 1 ) ) )		return t + 1;
		if ( frac < -0.5f || ( frac == -0.5f && ( t & 1 ) ) )	return t - 1;
		return t;
	}
}
// Outside +-2^23 x is integral, so the plain (exact) 64-bit conversion is already correct
inline s64 f32_to_s64_rm( f32 x, u32 rm )
{
	if ( x > -8388608.0f && x < 8388608.0f )
		return (s64)f32_to_s32_rm( x, rm );
	return (s64)x;
}

inline s32 f32_to_s32( f32 x )						{ return f32_to_s32_rm( x, gCPUState.FPUControl[31]._u32 ); }

//inline s64 f32_to_s64_trunc( f32 x )				{ return (s64)trunc_w_s( x ); }
inline s64 f32_to_s64_trunc( f32 x )				{ return (s64)truncf( x ); }
inline s64 f32_to_s64_round( f32 x )				{ return (s64)round_w_s( x ); }
inline s64 f32_to_s64_ceil( f32 x )					{ return (s64)ceil_w_s( x ); }
inline s64 f32_to_s64_floor( f32 x )				{ return (s64)floor_w_s( x ); }
inline s64 f32_to_s64( f32 x )						{ return f32_to_s64_rm( x, gCPUState.FPUControl[31]._u32 ); }

inline s32 d64_to_s32_trunc( d64 x )				{ return pspFpuTrunc( (f32)x ); }
inline s32 d64_to_s32_round( d64 x )				{ return pspFpuRound( (f32)x ); }
inline s32 d64_to_s32_ceil( d64 x )					{ return pspFpuCeil( (f32)x ); }
inline s32 d64_to_s32_floor( d64 x )				{ return pspFpuFloor( (f32)x ); }
inline s32 d64_to_s32( d64 x )						{ return f32_to_s32_rm( (f32)x, gCPUState.FPUControl[31]._u32 ); }

inline s64 d64_to_s64_trunc( d64 x )				{ return (s64)x; }
inline s64 d64_to_s64_round( d64 x )				{ return (s64)( x + 0.5f ); }
inline s64 d64_to_s64_ceil( d64 x )					{ return (s64)ceilf( x ); }
inline s64 d64_to_s64_floor( d64 x )				{ return (s64)floorf( x ); }
inline s64 d64_to_s64( d64 x )						{ return f32_to_s64_rm( (f32)x, gCPUState.FPUControl[31]._u32 ); }

#else

inline s32 f32_to_s32_trunc( f32 x )	{ SET_ROUND_MODE( RM_TRUNC ); return (s32)truncf(x); }
inline s32 f32_to_s32_round( f32 x )	{ SET_ROUND_MODE( RM_ROUND ); return (s32)roundf(x); }
inline s32 f32_to_s32_ceil( f32 x )	{ SET_ROUND_MODE( RM_CEIL ); return (s32)ceilf(x); }
inline s32 f32_to_s32_floor( f32 x )	{ SET_ROUND_MODE( RM_FLOOR ); return (s32)floorf(x); }
inline s32 f32_to_s32( f32 x )
{
#ifdef DAEDALUS_ACCURATE_CVT
	switch ( gCPUState.FPUControl[31]._u32 & FPCSR_RM_MASK )
	{
	case FPCSR_RM_RN:		return f32_to_s32_round( x );
	case FPCSR_RM_RZ:		return f32_to_s32_trunc( x );
	case FPCSR_RM_RP:		return f32_to_s32_ceil( x );
	case FPCSR_RM_RM:		return f32_to_s32_floor( x );
	default:				return (s32)x;
	}
#else
	SET_ROUND_MODE( gRoundingMode );
	return (s32)x;
#endif
}
inline s64 f32_to_s64_trunc( f32 x )	{ SET_ROUND_MODE( RM_TRUNC ); return (s64)truncf(x); }
inline s64 f32_to_s64_round( f32 x )	{ SET_ROUND_MODE( RM_ROUND ); return (s64)roundf(x); }
inline s64 f32_to_s64_ceil( f32 x )	{ SET_ROUND_MODE( RM_CEIL ); return (s64)ceilf(x); }
inline s64 f32_to_s64_floor( f32 x )	{ SET_ROUND_MODE( RM_FLOOR ); return (s64)floorf(x); }
inline s64 f32_to_s64( f32 x )
{
#ifdef DAEDALUS_ACCURATE_CVT
	switch ( gCPUState.FPUControl[31]._u32 & FPCSR_RM_MASK )
	{
	case FPCSR_RM_RN:		return f32_to_s64_round( x );
	case FPCSR_RM_RZ:		return f32_to_s64_trunc( x );
	case FPCSR_RM_RP:		return f32_to_s64_ceil( x );
	case FPCSR_RM_RM:		return f32_to_s64_floor( x );
	default:				return (s64)x;
	}
#else
	SET_ROUND_MODE( gRoundingMode );
	return (s64)x;
#endif
}
inline s32 d64_to_s32_trunc( d64 x )	{ SET_ROUND_MODE( RM_TRUNC ); return (s32)trunc(x); }
inline s32 d64_to_s32_round( d64 x )	{ SET_ROUND_MODE( RM_ROUND ); return (s32)round(x); }
inline s32 d64_to_s32_ceil( d64 x )	{ SET_ROUND_MODE( RM_CEIL ); return (s32)ceil(x); }
inline s32 d64_to_s32_floor( d64 x )	{ SET_ROUND_MODE( RM_FLOOR ); return (s32)floor(x); }
inline s32 d64_to_s32( d64 x )
{
#ifdef DAEDALUS_ACCURATE_CVT
	switch ( gCPUState.FPUControl[31]._u32 & FPCSR_RM_MASK )
	{
	case FPCSR_RM_RN:		return d64_to_s32_round( x );
	case FPCSR_RM_RZ:		return d64_to_s32_trunc( x );
	case FPCSR_RM_RP:		return d64_to_s32_ceil( x );
	case FPCSR_RM_RM:		return d64_to_s32_floor( x );
	default:				return (s32)x;
	}
#else
	SET_ROUND_MODE( gRoundingMode );
	return (s32)x;
#endif
}
inline s64 d64_to_s64_trunc( d64 x ) { SET_ROUND_MODE( RM_TRUNC ); return (s64)trunc(x); }
inline s64 d64_to_s64_round( d64 x ) { SET_ROUND_MODE( RM_ROUND ); return (s64)round(x); }
inline s64 d64_to_s64_ceil( d64 x )  { SET_ROUND_MODE( RM_CEIL ); return (s64)ceil(x); }
inline s64 d64_to_s64_floor( d64 x ) { SET_ROUND_MODE( RM_FLOOR ); return (s64)floor(x); }
inline s64 d64_to_s64( d64 x )
{
#ifdef DAEDALUS_ACCURATE_CVT
	switch ( gCPUState.FPUControl[31]._u32 & FPCSR_RM_MASK )
	{
	case FPCSR_RM_RN:		return d64_to_s64_round( x );
	case FPCSR_RM_RZ:		return d64_to_s64_trunc( x );
	case FPCSR_RM_RP:		return d64_to_s64_ceil( x );
	case FPCSR_RM_RM:		return d64_to_s64_floor( x );
	default:				return (s64)x;
	}
#else
	SET_ROUND_MODE( gRoundingMode );
	return (s64)x;
#endif
}
#endif

static void  R4300_Cop1_BCInstr( R4300_CALL_SIGNATURE );
static void  R4300_Cop1_SInstr( R4300_CALL_SIGNATURE );
static void  R4300_Cop1_DInstr( R4300_CALL_SIGNATURE );
static void  R4300_Cop1_WInstr( R4300_CALL_SIGNATURE );
static void  R4300_Cop1_LInstr( R4300_CALL_SIGNATURE );
static void  R4300_CoPro0( R4300_CALL_SIGNATURE );
static void  R4300_CoPro1( R4300_CALL_SIGNATURE );
static void  R4300_CoPro1_Disabled( R4300_CALL_SIGNATURE );
static void  R4300_Special( R4300_CALL_SIGNATURE );
static void  R4300_RegImm( R4300_CALL_SIGNATURE );
static void  R4300_Cop0_TLB( R4300_CALL_SIGNATURE );

static void  R4300_LWC1( R4300_CALL_SIGNATURE );
static void  R4300_LDC1( R4300_CALL_SIGNATURE );
static void  R4300_SWC1( R4300_CALL_SIGNATURE );
static void  R4300_SDC1( R4300_CALL_SIGNATURE );

//	Returns true if the specified opcode handler needs a valid entry in
//	gCPUState.CurrentPC to function correctly.
//	The PC must be set up for all branching instructions, but also
//	for any instruction that can potentially throw an exception (as it needs
//	to keep track of the location of the exception)
//	We could possibly get around this by explicitly setting the ErrorPC
//	AFTER the exception has been thrown. This might be a bit fiddly so
//	this function provides a conservative result for now.

bool	R4300_InstructionHandlerNeedsPC( OpCode op_code )
{
	switch( op_code.op )
	{
	// FIXME: These can potentially trow if memory is accessed through a function call?! //Salvy
	case OP_LWL:
	case OP_SWL:
	case OP_LWR:
	case OP_SWR:
	case OP_LDL:
	case OP_LDR:
	case OP_SDC1:
	case OP_LDC1:
		return false;

	case OP_ADDI:
	case OP_ADDIU:
	case OP_SLTI:
	case OP_SLTIU:
	case OP_ANDI:
	case OP_ORI:
	case OP_XORI:
	case OP_LUI:
	case OP_DADDI:
	case OP_DADDIU:
	case OP_CACHE:
		return false;

	case OP_SPECOP:
		//return R4300SpecialInstruction[ op_code.funct ];

		switch( op_code.spec_op )
		{
		case SpecOp_SLL:
		case SpecOp_SRL:
		case SpecOp_SRA:
		case SpecOp_SLLV:
		case SpecOp_SRLV:
		case SpecOp_SRAV:
		case SpecOp_MFHI:
		case SpecOp_MTHI:
		case SpecOp_MFLO:
		case SpecOp_MTLO:
		case SpecOp_DSLLV:
		case SpecOp_DSRLV:
		case SpecOp_DSRAV:
		case SpecOp_MULT:
		case SpecOp_MULTU:
		case SpecOp_DIV:		// Need to remove this if we can throw exception on divide by 0
		case SpecOp_DIVU:		// Ditto
		case SpecOp_DMULT:
		case SpecOp_DMULTU:
		case SpecOp_DDIV:		// Ditto
		case SpecOp_DDIVU:		// Ditto
		case SpecOp_ADD:		// Potentially can throw
		case SpecOp_ADDU:
		case SpecOp_SUB:		// Potentially can throw
		case SpecOp_SUBU:
		case SpecOp_AND:
		case SpecOp_OR:
		case SpecOp_XOR:
		case SpecOp_NOR:
		case SpecOp_SLT:
		case SpecOp_SLTU:
		case SpecOp_DADD:		// Potentially can throw
		case SpecOp_DADDU:
		case SpecOp_DSUB:		// Potentially can throw
		case SpecOp_DSUBU:
		case SpecOp_DSLL:
		case SpecOp_DSRL:
		case SpecOp_DSRA:
		case SpecOp_DSLL32:
		case SpecOp_DSRL32:
		case SpecOp_DSRA32:
			return false;
		default:
			break;
		}
		return true;

	case OP_REGIMM:
		// These are all traps or branches
		return true;

	case OP_COPRO0:
		// Only ERET needs PC
		return op_code.cop0tlb_funct == OP_ERET;

	case OP_COPRO1:
		// Potentially these can all throw, if cop1 is disabled
		// We explicitly handle this in the dynarec (we check the usuable flag once
		// per fragment). Care needs to be take if this is used elsewhere.
		return false;

	default:
		return true;
	}
}

void  R4300_SetSR( u32 new_value )
{
#ifdef DAEDALUS_DEBUG_CONSOLE
	// if((gCPUState.CPUControl[C0_SR]._u32 & SR_FR) != (new_value & SR_FR))
	// {
	// 	DBGConsole_Msg(0, "[MChanging FP to %s, STATUS=%08X", (new_value & SR_FR) ? "64bit" : "32bit", (new_value & SR_FR));
	// }
#endif

	bool interrupts_enabled_before = (gCPUState.CPUControl[C0_SR]._u32 & SR_IE) != 0;

	gCPUState.CPUControl[C0_SR]._u32 = new_value;
	R4300_UpdateFPRMode();

	bool interrupts_enabled_after = (gCPUState.CPUControl[C0_SR]._u32 & SR_IE) != 0;

	// CHECK COP1 UNUSUABLE
	if( (gCPUState.CPUControl[C0_SR]._u32 & SR_CU1) == 0 )
	{
		R4300Instruction[OP_COPRO1] = R4300_CoPro1_Disabled;
		R4300Instruction[OP_LWC1] = R4300_CoPro1_Disabled;
		R4300Instruction[OP_LDC1] = R4300_CoPro1_Disabled;
		R4300Instruction[OP_SWC1] = R4300_CoPro1_Disabled;
		R4300Instruction[OP_SDC1] = R4300_CoPro1_Disabled;
	}
	else
	{
		R4300Instruction[OP_COPRO1] = R4300_CoPro1;
		R4300Instruction[OP_LWC1] = R4300_LWC1;
		R4300Instruction[OP_LDC1] = R4300_LDC1;
		R4300Instruction[OP_SWC1] = R4300_SWC1;
		R4300Instruction[OP_SDC1] = R4300_SDC1;
	}

	// Serve any pending interrupts. Unmasking IM bits or clearing EXL/ERL with IE set
	// must also deliver interrupts that are already pending (mupen64plus checks on every write).
	(void)interrupts_enabled_before; (void)interrupts_enabled_after;
	R4300_CheckPendingInterrupts();
}

#ifdef DAEDALUS_ENABLE_ASSERTS
#define WARN_NOEXIST(inf)	{ DAEDALUS_ASSERT( false, "Instruction Unknown" ); }
#define WARN_NOIMPL(op)		{ DAEDALUS_ASSERT( false, "Instruction Not Implemented" ); }
#else
#define WARN_NOEXIST(inf) {}
#define WARN_NOIMPL(op) {}
#endif

// Reserved opcodes raise a Reserved Instruction exception (mupen64plus RESERVED). COP3 doesn't exist:
// its opcodes are reserved too, with CE = 0 (n64-systemtest cop_unusable).
static void  R4300_Unk( R4300_CALL_SIGNATURE )     { R4300_Exception_General( EXC_II, 0 ); }

// Undefined COP1 instructions raise a Floating-Point exception with only the Unimplemented
// Operation cause bit set (VR4300 hardware, n64-systemtest cop_unusable; mupen64plus DCFC1/DCTC1)
static void  R4300_Cop1_Unimplemented()
{
	gCPUState.FPUControl[31]._u32 = (gCPUState.FPUControl[31]._u32 & ~0x0003F000) | 0x00020000;
	R4300_Exception_General( EXC_FPE, 0 );
}

// COP2 has no coprocessor behind it, but MFC2/MTC2/CFC2/CTC2 (and the 64-bit DMFC2/DMTC2) move
// through a 64-bit latch; the other COP2 instructions are reserved with CE = 2. With CU2 clear
// they raise Coprocessor Unusable (mupen64plus MFC2..CTC2, RESERVED_COP2, check_cop2_unusable).
static u64 gCP2Latch = 0;

static bool R4300_Cop2Unusable()
{
	if( gCPUState.CPUControl[C0_SR]._u32 & SR_CU2 )
		return false;
	R4300_Exception_General( EXC_CPU, 2 );
	return true;
}

static void  R4300_ReservedCop2( R4300_CALL_SIGNATURE )
{
	if( !R4300_Cop2Unusable() )
		R4300_Exception_General( EXC_II, 2 );
}

static void  R4300_CoPro2( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	if( R4300_Cop2Unusable() )
		return;

	switch( op_code.cop0_op )		// fmt field (bits 25-21), same position for all coprocessors
	{
	case 0:	// MFC2
	case 2:	// CFC2
		gGPR[ op_code.rt ]._s64 = (s64)(s32)(u32)gCP2Latch;
		break;
	case 1:	// DMFC2
		gGPR[ op_code.rt ]._u64 = gCP2Latch;
		break;
	case 4:	// MTC2
	case 5:	// DMTC2
	case 6:	// CTC2
		gCP2Latch = gGPR[ op_code.rt ]._u64;
		break;
	default:
		R4300_Exception_General( EXC_II, 2 );
		break;
	}
}
static void  R4300_CoPro1_Disabled( R4300_CALL_SIGNATURE )

{
	// Cop1 Unusable
	#ifdef DAEDALUS_DEBUG_CONSOLE
	DBGConsole_Msg(0, "Thread accessing Cop1, throwing COP1 unusuable exception");
	DAEDALUS_ASSERT( (gCPUState.CPUControl[C0_SR]._u32 & SR_CU1) == 0, "COP1 usable flag in inconsistant state!" );
#endif
	R4300_Exception_CopUnusuable();
}

// Unaligned loads/stores raise an address error (AdEL/AdES) and don't access memory
// (VR4300 manual 6.4.6, n64-systemtest; not emulated by mupen64plus).
#define R4300_CHECK_ALIGN( address, mask, store )					\
	if( (address) & (mask) )										\
	{																\
		R4300_Exception_AddressError( (address), (store) );			\
		return;														\
	}

// LL/LLD set LLAddr to bits 35:4 of the physical address (VR4300 manual; n64-systemtest ll_sc).
// It is not cleared by ERET or SC, only the LLbit is.
static void R4300_SetLLAddr( u32 address )
{
	u32 physical;
	if( (address & 0xC0000000) == 0x80000000 )
	{
		physical = address & 0x1FFFFFFF;	// KSEG0/KSEG1
	}
	else
	{
		bool missing;
		physical = TLBEntry::Translate( address, missing );
	}
	gCPUState.CPUControl[C0_LLADDR]._u32 = physical >> 4;
}

// 8/16/64-bit stores to the cartridge ROM area (KSEG0/1, 0x10000000-0x1FBFFFFF) go to the PI write
// latch, shifted onto their byte lane (n64-systemtest cart_memory::write), instead of through the
// ROM read pointer into the ROM image. 32-bit stores reach the latch through WriteValue_ROM.
static inline bool R4300_IsCartRomAddress( u32 address )
{
	const u32 segment = address >> 29;
	return (segment == 4 || segment == 5) && ((address & 0x1FFFFFFF) - 0x10000000) < 0x0FC00000;
}

// The RCP (SP memory and the RCP registers, 0x04000000-0x048FFFFF) and PIF RAM only take 32-bit
// writes: SB/SH write the whole word, with the register shifted onto the byte lane and the other
// bytes taken from the shifted register too (n64-systemtest sp_memory and pif_memory SB/SH).
static inline bool R4300_IsRCPWordAddress( u32 address )
{
	const u32 segment = address >> 29;
	const u32 physical = address & 0x1FFFFFFF;
	return (segment == 4 || segment == 5) &&
		( (physical - 0x04000000) < 0x00900000 || (physical - 0x1FC007C0) < 0x40 );
}

// LL/SC: N64 has a single CPU, so the link only breaks on ERET (as in mupen64plus).
static void  R4300_LL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 3, false );
	const u32 value = Read32Bits(address);
	if( gCPUState.IsJobSet( CPU_CHECK_EXCEPTIONS ) )
		return;		// TLB exception: nothing loaded, no link
	gGPR[op_code.rt]._s64 = (s64)(s32)value;
	gCPUState.LLBit = 1;
	R4300_SetLLAddr( address );
}

static void  R4300_LLD( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 7, false );
	const u64 value = Read64Bits(address);
	if( gCPUState.IsJobSet( CPU_CHECK_EXCEPTIONS ) )
		return;		// TLB exception: nothing loaded, no link
	gGPR[op_code.rt]._u64 = value;
	gCPUState.LLBit = 1;
	R4300_SetLLAddr( address );
}

static void  R4300_SC( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 3, true );
	if( gCPUState.LLBit )
	{
		Write32Bits(address, gGPR[op_code.rt]._u32_0);
		gCPUState.LLBit = 0;
		gGPR[op_code.rt]._u64 = 1;
	}
	else
	{
		gGPR[op_code.rt]._u64 = 0;
	}
}

static void  R4300_SCD( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 7, true );
	if( gCPUState.LLBit )
	{
		Write64Bits(address, gGPR[op_code.rt]._u64);
		gCPUState.LLBit = 0;
		gGPR[op_code.rt]._u64 = 1;
	}
	else
	{
		gGPR[op_code.rt]._u64 = 0;
	}
}

static void  R4300_LDC2( R4300_CALL_SIGNATURE ) {  R4300_ReservedCop2( R4300_CALL_ARGUMENTS ); }
static void  R4300_SDC2( R4300_CALL_SIGNATURE ) {  R4300_ReservedCop2( R4300_CALL_ARGUMENTS ); }

// Trap instructions (mupen64plus e156eece): raise a Trap exception when the condition holds.
#define R4300_TRAP_IMM( name, cond )					\
static void  name( R4300_CALL_SIGNATURE )				\
{														\
	R4300_CALL_MAKE_OP( op_code );						\
	const s64 rs = gGPR[ op_code.rs ]._s64;				\
	const s64 imm = (s64)(s16)op_code.immediate;		\
	(void)rs; (void)imm;								\
	if( cond ) R4300_Exception_Trap();					\
}
#define R4300_TRAP_REG( name, cond )					\
static void  name( R4300_CALL_SIGNATURE )				\
{														\
	R4300_CALL_MAKE_OP( op_code );						\
	const s64 rs = gGPR[ op_code.rs ]._s64;				\
	const s64 rt = gGPR[ op_code.rt ]._s64;				\
	(void)rs; (void)rt;									\
	if( cond ) R4300_Exception_Trap();					\
}
R4300_TRAP_IMM( R4300_RegImm_TGEI,  rs >= imm )
R4300_TRAP_IMM( R4300_RegImm_TGEIU, (u64)rs >= (u64)imm )
R4300_TRAP_IMM( R4300_RegImm_TLTI,  rs < imm )
R4300_TRAP_IMM( R4300_RegImm_TLTIU, (u64)rs < (u64)imm )
R4300_TRAP_IMM( R4300_RegImm_TEQI,  rs == imm )
R4300_TRAP_IMM( R4300_RegImm_TNEI,  rs != imm )
R4300_TRAP_REG( R4300_Special_TGE,  rs >= rt )
R4300_TRAP_REG( R4300_Special_TGEU, (u64)rs >= (u64)rt )
R4300_TRAP_REG( R4300_Special_TLT,  rs < rt )
R4300_TRAP_REG( R4300_Special_TLTU, (u64)rs < (u64)rt )
R4300_TRAP_REG( R4300_Special_TEQ,  rs == rt )
R4300_TRAP_REG( R4300_Special_TNE,  rs != rt )
#undef R4300_TRAP_IMM
#undef R4300_TRAP_REG

// Branch-likely-and-link (mupen64plus 9563e24c). RA is written whether or not the branch is taken.
static void  R4300_RegImm_BLTZALL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	const s64 rs = gGPR[ op_code.rs ]._s64;
	gGPR[REG_ra]._s64 = (s64)(s32)(gCPUState.CurrentPC + 8);
	if ( rs < 0 )
	{
		u32	new_pc = gCPUState.CurrentPC + ((s32)(s16)op_code.immediate<<2) + 4;
		CPU_TakeBranch( new_pc );
	}
	else
	{
		INCREMENT_PC();		// Nullify the delay slot
	}
}

static void  R4300_RegImm_BGEZALL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	const s64 rs = gGPR[ op_code.rs ]._s64;
	gGPR[REG_ra]._s64 = (s64)(s32)(gCPUState.CurrentPC + 8);
	if ( rs >= 0 )
	{
		u32	new_pc = gCPUState.CurrentPC + ((s32)(s16)op_code.immediate<<2) + 4;
		CPU_TakeBranch( new_pc );
	}
	else
	{
		INCREMENT_PC();		// Nullify the delay slot
	}
}


static void  R4300_DBG_Bkpt( R4300_CALL_SIGNATURE )
{
#ifdef DAEDALUS_BREAKPOINTS_ENABLED
	R4300_CALL_MAKE_OP( op_code );

	// Entry is in lower 26 bits...
	u32 dwBreakPoint = op_code.bp_index;

	if (g_BreakPoints[dwBreakPoint].mEnabled && !g_BreakPoints[dwBreakPoint].mTemporaryDisable)
	{
		// Set the temporary disable so we don't execute bp immediately again
		g_BreakPoints[dwBreakPoint].mTemporaryDisable = true;
		CPU_Halt("BreakPoint");
		DBGConsole_Msg(0, "[RBreakPoint at 0x%08x]", gCPUState.CurrentPC);
		DECREMENT_PC();
	}
	else
	{
		// If this was on, disable it
		g_BreakPoints[dwBreakPoint].mTemporaryDisable = false;
		OpCode	original_op( g_BreakPoints[dwBreakPoint].mOriginalOp );
		R4300Instruction[ original_op.op ]( original_op._u32 );
	}
#else
	// Without the debugger this opcode is SWC2
	R4300_ReservedCop2( R4300_CALL_ARGUMENTS );
#endif
}

static void  R4300_J( R4300_CALL_SIGNATURE ) 				// Jump
{
	R4300_CALL_MAKE_OP( op_code );

	u32 pc = gCPUState.CurrentPC;
	u32 new_pc = (pc & 0xF0000000) | (op_code.target<<2);

	//SpeedHack(pc, new_pc);	// PMario and Tarzan use this, is it worth?
	CPU_TakeBranch( new_pc );
}

static void  R4300_JAL( R4300_CALL_SIGNATURE ) 				// Jump And Link
{
	R4300_CALL_MAKE_OP( op_code );

	u32 pc = gCPUState.CurrentPC;
	gGPR[REG_ra]._s64 = (s64)(s32)(pc + 8);		// Store return address
	u32	new_pc = (pc & 0xF0000000) | (op_code.target<<2);
	CPU_TakeBranch( new_pc );
	SpeedHack(pc, new_pc);
}

static void  R4300_BEQ( R4300_CALL_SIGNATURE ) 		// Branch on Equal
{
	R4300_CALL_MAKE_OP( op_code );

	if ( gGPR[op_code.rs]._u64 == gGPR[op_code.rt]._u64 )
	{
		s16 offset = (s16)op_code.immediate;
		u32 pc = gCPUState.CurrentPC;
		u32 new_pc = pc + ((s32)offset<<2) + 4;

		SpeedHack(pc, new_pc);
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_BNE( R4300_CALL_SIGNATURE )             // Branch on Not Equal
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs <> rt
	if ( gGPR[op_code.rs]._u64 != gGPR[op_code.rt]._u64 )
	{
		s16 offset = (s16)op_code.immediate;
		u32 pc = gCPUState.CurrentPC;
		u32 new_pc = pc + ((s32)offset<<2) + 4;

		SpeedHack(pc, new_pc);
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_BLEZ( R4300_CALL_SIGNATURE ) 			// Branch on Less than of Equal to Zero
{
	R4300_CALL_MAKE_OP( op_code );
	//branch if rs <= 0
	if (gGPR[op_code.rs]._s64 <= 0)
	{
		s16 offset( (s16)op_code.immediate );
		u32 pc( gCPUState.CurrentPC );
		u32 new_pc( pc + ((s32)offset<<2) + 4 );

		SpeedHack(pc, new_pc);
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_BGTZ( R4300_CALL_SIGNATURE ) 			// Branch on Greater than Zero
{
	R4300_CALL_MAKE_OP( op_code );

	//This compare needs to be 64bit! otherwise DK can go through walls (DK64)
	//if (gGPR[op_code.rs]._s32_0 > 0)
	if (gGPR[op_code.rs]._s64 > 0)
	{
		s16 offset{ (s16)op_code.immediate };
		u32 pc{ gCPUState.CurrentPC };
		u32 new_pc{ pc + ((s32)offset<<2) + 4 };

		SpeedHack(pc, new_pc);
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_DADDI( R4300_CALL_SIGNATURE ) 			// Doubleword ADD Immediate
{
	R4300_CALL_MAKE_OP( op_code );
	s64 result;
	if( __builtin_add_overflow( gGPR[op_code.rs]._s64, (s64)(s16)op_code.immediate, &result ) )
	{
		R4300_Exception_General( EXC_OV, 0 );	// Integer overflow: rt is not written
		return;
	}
	gGPR[op_code.rt]._s64 = result;
}

static void  R4300_DADDIU( R4300_CALL_SIGNATURE ) 			// Doubleword ADD Immediate Unsigned
{
	R4300_CALL_MAKE_OP( op_code );
	gGPR[op_code.rt]._s64 = gGPR[op_code.rs]._s64 + (s32)(s16)op_code.immediate;
}

static void  R4300_ADDI( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	s32 result;
	if( __builtin_add_overflow( gGPR[op_code.rs]._s32_0, (s32)(s16)op_code.immediate, &result ) )
	{
		R4300_Exception_General( EXC_OV, 0 );	// Integer overflow: rt is not written
		return;
	}
	gGPR[op_code.rt]._s64 = (s64)result;
}

static void  R4300_ADDIU( R4300_CALL_SIGNATURE ) 		// Add Immediate Unsigned
{
	R4300_CALL_MAKE_OP( op_code );
	gGPR[op_code.rt]._s64 = (s64)(s32)(gGPR[op_code.rs]._s32_0 + (s32)(s16)op_code.immediate);
}

static void  R4300_SLTI( R4300_CALL_SIGNATURE ) 			// Set on Less Than Immediate
{
	R4300_CALL_MAKE_OP( op_code );

	// Cast to s32s to ensure sign is taken into account
	if (gGPR[op_code.rs]._s64 < (s64)(s32)(s16)op_code.immediate)
	{
		gGPR[op_code.rt]._u64 = 1;
	}
	else
	{
#if 1 //1->default, 0->"hack" that speeds up SM64 when sound is off (just for reference) //Salvy
		gGPR[op_code.rt]._u64 = 0;
#else
		gGPR[op_code.rt]._u32_0 = 0;
#endif
	}
}

static void  R4300_SLTIU( R4300_CALL_SIGNATURE ) 		// Set on Less Than Immediate Unsigned
{
	R4300_CALL_MAKE_OP( op_code );

	// Cast to s32s to ensure sign is taken into account
	if (gGPR[op_code.rs]._u64 < (u64)(s64)(s32)(s16)op_code.immediate)
	{
		gGPR[op_code.rt]._u64 = 1;
	}
	else
	{
		gGPR[op_code.rt]._u64 = 0;
	}
}

static void  R4300_ANDI( R4300_CALL_SIGNATURE ) 				// AND Immediate
{
	R4300_CALL_MAKE_OP( op_code );

	//rt = rs & immediate
	gGPR[op_code.rt]._u64 = gGPR[op_code.rs]._u64 & (u64)(u16)op_code.immediate;
}


static void  R4300_ORI( R4300_CALL_SIGNATURE ) 				// OR Immediate
{
	R4300_CALL_MAKE_OP( op_code );

	//rt = rs | immediate
	gGPR[op_code.rt]._u64 = gGPR[op_code.rs]._u64 | (u64)(u16)op_code.immediate;
}

static void  R4300_XORI( R4300_CALL_SIGNATURE ) 				// XOR Immediate
{
	R4300_CALL_MAKE_OP( op_code );

	//rt = rs ^ immediate
	gGPR[op_code.rt]._u64 = gGPR[op_code.rs]._u64 ^ (u64)(u16)op_code.immediate;
}

static void  R4300_LUI( R4300_CALL_SIGNATURE ) 				// Load Upper Immediate
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[op_code.rt]._s64 = (s64)(s32)((s32)(s16)op_code.immediate<<16);
}

static void  R4300_BEQL( R4300_CALL_SIGNATURE ) 			// Branch on Equal Likely
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs == rt
	if ( gGPR[op_code.rs]._u64 == gGPR[op_code.rt]._u64 )
	{
		s16 offset( (s16)op_code.immediate );
		u32 pc( gCPUState.CurrentPC );
		u32 new_pc( pc + ((s32)offset<<2) + 4 );

		SpeedHack(pc, new_pc);
		CPU_TakeBranch( new_pc );
	}
	else
	{
		// Don't execute subsequent instruction
		INCREMENT_PC();
	}
}

static void  R4300_BNEL( R4300_CALL_SIGNATURE ) 			// Branch on Not Equal Likely
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs <> rt
	if ( gGPR[op_code.rs]._u64 != gGPR[op_code.rt]._u64 )
	{
		s16 offset = (s16)op_code.immediate;
		u32 pc = gCPUState.CurrentPC;
		u32 new_pc = pc + ((s32)offset<<2) + 4;
		CPU_TakeBranch( new_pc );
		SpeedHack(pc, new_pc);
	}
	else
	{
		// Don't execute subsequent instruction
		INCREMENT_PC();
	}
}

static void  R4300_BLEZL( R4300_CALL_SIGNATURE ) 		// Branch on Less than or Equal to Zero Likely
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs <= 0
	if ( gGPR[op_code.rs]._s64 <= 0 )
	{
		s16 offset = (s16)op_code.immediate;
		u32 pc = gCPUState.CurrentPC;
		u32 new_pc = pc + ((s32)offset<<2) + 4;

		SpeedHack(pc, new_pc);
		CPU_TakeBranch( new_pc );
	}
	else
	{
		// Don't execute subsequent instruction
		INCREMENT_PC();
	}
}

static void  R4300_BGTZL( R4300_CALL_SIGNATURE ) 		// Branch on Greater than Zero Likely
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs > 0
	if ( gGPR[op_code.rs]._s64 > 0 )
	{
		//ToDo : SpeedHack?
		u32	new_pc = gCPUState.CurrentPC + ((s32)(s16)op_code.immediate<<2) + 4;
		CPU_TakeBranch( new_pc );
	}
	else
	{
		// Don't execute subsequent instruction
		INCREMENT_PC();
	}
}

static void  R4300_LB( R4300_CALL_SIGNATURE ) 			// Load Byte
{
	R4300_CALL_MAKE_OP( op_code );
	#ifdef DAEDALUS_DEBUG_CONSOLE

#endif
	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );

	gGPR[op_code.rt]._s64 = (s64)(s8)Read8Bits(address);
}

static void  R4300_LBU( R4300_CALL_SIGNATURE ) 			// Load Byte Unsigned -- Zero extend byte...
{
	R4300_CALL_MAKE_OP( op_code );
		#ifdef DAEDALUS_DEBUG_CONSOLE

	#endif

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate);

	gGPR[op_code.rt]._u64 = (u64)(u8)Read8Bits(address);
}

static void  R4300_LH( R4300_CALL_SIGNATURE ) 		// Load Halfword
{
	R4300_CALL_MAKE_OP( op_code );
		#ifdef DAEDALUS_DEBUG_CONSOLE

#endif
	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 1, false );

	gGPR[op_code.rt]._s64 = (s64)(s16)Read16Bits(address);
}

static void  R4300_LHU( R4300_CALL_SIGNATURE )			// Load Halfword Unsigned -- Zero extend word
{
	R4300_CALL_MAKE_OP( op_code );
		#ifdef DAEDALUS_DEBUG_CONSOLE

#endif
	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 1, false );

	gGPR[op_code.rt]._u64 = (u64)(u16)Read16Bits(address);
}


static void  R4300_LWL( R4300_CALL_SIGNATURE ) 			// Load Word Left
{
	R4300_CALL_MAKE_OP( op_code );
		#ifdef DAEDALUS_DEBUG_CONSOLE

	#endif
	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	u32 nMem = Read32Bits(address & ~0x3);
	u32 nReg = gGPR[op_code.rt]._u32_0;

#if 1 //1-> tighter code, 0->old way //Corn
	nReg = (nReg & ~(~0 << ((address & 0x3) << 3))) | (nMem << ((address & 0x3) << 3));
#else
	switch (address % 4)
	{
        case 0: nReg = nMem; break;
        case 1: nReg = ((nReg & 0x000000FF) | (nMem << 8));  break;
        case 2: nReg = ((nReg & 0x0000FFFF) | (nMem << 16)); break;
        case 3: nReg = ((nReg & 0x00FFFFFF) | (nMem << 24)); break;
    }
#endif

	gGPR[op_code.rt]._s64 = (s64)(s32)nReg;
}

// Starcraft - not tested!
static void  R4300_LDL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	u64 nMem = Read64Bits(address & ~0x7);

	u64 nReg = gGPR[op_code.rt]._u64;
#if 1 //1-> tighter code, 0->old way //Corn
	nReg = (nReg & ~(~0LL << ((address & 0x7) << 3))) | (nMem << ((address & 0x7) << 3));
#else
	switch (address % 8)
	{
        case 0: nReg = nMem; break;
        case 1: nReg = ((nReg & 0x00000000000000FFLL) | (nMem << 8));  break;
        case 2: nReg = ((nReg & 0x000000000000FFFFLL) | (nMem << 16)); break;
        case 3: nReg = ((nReg & 0x0000000000FFFFFFLL) | (nMem << 24)); break;
        case 4: nReg = ((nReg & 0x00000000FFFFFFFFLL) | (nMem << 32)); break;
        case 5: nReg = ((nReg & 0x000000FFFFFFFFFFLL) | (nMem << 40)); break;
        case 6: nReg = ((nReg & 0x0000FFFFFFFFFFFFLL) | (nMem << 48)); break;
        case 7: nReg = ((nReg & 0x00FFFFFFFFFFFFFFLL) | (nMem << 56)); break;
   }
#endif
	gGPR[op_code.rt]._u64 = nReg;
}


static void  R4300_LWR( R4300_CALL_SIGNATURE ) 			// Load Word Right
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	u32 nMem = Read32Bits(address & ~0x3);

	u32 nReg = gGPR[op_code.rt]._u32_0;

#if 1 //1-> tighter code, 0->old way //Corn
	nReg = (nReg & (~0 << ( ((address & 0x3) + 1) << 3))) | (nMem >> ((~address & 0x3) << 3));
#else
	switch (address % 4)
	{
        case 0: nReg = (nReg & 0xFFFFFF00) | (nMem >> 24); break;
        case 1: nReg = (nReg & 0xFFFF0000) | (nMem >> 16); break;
        case 2: nReg = (nReg & 0xFF000000) | (nMem >>  8); break;
        case 3: nReg = nMem; break;
    }
#endif

	gGPR[op_code.rt]._s64 = (s64)(s32)nReg;
}

// Starcraft - not tested!
static void  R4300_LDR( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	u64 nMem = Read64Bits(address & ~0x7);
	u64 nReg = gGPR[op_code.rt]._u64;

#if 1 //1-> tighter code, 0->old way //Corn
	nReg = (nReg & (~0LL << ( ((address & 0x7) + 1) << 3))) | (nMem >> ((~address & 0x7) << 3));
#else
	switch (address % 8)
	{
        case 0: nReg = (nReg & 0xFFFFFFFFFFFFFF00LL) | (nMem >> 56); break;
        case 1: nReg = (nReg & 0xFFFFFFFFFFFF0000LL) | (nMem >> 48); break;
        case 2: nReg = (nReg & 0xFFFFFFFFFF000000LL) | (nMem >> 40); break;
        case 3: nReg = (nReg & 0xFFFFFFFF00000000LL) | (nMem >> 32); break;
        case 4: nReg = (nReg & 0xFFFFFF0000000000LL) | (nMem >> 24); break;
        case 5: nReg = (nReg & 0xFFFF000000000000LL) | (nMem >> 16); break;
        case 6: nReg = (nReg & 0xFF00000000000000LL) | (nMem >>  8); break;
        case 7: nReg = nMem; break;
    }
#endif

	gGPR[op_code.rt]._s64 = nReg;
}


static void  R4300_LW( R4300_CALL_SIGNATURE ) 			// Load Word
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 3, false );
	gGPR[op_code.rt]._s64 = (s64)(s32)Read32Bits(address);
}

static void  R4300_LWU( R4300_CALL_SIGNATURE ) 			// Load Word Unsigned
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 3, false );
	gGPR[op_code.rt]._u64 = (u64)(u32)Read32Bits(address);
}

static void  R4300_SW( R4300_CALL_SIGNATURE ) 			// Store Word
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 3, true );
	Write32Bits(address, gGPR[op_code.rt]._u32_0);
}

static void  R4300_SH( R4300_CALL_SIGNATURE ) 			// Store Halfword
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 1, true );
	if( Memory_StoreRaisesTLBException( address ) ) return;
	if( R4300_IsCartRomAddress( address ) )
	{
		DMA_PI_CartWrite( gGPR[op_code.rt]._u32_0 << ((2 - (address & 2)) * 8) );
		return;
	}
	if( R4300_IsRCPWordAddress( address ) )
	{
		Write32Bits( address & ~3, gGPR[op_code.rt]._u32_0 << ((2 - (address & 2)) * 8) );
		return;
	}
	Write16Bits(address, (u16)(gGPR[op_code.rt]._u32_0 & 0xffff));
}

static void  R4300_SB( R4300_CALL_SIGNATURE ) 			// Store Byte
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	if( Memory_StoreRaisesTLBException( address ) ) return;
	if( R4300_IsCartRomAddress( address ) )
	{
		DMA_PI_CartWrite( gGPR[op_code.rt]._u32_0 << ((3 - (address & 3)) * 8) );
		return;
	}
	if( R4300_IsRCPWordAddress( address ) )
	{
		Write32Bits( address & ~3, gGPR[op_code.rt]._u32_0 << ((3 - (address & 3)) * 8) );
		return;
	}
	Write8Bits(address, (u8)(gGPR[op_code.rt]._u32_0 & 0xff));
}

static void  R4300_SWL( R4300_CALL_SIGNATURE ) 			// Store Word Left
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	if( Memory_StoreRaisesTLBException( address ) ) return;
	u8* base = (u8*)ReadAddress(address & ~0x3);
	u32 dMem = QuickRead32Bits(base, 0x0);
	u32 dReg = gGPR[op_code.rt]._u32_0;

#if 1 //1-> tighter code, 0->old way //Corn
	u32 dNew = (dMem & ~(((u32)~0 >> ((address & 0x3) << 3)))) | (dReg >> ((address & 0x3) << 3));
#else
	u32 dNew = 0;
	switch (address % 4)
	{
	case 0:	dNew = dReg; break;			// Aligned
	case 1:	dNew = (dMem & 0xFF000000) | (dReg >> 8 ); break;
	case 2:	dNew = (dMem & 0xFFFF0000) | (dReg >> 16); break;
	default:dNew = (dMem & 0xFFFFFF00) | (dReg >> 24); break;
	}
#endif

	QuickWrite32Bits(base, 0x0, dNew);
}

static void  R4300_SWR( R4300_CALL_SIGNATURE ) 			// Store Word Right
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	if( Memory_StoreRaisesTLBException( address ) ) return;
	u8* base = (u8*)ReadAddress(address & ~0x3);
	u32 dMem = QuickRead32Bits(base, 0x0);
	u32 dReg = gGPR[op_code.rt]._u32_0;

#if 1 //1-> tighter code, 0->old way //Corn
	u32 dNew = (dMem & ~(~0 << ((~address & 0x3) << 3))) | (dReg << ((~address & 0x3) << 3));
#else
	u32 dNew = 0;
	switch (address % 4)
	{
	case 0:	dNew = (dMem & 0x00FFFFFF) | (dReg << 24); break;
	case 1:	dNew = (dMem & 0x0000FFFF) | (dReg << 16); break;
	case 2:	dNew = (dMem & 0x000000FF) | (dReg << 8); break;
	default:dNew = dReg; break;			// Aligned
	}
#endif

	QuickWrite32Bits(base, 0x0, dNew);

}

static void  R4300_SDL( R4300_CALL_SIGNATURE )//CYRUS64
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate);
	if( Memory_StoreRaisesTLBException( address ) ) return;
	u8* base = (u8*)ReadAddress(address & ~0x7);
	u64 nMem = QuickRead64Bits(base, 0x0);
	u64 nReg = gGPR[op_code.rt]._u64;
	u64 nNew = (nMem & ~(((u64)~0LL >> ((address & 0x7) << 3)))) | (nReg >> ((address & 0x7) << 3));

	QuickWrite64Bits(base, 0x0, nNew);
}

static void  R4300_SDR( R4300_CALL_SIGNATURE )//CYRUS64
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate);
	if( Memory_StoreRaisesTLBException( address ) ) return;
	u8* base = (u8*)ReadAddress(address & ~0x7);

	u64 nMem = QuickRead64Bits(base, 0x0);
	u64 nReg = gGPR[op_code.rt]._u64;
	u64 nNew = (nMem & ~(~0LL << ((~address & 0x7) << 3))) | (nReg << ((~address & 0x7) << 3));
	QuickWrite64Bits(base, 0x0, nNew);
}

static void  R4300_CACHE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
//	return;

#ifdef DAEDALUS_ENABLE_DYNAREC
	u32 cache_op  = op_code.rt;
	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );

	// Do Nothing
	u32 dwCache = cache_op & 0x3;
	u32 dwAction = (cache_op >> 2) & 0x7;

	if(dwCache == 0 && dwAction == 0)
	{
		// Index_Invalidate_I: the address only selects a cache line, which can hold code from
		// anywhere in memory (osInvalICache uses this on 0x80000000-0x80003FFF for ranges of
		// 16KB or more), so drop all compiled code at the next safe point.
		CPU_InvalidateICache();
	}
	else if(dwCache == 0 && dwAction == 4)
	{
		// Hit_Invalidate_I: only this line's address
		CPU_InvalidateICacheRange(address, 0x20);
	}
	//DBGConsole_Msg(0, "CACHE %s/%d, 0x%08x", gCacheNames[dwCache], dwAction, address);
#endif
}

static void  R4300_LWC1( R4300_CALL_SIGNATURE ) 				// Load Word to Copro 1 (FPU)
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 3, false );
	StoreFPR_Word( op_code.ft, Read32Bits(address) );
}


static void  R4300_LDC1( R4300_CALL_SIGNATURE )				// Load Doubleword to Copro 1 (FPU)
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 7, false );
	StoreFPR_Long( op_code.ft, Read64Bits(address));
}


static void  R4300_LD( R4300_CALL_SIGNATURE ) 				// Load Doubleword
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 7, false );
	gGPR[op_code.rt]._u64 = Read64Bits(address);
}


static void  R4300_SWC1( R4300_CALL_SIGNATURE ) 			// Store Word From Copro 1
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 3, true );
	Write32Bits(address, LoadFPR_Word(op_code.ft));
}

static void  R4300_SDC1( R4300_CALL_SIGNATURE )		// Store Doubleword From Copro 1
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 7, true );
	if( Memory_StoreRaisesTLBException( address ) ) return;
	Write64Bits(address, LoadFPR_Long(op_code.ft));
}


static void  R4300_SD( R4300_CALL_SIGNATURE )			// Store Doubleword
{
	R4300_CALL_MAKE_OP( op_code );

	u32 address = (u32)( gGPR[op_code.base]._s32_0 + (s32)(s16)op_code.immediate );
	R4300_CHECK_ALIGN( address, 7, true );
	if( Memory_StoreRaisesTLBException( address ) ) return;
	if( R4300_IsCartRomAddress( address ) )
	{
		DMA_PI_CartWrite( gGPR[op_code.rt]._u32_1 );	// Only the upper word reaches the PI
		return;
	}
	if( R4300_IsRCPWordAddress( address ) )
	{
		Write32Bits( address, gGPR[op_code.rt]._u32_1 );	// The RCP takes only the upper word (n64-systemtest spmem SD)
		return;
	}
	Write64Bits(address, gGPR[op_code.rt]._u64);
}


static void  R4300_Special_Unk( R4300_CALL_SIGNATURE ) { R4300_Exception_General( EXC_II, 0 ); }
static void  R4300_Special_SLL( R4300_CALL_SIGNATURE ) 		// Shift word Left Logical
{
	R4300_CALL_MAKE_OP( op_code );

	// NOP!
	if ( op_code._u32 == 0 ) return;


	gGPR[ op_code.rd ]._s64 = (s64)(s32)( (gGPR[ op_code.rt ]._u32_0 << op_code.sa) & 0xFFFFFFFF );
}

static void  R4300_Special_SRL( R4300_CALL_SIGNATURE ) 		// Shift word Right Logical
{
	R4300_CALL_MAKE_OP( op_code );
	gGPR[ op_code.rd ]._s64 = (s64)(s32)( gGPR[ op_code.rt ]._u32_0 >> op_code.sa );
}

static void  R4300_Special_SRA( R4300_CALL_SIGNATURE ) 		// Shift word Right Arithmetic
{
	R4300_CALL_MAKE_OP( op_code );
	// The shift happens on the whole 64-bit register, then the low word is sign-extended (mupen64plus SRA)
	gGPR[ op_code.rd ]._s64 = (s64)(s32)( gGPR[ op_code.rt ]._s64 >> op_code.sa );
}

static void  R4300_Special_SLLV( R4300_CALL_SIGNATURE ) 		// Shift word Left Logical Variable
{
	R4300_CALL_MAKE_OP( op_code );
	gGPR[ op_code.rd ]._s64 = (s64)(s32)( (gGPR[ op_code.rt ]._u32_0 << ( gGPR[ op_code.rs ]._u32_0 & 0x1F ) ) & 0xFFFFFFFF );
}

static void  R4300_Special_SRLV( R4300_CALL_SIGNATURE ) 		// Shift word Right Logical Variable
{
	R4300_CALL_MAKE_OP( op_code );
	gGPR[ op_code.rd ]._s64 = (s64)(s32)( gGPR[ op_code.rt ]._u32_0 >> ( gGPR[ op_code.rs ]._u32_0 & 0x1F ) );
}

static void  R4300_Special_SRAV( R4300_CALL_SIGNATURE ) 		// Shift word Right Arithmetic Variable
{
	R4300_CALL_MAKE_OP( op_code );
	// 64-bit shift, low word sign-extended (mupen64plus SRAV)
	gGPR[ op_code.rd ]._s64 = (s64)(s32)( gGPR[ op_code.rt ]._s64 >> ( gGPR[ op_code.rs ]._u32_0 & 0x1F ) );
}

static void  R4300_Special_JR( R4300_CALL_SIGNATURE ) 			// Jump Register
{
	R4300_CALL_MAKE_OP( op_code );

	u32	new_pc( gGPR[ op_code.rs ]._u32_0 );
	CPU_TakeBranch( new_pc );
}


static void  R4300_Special_JALR( R4300_CALL_SIGNATURE ) 		// Jump and Link register
{
	R4300_CALL_MAKE_OP( op_code );

	// Jump And Link
	u32	new_pc( gGPR[ op_code.rs ]._u32_0 );
	gGPR[ op_code.rd ]._s64 = (s64)(s32)(gCPUState.CurrentPC + 8);		// Store return address;
	CPU_TakeBranch( new_pc );
}

static void  R4300_Special_SYSCALL( R4300_CALL_SIGNATURE )
{
	R4300_Exception_Syscall();
}

static void  R4300_Special_BREAK( R4300_CALL_SIGNATURE ) 	// BREAK
{
	#ifdef DAEDALUS_PROFILER
	DPF( DEBUG_INTR, "BREAK Called. PC: 0x%08x. COUNT: 0x%08x", gCPUState.CurrentPC, gCPUState.CPUControl[C0_COUNT]._u32 );
#endif
	R4300_Exception_Break();
}

static void  R4300_Special_SYNC( R4300_CALL_SIGNATURE ) { /* Just ignore */}

static void  R4300_Special_MFHI( R4300_CALL_SIGNATURE ) 			// Move From MultHI
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._u64 = gCPUState.MultHi._u64;
}

static void  R4300_Special_MTHI( R4300_CALL_SIGNATURE ) 			// Move To MultHI
{
	R4300_CALL_MAKE_OP( op_code );

	gCPUState.MultHi._u64 = gGPR[ op_code.rs ]._u64;
}

static void  R4300_Special_MFLO( R4300_CALL_SIGNATURE ) 			// Move From MultLO
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._u64 = gCPUState.MultLo._u64;
}

static void  R4300_Special_MTLO( R4300_CALL_SIGNATURE ) 			// Move To MultLO
{
	R4300_CALL_MAKE_OP( op_code );

	gCPUState.MultLo._u64 = gGPR[ op_code.rs ]._u64;
}

// BEGIN MODIFIED BY Lkb - 8/jun/2001 - changed 0x1f to 0x3f because the value to be shifted is 64-bit long
static void  R4300_Special_DSLLV( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// Reserved Instruction exception
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._u64 << ( gGPR[ op_code.rs ]._u32_0 & 0x3F );
}

static void  R4300_Special_DSRLV( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// Reserved Instruction exception
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._u64 >> ( gGPR[ op_code.rs ]._u32_0 & 0x3F );
}

// Aeroguage uses!
static void  R4300_Special_DSRAV( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
		#ifdef DAEDALUS_DEBUG_CONSOLE

		#endif
	// Reserved Instruction exception
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._s64 >> ( gGPR[ op_code.rs ]._u32_0 & 0x3F );
}

static void  R4300_Special_MULT( R4300_CALL_SIGNATURE ) 			// MULTiply Signed
{
	R4300_CALL_MAKE_OP( op_code );

	s64 dwResult = (s64)gGPR[ op_code.rs ]._s32_0 * (s64)gGPR[ op_code.rt ]._s32_0;
	gCPUState.MultLo._u64 = (s64)(s32)(dwResult & 0xffffffff);
	gCPUState.MultHi._u64 = (s64)(s32)(dwResult >> 32);

}

static void  R4300_Special_MULTU( R4300_CALL_SIGNATURE ) 		// MULTiply Unsigned
{
	R4300_CALL_MAKE_OP( op_code );

	u64 dwResult = (u64)gGPR[ op_code.rs ]._u32_0 * (u64)gGPR[ op_code.rt ]._u32_0;
	gCPUState.MultLo._u64 = (s64)(s32)(dwResult & 0xffffffff);
	gCPUState.MultHi._u64 = (s64)(s32)(dwResult >> 32);
}

// Divide-by-zero and overflow results follow the VR4300 (mupen64plus 6c0732fc):
//   x / 0  -> LO = (x < 0) ? 1 : -1 (signed) or -1 (unsigned), HI = x
//   INT_MIN / -1 -> LO = INT_MIN, HI = 0
static void  R4300_Special_DIV( R4300_CALL_SIGNATURE ) 			//DIVide
{
	R4300_CALL_MAKE_OP( op_code );

	s32 nDividend = gGPR[ op_code.rs ]._s32_0;
	s32 nDivisor  = gGPR[ op_code.rt ]._s32_0;

	if (nDivisor == 0)
	{
		gCPUState.MultLo._s64 = nDividend < 0 ? 1 : -1;
		gCPUState.MultHi._s64 = (s64)nDividend;
	}
	else if (nDividend == (s32)0x80000000 && nDivisor == -1)
	{
		gCPUState.MultLo._s64 = (s64)nDividend;
		gCPUState.MultHi._s64 = 0;
	}
	else
	{
		gCPUState.MultLo._u64 = (s64)(s32)(nDividend / nDivisor);
		gCPUState.MultHi._u64 = (s64)(s32)(nDividend % nDivisor);
	}
}

static void  R4300_Special_DIVU( R4300_CALL_SIGNATURE ) 			// DIVide Unsigned
{
	R4300_CALL_MAKE_OP( op_code );

	u32 dwDividend = gGPR[ op_code.rs ]._u32_0;
	u32 dwDivisor  = gGPR[ op_code.rt ]._u32_0;

	if (dwDivisor == 0)
	{
		gCPUState.MultLo._s64 = -1;
		gCPUState.MultHi._s64 = (s64)(s32)dwDividend;
	}
	else
	{
		gCPUState.MultLo._u64 = (s64)(s32)(dwDividend / dwDivisor);
		gCPUState.MultHi._u64 = (s64)(s32)(dwDividend % dwDivisor);
	}
}

// 64x64 -> 128 bit unsigned multiply using 32 bit halves (no __int128 on the PSP toolchain).
static inline void R4300_Mul64x64( u64 a, u64 b, u64 & lo, u64 & hi )
{
	const u64 a_lo = (u32)a, a_hi = a >> 32;
	const u64 b_lo = (u32)b, b_hi = b >> 32;

	const u64 p0 = a_lo * b_lo;
	const u64 p1 = a_lo * b_hi;
	const u64 p2 = a_hi * b_lo;
	const u64 p3 = a_hi * b_hi;

	const u64 mid = (p0 >> 32) + (u32)p1 + (u32)p2;
	lo = (mid << 32) | (u32)p0;
	hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

static void R4300_DoDMULT( s64 a, s64 b )
{
	const bool negative = (a < 0) != (b < 0);
	const u64 ua = a < 0 ? (u64)0 - (u64)a : (u64)a;
	const u64 ub = b < 0 ? (u64)0 - (u64)b : (u64)b;

	u64 lo, hi;
	R4300_Mul64x64( ua, ub, lo, hi );
	if (negative)
	{
		lo = ~lo + 1;
		hi = ~hi + (lo == 0 ? 1 : 0);
	}
	gCPUState.MultLo._u64 = lo;
	gCPUState.MultHi._u64 = hi;
}

static void R4300_DoDDIV( s64 qwDividend, s64 qwDivisor )
{
	if (qwDivisor == 0)
	{
		gCPUState.MultLo._s64 = qwDividend < 0 ? 1 : -1;
		gCPUState.MultHi._s64 = qwDividend;
	}
	else if (qwDivisor == -1)
	{
		// Also covers INT64_MIN / -1 (LO = INT64_MIN, HI = 0) without overflowing.
		gCPUState.MultLo._u64 = (u64)0 - (u64)qwDividend;
		gCPUState.MultHi._s64 = 0;
	}
	else if ( (s64)(s32)qwDividend == qwDividend && (s64)(s32)qwDivisor == qwDivisor )
	{	// Fits in 32 bits: much faster than a 64-bit divide on the PSP //Corn
		const s32 d32 = (s32)qwDividend;
		const s32 v32 = (s32)qwDivisor;
		gCPUState.MultLo._s64 = (s64)(d32 / v32);
		gCPUState.MultHi._s64 = (s64)(d32 % v32);
	}
	else
	{
		gCPUState.MultLo._s64 = qwDividend / qwDivisor;
		gCPUState.MultHi._s64 = qwDividend % qwDivisor;
	}
}

static void R4300_DoDDIVU( u64 qwDividend, u64 qwDivisor )
{
	if (qwDivisor == 0)
	{
		gCPUState.MultLo._s64 = -1;
		gCPUState.MultHi._u64 = qwDividend;
	}
	else if( ((qwDividend | qwDivisor) >> 32) == 0 )
	{	// Fits in 32 bits //Corn
		const u32 d32 = (u32)qwDividend;
		const u32 v32 = (u32)qwDivisor;
		gCPUState.MultLo._u64 = d32 / v32;
		gCPUState.MultHi._u64 = d32 % v32;
	}
	else
	{
		gCPUState.MultLo._u64 = qwDividend / qwDivisor;
		gCPUState.MultHi._u64 = qwDividend % qwDivisor;
	}
}

// Called from the dynarec (DynaRecStubs.S) with 64-bit operands split into 32-bit halves.
extern "C" void Dynarec_DMULT_Exact( u32 a_lo, u32 a_hi, u32 b_lo, u32 b_hi )
{
	R4300_DoDMULT( (s64)(((u64)a_hi << 32) | a_lo), (s64)(((u64)b_hi << 32) | b_lo) );
}
extern "C" void Dynarec_DDIV_Exact( u32 a_lo, u32 a_hi, u32 b_lo, u32 b_hi )
{
	R4300_DoDDIV( (s64)(((u64)a_hi << 32) | a_lo), (s64)(((u64)b_hi << 32) | b_lo) );
}
extern "C" void Dynarec_DDIVU_Exact( u32 a_lo, u32 a_hi, u32 b_lo, u32 b_hi )
{
	R4300_DoDDIVU( ((u64)a_hi << 32) | a_lo, ((u64)b_hi << 32) | b_lo );
}

static void  R4300_Special_DMULT( R4300_CALL_SIGNATURE ) 		// Double Multiply
{
	R4300_CALL_MAKE_OP( op_code );
	// HI used to be left at 0 on the PSP (DAEDALUS_128BIT_MULT is desktop only).
	R4300_DoDMULT( gGPR[ op_code.rs ]._s64, gGPR[ op_code.rt ]._s64 );
}

static void  R4300_Special_DMULTU( R4300_CALL_SIGNATURE ) 			// Double Multiply Unsigned
{
	R4300_CALL_MAKE_OP( op_code );

	u64 lo, hi;
	R4300_Mul64x64( gGPR[ op_code.rs ]._u64, gGPR[ op_code.rt ]._u64, lo, hi );
	gCPUState.MultLo._u64 = lo;
	gCPUState.MultHi._u64 = hi;
}

static void  R4300_Special_DDIV( R4300_CALL_SIGNATURE ) 				// Double Divide
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_DoDDIV( gGPR[ op_code.rs ]._s64, gGPR[ op_code.rt ]._s64 );
}

static void  R4300_Special_DDIVU( R4300_CALL_SIGNATURE ) 			// Double Divide Unsigned
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_DoDDIVU( gGPR[ op_code.rs ]._u64, gGPR[ op_code.rt ]._u64 );
}

static void  R4300_Special_ADD( R4300_CALL_SIGNATURE ) 			// ADD signed - may throw exception
{
	R4300_CALL_MAKE_OP( op_code );

	s32 result;
	if( __builtin_add_overflow( gGPR[ op_code.rs ]._s32_0, gGPR[ op_code.rt ]._s32_0, &result ) )
	{
		R4300_Exception_General( EXC_OV, 0 );	// Integer overflow: rd is not written
		return;
	}
	gGPR[ op_code.rd ]._s64 = (s64)result;
}

static void  R4300_Special_ADDU( R4300_CALL_SIGNATURE ) 			// ADD Unsigned - doesn't throw exception
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._s64 = (s64)(s32)( gGPR[ op_code.rs ]._s32_0 + gGPR[ op_code.rt ]._s32_0 );
}

static void  R4300_Special_SUB( R4300_CALL_SIGNATURE ) 			// SUB Signed - may throw exception
{
	R4300_CALL_MAKE_OP( op_code );

	s32 result;
	if( __builtin_sub_overflow( gGPR[ op_code.rs ]._s32_0, gGPR[ op_code.rt ]._s32_0, &result ) )
	{
		R4300_Exception_General( EXC_OV, 0 );	// Integer overflow: rd is not written
		return;
	}
	gGPR[ op_code.rd ]._s64 = (s64)result;
}


static void  R4300_Special_SUBU( R4300_CALL_SIGNATURE ) 			// SUB Unsigned - doesn't throw exception
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._s64 = (s64)(s32)( gGPR[ op_code.rs ]._s32_0 - gGPR[ op_code.rt ]._s32_0 );
}

static void  R4300_Special_AND( R4300_CALL_SIGNATURE ) 				// logical AND
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rs ]._u64 & gGPR[ op_code.rt ]._u64;
}

static void  R4300_Special_OR( R4300_CALL_SIGNATURE ) 				// logical OR
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rs ]._u64 | gGPR[ op_code.rt ]._u64;
}

static void  R4300_Special_XOR( R4300_CALL_SIGNATURE ) 				// logical XOR
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rs ]._u64 ^ gGPR[ op_code.rt ]._u64;
}

static void  R4300_Special_NOR( R4300_CALL_SIGNATURE ) 				// logical Not OR
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._u64 = ~( gGPR[ op_code.rs ]._u64 | gGPR[ op_code.rt ]._u64 );
}

static void  R4300_Special_SLT( R4300_CALL_SIGNATURE ) 				// Set on Less Than
{
	R4300_CALL_MAKE_OP( op_code );

	// Cast to s32s to ensure sign is taken into account
	if ( gGPR[ op_code.rs ]._s64 < gGPR[ op_code.rt ]._s64 )
	{
		gGPR[ op_code.rd ]._u64 = 1;
	}
	else
	{
		gGPR[ op_code.rd ]._u64 = 0;
	}
}

static void  R4300_Special_SLTU( R4300_CALL_SIGNATURE ) 				// Set on Less Than Unsigned
{
	R4300_CALL_MAKE_OP( op_code );

	// Treated as unsigned....
	if ( gGPR[ op_code.rs ]._u64 < gGPR[ op_code.rt ]._u64 )
	{
		gGPR[ op_code.rd ]._u64 = 1;
	}
	else
	{
		gGPR[ op_code.rd ]._u64 = 0;
	}

}

static void  R4300_Special_DADD( R4300_CALL_SIGNATURE )//CYRUS64
{
	R4300_CALL_MAKE_OP( op_code );

	s64 result;
	if( __builtin_add_overflow( gGPR[ op_code.rs ]._s64, gGPR[ op_code.rt ]._s64, &result ) )
	{
		R4300_Exception_General( EXC_OV, 0 );	// Integer overflow: rd is not written
		return;
	}
	gGPR[ op_code.rd ]._s64 = result;
}

static void  R4300_Special_DADDU( R4300_CALL_SIGNATURE )//CYRUS64
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rs ]._u64 + gGPR[ op_code.rt ]._u64;

	//BUG FIX for Excite Bike - Salvy
	// I don't know why Excite bike only works if the operand is 32bit.. this for sure is wrong as docs say.
	// Also this causes Conker to fail to display a cutscene in the final boss!
	//gGPR[ op_code.rd ]._s64 = (s64)( gGPR[ op_code.rt ]._s32_0 + gGPR[ op_code.rs ]._s32_0 );
}

static void  R4300_Special_DSUB( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	s64 result;
	if( __builtin_sub_overflow( gGPR[ op_code.rs ]._s64, gGPR[ op_code.rt ]._s64, &result ) )
	{
		R4300_Exception_General( EXC_OV, 0 );	// Integer overflow: rd is not written
		return;
	}
	gGPR[ op_code.rd ]._s64 = result;
}

static void  R4300_Special_DSUBU( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// The order of rs and rt was wrong! It should be rs - rt, not rt - rs!!
	// It caused several lock ups in games ex Animal Crossing, and Conker to crash in last boss
	// Also signed extended to be safe
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rs ]._u64 - gGPR[ op_code.rt ]._u64;
}

static void  R4300_Special_DSLL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// Reserved Instruction exception
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._u64 << op_code.sa;
}

static void  R4300_Special_DSRL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// Reserved Instruction exception
    gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._u64 >> op_code.sa;
}

static void  R4300_Special_DSRA( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// Reserved Instruction exception
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._s64 >> op_code.sa;
}

static void  R4300_Special_DSLL32( R4300_CALL_SIGNATURE ) 			// Double Shift Left Logical 32
{
	R4300_CALL_MAKE_OP( op_code );

	// Reserved Instruction exception
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._u64 << ( 32 + op_code.sa );
}

static void  R4300_Special_DSRL32( R4300_CALL_SIGNATURE ) 			// Double Shift Right Logical 32
{
	R4300_CALL_MAKE_OP( op_code );

	// Reserved Instruction exception
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._u64 >> ( 32 + op_code.sa );
}

static void  R4300_Special_DSRA32( R4300_CALL_SIGNATURE ) 			// Double Shift Right Arithmetic 32
{
	R4300_CALL_MAKE_OP( op_code );

	// Reserved Instruction exception
	gGPR[ op_code.rd ]._u64 = gGPR[ op_code.rt ]._s64 >> ( 32 + op_code.sa );
}


static void  R4300_RegImm_Unk( R4300_CALL_SIGNATURE ) {  R4300_Exception_General( EXC_II, 0 ); }

static void  R4300_RegImm_BLTZ( R4300_CALL_SIGNATURE ) 			// Branch on Less than Zero
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs < 0
	if ( gGPR[ op_code.rs ]._s64 < 0 )
	{
		s16 offset= (s16)op_code.immediate;
		u32 pc = gCPUState.CurrentPC;
		u32 new_pc = pc + ((s32)offset<<2) + 4;

		SpeedHack(pc, new_pc);
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_RegImm_BLTZL( R4300_CALL_SIGNATURE ) 			// Branch on Less than Zero Likely
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs < 0
	if ( gGPR[ op_code.rs ]._s64 < 0 )
	{
		//ToDo: SpeedHack?
		u32	new_pc = gCPUState.CurrentPC + ((s32)(s16)op_code.immediate<<2) + 4;
		CPU_TakeBranch( new_pc );
	}
	else
	{
		// Don't execute subsequent instruction
		INCREMENT_PC();
	}

}
static void  R4300_RegImm_BLTZAL( R4300_CALL_SIGNATURE ) 		// Branch on Less than Zero And Link
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs >= 0
	// Store the return address even if branch not taken

	// Store return address
	const s64 rs = gGPR[ op_code.rs ]._s64;		// Read before RA is written (rs may be RA)
	gGPR[REG_ra]._s64 = (s64)(s32)(gCPUState.CurrentPC + 8);		// Store return address

	if ( rs < 0 )
	{
		//ToDo: SpeedHack?
		u32	new_pc = gCPUState.CurrentPC + ((s32)(s16)op_code.immediate<<2) + 4;
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_RegImm_BGEZ( R4300_CALL_SIGNATURE ) 			// Branch on Greater than or Equal to Zero
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs >= 0
	if ( gGPR[ op_code.rs ]._s64 >= 0 )
	{
		s16 offset = (s16)op_code.immediate;
		u32 pc = gCPUState.CurrentPC ;
		u32 new_pc = pc + ((s32)offset<<2) + 4;

		SpeedHack(pc, new_pc);
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_RegImm_BGEZL( R4300_CALL_SIGNATURE ) 			// Branch on Greater than or Equal to Zero Likely
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs >= 0
	if ( gGPR[ op_code.rs ]._s64 >= 0 )
	{
		//ToDO: SpeedHack?
		u32	new_pc = gCPUState.CurrentPC + ((s32)(s16)op_code.immediate<<2) + 4;
		CPU_TakeBranch( new_pc );
	}
	else
	{
		// Don't execute subsequent instruction
		INCREMENT_PC();
	}
}

static void  R4300_RegImm_BGEZAL( R4300_CALL_SIGNATURE ) 		// Branch on Greater than or Equal to Zero And Link
{
	R4300_CALL_MAKE_OP( op_code );

	//branch if rs >= 0
	// This always happens, even if branch not taken

	const s64 rs = gGPR[ op_code.rs ]._s64;		// Read before RA is written (rs may be RA)
	gGPR[REG_ra]._s64 = (s64)(s32)(gCPUState.CurrentPC + 8);		// Store return address

	if ( rs >= 0 )
	{
		//ToDo: SpeedHack?
		u32	new_pc( gCPUState.CurrentPC + ((s32)(s16)op_code.immediate<<2) + 4 );
		CPU_TakeBranch( new_pc );
	}
}


static void  R4300_Cop0_Unk( R4300_CALL_SIGNATURE ) { R4300_Exception_General( EXC_II, 0 ); }
// Undefined COP0 functions (CO = 1) do nothing on hardware: n64-systemtest's emulator-extension
// probes (COP0 CO funct 0x20-0x3F) rely on that to run on a real console.
static void  R4300_TLB_Unk( R4300_CALL_SIGNATURE )  { WARN_NOEXIST("R4300_TLB_Unk"); }

// Random register: decrements every cycle between Wired and 31 on hardware.
// Like mupen64plus, derive it from Count so TLBWR spreads writes over the non-wired entries.
static u32 R4300_GetRandom()
{
	u32 wired = gCPUState.CPUControl[C0_WIRED]._u32 & 0x3F;
	u32 count = gCPUState.CPUControl[C0_COUNT]._u32 / COUNTER_INCREMENT_PER_OP;	// Random steps once per instruction (mupen64plus: Count/count_per_op)
	if( wired >= 32 )
		return count % 64;		// Wired > 31: Random covers 0..63 (mupen64plus MFC0)
	return (count % (32 - wired)) + wired;
}

// COP0 register read (mupen64plus MFC0/DMFC0, mips_instructions.def). Registers that are 64 bits
// wide on the VR4300 come from gCP0Wide; the others are 32 bits and DMFC0 zero-extends them.
// Unused registers (7, 21-25, 31) return the last value written to any COP0 register.
static u64 R4300_ReadCop0( u32 reg )
{
#ifdef DAEDALUS_ENABLE_ASSERTS
	if ( reg == C0_CAUSE )
	{
		bool	mi_interrupt_set = (Memory_MI_GetRegister(MI_INTR_MASK_REG) & Memory_MI_GetRegister(MI_INTR_REG)) != 0;
		bool	cause_int_3_set( (gCPUState.CPUControl[C0_CAUSE]._u32 & CAUSE_IP3) != 0);

		DAEDALUS_ASSERT( mi_interrupt_set == cause_int_3_set, "CAUSE_IP3 inconsistant with MI_INTR_REG" );
	}
#endif

	switch( reg )
	{
	case C0_RAND:
		gCPUState.CPUControl[C0_RAND]._u32 = R4300_GetRandom();
		return gCPUState.CPUControl[C0_RAND]._u32;

	case C0_CONTEXT:
	case C0_BADVADDR:
	case C0_ENTRYHI:
	case C0_EPC:
	case C0_XCONTEXT:
	case C0_ERROR_EPC:
		return R4300_GetCop0Wide( reg );

	case 7: case 21: case 22: case 23: case 24: case 25: case 31:
		return gCP0Latch;

	default:
		// No specific handling needs for reads to these registers.
		return gCPUState.CPUControl[ reg ]._u32;
	}
}

static void  R4300_Cop0_MFC0( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	gGPR[ op_code.rt ]._s64 = (s64)(s32)(u32)R4300_ReadCop0( op_code.fs );
}

static void  R4300_Cop0_DMFC0( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	gGPR[ op_code.rt ]._u64 = R4300_ReadCop0( op_code.fs );
}

// Move Word To CopReg
static const u32 kCauseSW [[maybe_unused]] = CAUSE_SW1|CAUSE_SW2;

// COP0 register write (mupen64plus MTC0, mips_instructions.def). MTC0 passes the sign-extended
// 32-bit value, DMTC0 the full 64 bits; only the 64-bit registers keep the upper word.
static void  R4300_WriteCop0( u32 reg, u64 value )
{
	// Every write is latched and read back from the unused registers (mupen64plus cp0_latch)
	gCP0Latch = value;

	u32 new_value = (u32)value;

	switch ( reg )
	{
		case C0_INX:
			gCPUState.CPUControl[C0_INX]._u32 = new_value & 0x8000003F;
			//DBGConsole_Msg(0, "Setting EntryL0 register to 0x%08x", new_value);
			break;

		case C0_ENTRYLO0:
		case C0_ENTRYLO1:
			gCPUState.CPUControl[reg]._u32 = new_value & 0x3FFFFFFF;
			//DBGConsole_Msg(0, "Setting EntryL0 register to 0x%08x", new_value);
			break;

		case C0_PAGEMASK:
			gCPUState.CPUControl[C0_PAGEMASK]._u32 = new_value & 0x01FFE000;
			//DBGConsole_Msg(0, "Setting PageMask register to 0x%08x", new_value);
			break;

		case C0_WIRED:
			// Set to top limit on write to wired
			gCPUState.CPUControl[C0_RAND]._u32 = 31;
			#ifdef DAEDALUS_DEBUG_CONSOLE
			DBGConsole_Msg(0, "Setting Wired register to 0x%08x", new_value);
			#endif
			gCPUState.CPUControl[C0_WIRED]._u32 = new_value & 0x3F;
			break;

		case C0_ENTRYHI:
			// R (63:62), VPN2 (39:13) and ASID are writable; bits 8-12 read as zero.
			R4300_SetCop0Wide( C0_ENTRYHI, value & 0xC00000FFFFFFE0FFULL );
			break;

		case C0_CONTEXT:
			// Only PTEBase (63:23) is software writable, BadVPN2 is set by TLB exceptions.
			R4300_SetCop0Wide( C0_CONTEXT, (value & 0xFFFFFFFFFF800000ULL) | (R4300_GetCop0Wide( C0_CONTEXT ) & 0x007FFFF0) );
			break;

		case C0_XCONTEXT:
			// Only PTEBase (63:33) is software writable, R/BadVPN2 are set by TLB exceptions.
			R4300_SetCop0Wide( C0_XCONTEXT, (value & 0xFFFFFFFE00000000ULL) | (R4300_GetCop0Wide( C0_XCONTEXT ) & 0x1FFFFFFFFULL) );
			break;

		case C0_EPC:
		case C0_ERROR_EPC:
			R4300_SetCop0Wide( reg, value );
			break;

		case C0_CONFIG:
			// Only EP (27:24), BE (15) and K0 (3:0) are writable; the rest reads as 0x70066460.
			gCPUState.CPUControl[C0_CONFIG]._u32 = (new_value & 0x0F00800F) | 0x70066460;
			break;

		case C0_ECC:	// ParityError: 8 bits (mupen64plus)
			gCPUState.CPUControl[C0_ECC]._u32 = new_value & 0xFF;
			break;

		case C0_TAGLO:	// mupen64plus
			gCPUState.CPUControl[C0_TAGLO]._u32 = new_value & 0x0FFFFFC0;
			break;

		case C0_TAGHI:	// Always reads 0 (mupen64plus)
			gCPUState.CPUControl[C0_TAGHI]._u32 = 0;
			break;

		case 7: case 21: case 22: case 23: case 24: case 25: case 31:
			// Unused: only the latch above changes
			break;

		case C0_RAND:
		case C0_BADVADDR:
		case C0_PRID:
		case C0_CACHE_ERR:			// Furthermore, this reg must return 0 on reads.
			// All these registers are read only - make sure that software doesn't write to them
			#ifdef DAEDALUS_DEBUG_CONSOLE
			DBGConsole_Msg(0, "MTC0. Software attempted to write to read only reg %s: 0x%08x", Cop0RegNames[ reg ], new_value);
			#endif
			break;

		case C0_CAUSE:
			// Only IP1:0 (Interrupt Pending) bits are software writeable.
			// On writes, set all others to 0. Is this correct?
			//  Other bits are CE (copro error) BD (branch delay), the other
			// Interrupt pendings and EscCode.
				#ifdef DAEDALUS_ENABLE_ASSERTS
			DAEDALUS_ASSERT(new_value == 0, "CAUSE register invalid writing");
			#endif
			#ifdef DAEDALUS_DEBUG_CONSOLE
				if ( (new_value&~(CAUSE_SW1|CAUSE_SW2)) != (gCPUState.CPUControl[C0_CAUSE]._u32&~(CAUSE_SW1|CAUSE_SW2))  )
				{
					DBGConsole_Msg( 0, "[MWas previously clobbering CAUSE REGISTER" );
				}
				#endif
				#ifdef DAEDALUS_PROFILER
			DPF( DEBUG_REGS, "CAUSE set to 0x%08x", new_value );
			#endif

			gCPUState.CPUControl[C0_CAUSE]._u32 &=             ~(CAUSE_SW1|CAUSE_SW2);
			gCPUState.CPUControl[C0_CAUSE]._u32 |= (new_value & (CAUSE_SW1|CAUSE_SW2));
			// Software interrupts (IP0/IP1) can be raised this way.
			R4300_CheckPendingInterrupts();
			break;
		case C0_SR:
			// Software can enable/disable interrupts here. We check if Interrupt Enable is
			//  set, and if there are any pending interrupts. If there are, then we set the
			//  CHECK_POSTPONED_INTERRUPTS flag to make sure we check for interrupts that have
			//  occurred since we disabled interrupts
			R4300_SetSR(new_value & ~0x00080000);	// Bit 19 isn't writable (mupen64plus)
			break;

		case C0_COUNT:
			{
				// See comments below for COMPARE.
				// When this register is set, we need to check whether the next timed interrupt will
				//  be due to vertical blank or COMPARE
				#ifdef DAEDALUS_DEBUG_CONSOLE
				DBGConsole_Msg(0, "Count set - setting int");
				#endif
				// Re-base the pending compare event (mupen64plus translate_event_queue).
				CPU_SetCount(new_value);
				break;
			}
		case C0_COMPARE:
			{
				// When the value of COUNT equals the value of COMPARE, IP7 of the CAUSE register is set
				// (Timer interrupt). Writing to this register clears the timer interrupt pending flag.
				CPU_SetCompare(new_value);
			}
			break;

		// Need to check CONFIG register writes - not all fields are writable.
		// This also sets Endianness mode.

		// WatchHi/WatchLo are used to create a Watch Trap. This may not need implementing, but we should
		// Probably provide a warning on writes, just so that we know
		//case C0_WATCHLO:
		//	DBGConsole_Msg( 0, "[MWROTE TO WATCHLO REGISTER!" );
		//	gCPUState.CPUControl[ C0_WATCHLO ]._u32 = new_value& 0x0FFFFFC0;
		//	break;
		//case C0_WATCHHI:
		//	DBGConsole_Msg( 0, "[MWROTE TO WATCHHI REGISTER!" );
		//	gCPUState.CPUControl[ C0_WATCHHI ]._u32 = 0;
		//	break;

		default:
			// No specific handling needs for writes to these registers.
			gCPUState.CPUControl[ reg ]._u32 = new_value;
			break;
	}
}

static void  R4300_Cop0_MTC0( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_WriteCop0( op_code.fs, (u64)(s64)gGPR[ op_code.rt ]._s32_0 );
}

static void  R4300_Cop0_DMTC0( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_WriteCop0( op_code.fs, gGPR[ op_code.rt ]._u64 );
}

// Full 64-bit EntryHi of each TLB entry, for TLBR
static u64 gTLBEntryHiWide[32];

// TLBWI/TLBWR. As measured by n64-systemtest: PageMask bits work in pairs and only the upper bit
// of each pair counts, and the PFN fields of EntryLo are 20 bits wide.
static void R4300_TLBWrite( u32 i )
{
	const u32 pagemask_in = gCPUState.CPUControl[C0_PAGEMASK]._u32;
	u32 pagemask = 0;
	for( u32 bit = 14; bit <= 24; bit += 2 )
	{
		if( pagemask_in & (1u << bit) )
			pagemask |= 3u << (bit - 1);
	}

	const u64 hi = R4300_GetCop0Wide( C0_ENTRYHI );
	gTLBEntryHiWide[i] = hi;

	g_TLBs[i].UpdateValue(pagemask,
						(u32)hi,
						gCPUState.CPUControl[C0_ENTRYLO1]._u32 & 0x03FFFFFF,
						gCPUState.CPUControl[C0_ENTRYLO0]._u32 & 0x03FFFFFF);
}

static void  R4300_TLB_TLBR( R4300_CALL_SIGNATURE ) 				// TLB Read
{
	u32 index = gCPUState.CPUControl[C0_INX]._u32 & 0x3F;
	if( index >= 32 )
		return;		// Out of range index: no effect (mupen64plus)

	gCPUState.CPUControl[C0_PAGEMASK]._u32 = g_TLBs[index].pagemask;
	// EntryHi is 64 bits (R, VPN2 39:13); the TLB lookup only uses the low word
	u64 hi = gTLBEntryHiWide[index];
	if( (u32)hi != g_TLBs[index].hi )
		hi = g_TLBs[index].hi;
	R4300_SetCop0Wide( C0_ENTRYHI, hi & ~(u64)g_TLBs[index].pagemask & 0xC00000FFFFFFE0FFULL );
	gCPUState.CPUControl[C0_ENTRYLO0]._u32 = (g_TLBs[index].pfne & ~TLBLO_G) | g_TLBs[index].g;
	gCPUState.CPUControl[C0_ENTRYLO1]._u32 = (g_TLBs[index].pfno & ~TLBLO_G) | g_TLBs[index].g;

#ifdef DAEDALUS_PROFILER
	DPF( DEBUG_TLB, "TLBR: INDEX: 0x%04x. PAGEMASK: 0x%08x.", index, gCPUState.CPUControl[C0_PAGEMASK]._u32 );
	DPF( DEBUG_TLB, "      ENTRYHI: 0x%08x. ENTRYLO1: 0x%08x. ENTRYLO0: 0x%08x", gCPUState.CPUControl[C0_ENTRYHI]._u32, gCPUState.CPUControl[C0_ENTRYLO1]._u32, gCPUState.CPUControl[C0_ENTRYLO0]._u32 );
#endif
}


static void  R4300_TLB_TLBWI( R4300_CALL_SIGNATURE )			// TLB Write Index
{


	u32 i = gCPUState.CPUControl[C0_INX]._u32 & 0x3F;
	if( i >= 32 )
		return;		// Out of range index: no effect (mupen64plus)

 	#ifdef DAEDALUS_PROFILER
	DPF( DEBUG_TLB, "TLBWI: INDEX: 0x%04x. ", i );
	#endif
	R4300_TLBWrite( i );
}

static void  R4300_TLB_TLBWR( R4300_CALL_SIGNATURE )
{
	// Select an index between Wired and 31 from the Random register.
	// (Previously a static computed once, so every TLBWR hit the same entry,
	//  and it could index past the end of g_TLBs when Wired > 0.)
	u32 i = R4300_GetRandom();
	gCPUState.CPUControl[C0_RAND]._u32 = i;
	i &= 31;		// Only 32 entries

		#ifdef DAEDALUS_PROFILER
			DPF( DEBUG_TLB, "TLBWR: INDEX: 0x%04x. ", i );
		#endif
	R4300_TLBWrite( i );
}


static void  R4300_TLB_TLBP( R4300_CALL_SIGNATURE ) 				// TLB Probe
{
	u32 entryH = gCPUState.CPUControl[C0_ENTRYHI]._u32;
	const u64 entryH64 = R4300_GetCop0Wide( C0_ENTRYHI );

#ifdef DAEDALUS_PROFILER
	DPF( DEBUG_TLB, "TLBP: ENTRYHI: 0x%08x", entryH );
	#endif

    for( u32 i = 0; i < 32; i++ )
	{
		// R (63:62) and VPN2 (39:13) must match, as written with TLBWI/TLBWR
		const u64 vpn_mask = 0xC00000FFFFFFE000ULL & ~(u64)g_TLBs[i].pagemask;
		u64 tlb_hi = gTLBEntryHiWide[i];
		if( (u32)tlb_hi != g_TLBs[i].hi )
			tlb_hi = g_TLBs[i].hi;
		if( ((tlb_hi & vpn_mask) == (entryH64 & vpn_mask)) && ( (g_TLBs[i].g)
			|| ((g_TLBs[i].hi & TLBHI_PIDMASK) ==  (entryH    & TLBHI_PIDMASK))) )
		{
			#ifdef DAEDALUS_PROFILER
			DPF( DEBUG_TLB, "   Found matching TLB Entry - 0x%04x", i );
			#endif
			gCPUState.CPUControl[C0_INX]._u32 = i;
			return;
		}
    }

	gCPUState.CPUControl[C0_INX]._u32 = TLBINX_PROBE;
		#ifdef DAEDALUS_PROFILER
	DPF( DEBUG_TLB, "   No matching TLB Entry Found for 0x%08x", entryH );
	#endif
}

static void  R4300_TLB_ERET( R4300_CALL_SIGNATURE )
{

	if( gCPUState.CPUControl[C0_SR]._u32 & SR_ERL )
	{
		// Returning from an error trap
			#ifdef DAEDALUS_PROFILER
		DPF(DEBUG_INTR, "ERET: Returning from error trap");
		#endif
		CPU_SetPC( gCPUState.CPUControl[C0_ERROR_EPC]._u32 );
		gCPUState.CPUControl[C0_SR]._u32 &= ~SR_ERL;
	}
	else
	{
			#ifdef DAEDALUS_PROFILER
		DPF(DEBUG_INTR, "ERET: Returning from interrupt/exception");
		#endif
		// Returning from an exception
		CPU_SetPC( gCPUState.CPUControl[C0_EPC]._u32 );
		gCPUState.CPUControl[C0_SR]._u32 &= ~SR_EXL;
	}
	// Point to previous instruction (as we increment the pointer immediately afterwards
	DECREMENT_PC();

	// Ensure we don't execute this in the delay slot
	gCPUState.Delay = NO_DELAY;

	// ERET clears LLbit, and any interrupt raised while EXL was set must now be taken.
	gCPUState.LLBit = 0;
	R4300_CheckPendingInterrupts();
}


static void  R4300_Cop1_Unk( R4300_CALL_SIGNATURE )     { R4300_Cop1_Unimplemented(); }

static void  R4300_Cop1_MTC1( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// Manual says top bits undefined after load
	StoreFPR_Word( op_code.fs,  gGPR[ op_code.rt ]._s32_0 );
}

static void  R4300_Cop1_DMTC1( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// Manual says top bits undefined after load
	StoreFPR_Long( op_code.fs, gGPR[ op_code.rt ]._u64 );
}


static void  R4300_Cop1_MFC1( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// MFC1 in the manual says this is a sign-extended result
	gGPR[ op_code.rt ]._s64 = (s64)(s32)LoadFPR_Word( op_code.fs );

}

static void  R4300_Cop1_DMFC1( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	gGPR[ op_code.rt ]._s64 = LoadFPR_Long( op_code.fs );
}


// FCSR (FCR31): only FS, C, the cause/enable/flag fields and RM exist (n64-systemtest CFC1/CTC1 FCSR)
static const u32 FPCSR_WRITE_MASK = 0x0183FFFF;

// Writing FCSR with a cause bit whose exception is enabled (Unimplemented Operation is always
// enabled) raises the Floating-Point exception right away; FCSR keeps the written value
// (n64-systemtest 'Fire exception through CTC1').
static void R4300_CheckFPCSRCause( u32 fcsr )
{
	const u32 cause   = (fcsr >> 12) & 0x3F;
	const u32 enables = ((fcsr >> 7) & 0x1F) | 0x20;
	if( cause & enables )
		R4300_Exception_General( EXC_FPE, 0 );
}

static void  R4300_Cop1_CFC1( R4300_CALL_SIGNATURE ) 		// move Control word From Copro 1
{
	R4300_CALL_MAKE_OP( op_code );

	// Only defined for reg 0 or 31
	if ( op_code.fs == 0 || op_code.fs == 31 )
	//Saves a compare //Corn
	//if( !((op_code.fs + 1) & 0x1E) )
	{
		gGPR[ op_code.rt ]._s64 = (s64)gCPUState.FPUControl[ op_code.fs ]._s32;
		//gGPR[ op_code.rt ]._s32_0 = gCPUState.FPUControl[ op_code.fs ]._s32;  //copy only low part
	}
}

static void  R4300_Cop1_CTC1( R4300_CALL_SIGNATURE ) 		// move Control word To Copro 1
{
	R4300_CALL_MAKE_OP( op_code );

	#ifdef DAEDALUS_ENABLE_ASSERTS
	DAEDALUS_ASSERT( op_code.fs != 0, "CTC1 : Reg zero unhandled");
	#endif
	// Only defined for reg 0 or 31
	// TODO - Maybe an exception was raised?
	// Not needed for 0?
	/*if ( op_code.fs == 0 )
	{
		gCPUState.FPUControl[ op_code.fs ]._u64 = gGPR[ op_code.rt ]._u64;

	}*/
	if ( op_code.fs == 31 )
	{
		gCPUState.FPUControl[ 31 ]._u32 = gGPR[ op_code.rt ]._u32_0 & FPCSR_WRITE_MASK;

		u32		fpcr( gCPUState.FPUControl[ 31 ]._u32 );
		gRoundingMode = (ERoundingMode)( fpcr & FPCSR_RM_MASK );
		SET_ROUND_MODE(gRoundingMode);
		R4300_CheckFPCSRCause( fpcr );
	}
	#ifdef DAEDALUS_DEBUG_CONSOLE
	else
	{
		// Now generate lots of exceptions :-)
	}
	#endif
}

// Hack for the PSP, set rounding mode here, see notes in SET_ROUND_MODE
// Fixes collision issues in the final boss of DK64 and camera icon not rotating, fixes collision issues in Rayman, and JFG too
#ifdef DAEDALUS_PSP
static void  R4300_Cop1_CTC1_2( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

#ifdef DAEDALUS_ENABLE_ASSERTS
	DAEDALUS_ASSERT( op_code.fs != 0, "CTC1 : Reg zero unhandled");
	#endif
	// Only defined for reg 0 or 31
	// TODO - Maybe an exception was raised?
	// Not needed for 0?
	/*if ( op_code.fs == 0 )
	{
		gCPUState.FPUControl[ op_code.fs ]._u64 = gGPR[ op_code.rt ]._u64;
	}*/
	if ( op_code.fs == 31 )
	{
		gCPUState.FPUControl[ 31 ]._u32 = gGPR[ op_code.rt ]._u32_0 & FPCSR_WRITE_MASK;

		u32		fpcr( gCPUState.FPUControl[ 31 ]._u32 );
		gRoundingMode = (ERoundingMode)( fpcr & FPCSR_RM_MASK );
		pspFpuSetRoundmode( gNativeRoundingModes[ gRoundingMode ] );
		R4300_CheckFPCSRCause( fpcr );
	}
	#ifdef DAEDALUS_DEBUG_CONSOLE
	else
	{
		// Now generate lots of exceptions :-)
	}
	#endif
}
#endif

static void  R4300_BC1_BC1F( R4300_CALL_SIGNATURE )		// Branch on FPU False
{
	R4300_CALL_MAKE_OP( op_code );

	if ( !(gCPUState.FPUControl[31]._u32 & FPCSR_C) )
	{
		u32	new_pc = gCPUState.CurrentPC + (s32)(s16)op_code.immediate * 4 + 4;
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_BC1_BC1T( R4300_CALL_SIGNATURE )	// Branch on FPU True
{
	R4300_CALL_MAKE_OP( op_code );

	if ( gCPUState.FPUControl[31]._u32 & FPCSR_C )
	{
		u32	new_pc = gCPUState.CurrentPC + (s32)(s16)op_code.immediate * 4 + 4;
		CPU_TakeBranch( new_pc );
	}
}

static void  R4300_BC1_BC1FL( R4300_CALL_SIGNATURE )	// Branch on FPU False Likely
{
	R4300_CALL_MAKE_OP( op_code );

	if ( !(gCPUState.FPUControl[31]._u32 & FPCSR_C) )
	{
		u32	new_pc = gCPUState.CurrentPC + (s32)(s16)op_code.immediate*4 + 4;
		CPU_TakeBranch( new_pc );
	}
	else
	{
		// Don't execute subsequent instruction
		INCREMENT_PC();
	}
}

static void  R4300_BC1_BC1TL( R4300_CALL_SIGNATURE )		// Branch on FPU True Likely
{
	R4300_CALL_MAKE_OP( op_code );

	if ( gCPUState.FPUControl[31]._u32 & FPCSR_C )
	{
		u32	new_pc = gCPUState.CurrentPC + (s32)(s16)op_code.immediate * 4 + 4;
		CPU_TakeBranch( new_pc );
	}
	else
	{
		// Don't execute subsequent instruction
		INCREMENT_PC();
	}
}

// Raw IEEE bits of FPRs as doubles (a single is widened exactly; with SIM_DOUBLES a simulated double is
// widened from its float)
static u64 R4300_F32BitsToF64Bits( u32 f )
{
	const u64 sign = (u64)(f >> 31) << 63;
	const u32 exp  = (f >> 23) & 0xFF;
	u32 mant = f & 0x7FFFFF;
	if( exp == 0xFF )
		return sign | (0x7FFULL << 52) | ((u64)mant << 29);
	if( exp == 0 )
	{
		if( mant == 0 )
			return sign;
		s32 e = 1 - 127;
		while( (mant & 0x800000) == 0 ) { mant <<= 1; e--; }
		return sign | ((u64)(e + 1023) << 52) | ((u64)(mant & 0x7FFFFF) << 29);
	}
	return sign | ((u64)(exp - 127 + 1023) << 52) | ((u64)mant << 29);
}

static inline u64 R4300_RawFPR_S( u32 reg )
{
	return R4300_F32BitsToF64Bits( gCPUState.FPU[reg]._u32 );
}

static inline u64 R4300_RawFPR_D( u32 reg )
{
#ifdef SIM_DOUBLES
	if( gCPUState.FPU[reg+0]._u32 == SIMULATESIG )
		return R4300_F32BitsToF64Bits( FPR_HI( reg ) );
#endif
	return ((u64)FPR_HI( reg ) << 32) | gCPUState.FPU[reg+0]._u32;
}

//*****************************************************************************
// VR4300 single-precision arithmetic for the interpreter
//
// ADD/SUB/MUL/DIV/SQRT/ABS/NEG.S are computed exactly in integers and rounded with the FCSR
// rounding mode, so results and FCSR cause/flag bits match the VR4300 (n64-systemtest cop1;
// mupen64plus fpu.h gets the flags from the host FPU instead). VR4300 specifics:
// - a subnormal input, or an IEEE signalling NaN input (top mantissa bit clear), raises the
//   Unimplemented Operation exception (it can't be masked);
// - an IEEE quiet NaN input (top mantissa bit set; a signalling NaN in the legacy MIPS encoding the
//   VR4300 uses) is an Invalid Operation; invalid operations return the NaN 0x7FBFFFFF;
// - a result in the subnormal range raises Unimplemented Operation when FS is clear or when
//   Underflow or Inexact are enabled; otherwise it is flushed to zero or to the smallest normal
//   number, depending on the rounding mode, with Underflow and Inexact set;
// - an enabled cause raises the Floating-Point exception without writing the result or the flags.
// The dynarec generates these instructions natively and is unchanged.
//*****************************************************************************
enum
{
	FPU_CAUSE_I = 0x01,		// Same bit order as FCSR cause (bits 12-17), enable (7-11) and flag (2-6)
	FPU_CAUSE_U = 0x02,
	FPU_CAUSE_O = 0x04,
	FPU_CAUSE_Z = 0x08,
	FPU_CAUSE_V = 0x10,
	FPU_CAUSE_E = 0x20,
};

static const u32 kF32DefaultNaN = 0x7FBFFFFF;

// Writes the new cause bits; an enabled one (Unimplemented always is) raises the exception.
// Returns true if the instruction completes and its result should be written.
static bool R4300_FPUSetCause( u32 causes )
{
	u32 fcsr = (gCPUState.FPUControl[31]._u32 & ~0x0003F000) | (causes << 12);
	const u32 enables = ((fcsr >> 7) & 0x1F) | FPU_CAUSE_E;
	if( causes & enables )
	{
		gCPUState.FPUControl[31]._u32 = fcsr;
		R4300_Exception_General( EXC_FPE, 0 );
		return false;
	}
	gCPUState.FPUControl[31]._u32 = fcsr | ((causes & 0x1F) << 2);
	return true;
}

enum EF32Class { F32_ZERO, F32_NORMAL, F32_INF, F32_QNAN, F32_UNIMPLEMENTED };

static inline EF32Class R4300_F32Class( u32 f )
{
	const u32 exp  = (f >> 23) & 0xFF;
	const u32 mant = f & 0x7FFFFF;
	if( exp == 0 )		return mant == 0 ? F32_ZERO : F32_UNIMPLEMENTED;			// Subnormal
	if( exp == 0xFF )	return mant == 0 ? F32_INF : ((mant & 0x400000) ? F32_QNAN : F32_UNIMPLEMENTED);
	return F32_NORMAL;
}

// Rounds (-1)^sign * m * 2^e (m != 0, may carry a sticky bit in its LSB) to binary32
static u32 R4300_F32RoundPack( u32 sign, s32 e, u64 m, u32 & causes, bool & tiny )
{
	const u32 rm = gCPUState.FPUControl[31]._u32 & 3;
	const s32 lz = __builtin_clzll( m );
	m <<= lz;
	e -= lz;
	s32 exp = e + 63 + 127;			// Biased exponent of the value 1.xxx * 2^(e+63)
	if( exp <= 0 )
	{
		tiny = true;
		return sign << 31;
	}

	u32 mant = (u32)(m >> 40);
	const u64 rem = m & 0xFFFFFFFFFFULL;
	bool inc = false;
	switch( rm )
	{
	case 0:	inc = rem > 0x8000000000ULL || (rem == 0x8000000000ULL && (mant & 1)); break;
	case 1:	break;
	case 2:	inc = rem != 0 && !sign; break;
	case 3:	inc = rem != 0 && sign; break;
	}
	if( rem != 0 )
		causes |= FPU_CAUSE_I;
	if( inc && ++mant == 0x1000000 )
	{
		mant >>= 1;
		exp++;
	}
	if( exp >= 0xFF )
	{
		causes |= FPU_CAUSE_O | FPU_CAUSE_I;
		const bool to_inf = rm == 0 || (rm == 2 && !sign) || (rm == 3 && sign);
		return (sign << 31) | (to_inf ? 0x7F800000 : 0x7F7FFFFF);
	}
	return (sign << 31) | ((u32)exp << 23) | (mant & 0x7FFFFF);
}

static inline u32 R4300_F32Mant( u32 f )	{ return (f & 0x7FFFFF) | 0x800000; }
static inline s32 R4300_F32Exp( u32 f )		{ return (s32)((f >> 23) & 0xFF) - 150; }	// f = mant * 2^exp

static u64 R4300_ISqrt64( u64 n, bool & exact )
{
	u64 root = 0, bit = 1ULL << 62;
	while( bit > n ) bit >>= 2;
	while( bit != 0 )
	{
		if( n >= root + bit )	{ n -= root + bit; root = (root >> 1) + bit; }
		else					{ root >>= 1; }
		bit >>= 2;
	}
	exact = n == 0;
	return root;
}

// Tiny-result handling, then the cause bits/exception, then the write of a binary32 result
static void R4300_F32Complete( u32 fd, u32 result, u32 causes, bool tiny )
{
	if( tiny )
	{
		const u32 fcsr = gCPUState.FPUControl[31]._u32;
		const u32 enables = (fcsr >> 7) & 0x1F;
		if( (fcsr & FPCSR_FS) == 0 || (enables & (FPU_CAUSE_U | FPU_CAUSE_I)) )
		{
			R4300_FPUSetCause( FPU_CAUSE_E );
			return;
		}
		// Flush: rounding towards the result's infinity gives the smallest normal number
		const u32 sign = result >> 31;
		const u32 rm = fcsr & 3;
		result = (sign << 31) | (((rm == 2 && !sign) || (rm == 3 && sign)) ? 0x00800000 : 0);
		causes |= FPU_CAUSE_U | FPU_CAUSE_I;
	}

	if( R4300_FPUSetCause( causes ) )
		gCPUState.FPU[ fd ]._u32 = result;
}

// CVT.S.D (n64-systemtest CvtS): infinities pass through, NaN is an Invalid Operation, subnormal
// and IEEE signalling NaN inputs are unimplemented, the rest is rounded like an arithmetic result.
static void R4300_Cop1_D_ToS( OpCode op_code )
{
	const u64 d = R4300_RawFPR_D( op_code.fs );
	const u32 sign = (u32)(d >> 63);
	const u32 exp = (u32)(d >> 52) & 0x7FF;
	const u64 mant = d & 0xFFFFFFFFFFFFFULL;
	u32 causes = 0;
	bool tiny = false;
	u32 result;

	if( exp == 0 && mant != 0 )						{ R4300_FPUSetCause( FPU_CAUSE_E ); return; }
	if( exp == 0x7FF && mant != 0 )
	{
		if( (mant & 0x8000000000000ULL) == 0 )		{ R4300_FPUSetCause( FPU_CAUSE_E ); return; }
		causes = FPU_CAUSE_V;
		result = kF32DefaultNaN;
	}
	else if( exp == 0x7FF )		result = (sign << 31) | 0x7F800000;
	else if( exp == 0 )			result = sign << 31;
	else						result = R4300_F32RoundPack( sign, (s32)exp - 1075, mant | 0x10000000000000ULL, causes, tiny );

	R4300_F32Complete( op_code.fd, result, causes, tiny );
}

// CVT.S.W / CVT.S.L
static void R4300_Cop1_IntToS( OpCode op_code, s64 value )
{
	u32 causes = 0;
	bool tiny = false;
	u32 result = 0;
	if( value != 0 )
	{
		const u32 sign = value < 0;
		result = R4300_F32RoundPack( sign, 0, sign ? (u64)-value : (u64)value, causes, tiny );
	}
	R4300_F32Complete( op_code.fd, result, causes, tiny );
}

// CVT/ROUND/TRUNC/CEIL/FLOOR.W/L.S/D (n64-systemtest ConvertToW/ConvertToL): NaN, infinity and
// subnormal inputs and results outside the range (32-bit, or +-(2^53 - 1) for L) are unimplemented;
// a rounded result sets Inexact. The input is the raw double (or the widened single).
static void R4300_Cop1_ToInt( OpCode op_code, u64 d, bool unimplemented, u32 rm, bool to_long )
{
	const u32 sign = (u32)(d >> 63);
	const s32 exp = (s32)(d >> 52) & 0x7FF;
	const u64 mant = d & 0xFFFFFFFFFFFFFULL;
	if( unimplemented || exp == 0x7FF || (exp == 0 && mant != 0) )
	{
		R4300_FPUSetCause( FPU_CAUSE_E );
		return;
	}

	u64 ip = 0;
	bool inexact = false;
	if( exp != 0 )
	{
		const u64 m = mant | 0x10000000000000ULL;
		const s32 e = exp - 1075;						// value = m * 2^e
		if( e >= 0 )
		{
			if( e > 10 ) { R4300_FPUSetCause( FPU_CAUSE_E ); return; }
			ip = m << e;
		}
		else
		{
			const s32 shift = -e;
			bool inc = false;
			if( shift >= 64 )
			{
				ip = 0;
				inexact = true;
				inc = (rm == 2 && !sign) || (rm == 3 && sign);
			}
			else
			{
				ip = m >> shift;
				const u64 rem = m & ((1ULL << shift) - 1);
				const u64 half = 1ULL << (shift - 1);
				inexact = rem != 0;
				switch( rm )
				{
				case 0:	inc = rem > half || (rem == half && (ip & 1)); break;
				case 1:	break;
				case 2:	inc = inexact && !sign; break;
				case 3:	inc = inexact && sign; break;
				}
			}
			ip += inc;
		}
	}

	if( to_long ? ip >= (1ULL << 53) : ip > (sign ? 0x80000000ULL : 0x7FFFFFFFULL) )
	{
		R4300_FPUSetCause( FPU_CAUSE_E );
		return;
	}
	if( !R4300_FPUSetCause( inexact ? FPU_CAUSE_I : 0 ) )
		return;

	const s64 value = sign ? -(s64)ip : (s64)ip;
	if( to_long )	StoreFPR_Long( op_code.fd, (u64)value );
	else			StoreFPR_Word( op_code.fd, (s32)value );
}

static inline void R4300_Cop1_S_ToInt( OpCode op_code, u32 rm, bool to_long )
{
	const u32 f = gCPUState.FPU[ op_code.fs ]._u32;
	R4300_Cop1_ToInt( op_code, R4300_F32BitsToF64Bits( f ), R4300_F32Class( f ) == F32_UNIMPLEMENTED, rm, to_long );
}

static inline void R4300_Cop1_D_ToInt( OpCode op_code, u32 rm, bool to_long )
{
	R4300_Cop1_ToInt( op_code, R4300_RawFPR_D( op_code.fs ), false, rm, to_long );
}

enum ES32Op { S32_ADD, S32_SUB, S32_MUL, S32_DIV, S32_SQRT, S32_ABS, S32_NEG };

static void R4300_Cop1_S_Arith( OpCode op_code, ES32Op op )
{
	const u32 a = gCPUState.FPU[ op_code.fs ]._u32;
	u32 b = gCPUState.FPU[ op_code.ft ]._u32;
	const bool unary = op == S32_SQRT || op == S32_ABS || op == S32_NEG;
	if( op == S32_SUB )
		b ^= 0x80000000;

	const EF32Class ca = R4300_F32Class( a );
	const EF32Class cb = unary ? F32_ZERO : R4300_F32Class( b );
	if( ca == F32_UNIMPLEMENTED || cb == F32_UNIMPLEMENTED )
	{
		R4300_FPUSetCause( FPU_CAUSE_E );
		return;
	}

	u32 causes = 0;
	bool tiny = false;
	u32 result = 0;
	const u32 sa = a >> 31, sb = b >> 31;

	if( ca == F32_QNAN || cb == F32_QNAN )
	{
		causes = FPU_CAUSE_V;
		result = kF32DefaultNaN;
	}
	else switch( op )
	{
	case S32_ABS:	result = a & 0x7FFFFFFF; break;
	case S32_NEG:	result = a ^ 0x80000000; break;

	case S32_ADD:
	case S32_SUB:
		if( ca == F32_INF || cb == F32_INF )
		{
			if( ca == F32_INF && cb == F32_INF && sa != sb )	{ causes = FPU_CAUSE_V; result = kF32DefaultNaN; }
			else												{ result = ca == F32_INF ? a : b; }
		}
		else if( ca == F32_ZERO && cb == F32_ZERO )
		{
			result = sa == sb ? a : ((gCPUState.FPUControl[31]._u32 & 3) == 3 ? 0x80000000 : 0);
		}
		else if( ca == F32_ZERO )	{ result = b; }
		else if( cb == F32_ZERO )	{ result = a; }
		else
		{
			// Mantissas at bits 61..38 leave guard bits below and headroom above
			u64 ma = (u64)R4300_F32Mant( a ) << 38, mb = (u64)R4300_F32Mant( b ) << 38;
			s32 ea = R4300_F32Exp( a ) - 38, eb = R4300_F32Exp( b ) - 38;
			u32 sign_a = sa, sign_b = sb;
			if( ea < eb ) { std::swap( ma, mb ); std::swap( ea, eb ); std::swap( sign_a, sign_b ); }
			const s32 d = ea - eb;
			if( d >= 62 )	mb = 1;		// Sticky
			else if( d > 0 )	mb = (mb >> d) | ((mb & ((1ULL << d) - 1)) != 0);
			u64 m;
			u32 sign;
			if( sign_a == sign_b )	{ m = ma + mb; sign = sign_a; }
			else if( ma >= mb )		{ m = ma - mb; sign = sign_a; }
			else					{ m = mb - ma; sign = sign_b; }
			if( m == 0 )
				result = (gCPUState.FPUControl[31]._u32 & 3) == 3 ? 0x80000000 : 0;	// Exact cancellation
			else
				result = R4300_F32RoundPack( sign, ea, m, causes, tiny );
		}
		break;

	case S32_MUL:
		if( (ca == F32_INF && cb == F32_ZERO) || (ca == F32_ZERO && cb == F32_INF) )	{ causes = FPU_CAUSE_V; result = kF32DefaultNaN; }
		else if( ca == F32_INF || cb == F32_INF )	{ result = ((sa ^ sb) << 31) | 0x7F800000; }
		else if( ca == F32_ZERO || cb == F32_ZERO )	{ result = (sa ^ sb) << 31; }
		else
		{
			result = R4300_F32RoundPack( sa ^ sb, R4300_F32Exp( a ) + R4300_F32Exp( b ),
										(u64)R4300_F32Mant( a ) * R4300_F32Mant( b ), causes, tiny );
		}
		break;

	case S32_DIV:
		if( (ca == F32_ZERO && cb == F32_ZERO) || (ca == F32_INF && cb == F32_INF) )	{ causes = FPU_CAUSE_V; result = kF32DefaultNaN; }
		else if( cb == F32_ZERO )	{ causes = FPU_CAUSE_Z; result = ((sa ^ sb) << 31) | 0x7F800000; }
		else if( ca == F32_INF )	{ result = ((sa ^ sb) << 31) | 0x7F800000; }
		else if( ca == F32_ZERO || cb == F32_INF )	{ result = (sa ^ sb) << 31; }
		else
		{
			const u64 num = (u64)R4300_F32Mant( a ) << 40;
			const u64 den = R4300_F32Mant( b );
			const u64 q = num / den;
			const u64 m = (q << 1) | ((num % den) != 0);
			result = R4300_F32RoundPack( sa ^ sb, R4300_F32Exp( a ) - R4300_F32Exp( b ) - 41, m, causes, tiny );
		}
		break;

	case S32_SQRT:
		if( ca == F32_ZERO )		{ result = a; }
		else if( sa )				{ causes = FPU_CAUSE_V; result = kF32DefaultNaN; }
		else if( ca == F32_INF )	{ result = a; }
		else
		{
			u64 m = R4300_F32Mant( a );
			s32 e = R4300_F32Exp( a );
			if( e & 1 ) { m <<= 1; e--; }
			bool exact;
			const u64 root = R4300_ISqrt64( m << 38, exact );
			result = R4300_F32RoundPack( 0, (e - 38) / 2 - 1, (root << 1) | !exact, causes, tiny );
		}
		break;
	}

	R4300_F32Complete( op_code.fd, result, causes, tiny );
}

static void  R4300_Cop1_W_CVT_S( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_IntToS( op_code, (s32)LoadFPR_Word( op_code.fs ) );
}

static void  R4300_Cop1_W_CVT_D( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?
	s32 nTemp = LoadFPR_Word( op_code.fs );

	// Convert using current rounding mode?
	StoreFPR_Double( op_code.fd, s32_to_d64( nTemp ) );
}

static void  R4300_Cop1_L_CVT_S( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	// Only -2^55 <= x < 2^55 converts; larger magnitudes are unimplemented (n64-systemtest CvtS)
	const s64 value = (s64)LoadFPR_Long( op_code.fs );
	if( value >= (1LL << 55) || value < -(1LL << 55) )
	{
		R4300_FPUSetCause( FPU_CAUSE_E );
		return;
	}
	R4300_Cop1_IntToS( op_code, value );
}

static void  R4300_Cop1_L_CVT_D( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	s64 nTemp = (s64)LoadFPR_Long( op_code.fs );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?
	StoreFPR_Double( op_code.fd, s64_to_d64( nTemp ) );
}

static void  R4300_Cop1_S_Unk( R4300_CALL_SIGNATURE ) { R4300_Cop1_Unimplemented(); }

static void  R4300_Cop1_S_ADD( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_Arith( op_code, S32_ADD );
}

static void  R4300_Cop1_S_SUB( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_Arith( op_code, S32_SUB );
}

static void  R4300_Cop1_S_MUL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_Arith( op_code, S32_MUL );
}

static void  R4300_Cop1_S_DIV( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_Arith( op_code, S32_DIV );
}

static void  R4300_Cop1_S_SQRT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_Arith( op_code, S32_SQRT );
}


static void  R4300_Cop1_S_NEG( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_Arith( op_code, S32_NEG );
}

static void  R4300_Cop1_S_MOV( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// fd = fs
	f32 fValue = LoadFPR_Single( op_code.fs );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?
	StoreFPR_Single( op_code.fd, fValue );// Just copy bits directly?
}

static void  R4300_Cop1_S_ABS( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_Arith( op_code, S32_ABS );
}


static void  R4300_Cop1_S_TRUNC_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, 1, false );
}

static void  R4300_Cop1_S_TRUNC_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, 1, true );
}


static void  R4300_Cop1_S_ROUND_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, 0, false );
}

static void  R4300_Cop1_S_ROUND_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, 0, true );
}


static void  R4300_Cop1_S_CEIL_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, 2, false );
}

static void  R4300_Cop1_S_CEIL_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, 2, true );
}

static void  R4300_Cop1_S_FLOOR_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, 3, false );
}

static void  R4300_Cop1_S_FLOOR_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, 3, true );
}

// Convert float to long - this is used by WarGods
static void  R4300_Cop1_S_CVT_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, gCPUState.FPUControl[31]._u32 & 3, true );
}

// Convert float to word...
static void  R4300_Cop1_S_CVT_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_S_ToInt( op_code, gCPUState.FPUControl[31]._u32 & 3, false );
}

// Convert float to Sim-double...
static void  R4300_Cop1_S_CVT_D( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	f32 fX = LoadFPR_Single( op_code.fs );

	StoreFPR_Double( op_code.fd, f32_to_d64( fX ) );
}

// Used by Mario Party Draft mini game, Earth Worm Jim, Tom and Jerry, Power Puff Girls' disable esimulate double hack
// Convert float to double...
#ifdef SIM_DOUBLES
static void  R4300_Cop1_S_CVT_D_2( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	f32 fX = LoadFPR_Single( op_code.fs );

	REG64 r ;

	r._f64 = (f64)fX;

	gCPUState.FPU[op_code.fd+0]._u32 = r._u32_0;
	FPR_HI( op_code.fd ) = r._u32_1;
}
#endif

// C.cond.fmt on the raw IEEE bits (VR4300, n64-systemtest cop1 compares). Operands are compared as
// doubles; with SIM_DOUBLES a register holding a simulated double is widened from its float first.
// A NaN whose top mantissa bit is set (a signalling NaN on the VR4300, which uses the legacy MIPS
// encoding) raises Invalid Operation for every compare; any NaN does for the signalling compares
// (cond bit 3). An enabled Invalid Operation raises the Floating-Point exception before the
// condition bit is written. Each FP instruction rewrites the cause bits.
static void R4300_Cop1_Compare( u64 a, u64 b, u32 cond )
{
	const bool nan_a = (a & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL;
	const bool nan_b = (b & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL;
	const bool unordered = nan_a || nan_b;
	const bool sig_nan = (nan_a && (a & 0x0008000000000000ULL)) || (nan_b && (b & 0x0008000000000000ULL));

	u32 fcsr = gCPUState.FPUControl[31]._u32 & ~0x0003F000;		// Clear the cause bits
	if( unordered && ((cond & 8) || sig_nan) )
	{
		fcsr |= 0x00010000;										// Cause: Invalid Operation
		if( fcsr & 0x00000800 )									// Enabled: exception, C unchanged
		{
			gCPUState.FPUControl[31]._u32 = fcsr;
			R4300_Exception_General( EXC_FPE, 0 );
			return;
		}
		fcsr |= 0x00000040;										// Flag
	}

	bool less = false, equal = false;
	if( !unordered )
	{
		// Sign-magnitude to ordered integers (+0 and -0 compare equal)
		const s64 ka = (a >> 63) ? -(s64)(a & 0x7FFFFFFFFFFFFFFFULL) : (s64)a;
		const s64 kb = (b >> 63) ? -(s64)(b & 0x7FFFFFFFFFFFFFFFULL) : (s64)b;
		less  = ka < kb;
		equal = ka == kb;
	}

	const bool result = ((cond & 1) && unordered) || ((cond & 2) && equal) || ((cond & 4) && less);
	gCPUState.FPUControl[31]._u32 = result ? (fcsr | FPCSR_C) : (fcsr & ~FPCSR_C);
}

static void  R4300_Cop1_S_EQ( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 2 );
}

static void  R4300_Cop1_S_LT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 12 );
}

static void  R4300_Cop1_S_NGE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 13 );
}

static void  R4300_Cop1_S_LE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 14 );
}

static void  R4300_Cop1_S_SEQ( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 10 );
}

static void  R4300_Cop1_S_UEQ( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 3 );
}

static void  R4300_Cop1_S_NGLE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 9 );
}

static void  R4300_Cop1_S_OLE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 6 );
}

static void  R4300_Cop1_S_ULE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 7 );
}

static void  R4300_Cop1_S_UN( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 1 );
}

static void  R4300_Cop1_S_F( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 0 );
}

// Blast Corps fails here.
// DK64 trows fp nan exception
static void  R4300_Cop1_S_NGT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 15 );
}

static void  R4300_Cop1_S_ULT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 5 );
}

static void  R4300_Cop1_S_SF( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 8 );
}

static void  R4300_Cop1_S_NGL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 11 );
}

static void  R4300_Cop1_S_OLT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_S( op_code.fs ), R4300_RawFPR_S( op_code.ft ), 4 );
}

static void  R4300_Cop1_D_Unk( R4300_CALL_SIGNATURE )		{ R4300_Cop1_Unimplemented(); }


static void  R4300_Cop1_D_ABS( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	d64 fX = LoadFPR_Double( op_code.fs );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	StoreFPR_Double( op_code.fd, R4300_AbsD(fX) );
}

// Used by Buck Bumble to properly work with simulate doubles...
#ifdef SIM_DOUBLES
static void  R4300_Cop1_D_ADD_2( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// fd = fs+ft
	d64 fX = LoadFPR_Double( op_code.fs );
	d64 fY = LoadFPR_Double( op_code.ft );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	REG64	r;

	// Use double, float won't work for buck bumble
	r._f64 = f64( (f64)fX + (f64)fY );

	gCPUState.FPU[op_code.fd+0]._u32 = r._u32_0;
	FPR_HI( op_code.fd ) = r._u32_1;

}
#endif
static void  R4300_Cop1_D_ADD( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// fd = fs+ft
	d64 fX = LoadFPR_Double( op_code.fs );
	d64 fY = LoadFPR_Double( op_code.ft );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	StoreFPR_Double( op_code.fd, fX + fY );

}

static void  R4300_Cop1_D_SUB( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// fd = fs-ft
	d64 fX = LoadFPR_Double( op_code.fs );
	d64 fY = LoadFPR_Double( op_code.ft );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	StoreFPR_Double( op_code.fd, fX - fY );
}

static void  R4300_Cop1_D_MUL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// fd = fs*ft
	d64 fX = LoadFPR_Double( op_code.fs );
	d64 fY = LoadFPR_Double( op_code.ft );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	StoreFPR_Double( op_code.fd, fX * fY );
}

static void  R4300_Cop1_D_DIV( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// fd = fs/ft
	d64 fDividend = LoadFPR_Double( op_code.fs );
	d64 fDivisor = LoadFPR_Double( op_code.ft );

	#ifdef DAEDALUS_ENABLE_ASSERTS
	DAEDALUS_ASSERT(fDivisor != 0, "Double divide by zero");
	#endif

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	StoreFPR_Double( op_code.fd,  fDividend / fDivisor );
}

static void  R4300_Cop1_D_SQRT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// fd = sqrt(fs)
	d64 fX = LoadFPR_Double( op_code.fs );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	StoreFPR_Double( op_code.fd, R4300_SqrtD(fX) );
}


static void  R4300_Cop1_D_NEG( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

	// fd = -(fs)
	d64 fX = LoadFPR_Double( op_code.fs );
// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

	StoreFPR_Double( op_code.fd, -fX );
}

static void  R4300_Cop1_D_MOV( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );

// 	SET_ROUND_MODE( gRoundingMode );		//XXXX Is this needed?

#if 1
	//Fast way, just copy registers //Corn
	gCPUState.FPU[op_code.fd+0]._u32 = gCPUState.FPU[op_code.fs+0]._u32;
	FPR_HI( op_code.fd ) = FPR_HI( op_code.fs );
#else
	// fd = fs
	d64 fX = LoadFPR_Double( op_code.fs );
	StoreFPR_Double( op_code.fd, fX );
#endif
}

static void  R4300_Cop1_D_TRUNC_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, 1, false );
}

static void  R4300_Cop1_D_TRUNC_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, 1, true );
}


static void  R4300_Cop1_D_ROUND_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, 0, false );
}

static void  R4300_Cop1_D_ROUND_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, 0, true );
}


static void  R4300_Cop1_D_CEIL_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, 2, false );
}

static void  R4300_Cop1_D_CEIL_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, 2, true );
}

static void  R4300_Cop1_D_FLOOR_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, 3, false );
}

static void  R4300_Cop1_D_FLOOR_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, 3, true );
}

static void  R4300_Cop1_D_CVT_S( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToS( op_code );
}

// Convert f64 to word...
static void  R4300_Cop1_D_CVT_W( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, gCPUState.FPUControl[31]._u32 & 3, false );
}

static void  R4300_Cop1_D_CVT_L( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_D_ToInt( op_code, gCPUState.FPUControl[31]._u32 & 3, true );
}

static void  R4300_Cop1_D_EQ( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 2 );
}

static void  R4300_Cop1_D_LE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 14 );
}


static void  R4300_Cop1_D_LT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 12 );
}

static void  R4300_Cop1_D_F( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 0 );
}

static void  R4300_Cop1_D_UN( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 1 );
}

static void  R4300_Cop1_D_UEQ( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 3 );
}

static void  R4300_Cop1_D_OLT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 4 );
}

static void  R4300_Cop1_D_ULT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 5 );
}

static void  R4300_Cop1_D_OLE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 6 );
}

static void  R4300_Cop1_D_ULE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 7 );
}

static void  R4300_Cop1_D_SF( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 8 );
}

// Same as above..
static void  R4300_Cop1_D_NGLE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 9 );
}

static void  R4300_Cop1_D_SEQ( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 10 );
}

// Same as above..
static void  R4300_Cop1_D_NGL( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 11 );
}

static void  R4300_Cop1_D_NGE( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 13 );
}

static void  R4300_Cop1_D_NGT( R4300_CALL_SIGNATURE )
{
	R4300_CALL_MAKE_OP( op_code );
	R4300_Cop1_Compare( R4300_RawFPR_D( op_code.fs ), R4300_RawFPR_D( op_code.ft ), 15 );
}

#include "R4300_Jump.inl"		// Jump table

CPU_Instruction	R4300_GetInstructionHandler( OpCode op_code )
{
	switch( op_code.op )
	{
	case OP_SPECOP:
		return R4300SpecialInstruction[ op_code.spec_op ];

	case OP_REGIMM:
		return R4300RegImmInstruction[ op_code.regimm_op ];

	case OP_COPRO0:
		switch( op_code.cop0_op )
		{
		case Cop0Op_TLB:
			return R4300TLBInstruction[ op_code.cop0tlb_funct ];
		default:
			return R4300Cop0Instruction[ op_code.cop0_op ];
		}

	case OP_COPRO1:
		// It is the responsibility of the caller to check whether the
		// copprocessor is enabled, and throw and exception accordingly.
		switch( op_code.cop1_op )
		{
		case Cop1Op_BCInstr:
			return R4300Cop1BC1Instruction[ op_code.cop1_bc ];
		case Cop1Op_SInstr:
			return R4300Cop1SInstruction[ op_code.cop1_funct ];
		case Cop1Op_DInstr:
				return R4300Cop1DInstruction[ op_code.cop1_funct ];

		}
		return R4300Cop1Instruction[ op_code.cop1_op ];

	default:
		return R4300Instruction[ op_code.op ];
	}
}

//Used to swap functions(apply hacks) in interpreter mode (used for the PSP only)
void R4300_Init()
{
#ifdef SIM_DOUBLES
	if(g_ROM.GameHacks == BUCK_BUMBLE)
	{
		R4300Cop1DInstruction[Cop1OpFunc_ADD]	= R4300_Cop1_D_ADD_2;
	}
	else
	{
		R4300Cop1DInstruction[Cop1OpFunc_ADD]	= R4300_Cop1_D_ADD;
	}

	// Mario Party Draft mini game, Earth Worm Jim, Tom and Jerry, Power Puff Girls
    if( g_ROM.DISABLE_SIM_CVT_D_S )
	{
		R4300Cop1SInstruction[Cop1OpFunc_CVT_D] = R4300_Cop1_S_CVT_D_2;
	}
	else
	{
		R4300Cop1SInstruction[Cop1OpFunc_CVT_D] = R4300_Cop1_S_CVT_D;
	}
#endif
#ifdef DAEDALUS_PSP
	if(g_ROM.SET_ROUND_MODE)
	{
		R4300Cop1Instruction[Cop1Op_CTC1]	= R4300_Cop1_CTC1_2;
	}
	else
	{
		R4300Cop1Instruction[Cop1Op_CTC1]	= R4300_Cop1_CTC1;
	}
#endif
}
