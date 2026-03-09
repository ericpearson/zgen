# Genesis Emulator Sonic 2 2P Title Flash Handoff

## Goal

Fix the remaining rare flashes on the Sonic 2 two-player title/demo split with
hardware-coherent behavior only. No game-specific hacks, no broad timing
offsets, no fake line nudges.

This handoff replaces the older `handoff.md` content because several earlier
assumptions are now obsolete.

## Short Version

Three real fixes have already landed:

- `2216b2d` `Fix Sonic 2 title split display-enable latching`
- `b960565` `Preserve X flag for 68K compare instructions`
- `8ea566d` `Fix 68K quick memory operation timing`

Those are real correctness fixes and should stay.

The original flashing black divider race is fixed. What remains is a much
rarer set of late split outliers. The current best lead is not another VDP
seam bug. It is upstream control/state on the V-int helper dispatch path,
specifically the helper target written into `FFFFF6E2` (`15A0` vs `15AA`),
which changes how much masked work the 68K does before H-int becomes
serviceable.

## What Was Fixed For Real

### 1. VDP display-enable latch behavior

Committed in `2216b2d`.

Files:
- `src/video/vdp.cpp`

Effect:
- HBlank display-enable writes no longer apply with the old inconsistent
  immediate-vs-next-line behavior.
- Display-enable changes now resolve on a deterministic `px=0` edge.
- Slot-table selection follows the effective queued display state instead of
  only the raw latched bit.

Result:
- This removed the original obvious flashing black seam on the Sonic 2 title
  load.

Important:
- Do not revert this. It was the real fix for the original visible race.

### 2. 68K compare-family `X` flag preservation

Committed in `b960565`.

Files:
- `src/cpu/m68k_ops_arith.cpp`
- `tests/m68k_test.cpp`

Effect:
- `CMP`, `CMPI`, `CMPA`, and `CMPM` were incorrectly clobbering `X` because
  they reused subtraction logic that updated it.
- They now preserve `X` correctly.

Result:
- Real CPU correctness fix.
- Did not eliminate the Sonic 2 late split outliers.

### 3. 68K `ADDQ/SUBQ` memory-destination timing

Committed in `8ea566d`.

Files:
- `src/cpu/m68k_ops_arith.cpp`
- `tests/m68k_test.cpp`

Effect:
- Memory-destination `ADDQ`/`SUBQ` timings were undercharged.
- This particularly mattered in the V-int tail at `00045E`:
  `ADDQ.L #1,$FFFFFE0C.w`

Result:
- Real CPU timing fix.
- Did not fully remove the remaining title-split outliers.

## Current Bug State

The original broad black seam is fixed.

What remains are rare late split pairs after the normal title/demo path is
already mostly correct.

Latest strong validation run:
- script: `scripts/run_sonic2_title_trace.sh`
- run from `build/`
- log: `/tmp/sonic2_validate_without_linebudget_3200.log`

Distribution:
- `109/113`: `1861`
- `110/114`: `1`
- `111/115`: `1`
- `112/116`: `1`
- `113/117`: `1`
- `115/119`: `1`
- `120/124`: `1`

Interpretation:
- Most frames are fine.
- The remaining bug is rare and upstream of the final display-enable apply.
- This is no longer the old “always wrong seam” problem.

## What Is Ruled Out

### Not the old VDP display-enable race

That was the original bug and it is already fixed by `2216b2d`.

### Not the speculative VDP line-budget cleanup

I tested a VDP line-budget consistency change in:
- `src/video/vdp.cpp`
- `src/video/vdp_scanline.cpp`

Then I removed it and reran the same 3200-frame validation. The result was
identical with and without that change:

- `109/113`: `1861`
- `110/114`: `1`
- `111/115`: `1`
- `112/116`: `1`
- `113/117`: `1`
- `115/119`: `1`
- `120/124`: `1`

Conclusion:
- The line-budget change is not the reason for the remaining flicker.
- It has been reverted and is out of the active theory set.

### Not “the game is taking a bogus long path only in our emulator”

Reference testing with BlastEm showed:
- idle boot also reaches `FE10=00`
- idle boot also reaches `FFD8=1`
- idle boot also enters the same long masked `00E320..00E35C` path

So that path itself is legitimate and not, by itself, proof of emulator error.

## Reference Emulator Findings

Reference target:
- BlastEm in `third_party/blastem-src/blastem-884de5ef1263`

Helper script:
- `scripts/run_blastem_sonic2_trace.sh`

Why BlastEm was used:
- practical local reference
- easy to instrument narrowly
- Genesis-focused accuracy target

### What BlastEm confirmed

On plain idle boot, without special input:
- Sonic 2 naturally reaches the 2P intro/title path
- it writes the same display-enable register values we care about
- it also reaches `FE10=00`, `FFD8=1`, and the long masked helper path

### Display-enable writes in BlastEm

BlastEm logs showed the same relevant VDP register activity:
- `reg=1` / mode 2
- disable low byte: `0x34`
- enable low byte: `0x74`

Observed BlastEm split-write line pairs:
- normal frames: mostly `108/112`
- rare late pairs also happen there:
  - `114/118`
  - `117/121`
  - `118/122`

Interpretation:
- The old hard assumption that hardware must always be exactly `109/113` is no
  longer trustworthy.
- The game and reference emulator do show some variability on this path.
- That does not mean our outliers are correct, but it changes the acceptance
  standard.

### Important cold-boot divergence found against BlastEm

On cold boot comparison:
- BlastEm normal title frames went through `001568` and then straight toward
  the V-int tail at `00045E..000466`
- our late frame went through extra helper work at `0015AA..0015B2` before the
  same V-int tail

That pointed at the helper target behind `jmp (a3)` at `001568`, which is now
the most useful remaining lead.

## Current Best Diagnosis

The remaining bug is most likely upstream helper-target control/state in the
V-int work path, not another direct VDP seam-model issue.

### Concrete evidence

I expanded local logging to include:
- V-int work path
- helper-state RAM writes
- H-int handler timing
- targeted bad-frame traces

Key local logs:
- `/tmp/sonic2_bad110_trace.log`
- `/tmp/sonic2_vint_a3_divider_1400.log`
- `/tmp/sonic2_helper_state_1400.log`

### What the bad `110/114` frame shows

On the small late outlier:
- H-int is already pending on line `107`
- CPU is still masked while finishing the V-int tail
- the sequence is:
  - `00045E` `ADDQ.L #1,$FFFFFE0C.w`
  - `000462` `MOVEM.L (A7)+,...`
  - `000466` `RTE`
- the H-int handler starts too late, on line `109`

This proves the small slip is not a renderer-side race anymore.

### Why the helper dispatch now matters more than generic interrupt timing

The trace with `A3` included showed that the bad frame is not entering the same
V-int helper target as the good frame.

Bad/suspicious frame evidence from `/tmp/sonic2_vint_a3_divider_1400.log`:
- `frame=1287` shows `a3=000015A0`
- `frame=1285` shows `a3=000015AA`

Both feed different amounts of work before the same eventual V-int tail and
H-int overlap.

This is more specific than “RTE timing might be wrong”:
- the helper target itself is changing
- that changes how long the CPU stays busy before H-int can be serviced

### Helper target state is written explicitly in RAM

From `/tmp/sonic2_helper_state_1400.log`:
- `FFFFF6E2` is written as `15A0`
- `FFFFF6E2` is also written as `15AA`

Observed writer sites:
- `0016C6`
- `001760`

Representative log lines:
- `write16 addr=FFF6E2 val=15A0`
- `write16 addr=FFF6E2 val=15AA`

These writes are deliberate game-state/control updates, not random corruption.

Current strongest hypothesis:
- the remaining late frames are driven by when and why the game chooses the
  helper target written to `FFFFF6E2`
- that target then controls whether the CPU burns extra masked work before the
  H-int handler

## What We Have Been Using To Reproduce

Primary ROM:
- `build/Sonic The Hedgehog 2 (World) (Rev A).md`

Important user instruction:
- always run with a real window when validating behavior
- use the script
- run from `build/` so state auto-load and logs behave correctly

Script used for local runs:
- `scripts/run_sonic2_title_trace.sh`

The script already does the important directory handling:
- resolves the ROM path
- `cd`s into `build/`
- launches `./genesis`

This avoids the earlier issue where the app was run from the wrong directory
and state/log behavior was inconsistent.

## Script Interface That Another AI Should Use

Main script:
- `scripts/run_sonic2_title_trace.sh`

Important options:
- `--frames N`
- `--out /tmp/file.log`
- `--log-hint 1`
- `--log-bad-split 1`
- `--bad-line 110`
- `--log-vint-work 1`
- `--log-hint-handler 1`
- `--log-divider 1`
- `--log-divider-regs 1`
- `--log-helper-state 1`
- `--log-split-gates 1`
- `--log-split-state 1`

Examples used during this investigation:

```bash
./scripts/run_sonic2_title_trace.sh \
  --frames 3200 \
  --log-hint 1 \
  --log-bad-split 0 \
  --log-split-gates 0 \
  --log-split-state 0 \
  --out /tmp/sonic2_validate_without_linebudget_3200.log

./scripts/run_sonic2_title_trace.sh \
  --frames 3200 \
  --log-hint 1 \
  --log-bad-split 1 \
  --bad-line 110 \
  --log-split-gates 0 \
  --log-split-state 0 \
  --out /tmp/sonic2_bad110_trace.log

./scripts/run_sonic2_title_trace.sh \
  --frames 1400 \
  --log-hint 1 \
  --log-vint-work 1 \
  --log-hint-handler 1 \
  --log-bad-split 1 \
  --bad-line 110 \
  --log-split-gates 0 \
  --log-split-state 0 \
  --out /tmp/sonic2_vint_dispatch_compare_1400.log

./scripts/run_sonic2_title_trace.sh \
  --frames 1400 \
  --log-divider 1 \
  --log-hint 1 \
  --log-vint-work 1 \
  --log-hint-handler 1 \
  --log-bad-split 1 \
  --bad-line 110 \
  --log-split-gates 0 \
  --log-split-state 0 \
  --out /tmp/sonic2_vint_a3_divider_1400.log

./scripts/run_sonic2_title_trace.sh \
  --frames 1400 \
  --log-helper-state 1 \
  --log-divider 1 \
  --log-vint-work 1 \
  --log-hint 1 \
  --log-bad-split 1 \
  --bad-line 110 \
  --log-split-gates 0 \
  --log-split-state 0 \
  --out /tmp/sonic2_helper_state_1400.log
```

BlastEm comparison script:

```bash
scripts/run_blastem_sonic2_trace.sh --frames 4000 --out /tmp/blastem_sonic2_trace.log
```

## Current Local Instrumentation

These changes are diagnostic only unless env vars are set.

### `src/genesis.cpp`

Current useful additions:
- V-int work logging expanded to cover `001528..001778`
- `A3` included in V-int log output
- bad-split trace support

Purpose:
- identify the helper target at `jmp (a3)` and the exact instruction path when
  H-int is pending but not yet taken

### `src/memory/bus.cpp`

Current useful additions:
- helper-state RAM write logging for the `FFFFF6E0..` region

Purpose:
- catch explicit writes of helper-target state such as `FFFFF6E2 = 15A0/15AA`

### `scripts/run_sonic2_title_trace.sh`

Current useful additions:
- passes through debug env toggles for:
  - `--log-vint-work`
  - `--log-hint-handler`
  - `--log-divider`
  - `--log-divider-regs`
  - `--log-helper-state`

## Current Worktree Reality

At the time of this handoff, `git status --short` showed:

- modified:
  - `src/genesis.cpp`
  - `src/main.cpp`
  - `src/memory/bus.cpp`
  - `src/ui/app_ui.cpp`
  - `src/ui/app_ui.h`
- untracked:
  - `scripts/run_blastem_sonic2_trace.sh`
  - `scripts/run_sonic2_title_trace.sh`
  - `third_party/blastem-src/`

Important:
- only some of those are from this debugging thread
- do not blindly revert unrelated user changes

The changes directly relevant to this Sonic 2 investigation are:
- `src/genesis.cpp`
- `src/memory/bus.cpp`
- `scripts/run_sonic2_title_trace.sh`
- `scripts/run_blastem_sonic2_trace.sh`
- `third_party/blastem-src/` instrumentation

## Recommended Next Step

The next useful move is not another generic timing tweak.

It is:

1. Correlate `FFFFF6E2 = 15A0/15AA` writes to exact frame outcomes.
2. Identify the code path that decides which helper target is written at:
   - `0016C6`
   - `001760`
3. Compare that decision path against BlastEm on the same cold-boot/title path.
4. Fix the first proven divergence feeding the helper target.

Why this is the best next step:
- it is narrower than “audit all interrupt timing”
- it is upstream of the bad frame
- it matches the strongest concrete evidence in the current logs

## If Another AI Needs The Most Important Facts Immediately

- Do not re-open the already-fixed VDP latch race as the main suspect.
- Do not assume `109/113` is the only valid hardware outcome; BlastEm did not
  support that.
- Do not assume the long `FE10=00 / FFD8=1 / 00E320..00E35C` path is emulator
  corruption; BlastEm also reaches it.
- The best current lead is helper-target state in `FFFFF6E2`.
- The current concrete divergence is `15A0` vs `15AA`.
- The remaining small late split is visible as H-int pending while the 68K is
  still finishing V-int work and return.

## Files And Logs Worth Preserving

Key logs:
- `/tmp/sonic2_validate_without_linebudget_3200.log`
- `/tmp/sonic2_bad110_trace.log`
- `/tmp/sonic2_vint_a3_divider_1400.log`
- `/tmp/sonic2_helper_state_1400.log`

Key commits:
- `2216b2d`
- `b960565`
- `8ea566d`

Key script:
- `scripts/run_sonic2_title_trace.sh`

## Bottom Line

The project is past the original obvious raster bug. The remaining issue is a
rare late split that now looks most plausibly tied to helper-target selection
in V-int state/control, with the crucial observable being `FFFFF6E2` changing
between `15A0` and `15AA`. That is the current best root-cause track.
