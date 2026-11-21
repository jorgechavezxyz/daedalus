/*
Copyright (C) 2012 StrmnNrmn

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

#include "RomFile/RomFileMemory.h"
#include "Utility/MemoryHeap.h"

#include <stdlib.h>
#include <iostream> 
#include <new>
#include <malloc.h>

extern bool PSP_IS_SLIM;

CROMFileMemory::~CROMFileMemory() {}


class IROMFileMemory : public CROMFileMemory
{
public:
	IROMFileMemory();
	~IROMFileMemory();

//	virtual	bool	IsAvailable();
	virtual void *	Alloc( u32 size );
	virtual void	Free(void * ptr);
	virtual u32		GetCapacity() const;

private:
#ifdef DAEDALUS_PSP
	CMemoryHeap *	mRomMemoryHeap;
#endif
	u32				mCapacity = 0xFFFFFFFF;
};

#ifdef DAEDALUS_PSP
u32 gRomHeapLargestFreeAtInit = 0;
const u32 kRomMemoryReserve = 4 * 1024 * 1024;

// Largest single block malloc can currently provide (binary search; nothing is kept).
u32 PSP_GetLargestFreeBlock()
{
	u32 lo = 0, hi = 128 * 1024 * 1024;
	while( hi - lo > 64 * 1024 )
	{
		u32 mid = lo + (hi - lo) / 2;
		void * p = malloc( mid );
		// Stop GCC from folding away the malloc/free pair (it then assumes success).
		asm volatile( "" : "+r"( p ) : : "memory" );
		if( p ) { free( p ); lo = mid; }
		else	{ hi = mid; }
	}
	return lo;
}
#endif



template<> bool CSingleton< CROMFileMemory >::Create()
{
	#ifdef DAEDALUS_ENABLE_ASSERTS
	DAEDALUS_ASSERT_Q(mpInstance == nullptr);
	#endif

	mpInstance = std::make_shared<IROMFileMemory>();
	return mpInstance != NULL;
}


IROMFileMemory::IROMFileMemory()
{
#ifdef DAEDALUS_PSP
	//
	// PSP-2000/3000/Go (64MB): ROM data and the ROM cache are allocated when a ROM opens,
	// sized to what that ROM needs, so all memory not used by the emulator can hold ROM data.
	// PSP-1000: small fixed heap, used for the ROM cache only.
	//
	if( PSP_IS_SLIM )
	{
		mRomMemoryHeap = nullptr;
	}
	else
	{
		mCapacity = 2 * 1024 * 1024;
		mRomMemoryHeap = CMemoryHeap::Create( mCapacity );
	}
	gRomHeapLargestFreeAtInit = PSP_GetLargestFreeBlock();
#endif
}


IROMFileMemory::~IROMFileMemory()
{
#ifdef DAEDALUS_PSP
	delete mRomMemoryHeap;
#endif
}

u32 IROMFileMemory::GetCapacity() const
{
#ifdef DAEDALUS_PSP
	if( mRomMemoryHeap == nullptr )
	{
		// Keep kRomMemoryReserve free for allocations made after the ROM is loaded
		// (dynarec fragment data, textures, audio).
		const u32 largest = PSP_GetLargestFreeBlock();
		return largest > kRomMemoryReserve ? largest - kRomMemoryReserve : 0;
	}
#endif
	return mCapacity;
}

/*
bool IROMFileMemory::IsAvailable()
{
	DAEDALUS_ASSERT( mRomMemoryHeap != NULL, "This heap isn't available" );

	return mRomMemoryHeap != NULL;
}
*/

void * IROMFileMemory::Alloc( u32 size )
{
#ifdef DAEDALUS_PSP
	if( mRomMemoryHeap == nullptr )
		return memalign( 64, size );		// nullptr if it doesn't fit; callers fall back
	return mRomMemoryHeap->Alloc( size );
#else
	return malloc( size );
#endif
}


void  IROMFileMemory::Free(void * ptr)
{
#ifdef DAEDALUS_PSP
	if( mRomMemoryHeap == nullptr )
	{
		free( ptr );
		return;
	}
	mRomMemoryHeap->Free( ptr );
#else
	free( ptr );
#endif
}
