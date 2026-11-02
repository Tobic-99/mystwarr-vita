// Function-entry trace of the recompiled 68000 (only in -DMW_TRACE builds; see runtime/*_rt.h).
#pragma once
#include <cstdint>

namespace mwtrace {
void enter(int cpu, uint32_t addr);   // cpu 0 = ARM, 1 = 68000
void dump(int cpu, int n);            // print the last n functions entered by cpu
}
