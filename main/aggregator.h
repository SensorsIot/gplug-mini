// Cycle-boundary arithmetic — a pure core (FR-AGG-01).
//
// No clock read, no I/O, no static state: the caller passes the timestamps in.
// That is what lets the 2000 ms rule be tested at host tier in microseconds
// instead of by waiting two seconds on a bench.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gplug {

// The meter is silent between transmissions. A gap of at least this long means
// the cycle that was arriving has finished (FSD D-T2).
constexpr uint32_t CYCLE_GAP_MS = 2000;

// True when the buffered bytes should now be handed to the decoder.
//
// The gap decides *when to parse*, not what a transmission contains: DLMS
// General Block Transfer marks its own last block, and that flag — not silence —
// is the authority on where a transmission ends. This only answers "has the
// meter stopped talking for long enough that whatever we hold is complete".
constexpr bool cycle_ended(uint32_t now_ms, uint32_t last_byte_ms, size_t buffered) {
  return buffered > 0 && (now_ms - last_byte_ms) >= CYCLE_GAP_MS;
}

}  // namespace gplug
