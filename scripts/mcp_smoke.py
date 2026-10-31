#!/usr/bin/env python3
"""Drives `stategate mcp` through the whole AI loop on a copy of the posture
example, the way an MCP client would: ticket writer fills drafts, the
architect approves (through a pseudo-terminal), the implementer gets the task,
writes files, runs the gate red, fixes it, and runs it green.

Usage: scripts/mcp_smoke.py [path/to/stategate]
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SG = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "stategate"))
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok    {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}")
        if detail:
            print("        " + str(detail).replace("\n", "\n        "))


class Client:
    def __init__(self, cwd):
        self.p = subprocess.Popen([SG, "mcp"], cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        self.next_id = 0

    def request(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()
        reply = json.loads(self.p.stdout.readline())
        assert reply.get("id") == self.next_id, reply
        return reply

    def notify(self, method):
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.p.stdin.flush()

    def call(self, name, **args):
        r = self.request("tools/call", {"name": name, "arguments": args})["result"]
        return r["content"][0]["text"], r["isError"]

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)


def sh(cmd, cwd, stdin=None):
    return subprocess.run(cmd, cwd=cwd, shell=True, input=stdin, text=True, capture_output=True)


def status_of(cwd, tid):
    for line in open(os.path.join(cwd, ".fsm", "tickets", tid + ".yaml")):
        if line.startswith("status: "):
            return line.split(": ", 1)[1].strip()
    return "?"


def main():
    work = tempfile.mkdtemp()
    os.environ.setdefault("CARGO_TARGET_DIR", os.path.join(work, "target"))
    try:
        d = os.path.join(work, "case")
        shutil.copytree(os.path.join(ROOT, "examples", "posture"), d,
                        ignore=shutil.ignore_patterns("target", "Cargo.lock"))
        for f in os.listdir(os.path.join(d, ".fsm", "tickets")):
            os.remove(os.path.join(d, ".fsm", "tickets", f))
        with open(os.path.join(d, "CHANGELOG.md"), "w") as f:
            f.write("# Changelog\n")
        sh("git init -q . && git config user.name t && git config user.email t@t && git add -A && git commit -qm base", d)
        sh(f"{SG} ticket gen", d)
        sh("git add -A && git commit -qm drafts", d)

        print("stategate mcp smoke test")
        c = Client(d)
        init = c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                        "clientInfo": {"name": "smoke", "version": "0"}})
        c.notify("notifications/initialized")
        check("initialize names the server and offers tools",
              init["result"]["serverInfo"]["name"] == "stategate" and "tools" in init["result"]["capabilities"], init)
        names = [t["name"] for t in c.request("tools/list")["result"]["tools"]]
        check("tools are the AI steps only",
              names == ["status", "list_drafts", "fill_ticket", "get_task", "read_file", "write_file", "run_gate"], names)
        check("no tool approves, signs off or splits",
              not any(n in names for n in ("signoff", "approve", "start", "split")))

        text, err = c.call("list_drafts")
        check("list_drafts shows the drafts with their files", not err and "T001" in text and "src/actions/start_nudge.rs" in text, text)
        text, err = c.call("fill_ticket", id="T001", title="Nudge", goal="Log NUDGE and request intensity 5.")
        check("fill_ticket opens a draft", not err and status_of(d, "T001") == "open", text)
        text, err = c.call("get_task")
        check("get_task without an active ticket says so", err and "no active ticket" in text, text)

        r = sh(f"printf 'approve\\n' | script -qec '{SG} ticket start T001' /dev/null", d)
        check("the architect approves at a terminal", status_of(d, "T001") == "active", r.stdout + r.stderr)

        text, err = c.call("get_task")
        check("get_task returns the brief with the current code", not err and "## Goal" in text and "// @action start_nudge" in text, text[:400])
        text, err = c.call("write_file", path="src/sneaky.rs", content="pub fn x() {}\n")
        check("write_file refuses a file outside the ticket", err and "does not allow" in text and not os.path.exists(os.path.join(d, "src/sneaky.rs")), text)
        text, err = c.call("write_file", path="../escape.rs", content="x")
        check("write_file refuses a path that leaves the project", err and ".." in text, text)
        text, err = c.call("write_file", path=".fsm/tickets/T001.yaml", content="x")
        check("write_file refuses .fsm/", err, text)
        text, err = c.call("read_file", path="posture.yaml")
        check("read_file reads the model", not err and "machine:" in text, text[:200])

        nudge = open(os.path.join(d, "src/actions/start_nudge.rs")).read().replace("// @stub", "// was a stub")
        nudge = "\n".join(l for l in nudge.splitlines() if "was a stub" not in l) + "\n"
        text, err = c.call("write_file", path="src/actions/start_nudge.rs", content=nudge)
        check("write_file writes an allowed file", not err, text)
        text, err = c.call("run_gate")
        check("run_gate red: errors returned and saved to the ticket",
              err and "GATE RED" in text and status_of(d, "T001") == "active", text[-600:])
        text, err = c.call("get_task")
        check("get_task shows the last errors", "The last gate run failed" in text and "## T001" in text, text[-600:])
        text, err = c.call("write_file", path="CHANGELOG.md", content="# Changelog\n\n## T001\nstart_nudge logs NUDGE.\n")
        text, err = c.call("run_gate")
        log = sh("git log -1 --format=%s", d).stdout.strip()
        check("run_gate green: committed and done", not err and "GATE GREEN" in text and status_of(d, "T001") == "done" and log == "T001: Nudge",
              text[-600:] + "\nlast commit: " + log)
        text, err = c.call("status")
        check("status lists the tickets", not err and "T001" in text and "done" in text, text)

        bad = c.request("tools/call", {"name": "signoff", "arguments": {}})
        check("an unknown tool is an error", "error" in bad, bad)
        c.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
