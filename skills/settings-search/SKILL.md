---
name: settings-search
description: Open Android Settings and search for a setting by name (e.g. "wifi", "battery"); returns the result titles.
package: com.android.settings
app_version: any (verified on SO-51A, Android 12; other phones may use other resource ids)
revision: 3
---

# Search Android Settings

The worked example for `skills/README.md`. It shows the pieces every skill has: an
entry state, named steps, a check after each step, a side-effect flag and a final check.

```sh
PYTHONPATH=python python3 skills/settings-search/run.py --query wifi
```

- **Starts from** any screen: it launches Settings itself, then requires Settings in the
  foreground.
- **Steps:** `launch` → `open_search` (skipped when Settings reopens on its search page) → `enter_query` → `results` (waits for the result list, which loads after the text changes).
- **Returns** `{"query": …, "results": [titles]}`; an empty list when Settings says "No results" (e.g. a Chinese query on an English phone).
- **Hands over to the AI (exit 3)** when the search box or the results do not show up.
  Vendors move them, so the first run on a new phone may need the AI to adapt the
  selectors.
