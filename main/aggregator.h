// Cycle-boundary arithmetic — a pure core (FR-AGG-01).
//
// No clock read, no I/O, no static state: the caller passes the timestamps in.
// That is what lets the 2000 ms rule be tested at host tier in microseconds
// instead of by waiting two seconds on a bench.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

// One cycle's decoded values, assembled into the state message.
//
// A pure core with no clock, no broker and no logging, so the three rules that
// govern a measurement set can be tested at host tier in microseconds. They are
// easy to get wrong in ways that look right on a bench: a duplicated register
// and an empty set both produce a message that arrives and parses.
class CycleSet {
 public:
  // FR-AGG-04 — the first occurrence of a register wins.
  //
  // A meter may repeat a register within one cycle, and the interface spec says
  // the first is the reading for that cycle. Overwriting is the tempting
  // implementation and it is wrong in a way nothing downstream can detect: both
  // values are plausible, so a wrong one is indistinguishable from a right one.
  // Returns false when the label was already present and nothing was stored.
  bool add_integer(const char* label, uint64_t value) {
    if (!claim(label)) return false;
    append("%llu", static_cast<unsigned long long>(value));
    return true;
  }

  bool add_real(const char* label, double value) {
    if (!claim(label)) return false;
    append("%.3f", value);
    return true;
  }

  // FR-AGG-06 — a set with no decoded values is discarded, never published.
  // An all-zero or empty payload is worse than silence: Home Assistant records
  // it as a reading, and a zeroed cumulative register reads as consumption.
  bool empty() const { return count_ == 0; }

  size_t size() const { return count_; }

  // The state message. Valid until the next clear().
  //
  // Reading it closes the set: the JSON is finished, so a later value has
  // nowhere to go and every add is refused from here. Both refusals return
  // false for the same reason the duplicate rule does — the caller checks one
  // return value, not two states.
  const char* json() {
    if (count_ == 0) return "";
    if (!closed_) {
      if (len_ + 2 < sizeof(json_)) { json_[len_++] = '}'; json_[len_] = '\0'; }
      closed_ = true;
    }
    return json_;
  }

  void clear() {
    count_ = 0;
    len_ = 0;
    json_[0] = '\0';
    closed_ = false;
  }

 private:
  static constexpr size_t MAX_VALUES = 16;
  static constexpr size_t MAX_LABEL = 28;

  bool claim(const char* label) {
    if (closed_ || count_ >= MAX_VALUES) return false;
    for (size_t i = 0; i < count_; ++i) {
      if (std::strncmp(labels_[i], label, MAX_LABEL) == 0) return false;
    }
    const size_t n = std::strlen(label);
    std::memcpy(labels_[count_], label, n < MAX_LABEL ? n : MAX_LABEL - 1);
    labels_[count_][n < MAX_LABEL ? n : MAX_LABEL - 1] = '\0';
    append("%s\"%s\":", len_ ? "," : "{", label);
    ++count_;
    return true;
  }

  void append(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(json_ + len_, sizeof(json_) - len_, fmt, ap);
    va_end(ap);
    if (n > 0 && len_ + static_cast<size_t>(n) < sizeof(json_)) len_ += static_cast<size_t>(n);
  }

  char json_[512]{};
  char labels_[MAX_VALUES][MAX_LABEL]{};
  size_t count_{ 0 };
  size_t len_{ 0 };
  bool closed_{ false };
};

}  // namespace gplug
