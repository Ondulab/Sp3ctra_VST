---
name: verify
description: Build, launch and drive the Sp3ctra standalone to verify a change end-to-end (macOS, CGEvent-driven UI, window screenshots).
---

# Verify a change in the Sp3ctra standalone

## Build + launch
```bash
cmake --build vst/build --target Sp3ctraVST_Standalone -j 8
open vst/build/Sp3ctraVST_artefacts/Release/Standalone/Sp3ctra.app
```
- ALWAYS confirm the running process is younger than the binary:
  `ps -axo pid,lstart,command | grep MacOS/Sp3ctra` vs `stat -f %Sm <binary>`.
  A stale instance silently shows the old build.
- If `open` silently fails right after a `pkill -9`, exec the binary directly
  in the background with stdout redirected to a log (RT_PROFILER lines prove
  the audio/synth threads run).

## Window capture (2nd display safe)
- Window id/bounds: Swift `CGWindowListCopyWindowInfo` (owner "Sp3ctra").
- `screencapture -o -x -l<windowID> out.png` (`-o` = no shadow).
- Mapping (Retina ×2): `screen_pt = image_px / 2 + windowOrigin`. Exact with `-o`.

## Driving the UI
- JUCE components are invisible to AppleScript/AX — post CGEvents.
  Compile a tiny swiftc helper: `list | click x y | dblclick x y | drag x1 y1 x2 y2`
  (mouseMoved, down, 30×dragged, up; dblclick sets `mouseEventClickState` on
  down and up). Compiled binary ⇒ negative screen coords work as argv.
- Catalogue chips drag-drop onto rack blocks; dropping selects the new block.
- Re-capture and re-calibrate after EVERY selection change (zone-1 panel count
  changes ⇒ whole-window re-layout).
- ~30–60 s after launch a one-shot deferred restore re-selects the persisted
  block and re-layouts: wait it out, or click your target block right before
  each gesture.

## Gotchas
- The standalone persists APVTS+window to
  `~/Library/Application Support/Sp3ctra.settings` even without a clean quit —
  stray gestures pollute the user's session; report any accidental edits.
- `cd` state between Bash calls: screenshots land in the CURRENT directory —
  keep out of the repo.
- The user may be live on the machine (watch for VS Code window-title changes
  between captures): stop posting CGEvents immediately.
- Rack LEDs are real evidence: ○ disabled / ◐ enabled-idle / ● processing —
  ● proves the C engine runs inside the RT chain executor.
