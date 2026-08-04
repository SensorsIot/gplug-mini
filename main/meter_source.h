// The one seam between the production and simulated builds (FR-BLD-03).
//
// Everything above this interface is the same code in both binaries, which is
// what makes a bench run against the simulated build evidence about the
// production one. Selected by Kconfig, never at runtime.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gplug {

// Starts whichever source this build was compiled with.
void meter_source_start();

// Reads up to `max` bytes, returning how many arrived. Non-blocking beyond the
// timeout: the caller needs to see gaps, because a gap is what ends a cycle.
size_t meter_source_read(uint8_t* out, size_t max, uint32_t timeout_ms);

// What this build actually is, for the boot banner. Never used to choose
// behaviour — that would put a runtime branch on the seam.
const char* meter_source_name();

// Advance to the next candidate line configuration after a burst that decoded
// to nothing. False when the source has no line settings. See meter_uart.cpp
// for why this is a probe rather than a constant.
bool meter_source_try_next_line();

}  // namespace gplug
