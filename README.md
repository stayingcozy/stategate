# stategate

Model-first development for code written by AI agents. You design the software
as a state machine in YAML. stategate generates the wiring and the transition
tests, any agent you like writes the logic inside each node, and a
deterministic gate decides what lands. The gate reports status; agents don't.

Status: weekend MVP. The command-line tool, the gate and a read-only viewer
with a simulator work end to end on the posture wearable example.

![stategate-view showing the posture machine, the gate result and the simulator](docs/viewer.png)

## Build

Needs a C11 compiler, CMake, libyaml (`libyaml-dev` on Debian/Ubuntu,
`brew install libyaml` on macOS) and, for the generated code, Rust. The viewer
fetches raylib 5.5 at configure time; on Linux it also needs the X11 and GL
headers (`libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev`).
Pass `-DSTATEGATE_VIEWER=OFF` to build only the command-line tool.

```sh
cmake -S . -B build && cmake --build build
sudo cmake --install build      # installs stategate (alias fsm) and stategate-view
scripts/redpaths.sh             # proves the gate turns red for each broken rule
```

## Ten-minute walkthrough

```sh
cd examples/posture
stategate validate               # checks 1-3 on posture.yaml
stategate gen                    # Rust wiring, 48 generated tests, manifest
stategate gate                   # all 11 checks; writes .fsm/gate.json
cargo run --example sim -- 15000 POWER_ON CALIBRATED
```

The simulation runs Step 0 on fake hardware:

```
    0.0s  posture.Off --POWER_ON--> posture.Calibrating
    0.0s  posture.Calibrating --CALIBRATED--> posture.Monitoring
   10.0s  posture.Monitoring --SLOUCH--> posture.Slouching
   13.0s  posture.Slouching --GRACE_ELAPSED--> posture.Nudging
   13.0s  NUDGE
   14.5s  posture.Nudging --NUDGE_DONE--> posture.Slouching
```

## A new project, end to end

Every step is a command, so a future app or an MCP server can drive the same
flow. `you` steps happen at your terminal; `AI` steps work with any assistant.

```sh
# you: set up and design
mkdir blinky && cd blinky && git init
#   write blinky.yaml (see examples/posture/posture.yaml)
stategate init                 # AGENTS.md, CLAUDE.md, CHANGELOG.md, pre-commit hook
stategate validate             # checks 1-3
stategate-view .               # live view while you edit
stategate gen                  # Rust wiring, transition tests, node stubs
stategate signoff              # only when the model has locked items
git add -A && git commit -m "model"

# no AI: ticket skeletons
stategate ticket gen           # one draft per stub node: node, step and files filled in

# AI: fill in each draft (the ticket-writer role in AGENTS.md)
stategate ticket fill T001 --title "..." --goal "..."    # draft becomes open
stategate ticket check
git add -A && git commit -m "tickets"

# you: approve or skip each one
stategate ticket start         # approved tickets queue; the first one starts

# AI: implement the active ticket, until green (the implementer role)
stategate ticket prompt        # goal, node, allowed files, current code, last errors
#   ...the AI edits the allowed files...
stategate ticket run           # green: commit, done, next ticket starts
                               # red: errors saved to the ticket; prompt again
# you, after 3 reds: stategate ticket split T001 "..." "..."
```

## The viewer

```sh
stategate-view examples/posture
```

The left two-thirds draws the machine from the layout saved in the model.
The right third shows the last gate result from `.fsm/gate.json`, the stub
count, the active ticket, a simulator and the step ladder. Click an event to
fire it: the active state lights up and the transition it took turns blue;
ignored events say so. The viewer reloads the model and the gate result when
either file changes, keeps showing the last valid model while the file has
errors, and never writes anything. Drag to pan, scroll to zoom, F to fit.

It is read-only for now: you or your agent edit the YAML. `--send EV1,EV2`
fires events at start-up and `--screenshot out.png` saves one frame and exits,
which CI uses as a smoke test.

## How work flows

1. **You design the model** (`posture.yaml`). Every (state, event) pair is
   either handled under `on` or listed under `ignore`; a missing pair is an
   error. Safety rules are `locked` invariants.
2. **You sign off locked items** with `stategate signoff`. Changing one later
   turns the gate red until you sign off again. A model with nothing locked
   needs no sign-off.
3. **stategate generates** `src/gen/`, `tests/transitions.rs`,
   `tests/invariants.rs` and the manifest. It scaffolds one file per action
   and service once, tagged with node IDs, and never overwrites them.
4. **`stategate ticket gen` drafts the tickets**, with no AI: one per stub
   action or service in the active step, with the node, step and files
   (the node's file and `tests/logic_<node>.rs`) filled in.
5. **A ticket-writer AI fills them in** with `stategate ticket fill <id>
   --title ... --goal ...`, which turns a draft into an open ticket. It can't
   change the node, step or files. `stategate ticket new` writes a ticket
   from scratch, checked against the model the same way.
6. **You approve them** with `stategate ticket start`. It walks through every
   open ticket, showing its goal, nodes and files, and you type `approve` or
   `skip` for each at a terminal. Approved tickets queue in id order and the
   first one starts. `stategate ticket start T002` approves and starts one.
7. **An implementer AI works the active ticket.** `stategate ticket prompt`
   prints everything it needs as one block of Markdown: the goal, where the
   node runs, the allowed files and their code, and the last errors.
   `stategate ticket run` runs the gate: green commits the work, marks the
   ticket done and starts the next one; red saves the errors into the ticket
   for the next prompt. Three reds lock the ticket until you approve a split
   with `stategate ticket split`. Hand-setting a ticket to `active`, or
   changing one after you approved it, turns the gate red or keeps it from
   starting.
8. **The gate decides.** `ticket run` and the pre-commit hook both run all
   11 checks; `stategate gate` runs them without committing anything.

## The gate

Checks run in order and stop at the first failure.

| # | Check | Fails when |
| --- | --- | --- |
| 1 | schema | the YAML has unknown keys, wrong types or missing fields |
| 2 | statechart | no single initial state, an unreachable state, a dangling target, or a pair neither handled nor ignored |
| 3 | invariants | a locked invariant doesn't hold on the model (e.g. a state ignores FAULT) |
| 4 | signoff | locked items differ from what the architect signed (nothing locked: passes) |
| 5 | drift | a generated file was edited by hand, or the model changed without `gen` |
| 6 | no_wildcards | generated code contains a `_ =>` match arm |
| 7 | trace_tags | an action or service file lacks its `@action`/`@service`/`@node` tags |
| 8 | stubs | a locked item is a stub; with `--release`, any stub remains |
| 9 | ticket_scope | a file outside the active ticket changed, the ticket wasn't approved or changed after approval, or it is locked |
| 10 | changelog | `CHANGELOG.md` has no `## <ticket>` entry naming the ticket's node IDs |
| 11 | tests | `cargo test` fails |

## Invariants the gate can verify

| Kind | Example | Enforced by |
| --- | --- | --- |
| `every_state_handles` | any FAULT goes to Off | check 3 on the model, plus a generated test per state |
| `max_time_in_state` | Nudging lasts at most 2000 ms | a generated watchdog in `Machine::tick`, plus a generated test |
| `hal_clamp` | intensity is at most 3 | the generated `SafeHal`, the only HAL handle node logic receives, plus a generated test |

## Known limits

- Sign-off, ticket approval and ticket splits ask for typed confirmation at a
  terminal. That stops an agent from approving by accident, not one that sets
  out to fake a terminal or to compute the approval hash itself. Real
  protection (signed commits, a key only you hold) comes later.
- Ticket files are exempt from the scope check, because they change as the
  queue moves; their approval hashes guard them instead. That means an agent
  could edit the active ticket's failure counter. Moving the counter out of
  reach comes later.
- Flat states only; no nesting, parallel regions or history yet.
- The viewer's simulator follows the model's transition table in C, separately
  from the generated Rust. Both read the same table, and the generated tests
  cover the Rust side.
- Rust is the only target language so far; C is next.
