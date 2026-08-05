// Frame-start arithmetic — a pure core (FR-MTR-06).
//
// No I/O and no state: the caller passes the bytes in. That is what lets the
// rule be tested at host tier against real captures rather than only on a bench
// that delivers one usable telegram in three.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gplug {

// How many leading bytes to drop before handing a buffer to the decoder.
//
// A closing flag and the next opening flag are adjacent on the wire, so `7E 7E`
// occurs at every frame boundary and a buffer may open on a flag that opens
// nothing. What distinguishes the two is the byte after: an HDLC type-3 frame
// carries a format byte of 0xA0..0xAF, so `7E` followed by an A0-class byte is
// a frame start and `7E` followed by anything else is not.
//
// Handing the decoder a buffer that begins on a bare flag costs the whole cycle
// — measured, 10 values become 0 — because the second flag is read as the
// format byte and the frame length taken from padding.
//
// **Only leading flags are skipped, never payload.** Seeking forward to the
// first complete frame looks like the more thorough fix and is worse: a cycle
// that opens inside a frame still yields values from the payload it carries,
// and skipping to the next flag throws those away. Measured on a 338-byte
// capture opened 200 bytes in — seeking left 63 bytes and decoded nothing where
// the untrimmed 138 decoded fine. The decoder resynchronises on its own; it
// only needs not to be handed a false frame start.
constexpr size_t leading_flags(const uint8_t* bytes, size_t len) {
  size_t i = 0;
  while (i + 1 < len && bytes[i] == 0x7E && (bytes[i + 1] & 0xF0) != 0xA0) ++i;
  return i;
}

}  // namespace gplug
