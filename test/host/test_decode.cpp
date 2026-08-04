// Host-tier decode tests — no hardware, no meter.
//
// These pin the three properties of dlms_parser that gPlug-mini's decoding
// depends on. Each was measured against a published capture; each would go
// unnoticed if a library upgrade changed it, and each fails in a way that looks
// like a working meter rather than a broken one.
//
// Run one case per invocation so CTest reports them separately:
//     test_decode <case> <fixture>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dlms_parser/dlms_parser.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

std::vector<uint8_t> read_hex(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = std::fopen(path, "r");
  if (!f) { std::perror(path); std::exit(2); }
  unsigned v;
  while (std::fscanf(f, "%2x", &v) == 1) bytes.push_back(static_cast<uint8_t>(v));
  std::fclose(f);
  return bytes;
}

struct Value {
  std::string obis;
  std::string text;
  unsigned long long raw{ 0 };
  int scaler{ 0 };
  bool numeric{ false };
};

std::vector<Value> decode(const char* fixture) {
  std::vector<uint8_t> bytes = read_hex(fixture);   // parse() takes a mutable span
  static std::vector<Value> out;          // static: the callback is a plain function
  out.clear();

  dlms_parser::DlmsParser parser(
      [](const dlms_parser::AxdrCapture& c) {
        std::array<char, 32> obis_buf;
        const std::string_view obis = c.obis.to_string(obis_buf);
        Value v;
        v.obis.assign(obis.data(), obis.size());
        v.numeric = c.is_numeric();
        if (v.numeric) {
          for (auto b : c.value) v.raw = (v.raw << 8) | b;
          v.scaler = c.has_scaler_unit ? c.scaler : 0;
        } else {
          std::array<char, 128> buf;
          const std::string_view s = c.value_as_string(buf);
          v.text.assign(s.data(), s.size());
        }
        out.push_back(v);
      },
      nullptr);
  parser.load_default_patterns();
  parser.parse({ bytes.data(), bytes.size() });
  return out;
}

const Value* find(const std::vector<Value>& vs, const char* obis) {
  for (const auto& v : vs) if (v.obis == obis) return &v;
  return nullptr;
}

// The meter's identity does not sit at one fixed OBIS code. One configuration
// publishes it as the COSEM logical device name, the other as device ID 1, and
// the two carry different lengths. Look in both, in this order.
const Value* find_identity(const std::vector<Value>& values) {
  for (const char* obis : { "0.0.42.0.0.255", "0.0.96.1.0.255" }) {
    const Value* v = find(values, obis);
    if (v && !v->text.empty()) return v;
  }
  return nullptr;
}

// TS-HOST-01 — FR-MTR-05. Both serial forms are real, from the same meter model.
// A decoder that assumes one length, or one OBIS code, is wrong half the time —
// and wrong by finding nothing, which reads as a quiet meter rather than a bug.
void serial_length(const char* fixture, size_t expected_len, const char* expected) {
  const auto values = decode(fixture);
  const Value* serial = find_identity(values);
  check(serial != nullptr, "meter identity decoded from one of the two OBIS codes");
  if (!serial) return;
  printf("       %s = '%s' (%zu characters)\n",
         serial->obis.c_str(), serial->text.c_str(), serial->text.size());
  check(serial->text.size() == expected_len, "serial has the length this fixture carries");
  check(serial->text == expected, "serial matches the published value exactly");
}

// TS-HOST-02 — FR-AGG-01. The 8-character serial straddles a General Block
// Transfer boundary: '4433' ends one block and '7811' begins the next. Reading
// it whole is proof the library reassembled; a frame-by-frame decoder returns
// '4433' and reports no error at all.
void block_reassembly(const char* fixture) {
  const auto values = decode(fixture);
  const Value* serial = find_identity(values);
  check(serial != nullptr, "serial decoded from a value split across two blocks");
  if (!serial) return;
  check(serial->text == "44337811", "both halves present — '4433' alone means no reassembly");
}

// TS-HOST-03 — FR-DEC-03. The library hands over the meter's scaler beside the
// raw bytes and does not apply it behind the caller's back, so scaling happens
// exactly once: in whoever reads it.
void scaling_applied_once(const char* fixture) {
  const auto values = decode(fixture);
  const Value* power = find(values, "1.0.1.7.0.255");
  check(power != nullptr, "active power+ decoded");
  if (!power) return;
  printf("       raw = %llu, scaler = %d\n", power->raw, power->scaler);
  check(power->raw == 777, "raw value is the integer the meter sent, unscaled");
  check(power->scaler == 0, "this meter sends scaler 0 — raw and scaled coincide");
}

// TS-HOST-04 — FR-DEC-04. float has a 24-bit mantissa and is exact only to
// 16,777,216. A lifetime energy total in Wh passes that at 16.8 MWh, and Home
// Assistant derives consumption from differences between totals — so a total
// that quantises invents consumption that never happened.
void energy_needs_integers(const char* fixture) {
  const auto values = decode(fixture);
  const Value* energy = find(values, "1.1.1.8.0.255");
  check(energy != nullptr, "cumulative energy register decoded");
  if (!energy) return;

  const unsigned long long exact = energy->raw;
  const float as_float = static_cast<float>(exact);
  printf("       raw = %llu, as float = %.0f\n", exact, static_cast<double>(as_float));

  check(exact == 25149419ULL, "raw integer is exact");
  check(exact > (1ULL << 24), "fixture exercises the range where float stops being exact");
  // Not asserting that float is lossy for its own sake: this is the assumption
  // FR-DEC-04 rests on. If it ever stops holding, this fails and we revisit.
  check(static_cast<unsigned long long>(as_float) != exact,
        "float loses this value — cumulative registers must stay integers");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: test_decode <case> <fixture>\n"); return 2; }
  const std::string name = argv[1];
  const char* fixture = argv[2];
  printf("%s (%s)\n", name.c_str(), fixture);

  if (name == "serial16")        serial_length(fixture, 16, "LGZ1030655933512");
  else if (name == "serial8")    serial_length(fixture, 8, "44337811");
  else if (name == "reassembly") block_reassembly(fixture);
  else if (name == "scaling")    scaling_applied_once(fixture);
  else if (name == "energy")     energy_needs_integers(fixture);
  else { std::fprintf(stderr, "unknown case: %s\n", name.c_str()); return 2; }

  printf("%s: %d failure(s)\n", name.c_str(), failures);
  return failures == 0 ? 0 : 1;
}
