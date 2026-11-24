/*
Copyright (C) 2009 StrmnNrmn

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

// Stuff to handle Processor

#include "Base/Types.h"

#include "Core/CPU.h"
#include "Debug/Registers.h"					// For REG_?? defines
#include "Core/Memory.h"
#include "Core/Interpret.h"
#include "Core/Interrupt.h"
#include "RomFile/ROMBuffer.h"
#include "Core/R4300.h"
#include "Interface/ConfigOptions.h"
#include "Debug/DBGConsole.h"
#include "Debug/DebugLog.h"
#include "OSHLE/patch.h"				// GetCorrectOp
#include "Ultra/ultra_R4300.h"
#include "Base/Macros.h"
#include "Utility/Profiler.h"
#include "Debug/Synchroniser.h"

//*****************************************************************************
//	Execute a single MIPS op. The conditionals for the templated arguments
//	are completely optimised away by the compiler.
//
//	TranslateOp:	Use this to translate breakpoints/patches to original op
//					before execution.
//*****************************************************************************
static inline bool R4300_IsNonLikelyBranch( OpCode op_code )
{
	switch( op_code.op )
	{
	case OP_BEQ: case OP_BNE: case OP_BLEZ: case OP_BGTZ:
		return true;
	case OP_REGIMM:
		return (op_code.regimm_op & ~0x11) == 0;					// BLTZ, BGEZ, BLTZAL, BGEZAL
	case OP_COPRO1:
		return op_code.rs == 8 && (op_code.rt & 2) == 0;			// BC1F, BC1T
	default:
		return false;
	}
}

// static: Dynamo.cpp defines a CPU_EXECUTE_OP template with the same signature. Without internal
// linkage the two instantiations collide (ODR) and the linker could pick Dynamo's version for the
// interpreter core.
template< bool TranslateOp > static inline void CPU_EXECUTE_OP()
{
	u8 * p_Instruction = 0;

	CPU_FETCH_INSTRUCTION( p_Instruction, gCPUState.CurrentPC );
	OpCode op_code = *(OpCode*)p_Instruction;

	// Cache instruction base pointer (used for SpeedHack() @ R4300.0)
	gLastAddress = p_Instruction;

#ifdef DAEDALUS_BREAKPOINTS_ENABLED
	if ( TranslateOp )
	{
		// Handle breakpoints correctly
		if (op_code.op == OP_DBG_BKPT)
		{
			// Turn temporary disable on to allow instr to be processed
			// Entry is in lower 26 bits...
			u32	breakpoint( op_code.bp_index );

			if ( breakpoint < g_BreakPoints.size() )
			{
				if (g_BreakPoints[ breakpoint ].mEnabled)
				{
					g_BreakPoints[ breakpoint ].mTemporaryDisable = true;
				}
			}
		}
		else
		{
			op_code = GetCorrectOp( op_code );
		}
	}
#endif

	SYNCH_POINT( DAED_SYNC_REG_PC, gCPUState.CurrentPC, "Program Counter doesn't match" );
	SYNCH_POINT( DAED_SYNC_FRAGMENT_PC, gCPUState.CurrentPC + gCPUState.Delay, "Program Counter/Delay doesn't match while interpreting" );

	SYNCH_POINT( DAED_SYNC_REG_PC, gCPUState.CPUControl[C0_COUNT]._u32, "Count doesn't match" );

	R4300_ExecuteInstruction(op_code);
	gGPR[0]._u64 = 0;	//Ensure r0 is zero

#ifdef DAEDALUS_PROFILE_EXECUTION
		gTotalInstructionsEmulated++;
#endif

	SYNCH_POINT( DAED_SYNC_REGS, CPU_ProduceRegisterHash(), "Registers don't match" );

	// Increment count register
	gCPUState.CPUControl[C0_COUNT]._u32 = gCPUState.CPUControl[C0_COUNT]._u32 + COUNTER_INCREMENT_PER_OP;

	if (CPU_ProcessEventCycles( COUNTER_INCREMENT_PER_OP ) )
	{
		CPU_HANDLE_COUNT_INTERRUPT();
	}

	switch (gCPUState.Delay)
	{
	case DO_DELAY:
		// We've got a delayed instruction to execute. Increment
		// PC as normal, so that subsequent instruction is executed
		INCREMENT_PC();
		gCPUState.Delay = EXEC_DELAY;

		break;
	case EXEC_DELAY:
		{
			//bool	backwards( gCPUState.TargetPC <= gCPUState.CurrentPC );

			// We've just executed the delayed instr. Now carry out jump as stored in gCPUState.TargetPC;
			CPU_SetPC(gCPUState.TargetPC);
			gCPUState.Delay = NO_DELAY;

			// Fetching from an unaligned target (JR/JALR) raises AdEL with EPC = BadVAddr = target
			if( (gCPUState.TargetPC & 3) && !gCPUState.IsJobSet( CPU_CHECK_EXCEPTIONS ) )
				R4300_Exception_AddressError( gCPUState.TargetPC, false );

		}
		break;
	case NO_DELAY:
		// Normal operation - just increment the PC
		INCREMENT_PC();

		// A conditional (non-likely) branch that isn't taken still has a delay slot: run the next
		// instruction as one, so an exception there reports BD and EPC = branch (VR4300; n64-systemtest
		// 'Unaligned LW exception (delay slot)'). The branch handlers only set DO_DELAY when taken,
		// which the dynarec's trace recorder relies on, so this is done here, in the interpreter loop.
		if( R4300_IsNonLikelyBranch( op_code ) && !gCPUState.IsJobSet( CPU_CHECK_EXCEPTIONS ) )
		{
			gCPUState.TargetPC = gCPUState.CurrentPC + 4;
			gCPUState.Delay = EXEC_DELAY;
		}
		break;
	}
}


//*****************************************************************************
// Keep executing instructions until there are other tasks to do (i.e. gCPUState.GetStuffToDo() is set)
// Process these tasks and loop
//*****************************************************************************
void CPU_Go()
{
	DAEDALUS_PROFILE( __FUNCTION__ );

	while (CPU_KeepRunning())
	{
		//
		// Keep executing ops as long as there's nothing to do
		//
		u32	stuff_to_do( gCPUState.GetStuffToDo() );
		while(stuff_to_do == 0)
		{
			CPU_EXECUTE_OP< false >();

			stuff_to_do = gCPUState.GetStuffToDo();
		}

		if (CPU_CheckStuffToDo())
			break;
	}
}


void Inter_SelectCore()
{
   g_pCPUCore = CPU_Go;
}

//*****************************************************************************
// Hacky function to use when debugging
//*****************************************************************************
void CPU_Skip()
{
	#ifdef DAEDALUS_DEBUG_CONSOLE
	if (CPU_IsRunning())
	{
		DBGConsole_Msg(0, "Already Running");
		return;
	}
#endif
	INCREMENT_PC();
}

//*****************************************************************************
//
//*****************************************************************************
void CPU_Step()
{
		#ifdef DAEDALUS_DEBUG_CONSOLE
	if (CPU_IsRunning())
	{
		DBGConsole_Msg(0, "Already Running");
		return;
	}
#endif

	CPU_CheckStuffToDo();

	CPU_EXECUTE_OP< true >();
}
