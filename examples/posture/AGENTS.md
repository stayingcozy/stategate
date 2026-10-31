# Rules for coding agents

This project is built with stategate. The model file `posture.yaml` is the single source of
truth: code is generated from it, and changes flow model to code only. A deterministic
gate decides whether work lands. Your description of your work does not count;
only `stategate gate` does.

There are two agent roles. Do one per session and say which one you are doing.

## Ticket writer

You turn the model's next step into small tickets. You never write code.

- Write each ticket with `stategate ticket new <id> --title "..." --step <step>
  --node <node id> --file <path or glob>`, repeating `--node` and `--file` as needed.
  It checks the ticket against the model and writes it as `open`.
- Keep each ticket small: one behaviour, the fewest files that can deliver it, and the
  test files that prove it (usually `tests/logic_*.rs`).
- Never grant generated files, the model or `.fsm/`; `ticket new` refuses them.
- Run `stategate ticket check` when you are done, then stop. Only the architect approves
  tickets, at a terminal with `stategate ticket start`. Approved tickets then run one at
  a time in id order. Never set a ticket's status, base or approval yourself, and never
  edit a ticket that is not `open`.

## Implementer

1. Work only on the ticket in `.fsm/tickets/` whose status is `active`. Touch only the
   files its `files:` list allows, plus `CHANGELOG.md`. If there is no active ticket, stop:
   the architect has not approved one yet.
   When the gate is green, commit your work, then run `stategate ticket done <id>`. It
   starts the next approved ticket, if any; carry on with that one the same way.
2. Never edit generated files (anything starting with `@generated`), the model, or
   anything else in `.fsm/`, including the ticket. The gate detects it.
3. Keep the trace tags at the top of each action and service file (`@action`,
   `@service`, `@node`). Leave `@stub` in place unless the ticket says to implement
   that item for real.
4. Run `stategate gate` after every change. Report its output as it is; never claim
   something works when the gate says otherwise.
5. Add a `## <ticket id>` entry to `CHANGELOG.md` that names every node ID in the ticket.
6. After 3 failed gate runs the ticket locks. Stop. Propose how to split it into
   smaller steps; the architect decides.
7. Never weaken, bypass or stub a locked item (safety invariants and locked actions).
   If one is in your way, stop and ask.
8. If the model looks wrong, propose a model change to the architect. Do not work
   around it in code.
