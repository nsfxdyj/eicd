# eicd — Edge Industrial Communication Daemon

[![release](https://img.shields.io/github/v/release/nsfxdyj/eicd)](https://github.com/nsfxdyj/eicd/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

`eicd` is an edge gateway daemon that polls industrial devices over
**Modbus TCP** (southbound) and publishes telemetry to an **MQTT** broker
(northbound), with **store-and-forward** buffering to survive network
outages. It is written in C11 against nothing but POSIX and pthreads —
no libmodbus, no libmosquitto, no JSON library — so it builds and runs
on any Linux box, including ARM industrial gateways. The whole daemon is
a single binary of ~3,500 lines of C, and every blocking call carries an
explicit deadline, so no thread can hang indefinitely.

## What this is for

You have a row of PLCs, meters, or drives speaking Modbus TCP, and you
need their data in an MQTT-based platform — an energy dashboard, a
historian, an alarm pipeline. `eicd` is the piece that sits in between.
It does exactly one job and tries to be boring about it: poll registers,
report changes, survive outages.

Typical fits:

- **Retrofit telemetry** for existing plants, where touching the control
  network is not an option — put a cheap ARM gateway next to the PLC
  cabinet and point it at your broker.
- **A protocol front-end for cloud/SCADA teams** who want clean,
  source-timestamped JSON instead of writing their own Modbus stack.
- **Fleet deployments** where "zero dependencies" pays for itself: the
  binary is ~60 KB, cross-compiles with one `make CC=...`, and needs
  nothing on the target beyond a Linux kernel.

It is not a SCADA system, not a forty-driver protocol converter, and not
a no-code platform. If your site needs Modbus RTU (serial), TLS to the
broker, or southbound writes, check the [Roadmap](#roadmap) first —
some of it is scheduled, and the rest is the kind of work I do on
commission (see [Customization and support](#customization-and-support)).

## Demo

Recorded live from the bundled simulators — config check, startup, live
telemetry, a broker outage with store-and-forward recovery, and graceful
shutdown (55 s):

![eicd live demo](docs/demo.gif)

Run it yourself in four commands: [Quick start](#quick-start-no-hardware).

## Features

| Area | What you get |
|---|---|
| Southbound | Modbus TCP master: FC 0x01/0x02/0x03/0x04 (poll) and 0x06 (write single register primitive). One worker thread per device, per-device poll interval/timeout/retry. |
| Data model | CSV point table; types `bit/u16/s16/u32/s32/f32`; all four 32-bit word orders (`ABCD/BADC/CDAB/DCBA`); linear scaling `raw * scale + offset`. |
| Reporting | Report-by-exception (deadband), quality-transition reports, periodic integrity report, NaN rejection for floats. |
| Northbound | MQTT 3.1.1 publisher, QoS 0 or QoS 1 (PUBACK-confirmed), keepalive, exponential reconnect backoff. |
| Reliability | In-memory store-and-forward ring (drop-oldest, peek/commit delivery), graceful drain on shutdown, single-instance PID lock, SIGPIPE-safe. |
| Ops | INI config, leveled logging (stderr/file+rotation/syslog), SIGHUP = reopen log + refresh log level, `-t` config test mode, periodic statistics, systemd unit, logrotate snippet. |

## Architecture

```
            PLCs / RTUs / drives                                MQTT broker
          ┌──────┐ ┌──────┐                                    ┌──────────┐
          │PLC #1│ │PLC #2│  ...                               │  broker  │
          └──┬───┘ └──┬───┘                                    └────▲─────┘
             │        │ Modbus TCP (port 502)                       │ MQTT 1883
   ──────────┼────────┼──────────────── process boundary ───────────┼─────────
        ┌────▼────────▼─────────────────────────────────────────┐  │
        │  device worker 1   device worker 2  ...  (per device) │  │
        │  └─ poll ─► block read ─► decode/scale ─► deadband    │  │
        └──────────────┬────────────────────────────────────────┘  │
                       │ report (point table lock)                  │
                ┌──────▼───────────┐   peek/commit   ┌──────────────┴───┐
                │ store-and-forward│ ──────────────► │ MQTT publisher   │
                │ ring buffer      │                 │ thread           │
                │ (drop-oldest)    │ ◄────────────── │ backoff/keepal.  │
                └──────────────────┘   PUBACK/fail   └──────────────────┘
                       ▲
              main thread: config, signals (SIGTERM/SIGINT/SIGHUP),
              supervision, 60 s statistics, graceful shutdown
```

**Thread model**

| Thread | Count | Role |
|---|---|---|
| `main` | 1 | Startup sequencing, signal handling, stats, shutdown |
| `dev-<name>` | 1 per enabled device | Connect, block-poll, decode, evaluate report criteria |
| `eicd-publisher` | 1 | Drain queue → MQTT publish, reconnect backoff, keepalive |

**Data flow of one value change**

1. The device worker reads a register block (consecutive points are
   coalesced into one Modbus request).
2. Each point is decoded (word order), scaled, and compared against the
   last *reported* value under the point-table lock.
3. If the deadband is exceeded (or quality changed, or the integrity
   interval expired), a JSON frame is rendered and pushed into the ring
   buffer — the worker never blocks on the network here.
4. The publisher thread peeks the head message, publishes with the
   configured QoS, and only then *commits* (retires) it. A failed publish
   is retried, never dropped.

## Repository layout

```
eicd/
├── Makefile                  # strict-warning C11 build, install targets
├── README.md
├── LICENSE
├── include/
│   ├── defs.h                # version, hard limits, shared return codes
│   ├── log.h                 # leveled thread-safe logging
│   ├── util.h                # clocks, parsing, bounded socket I/O
│   ├── config.h              # INI config model
│   ├── points.h              # point table, decoding, report logic
│   ├── ringbuf.h             # store-and-forward queue (peek/commit)
│   ├── modbus_tcp.h          # Modbus TCP client (master)
│   ├── mqtt_client.h         # minimal MQTT 3.1.1 publisher client
│   ├── device.h              # southbound polling worker
│   ├── northbound.h          # JSON rendering + publisher thread
│   ├── daemonize.h           # double-fork daemon, PID file lock
│   └── app.h                 # application context, lock order, helpers
├── src/                      # one .c per header above + main.c
├── etc/
│   ├── eicd.conf.example     # annotated reference configuration
│   └── points.csv.example    # point table matching the simulator
├── packaging/
│   ├── eicd.service          # systemd unit (hardened)
│   └── eicd.logrotate        # logrotate snippet
└── tools/
    ├── modbus_sim.py         # Modbus TCP server with animated process model
    └── mqtt_stub_broker.py   # MQTT 3.1.1 stub broker, prints every publish
```

## Building

Requires a C11 compiler and pthreads.

```sh
make            # produces ./eicd
make clean
```

The build uses `-Wall -Wextra -Wshadow -Wformat=2 -Wstrict-prototypes
-Wmissing-prototypes` and is warning-clean with GCC. There is no
`configure` step and no dependency to install; cross-compiling is a
matter of `make CC=arm-linux-gnueabihf-gcc`.

## Testing

```sh
make test
```

The reliability suite covers Modbus CRC/MBAP framing, all four 32-bit
word orders, invalid float input, ring wrap with stale peek/commit,
fragmented and coalesced Modbus responses, peer disconnects, and MQTT
remaining-length plus QoS 1 PUBACK handling. Network tests use local
`socketpair` endpoints and require no external PLC or broker.

CI builds the daemon and runs this suite with both GCC and Clang, then
repeats it with AddressSanitizer and UndefinedBehaviorSanitizer.

## Quick start (no hardware)

The repository ships with two development tools: a Modbus TCP simulator
that animates a small process model, and a stub MQTT broker that prints
every published message. The example configuration already points at them.

```sh
# terminal 1 — fake PLC on 127.0.0.1:1502
python3 tools/modbus_sim.py

# terminal 2 — fake broker on 127.0.0.1:1883, prints publishes
python3 tools/mqtt_stub_broker.py

# terminal 3 — validate the configuration, then run in the foreground
mkdir -p /tmp/eicd-run
cp etc/eicd.conf.example /tmp/eicd-run/eicd.conf   # paths pre-set for /tmp/eicd-run
cp etc/points.csv.example /tmp/eicd-run/points.csv
./eicd -t -c /tmp/eicd-run/eicd.conf
./eicd -f -c /tmp/eicd-run/eicd.conf
```

You should see JSON telemetry appear in terminal 2 within a second:

```
PUBLISH  factory/line1/eicd-gw-01/simplc/reactor_temp  {"ts":1721404801842,"gw":"eicd-gw-01","dev":"simplc","tag":"reactor_temp","value":81.2375031,"unit":"degC","q":"good"}
```

**Try the failure modes:**

* **Kill the broker** (`Ctrl-C` in terminal 2). The publisher logs
  `publish failed, message kept`, then reconnects with exponential
  backoff (1 s → 2 s → 4 s … 60 s). Telemetry accumulates in the ring.
* **Restart the broker.** The queue drains in FIFO order — look at the
  `ts` fields: buffered messages arrive with their original timestamps.
  `dropped=0` in the stats line confirms nothing was lost.
* **Kill the Modbus simulator.** All points of that device are
  re-published once with `"q":"bad"` (keeping last-known values) and the
  worker retries every `retry_interval_s`. Restart it and quality returns
  to `"good"` on the next cycle.
* **Stop the daemon** with `Ctrl-C` / `SIGTERM`: workers stop, the queue
  drains (up to `drain_timeout_s`), and the PID file is removed.

## Installation

```sh
sudo make install                 # /usr/local/sbin/eicd, /etc/eicd/*, /var/lib/eicd
sudo useradd -r -s /usr/sbin/nologin -d /var/lib/eicd eicd
sudo cp packaging/eicd.service /etc/systemd/system/
# edit /etc/eicd/eicd.conf and /etc/eicd/points.csv for your site
sudo eicd -t -c /etc/eicd/eicd.conf      # validate before enabling!
sudo systemctl enable --now eicd
```

`make install` never overwrites an existing configuration. The systemd
unit runs the daemon in **foreground mode** (`-f`) with logs going to the
journal (`journalctl -u eicd -f`), and applies standard hardening
(`NoNewPrivileges`, `ProtectSystem=strict`, …).

## Configuration reference

INI format. Unknown keys warn (forward compatibility); malformed values
are fatal. Validate any change with `eicd -t -c <file>` before restarting.

### `[daemon]`

| Key | Default | Meaning |
|---|---|---|
| `pid_file` | `/var/lib/eicd/eicd.pid` | PID file; also the single-instance `flock()` anchor |
| `work_dir` | `/var/lib/eicd` | Data directory (created if missing) |

### `[log]`

| Key | Default | Meaning |
|---|---|---|
| `level` | `info` | `debug` / `info` / `warn` / `error` (SIGHUP-reloadable) |
| `file` | *(empty = stderr)* | Log file; size rotation keeps one `.1` backup |
| `max_size_mb` | `10` | Rotation threshold for `file` |
| `syslog` | `false` | Mirror to syslog (`LOG_DAEMON`) |

### `[gateway]`

| Key | Default | Meaning |
|---|---|---|
| `points_file` | `/etc/eicd/points.csv` | Point table path |
| `queue_capacity` | `1024` | Store-and-forward ring slots (~1.4 KB each) |
| `report_interval_s` | `60` | Integrity report period; `0` = change-only |
| `drain_timeout_s` | `10` | Max queue drain time on shutdown |

### `[mqtt]`

| Key | Default | Meaning |
|---|---|---|
| `host` / `port` | — / `1883` | Broker address (**host is required**) |
| `client_id` | `eicd-gateway` | MQTT client id; also the `"gw"` payload field |
| `username` / `password` | empty | Optional MQTT auth |
| `keepalive_s` | `60` | MQTT keepalive; pings are sent at 75 % idle |
| `qos` | `1` | `0` or `1` (QoS 1 waits for PUBACK; recommended) |
| `retain` | `false` | MQTT retain flag |
| `topic_prefix` | `eicd/gateway` | Publish prefix; topics are `<prefix>/<device>/<tag>` |
| `reconnect_min_s` / `reconnect_max_s` | `1` / `60` | Exponential backoff bounds |

### `[device.<name>]` — one section per PLC/RTU

| Key | Default | Meaning |
|---|---|---|
| `host` / `port` | — / `502` | Modbus TCP endpoint (**host is required**) |
| `unit_id` | `1` | Modbus unit identifier (0–247) |
| `poll_interval_ms` | `1000` | Scan cycle (50–3,600,000) |
| `timeout_ms` | `3000` | Per-transaction timeout (100–60,000) |
| `retry_interval_s` | `5` | Fixed reconnect delay after failure |
| `enabled` | `true` | `false` keeps the config but skips the device |

Up to 32 devices and 2048 points per instance (compile-time limits in
`include/defs.h`).

## Point table reference

CSV, one point per line, `#` comments allowed, optional header row.
**All ten columns are required** (leave a field empty where irrelevant):

```
tag,device,func,addr,dtype,worder,scale,offset,unit,deadband
reactor_temp,simplc,holding,100,f32,ABCD,1.0,0.0,degC,0.2
pump_running,simplc,coil,5,bit,,1.0,0.0,,0
```

| Column | Values | Notes |
|---|---|---|
| `tag` | `[A-Za-z0-9._-]`, unique | Becomes the last MQTT topic segment |
| `device` | a `[device.<name>]` from the config | Validated at load |
| `func` | `coil`, `discrete`, `holding`, `input` | Modbus table to read |
| `addr` | `0..65535` | **Zero-based** protocol address (see below) |
| `dtype` | `bit`, `u16`, `s16`, `u32`, `s32`, `f32` | `bit` only for coil/discrete |
| `worder` | `ABCD`, `BADC`, `CDAB`, `DCBA` | 32-bit order; ignored for 16-bit |
| `scale` | float, non-zero | `engineering = raw × scale + offset` |
| `offset` | float | |
| `unit` | ≤ 15 chars | Engineering unit, appears in the payload |
| `deadband` | float ≥ 0 | Report-by-exception threshold (engineering units) |

**Addressing.** Addresses are zero-based protocol values: what many PLC
manuals call "holding register 40001" is `holding, addr 0` here
(40001 → 0, 30117 → 116, etc.). A wrong address usually surfaces as a
Modbus exception 02, which the log prints explicitly.

**Word order.** Registers are big-endian on the wire; `worder` describes
how two registers assemble into a 32-bit value. `ABCD` is standard,
`CDAB` is the widespread "word swap" (many Schneider/Modicon devices),
`BADC`/`DCBA` cover byte-swapped variants. If a float looks like garbage,
try `CDAB` first.

**Scaling example.** A 4–20 mA transmitter on a 0–27648 raw range:
`scale = 0.000579154`, `offset = 4.0` gives milliamps directly.

## MQTT integration

**Topic scheme**

```
<topic_prefix>/<device>/<tag>
factory/line1/eicd-gw-01/simplc/reactor_temp
```

**Payload schema** (compact JSON, one point per message):

```json
{
  "ts":    1721404801842,        // event time, epoch milliseconds
  "gw":    "eicd-gw-01",         // gateway id (= mqtt client_id)
  "dev":   "simplc",             // device name from the config
  "tag":   "reactor_temp",       // point tag
  "value": 81.2375031,           // engineering value (number)
  "unit":  "degC",               // unit string from the point table
  "q":     "good"                // "good" | "bad" | "uncertain"
}
```

Notes:

* `ts` is stamped at the source — buffered messages keep their original
  event time across a store-and-forward episode.
* Integer types serialise exactly; `f32` serialises with 9 significant
  digits (lossless single-precision round trip).
* With `qos = 1`, every message is PUBACK-confirmed before it is retired
  from the queue. QoS 2 and subscriptions are intentionally out of scope
  (see [Roadmap](#roadmap)).

## Store-and-forward semantics

* The ring holds `queue_capacity` fixed slots (~1.4 KB each — 1024 slots
  ≈ 1.4 MB RAM).
* **Push never blocks.** When full, the *oldest* message is overwritten
  and the `dropped` counter increments — a northbound outage can never
  stall southbound polling.
* **Delivery order is FIFO.** The publisher peeks the head, publishes,
  then commits by sequence number — a commit can never retire a message
  that was not actually delivered, even when the ring wrapped meanwhile.
* **Sizing rule of thumb:**

  ```
  capacity ≥ (worst-case outage_seconds × points_per_second) + 20 %
  ```
  e.g. 50 points reporting 1/s on average with a 10 min outage target:
  50 × 600 × 1.2 ≈ 36 000 slots (~50 MB). For longer outages, see the
  disk-spool item in [Roadmap](#roadmap).
* On shutdown, remaining messages are drained for up to
  `drain_timeout_s`; whatever does not fit is logged and discarded
  (the ring is in-memory).

## Operations

**Signals**

| Signal | Effect |
|---|---|
| `SIGTERM` / `SIGINT` | Graceful shutdown: stop workers, drain queue, exit 0 |
| `SIGHUP` | Reopen log file (logrotate) + re-read `log.level` only |

Structural changes (devices, points, broker) require a restart — by
design; see [Design notes](#design-notes).

**Logging & statistics.** Every 60 s the daemon logs a gateway summary
(good/bad points, queue depth, drop counter) plus a per-device line
(polls ok/fail, transactions ok/fail), and the publisher logs its
counters (published/failed/enqueued/dequeued/dropped). These five
numbers are your first health dashboard; alert on `dropped > 0` and
`device OFFLINE`.

**logrotate.** If `[log] file` is set, install `packaging/eicd.logrotate`
to `/etc/logrotate.d/eicd` — it sends SIGHUP after rotation, and the
daemon reopens the file.

**Single instance.** The PID file is `flock()`-ed for the process
lifetime; a second instance refuses to start, and the lock survives
`SIGKILL` (the kernel releases it, so no stale-lock problem).

## Security notes

* **Run unprivileged.** The systemd unit uses a dedicated `eicd` user
  plus `NoNewPrivileges`, `ProtectSystem=strict`, an empty capability
  bounding set, etc. The daemon needs nothing beyond network access.
* **No TLS in this version.** Terminate TLS at the broker (same host /
  trusted DMZ), or put the broker behind stunnel/Envoy. Credentials are
  otherwise on the wire in clear text — treat `username`/`password` as
  placeholders until the TLS roadmap item lands.
* **Config hygiene.** Keep `eicd.conf` at `0640 root:eicd`; it contains
  the broker password. `make install` sets those permissions and never
  overwrites an existing config.
* **Input validation.** Tags, device names, units and the client id are
  restricted to JSON/topic-safe character sets at load time; everything
  that ends up inside a JSON string is validated, so payload injection
  through configuration is not possible.
* **Network segmentation.** The daemon fits a two-interface gateway
  setup: control network (OT) southbound, IT/DMZ northbound. It
  initiates outbound connections only.

## Design notes

* **No third-party libraries.** Both protocol stacks (Modbus TCP client,
  MQTT 3.1.1 publisher) are implemented against RFC/spec on purpose:
  the resulting binary has a tiny, auditable attack surface and no
  dependency drift. The trade-off is a deliberately narrow feature set
  (documented in each header).
* **No watchdog thread.** A watchdog only helps against threads that can
  block forever. Here *every* blocking call (connect, send, recv, queue
  peek) is bounded by an explicit deadline via `poll()` — so a stuck
  thread is structurally impossible, and the supervision loop only needs
  to check flags.
* **Lock order:** `point_table.mtx → ringbuf.mtx`, documented in
  `app.h`. The publisher takes only the queue lock; workers take the
  table lock and may briefly take the queue lock to push. No cycle
  exists, hence no deadlock.
* **Peek + commit by sequence number** in the ring buffer (see
  `ringbuf.h`): the classic "don't lose the message on publish failure"
  pattern, hardened against the drop-oldest wrap case.
* **Report-by-exception + integrity report** mirrors how industrial
  historians (OSIsoft PI, etc.) have worked for decades: deadband filters
  noise, the integrity interval bounds staleness, and quality transitions
  always report immediately.
* **Live reload is deliberately shallow** (log level + log reopen only).
  Re-pointing devices and re-mapping a shared point table under running
  workers is error-prone; a config change for an industrial gateway
  should be a restart, validated with `-t` first.
* **Fixed-size, pre-allocated structures** (point table, config arrays,
  ring slots): predictable memory footprint, no allocator churn in
  steady state — the only heap allocation after startup is each device's
  index array, allocated once at init.

## Performance and limits

Measured on a 2020-era x86 VM (single core):

| Metric | Value |
|---|---|
| Binary size (stripped, `-O2`) | ~60 KB |
| Resident memory (7 points, 1024-slot ring) | ~2.5 MB |
| CPU at 1 poll/s, 7 points | < 0.1 % |
| Points per instance | 2048 (compile-time, `EICD_MAX_POINTS`) |
| Devices per instance | 32 (compile-time, `EICD_MAX_DEVICES`) |
| Sustained publish rate | thousands of msg/s (broker permitting) |

The practical limit is almost always the control network, not the
daemon: keep `poll_interval_ms × devices` within what your PLCs can
serve, and use block reads (automatic for consecutive addresses).

## Roadmap

In priority order:

1. **TLS for MQTT** (Mbed TLS or OpenSSL, optional at build time)
2. **Disk-backed spool** for multi-hour store-and-forward
3. **Modbus RTU** (serial) via a `device.type = rtu` section — the CRC16
   helper is already in place
4. **Southbound writes / downlink commands** (MQTT command topic → FC 6/16)
5. Sparkplug B payload profile (opt-in alongside the native JSON schema)
6. Prometheus `/metrics` endpoint
7. OPC UA southbound (via open62541, optional build flavour)

## Troubleshooting

| Symptom | Likely cause / check |
|---|---|
| `cannot lock pid file ... (already running?)` | Another instance is alive (`pgrep eicd`), or the pid directory is not writable |
| `exception 02 (illegal data address)` | Wrong `addr` (remember: zero-based), or the point is outside the PLC's mapped range |
| `exception 01 (illegal function)` | The device does not support that FC (e.g. input registers on a holding-only device) |
| Values look like garbage floats | Wrong `worder` — try `CDAB` |
| `broker refused connection (rc=N)` | MQTT auth/client-id rejected: rc 4 = bad credentials, 5 = not authorised |
| `dropped` counter increases | Ring too small for your outage windows — raise `queue_capacity` (see sizing rule) |
| Nothing published, no errors | Check `report_interval_s` and deadbands: with `0` and a deadband larger than the signal swing, points legitimately stay silent |

Always start with `./eicd -t -c <conf>` (config + points validation) and
a foreground (`-f`) run in a terminal.

## Customization and support

`eicd` comes out of three years of shipping data-acquisition gateways in
the power sector. If you need a variant — Modbus RTU on a serial port,
TLS to the broker, disk spooling for long outages, a different payload
schema (Sparkplug B, or whatever your platform expects), IEC 60870-5-101
on the southbound side, or a hardened deployment for a specific device —
that is consulting work I do. I also take on custom edge-gateway
development and private-deployment support more generally.

- Email: nsfxdyj@gmail.com
- Or open an issue in this repository — I read those faster than my
  inbox.

## License

MIT — see [LICENSE](LICENSE).
