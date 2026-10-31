# Rules for coding agents

This project is built with stategate. The model file `posture.yaml` is the single source of
truth: code is generated from it, and changes flow model to code only. A deterministic
gate decides whether work lands. Your description of your work does not count;
only `stategate gate` does.

There are two agent roles. Do one per session and say which one you are doing.

## Ticket writer

You describe the work in tickets. You never write code.

- `stategate ticket gen` (already run by the architect, or run it yourself) writes one
  `draft` ticket per stub node with its node, step and files filled in.
- For each draft, run `stategate ticket fill <id> --title "..." --goal "..."`. The goal
  says what the node must do for this step, in a few sentences an implementer can test.
  A filled draft becomes `open`. You cannot change a ticket's node, step or files.
- For work `ticket gen` doesn't cover, write a ticket with `stategate ticket new <id>
  --title "..." --goal "..." --step <step> --node <node id> --file <path or glob>`. Keep
  it small: one behaviour, the fewest files, and the tests that prove it.
- Run `stategate ticket check`, then stop. Only the architect approves tickets, at a
  terminal with `stategate ticket start`. Never set a ticket's status, base or approval.

## Implementer

1. Run `stategate ticket prompt`. It prints the active ticket: goal, node, the files you
   may change, their current code and the errors from the last failed run. Touch only
   those files plus `CHANGELOG.md`. If there is no active ticket, stop.
   When you are done, run `stategate ticket run`. Green commits the work, marks the
   ticket done and starts the next approved one: run `ticket prompt` again and carry on.
   Red saves the errors into the ticket: run `ticket prompt`, fix them, and run again.
2. Never edit generated files (anything starting with `@generated`), the model, or
   anything else in `.fsm/`, including the ticket. The gate detects it.
3. Keep the trace tags at the top of each action and service file (`@action`,
   `@service`, `@node`). Remove `// @stub` once the node's logic is real.
4. `stategate gate` runs the same checks without committing. Report gate output as it
   is; never claim something works when the gate says otherwise.
5. Add a `## <ticket id>` entry to `CHANGELOG.md` that names every node ID in the ticket.
6. After 3 failed gate runs the ticket locks. Stop. Propose how to split it into
   smaller steps; the architect decides.
7. Never weaken, bypass or stub a locked item (safety invariants and locked actions).
   If one is in your way, stop and ask.
8. If the model looks wrong, propose a model change to the architect. Do not work
   around it in code.
