/*
Copyright (C) 2008-2009 Howard Su (howard0su@gmail.com)

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

#include "Core/ROM.h"
#include "Core/Memory.h"
#include "Core/Save.h"
#include "Interface/ConfigOptions.h"
#include "Debug/DBGConsole.h"
#include "Debug/Dump.h"
#include "Utility/Paths.h"
#include <iostream>
#include <cstring> 
#include <vector>
#include <fstream>
#include <algorithm>
#include <ctime>

static void InitMempackContent();

static std::filesystem::path gSaveFileName;
static bool				gSaveDirty;
static u32				gSaveSize;
static std::filesystem::path	gMempackFileName;
static bool				gMempackDirty;


bool Save_Reset()
{
	const char * ext;
	switch (g_ROM.settings.SaveType)
	{
	// EEPROM sizes are in bits: 4Kbit = 512 bytes, 16Kbit = 2048 bytes.
	case SAVE_TYPE_EEP4K:
		ext = ".sav";
		gSaveSize = 512;
		break;
	case SAVE_TYPE_EEP16K:
		ext = ".sav";
		gSaveSize = 2048;
		break;
	case SAVE_TYPE_SRAM:
		ext = ".sra";
		gSaveSize = 32 * 1024;
		break;
	case SAVE_TYPE_FLASH:
		ext = ".fla";
		gSaveSize = 128 * 1024;
		break;
	default:
		ext = "";
		gSaveSize = 0;
		break;
	}
	DAEDALUS_ASSERT( gSaveSize <= MemoryRegionSizes[MEM_SAVE], "Save size is larger than allocated memory");

	gSaveDirty = false;
	if (gSaveSize > 0)
	{
		std::filesystem::path saveDir = setBasePath("SaveGames");
		std::filesystem::path romName = g_ROM.mFileName;
		gSaveFileName = saveDir / (romName.filename().string());
		gSaveFileName.replace_extension(ext);
		std::ifstream file(gSaveFileName, std::ios::in | std::ios::binary);
		if (file.is_open())
		{
			DBGConsole_Msg(0, "Loading save from [C%s]", gSaveFileName.string().c_str());

			u8 buffer[2048];
			u8 * dst = (u8*)g_pMemoryBuffers[MEM_SAVE];

			// Older builds wrote 4KB/16KB EEPROM files; only the first gSaveSize bytes are used.
			for (u32 d = 0; d < gSaveSize; d += sizeof(buffer))
			{
				const u32 chunk = std::min<u32>(sizeof(buffer), gSaveSize - d);
				memset(buffer, 0, sizeof(buffer));
				file.read(reinterpret_cast<char*>(buffer), chunk);

				for (u32 i = 0; i < chunk; i++)
				{
					dst[d+i] = buffer[i^U8_TWIDDLE];
				}
			}

		}
		else
		{
			DBGConsole_Msg(0, "Save File [C%s] cannot be found.", gSaveFileName.string().c_str());
		}
	}

	// init mempack, we always assume the presence of the mempack for simplicity 
	{	
		std::filesystem::path saveDir = setBasePath("SaveGames");
		std::filesystem::path romName = g_ROM.mFileName;
		gMempackFileName = saveDir / (romName.filename().string());
		gMempackFileName.replace_extension(".mpk");
		bool fileExists = std::filesystem::exists(gMempackFileName); 
		std::fstream file(gMempackFileName, std::ios::in | std::ios::out | std::ios::binary);
		if (!file)
		{
			// Initialise Mempack but do not write file unless it's needed
			InitMempackContent();

		}
		else
			{
				DBGConsole_Msg(0, "Loading MemPack from [C%s]", gMempackFileName.string().c_str());
				file.read(static_cast<char*>(g_pMemoryBuffers[MEM_MEMPACK]), MemoryRegionSizes[MEM_MEMPACK]);
				gMempackDirty = false;
			}
		}

	return true;
}

void Save_Fini()
{
	gSaveDirty = true;
	gMempackDirty = true;
	
	Save_Flush();
}

void Save_MarkSaveDirty()
{
	// Called on every save write; no console output here (it stalls the PSP).
	gSaveDirty = true;
}

void Save_MarkMempackDirty()
{
	gMempackDirty = true;
}

void Save_Flush()
{
	if (gSaveDirty && g_ROM.settings.SaveType != SAVE_TYPE_UNKNOWN)
	{

		DBGConsole_Msg(0, "Saving to [C%s]", gSaveFileName.string().c_str());
		std::ofstream fp(gSaveFileName, std::ios::binary);
		if (fp.is_open())
		{
			u8 buffer[2048];
			u8 * src = (u8*)g_pMemoryBuffers[MEM_SAVE];

			for (u32 d = 0; d < gSaveSize; d += sizeof(buffer))
			{
				const u32 chunk = std::min<u32>(sizeof(buffer), gSaveSize - d);
				for (u32 i = 0; i < chunk; i++)
				{
					buffer[i^U8_TWIDDLE] = src[d+i];
				}
				fp.write(reinterpret_cast<char*>(buffer), chunk);
			}
		}
		gSaveDirty = false;
	}

	if (gMempackDirty)
	{
		// XXX We could have file generate on write and initiaise if not already done.

		DBGConsole_Msg(0, "Saving MemPack to [C%s]", gMempackFileName.string().c_str());
		std::ofstream fp(gMempackFileName, std::ios::out | std::ios::binary );
		if (fp.is_open())
		{
			fp.write(reinterpret_cast<char*>(g_pMemoryBuffers[MEM_MEMPACK]), MemoryRegionSizes[MEM_MEMPACK]);	
		}
		gMempackDirty = false;
	}
}

// Mempack Stuffs

// Blank Controller Pak in the layout of mupen64plus format_mempak (paks/mempak.c): page 0 holds
// the ID block (random serial, device id 0x0001, 1 bank, version 0, sum and 0xFFF2 - sum) at
// blocks 1, 3, 4 and 6; page 1 is the index table (5 reserved inodes, inode 0's second byte is
// the checksum of the 123 free inodes, each 0x0003) and page 2 its backup; directory and data
// pages are zero.
static void FormatMempack(u8 * mem, u64 & seed)
{
	const u32 kPageSize = 256;
	u8 * page_0 = mem;
	u8 * page_1 = mem + kPageSize;

	memset(mem, 0, 0x8000);

	u8 * id = page_0 + 1 * 32;
	for (u32 k = 0; k < 6; ++k)
	{
		seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;		// xorshift64
		const u32 serial = (u32)(seed >> 32);
		id[k * 4 + 0] = (u8)(serial >> 24);
		id[k * 4 + 1] = (u8)(serial >> 16);
		id[k * 4 + 2] = (u8)(serial >> 8);
		id[k * 4 + 3] = (u8)serial;
	}
	id[24] = 0x00; id[25] = 0x01;		// device id
	id[26] = 0x01;						// banks
	id[27] = 0x00;						// version
	u16 sum = 0;
	for (u32 i = 0; i < 28; i += 2)
		sum += (u16)((id[i] << 8) | id[i + 1]);
	const u16 isum = (u16)(0xFFF2 - sum);
	id[28] = (u8)(sum >> 8);  id[29] = (u8)sum;
	id[30] = (u8)(isum >> 8); id[31] = (u8)isum;
	memcpy(page_0 + 3 * 32, id, 32);
	memcpy(page_0 + 4 * 32, id, 32);
	memcpy(page_0 + 6 * 32, id, 32);

	u8 checksum = 0;
	for (u32 i = 5; i < 128; ++i)
	{
		page_1[2 * i + 0] = 0x00;
		page_1[2 * i + 1] = 0x03;
		checksum += 0x03;
	}
	page_1[1] = checksum;
	memcpy(page_1 + kPageSize, page_1, kPageSize);
}

static void InitMempackContent()
{
	u64 seed = (u64)time(nullptr) * 0x9E3779B97F4A7C15ULL | 1;
	for (size_t dst_off = 0; dst_off + 0x8000 <= MemoryRegionSizes[MEM_MEMPACK]; dst_off += 32 * 1024)
	{
		FormatMempack(static_cast<u8*>(g_pMemoryBuffers[MEM_MEMPACK]) + dst_off, seed);
	}
}