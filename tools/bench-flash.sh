#!/usr/bin/env bash
# Put the board in a known state, then flash it over serial.
#
#   tools/bench-flash.sh <dir-with-coldflash-binaries> [slot]
#
# Flashing is normally one call to the workbench's /api/flash. This wrapper
# exists because that call assumes a healthy board, and on 2026-08-05/06 a full
# day went into failures that were never about the firmware: a slot still
# flapping after a power cycle, a chip left in SPI download boot, and a supply
# that browned out whenever the radio started. Each looked like a flash problem.
#
# So every step below answers one question before the next is allowed to matter,
# and refuses to flash into a state where the result could not be trusted.
#
# Needs: BENCH (default 192.168.0.27), ssh access as pi for the OpenOCD steps.
set -euo pipefail

DIR="${1:?usage: bench-flash.sh <dir-with-coldflash-binaries> [slot]}"
SLOT="${2:-SLOT1}"
BENCH="${BENCH:-192.168.0.27}"
URL="http://${BENCH}:8080"
CHIP="${CHIP:-esp32c3}"
OOCD_SCRIPTS=/usr/local/share/openocd-esp32/scripts
OOCD_CFG="board/${CHIP}-builtin.cfg"

say() { printf '\n== %s\n' "$1"; }

for f in bootloader.bin partition-table.bin ota_data_initial.bin gplug-mini.bin; do
  [ -f "$DIR/$f" ] || { echo "missing $DIR/$f"; exit 1; }
done

# ── 1. Is the slot in a state worth flashing into? ───────────────────────────
#
# A slot that is flapping has a device enumerating and disappearing every few
# hundred milliseconds. esptool will start, lose the endpoint mid-write, and
# report a timeout that reads exactly like a broken image.
say "slot state"
state=$(curl -sf -m 10 "$URL/api/devices" \
        | python3 -c "import json,sys; print([s for s in json.load(sys.stdin)['slots'] if s['label']=='$SLOT'][0]['state'])")
echo "   $SLOT: $state"
if [ "$state" = "flapping" ]; then
  echo "   refusing to flash a flapping slot — POST /api/serial/recover and wait for idle."
  echo "   If it keeps flapping, read the serial directly: a repeating"
  echo "   'E BOD: Brownout detector was triggered' is a power fault, not a flash fault."
  exit 2
fi

# ── 2. A known state, via JTAG ───────────────────────────────────────────────
#
# JTAG is used only to *establish* the state, never to program: it answers even
# when the serial endpoint does not, so it can tell a dead chip from a dead CDC.
# The reset cause and strapping register are printed because they are what
# distinguish the failures that otherwise look identical:
#
#   GPIO_STRAP bit1/bit2 low  -> GPIO8/GPIO9 low at reset -> SPI download boot,
#                                where the ROM does not listen on USB at all
#   reset cause "brownout"    -> the supply collapses; nothing downstream is
#                                trustworthy until that is fixed
say "known state via OpenOCD (JTAG is used to inspect and reset, not to program)"
ssh -o BatchMode=yes "pi@${BENCH}" "
  sudo systemctl stop rfc2217-portal.service
  sleep 2
  sudo timeout 90 openocd-esp32 -s ${OOCD_SCRIPTS} -f ${OOCD_CFG} \
    -c 'init' -c 'halt' -c 'mdw 0x60004038' -c 'reset run' -c 'exit' 2>&1 \
  | grep -E 'tap/device found|Reset cause|0x60004038|Error' || true
" || { echo "   OpenOCD could not reach the chip — it is not just the serial port"; exit 3; }

# ── 3. Flash over serial, the ordinary way ───────────────────────────────────
say "flash over serial"
ssh -o BatchMode=yes "pi@${BENCH}" "sudo systemctl start rfc2217-portal.service" || true
sleep 8

http=$(curl -s -m 400 -X POST "$URL/api/flash" \
  -F "slot=$SLOT" -F "chip=$CHIP" -F baud=460800 \
  -F "bin@0x0000=@$DIR/bootloader.bin" \
  -F "bin@0x8000=@$DIR/partition-table.bin" \
  -F "bin@0xd000=@$DIR/ota_data_initial.bin" \
  -F "bin@0x20000=@$DIR/gplug-mini.bin" \
  -o /tmp/bench-flash.json -w '%{http_code}')

if [ "$http" = "200" ] && python3 -c "import json,sys; sys.exit(0 if json.load(open('/tmp/bench-flash.json')).get('ok') else 1)"; then
  echo "   flashed over serial"
else
  # Only now is JTAG programming justified: serial has actually been tried, and
  # program_esp verifies each region so the output says which were really wrong.
  say "serial flash failed — falling back to JTAG"
  tail -c 400 /tmp/bench-flash.json || true
  scp -q "$DIR"/{bootloader.bin,partition-table.bin,ota_data_initial.bin,gplug-mini.bin} "pi@${BENCH}:/tmp/gplug-fw/" 2>/dev/null \
    || ssh -o BatchMode=yes "pi@${BENCH}" 'mkdir -p /tmp/gplug-fw' && \
       scp -q "$DIR"/{bootloader.bin,partition-table.bin,ota_data_initial.bin,gplug-mini.bin} "pi@${BENCH}:/tmp/gplug-fw/"
  ssh -o BatchMode=yes "pi@${BENCH}" "
    sudo systemctl stop rfc2217-portal.service; sleep 2
    sudo timeout 300 openocd-esp32 -s ${OOCD_SCRIPTS} -f ${OOCD_CFG} \
      -c 'init' -c 'halt' \
      -c 'program_esp /tmp/gplug-fw/bootloader.bin       0x0     verify' \
      -c 'program_esp /tmp/gplug-fw/partition-table.bin  0x8000  verify' \
      -c 'program_esp /tmp/gplug-fw/ota_data_initial.bin 0xd000  verify' \
      -c 'program_esp /tmp/gplug-fw/gplug-mini.bin       0x20000 verify' \
      -c 'reset run' -c 'exit' 2>&1 | grep -E 'Programming Finished|Verify OK|matches|Error' || true
    sudo systemctl start rfc2217-portal.service"
fi

# ── 4. The bench is not back until the broker is ─────────────────────────────
# mosquitto does not survive an rfc2217-portal restart. It stops silently, and
# the next test then fails against an address that worked minutes earlier.
say "restore bench services"
sleep 6
curl -s -m 20 -X POST "$URL/api/mqtt/start" -H 'Content-Type: application/json' -d '{}' >/dev/null && echo "   broker started"

# ── 5. Prove it boots, and say what it is ────────────────────────────────────
say "boot banner"
python3 - "$URL" "$SLOT" <<'PY'
import json, sys, time, urllib.request
url, slot = sys.argv[1], sys.argv[2]
def api(path, body=None, t=20):
    d = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(f"{url}{path}", data=d, method="POST" if d else "GET",
                               headers={"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(r, timeout=t + 10))
seen, end = [], time.time() + 40
while time.time() < end:
    try: out = api("/api/serial/monitor", {"slot": slot, "timeout": 5}, 5).get("output") or []
    except Exception: continue
    for line in out:
        if line not in seen: seen.append(line)
    if any("gPlug-mini" in l for l in seen): break
banner = [l for l in seen if "gPlug-mini" in l or "reset reason" in l]
for l in banner[:4]: print("  ", l)
if not banner:
    print("   no banner in 40 s. If the serial is silent while OpenOCD still answers,")
    print("   the chip is fine and the CDC is not — check the boot mode, not the image.")
    sys.exit(4)
PY
