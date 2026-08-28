# Settings that survive a relaunch, and diagnosable failures

## Why

Twice an in-headset test produced "feature X doesn't work" when the feature was simply **off** —
once `grip`, once `aim`. Each cost a full cycle (play, tab out, read counters) to discover a
default rather than a bug. Every new DLL requires a relaunch, so anything tuned live evaporated
exactly when it mattered. That is a process failure, and it was worth fixing before continuing.

## The settings file

`bfbc2vr.cfg`, next to the log. It is **a list of commands**, in the same syntax as
`bfbc2vr_cmd.txt`:

```
grip on
aim on
aim kp 0.30
hud dist 1.80
```

Every setting already has a command that both sets and reports it, so there is no second schema
to keep in step: the console records each accepted command (`settings::note`, called once in
`vrcmd::run_line`) and replays the stored lines through the same dispatcher at startup. No module
registers anything.

- **Only allowlisted verbs persist.** Transient and diagnostic commands — `shot`, `census`,
  `recenter`, `dik`, `mouse`, the memory-scanner verbs, the FOV hunts — are never written, and a
  stale file containing one is skipped with a log line rather than replayed.
- **Measured values are not settings.** The mouse gain and turn sign are measured live;
  persisting them would suppress re-measurement after an in-game sensitivity change. Discovered
  addresses are ASLR-dependent and never persist.
- **Replayed on the first frame**, not in `DllMain`, so a replayed command cannot touch OpenVR or
  D3D under loader lock.
- Written with `_SH_DENYWR` like the log, so it stays readable and hand-editable while the game
  runs. Delete a line to restore that default.

`save` forces a write; the status block reports how many settings were stored, applied and
skipped.

## Naming every reason the aim loop declines

The aim loop could previously decline for eight reasons, four of them silent and the rest sharing
one counter — so "nothing happened" was undiagnosable without another relaunch. Now every early
return is named and counted:

`disabled`, `stale-sample`, `turn-settling`, `no-hmd`, `no-reference`, `no-controller`, `pole`,
`error-cap`, `deadzone`, `zero-counts`, `not-foreground`, `gated-off`.

`camover::aim_error()` reports *which* of its internal failures occurred rather than a bare
`false`, and the frame gate in `vrinput::on_present()` distinguishes its three merged conditions.

The turn machinery is counted too, including **`no-response`** — the injection was emitted and the
camera did not move — which is the most diagnostic failure in the subsystem.

**A summary line goes to the log every 600 injected frames** whenever aim is on. That matters
because live status is useless while the player is alt-tabbed: the game unacquires DirectInput the
moment it loses focus, so everything reads as idle. The log is the record of what happened while
they were actually playing.

Also fixed: the snap counter incremented even when the turn was never emitted, so it reported
turns that did not happen.
