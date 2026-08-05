// The LED, driven from the supervisor state (FR-LED-01..05).
//
// The patterns are FSD §11.3's table and nothing else. They are transcribed
// here so a host test can read them against the spec; inventing a plausible set
// instead produces an indicator that is self-consistent and wrong, and a test
// written from the code then asserts the invention.
//
// One owner. No other code sets a pin: two writers make a pattern that is
// neither, and the symptom is an LED that looks almost right, which nobody
// reports.
#pragma once

#include <cstdint>

namespace gplug {

enum class Indication {
  Boot,           // red -> green -> blue, 500 ms each — a one-shot sequence
  Provisioning,   // blue steady
  Connecting,     // red blink, 5 s period
  Linked,         // red blink, 1 s period
  Operational,    // off; green pulse 100 ms per publication
  Updating,       // alternating blue/green, 200 ms
};

// Two phases and a period. A steady pattern has period 0 and uses phase A; a
// blink is phase A against darkness; an alternation is two lit phases.
//
// One shape for all three because §11.3 contains all three, and a struct that
// could only express blinking would have quietly turned Updating into one.
struct Pattern {
  bool a_red, a_green, a_blue;
  bool b_red, b_green, b_blue;
  uint16_t period_ms;   // full cycle; each phase runs for half
};

constexpr Pattern pattern_for(Indication i) {
  switch (i) {
    case Indication::Boot:
      return { true, false, false, false, false, true, 1500 };
    case Indication::Provisioning:
      return { false, false, true, false, false, true, 0 };
    // 5 s and 1 s. The two red blinks differ only in rate, which is why the
    // rate is the assertion — a table edit that made them equal would leave two
    // states genuinely indistinguishable on the board.
    case Indication::Connecting:
      return { true, false, false, false, false, false, 5000 };
    case Indication::Linked:
      return { true, false, false, false, false, false, 1000 };
    case Indication::Operational:
      return { false, false, false, false, false, false, 0 };
    case Indication::Updating:
      return { false, false, true, false, true, false, 200 };
  }
  return { false, false, false, false, false, false, 0 };
}

// §11.3: OPERATIONAL is dark except for a pulse per publication.
constexpr uint16_t PUBLISH_PULSE_MS = 100;

// FR-LED-05's prohibited outcome is "two states indistinguishable". That is a
// property of the table, so it is checked where the table is edited.
constexpr bool distinguishable(Indication a, Indication b) {
  if (a == b) return true;
  const Pattern x = pattern_for(a), y = pattern_for(b);
  return x.a_red != y.a_red || x.a_green != y.a_green || x.a_blue != y.a_blue ||
         x.b_red != y.b_red || x.b_green != y.b_green || x.b_blue != y.b_blue ||
         x.period_ms != y.period_ms;
}

#ifndef GPLUG_HOST_TEST
// Starts the indicator task and runs the BOOT sequence. Idempotent.
void indicator_start();

void indicate(Indication i);

// FR-LED-02 — one green pulse per published set. A pulse rather than a state:
// publishing is an event, and a device that stopped an hour ago would otherwise
// look identical to one publishing now.
void indicate_publish();
#endif

}  // namespace gplug
