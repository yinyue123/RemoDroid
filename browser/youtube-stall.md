# YouTube Music stops at about 2:05 — a watchdog in rd-shell (plan, for review)

Contents

1. What happens
2. How it was found
3. The fix: a watchdog
4. How the script gets into the page
5. The script
6. What changes in rd-shell
7. Failures and limits
8. Decisions for you
9. Tests (the real browser-oect, the real YouTube Music, the real HomePod)
10. Order of work

---

## 1. What happens

On browser-oect, YouTube Music sometimes freezes about two minutes into a song: the play button turns into a
spinner, the clock stops between **2:03 and 2:06**, and it stays like that until someone touches it (once for
two hours, 11-08 10:14 → 12:21). Pause/play doesn't help. About two minutes later the HomePod drops the
AirPlay session (`mod.raop-sink: error -32`), because nothing is playing to it any more.

It is YouTube's player, not the network, the proxy or AirPlay. **On some songs the player counts its
read-ahead from 0:00 instead of from where the song is.** Its read-ahead target is 120 s, so once it holds the
song's first ~120 s of video it believes it is 120 s ahead and asks for nothing more. Playback reaches the end
of what it has; Chrome plays about 3 s more audio past the end of the video (its video-underflow grace) and
stops.

```
            0:00                    1:00                    2:00 2:03          3:45
 video      ██████████████████████████████████████████████████░░░░░░░░░░░░░░░░░░░   fetched to ~2:00, then nothing
 audio      ████████████████████████████████████████████████████████████░░░░░░░░░   fetched to 2:30, then nothing
 playing    ───────────────────────────────────────────────────────▶ ✕              video runs dry, +3 s audio, stop
 player     "I am 120 s ahead"  (it measures from 0:00, not from 2:00)
```

## 2. How it was found

From the netlog (`--log-net-log=/tmp/rd-netlog.json`, on since 2026-11-02) and by driving the screen with
xdotool, on 2026-11-08:

| what | showed |
|---|---|
| googlevideo requests | all `200 OK`, complete; none stuck, none failed. During a stall there are simply none |
| `/api/stats/qoe` | position frozen at 123–127 s, state `B` (buffering); the buffered end frozen ~3 s before it |
| `rbuf` on every videoplayback request (the player's own "seconds buffered ahead") | normal song: buffered end − position (42 s at 0:18 when the audio reached 0:60). Bad song: **buffered end − 0** (30 s, 60 s, 90 s, 120 s), from the first requests on |
| all 65 plays in the netlog | 7 counted from 0:00; every one stopped fetching at 150 s of audio. The 58 others counted right and played to the end |
| pause/play on the spinner | nothing: no request |
| a seek within what is buffered (tap on the progress bar, 17–20 s back), twice | the next chunks within 7 s, `rbuf` right again (29.9 s at 2:00 for audio to 2:30), played to the end |

Why YouTube's code counts from 0 on those songs is not known: it is inside its minified player, and the kiosk
gives no DevTools. Both good and bad songs had been preloaded as "next", and both had the 10–15 s CPU burst
at the song change.

## 3. The fix: a watchdog

Do what the tap did, by itself: when the player has been stuck long enough with nothing coming, seek back 2 s.

```
 every 2 s, in the YouTube Music page
   ┌───────────────────────────────────────────────────────────────┐
   │ video playing (not paused, not ended), readyState < 3         │
   │ currentTime unchanged for 8 s                                 │
   │ buffered end − currentTime < 0.5 s   (it ran out)             │
   └──────────────┬────────────────────────────────────────────────┘
                  │ all three
                  ▼
   currentTime = max(t − 2, buffered start)       ── a seek, inside what is buffered
   record it: time, song, position
                  │
                  ▼  next checks
   moving again within 15 s?  yes → "recovered"   no → try again after 30 s, at most 3 per song
```

It hides YouTube's bug; it doesn't fix it. What the listener gets is a short jump back ~2 minutes into about
one song in ten, instead of silence.

## 4. How the script gets into the page

rd-shell holds the browser's only DevTools connection (`--remote-debugging-pipe`) and today uses browser-level
`Target` calls only, so that a page has nothing to notice (rd-shell.c's header says so). The watchdog needs to
be inside the page, so that rule has to give a little — D1 in §8.

★ For tabs on `music.youtube.com` only (as tested in T1, §9 — the first design used
`Page.addScriptToEvaluateOnNewDocument` and `Runtime.addBinding`; neither works without `Runtime.enable`):

```
 Target.targetCreated / targetInfoChanged   url host = music.youtube.com, not attached yet
   └▶ Target.attachToTarget {targetId, flatten: true}                    → sessionId
 every 10 s, per attached tab
   └▶ Page.createIsolatedWorld {frameId: targetId, worldName: "rd"}      → the same world each time, by name
        └▶ Runtime.evaluate {expression: script, contextId}              installs the watchdog if this document
                                                                         lacks it; returns its new records
 Target.detachedFromTarget / targetDestroyed / navigates off the host   → forget the session
```

- A page's main frame id is its target id, so no `Page.getFrameTree`.
- A reload or a new document gets the watchdog again within 10 s; a stall takes minutes to build.
- Within one session, `createIsolatedWorld` with the same name returns the same world (same context id,
  same variables). Another session gets a world of its own: rd-shell keeps one session per tab.

- **An isolated world** is what an extension's content script gets: the same DOM, separate JavaScript. The
  page's scripts can't see the watchdog's variables or timers, and the watchdog can't see theirs. So it uses
  only the `<video>` element's own properties (D2).
- **No `Runtime.enable`, no `Debugger`, no `Console`.** `Runtime.enable` is the call bot checks detect (it
  makes the console serialise objects). `Page.addScriptToEvaluateOnNewDocument` and `Runtime.evaluate` with a
  context id work without it.
- Flattened sessions travel on the same pipe: messages carry a `sessionId`; rd-shell's loop already reads
  every message.

The `Page.getFrameTree` call that gives the main frame's id is the one other call. Nothing is sent to other
tabs or sites.

## 5. The script

About 40 lines, in the isolated world:

```js
(() => {
  if (window.__rdWatch) return; window.__rdWatch = 1;     // this world only; once per document
  let last = -1, still = 0, tries = 0, lastTry = 0, src = '';
  setInterval(() => {
    const v = document.querySelector('video');
    if (!v || v.paused || v.ended) { still = 0; return; }
    if (v.currentSrc !== src) { src = v.currentSrc; tries = 0; }    // a new song
    const t = v.currentTime, b = v.buffered;
    const end = b.length ? b.end(b.length - 1) : 0;
    still = (t === last && v.readyState < 3) ? still + 2 : 0; last = t;
    if (still < 8 || end - t > 0.5 || tries >= 3 || Date.now() - lastTry < 30000) return;
    tries++; lastTry = Date.now(); still = 0;
    v.currentTime = Math.max(t - 2, b.length ? b.start(0) : 0);
    report({ at: t, end, tries, title: document.title });
  }, 2000);
})();
```

`report` pushes the event onto a list in the "rd" world; the 10 s `Runtime.evaluate` returns and empties it,
and rd-shell logs each one to the journal
(`rd-shell: youtube: stuck at 123.5 s (buffered to 120.6), sought back, try 1 — <title>`), then
`recovered` or `still stuck` 15 s later.

## 6. What changes in rd-shell

| where | change |
|---|---|
| `T` (the tabs) | per tab: the session id, the frame id, attached or not |
| `tab_info` | on a `music.youtube.com` url and no session: `Target.attachToTarget`; on leaving the host: `Target.detachFromTarget` |
| `cdp_message` | answers to the attach, the frame tree and the isolated world, matched by id; `Target.detachedFromTarget`; the binding event or the counter's answer → `LOG` |
| `cdp_send` | an optional `sessionId` |
| the header comment | the rule becomes "nothing attaches to a page, except YouTube Music tabs for the stall watchdog (youtube-stall.md), in an isolated world, with no Runtime.enable" |
| `/data/browser.conf`, `browser.conf.example` | `youtube_watchdog = 1` (D4) |
| `remote-browser-plan.md` | a line under "Controller bar", pointing here |

Nothing else in the image changes. It ships as a new image from the browser-image Action, installed on
browser-oect with `reinstall` and `keep_persist`.

## 7. Failures and limits

| case | what happens |
|---|---|
| the user pauses | `paused` → nothing |
| a slow network, a real buffering wait | after 8 s with nothing buffered ahead, a 2 s seek back: harmless, it asks again. At most 3 a song |
| an ad, a song change, the end | `ended` or a new `currentSrc` resets the count; an ad that buffers is treated like a song |
| YouTube changes its player | the script only reads the `<video>` element; it can stop helping, it can't break playback |
| the seek doesn't reset the player's count (a native seek is not the tap YouTube's bar makes) | step 1 tests exactly this before anything else; if it fails, D2's other option |
| the page detects the session | the reason for D1. §9's T5 checks what a detection page can see |
| DevTools pipe closes (browser restart) | sessions go with it; on the next start the tabs are seen again and attached again |
| the user browses YouTube Music in two tabs | each tab gets its own session |

## 8. Decisions for you

The first option is my recommendation (★).

| # | question | ★ | other |
|---|---|---|---|
| D1 | how the script gets into the page | a DevTools session on `music.youtube.com` tabs only, the script in an isolated world, no `Runtime.enable` | **an extension** with a content script: no DevTools session, but the browser must be started with `--load-extension` (recent Chromium needs a feature switched off for that) and an installed extension is visible in the profile. **From outside only**: rd-shell watches PipeWire for Chrome's stream going away while the page says it is playing, then taps the progress bar with XTest as a person would — nothing in the page at all, but it can't tell a stall from other silences, and the tap's place depends on the layout |
| D2 | the nudge | `video.currentTime = t − 2` from the isolated world | YouTube's own `movie_player.seekTo(t − 2)`: the same call the progress bar makes, so surely right, but only reachable from the page's own world, where its scripts can see the watchdog |
| D3 | where | `music.youtube.com` | also `www.youtube.com`: the same player; not seen to stall there, because nobody listens there |
| D4 | on or off | `youtube_watchdog = 1` in `/data/browser.conf`, on by default | off by default |
| D5 | the netlog | keep it until §9 passes (it is how a stall is seen and timed), then remove `--log-net-log` from `args` | remove now: it is 275 MB in RAM and grows ~20 MB an hour |

## 9. Tests (the real browser-oect, the real YouTube Music, the real HomePod)

The bug can't be made on demand: it comes about once in ten songs. So the main test is listening, with the
netlog on and the watchdog's journal lines, and every stall is checked against both.

```
 T1 the nudge works      before any rd-shell code: in a stall, set video.currentTime = t − 2 by hand (the
                         same as the script, through a one-off DevTools session) and see the next requests'
                         rbuf go right. If it doesn't, D2's other option, and T1 again
 T2 real stalls          3–4 hours of the radio to the HomePod (~40 songs, ~4 stalls expected). For every
                         plays-from-0 song in the netlog (the §2 rbuf check, as a script): a journal line, the
                         position moving within 15 s, the song played to the end. No stall longer than 25 s
 T3 no false nudges      pause for 5 min; seek forward and back by hand; skip songs quickly; let a song end;
                         an ad if one comes: no journal line
 T4 a real network stall the kill switch blocks the proxy for 60 s mid-song: at most 3 nudges, then quiet;
                         playback resumes by itself once the proxy is back
 T5 nothing to see       the watchdog's host list pointed at a bot-detection page (CreepJS, sannysoft) for
                         one run: the same verdict with and without the watchdog. Back to music.youtube.com
 T6 restarts             Restart browser, a user switch, New identity, kill -9 of the browser: the YouTube tab
                         is watched again (journal: "youtube: watching <tab>")
 T7 other tabs           a second tab on another site: no attach to it (the journal, and T5's check)
```

### 9.1 T1 results (2026-11-08)

Run with `--remote-debugging-port=9222` added to `args` for the test only (backup:
`/data/browser.conf.before-t1`), and a standard-library Python client on the OECT making rd-shell's calls.

| check | result |
|---|---|
| `Runtime.addBinding {executionContextName: "rd"}`, then a call from the world | the binding never appears in the world; no `bindingCalled` |
| `Page.addScriptToEvaluateOnNewDocument {worldName: "rd"}` | not shown either way: it was checked from a second session, and a second session gets its own "rd" world (row 3), so the check could not see it |
| `Page.createIsolatedWorld` twice, same name, same session | same context id (5, 5); a variable set in one is there in the other |
| the page's own world: `typeof __rdWatch, __rdRec, rdReport` | `undefined undefined undefined` |
| install + drain by `Runtime.evaluate` every 10 s | works: `{"ev": "start", "title": "0+0 \| YouTube Music"}` came back |
| the nudge resets YouTube's count | **yes, twice** (below) |
| a world outlives the session that made it | yes: the 13:10 watcher's copy kept running in the page after that watcher was stopped at 13:16 |

**The nudge, 13:16–14:53 (26 plays, two stalls).** Both at 2:05, both ended the same way, read from the netlog:

| song (docid) | stuck at | seek, about 10 s later | then |
|---|---|---|---|
| sqQqRxeNg-c | 125.715 s (`vps B`, 13:24:37) | to 123.715 s, `S:ss.104` (13:24:47) | `PL`; audio fetched again from 13:24:58; played to the end (148 s) |
| djV11Xbc914 | 125.958 s (`vps B`, 13:27:19) | to 123.958 s, `S:ss.104` (13:27:29) | `PL`; the next audio request counts from the real position (`rbuf` 21 s at 150 s buffered, 120 s before); played to the end |

`ss.104` is how YouTube logs a seek made on the `<video>` element from outside its player: setting
`currentTime` from an isolated world at 16:22 (paused, same position, no sound) logged `S:ss.104`. The seek is
exactly the script's `t − 2`. Before the watchdog, stalls at 2:05 lasted until someone tapped the bar.

These two were done by the copy the first watcher (13:10) left in the page, not by the 13:16 watcher's copy:
both run the same script, the first one's 2 s tick came first, and after its seek the second copy saw the time
move and stood down. The first copy's records went to the binding, which never delivers, so nothing was
logged. The 13:16 watcher's drains kept working (`[]` every 10 s, seen with strace at 16:20).

What this means for rd-shell: the watchdog lives as long as the document, not the session. rd-shell holds one
session per tab for the tab's life and the browser dies with rd-shell, so there is one copy per document. If a
second copy ever appears (a re-attach), the two cannot seek twice for one stall: whichever acts first moves the
time, which resets the other's count.

Found on the way: restarting rd-shell with the port added brought the browser back with only a New Tab, not
Frank's YouTube Music tab, and rd-shell then saved that as his tabs. Whether the port causes it is to be seen
when the port comes out again.

## 10. Order of work

1. T1 by hand on browser-oect: the native seek resets YouTube's count; how `report` reaches rd-shell
2. rd-shell: §4–§6; built and tested in the image from the browser-image Action, not locally
3. reinstall browser-oect with the new image (`keep_persist`); T2–T7
4. D5: the netlog off, if T2 passed
