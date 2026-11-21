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
const u32 kRomMemoryReserve = 2 * 1024 * 1024;	// left free for everything else

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
	// PSP-2000/3000/Go: reserve one heap at startup for ROM data / the ROM cache, sized from the
	// memory actually free (64MB layout: ~30MB; 24MB layout: a few MB). Allocating per ROM
	// instead fragmented memory and lost ~8MB every time a different ROM was opened.
	// PSP-1000: small fixed heap, used for the ROM cache only.
	//
	gRomHeapLargestFreeAtInit = PSP_GetLargestFreeBlock();
	if( PSP_IS_SLIM )
	{
		const u32 kMaxHeap = 32 * 1024 * 1024 + 64 * 1024;	// Enough for a 32MB ROM
		const u32 kMinHeap = 1 * 1024 * 1024;
		const u32 free_mem = gRomHeapLargestFreeAtInit;
		u32 size = free_mem > kRomMemoryReserve ? free_mem - kRomMemoryReserve : 0;
		if( size > kMaxHeap ) size = kMaxHeap;
		if( size < kMinHeap ) size = kMinHeap;
		mCapacity = size & ~0xFFFFu;
	}
	else
	{
		mCapacity = 2 * 1024 * 1024;
	}
	mRomMemoryHeap = CMemoryHeap::Create( mCapacity );
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
	return mRomMemoryHeap->Alloc( size );		// nullptr if it doesn't fit; callers fall back
#else
	return malloc( size );
#endif
}


void  IROMFileMemory::Free(void * ptr)
{
#ifdef DAEDALUS_PSP
	mRomMemoryHeap->Free( ptr );
#else
	free( ptr );
#endif
}
