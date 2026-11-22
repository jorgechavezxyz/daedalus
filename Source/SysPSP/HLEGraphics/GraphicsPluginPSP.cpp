/*
Copyright (C) 2007 StrmnNrmn

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

#include "Debug/DBGConsole.h"

#include "HLEGraphics/BaseRenderer.h"
#include "HLEGraphics/TextureCache.h"
#include "HLEGraphics/DLParser.h"
#include "HLEGraphics/DLDebug.h"
#include "HLEGraphics/DisplayListDebugger.h"

#include "Graphics/GraphicsContext.h"
#include "HLEGraphics/GraphicsPlugin.h"

#include "Utility/Profiler.h"
#include "Utility/FramerateLimiter.h"
#include "Interface/Preferences.h"
#include "System/Timing/Timing.h"

#include <pspdebug.h>

#include "Core/Memory.h"
#include "Core/CPU.h"
#include "Utility/BenchTimers.h"
#include "Core/ROM.h"
#include "RomFile/RomFileMemory.h"
#include "RomFile/ROMBuffer.h"
#include "Interface/ConfigOptions.h"
#include <cstdio>


extern void battery_warning();
extern void HandleEndOfFrame();

extern bool gFrameskipActive;

u32		gSoundSync =  44100;
u32		gVISyncRate = 1500;
bool	gTakeScreenshot = false;
bool	gTakeScreenshotSS = false;

EFrameskipValue		gFrameskipValue = FV_DISABLED;

namespace
{
	//u32					gVblCount = 0;
	u32					gFlipCount = 0;
	//float				gCurrentVblrate = 0.0f;
	float				gCurrentFramerate = 0.0f;
	u64					gLastFramerateCalcTime = 0;
	u64					gTicksPerSecond = 0;

#ifdef DAEDALUS_FRAMERATE_ANALYSIS
	u32					gTotalFrames = 0;
	u64					gFirstFrameTime = 0;
	FILE *				gFramerateFile = nullptr;
#endif

static void	UpdateFramerate()
{
#ifdef DAEDALUS_FRAMERATE_ANALYSIS
	gTotalFrames++;
#endif
	gFlipCount++;

	u64			now = 0;
	NTiming::GetPreciseTime( &now );

	if(gLastFramerateCalcTime == 0)
	{
		u64		freq = 0;
		gLastFramerateCalcTime = now;

		NTiming::GetPreciseFrequency( &freq );
		gTicksPerSecond = freq;
	}

#ifdef DAEDALUS_FRAMERATE_ANALYSIS
	if( gFramerateFile == nullptr )
	{
		gFirstFrameTime = now;
		gFramerateFile = fopen( "framerate.csv", "w" );
	}
	fprintf( gFramerateFile, "%d,%f\n", gTotalFrames, f32(now - gFirstFrameTime) / f32(gTicksPerSecond) );
#endif

	// If 1 second has elapsed since last recalculation, do it now
	u64		ticks_since_recalc( now - gLastFramerateCalcTime );
	if(ticks_since_recalc > gTicksPerSecond)
	{
		//gCurrentVblrate = float( gVblCount * gTicksPerSecond ) / float( ticks_since_recalc );
		gCurrentFramerate = float( gFlipCount * gTicksPerSecond ) / float( ticks_since_recalc );

		//gVblCount = 0;
		gFlipCount = 0;
		gLastFramerateCalcTime = now;

#ifdef DAEDALUS_FRAMERATE_ANALYSIS
		if( gFramerateFile != nullptr )
		{
			fflush( gFramerateFile );
		}
#endif
	}

}
}

// Benchmark logging for real hardware: if benchmark.txt exists next to the EBOOT when a ROM
// starts, append one line per real-time second to bench.log with emulated VI/s, emulation
// speed and frames presented/rendered. Cheap when disabled (one flag test per flip).
extern bool gSelectiveInvalidation;
u64 gBenchPhaseUs[NUM_BENCH_PHASES] = {};
u32 gBenchRomCacheMisses = 0;
u32 gBenchFragmentFlushes = 0;
u32 gBenchPartialInvalidations = 0;
u32 gBenchFlushReasons[4] = {};

namespace
{
	bool	gBenchChecked = false;
	bool	gBenchEnabled = false;
	u64		gBenchFreq = 0;
	u64		gBenchStart = 0;
	u64		gBenchLast = 0;
	u32		gBenchLastVI = 0;
	u32		gBenchFlips = 0;
	u32		gBenchRendered = 0;
	u32		gBenchSeconds = 0;
	u32		gBenchTotalFlips = 0;
	u32		gBenchTotalRendered = 0;
	u32		gBenchStartVI = 0;
	u64		gBenchTotalPhaseUs[NUM_BENCH_PHASES] = {};
	u32		gBenchTotalRomMisses = 0;
	u32		gBenchTotalFlushes = 0;
	u32		gBenchDuration = 120;	// seconds; "seconds=N" in benchmark.txt
	char	gBenchBuffer[1024];		// Lines are batched so Memory Stick writes don't skew results
	u32		gBenchBufferLen = 0;

	void BenchmarkFlushBuffer()
	{
		if( gBenchBufferLen == 0 ) return;
		if( FILE * log = fopen( "bench.log", "a" ) )
		{
			fwrite( gBenchBuffer, 1, gBenchBufferLen, log );
			fclose( log );
		}
		gBenchBufferLen = 0;
	}

	void BenchmarkWriteAverage();

	void BenchmarkFlip( bool rendered )
	{
		if( !gBenchChecked )
		{
			gBenchChecked = true;
			if( FILE * f = fopen( "benchmark.txt", "r" ) )
			{
				char line[64];
				while( fgets( line, sizeof( line ), f ) )
				{
					unsigned v;
					if( sscanf( line, "seconds=%u", &v ) == 1 && v >= 10 ) gBenchDuration = v;
				}
				fclose( f );
				gBenchEnabled = true;
				NTiming::GetPreciseFrequency( &gBenchFreq );
				NTiming::GetPreciseTime( &gBenchStart );
				gBenchLast = gBenchStart;
				gBenchStartVI = gBenchLastVI = CPU_GetVerticalInterruptCount();
				for( u32 i = 0; i < NUM_BENCH_PHASES; ++i ) { gBenchPhaseUs[i] = 0; gBenchTotalPhaseUs[i] = 0; }
				gBenchRomCacheMisses = gBenchFragmentFlushes = gBenchTotalRomMisses = gBenchTotalFlushes = gBenchPartialInvalidations = 0;
				gBenchFlushReasons[0] = gBenchFlushReasons[1] = gBenchFlushReasons[2] = gBenchFlushReasons[3] = 0;
				if( FILE * log = fopen( "bench.log", "a" ) )
				{
					fprintf( log, "\n# %s | build %s %s | dynarec=%d frameskip=%d countperop=%u selective=%d\n",
						g_ROM.settings.GameName.c_str(), __DATE__, __TIME__, gDynarecEnabled, (int)gFrameskipValue, gCountPerOp, gSelectiveInvalidation );
					fprintf( log, "# memory: free at startup %uKB, free with ROM loaded %uKB, ROM %uKB %s\n",
						gRomHeapLargestFreeAtInit / 1024, PSP_GetLargestFreeBlock() / 1024, RomBuffer::GetRomSize() / 1024,
						RomBuffer::IsRomAddressFixed() ? "fully in RAM" : "streamed from storage" );
					fprintf( log, "# sec, vi_per_sec, speed_pct, flips_per_sec, rendered_per_sec, free_kb, gfx_pct, audio_pct, present_pct, limit_pct, cpu_other_pct, romread_pct, compile_pct, dma_pct, rom_misses, dynarec_flushes\n" );
					fclose( log );
				}
			}
		}
		if( !gBenchEnabled ) return;

		gBenchFlips++;
		if( rendered ) gBenchRendered++;

		u64 now; NTiming::GetPreciseTime( &now );
		if( now - gBenchLast < gBenchFreq ) return;

		const f32 secs = f32( now - gBenchLast ) / f32( gBenchFreq );
		const u32 vi = CPU_GetVerticalInterruptCount();
		const f32 vi_per_sec = f32( vi - gBenchLastVI ) / secs;
		const f32 hz = f32( FramerateLimiter_GetTvFrequencyHz() );
		gBenchSeconds++;
		gBenchTotalFlips += gBenchFlips;
		gBenchTotalRendered += gBenchRendered;
		f32 phase_pct[NUM_BENCH_PHASES]; f32 other_pct = 100.0f;
		// UpdateScreen (present) runs inside the graphics task, so report gfx without it.
		// ROM reads mostly happen inside PI DMA: report DMA without them.
		gBenchPhaseUs[BENCH_DMA] = gBenchPhaseUs[BENCH_DMA] > gBenchPhaseUs[BENCH_ROMREAD] ? gBenchPhaseUs[BENCH_DMA] - gBenchPhaseUs[BENCH_ROMREAD] : 0;
		gBenchPhaseUs[BENCH_GFX] = gBenchPhaseUs[BENCH_GFX] > gBenchPhaseUs[BENCH_PRESENT] ? gBenchPhaseUs[BENCH_GFX] - gBenchPhaseUs[BENCH_PRESENT] : 0;
		for( u32 i = 0; i < NUM_BENCH_PHASES; ++i ) { phase_pct[i] = f32( gBenchPhaseUs[i] ) / ( secs * 10000.0f ); if( i <= BENCH_LIMIT ) other_pct -= phase_pct[i]; }
		if( gBenchBufferLen + 96 > sizeof( gBenchBuffer ) ) BenchmarkFlushBuffer();
		gBenchBufferLen += snprintf( gBenchBuffer + gBenchBufferLen, sizeof( gBenchBuffer ) - gBenchBufferLen,
			"%u, %.1f, %.1f, %.1f, %.1f, %u, %.0f, %.0f, %.0f, %.0f, %.0f, %.1f, %.1f, %.1f, %u, %u\n", gBenchSeconds, vi_per_sec, 100.0f * vi_per_sec / hz,
			f32( gBenchFlips ) / secs, f32( gBenchRendered ) / secs, PSP_GetLargestFreeBlock() / 1024,
			phase_pct[BENCH_GFX], phase_pct[BENCH_AUDIO], phase_pct[BENCH_PRESENT], phase_pct[BENCH_LIMIT], other_pct,
			phase_pct[BENCH_ROMREAD], phase_pct[BENCH_COMPILE], phase_pct[BENCH_DMA], gBenchRomCacheMisses, gBenchFragmentFlushes );
		gBenchTotalRomMisses += gBenchRomCacheMisses; gBenchTotalFlushes += gBenchFragmentFlushes;
		gBenchRomCacheMisses = 0; gBenchFragmentFlushes = 0;
		for( u32 i = 0; i < NUM_BENCH_PHASES; ++i ) { gBenchTotalPhaseUs[i] += gBenchPhaseUs[i]; gBenchPhaseUs[i] = 0; }
		// Write every line early on (so a crash still leaves data), then batch to avoid stalls
		if( gBenchSeconds <= 20 || gBenchSeconds % 10 == 0 ) BenchmarkFlushBuffer();
		if( gBenchSeconds >= gBenchDuration )
		{
			// Done: write the average and stop (gBenchChecked stays set until the ROM closes)
			BenchmarkWriteAverage();
			if( FILE * log = fopen( "bench.log", "a" ) ) { fprintf( log, "# finished\n" ); fclose( log ); }
			gBenchEnabled = false;
		}
		gBenchLast = now;
		gBenchLastVI = vi;
		gBenchFlips = 0;
		gBenchRendered = 0;
	}

	void BenchmarkWriteAverage()
	{
		if( gBenchEnabled ) BenchmarkFlushBuffer();
		if( gBenchEnabled && gBenchSeconds > 0 )
		{
			u64 now; NTiming::GetPreciseTime( &now );
			const f32 secs = f32( now - gBenchStart ) / f32( gBenchFreq );
			const f32 hz = f32( FramerateLimiter_GetTvFrequencyHz() );
			const f32 vi_per_sec = f32( CPU_GetVerticalInterruptCount() - gBenchStartVI ) / secs;
			if( FILE * log = fopen( "bench.log", "a" ) )
			{
				fprintf( log, "# average over %.0fs: vi/s %.1f, speed %.1f%%, flips/s %.1f, rendered/s %.1f\n",
					secs, vi_per_sec, 100.0f * vi_per_sec / hz, f32( gBenchTotalFlips ) / secs, f32( gBenchTotalRendered ) / secs );
				f32 tot[NUM_BENCH_PHASES]; f32 other = 100.0f;
				for( u32 i = 0; i < NUM_BENCH_PHASES; ++i ) { tot[i] = f32( gBenchTotalPhaseUs[i] ) / ( secs * 10000.0f ); if( i <= BENCH_LIMIT ) other -= tot[i]; }
				fprintf( log, "# time split: gfx %.1f%%, audio %.1f%%, present %.1f%%, limiter %.1f%%, cpu+other %.1f%%\n",
					tot[BENCH_GFX], tot[BENCH_AUDIO], tot[BENCH_PRESENT], tot[BENCH_LIMIT], other );
				fprintf( log, "# within cpu+other: rom reads %.1f%%, dynarec compile %.1f%%, PI DMA %.1f%% | rom chunk misses %u, dynarec cache flushes %u, partial invalidations %u\n",
					tot[BENCH_ROMREAD], tot[BENCH_COMPILE], tot[BENCH_DMA], gBenchTotalRomMisses, gBenchTotalFlushes, gBenchPartialInvalidations );
				fprintf( log, "# flush reasons: requested %u, fragment count %u, code buffer full %u, hot-trace map %u\n",
					gBenchFlushReasons[0], gBenchFlushReasons[1], gBenchFlushReasons[2], gBenchFlushReasons[3] );
				fclose( log );
			}
		}
	}

	void BenchmarkRomClosed()
	{
		BenchmarkWriteAverage();
		gBenchChecked = gBenchEnabled = false;
		gBenchSeconds = gBenchTotalFlips = gBenchTotalRendered = 0;
		gBenchFlips = gBenchRendered = 0;
	}
}

class CGraphicsPluginImpl : public CGraphicsPlugin
{
	public:
	virtual	~CGraphicsPluginImpl();

		virtual 	bool		Initialise();
		virtual bool		StartEmulation()		{ return true; }
		virtual void		ViStatusChanged()		{}
		virtual void		ViWidthChanged()		{}
		virtual void		ProcessDList();

		virtual void		UpdateScreen();

		virtual void		RomClosed();
};

CGraphicsPluginImpl::~CGraphicsPluginImpl()
{
}


CGraphicsPlugin::~CGraphicsPlugin()
{
}


bool CGraphicsPluginImpl::Initialise()
{
	if(!CreateRenderer())
	{
		return false;
	}

	if(!CTextureCache::Create())
	{
		return false;
	}

	if (!DLParser_Initialise())
	{
		return false;
	}

	return true;
}

#if defined(DAEDALUS_AUTOLOAD) && defined(DAEDALUS_DEBUG_DISPLAYLIST)
bool gAutoloadDumpDisplayList = false;	// set by the DLDUMP command in autoload_input.txt
#endif

void CGraphicsPluginImpl::ProcessDList()
{
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
#ifdef DAEDALUS_AUTOLOAD
	if (gAutoloadDumpDisplayList)
	{
		gAutoloadDumpDisplayList = false;
		DLDebugOutput * output = DLDebug_CreateFileOutput();
		DLParser_Process(kUnlimitedInstructionCount, output);
		delete output;
		return;
	}
#endif
	if (!DLDebugger_Process())
	{
		DLParser_Process();
	}
#else
	DLParser_Process();
#endif
}

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
extern u32 gNumInstructionsExecuted;
extern u32 gNumDListsCulled;
extern u32 gNumRectsClipped;
#endif

void CGraphicsPluginImpl::UpdateScreen()
{
	//gVblCount++;

	static u32		last_origin = 0;
	u32 current_origin = Memory_VI_GetRegister(VI_ORIGIN_REG);
	static bool Old_FrameskipActive = false;
	static bool Older_FrameskipActive =false;

	if( current_origin != last_origin )
	{
		//printf( "Flip (%08x, %08x)\n", current_origin, last_origin );
		if( gGlobalPreferences.DisplayFramerate )
			UpdateFramerate();

		BenchmarkFlip( !gFrameskipActive );

		const f32 Fsync = FramerateLimiter_GetSync();

		//Calc sync rates for audio and game speed //Corn
		const f32 inv_Fsync = 1.0f / Fsync;
		gSoundSync = (u32)(44100.0f * inv_Fsync);
		gVISyncRate = (u32)(1500.0f * inv_Fsync);
		if( gVISyncRate > 4000 ) gVISyncRate = 4000;
		else if ( gVISyncRate < 1500 ) gVISyncRate = 1500;

		if(!gFrameskipActive)
		{
			if( gGlobalPreferences.DisplayFramerate )
			{
				pspDebugScreenSetTextColor( 0xffffffff );
				pspDebugScreenSetBackColor(0);
				pspDebugScreenSetXY(0, 0);

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
				pspDebugScreenPrintf( "Dlist[%d] Cull[%d] | Tris[%d] Cull[%d] | Rect[%d] Clip[%d] ", gNumInstructionsExecuted, gNumDListsCulled, gRenderer->GetNumTrisRendered(), gRenderer->GetNumTrisClipped(), gRenderer->GetNumRect(), gNumRectsClipped);
#else
				pspDebugScreenPrintf( "FPS[%#.1f] VB[%d/%d] Sync[%#.1f%%]   ", gCurrentFramerate, u32( Fsync * f32( FramerateLimiter_GetTvFrequencyHz() ) ), FramerateLimiter_GetTvFrequencyHz(), Fsync * 100.0f );
#endif
			}
			if( gGlobalPreferences.BatteryWarning )
			{
				battery_warning();
			}
			if(gTakeScreenshot)
			{
				CGraphicsContext::Get()->DumpNextScreen();
				gTakeScreenshot = false;
			}

			{ BENCH_SCOPE( BENCH_PRESENT ); CGraphicsContext::Get()->UpdateFrame( false ); }
			HandleEndOfFrame();
		}

		static u32 current_frame = 0;
		current_frame++;


		Older_FrameskipActive = Old_FrameskipActive;
		Old_FrameskipActive = gFrameskipActive;

		switch(gFrameskipValue)
		{
		case FV_DISABLED:
			gFrameskipActive = false;
			break;
		case FV_AUTO1:
			if(!Old_FrameskipActive && (Fsync < 0.965f)) gFrameskipActive = true;
			else gFrameskipActive = false;
			break;
		case FV_AUTO2:
			if((!Old_FrameskipActive | !Older_FrameskipActive) && (Fsync < 0.965f)) gFrameskipActive = true;
			else gFrameskipActive = false;
			break;
		default:
			gFrameskipActive = (current_frame % (gFrameskipValue - 1)) != 0;
			break;
		}

		last_origin = current_origin;
	}
}

void CGraphicsPluginImpl::RomClosed()
{
	#ifdef DAEDALUS_DEBUG_CONSOLE
	DBGConsole_Msg(0, "Finalising PSPGraphics");
	#endif
	BenchmarkRomClosed();
	DLParser_Finalise();
	CTextureCache::Destroy();
	DestroyRenderer();
}

class std::unique_ptr<CGraphicsPlugin>	CreateGraphicsPlugin()
{
	DBGConsole_Msg( 0, "Initialising PSP Graphics Plugin" );
	auto plugin = std::make_unique<CGraphicsPluginImpl>();
	if (!plugin->Initialise())
	{
		plugin = nullptr;
	}

	return plugin;
}