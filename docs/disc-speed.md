# Disc Speed Configuration

**Date:** 2026-05-29  
**Branch:** `overlay-discovery`  
**Priority:** do this first — small, self-contained, no dependencies.

---

## Problem

The runtime emulates PSX CD-ROM timing faithfully (1x = ~150 KB/s, seek delays ~40-500ms).
Loading screens feel like real PSX hardware. The ISO is on a local SSD; the delay is purely
artificial. There is no reason a developer or player should have to sit through it.

---

## Solution

Add a `disc_speed` setting to the per-game TOML. The CDROM controller state machine still fires
in the correct sequence (Idle → Seeking → Reading → INT1) — we just compress the time between
transitions.

```toml
[disc]
speed = "instant"   # "1x" | "2x" | "4x" | "instant"
```

- `"1x"` — default, authentic PSX timing. Preserves the original experience.
- `"2x"`, `"4x"` — scaled multipliers. Useful if a game breaks at instant but tolerates faster.
- `"instant"` — no timing delays. Data arrives as fast as the host ISO read. Recommended for
  development and testing.

---

## Risk

Games that poll CDROM status registers in a tight loop with an expiry counter, or that use CD
timing as a crude delay mechanism, may misfire at `"instant"`. Symptoms would be a hang or
corrupted load immediately after a disc read. The multiplier options exist as a fallback dial.

Most games — including Tomba — wait on the INT1 interrupt and are indifferent to how quickly it
fires. `"instant"` is expected to be safe for all titles targeted by this recompiler.

---

## Implementation

Change is localized to `runtime/src/cdrom/cdrom.c`. The timing delay paths (seek duration, sector
read interval) check `disc_speed` and either apply the emulated delay or skip it. No other
subsystem is affected.

---

## Relation to overlay-discovery

Independent. Do this first. Faster disc loads make overlay-discovery iteration faster too —
each playthrough that warms the overlay log completes quicker.
