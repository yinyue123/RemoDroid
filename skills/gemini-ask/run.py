"""Ask Gemini (the Google app's assistant) a question and return its answer text."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))

from remodroid import EntryMismatch, find  # noqa: E402
from remodroid.skill import main  # noqa: E402

GEMINI = "com.google.android.apps.bard"          # launches Gemini inside the Google app
HOST = "com.google.android.googlequicksearchbox"
INPUT = {"rid": "assistant_robin_input_collapsed_text_half_sheet"}
SEND = [{"desc": "发送"}, {"desc": "Send"}, {"desc": "Send message"}]          # no resource id; label per locale
STOP = [{"desc": "停止生成回答"}, {"desc": "Stop response"}, {"desc": "Stop generating"}]


def first(ui, candidates):
    for sel in candidates:
        if find(ui, **sel):
            return sel
    return None


def answer(ui, question):
    """Assistant text after the last user message that matches the question."""
    nodes = ui["nodes"]
    start = max((i for i, n in enumerate(nodes) if n.get("rid", "").endswith("assistant_robin_user_message_text")
                 and n.get("text") == question), default=None)
    if start is None:
        return None
    return [n["text"] for n in nodes[start + 1:] if n.get("rid", "").endswith(":id/assistant_robin_text") and n.get("text")]


def run(c, question, timeout="60"):
    """Ask Gemini a question."""
    c.launch(GEMINI, step="launch", wait={"for": "idle", "timeout_ms": 3000})
    ui = c.ui()
    if ui.get("package") != HOST and not c.skipping:
        raise EntryMismatch(f"Gemini did not open ({ui.get('package')}); is the phone unlocked?", "launch")
    c.require(ui, has=INPUT, step="launch")

    c.tap(step="focus", wait={"for": "ui_changed", "timeout_ms": 3000}, **INPUT)
    c.set_text(question, step="type", **INPUT)
    send = first(c.ui(), SEND)
    if send is None:
        c.require(has=SEND[0], step="send")  # no send button: let the AI look
    c.tap(step="send", side_effect=True, **(send or SEND[0]))  # the question is sent: never repeat this step

    # The reply streams in: done when our question has an answer and the stop button is gone.
    deadline = time.time() + float(timeout)
    while True:
        ui = c.ui()
        text = answer(ui, question)
        if text and not first(ui, STOP):
            return {"question": question, "answer": "\n".join(text)}
        if time.time() > deadline:
            c.require(ui, has={"rid": "assistant_robin_text"}, step="answer")  # no answer: let the AI look
            return {"question": question, "answer": "\n".join(text or []), "partial": True}
        time.sleep(1)


if __name__ == "__main__":
    main(run)
