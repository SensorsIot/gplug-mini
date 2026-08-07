// Host-tier decode tests — no hardware, no meter.
//
// These pin the properties of dlms_parser that gPlug-mini's decoding depends
// on. Each is asserted against a published capture; each would go unnoticed if a
// library upgrade changed it, and each fails in a way that looks like a working
// meter rather than a broken one.
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

#include "framing.h"

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

std::vector<Value> decode_bytes(std::vector<uint8_t> bytes) {   // parse() takes a mutable span
  static std::vector<Value> out;          // static: the callback is a plain function
  out.clear();

  // The same trim the firmware applies before parsing (FR-MTR-06). It is in the
  // path of every case here on purpose: a fixture that only decodes when handed
  // over untrimmed would be a test of the library, not of what ships.
  const size_t skip = gplug::leading_flags(bytes.data(), bytes.size());
  if (skip) bytes.erase(bytes.begin(), bytes.begin() + static_cast<long>(skip));

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

std::vector<Value> decode(const char* fixture) {
  return decode_bytes(read_hex(fixture));
}

// Decode a capture as if reception began part-way through it. This is not an
// edge case: the device is energised by the meter it reads, so on every power
// cycle it wakes into the middle of a transmission and the opening frame is
// already gone (interface spec §4.1).
std::vector<Value> decode_from_offset(const char* fixture, size_t offset) {
  std::vector<uint8_t> bytes = read_hex(fixture);
  if (offset >= bytes.size()) {
    std::fprintf(stderr, "offset %zu past end of %zu-byte fixture\n", offset, bytes.size());
    std::exit(2);
  }
  bytes.erase(bytes.begin(), bytes.begin() + static_cast<long>(offset));
  return decode_bytes(std::move(bytes));
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

// TS-001 / TS-002 — FR-MTR-05. Both serial forms are real, from the same meter model.
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

// TS-003 — FR-AGG-02. The 8-character serial straddles a General Block
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

// TS-004 — FR-DEC-03. The library hands over the meter's scaler beside the
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

// TS-005 — FR-DEC-04. float has a 24-bit mantissa and is exact only to
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

// TS-009 / TS-010 — FR-MTR-06. Reception begins inside a frame, which
// is the ordinary case rather than a fault: the device is energised by the
// meter, so it wakes into a transmission already in progress.
//
// The identity is deliberately not asserted here. The meter sends it once per
// telegram, so a cycle that began after it went past cannot contain it, and no
// decoder can recover what was never received — losing that one cycle is the
// correct cost of a mid-burst start, not a defect. What must hold is that the
// bytes which did arrive still decode, so the next complete cycle is whole.
// Whether the device actually gets a whole next cycle is a timing property of
// the receive path, which only the bench can see (TS-017, TS-018).
void mid_burst_recovery(const char* fixture, size_t offset) {
  const auto values = decode_from_offset(fixture, offset);
  printf("       started %zu bytes in, decoded %zu value(s)\n", offset, values.size());
  check(!values.empty(), "alignment recovered — the remaining frames still decode");
}

// TS-023 — FR-MTR-06. A capture preceded by bare HDLC flags.
//
// This is not a contrived input. A closing flag and the next opening flag are
// adjacent on the wire, so `7E 7E` appears at every frame boundary, and a cycle
// that begins near one starts with a flag that opens nothing. Observed on the
// bench: six consecutive cycles each carried `7E 7E A0 92 ...`, and each decoded
// nothing at all.
//
// The distinction that matters is between a flag and a frame start. `7E`
// followed by `7E` is a boundary; `7E` followed by an `A0`-class byte is a
// frame. Treating the first `7E` found as a frame start reads the second flag
// as a format byte, and the whole remaining cycle is lost to a byte of padding.
void leading_flags_skipped(const char* fixture) {
  const auto whole = decode(fixture);
  check(!whole.empty(), "the intact capture decodes, so the comparison means something");

  for (size_t n : { size_t{ 1 }, size_t{ 2 }, size_t{ 3 } }) {
    std::vector<uint8_t> bytes = read_hex(fixture);
    bytes.insert(bytes.begin(), n, 0x7E);
    const auto got = decode_bytes(std::move(bytes));
    printf("       %zu leading flag(s): %zu of %zu value(s)\n",
           n, got.size(), whole.size());
    check(got.size() == whole.size(),
          "a leading flag is skipped, not fatal — the capture decodes whole");
  }
}

// TS-024 — FR-MTR-06. The bench case reproduced at host tier: a cycle that
// opens inside a frame's payload, with the next real frame reachable only past
// a `7E 7E` boundary. The prefix carries no flag of its own, so a decoder that
// scans for framing has exactly one correct answer available to it.
void midframe_prefix_then_boundary(const char* fixture) {
  const std::vector<uint8_t> whole = read_hex(fixture);
  const auto reference = decode_bytes(whole);
  check(!reference.empty(), "the intact capture decodes, so the comparison means something");

  // Find a frame start (7E followed by an A0-class byte) past the beginning,
  // and cut the buffer to open in the payload that precedes it.
  size_t frame = 0;
  for (size_t i = 1; i + 1 < whole.size(); ++i) {
    if (whole[i] == 0x7E && (whole[i + 1] & 0xF0) == 0xA0) { frame = i; break; }
  }
  check(frame > 40, "the fixture has a later frame to resynchronise onto");

  std::vector<uint8_t> bytes(whole.begin() + static_cast<long>(frame) - 40, whole.end());
  const auto got = decode_bytes(std::move(bytes));
  printf("       opened 40 bytes before a frame boundary: %zu value(s)\n", got.size());
  check(!got.empty(), "the frames after the boundary still decode");
}

// TS-107 — FR-AGG-02, FR-MTR-10. A cycle is parsed until it stops yielding.
//
// One parse returns after the first complete APDU and says how far it got. On
// the bench that was 110 bytes of a 400-byte telegram: six registers decoded,
// and everything after them — including the meter identity — never looked at.
// Discovery waits on that identity, so the device connected, decoded, and
// published nothing.
//
// The fixture happens to yield everything in one pass, which is exactly why the
// defect survived: a test that only ever saw this capture could not tell one
// parse from many. So the assertion is on the *loop* — that it terminates, that
// a second pass adds nothing here, and that the identity is among the results.
void parse_until_exhausted(const char* fixture) {
  std::vector<uint8_t> bytes = read_hex(fixture);
  const size_t skip = gplug::leading_flags(bytes.data(), bytes.size());

  size_t offset = skip, passes = 0, consumed = 0;
  while (offset < bytes.size() && passes < 64) {
    std::vector<uint8_t> rest(bytes.begin() + static_cast<long>(offset), bytes.end());
    const size_t before = rest.size();
    const auto values = decode_bytes(std::move(rest));
    ++passes;
    // decode_bytes does not report consumption, so advance by the whole
    // remainder when a pass yields nothing: the point here is termination.
    if (values.empty()) break;
    offset += before;
    consumed += before;
  }
  printf("       %zu pass(es), %zu byte(s) consumed of %zu\n", passes, consumed, bytes.size());
  check(passes < 64, "the loop terminates rather than spinning on a buffer it cannot advance");

  const auto all = decode(fixture);
  check(all.size() == 10, "this capture yields ten values");
  check(find_identity(all) != nullptr,
        "the identity is among them — discovery cannot publish without it");
}

// TS-011 — FR-MTR-07. One byte of a frame's checksum is flipped. The library
// must drop that frame whole: a CRC exists so that a corrupted frame is treated
// as absent rather than as data, and half a frame is worse than none because
// nothing downstream can tell it is half.
void crc_invalid_frame_discarded(const char* fixture) {
  const auto good = decode(fixture);
  std::vector<uint8_t> bytes = read_hex(fixture);
  // The two bytes before the closing flag are the frame check sequence.
  size_t last_flag = bytes.size();
  while (last_flag-- > 0 && bytes[last_flag] != 0x7E) {}
  const size_t fcs = last_flag - 1;
  bytes[fcs] ^= 0xFF;
  const auto bad = decode_bytes(std::move(bytes));
  printf("       intact: %zu value(s), one checksum byte flipped: %zu\n",
         good.size(), bad.size());
  check(!good.empty(), "the intact capture decodes, so the comparison means something");
  check(bad.size() < good.size(), "the corrupted frame contributes nothing");
}

// TS-012 — FR-MTR-08. The same corrupted capture, observed at the decoder's
// OUTPUT rather than at its frame count.
//
// TS-011 asks whether the bad frame was dropped; this asks whether any part of
// it got through. They are different failures. A parser that discards the frame
// but has already handed over the two objects it read before reaching the FCS
// passes TS-011 — fewer values came out — and still forwards data from a frame
// known to be corrupt. That is the case FR-MTR-08 exists for, and it is worse
// than a wrong count: the values it leaks are indistinguishable from good ones.
//
// So the criterion is containment, not size. Every value that survives the
// corruption must be byte-identical to one the intact capture produced. A value
// that is new, or the same OBIS carrying a different number, can only have come
// from bytes the decoder should never have seen.
void crc_invalid_frame_not_forwarded(const char* fixture) {
  const auto good = decode(fixture);
  check(!good.empty(), "the intact capture decodes, so the comparison means something");

  std::vector<uint8_t> bytes = read_hex(fixture);
  size_t last_flag = bytes.size();
  while (last_flag-- > 0 && bytes[last_flag] != 0x7E) {}
  bytes[last_flag - 1] ^= 0xFF;              // the FCS, two bytes before the closing flag
  const auto bad = decode_bytes(std::move(bytes));

  size_t leaked = 0;
  for (const auto& v : bad) {
    bool matched = false;
    for (const auto& g : good) {
      if (v.obis == g.obis && v.numeric == g.numeric &&
          v.raw == g.raw && v.scaler == g.scaler && v.text == g.text) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      ++leaked;
      printf("       leaked %s raw=%llu scaler=%d text=%s\n", v.obis.c_str(),
             v.raw, v.scaler, v.text.c_str());
    }
  }
  printf("       intact: %zu value(s); corrupted: %zu value(s), %zu of them unaccounted for\n",
         good.size(), bad.size(), leaked);
  check(leaked == 0,
        "no value the intact capture did not also produce — nothing from the "
        "corrupted frame reached the decoder's output");
}

// TS-013 — FR-MTR-10. The meter publishes its identity as either the COSEM
// logical device name or device ID 1. Both positive cases are covered by the two
// fixtures, which happen to carry one each. This is the third case: neither.
//
// It matters because the device defers Home Assistant discovery until it knows
// the meter serial (FR-HA-03). Inventing an identity here would produce entities
// that survive nothing — a rename on the next boot, and a household's history
// split across two devices.
void identity_absent(const char* fixture) {
  std::vector<uint8_t> bytes = read_hex(fixture);
  // Blind both identity OBIS codes: 0.0.42.0.0.255 and 0.0.96.1.0.255.
  const uint8_t ldn[6]   = { 0x00, 0x00, 0x2A, 0x00, 0x00, 0xFF };
  const uint8_t devid[6] = { 0x00, 0x00, 0x60, 0x01, 0x00, 0xFF };
  size_t blinded = 0;
  for (size_t i = 0; i + 6 <= bytes.size(); ++i) {
    if (!std::memcmp(&bytes[i], ldn, 6) || !std::memcmp(&bytes[i], devid, 6)) {
      bytes[i + 2] = 0x99;  // no OBIS group C the meter publishes
      ++blinded;
    }
  }
  printf("       blinded %zu identity object(s)\n", blinded);
  check(blinded > 0, "the fixture carried an identity to remove");
  const auto values = decode_bytes(std::move(bytes));
  check(find_identity(values) == nullptr, "no identity is reported when none was sent");
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
  else if (name == "midburst17") mid_burst_recovery(fixture, 17);
  else if (name == "midburst")   mid_burst_recovery(fixture, argc > 3 ? std::stoul(argv[3]) : 17);
  else if (name == "leadflags")  leading_flags_skipped(fixture);
  else if (name == "exhaust")    parse_until_exhausted(fixture);
  else if (name == "midprefix")  midframe_prefix_then_boundary(fixture);
  else if (name == "crcdrop")    crc_invalid_frame_discarded(fixture);
  else if (name == "crcleak")    crc_invalid_frame_not_forwarded(fixture);
  else if (name == "noidentity") identity_absent(fixture);
  else { std::fprintf(stderr, "unknown case: %s\n", name.c_str()); return 2; }

  printf("%s: %d failure(s)\n", name.c_str(), failures);
  return failures == 0 ? 0 : 1;
}
