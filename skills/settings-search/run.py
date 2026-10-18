"""Search Android Settings for a setting by name and return the result titles."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))

from remodroid import EntryMismatch, find  # noqa: E402
from remodroid.skill import main  # noqa: E402

SETTINGS = "com.android.settings"
SEARCH_PKG = "com.android.settings.intelligence"  # the search page, inside the Settings task
# Search box on AOSP / Pixel / Sony and most Android 12+ builds, in the order we try them.
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
    ui = c.ui()
    if ui.get("package") not in (SETTINGS, SEARCH_PKG) and not c.skipping:
        raise EntryMismatch(f"Settings is not in front ({ui.get('package')})", "launch")

    field = first(ui, QUERY_FIELD)
    if field is None:  # the main Settings page: open the search page (Settings may reopen on it)
        bar = first(ui, SEARCH_BAR)
        if bar is None:
            c.require(ui, has=SEARCH_BAR[0], step="open_search")  # raises NeedsAI with the reason
        c.tap(step="open_search", wait={"selector": {"rid": "search_src_text"}, "timeout_ms": 3000},
              **(bar or SEARCH_BAR[0]))
        field = first(c.ui(), QUERY_FIELD)
        if field is None:
            c.require(has=QUERY_FIELD[0], step="enter_query")
    c.set_text(query, step="enter_query", **(field or QUERY_FIELD[0]))

    # Results load after the text changes: wait for the list, or for "No results" (a valid answer,
    # e.g. a Chinese query on an English phone), not just for any change.
    deadline = time.time() + 8
    while True:
        ui = c.ui()
        titles = [n["text"] for n in find(ui, rid="title", within={"rid": "list_results"}) if n.get("text")]
        if titles or find(ui, text="No results") or c.skipping:
            return {"query": query, "results": titles[:20]}
        if time.time() > deadline:
            c.require(ui, has={"rid": "list_results"}, step="results")  # neither: let the AI look
            return {"query": query, "results": []}
        time.sleep(0.5)


if __name__ == "__main__":
    main(run)
