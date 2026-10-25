# Rules for coding agents

This project is built with stategate. The model file `posture.yaml` is the single source of
truth: code is generated from it, and changes flow model to code only. A deterministic
gate decides whether work lands. Your description of your work does not count;
only `stategate gate` does.

1. Work only on the ticket in `.fsm/tickets/` whose status is `active`. Touch only the
   files its `files:` list allows, plus `CHANGELOG.md`.
2. Never edit generated files (anything starting with `@generated`), the model, or
   anything else in `.fsm/`. The gate detects it.
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
