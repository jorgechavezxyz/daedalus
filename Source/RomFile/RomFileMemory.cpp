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
	virtual u32		GetCapacity() const		{ return mCapacity; }

private:
#ifdef DAEDALUS_PSP
	CMemoryHeap *	mRomMemoryHeap;
#endif
	u32				mCapacity = 0xFFFFFFFF;
};

#ifdef DAEDALUS_PSP
u32 gRomHeapLargestFreeAtInit = 0;

// Largest single block malloc can currently provide (binary search; nothing is kept).
u32 PSP_GetLargestFreeBlock()
{
	u32 lo = 0, hi = 128 * 1024 * 1024;
	while( hi - lo > 64 * 1024 )
	{
		u32 mid = lo + (hi - lo) / 2;
		void * p = malloc( mid );
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
	// Allocate large memory heap for SLIM+ (32Mb) Used for ROM Buffer and ROM Cache
	// Otherwise allocate small memory heap for PHAT (2Mb) Used for ROM cache only
	//
	if( PSP_IS_SLIM )
	{
		// With the full 64MB available (MEMSIZE=1) a 32MB ROM can stay entirely in RAM,
		// which avoids streaming from the Memory Stick. Size the heap from what is actually
		// free, keeping enough back for textures, the dynarec and audio.
		const u32 kWanted   = 32 * 1024 * 1024 + 64 * 1024;
		const u32 kReserve  = 12 * 1024 * 1024;
		const u32 kFallback = 16 * 1024 * 1024;
		const u32 largest   = PSP_GetLargestFreeBlock();
		mCapacity = (largest >= kWanted + kReserve) ? kWanted : kFallback;
		gRomHeapLargestFreeAtInit = largest;
	}
	else
	{
		mCapacity = 2 * 1024 * 1024;
	}
	mRomMemoryHeap = CMemoryHeap::Create( mCapacity );
#endif
// #ifdef DAEDALUS_POSIX
// 	mRomMemoryHeap = CMemoryHeap::Create(21 * 1024 * 1024);
// #endif
}


IROMFileMemory::~IROMFileMemory()
{
#ifdef DAEDALUS_PSP
	delete mRomMemoryHeap;
#endif
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
	std::cout << "Allocating Memory" << std::endl;
#ifdef DAEDALUS_PSP
	return mRomMemoryHeap->Alloc( size );
#else
	return malloc( size );
#endif
}


void  IROMFileMemory::Free(void * ptr)
{
#ifdef DAEDALUS_PSP
	mRomMemoryHeap->Free( ptr );
#else
std::cout << "Freeing Memory" << std::endl;
	free( ptr );
#endif
}
