"""Search Android Settings for a setting by name and return the result titles."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))

from remodroid import find  # noqa: E402
from remodroid.skill import main  # noqa: E402

SETTINGS = "com.android.settings"
# Search box on AOSP / Pixel / most Android 12+ builds, in the order we try them.
SEARCH_BAR = [{"rid": "search_action_bar"}, {"rid": "search_bar"}, {"desc": "Search settings"},
              {"label": "Search settings"}]
QUERY_FIELD = [{"rid": "search_src_text"}, {"rid": "open_search_view_edit_text"}, {"cls": "EditText"}]


def first(ui, candidates):
    for sel in candidates:
        if find(ui, **sel):
            return sel
    return None


def run(c, query):
    """Search Android Settings for a setting."""
    c.launch(SETTINGS, step="launch", wait={"for": "idle", "timeout_ms": 3000})
    ui = c.require(package=SETTINGS, step="launch", entry=True)

    bar = first(ui, SEARCH_BAR)
    if bar is None:
        c.require(ui, has=SEARCH_BAR[0], step="open_search")  # raises NeedsAI with the reason
    c.tap(step="open_search", wait={"for": "ui_changed", "timeout_ms": 3000}, **(bar or SEARCH_BAR[0]))

    ui = c.ui()
    field = first(ui, QUERY_FIELD)
    if field is None:
        c.require(ui, has=QUERY_FIELD[0], step="enter_query")
    c.set_text(query, step="enter_query", wait={"for": "ui_changed", "timeout_ms": 4000}, **(field or QUERY_FIELD[0]))

    ui = c.ui()
    titles = [n["text"] for n in find(ui, rid="title") if n.get("text")]
    if not titles:
        c.require(ui, has={"rid": "title"}, step="results")  # no result list: let the AI look
    return {"query": query, "results": titles[:20]}


if __name__ == "__main__":
    main(run)
