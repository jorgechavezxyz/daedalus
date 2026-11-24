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

// Various stuff to map an address onto the correct memory region


#include "Base/Types.h"

#include "Core/DMA.h"
#include "Utility/BenchTimers.h"
#include "Core/Memory.h"
#include "Core/RSP_HLE.h"
#include "Core/CPU.h"
#include "Core/ROM.h"
#include "Ultra/ultra_os.h"
#include "RomFile/ROMBuffer.h"
#include "Core/PIF.h"
#include "Core/Interrupt.h"
#include "Core/Save.h"
#include "Debug/DebugLog.h"
#include "Debug/DBGConsole.h"
#include "OSHLE/OSTask.h"
#include "OSHLE/patch.h"
#include "Utility/FastMemcpy.h"

bool gDMAUsed = false;
//*****************************************************************************
//
//*****************************************************************************
// SP DMA (mupen64plus rsp_core.c do_sp_dma): 'count' rows of 'length' bytes (both addresses 8-byte
// aligned, length rounded up to 8); RDRAM skips 'skip' bytes after each row and the SP address wraps
// inside its 4KB bank (DMEM or IMEM, never into the other). Afterwards the address registers point
// past the transfer and the length register reads 0xFF8 (n64-systemtest sp_memory::dma).
static void DMA_SP_Transfer( bool to_rdram, u32 len_reg_offset )
{
	const u32 mem_reg  = Memory_SP_GetRegister(SP_MEM_ADDR_REG);
	const u32 dram_reg = Memory_SP_GetRegister(SP_DRAM_ADDR_REG);
	const u32 len_reg  = Memory_SP_GetRegister(len_reg_offset);

	const u32 length = ((len_reg & 0xFFF) | 7) + 1;
	const u32 count  = ((len_reg >> 12) & 0xFF) + 1;
	const u32 skip   = (len_reg >> 20) & 0xFFF;

	u32 memaddr  = mem_reg & 0xFF8;
	u32 dramaddr = dram_reg & 0xFFFFF8;
	u8 * spmem = (mem_reg & 0x1000) == 0 ? g_pu8SpDmemBase : g_pu8SpImemBase;

	if( count == 1 && memaddr + length <= 0x1000 && dramaddr + length <= gRamSize )
	{
		// Common case: a single row that doesn't wrap. Both sides are 8-byte aligned, so the
		// word-swizzled memory can be copied directly.
		if( to_rdram )
			fast_memcpy( &g_pu8RamBase[dramaddr], &spmem[memaddr], length );
		else
			fast_memcpy( &spmem[memaddr], &g_pu8RamBase[dramaddr], length );
		memaddr  += length;
		dramaddr += length;
	}
	else
	{
		for( u32 c = 0; c < count; c++ )
		{
			for( u32 i = 0; i < length; i += 8 )
			{
				u32 * sp = (u32 *)&spmem[memaddr & 0xFF8];
				u32 * rd = (u32 *)&g_pu8RamBase[dramaddr & 0x7FFFF8];
				if( to_rdram )	{ rd[0] = sp[0]; rd[1] = sp[1]; }
				else			{ sp[0] = rd[0]; sp[1] = rd[1]; }
				memaddr  += 8;
				dramaddr += 8;
			}
			dramaddr += skip;
		}
	}

	Memory_SP_SetRegister(SP_MEM_ADDR_REG, (mem_reg & 0x1000) | (memaddr & 0xFF8));
	Memory_SP_SetRegister(SP_DRAM_ADDR_REG, dramaddr & 0xFFFFF8);
	Memory_SP_SetRegister(len_reg_offset, 0xFF8);

	//Clear the DMA Busy
	Memory_SP_SetRegister(SP_DMA_BUSY_REG, 0);
	Memory_SP_ClrRegisterBits(SP_STATUS_REG, SP_STATUS_DMA_BUSY);
}

void DMA_SP_CopyFromRDRAM()
{
	DMA_SP_Transfer( false, SP_RD_LEN_REG );
}

//*****************************************************************************
//
//*****************************************************************************
void DMA_SP_CopyToRDRAM()
{
	DMA_SP_Transfer( true, SP_WR_LEN_REG );
}

//*****************************************************************************
// Copy 64bytes from DRAM to PIF_RAM
//*****************************************************************************
void DMA_SI_CopyFromDRAM( )
{
	u32 mem  = Memory_SI_GetRegister(SI_DRAM_ADDR_REG) & 0x1fffffff;
	u32 * dst = (u32 *)g_pMemoryBuffers[MEM_PIF_RAM];
	u32 * src = (u32 *)(g_pu8RamBase + mem);

	DPF( DEBUG_MEMORY_PIF, "DRAM (0x%08x) -> PIF Transfer ", mem );

	for(u32 i = 0; i < 16; i++)
	{
		dst[i] = BSWAP32(src[i]);
	}

	DMA_SI_ScheduleCompletion( false );
}

//*****************************************************************************
// Copy 64bytes to DRAM from PIF_RAM
//*****************************************************************************
void DMA_SI_CopyToDRAM( )
{
	// Check controller status!
	CController::Get()->Process();

	u32 mem = Memory_SI_GetRegister(SI_DRAM_ADDR_REG) & 0x1fffffff;
	u32 * src = (u32 *)g_pMemoryBuffers[MEM_PIF_RAM];
	u32 * dst = (u32 *)(g_pu8RamBase + mem);

	DPF( DEBUG_MEMORY_PIF, "PIF -> DRAM (0x%08x) Transfer ", mem );

	for(u32 i = 0; i < 16; i++)
	{
		dst[i] = BSWAP32(src[i]);
	}

	//Skipping this IRQ fixes allows Body Harvest, Nightmare Creatures and Cruisn' USA to boot
	//(kept: the delayed interrupt below may make it unnecessary, but those games are untested)
	DMA_SI_ScheduleCompletion( g_ROM.GameHacks == BODY_HARVEST );
}

//*****************************************************************************
// SI DMA timing (mupen64plus si_controller.c): the data moves at once, but the
// controller stays busy and the SI interrupt arrives 0x900 cycles later instead of
// immediately.
//*****************************************************************************
static const s32	kSIDMADuration = 0x900;		// mupen64plus DEFAULT_SI_DMA_DURATION
static bool			gSISkipInterrupt = false;

void DMA_SI_Complete()
{
	Memory_SI_ClrRegisterBits(SI_STATUS_REG, SI_STATUS_DMA_BUSY);
	Memory_SI_SetRegisterBits(SI_STATUS_REG, SI_STATUS_INTERRUPT);
	Memory_MI_SetRegisterBits(MI_INTR_REG, MI_INTR_SI);
	if (!gSISkipInterrupt)
		R4300_Interrupt_UpdateCause3();
}

void DMA_SI_ScheduleCompletion( bool skip_interrupt )
{
	gSISkipInterrupt = skip_interrupt;

	bool pending = false;
	for (u32 i = 0; i < gCPUState.NumEvents; ++i)
		pending |= gCPUState.Events[i].mEventType == CPU_EVENT_SI;

	// The event table is small (and its layout is fixed for the recompiler), so if it's full,
	// or a completion is already queued, finish right away as Daedalus always did.
	if (!pending && gCPUState.NumEvents < MAX_CPU_EVENTS)
	{
		Memory_SI_SetRegisterBits(SI_STATUS_REG, SI_STATUS_DMA_BUSY);
		CPU_AddEvent(kSIDMADuration, CPU_EVENT_SI);
	}
	else
	{
		DMA_SI_Complete();
	}
}

//*****************************************************************************
// AI DMA (mupen64plus ai_controller.c): a 2-entry FIFO with AI_STATUS BUSY/FULL, and the AI
// interrupt raised when each DMA ends, after the time the buffer takes to play. Daedalus never
// raised it, so games whose audio thread waits for OS_EVENT_AI (the MusyX games: Rogue Squadron,
// Tarzan, Resident Evil 2, ...) wrote one buffer and never sent an audio task again.
// On for every game; roms.ini AIInterrupt=no turns it off. Same event-table fallback as SI/PI:
// with no free slot the DMA ends at once.
//*****************************************************************************
static u32			gAILength[2] = {};			// FIFO: [0] the DMA playing (pending CPU_EVENT_AI), [1] the queued one
static u32			gAIDuration[2] = {};

static u32 DMA_AI_Duration( u32 length )
{
	// mupen get_dma_duration, with CPU counts per second estimated from Daedalus' VI period
	static const u32 kVIClock[3] = { 49656530, 48681812, 48628316 };	// OS_TV_PAL, NTSC, MPAL
	const u32 tv = g_ROM.TvType < 3 ? g_ROM.TvType : OS_TV_NTSC;
	const u32 samples_per_sec = kVIClock[tv] / (1 + Memory_AI_GetRegister(AI_DACRATE_REG));
	const u32 counts_per_sec = CPU_GetVIInterruptCycles() * (tv == OS_TV_PAL ? 50 : 60);
	return length * (counts_per_sec / (4 * samples_per_sec));
}

static void DMA_AI_RaiseInterrupt()
{
	Memory_MI_SetRegisterBits(MI_INTR_REG, MI_INTR_AI);
	R4300_Interrupt_UpdateCause3();
}

// Starts a DMA; false if it ended at once (no event slot)
static bool DMA_AI_Start( u32 duration )
{
	bool pending = false;
	for (u32 i = 0; i < gCPUState.NumEvents; ++i)
		pending |= gCPUState.Events[i].mEventType == CPU_EVENT_AI;

	if (pending || gCPUState.NumEvents >= MAX_CPU_EVENTS)
		return false;

	Memory_AI_SetRegisterBits(AI_STATUS_REG, AI_STATUS_DMA_BUSY);
	CPU_AddEvent(duration > 0 ? duration : 1, CPU_EVENT_AI);
	return true;
}

// AI_LEN written (by the CPU or the osAiSetNextBuffer patch)
void DMA_AI_LenWritten()
{
	if (!g_ROM.settings.AIInterrupt)
		return;

	const u32 length = Memory_AI_GetRegister(AI_LEN_REG);
	if (length == 0)
		return;

	const u32 duration = DMA_AI_Duration(length);
	if (Memory_AI_GetRegister(AI_STATUS_REG) & AI_STATUS_DMA_BUSY)
	{
		gAILength[1] = length & ~7u;
		gAIDuration[1] = duration;
		Memory_AI_SetRegisterBits(AI_STATUS_REG, AI_STATUS_FIFO_FULL);
	}
	else if (DMA_AI_Start(duration))
	{
		gAILength[0] = length & ~7u;
		gAIDuration[0] = duration;
	}
	else
	{
		DMA_AI_RaiseInterrupt();
	}
}

// AI_LEN read (by the CPU or the osAiGetLength patch): like mupen get_remaining_dma_length, the
// bytes of the current DMA still to play. Without the AI interrupt, the written length as before.
u32 DMA_AI_GetLength()
{
	if (!g_ROM.settings.AIInterrupt)
		return Memory_AI_GetRegister(AI_LEN_REG);

	if (!(Memory_AI_GetRegister(AI_STATUS_REG) & AI_STATUS_DMA_BUSY) || gAIDuration[0] == 0)
		return 0;

	s32 remaining = 0;
	for (u32 i = 0; i < gCPUState.NumEvents; ++i)
	{
		remaining += gCPUState.Events[i].mCount;
		if (gCPUState.Events[i].mEventType == CPU_EVENT_AI)
		{
			if (remaining <= 0)
				return 0;
			const u64 length = (u64)(u32)remaining * gAILength[0] / gAIDuration[0];
			return (u32)length & ~7u;
		}
	}
	return 0;
}

// CPU_EVENT_AI: end of the current DMA
void DMA_AI_Complete()
{
	Memory_AI_ClrRegisterBits(AI_STATUS_REG, AI_STATUS_DMA_BUSY);
	if (Memory_AI_GetRegister(AI_STATUS_REG) & AI_STATUS_FIFO_FULL)
	{
		Memory_AI_ClrRegisterBits(AI_STATUS_REG, AI_STATUS_FIFO_FULL);
		gAILength[0] = gAILength[1];
		gAIDuration[0] = gAIDuration[1];
		DMA_AI_Start(gAIDuration[0]);
	}
	DMA_AI_RaiseInterrupt();
}

/*
#define PI_DOM2_ADDR1		0x05000000	// to 0x05FFFFFF
#define PI_DOM1_ADDR1		0x06000000	// to 0x07FFFFFF
#define PI_DOM2_ADDR2		0x08000000	// to 0x0FFFFFFF
#define PI_DOM1_ADDR2		0x10000000	// to 0x1FBFFFFF
#define PI_DOM1_ADDR3		0x1FD00000	// to 0x7FFFFFFF
*/

#define IsDom1Addr1( x )		( (x) >= PI_DOM1_ADDR1 && (x) < PI_DOM2_ADDR2 )
#define IsDom1Addr2( x )		( (x) >= PI_DOM1_ADDR2 && (x) < 0x1FBFFFFF )
#define IsDom1Addr3( x )		( (x) >= PI_DOM1_ADDR3 && (x) < 0x7FFFFFFF )
#define IsDom2Addr1( x )		( (x) >= PI_DOM2_ADDR1 && (x) < PI_DOM1_ADDR1 )
#define IsDom2Addr2( x )		( (x) >= PI_DOM2_ADDR2 && (x) < PI_DOM1_ADDR2 )

#define IsFlashDomAddr( x )		( (x) >= PI_DOM2_ADDR2 && (x) < 0x08010000 )

//*****************************************************************************
//
//*****************************************************************************
bool DMA_HandleTransfer( u8 * p_dst, u32 dst_offset, u32 dst_size, const u8 * p_src, u32 src_offset, u32 src_size, u32 length )
{
	if( ( s32( length ) <= 0 ) ||
		(src_offset + length) > src_size ||
		(dst_offset + length) > dst_size )
	{
		return false;
	}

	fast_memcpy_swizzle(&p_dst[dst_offset], &p_src[src_offset], length);
	return true;
}

//*****************************************************************************
//
//*****************************************************************************
static void OnCopiedRom()
{
	if (!gDMAUsed)
	{
		gDMAUsed = true;

#ifdef DAEDALUS_ENABLE_OS_HOOKS
		// Note the rom is only scanned when the ROM jumps to the game boot address
		// ToDO: try to reapply patches - certain roms load in more of the OS after a number of transfers ?
		Patch_ApplyPatches();
#endif

		// Set RDRAM size
		u32 addr = (g_ROM.cic_chip != CIC_6105) ? (u32)0x318 : (u32)0x3F0;
		*(u32 *)(g_pu8RamBase + addr) = gRamSize;

		// Azimer's DK64 hack, it makes DK64 boot!
		if(g_ROM.GameHacks == DK64)
			*(u32 *)(g_pu8RamBase + 0x2FE1C0) = 0xAD170014;
	}
}

void DMA_PI_CopyToRDRAM()
{
	BENCH_SCOPE( BENCH_DMA );
	const u32 dram_reg = Memory_PI_GetRegister(PI_DRAM_ADDR_REG);
	const u32 cart_reg = Memory_PI_GetRegister(PI_CART_ADDR_REG);
	u32 mem_address  = dram_reg & 0x00FFFFFE;
	u32 cart_address = cart_reg & ~1u;
	u32 pi_length_reg = (Memory_PI_GetRegister(PI_WR_LEN_REG) & 0x00FFFFFF) + 1;
	bool copy_succeeded = false;

	// mupen64plus dma_pi_write: the PI handles the first 128 bytes differently
	// (https://n64brew.dev/wiki/Peripheral_Interface#Unaligned_DMA_transfer): odd lengths
	// are only rounded up from 0x7F on, and short transfers lose the RDRAM misalignment.
	if( pi_length_reg >= 0x7F && (pi_length_reg & 1) )
		pi_length_reg++;
	if( pi_length_reg <= 0x80 )
		pi_length_reg = pi_length_reg >= (mem_address & 7) ? pi_length_reg - (mem_address & 7) : 0;

	DPF( DEBUG_MEMORY_PI, "PI: Copying %d bytes of data from 0x%08x to 0x%08x", pi_length_reg, cart_address, mem_address );

	if ( IsDom2Addr1( cart_address ))
	{
		//DBGConsole_Msg(0, "[YReading from Cart domain 2/addr1]");
		const u8* p_src    = (const u8*)g_pMemoryBuffers[MEM_SAVE];
		u32       src_size = (MemoryRegionSizes[MEM_SAVE]);
		cart_address -= PI_DOM2_ADDR1;
		copy_succeeded = DMA_HandleTransfer( g_pu8RamBase, mem_address, gRamSize, p_src, cart_address, src_size, pi_length_reg );
	}
	else if ( IsDom1Addr1( cart_address ))
	{
		//DBGConsole_Msg(0, "[YReading from Cart domain 1/addr1]");
		cart_address -= PI_DOM1_ADDR1;
		CPU_InvalidateICacheRange( 0x80000000 | mem_address, pi_length_reg );
		copy_succeeded = RomBuffer::CopyToRam( g_pu8RamBase, mem_address, gRamSize, cart_address, pi_length_reg );
	}
	else if ( IsDom2Addr2( cart_address ) )
	{
		//DBGConsole_Msg(0, "[YReading from Cart domain 2/addr2]");
		const u8* p_src    = (const u8*)g_pMemoryBuffers[MEM_SAVE];
		u32       src_size = (MemoryRegionSizes[MEM_SAVE]);
		cart_address -= PI_DOM2_ADDR2;

		if (g_ROM.settings.SaveType != SAVE_TYPE_FLASH)
			copy_succeeded = DMA_HandleTransfer( g_pu8RamBase, mem_address, gRamSize, p_src, cart_address, src_size, pi_length_reg );
		else
			copy_succeeded = DMA_FLASH_CopyToDRAM(mem_address, cart_address, pi_length_reg);

	}
	else if ( IsDom1Addr2( cart_address ) )
	{
		//DBGConsole_Msg(0, "[YReading from Cart domain 1/addr2]");
		cart_address -= PI_DOM1_ADDR2;
		CPU_InvalidateICacheRange( 0x80000000 | mem_address, pi_length_reg );
		copy_succeeded = RomBuffer::CopyToRam( g_pu8RamBase, mem_address, gRamSize, cart_address, pi_length_reg );

	}
	else if ( IsDom1Addr3( cart_address ) )
	{
		//DBGConsole_Msg(0, "[YReading from Cart domain 1/addr3]");
		cart_address -= PI_DOM1_ADDR3;
		CPU_InvalidateICacheRange( 0x80000000 | mem_address, pi_length_reg );
		copy_succeeded = RomBuffer::CopyToRam( g_pu8RamBase, mem_address, gRamSize, cart_address, pi_length_reg );
	}
	else
	{
		DBGConsole_Msg(0, "[YUnknown PI Address 0x%08x]", cart_address);
	}

	if(copy_succeeded)
	{
		OnCopiedRom();
	}
#ifdef DAEDALUS_DEBUG_CONSOLE	
	else
	{
		DBGConsole_Msg(0, "PI: Copying 0x%08x bytes of data from 0x%08x to 0x%08x",
			Memory_PI_GetRegister(PI_WR_LEN_REG),
			Memory_PI_GetRegister(PI_CART_ADDR_REG),
			Memory_PI_GetRegister(PI_DRAM_ADDR_REG));
		DBGConsole_Msg(0, "PIXFer: Copy overlaps RAM/ROM boundary");
		DBGConsole_Msg(0, "PIXFer: Not copying, but issuing interrupt");
	}
#endif
	// The address registers advance past the transfer; the length registers read back 0x7F
	// (mupen64plus dma_pi_write, read_pi_regs)
	Memory_PI_SetRegister(PI_DRAM_ADDR_REG, ((dram_reg + pi_length_reg + 7) & ~7u) & 0x00FFFFFE);
	Memory_PI_SetRegister(PI_CART_ADDR_REG, (cart_reg + pi_length_reg + 1) & ~1u);
	Memory_PI_SetRegister(PI_WR_LEN_REG, 0x7F);

	// mupen64plus cart_rom_dma_write: length/8 cycles for ROM, 0x1000 for SRAM/FlashRAM
	DMA_PI_ScheduleCompletion( (cart_reg & 0x1fffffff) >= 0x10000000 ? pi_length_reg / 8 : 0x1000 );
}
//*****************************************************************************
//
//*****************************************************************************
void DMA_PI_CopyFromRDRAM()
{
	u32 mem_address = Memory_PI_GetRegister(PI_DRAM_ADDR_REG) & 0xFFFFFFFF;
	u32 cart_address = Memory_PI_GetRegister(PI_CART_ADDR_REG)  & 0xFFFFFFFF;
	u32 pi_length_reg = (Memory_PI_GetRegister(PI_RD_LEN_REG)  & 0xFFFFFFFF) + 1;
	bool copy_succeeded = false;

	if( pi_length_reg & 0x1 )
	{
		//This makes Doraemon 3 work
		DBGConsole_Msg(0, "PI DMA odd length");
		pi_length_reg++;
	}

	DPF(DEBUG_MEMORY_PI, "PI: Copying %d bytes of data from 0x%08x to 0x%08x", pi_length_reg, mem_address, cart_address );

	if ( IsDom2Addr1( cart_address ) )
	{
		//DBGConsole_Msg(0, "[YWriting to Cart domain 2/addr1]");
		u8 * p_dst = (u8 *)g_pMemoryBuffers[MEM_SAVE];
		u32	dst_size = MemoryRegionSizes[MEM_SAVE];
		cart_address -= PI_DOM2_ADDR1;

		copy_succeeded = DMA_HandleTransfer( p_dst, cart_address, dst_size, g_pu8RamBase, mem_address, gRamSize, pi_length_reg );
		Save_MarkSaveDirty();
	}
	else if ( IsDom1Addr1( cart_address ) )
	{
		//DBGConsole_Msg(0, "[YWriting to Cart domain 1/addr1]");
		cart_address -= PI_DOM1_ADDR1;
		copy_succeeded = RomBuffer::CopyFromRam( cart_address, g_pu8RamBase, mem_address, gRamSize, pi_length_reg );
	}
	else if ( IsDom2Addr2( cart_address ) )
	{
		//DBGConsole_Msg(0, "[YWriting to Cart domain 2/addr2]");
		u8 * p_dst = (u8 *)g_pMemoryBuffers[MEM_SAVE];
		u32	dst_size = MemoryRegionSizes[MEM_SAVE];
		cart_address -= PI_DOM2_ADDR2;

		if (g_ROM.settings.SaveType != SAVE_TYPE_FLASH)
			copy_succeeded = DMA_HandleTransfer( p_dst, cart_address, dst_size, g_pu8RamBase, mem_address, gRamSize, pi_length_reg );
		else
			copy_succeeded = DMA_FLASH_CopyFromDRAM(mem_address, pi_length_reg);

		Save_MarkSaveDirty();
	}
	else if ( IsDom1Addr2( cart_address ) )
	{
		//DBGConsole_Msg(0, "[YWriting to Cart domain 1/addr2]");
		cart_address -= PI_DOM1_ADDR2;
		copy_succeeded = RomBuffer::CopyFromRam( cart_address, g_pu8RamBase, mem_address, gRamSize, pi_length_reg );
	}
	else if ( IsDom1Addr3( cart_address ) )
	{
		//DBGConsole_Msg(0, "[YWriting to Cart domain 1/addr3]");
		cart_address -= PI_DOM1_ADDR3;
		copy_succeeded = RomBuffer::CopyFromRam( cart_address, g_pu8RamBase, mem_address, gRamSize, pi_length_reg );
	}
	else
	{
		DBGConsole_Msg(0, "[YUnknown PI Address 0x%08x]", cart_address);
	}

#ifdef DAEDALUS_DEBUG_CONSOLE
	if(!copy_succeeded)
	{
		DBGConsole_Msg(0, "PI: Copying %d bytes of data from 0x%08x to 0x%08x",
			pi_length_reg, mem_address, cart_address);
		DBGConsole_Msg(0, "PIXFer: Copy overlaps RAM/ROM boundary");
		DBGConsole_Msg(0, "PIXFer: Not copying, but issuing interrupt");
	}
#endif
	DMA_PI_ScheduleCompletion( 0x1000 );	// mupen64plus: RDRAM -> cart DMAs take 0x1000 cycles
}

//*****************************************************************************
// PI DMA timing (mupen64plus pi_controller.c): the data moves at once, but PI_STATUS
// stays busy and the PI interrupt arrives after the DMA's duration instead of instantly.
//*****************************************************************************
// A 32-bit CPU write to the cartridge ROM area is latched by the PI: the next read from the cart
// returns it (once), PI_STATUS.IO_BUSY is set while it is pending, writes made while the PI is busy
// are dropped, and the value goes away after a short while (mupen64plus write_cart_rom/read_cart_rom;
// n64-systemtest cart_memory::write: only the first write counts and the value has decayed after
// ~110 three-instruction loop iterations). A Bug's Life and Toy Story 2 depend on the latch.
static const u32	kCartWriteLatchCycles = 300;
static bool			gPIEventIsCartWrite = false;		// The pending CPU_EVENT_PI ends a cart write, not a DMA

void DMA_PI_CartWrite( u32 value )
{
	if( Memory_PI_GetRegister(PI_STATUS_REG) & (PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY) )
		return;

	RomBuffer::SaveRomValue( value );

	bool pending = false;
	for (u32 i = 0; i < gCPUState.NumEvents; ++i)
		pending |= gCPUState.Events[i].mEventType == CPU_EVENT_PI;

	// Without a free event slot the value simply stays until it is read (the old behaviour)
	if( !pending && gCPUState.NumEvents < MAX_CPU_EVENTS )
	{
		Memory_PI_SetRegisterBits(PI_STATUS_REG, PI_STATUS_IO_BUSY);
		gPIEventIsCartWrite = true;
		CPU_AddEvent(kCartWriteLatchCycles, CPU_EVENT_PI);
	}
}

// A CPU read from the cart is synchronous: it consumes the latched value and the PI is no longer busy
void DMA_PI_CartRead()
{
	if( Memory_PI_GetRegister(PI_STATUS_REG) & PI_STATUS_IO_BUSY )
		Memory_PI_ClrRegisterBits(PI_STATUS_REG, PI_STATUS_IO_BUSY);
}

// CPU_EVENT_PI: end of a cart write latch or of a DMA
void DMA_PI_OnEvent()
{
	if( gPIEventIsCartWrite )
	{
		gPIEventIsCartWrite = false;
		Memory_PI_ClrRegisterBits(PI_STATUS_REG, PI_STATUS_IO_BUSY);
		RomBuffer::ClearRomValue();
		return;
	}
	DMA_PI_Complete();
}

void DMA_PI_Complete()
{
	Memory_PI_ClrRegisterBits(PI_STATUS_REG, PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY);
	Memory_PI_SetRegisterBits(PI_STATUS_REG, PI_STATUS_INTERRUPT);	// mupen64plus pi_end_of_dma_event
	Memory_MI_SetRegisterBits(MI_INTR_REG, MI_INTR_PI);
	R4300_Interrupt_UpdateCause3();
}

void DMA_PI_ScheduleCompletion( u32 cycles )
{
	bool pending = false;
	for (u32 i = 0; i < gCPUState.NumEvents; ++i)
		pending |= gCPUState.Events[i].mEventType == CPU_EVENT_PI;

	// Same fallback as SI: the event table is fixed-size, so if it's full or a PI completion
	// is already queued, finish right away.
	if (cycles > 0 && !pending && gCPUState.NumEvents < MAX_CPU_EVENTS)
	{
		Memory_PI_SetRegisterBits(PI_STATUS_REG, PI_STATUS_DMA_BUSY);
		CPU_AddEvent(cycles, CPU_EVENT_PI);
	}
	else
	{
		DMA_PI_Complete();
	}
}
