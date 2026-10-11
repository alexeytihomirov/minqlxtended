# sv_demoStreamKeyframeSec Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every streamed POV carries a full (non-delta) snapshot at least once every `sv_demoStreamKeyframeSec` seconds while its slot is active, so a relay can start a late viewer from a recent keyframe.

**Architecture:** One game-thread array holds, per slot, the monotonic time of the last block queued with `SBF_FULL` (or of the gamestate that opened the POV). Once per server frame, `Stream_Frame` sets `stream_resync_pending[slot]` for every active slot whose last full block is older than the interval. Nothing else is new: the existing `stream_force_resyncs` turns the pending flag into `deltaMessage = -1`, and `Stream_Block` clears it when the full block goes out - the same path a GAP takes today (`stream_note_drop`).

**Tech Stack:** C (gnu, `-Werror`), the fork's `src/features/stream.c`, `make nopy` on `ubuntu:24.04`.

**Spec:** `D:/QuakeLiveData/QL Server/docs/superpowers/specs/2026-10-11-demo-stream-relay-hardening-design.md`, section 4.1 only (sections 4.2-4.5 are other repos and other plans).

## Global Constraints

- Cvar name `sv_demoStreamKeyframeSec`, default `10`, `0` turns it off, effective range `2..60` seconds (1 is clamped to 2, above 60 to 60; 0 or below is off).
- Registered `CVAR_ARCHIVE` like every other `sv_demoStream*` cvar; never `CVAR_SERVERINFO` / `CVAR_SYSTEMINFO`.
- `sv_demoStreamResync 0` disables keyframes too: same mechanism, same switch.
- No other engine change: protocol version stays 1, frame format unchanged, `stream.h` unchanged, no Python binding (so `tools/gen_stub.py` is not touched), no `src/server/hooks.c` change.
- Time source: `stream_now_ms()` (`src/features/stream.c:294`, `CLOCK_MONOTONIC`, returns `uint64_t` ms). Not `svs->time`, not the engine's `Sys_Milliseconds`.
- All new state is game-thread only, like `stream_resync_pending`; no atomics, no lock.
- Build: `make nopy EXTRA_CFLAGS=-Werror` and `make nopy_debug EXTRA_CFLAGS=-Werror` with zero warnings on `ubuntu:24.04`; `python3 tools/check_consistency.py --check` passes (`D:/QuakeLiveData/QL Server/skills/qlsm-dev/reference/engine-fork.md`, section Building; CI: `.github/workflows/build.yml`).
- Fork commit style: plain sentence-case subject, no `feat:` prefix, no AI attribution lines.

## Review Focus

- **Link down or shedding.** While `stream_link_up` is 0 or the lag guard sheds, blocks never reach `Stream_Block`, so the timestamp goes stale. Expected: no keyframe is armed then (the GAP path already arms one for the reconnect); a player is not forced into full snapshots that nobody receives. Pinned by Task 2 step 1, items 3-4.
- **Cvar set to 0, negative, 1 or 999 at runtime.** Expected: 0 and negative turn it off on the next frame; 1 behaves as 2; 999 behaves as 60. No restart. Pinned by Task 2 step 1, item 1 and Task 3 step 4.
- **`sv_demoStreamResync 0`.** Expected: keyframes off too, and `resyncing` in `qlx_stream` stays 0 (the flag is never set, not just ignored). Pinned by Task 2 step 1, item 2.
- **Many POVs opened at the same gamestate (map start).** They come due in the same frame; expected: `sv_demoStreamResyncPerFrame` (default 1) staggers the forced fulls one per frame, nobody is forced twice, and the slots not yet served stay pending without being re-armed. Pinned by Task 2 step 1, item 5.
- **A full block that fails `stream_put` (ring full).** Expected: the timestamp is not advanced, so the keyframe is due again on the next frame (the drop itself still goes through `stream_note_drop` as today). Pinned by Task 1 step 1, item 3.

---

### Task 1: Register the cvar and stamp the last full block per slot

**Files:**
- Modify: `src/features/stream.c:199-214` (cvar pointers), `:217-220` (per-slot arrays), `:1296-1316` (`Stream_Want`, gamestate branch), `:1341-1360` (`Stream_Block`), `:1839-1841` (`Stream_Init`)

**Interfaces:**
- Consumes: `stream_now_ms(void) -> uint64_t` (`stream.c:294`); flags `SBF_FULL`, `SBF_GAMESTATE`.
- Produces for Task 2:
  - `static cvar_t *sv_demoStreamKeyframeSec;`
  - `static uint64_t stream_last_full_ms[MAX_STREAM_CLIENTS];` - game thread only; time of the last full block or opening gamestate successfully queued for the slot.

- [ ] **Step 1: Write the code-reading checklist (the "failing test")**

There is no unit harness for `stream.c` (the only C tests are `src/democut/test_cut.c` and `src/democut/test_scan.c`), so the test for this task is this checklist, checked against the diff after step 3:

1. `sv_demoStreamKeyframeSec = Cvar_Get("sv_demoStreamKeyframeSec", "10", CVAR_ARCHIVE);` sits right after the `sv_demoStreamResyncPerFrame` line in `Stream_Init`, with a comment above it in the style of the `sv_demoStreamMaxLag` one: forces a full snapshot on each streamed POV at least this often so a relay can start late viewers; `0` off; `2..60` seconds; no effect with `sv_demoStreamResync 0`.
2. In `Stream_Want`, the gamestate branch sets `stream_last_full_ms[slot] = stream_now_ms();` next to `stream_resync_pending[slot] = 0;`, i.e. only after the OPEN record was queued (past the `stream_put(...) != 0` early return).
3. In `Stream_Block`, `stream_last_full_ms[slot]` is set to `stream_now_ms()` only when `stream_put` returned 0 and `flags & (SBF_FULL | SBF_GAMESTATE)`. The existing `stream_resync_pending[slot] = 0;` stays where it is, before the put; its behaviour does not change.
4. No other function changes.

- [ ] **Step 2: Confirm the checklist fails on the base**

Run: `grep -n "sv_demoStreamKeyframeSec\|stream_last_full_ms" src/features/stream.c`
Expected: no output.

- [ ] **Step 3: Implement**

Add the cvar pointer after `sv_demoStreamResyncPerFrame`, the array after `stream_resync_pending` with a one-line comment ("when this slot last queued a full snapshot; see stream_arm_keyframes"), the two stamps and the registration, as the checklist says.

- [ ] **Step 4: Re-check**

Run: `grep -n "sv_demoStreamKeyframeSec\|stream_last_full_ms" src/features/stream.c`
Expected: 5 lines - pointer, array, stamp in `Stream_Want`, stamp in `Stream_Block`, registration. Walk items 1-4 against `git diff`; all hold. (Nothing reads the new state yet; that is Task 2. Both additions are file-scope variables, which `-Werror` does not flag as unused, so the tree stays buildable; the build of record is Task 3.)

- [ ] **Step 5: Commit**

```bash
git add src/features/stream.c
git commit -m "Track when each streamed POV last carried a full snapshot"
```

### Task 2: Arm the keyframe resync once per frame

**Files:**
- Modify: `src/features/stream.c:1648-1681` (add `stream_keyframe_ms` and `stream_arm_keyframes` just above `stream_force_resyncs`), `:1730-1734` (`Stream_Frame`, call before `stream_force_resyncs()`)

**Interfaces:**
- Consumes: from Task 1 `sv_demoStreamKeyframeSec`, `stream_last_full_ms[]`; existing `stream_active[]`, `stream_resync_pending[]`, `stream_link_up` (atomic), `stream_shedding`, `sv_demoStreamResync`, `sv_maxclients`, `cvar_clamped(cvar_t *, int fallback, int lo, int hi) -> int` (`src/engine/quake_common.h:2126`).
- Produces (file-local only):
  - `static uint64_t stream_keyframe_ms(void);` - `0` when off, else the interval in ms.
  - `static void stream_arm_keyframes(void);` - game thread, once per frame from `Stream_Frame`.

- [ ] **Step 1: Write the code-reading checklist (the "failing test")**

1. `stream_keyframe_ms()`: a NULL cvar reads as the default 10; `integer <= 0` returns 0 (check this before clamping, because `cvar_clamped` would turn 0 into 2); otherwise `(uint64_t)cvar_clamped(sv_demoStreamKeyframeSec, 10, 2, 60) * 1000`. So 1 -> 2000, 10 -> 10000, 999 -> 60000.
2. `stream_arm_keyframes` returns at once when `sv_demoStreamResync && !sv_demoStreamResync->integer` (the test `stream_force_resyncs` uses), so the flag is never set with resync off; and when `stream_keyframe_ms() == 0`.
3. It returns at once when `!atomic_load_explicit(&stream_link_up, memory_order_relaxed)`.
4. It returns at once when `stream_shedding` is set. It reads the variable and does not call `stream_over_lag()`, which moves the hysteresis.
5. It iterates slots `0 .. min(sv_maxclients->integer, MAX_STREAM_CLIENTS) - 1` with the same NULL / `<= 0` guards as `stream_force_resyncs`, reads `stream_now_ms()` once before the loop, and sets `stream_resync_pending[slot] = 1` for each slot with `stream_active[slot] && !stream_resync_pending[slot] && now - stream_last_full_ms[slot] >= interval`. It does not touch `deltaMessage`, the per-frame budget or `stream_st_resyncs`; `stream_force_resyncs` already does those.
6. `Stream_Frame` calls `stream_arm_keyframes();` on the line before `stream_force_resyncs();` (after `stream_publish_guid`), so it runs only with streaming on and the thread reconciled.
7. A short comment above `stream_arm_keyframes` says why: a relay can start a viewer only at a full snapshot, and this bounds how far back the last one is.

- [ ] **Step 2: Confirm it fails on the Task 1 commit**

Run: `grep -n "stream_arm_keyframes\|stream_keyframe_ms" src/features/stream.c`
Expected: no output.

- [ ] **Step 3: Implement the two functions and the call**

As the checklist says. `now - stream_last_full_ms[slot]` is unsigned 64-bit; every active slot has a stamp from its OPEN (Task 1, item 2), so no zero special case.

- [ ] **Step 4: Re-check**

Run: `grep -n "stream_arm_keyframes\|stream_keyframe_ms" src/features/stream.c`
Expected: 4 lines - two definitions, the `stream_keyframe_ms()` call inside `stream_arm_keyframes`, and the `stream_arm_keyframes();` call directly above `stream_force_resyncs();` in `Stream_Frame`. Walk items 1-7 against `git diff HEAD~1`. `git diff demo-match-capture --stat` lists only `src/features/stream.c` and this plan file.

- [ ] **Step 5: Commit**

```bash
git add src/features/stream.c
git commit -m "Force a full snapshot on each streamed POV every sv_demoStreamKeyframeSec"
```

### Task 3: Build with -Werror and run the consistency check

**Files:**
- None planned. A warning fix goes into an extra commit on `src/features/stream.c` only.

**Interfaces:**
- Consumes: the branch head after Task 2.
- Produces: build evidence for Task 4.

Build environment, checked on the dev machine on 2026-10-11: WSL and Docker are not available. `gcc` on PATH is MinGW-w64 16.1.0 (WinLibs), which has no `netdb.h` / `sys/socket.h`, so a local `gcc -fsyntax-only` of `stream.c` is not possible - do not try to make it work. The build runs on home-docker (`home@192.168.100.100`, `D:/QuakeLiveData/QL Server/skills/qlsm-dev/reference/deploy-and-hosts.md`) in `ubuntu:24.04`, per `engine-fork.md`. SSH from the implementer's session to home-docker was not verified by the plan author: if `ssh home@192.168.100.100 true` fails, stop and ask the operator (SSH setup lives in `D:/Code/home-infra`, per `D:/QuakeLiveData/QL Server/skills/ql-local/SKILL.md`).

- [ ] **Step 1: Consistency check (local, Python 3.12 is on PATH)**

Run: `python -I tools/check_consistency.py --check` from the worktree root
Expected: `6 consistency checks passed`, exit 0 (the base gives the same output).

- [ ] **Step 2: Ship the committed tree to home-docker**

The branch is not pushed, so copy it:

```bash
git archive --format=tar HEAD | ssh home@192.168.100.100 'rm -rf /tmp/stream-keyframe && mkdir /tmp/stream-keyframe && tar -x -C /tmp/stream-keyframe'
```

Expected: exit 0. `HEAD` is what gets built, not the working copy.

- [ ] **Step 3: Build both nopy targets with -Werror**

```bash
ssh home@192.168.100.100 'docker run --rm -v /tmp/stream-keyframe:/src -w /src ubuntu:24.04 bash -c "apt-get update -qq && apt-get install -y -qq build-essential >/dev/null && make nopy EXTRA_CFLAGS=-Werror && make nopy_debug EXTRA_CFLAGS=-Werror && strings bin/minqlxtended_nopy.so | grep -c sv_demoStreamKeyframeSec"'
```

Expected: both makes finish with no `warning:` line, and the last line prints `1` or more. If the container lacks something the Makefile needs (`Makefile:20` calls `python3-config`), add the package to the same `apt-get` line and say so in the report; do not edit the Makefile. On any warning: fix in `stream.c`, commit, repeat steps 2-3.

- [ ] **Step 4: Runtime clamp check (optional, needs a non-production server)**

Only if the operator provides a test QLDS: load the built `.so`, stream a real client to `tools/demo_stream_relay.py`, set `sv_demoStreamKeyframeSec` to `1`, `0`, `999` in turn and read `qlx_stream`: `resyncs` rises about every 2 s, stops, then about every 60 s. Bots never stream (`engine-fork.md`, Live test facts). Skipping this is fine; Task 4 step 4 is the required live check.

- [ ] **Step 5: Clean up**

Run: `ssh home@192.168.100.100 'rm -rf /tmp/stream-keyframe'`
Expected: exit 0.

### Task 4: Land on demo-match-capture, refresh the PR branch, live check

**Files:**
- None new. Branches: `stream-keyframe` -> `demo-match-capture` -> `server-side-demo-capture`.

**Interfaces:**
- Consumes: the built branch from Task 3.
- Produces: the commits on `demo-match-capture` and an updated `server-side-demo-capture` (PR tjone270/minqlxtended#8).

Force-pushing `server-side-demo-capture` rewrites a public PR branch: get the operator's OK before step 3. Work from this worktree with `git -C`; never edit the shared checkout `D:/QuakeLiveData/QL Server/_build/minqlxtended-fork`.

- [ ] **Step 1: Check the base is current**

Run: `git merge-base stream-keyframe demo-match-capture` and `git rev-parse demo-match-capture`
Expected: the same hash. If not, rebase `stream-keyframe` onto `demo-match-capture` and repeat Task 3 steps 1-3.

- [ ] **Step 2: Fast-forward demo-match-capture and push**

Fast-forward `demo-match-capture` to `stream-keyframe` and push it. Confirm with `git ls-remote origin demo-match-capture` that the remote hash equals `git rev-parse demo-match-capture`. Whether this plan file stays on `demo-match-capture` is the operator's call; it must not reach the squashed PR branch (step 3).

- [ ] **Step 3: Refresh server-side-demo-capture (after the operator's OK)**

Follow `engine-fork.md`, section Branches: amend the squashed commits so the code tree equals `demo-match-capture`, force-push, then `git diff demo-match-capture server-side-demo-capture -- src tools python Makefile` is empty and `git ls-remote origin server-side-demo-capture` matches local. Tokens cannot edit the PR description; give the operator one line for `docs/superpowers/specs/2026-09-13-minqlxtended-fork-pr-body.md` (monorepo): "`sv_demoStreamKeyframeSec` (default 10, 0 off, 2..60): forces a full snapshot on each streamed POV at least that often, so a relay can start late viewers."

- [ ] **Step 4: Live check (spec section 6, step 6) - operator step**

The operator ships the new hash through the runtime-source addon in qlsm. Then on each host `strings minqlxtended.x64.so | grep sv_demoStreamKeyframeSec` prints the name, and `qlx_stream` on a streamed instance with a real client shows `resyncs` rising about every 10 s. Record the result in monorepo task `root-0011`.

- [ ] **Step 5: Cvar documentation**

No cvar table lives in this repo: `README.md:69` points to the upstream wiki page `https://github.com/tjone270/minqlxtended/wiki/Configuration`, which we do not edit, and qlsm's cvar catalog leaves fork-only cvars out by decision (`engine-fork.md`, Pins and the cvar catalog). The in-repo documentation is the registration comment from Task 1. Hand the operator this wiki line for the streaming section: "`sv_demoStreamKeyframeSec` - default `10`. Seconds between forced full snapshots on each streamed POV; `0` off, clamped to `2..60`. No effect with `sv_demoStreamResync 0`."
