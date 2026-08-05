# gPlug-mini — Architecture bindings

The layering itself is FSD §2.4 — what the components are, and what depends on
what. This file carries the rules that make the source mirror it, none of which is
observable from outside a running device.

## Source layout

One module per component, named for the component. An interface used by exactly
one feature still gets its own module.

```
main/
  app_main.cpp            composition root — the only file that wires modules together

  agg/  aggregator.{h,cpp}        L2  cycle assembly, publish decision
  sup/  supervisor.{h,cpp}        L2  state machine

  mtr/  meter_uart.{h,cpp}        L1  UART config, byte feed
        obis_map.{h,cpp}          L1  OBIS code → measurement label   (pure)
  ha/   ha_discovery.{h,cpp}      L1  discovery payload construction  (pure)
        ha_publish.{h,cpp}        L1  topic layout, publish calls
  prv/  portal.{h,cpp}            L1  HTTP handlers, DNS hijack
  ota/  updater.{h,cpp}           L1  command handling, download, validity marking
  led/  indicator.{h,cpp}         L1  state → pattern                 (pure core)

  cfg/  config.{h,cpp}            L0  NVS read/write
  net/  wifi.{h,cpp}              L0  station and SoftAP lifecycle
        mqtt.{h,cpp}              L0  client lifecycle

testing/host/     C++, no ESP-IDF headers
tests/target/   on-device
testing/bench/    Python, drives the Embedded Workbench over HTTP
```

`dlms_parser` is a managed component, not a module here. It is L0: it owns the
HDLC and DLMS protocol logic, and this project neither implemented nor tests it.

## Dependency direction

Lower layers never include higher ones. `main/` includes everything; nothing
includes `main/`.

The meter path needs the inversion most often: `meter_uart` receives bytes and the
aggregator must hear about them, but `meter_uart` must not know the aggregator
exists. The lower module exposes a registration hook and `app_main` connects it:

```cpp
// meter_uart.h  — knows nothing about who consumes frames
using FrameSink = void (*)(const uint8_t* payload, size_t len, void* ctx);
void meter_uart_on_frame(FrameSink sink, void* ctx);
```

The same applies to the supervisor's state changes reaching the indicator, and to
MQTT connection events reaching the OTA updater's validity marking.

## Pure cores

Split the decision from the I/O so the host tier can reach it with no ESP-IDF
headers in scope. Four cores carry most of the risk:

| Core | Decides | Failure it prevents |
|---|---|---|
| `obis_map` | OBIS code → label, unit, scale | A value published under the wrong label, or scaled twice (`FR-DEC-03`) |
| `aggregator` transition | Whether a gap ends a cycle | Publishing mid-burst, or never publishing |
| `ha_discovery` | The discovery payload | An energy entity the Energy Dashboard rejects |
| `supervisor` transition | Next state from (state, event) | Entering AP mode on a WiFi drop (`FR-SUP-04`) |

Each is a free function taking values and returning values. No `esp_` call, no
static state, no clock read — pass the timestamp in.

The supervisor transition function is worth stating as a rule: `next_state(state,
event, guards) -> action` is pure, and the task loop that calls it does the I/O.
The FSD's transition table has 32 rows and every one becomes a host test case that
runs in microseconds. A state machine welded to its I/O can only be tested on the
bench, which is how transition coverage quietly becomes state coverage.

## Build variants

Production and simulated builds differ at exactly one seam: what feeds bytes to
the aggregator. Selected by Kconfig (`FR-BLD-03`), never at runtime.

```
CONFIG_GPLUG_SIM_METER=n   meter_uart feeds the aggregator
CONFIG_GPLUG_SIM_METER=y   sim_source feeds the aggregator from an embedded capture
```

Everything above that seam is the same code in both binaries — that is what makes
a bench run against the simulated build evidence about the production one.

Two prohibitions follow, and they are not stylistic:

- **The capture data is compiled only into the simulated build.** `FR-BLD-04`
  requires the production binary to be incapable of fabricating readings, and it is
  checkable: search the artefact for the capture bytes.
- **The simulated build never publishes under the production discovery prefix**
  (`FR-BLD-06`). One synthetic value in Home Assistant's `total_increasing`
  statistics is effectively permanent.

## Stack

`dlms_parser` walks the AXDR structure recursively, and the default 3584-byte
main task stack faults on the first telegram. The build sets 8192; a measured
cycle leaves 3896 bytes free, so the decode itself costs about 4.3 KB.

**The depth follows the telegram, not the code.** A larger register set, or a
malformed one, recurses further than anything tested here — which is why the
firmware logs remaining headroom every cycle instead of trusting a constant.
Treat a falling number as a defect, not as a reason to raise the constant.

## Prohibitions

| Never | Because |
|---|---|
| Transmit on the meter link | The customer interface belongs to a sealed metering device — `FR-MTR-04` |
| Enter AP mode because WiFi dropped | Strands the device in a portal nobody can see — `FR-SUP-04` |
| Reset to clear a fault | Loses uptime, diagnostics, and any chance of diagnosis — `FR-WDT-05` |
| Poll for firmware updates | Delivers a bad build everywhere before anyone notices — `FR-OTA-02` |
| Retain measurement state messages | A retained reading is indistinguishable from a current one — `FR-HA-07` |
| Publish a placeholder for an absent register | Zero-meaning-absent and zero-meaning-zero are different facts — `FR-HA-05` |
| Mark an OTA image valid before MQTT connects | Makes rollback protect against nothing but a crash loop — `FR-OTA-05` |
