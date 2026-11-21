// Lightweight wall-clock accounting for bench.log (PSP only).
// Each phase accumulates microseconds; the benchmark logger reports and resets them per second.
#pragma once

#include "Base/Types.h"

enum EBenchPhase
{
	BENCH_GFX = 0,		// RSP graphics task: display list processing (HLE graphics on the CPU)
	BENCH_AUDIO,		// RSP audio task on the main CPU
	BENCH_PRESENT,		// UpdateFrame: waiting for the GE to finish and flipping
	BENCH_LIMIT,		// Frame limiter / vsync sleep
	NUM_BENCH_PHASES
};

extern u64 gBenchPhaseUs[NUM_BENCH_PHASES];

#ifdef DAEDALUS_PSP
#include <pspkernel.h>
struct CBenchScope
{
	EBenchPhase mPhase;
	u64 mStart;
	explicit CBenchScope( EBenchPhase phase ) : mPhase( phase ), mStart( sceKernelGetSystemTimeWide() ) {}
	~CBenchScope() { gBenchPhaseUs[mPhase] += sceKernelGetSystemTimeWide() - mStart; }
};
#define BENCH_SCOPE( phase ) CBenchScope _bench_scope_##phase( phase )
#else
#define BENCH_SCOPE( phase ) do {} while(0)
#endif
