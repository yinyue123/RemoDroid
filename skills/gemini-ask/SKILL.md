---
name: gemini-ask
description: Ask Gemini (the assistant in the Google app) one question on the phone and return its text answer.
package: com.google.android.googlequicksearchbox (launched as com.google.android.apps.bard)
app_version: verified on SO-51A, Android 12, Chinese Gemini UI ("Gemini Flash")
revision: 1
---

# Ask Gemini

Learned from one AI-driven exploration (trace: launch → tap the input → set_text → tap 发送) and
compiled into a replay with a check after each step.

```sh
PYTHONPATH=python python3 skills/gemini-ask/run.py --question "1+1等于几？只回答数字。" [--timeout 60]
```

- **Starts from** any unlocked screen. It cannot draw the lock pattern, so on a locked phone it exits 2.
- **Steps:** `launch` → `focus` → `type` → `send` (**side effect**: the question is sent; a resume
  never repeats it) → waits until the answer stops streaming.
- **Returns** `{"question", "answer"}`, the answer text after this question. `"partial": true`
  means it was still streaming when `--timeout` ran out.
- **Hands over to the AI (exit 3)** when the input or send button is missing (a sign-in prompt, an
  update dialog, another locale's labels) or no answer appears.
- The send and stop buttons have no resource ids, so they are matched by label (Chinese and English
  are listed). Add labels for other locales here.
