# Multi-machine design, v2: the lean cut

Status: design agreed, nothing implemented yet. Written 2026-11-09. This revises
[multi-machine-design.md](multi-machine-design.md) (v1). Where the two disagree, this one
wins. Unchanged parts of v1 still apply: the reasons for rejecting parallel regions, the
QP prior art, and the posture logger reference project.

## What changed from v1

| v1 proposed | v2 decision |
|---|---|
| Four new views: System, Messages, Hardware, Requirements | One new file, `system.yaml`. Hardware becomes resources declared on nodes. Messages and Requirements are not views. |
| Messages view that reads `.proto` files and checks field numbers | Payloads are opaque type names. Protobuf validates itself (protoc, nanopb, prost). stategate never parses `.proto`. |
| Hardware view with parts, buses, board header and HAL trait generation | Hardware independent. A node declares the resources it uses: comm type, address, GPIO pin. stategate checks collisions and scope only. |
| Rate budget check | Deferred. |
| Live view with a trace protocol, a model-hash handshake and derived sequence diagrams | Log replay first: generated code writes trace records, the viewer replays the file. |
| Trace-based translator (model from recorded traces) | Dropped. |
| Requirements view and IEC 62304 report | Deferred. |
| C backend replaces or joins Rust (undecided) | Both backends, held to one conformance contract (below). |

## Scope rule, tightened

v1 let a feature in if stategate could validate it, generate from it, scope tickets with
it, or watch it live. Nearly anything passes one of those. v2 requires **two of the four,
and one of them must be validate.** Everything else stays Markdown or belongs to another
tool.

The second rule: **new features compile down to machines and events.** If a feature can be
expressed as states, events, invariants or ticket scope, it reuses the checks that exist
today instead of adding a new kind of check.

Out of scope, unchanged from v1: chat UI, editor or IDE, BOM, PCB layout, mechanical,
authored sequence diagrams, external issue trackers. Added: part databases, board
support packages, `.proto` parsing, the translator.

## The system file

```yaml
# system.yaml (field names not final)
system: { id: posture_logger }
machines:
  - { id: device,  file: device.yaml }
  - { id: imu,     file: bno055.yaml, instances: 4 }
  - { id: sampler, file: sampler.yaml }
  - { id: logger,  file: logger.yaml }
  - { id: link,    file: link.yaml }
channels:
  - { from: sampler, to: logger, event: TICK, kind: queue, depth: 64, drop: never }
  - { from: sampler, to: link,   event: TICK, kind: queue, depth: 16, drop: oldest }
  - { from: device,  to: logger, event: START, kind: sync }
external: [BUTTON_PRESS, PHONE_CONNECT]   # sent from outside the model: ISR, phone, host
```

Events may name an opaque payload type in the machine file:

```yaml
events:
  - TICK: { payload: posture.v1.Tick }   # a string; the protobuf toolchain checks it
  - START
```

## Checks, all built from existing ones

| Check | How it reuses the FSM | Fails when |
|---|---|---|
| Cross-machine totality | A channel means event E from A is delivered to B, so B's existing totality check must cover E | A machine sends an event no channel carries, a receiver neither handles nor ignores a delivered event, or an inbound event has no sender and is not `external` |
| Channel policy | Plain schema check | A `queue` channel has no `depth` or `drop` |
| Reply timeouts | New invariant kind, `waits_for`, locked like any other invariant | A state waiting for a reply has no `after` transition |
| Emits scope | Services already declare `emits`; actions get the same field | Node code sends an event its node doesn't declare |
| Resource collisions | Resources declared on nodes (below) | Two nodes claim the same pin, or the same address on one bus |
| Resource scope | Ticket scope already lists files; it now lists resources too | Node code touches a pin or bus its node doesn't declare |

Static wait-cycle detection (A waits for B while B waits for A) comes after these, since
it needs request and reply pairs to be declared explicitly.

## Hardware as resources

stategate knows nothing about parts. A node declares what it touches, using three
fields: comm type, address, and GPIO pin.

```yaml
services:
  imu_read: { emits: [TICK], uses: [ { comm: i2c, bus: i2c0.mux0.ch0, addr: 0x28 } ] }
  sd_write: { uses: [ { comm: spi, bus: spi0, cs: GPIO5 } ] }
  qt_power: { uses: [ { comm: gpio, pin: GPIO7 } ] }
```

Pins used by a bus are declared once in `system.yaml` (for example
`buses: [ { id: i2c0, comm: i2c, sda: GPIO3, scl: GPIO4 } ]`) so a collision between a bus
line and a chip select is caught. This is the check that would flag the Adalogger SD
chip select landing on GPIO3. A mux channel is just another bus id (`i2c0.mux0.ch1`), so four
BNO055s at 0x28 on separate channels don't collide.

No board header and no HAL trait are generated from this. `SafeHal` clamps stay where they
are, on HAL calls.

## Two backends, one contract

Rust and C are both supported. They stay consistent because neither backend owns the
semantics: one language-neutral source does, and both backends are tested against it.

1. **One transition table.** `stategate gen` writes `.fsm/table.json` (state, event,
   target, actions in order, ignored pairs). Both backends generate dispatch from it. The
   manifest hash already covers the model, so drift checks stay single-sourced.
2. **One set of test vectors.** Transition tests are generated as data first
   (`.fsm/vectors.json`: start state, event sequence, expected states and action calls),
   then rendered into each language's test framework: `cargo test` for Rust, Unity for C.
   The same vectors must pass in both, so behaviour can't diverge.
3. **Same gate checks, per-language mechanics.**

| Gate check | Rust | C |
|---|---|---|
| no_wildcards | `match` with no `_` arm | `switch` over an enum with no `default`, built with `-Wswitch-enum -Werror` |
| trace_tags | `// @action`, `@service`, `@node` comments | The same comments, unchanged |
| hal_clamp | Generated `SafeHal` wrapper | Generated wrapper functions; node files may not include the raw HAL header (gate greps includes) |
| tests | `cargo test` | CMake + Unity, built with GCC or Clang |

4. **One trace record format.** Both backends write the same bytes (below), so the viewer
   doesn't care which language produced a log.

`language` moves off the machine. The target picks it, so one model can be generated as
Rust for the host simulator and as C for the device.

The cost to accept: every new feature (channels, nesting) is implemented in two
generators. The shared table and vectors keep that cost to rendering, not semantics.

## Live view: log replay

- Generated code calls one trace hook per transition. The record holds machine,
  instance, from, event, to, timestamp and sequence number, about a dozen bytes.
- The log file starts with a header carrying the model hash. The viewer refuses to replay
  a log whose hash doesn't match the model.
- The viewer replays the log on the existing graph, with play, pause and step.
- Transport (UART, BLE, WebSocket) is the firmware's job: anything that can save the bytes
  to a file works. A streaming protocol comes later, only if replay proves too slow to
  use.

## Next steps (in order)

1. `system.yaml`, channels and external events, with cross-machine totality and channel
   policy checks.
2. `waits_for` invariant and `emits` on actions.
3. `uses:` resources with collision and scope checks.
4. Shared `table.json` and `vectors.json`; move the Rust generator onto them.
5. C backend on the same table and vectors, with Unity tests.
6. Trace hook and log replay in the viewer.

Deferred: rate budget, wait-cycle detection, shallow nesting, requirement IDs and the
traceability report, a streaming trace protocol.

## Open questions carried from v1

- Instances: how per-instance parameters (such as a mux channel) reach actions and
  services. Likely a generated per-instance constant passed to node functions.
- Run-to-completion per task, and what happens to an event sent to a machine that is
  mid-transition.
- Does the simulator extend to several machines, and how are queue delays modelled?
- Do channel depths and `external` lists get `locked` and signed off like invariants?
