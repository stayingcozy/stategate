# Multi-machine design: handoff notes

Status: design direction agreed, nothing implemented yet. Written 2026-11-07 for the
"fsm-ai" project thread, from a design session that used a real device as the reference.

## Decision

stategate models a system as **several small flat machines that communicate through
typed events over declared channels**, with **shallow nesting inside a machine later**.
It will **not** add parallel (orthogonal) regions.

- Multiple machines solve state explosion and map one-to-one onto how firmware runs:
  one machine per task (FreeRTOS task, thread, ISR), channels as queues, the phone or
  host as an external machine.
- Shallow nesting (one or two levels, no history states) removes repetition such as
  every state handling FAULT. Today `inv.fault_off` (`every_state_handles`) works around
  its absence.
- Parallel regions were rejected: ambiguous semantics when one event reaches several
  regions (UML, SCXML and Harel differ), cross-region guards couple regions back
  together, invariants still need the product state space, and they hide the real
  deployment (regions imply one thread in lockstep; real devices run concerns on
  different tasks and cores).
- For the rare case that needs lockstep, a channel can be `sync` (delivered at once, same
  task, fixed order) instead of `queue` (asynchronous, the default). One concept,
  machines plus channels, with the choice explicit.
- Prior art for this exact combination: Miro Samek's QP framework (active objects, each
  running a hierarchical state machine, in C on microcontrollers). Worth reading for its
  C dispatch and run-to-completion rules.

## Scope rule (anti-bloat)

A design view earns a place in stategate only if stategate can **validate** it,
**generate** from it, **scope tickets** with it, or **watch it live**. Anything that only
produces documentation stays Markdown in the repo.

Out of scope: chat UI (MCP clients do it), text editor or IDE, BOM costing, PCB layout
(KiCad), mechanical, authored sequence diagrams (derive them from traces instead),
external issue trackers beyond the in-repo tickets.

## Design views

| View | Holds | Validates | Generates | Ticket scope |
|---|---|---|---|---|
| Machines (exists) | States, events, actions, services, invariants | Totality, reachability, invariants | Transition code and tests | Per action or service node |
| System (new) | Machines, instances, task placement, channels, rates | Wiring, timeouts, wait cycles, overflow policy, rate budget | Task setup, queues, event routing | Per machine or channel |
| Messages (new) | References to `.proto` files; event payload types | Payload types exist; field numbers never reused | nanopb (C) or prost (Rust) bindings | Per message handler |
| Hardware (new) | Pins, buses, mux channels, I2C addresses, power-enable pins | Address collision per bus segment; a pin used twice | Board header, HAL trait | Per driver |
| Requirements (later) | Requirement IDs linked to invariants and tickets | Every locked requirement links to an invariant or test | Traceability report (IEC 62304 style) | Already via node IDs |

Messages: do not invent an IDL. Reference `.proto` files and let events carry a payload
type from them.

## System-level checks (multi-machine)

| Check | Fails when |
|---|---|
| Wiring | A machine sends an event no receiver accepts, or accepts an outside event nobody sends (unless marked `external`, e.g. ISR or phone) |
| Request timeouts | A state waiting for a reply has no timeout transition |
| Wait cycles | A waits for B while B waits for A (static, from request/reply pairs) |
| Overflow policy | A channel has no declared depth and drop policy |
| Rate budget | Producer rate x message size exceeds the channel's or transport's declared capacity |

Machine-level checks once nesting lands:
- Totality with inheritance: a pair is decided if the state or any ancestor handles or ignores it.
- A child may not override a locked ancestor's transition (safety; medical relevance).
- Entry and exit order fixed by rule (exit innermost first, enter outermost first).

## Sketch of the new files (illustrative; field names not final)

```yaml
# system.yaml
system: { id: posture_logger, target: esp32s3, language: c }
machines:
  - { id: device,  file: device.yaml }                       # Boot, Provisioning, Idle, Recording, Fault
  - { id: imu,     file: bno055.yaml, instances: 4, param: mux_ch }
  - { id: sampler, file: sampler.yaml, task: { core: 1, period_ms: 10 } }
  - { id: logger,  file: logger.yaml }
  - { id: link,    file: link.yaml }                         # BLE setup, WiFi stream
channels:
  - { from: sampler, to: logger, msg: posture.v1.Tick, depth: 64, rate_hz: 100 }
  - { from: sampler, to: link,   msg: posture.v1.Tick, depth: 16, rate_hz: 100, drop: oldest }

# hardware.yaml
buses:
  - { id: i2c0, sda: GPIO3, scl: GPIO4, hz: 100000, power_en: GPIO7 }
devices:
  - { id: mux,          part: PCA9548,  bus: i2c0,    addr: 0x70 }
  - { id: imu_mid_back, part: BNO055,   bus: mux.ch0, addr: 0x28 }
  - { id: fuel_gauge,   part: MAX17048, bus: i2c0,    addr: 0x36 }
```

Note: `language` moves from the machine (`machine.language` today) to the system or target.

## Live debugging

- Generated code emits a compact trace record per transition: machine, instance, from,
  event, to, timestamp, sequence number (on the order of a dozen bytes).
- The trace record is one of the project's protobuf messages, carried over whatever
  transport exists: UART, BLE characteristic, WebSocket, or a debug probe.
- The handshake carries the model hash, so the viewer refuses to animate firmware that
  doesn't match the model. This extends the existing drift check to the running device.
- Messages between machines become arrows: the viewer can draw sequence diagrams from
  traces instead of anyone authoring them.
- First step toward the code-to-model translator: infer a candidate model from recorded
  traces and have a human confirm it. Translating arbitrary C into an FSM is much harder.

## C backend notes

- `switch` over an enum with no `default`, compiled with `-Wswitch-enum -Werror`: the C
  equivalent of the `no_wildcards` check.
- `SafeHal` becomes generated wrapper functions with the clamps; node code must not be
  able to include the raw HAL header.
- Trace tags (`@action`, `@service`, `@node`) stay as comments, unchanged.
- Nested-state dispatch: walk the parent chain to find a handler (QP style).

## Next steps (in order)

1. **System view with typed events.** Multiple machines, channels (`queue` / `sync`),
   instances with parameters, wiring and timeout checks. Unlocks everything else.
2. **Hardware view.** Pin and address validation, generated board header and HAL trait.
3. **C backend.** The reference device is C.
4. **Trace protocol and live view** of a real device.
5. **Requirement IDs and the traceability report.**
6. Later: shallow nesting inside machines, then the trace-based translator.

Immediate design task: sketch the posture logger (below) as its machines, their states,
the events between them and which events carry protobuf payloads. That defines both what
`system.yaml` must express and what the schema must contain.

## Open questions

- Event payloads: does an event carry a protobuf message directly, or a stategate-level
  type that references one?
- Instances: how are per-instance parameters (e.g. mux channel) declared and passed to
  actions and services? Static array of instances or generated per instance?
- External machines (phone app, host): declared as stub machines with an event alphabet
  only, or as a plain list of external events?
- Run-to-completion rules per task, and what happens to events sent to a machine that is
  mid-transition.
- How is the trace transport chosen per target, and what is the minimal C trace API?
- Ticket scope for drivers: can the gate verify a driver touches only its declared bus
  and pins?
- Sign-off: do system-level items (channel depth, rates, external events) get `locked`
  the way invariants do?
- Does the existing simulator (C, follows the model's transition table) extend to several
  machines, and how are channel delays modelled in it?
- Known limit carried over from the README: approvals rely on a typed terminal
  confirmation, not cryptographic signing.

## Reference project: posture logger

The design session used a real device, a back-posture recorder. It is a different
device from `examples/posture` (that one is a vibration-nudge wearable) and is a good
second example for the system and hardware views.

Hardware (Adafruit parts):
- ESP32-S3 Feather 8MB with w.FL antenna (5885), external 2.4 GHz antenna (2308)
- 4 x BNO055 9-DOF IMU, STEMMA QT (4646): two shoulders, lower back on 400 mm cables,
  mid-back on a 50 mm cable near the electronics (magnetometer interference risk)
- PCA9548 8-channel I2C mux (5626), one BNO055 per channel, all at 0x28, I2C at 100 kHz
- Adalogger FeatherWing (2922): microSD over SPI plus PCF8523 RTC on I2C, CR1220 cell
- FeatherWing Doubler (2890), side by side; 2500 mAh LiPo (328)
- Onboard MAX17048 fuel gauge at 0x36; mux at 0x70
- STEMMA QT port power is switched by GPIO7; SDA GPIO3, SCL GPIO4

Known hardware risk the hardware view should catch: the Adalogger's SD chip-select
default may land on GPIO3, the I2C data line. Unverified; it is exactly the pin-conflict
class the validator targets.

Firmware architecture:
- A sensor task on its own core reads the four sensors every 10 ms (quaternions),
  stamps each tick (microsecond timer, sequence number), encodes one protobuf message
  per tick, and pushes it to one queue per consumer.
- Consumers: SD logger (length-prefixed protobuf frames, hourly files, 24 h ring), BLE
  (setup, pairing, WiFi credentials, control, status, optional low-rate preview), WiFi
  (WebSocket live stream). Separate queues so a slow radio never stalls logging.
- No ZeroMQ on the ESP32 (libzmq is too heavy, and it doesn't run over BLE). FreeRTOS
  queues give the same decoupling; nanopb for protobuf. A PC can republish to ZeroMQ.
- No cloud: data stays on the device and the phone (medical device).

Candidate machines for the multi-machine sketch:
- `device`: Boot, Provisioning, Idle, Recording, Fault
- `imu` (x4 instances, param mux channel): Init, Calibrating, Ready, Error
- `sampler`: periodic, drives reads and emits Tick
- `logger`: Unmounted, Ready, Logging, Full, Error
- `link`: Off, Advertising, BleConnected, WifiJoining, Streaming

Protobuf schema (not yet designed): one wrapper message with variants for sample data,
status, battery, calibration, commands and the trace record, plus a schema version number.

## Observations on the current repo

- Strong, keep as the backbone: totality, locked invariants with sign-off, `SafeHal`
  enforcing limits in generated code, file-scoped tickets, the deterministic gate, MCP
  instead of a chat UI.
- Limits this design addresses: single machine only, flat states only, events carry no
  payload, HAL is a flat list of calls, language set per machine, Rust only.
