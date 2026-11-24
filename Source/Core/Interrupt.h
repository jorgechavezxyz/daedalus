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

#ifndef CORE_INTERRUPT_H_
#define CORE_INTERRUPT_H_

#include "Core/CPU.h"
#include "Ultra/ultra_R4300.h"


// 64-bit COP0 registers (Context, BadVAddr, EntryHi, EPC, XContext, ErrorEPC).
// gCPUState.CPUControl[] holds the low word (read by MFC0 and the dynarec); gCP0Wide[] holds
// the full value for DMFC0/DMTC0. If the low word was changed by a 32-bit write path that
// doesn't know about gCP0Wide, the register reads back sign-extended, which is what a 32-bit
// write does on hardware.
extern u64 gCP0Wide[32];
extern u64 gCP0Latch;		// Last value written with MTC0/DMTC0; read back from unused COP0 registers

inline u64 R4300_GetCop0Wide( u32 reg )
{
	const u32 lo = gCPUState.CPUControl[reg]._u32;
	const u64 wide = gCP0Wide[reg];
	return ( (u32)wide == lo ) ? wide : (u64)(s64)(s32)lo;
}

inline void R4300_SetCop0Wide( u32 reg, u64 value )
{
	gCP0Wide[reg] = value;
	gCPUState.CPUControl[reg]._u32 = (u32)value;
}

void R4300_Exception_TLB( u32 virtual_address, u32 exception_code, u32 exception_vector );
void R4300_Exception_Break();
void R4300_Exception_Syscall();
void R4300_Exception_Trap();
void R4300_Exception_CopUnusuable();

// Update the CAUSE_IP3 value after MI_INTR_MASK_REG or MI_INTR_REG changes
inline void R4300_Interrupt_UpdateCause3()
{
	//
	// If any interrupts pending when they are unmasked, the interrupt fires
	//
	if ((Memory_MI_GetRegister(MI_INTR_MASK_REG) &
		 Memory_MI_GetRegister(MI_INTR_REG)) == 0)
	{
		// Clear the Cause register
		gCPUState.CPUControl[C0_CAUSE]._u32 &= ~CAUSE_IP3;
	}
	else
	{
		gCPUState.CPUControl[C0_CAUSE]._u32 |= CAUSE_IP3;
		gCPUState.AddJob( CPU_CHECK_INTERRUPTS );
	}
}

// Queue an interrupt check if an unmasked interrupt is pending and interrupts can be taken.
inline void R4300_CheckPendingInterrupts()
{
	const u32 sr = gCPUState.CPUControl[C0_SR]._u32;
	if( (sr & (SR_EXL | SR_ERL | SR_IE)) == SR_IE &&
		(sr & gCPUState.CPUControl[C0_CAUSE]._u32 & CAUSE_IPMASK) != 0 )
	{
		gCPUState.AddJob( CPU_CHECK_INTERRUPTS );
	}
}

void R4300_Handle_Exception();
void R4300_Handle_Interrupt();

#ifdef DAEDALUS_PROFILE_EXECUTION
extern u32 gNumExceptions;
extern u32 gNumInterrupts;
#endif


#endif // CORE_INTERRUPT_H_
