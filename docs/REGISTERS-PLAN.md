# Named Clipboard Registers — Implementation Plan (draft 2)

Goal: `clipp copy url` / `clipp paste url` / `clipp ls` / `clipp rm url` — named
registers replicated across the mesh with no server, surviving partitions, with
convergent semantics. The existing synced clipboard is conceptually the unnamed
register; v1 treats it observationally (see Phase 2) without touching the
shipping CLIP path.

Design is settled (three review rounds). What follows is the build plan.

## Settled design

### Data model
LWW-element-map. One record per name:
`(name, value | TOMBSTONE, written, touched, originHostId, flags)`.

- `flags` is a byte fixed **now** so the wire/record format never has to change
  retroactively: `TOMBSTONE = 0x01`, `PRIVATE = 0x02`. (Remaining bits reserved.)
  Flags are part of the value: on a value conflict the **winner's** flags win.
- `originHostId` is the existing 16-byte host id (LWW tiebreak, below).

### Clocks
`written` and `touched` are hybrid logical clocks (HLC = 64-bit wall ms +
64-bit logical counter), tiebroken by HostId. Wall clocks alone are disqualified
(skew poisons LWW).

- `written` advances only on writes; sole input to value conflict resolution.
- `touched` advances on reads and writes; sole input to expiry.
- Merge: value (and its flags) go to max `(written, hostId)`; `touched` = max of
  both sides regardless of which value won. Both merges are lattice joins →
  convergence (SEC) holds.

### Expiry — the GC, lazy-only, drop-on-access
Every record — value or tombstone — self-expires at `touched + TTL`, a pure
predicate of `(record, local now)`. No timer. Expiry is never a stored state; a
record is expired the instant the clock crosses.

- **Drop-on-access (lazy delete):** any code path that encounters an expired
  record *erases* it. Applied at every surface: `Get`, `List`, `ApplyRemote`
  (merge), **digest build**, and the write path. Result: physical map == live
  set at all times → the 1024 cap is exact, memory is bounded by the cap, no
  sweep needed. Filtering-without-erase was rejected — it leaks (dead names
  resident forever).
- Digest build walks the whole map anyway, so every (re)connect doubles as a
  full compaction. The write path does an O(live) ≤ O(1024) prune-then-count;
  negligible at human write rates.
- **No expired record ever propagates:** `ApplyRemote` ignores an incoming
  record already expired by local clock, and treats an expired local record as
  absent. Digests exclude expired records. (Skew window is bounded by clock skew
  ≪ TTL; both sides converge once clocks agree.)
- Mortal data ⇒ mortal tombstones. Invariant: a tombstone only shadows records
  with `written ≤` its own, and `touched` max-merge gives it `touched ≥` anything
  it dominates → it never expires before its victims.

### Idle-TTL with read-refresh
Reads refresh `touched` (LRU — frequently used registers stay alive). Invariant
to preserve if TTL ever becomes tunable: **TTL ≫ touch quantum** so a register
can't expire between refreshes.

### Deletes
`rm` writes a tombstone — but is **not** blind. It resolves existence first
(locally in the daemon; via RLST/RGET at the gateway for the one-shot CLI),
writes the tombstone if present, and **errors on absent** (covers a missing
name, an already-tombstoned/expired name, and zero wildcard matches). Local
expiry emits nothing on the wire — convergent by construction.

### Quarantine — removed
Folded into lazy-prune-at-digest. On reconnect the digest build prunes, so dead
records simply aren't advertised; nothing to drop, no `LastMeshContact`, no
startup branch. **Accepted residual:** a register *read-refreshed locally during
a >TTL partition* that was `rm`'d elsewhere keeps a recent `touched`, survives
the prune, and resurrects on reconnect. At a 90-day TTL that requires a 90-day
partition with active local reads of a concurrently-deleted register. Eaten
knowingly.

### Persistence
Register data is **ephemeral** (RAM-only; mesh re-heal is the durability story —
single-device users get nothing persistent, all-devices-restart loses state).
The **one** persisted artifact is the **HLC floor**, because the mesh is now the
durable store: after an ephemeral wipe + a regressed wall clock, a fresh local
write could land below the mesh's existing HLC and silently lose the LWW
compare. Persist the high-water HLC via the `OriginSequenceFloor` pattern
(Settings key, batched, seed `lastSeen` from it on start, +1 ms guard so the
counter can't be reused within a millisecond across restarts). The sub-second
window where a daemon's *first* write (before initial anti-entropy ratchets the
clock to mesh time) could still lose is accepted; the CLI path is immune (it
talks to an already-synced gateway).

### Layering law
Replicated state changes for exactly two reasons — explicit user ops
(copy/rm/paste-touch) and the shared TTL policy. Per-host history limits
(memory/age/items) apply to the local activity log only and never emit
replicated mutations. Register cap is a write-time refusal at origin, never
background eviction. No local resource policy reaches the replicated layer.

### Commands

```
clipp ls                # register names, one per line, sorted alpha (script surface)
clipp ls -v             # aligned terminal table; alpha; see formatting note
clipp ls u?l            # shell-style wildcards (? and *) in any command
clipp rm ur*            # remove all matching 'ur*'; error if zero matches
clipp copy url          # insert/overwrite register 'url' from stdin
clipp paste url         # output register 'url' to stdout
clipp copy pw --private # contents masked in 'ls -v'; on `paste pw` masked/refused
                        # when isatty(stdout), real bytes when piped; flag rides the wire
```

- Bare `clipp copy` / `clipp paste` (no name) = current behavior, byte-for-byte.
- `ls -v` columns: name / age / size / origin (hostname, fall back to hostid) /
  contents. Contents sanitized to printable, width-capped, inverse `>` overflow
  marker on a tty; private rows masked. Piped (no tty): keep the formatted table
  at 80 cols (honor `$COLUMNS` if set) — `ls` stays the parse target, `ls -v`
  stays the human surface.
- Errors (no such register, no register-capable peer, comms failure) go to
  stderr with a non-zero exit.
- Text-only in v1.

## Phase 1 — Core engine + first unit tests

Pure, I/O-free. `src/Hlc.h` + `src/RegisterStore.{h,cpp}`.

- **`Hlc`:** `{uint64 wallMs; uint64 counter}`; `now()`, `ratchetOnReceive()`
  (standard HLC update), `pack`/`unpack` (16 bytes BE), total order
  `(wallMs, counter)` extended by HostId for LWW.
- **`RegisterRecord`:** name, value (or tombstone), `written`, `touched`,
  `originHostId`, `flags`.
- **`RegisterStore`:** `Upsert` (local write), `ApplyRemote` (merge), `Touch`,
  `Delete` (tombstone), `Get`, `List`/`Snapshot`, `Digest`
  (`vector<(name, written, touched)>`), expiry predicate, drop-on-access at every
  read/merge/digest/write surface.
- **Validation:** name `[a-z0-9._-]{1,64}`, case-sensitive ASCII (Unicode names
  out of scope — normalization rat hole; the `""` key is internal-only for the
  default-register mirror and never user-addressable). Value cap = existing
  payload cap. Count cap 1024 → write-time refusal with a clear message.
- **Tests (new to repo):** doctest/Catch2 single-header + a `clipp-tests` CMake
  target + CTest. Highest-value surface in the project:
  - property tests: commutativity / associativity / idempotence of merge over
    randomized op sets;
  - N-replica random delivery-schedule simulations converge to identical state;
  - tombstone dominance + expiry invariants (tombstone never precedes a record
    it dominates);
  - clock-skew scenarios;
  - drop-on-access keeps physical == live, and expired records never enter a
    digest.

Done when: property tests pass; simulated partition/heal converges; invariants
hold; physical == live after arbitrary op/expiry sequences.

## Phase 2 — Daemon integration

- Global `RegisterStore` alongside `g_clipboardActivityStore`, behind a mutex
  wrapper mirroring the existing store (hit from network-recv threads, CLI
  ingress, and lazy expiry). Register writes are **not** shown in the GUI —
  interrogated via `ls`/`ls -v` only.
- **HLC floor persistence:** new Settings key, `OriginSequenceFloor` pattern
  (batched, pre-seed on start, +1 ms guard).
- **Default-register mirror (observational):** local copies and applied CLIP
  frames update `record[""]` so `ls` shows the live clipboard. The CLIP apply
  path is untouched in v1 — the engine does not yet arbitrate the OS clipboard.
- New Settings keys (stored-only, no GUI in v1): `RegisterTtlSeconds`
  (default 90 d), `RegisterMaxCount` (default 1024). Touch quantum is a
  compile-time constant (24 h), not a setting.

Done when: daemon boots with an empty store; HLC floor survives restart with no
regression; local copy/paste/ls/rm work against the local store; `ls` shows the
live default clipboard.

## Phase 3 — Wire protocol

Leans on: unknown frame tags are logged-and-ignored (Peer.cpp), capability bits
ride the handshake (`Caps[0]`, `CAP0_SERVES_RECENT` precedent), frames are
secretstream-encrypted.

- **`CAP0_SERVES_REGISTERS = 0x02`.** Send register frames only to peers
  advertising it (ignore-unknown is the safety net; cap-gating is etiquette —
  avoids warning-log spam on old peers, especially from touches).
- **Frames** (tags illustrative):
  - `REGW` — record push. Carries name + `written` + `touched` + record flags
    (`TOMBSTONE`/`PRIVATE`) + transport flags (`TOUCH_ONLY` = mutate `touched`
    only, no value; `RELAY` = gateway rebroadcasts, mirrors `CLPM_FLAG_RELAY`).
    Wire struct ≈ ClipboardMessage + name + two HLCs + flags, reusing
    compression/hash fields. Transport flags are not persisted into the record.
  - `RGET` — read by name → `REGW` or `NONE`; **touch side effect at the serving
    peer** (the read refreshes `touched` there and propagates).
  - `RLST` — list names + metadata.
  - `RSYN`/`REOS` — anti-entropy.
- **Anti-entropy on connect:** each side sends `RSYN` with its full digest
  (`(name, written, touched)`, expired records pruned out at build time). At
  ≤1024 names this is one frame (≪ that in practice). Each side replies with
  `REGW` for records the digest shows missing/stale — a `TOUCH_ONLY` `REGW` when
  only `touched` differs. Hook where SYNC/EOSY runs today (Peer.cpp recv loop /
  NetworkRuntime on-connect).
- **Steady state:** a local write/rm/touch-past-quantum broadcasts one `REGW` to
  connected register-capable peers (mirrors `BroadcastClipboard`).

Done when: two daemons converge via on-connect anti-entropy; steady-state `REGW`
propagates; `TOUCH_ONLY` is rate-limited by the quantum; an old peer ignores
`REGW` with no disconnect and no log spam.

## Phase 4 — CLI + one-shot relay

- `clipp copy [name]`, `clipp paste [name]` — optional positional on the existing
  subcommands (CLI11, src/Cli.cpp); bare = current behavior. New top-level
  `clipp ls`, `clipp rm <name>`, plus `-v`, wildcards, `--private`.
- One-shot flow reuses `RelayPayloads`/`BrowseStream`; gateway selection requires
  `CAP0_SERVES_REGISTERS`. Named `copy` → `REGW` with `RELAY` (gateway applies +
  rebroadcasts). `paste` → `RGET`. `ls` → `RLST`. `rm` → existence check then
  tombstone `REGW`, error if absent. Wildcard `rm`/`ls` = `RLST` → match locally
  → act.
- Name validation at the CLI and at `REGW`/`RGET` ingress (defense in depth).
- `ls -v` formatting per the Commands note (alignment, age/size humanization,
  sanitize + width-cap + `>` overflow on tty, 80-col when piped, private masked).
- `--private`: CLI sets the `PRIVATE` flag bit (format already reserved in
  Phase 1/3); masks contents in `ls -v`; on `paste` of a private register,
  masks/refuses when `isatty(stdout)`, emits real bytes when piped. Private
  values never hit the debug log.

Done when: copy/paste/ls/rm/wildcards/`--private` work end-to-end through a
gateway; bare copy/paste unchanged byte-for-byte; new CLI against an old-only
mesh fails with a clear "no register-capable peer found".

## Phase 5 — Hardening + verification

- Ingress sanity: clamp/reject HLC > now + 24 h (clock-poison defense).
- Mixed-version checklist: old GUI peer in mesh (ignores `REGW`, no disconnect,
  no log spam thanks to cap-gating); old CLI against new gateway; new CLI against
  old-only mesh (clear failure).
- Functional checklist (cross-platform pass): two-device write/read/ls/rm;
  partition → divergent writes → heal → converge; `rm` on A while B offline →
  B rejoins → register stays dead; touch keeps a register alive past would-be
  expiry; `rm` of an absent/expired name errors; default register visible in
  `ls`; `--private` masking + tty/pipe behavior.
- iOS: engine compiles in via the existing `.mm`-include pattern; share extension
  and iOS UI unchanged in v1.

## Deferred to post-v1

- **Phase B — engine authoritative for the default register:** make the engine
  arbitrate the OS clipboard so late-arriving CLIP frames stop clobbering newer
  state. A correctness improvement, but it changes shipping clipboard behavior —
  decide after v1 ships.
- **Persistence of register data:** revisit once the feature has proven out;
  would reuse the LogPaths-style app-data resolution + libsodium-secretbox at
  rest (at-rest trust = on-wire trust; key erase/rotation invalidates the cache).
- **Non-text payloads.**

## Risks / notes

- Read-refresh makes reads emit writes — partition-tolerant by design, but worth
  remembering when debugging "why did a frame go out on paste."
- CLIPP_HEADLESS is one-shot only (not a daemon), so scripted multi-peer
  integration tests aren't free; the in-process replica simulation in Phase 1
  carries the convergence burden.
- Pinned/never-expire registers are explicitly rejected — that single flag would
  resurrect the entire tombstone-lifecycle problem ("that's a file").
- Two accepted residuals, both narrow and documented above: the read-refreshed-
  during->TTL-partition resurrection, and the cold-start first-write-before-sync
  race.
