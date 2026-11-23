/*
Copyright (C) 2006,2007 StrmnNrmn

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

#include <pspdebug.h>
#include <stdlib.h>


#include <pspctrl.h>
#include <psprtc.h>
#include <psppower.h>
#include <pspsdk.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspge.h>
#include <pspfpu.h>
#include <pspkernel.h>
#include <kubridge.h>
#include <pspsysmem.h>

#include "Interface/ConfigOptions.h"
#include "Interface/Cheats.h"
#include "Core/CPU.h"
#include "Core/CPU.h"
#include "Core/Memory.h"
#include "Ultra/ultra_rcp.h"
#include "Core/PIF.h"
#include "Ultra/ultra_R4300.h"
#include "RomFile/RomSettings.h"
#include "RomFile/RomFileMemory.h"
#include "Core/Save.h"
#include "Debug/DBGConsole.h"
#include "Debug/DebugLog.h"
#include "Graphics/GraphicsContext.h"
#include "HLEGraphics/DLParser.h"
#include "HLEGraphics/DisplayListDebugger.h"
#include "HLEGraphics/TextureCache.h"
#include "Input/InputManager.h"
#include "Interface/RomDB.h"
#include "UI/DrawText.h"
#include "UI/MainMenuScreen.h"
#include "UI/PauseScreen.h"
#include "UI/SplashScreen.h"
#include "UI/UIContext.h"
#include <iostream>
#include <cstring>
#include <malloc.h>
#include <unistd.h>
#include <cstdio>

#include "System/SystemInit.h"
#include "Utility/BatchTest.h"

#include "Utility/ModulePSP.h"
#include "Interface/Preferences.h"
#include "Utility/Profiler.h"
#include "System/Thread/Thread.h"
#include "Utility/Translate.h"
#include "Utility/Timer.h"


#include <filesystem>

#ifdef DAEDALUS_SDL
#include <SDL2/SDL.h>
#else

PSP_MODULE_INFO( DaedalusX64 1.1.9a, 0, 1, 1 );
PSP_MAIN_THREAD_ATTR( PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU );
#endif


extern "C"
{
	/* Video Manager functions */
	int pspDveMgrCheckVideoOut();
	int pspDveMgrSetVideoOut(int, int, int, int, int, int, int);
}

/* Kernel Exception Handler functions */

extern void VolatileMemInit();

/* Video Manager functions */
extern int HAVE_DVE;
extern int PSP_TV_CABLE;
extern int PSP_TV_LACED;

bool g32bitColorMode = false; // We might be able to ditch this soon. Phat exclusive.
bool PSP_IS_SLIM = false;

PSP_HEAP_SIZE_KB(-256);

#ifdef DAEDALUS_STARTUP_LOG
#include <cstdarg>
extern "C" void StartupLog( const char * fmt, ... )
{
	if( FILE * f = fopen( "startup.log", "a" ) )
	{
		va_list args; va_start( args, fmt ); vfprintf( f, fmt, args ); va_end( args );
		fputc( '\n', f );
		fclose( f );
	}
}
#define SLOG(...) StartupLog(__VA_ARGS__)
#else
#define SLOG(...) do {} while(0)
#endif

// HOME button: without a registered exit callback the PSP can't quit homebrew from the
// HOME menu (upstream never registered one).
static int ExitCallback( int, int, void * )
{
	sceKernelExitGame();
	return 0;
}

static int CallbackThread( SceSize, void * )
{
	int cbid = sceKernelCreateCallback( "Exit Callback", ExitCallback, nullptr );
	sceKernelRegisterExitCallback( cbid );
	sceKernelSleepThreadCB();
	return 0;
}

static void SetupExitCallback()
{
	int thid = sceKernelCreateThread( "daedalus_exit_cb", CallbackThread, 0x11, 0xFA0, PSP_THREAD_ATTR_USER, nullptr );
	if( thid >= 0 )
		sceKernelStartThread( thid, 0, nullptr );
}

#ifdef DAEDALUS_STARTUP_LOG
// Diagnostics: log emulator state every 2 seconds from a high-priority thread, so a hang or
// crash (when no frames are drawn and bench.log stops) still leaves a trail.
extern u32 Dynamo_GetFragmentCount();
#include "Utility/BenchTimers.h"
static int WatchdogThread( SceSize, void * )
{
	u32 tick = 0;
	for(;;)
	{
		sceKernelDelayThread( 2000000 );
		tick += 2;
		if( FILE * f = fopen( "watchdog.log", "a" ) )
		{
			fprintf( f, "t=%us pc=%08x vi=%u count=%08x stufftodo=%08x free=%uKB fragments=%u partial=%u flushes(req/count/buf/hot)=%u/%u/%u/%u\n",
				tick, gCPUState.CurrentPC, CPU_GetVerticalInterruptCount(), gCPUState.CPUControl[C0_COUNT]._u32, gCPUState.GetStuffToDo(),
				PSP_GetLargestFreeBlock() / 1024, Dynamo_GetFragmentCount(), gBenchPartialInvalidations,
				gBenchFlushReasons[0], gBenchFlushReasons[1], gBenchFlushReasons[2], gBenchFlushReasons[3] );
			fclose( f );
		}
	}
	return 0;
}
static void StartWatchdog()
{
	remove( "watchdog.log" );
	int thid = sceKernelCreateThread( "daedalus_watchdog", WatchdogThread, 0x10, 0x2000, PSP_THREAD_ATTR_USER, nullptr );
	if( thid >= 0 ) sceKernelStartThread( thid, 0, nullptr );
}
#endif

static bool	Initialize()
{
	SLOG("Initialize: start");
#ifdef DAEDALUS_STARTUP_LOG
	{
		struct mallinfo mi = mallinfo();
		const u32 largest = PSP_GetLargestFreeBlock();
		SLOG("memory: heap used %uKB, largest free %uKB, heap end ~%p (0x0A000000 = 24MB layout, 0x0C000000 = 64MB layout)",
			(u32)mi.uordblks / 1024, largest / 1024, (u8*)sbrk( 0 ) + largest );
	}
#endif
	SetupExitCallback();
#ifdef DAEDALUS_STARTUP_LOG
	StartWatchdog();
#endif
	{ int r = scePowerSetClockFrequency(333, 333, 166); SLOG("clock 333 -> %08x", r); }
	pspFpuSetEnable(0); // Disable FPU Exceptions
	VolatileMemInit();
	SLOG("VolatileMemInit done");
	pspDebugScreenInit();
	sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

	#ifdef DAEDALUS_SDL
		if( SDL_Init( SDL_INIT_AUDIO ) < 0 )
	{
		printf( "SDL could not initialize! SDL Error: %s\n", SDL_GetError() );
		return false;
	}
	#endif

	// Detect PSP greater than PSP 1000
	int model = kuKernelGetModel();
	SLOG("kuKernelGetModel -> %d", model);
	bool is_slim = model > 0;
#ifdef DAEDALUS_AUTOLOAD
	// Testing aid: PPSSPP does not implement kubridge, so allow forcing the Slim/Go code path.
	if( FILE * f = fopen( "force_slim.txt", "r" ) ) { fclose( f ); is_slim = true; }
#endif
	if ( is_slim )
	{
		std::cout << "PSP is Slim or higher" << std::endl;
		// Can't use extra memory if ME isn't available
		PSP_IS_SLIM = true;
		g32bitColorMode = true;
		sceGeEdramSetSize(4*1024*1024);
		SLOG("edram set");
		HAVE_DVE = CModule::Load("Plugins/dvemgr.prx");
		SLOG("dvemgr load -> %08x", HAVE_DVE);
		if (HAVE_DVE >= 0)
			PSP_TV_CABLE = pspDveMgrCheckVideoOut();
		if (PSP_TV_CABLE == 1)
			PSP_TV_LACED = 1; // composite cable => interlaced
		else if( PSP_TV_CABLE == 0 )
			CModule::Unload( HAVE_DVE );	// Stop and unload dvemgr.prx since if no video cable is connected
	}

// This Breaks gdb, better disable it in debug build
#ifdef DAEDALUS_DEBUG_CONSOLE
extern void initExceptionHandler();
	initExceptionHandler();
#endif

	HAVE_DVE = (HAVE_DVE < 0) ? 0 : 1; // 0 == no dvemgr, 1 == dvemgr


	SLOG("System_Init start");
	if (!System_Init())
	{
		SLOG("System_Init FAILED");
		return false;
	}
	SLOG("System_Init ok");
	return true;
}


#ifdef DAEDALUS_PROFILE_EXECUTION
static CTimer		gTimer;
#endif

void HandleEndOfFrame()
{
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	if(DLDebugger_IsDebugging())
		return;
			DPF( DEBUG_FRAME, "********************************************" );
#endif

// How long did the last frame take?
#ifdef DAEDALUS_PROFILE_EXECUTION
	DumpDynarecStats( elapsed_time );
#endif

	//Enter debug menu as soon as select is pressed
	static u32 oldButtons = 0;
	SceCtrlData pad;
	bool		activate_pause_menu = false;
	sceCtrlPeekBufferPositive(&pad, 1);

	// If KernelButtons.prx not found. Use select for pause instead
	if(oldButtons != pad.Buttons)
	{
		// if( gCheatsEnabled && (pad.Buttons & PSP_CTRL_SELECT) )
		// {
		// 	CheatCodes_Activate( GS_BUTTON );
		// }

		if(pad.Buttons & PSP_CTRL_SELECT)
				activate_pause_menu = true;
	}

	if(activate_pause_menu)
	{

		CGraphicsContext::Get()->SwitchToLcdDisplay();
		CGraphicsContext::Get()->ClearAllSurfaces();

		auto	p_context = CUIContext::Create();

		if(p_context != NULL)
		{
			auto	pause = CPauseScreen::Create( p_context );
			pause->Run();
			delete p_context;
		}


		// Commit the preferences database before starting to run
		// CPreferences::Get()->Commit();
	}

	//	Reset the elapsed time to avoid glitches when we restart
	#ifdef DAEDALUS_PROFILE_EXECUTION
	gTimer.Reset();
	#endif

}


#ifdef DAEDALUS_AUTOLOAD
bool gAutoloadForceInterpreter = false;
int gAutoloadAudioMode = -1;
static u32 gAutoloadVbls = 0;
static u64 gAutoloadStartTick = 0;
static int AutoloadWatchdog(SceSize, void *)
{
	for(;;)
	{
		sceKernelDelayThread( 2000000 );
		if( FILE * f = fopen( "autoload.log", "a" ) )
		{
			fprintf( f, "wd pc=%08x count=%08x vis=%u\n", gCPUState.CurrentPC, gCPUState.CPUControl[C0_COUNT]._u32, gAutoloadVbls );
			fclose( f );
		}
	}
	return 0;
}
static void AutoloadDumpFramebuffer()
{
	// Write the VI framebuffer straight from RDRAM (independent of HLE graphics).
	u32 status = Memory_VI_GetRegister( VI_STATUS_REG );
	u32 origin = Memory_VI_GetRegister( VI_ORIGIN_REG ) & 0x00FFFFFF;
	u32 width  = Memory_VI_GetRegister( VI_WIDTH_REG ) & 0xFFF;
	u32 type   = status & 3;
	if( width == 0 || width > 1024 || type < 2 ) return;
	u32 bpp    = type == 2 ? 2 : 4;
	u32 height = width <= 320 ? 240 : 480;
	if( origin + width * height * bpp > gRamSize ) return;
	FILE * f = fopen( "fb.ppm", "wb" );
	if( !f ) return;
	fprintf( f, "P6 %u %u 255\n", width, height );
	static u8 row[1024 * 3];
	const u8 * ram = g_pu8RamBase;
	for( u32 y = 0; y < height; ++y )
	{
		for( u32 x = 0; x < width; ++x )
		{
			u32 a = origin + (y * width + x) * bpp;
			u8 r, g, b;
			if( bpp == 2 )
			{
				u16 p = *(const u16 *)(ram + (a ^ 2));
				r = ((p >> 11) & 0x1f) << 3; g = ((p >> 6) & 0x1f) << 3; b = ((p >> 1) & 0x1f) << 3;
			}
			else
			{
				u32 p = *(const u32 *)(ram + a);
				r = p >> 24; g = p >> 16; b = p >> 8;
			}
			row[x*3] = r; row[x*3+1] = g; row[x*3+2] = b;
		}
		fwrite( row, 3, width, f );
	}
	fclose( f );
}
static void AutoloadVblCallback(void *)
{
	AutoloadDumpFramebuffer();
	// Once per 60 VIs, log progress so headless test runs can be inspected.
	gAutoloadVbls += 64;
	u64 now; sceRtcGetCurrentTick( &now );
	if( FILE * f = fopen( "autoload.log", "a" ) )
	{
		fprintf( f, "vi=%u heap_used=%uKB pc=%08x host_ms=%u vi_status=%08x origin=%08x width=%u\n", gAutoloadVbls, (u32)(mallinfo().uordblks / 1024), gCPUState.CurrentPC, (u32)((now - gAutoloadStartTick) / 1000), Memory_VI_GetRegister( VI_STATUS_REG ), Memory_VI_GetRegister( VI_ORIGIN_REG ), Memory_VI_GetRegister( VI_WIDTH_REG ) );
		fclose( f );
	}
}
#endif

int main(int argc, char* argv[])
{
	SLOG("main: argc=%d", argc);
	if( Initialize() )
	{
#ifdef DAEDALUS_BATCH_TEST_ENABLED
		if( argc > 1 )
		{
			BatchTestMain( argc, argv );
		}
#else
		//Makes it possible to load a ROM directly without using the GUI
		//There are no checks for wrong file name so be careful!!!
		//Ex. from PSPLink -> ./Daedalus.prx "Roms/StarFox 64.v64" //Corn
#ifdef DAEDALUS_AUTOLOAD
		// Testing aid: boot the ROM path listed in autoload.txt, skipping the UI.
		static char autoload_path[512];
		if( FILE * af = fopen( "autoload.txt", "r" ) )
		{
			if( fgets( autoload_path, sizeof(autoload_path), af ) )
			{
				autoload_path[strcspn( autoload_path, "\r\n" )] = 0;
				if( autoload_path[0] )
				{
					argv[1] = autoload_path;
					argc = 2;
				}
			}
			// Optional second line "interp" disables the dynarec; third line "audio" enables
			// synchronous HLE audio (off by default on PSP).
			char mode[32];
			if( fgets( mode, sizeof(mode), af ) && strncmp( mode, "interp", 6 ) == 0 )
				gAutoloadForceInterpreter = true;
			if( fgets( mode, sizeof(mode), af ) && strncmp( mode, "audio", 5 ) == 0 )
				gAutoloadAudioMode = 2;	// APM_ENABLED_SYNC
			fclose( af );
		}
#endif
		if( argc > 1 )
		{
			printf("Loading %s\n", argv[1] );
#ifdef DAEDALUS_AUTOLOAD
			remove( "autoload.log" );
			if( FILE * f = fopen( "autoload.log", "a" ) ) { fprintf( f, "before_open heap_used=%uKB\n", (u32)(mallinfo().uordblks / 1024) ); fclose( f ); }
			sceRtcGetCurrentTick( &gAutoloadStartTick );
			bool opened = System_Open( argv[1] );
			if( FILE * f = fopen( "autoload.log", "a" ) ) { fprintf( f, "open %s -> %d dynarec=%d free_at_heap_init=%uKB largest_malloc_now=%uKB rom_heap=%uKB\n", argv[1], opened, gDynarecEnabled, gRomHeapLargestFreeAtInit / 1024, PSP_GetLargestFreeBlock() / 1024, CROMFileMemory::Get()->GetCapacity() / 1024 ); fprintf( f, "settings name=%s savetype=%s countperop=%u\n", g_ROM.settings.GameName.c_str(), ROM_GetSaveTypeName( g_ROM.settings.SaveType ), gCountPerOp ); fclose( f ); }
			CPU_RegisterVblCallback( &AutoloadVblCallback, nullptr );
			SceUID wd = sceKernelCreateThread( "autoload_wd", AutoloadWatchdog, 0x10, 0x4000, PSP_THREAD_ATTR_USER, nullptr );
			if( wd >= 0 ) sceKernelStartThread( wd, 0, nullptr );
#else
			System_Open( argv[1] );
#endif
			CPU_Run();
			System_Close();
			System_Finalize();
			return 0;
		}
#endif
		SLOG("Translate_Init");
		Translate_Init();
		bool show_splash = true;
		for(;;)
		{
			SLOG("DisplayRomsAndChoose");
			DisplayRomsAndChoose( show_splash );
			SLOG("ROM chosen, CPU_Run");
			show_splash = false;

			CRomDB::Get()->Commit();
			CPreferences::Get()->Commit();

			CPU_Run();
			System_Close();
		}

		System_Finalize();
	}

	return 0;
}
