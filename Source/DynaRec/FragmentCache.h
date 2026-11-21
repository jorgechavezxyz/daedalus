/*
Copyright (C) 2006 StrmnNrmn

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

#ifndef DYNAREC_FRAGMENTCACHE_H_
#define DYNAREC_FRAGMENTCACHE_H_

#include "Base/Types.h"


class	CFragment;
class	CJumpLocation;
class	CCodeBufferManager;

#include <map>
#include "DynaRec/AssemblyUtils.h"
#include <vector>
#include <array>
#include <memory>
#include <filesystem>

struct FHashT
{
	u32	addr;
	uintptr_t ptr;
};

//*************************************************************************************
//
//*************************************************************************************
class CFragmentCacheCoverage
{
public:
	CFragmentCacheCoverage() { Reset(); }

	void			ExtendCoverage( u32 address, u32 len );
	bool			IsCovered( u32 address, u32 len ) const;

	void			Reset();

private:
	static u32		AddressToIndex( u32 address );

private:
	static const u32 BASE_ADDRESS = 0x80000000;

	static const u32 MEMORY_8_MEG = 8*1024*1024;
	static const u32 MEM_USAGE_SHIFT = 12;		// 4k
	static const u32 NUM_MEM_USAGE_ENTRIES = MEMORY_8_MEG >> MEM_USAGE_SHIFT;

	std::array<bool, NUM_MEM_USAGE_ENTRIES> mCacheCoverage;

};

//*************************************************************************************
//
//*************************************************************************************
class CFragmentCache
{
public:
	CFragmentCache();
	~CFragmentCache();

#ifdef DAEDALUS_DEBUG_DYNAREC
	CFragment *				LookupFragment( u32 address ) const;
#endif
	CFragment *				LookupFragmentQ( u32 address ) const;
	void					InsertFragment( CFragment * p_fragment );

	u32						GetCacheSize() const					{ return mFragments.size(); }
	void					Clear();

#ifdef DAEDALUS_DEBUG_DYNAREC
	void					DumpStats( const std::filesystem::path outputdir ) const;
#endif

	u32						GetMemoryUsage() const					{ return mMemoryUsage; }

	std::shared_ptr<CCodeBufferManager>	GetCodeBufferManager() const			{ return mpCodeBufferManager; }

	bool					ShouldInvalidateOnWrite( u32 address, u32 length ) const;

	// Remove only the fragments built from code in [address, address + length) instead of
	// flushing everything. Returns the number of fragments removed.
	u32						InvalidateRange( u32 address, u32 length );
	bool					IsCodeBufferNearlyFull() const;

private:
	struct SFragmentEntry
	{
		SFragmentEntry( u32 address, CFragment * fragment )
			:	Address( address )
			,	Fragment( fragment )
		{
		}

		bool operator<( const SFragmentEntry & rhs ) const
		{
			return Address < rhs.Address;
		}

		u32			Address;
		CFragment *	Fragment;
	};
	using FragmentVec = std::vector<SFragmentEntry>;

	FragmentVec				mFragments;			// Sorted on Address

	u32						mMemoryUsage;
	u32						mInputLength;
	u32						mOutputLength;
	using JumpList = std::vector< CJumpLocation >;
	using JumpMap = std::map< u32, JumpList >;
	JumpMap					mJumpMap;

	// Jumps that have been patched to branch straight into a fragment, keyed by that fragment's
	// address, with the instruction they held before patching (so they can be unlinked).
	struct SLinkedJump
	{
		CJumpLocation	Jump;
		u32				OriginalOp;
	};
	using LinkedJumpMap = std::map< u32, std::vector< SLinkedJump > >;
	LinkedJumpMap			mLinkedJumps;

	void					LinkJump( u32 target_address, CJumpLocation jump, CCodeLabel target );

	// Live fragments per 4KB page of RDRAM, so writes only look at fragments that can overlap
	// them (replaces the old "page ever held code" marks, which were never cleared).
	static const u32		kPageShift = 12;
	static const u32		kNumPages = (8 * 1024 * 1024) >> kPageShift;
	std::array< std::vector< CFragment * >, kNumPages >	mPageFragments;
	void					AddToPages( CFragment * p_fragment );
	void					RemoveFromPages( CFragment * p_fragment );

	mutable u32				mCachedFragmentAddress;
	mutable CFragment *		mpCachedFragment;

	static const u32 HASH_TABLE_BITS = 15;
	static const u32 HASH_TABLE_SIZE = 1<<HASH_TABLE_BITS;

	//Low 2 bits will always be 0, remove this redundancy (the amount of folding depends on the hash table size)
	//#define MakeHashIdx( addr ) (((addr >> (2 * HASH_TABLE_BITS + 2)) ^ (addr >> (HASH_TABLE_BITS + 2)) ^ addr >> 2 ) & (HASH_TABLE_SIZE-1))
	#define MakeHashIdx( addr ) (((addr >> (HASH_TABLE_BITS + 2)) ^ addr >> 2 ) & (HASH_TABLE_SIZE-1))

	mutable std::array<FHashT, HASH_TABLE_SIZE> mpCacheHashTable;
	// mutable FHashT			mpCacheHashTable[HASH_TABLE_SIZE];

	std::shared_ptr<CCodeBufferManager>	mpCodeBufferManager;

	CFragmentCacheCoverage	mCacheCoverage;
};

extern CFragmentCache				gFragmentCache;
#endif // DYNAREC_FRAGMENTCACHE_H_
