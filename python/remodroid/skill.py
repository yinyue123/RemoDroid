"""Runs a learned workflow (skills/<task>/run.py) with the exit-code contract from skills/README.md.

    from remodroid.skill import main
    def run(c, query): ...
    if __name__ == "__main__":
        main(run)

Exit codes: 0 done (stdout: {"ok": true, "result", "journal"}); 2 the phone is not in this
skill's entry state; 3 NeedsAI (stdout: reason, failed step, completed steps, fresh UI JSON);
1 any other error (device offline, bad arguments).
"""
import argparse
import inspect
import json
import sys

from .client import ApiError, Client, EntryMismatch, NeedsAI


def _parse_args(argv, fn):
    p = argparse.ArgumentParser(description=(fn.__doc__ or "RemoDroid skill").strip().splitlines()[0])
    p.add_argument("--url", help="API base URL (default: $RD_URL)")
    p.add_argument("--device", help="device id (default: $RD_DEVICE or local)")
    p.add_argument("--token", help="API token (default: $RD_TOKEN)")
    p.add_argument("--from-step", help="resume: skip the steps before this one (they are done)")
    p.add_argument("--trace", help="append every API call to this JSONL file")
    params = [n for n in inspect.signature(fn).parameters if n != "c"]
    for name in params:
        p.add_argument("--" + name.replace("_", "-"), dest=name)
    a = p.parse_args(argv)
    return a, {n: getattr(a, n) for n in params if getattr(a, n) is not None}


def main(fn, argv=None):
    a, kwargs = _parse_args(sys.argv[1:] if argv is None else argv, fn)
    c = Client(a.url, a.device, a.token, trace=a.trace, from_step=a.from_step)
    try:
        result = fn(c, **kwargs)
        if c.skipping:
            raise NeedsAI(f"--from-step {c.skipping} never came up", c.skipping)
        print(json.dumps({"ok": True, "result": result, "journal": c.journal}, ensure_ascii=False, default=str))
        sys.exit(0)
    except NeedsAI as e:
        try:
            ui = c.ui()
        except ApiError as err:
            ui = {"error": err.code, "msg": err.msg}
        print(json.dumps({"ok": False, "needs_ai": not isinstance(e, EntryMismatch),
                          "entry_mismatch": isinstance(e, EntryMismatch), "reason": e.reason, "step": e.step,
                          "detail": e.detail, "completed": c.journal,
                          "resume": "run again with --from-step <next step> after fixing the screen",
                          "ui": ui}, ensure_ascii=False, default=str))
        sys.exit(2 if isinstance(e, EntryMismatch) else 3)
    except ApiError as e:
        print(json.dumps({"ok": False, "error": e.code, "msg": e.msg, "status": e.status, "completed": c.journal}))
        sys.exit(1)
