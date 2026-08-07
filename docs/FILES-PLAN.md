# File Clipboard — Design & Implementation Plan (draft 2)

Goal: copy files/folders in Explorer/Finder on one device and paste them on
another, with **no eager transfer** — bytes move only for files actually pasted,
and only when pasted. The synced clipboard already does this for text/image
by-value; files are the first **by-reference** clipboard type.

Draft 2 settles the transport, metadata-fidelity, conflict-reporting and
liveness questions that draft 1 left implicit. One item remains open and is
marked **OPEN** inline.

## Verified ground (don't relitigate)

- **Windows is natively lazy on the clipboard.** `OleSetClipboard` + a custom
  `IDataObject` exposing `CFSTR_FILEDESCRIPTOR` (the tree manifest) + one
  `CFSTR_FILECONTENTS` per file as `TYMED_ISTREAM`. The target pulls each file's
  `IStream` at paste; `IDataObjectAsyncCapability` keeps Explorer's UI unblocked.
  Same `IDataObject` for clipboard and drag. This is the 7-Zip-paste path.
- **macOS has no lazy paste on the clipboard. Measured, not guessed** (2026-06-17
  probe, `scratch/macos-paste-probe`): Finder ignores both `NSFilePromiseProvider`
  and a lazy `NSPasteboardItemDataProvider`+`public.file-url` on Cmd-V. The only
  OS-blessed lazy path is a **File Provider extension**: the clipboard carries
  ordinary `public.file-url`s pointing at **dataless** items in a Clipp provider
  domain; Finder's paste-copy hydrates them on read (same as copying a not-yet-
  downloaded iCloud Drive file).
- Eager staging on the receiver is **rejected**: you frequently copy and never
  paste on that device, and lazy gets OS-provided progress/cancel/error UI free.
- **Receivers auto-adopt a bundle onto their OS clipboard**, exactly like text and
  images. A reference costs nothing until pasted, so the usual objection to
  auto-adopting something huge doesn't apply.

## Settled design

### The two recursions are not the same shape, the two RPCs are

Both OS mechanisms are pull-based and collapse to two operations against the
**origin** peer (the device that owns the real files on disk):

```
listChildren(eventGuid, relPath, maxDepth) -> [ {name, isDir, size, mtime, flags} ]
readContents(eventGuid, relPath, offset, length) -> bytes
```

| Engine op | Windows adapter | macOS adapter |
|---|---|---|
| `listChildren` | `GetData(CFSTR_FILEDESCRIPTOR)` builds the **flat, complete** descriptor → drives a full recursive walk at paste | `NSFileProviderEnumerator.enumerateItems` → **one container at a time**, lazily, as Finder descends |
| `readContents` | `CFSTR_FILECONTENTS[i]` → `IStream::Read` (ranged) | `fetchContents` → **whole file** (no byte-range in the replicated API) |

The residual asymmetry: **Windows demands the entire tree's *metadata* at paste**
(one flat `FILEGROUPDESCRIPTOR`), so the descriptor build recurses down the whole
tree before the first byte moves.

**`maxDepth`, and why the recursive walk STREAMS.** One level per call is the
macOS shape; imposed on Windows it costs a round trip *per directory* — a 5k-
directory tree is ~5 s of pure latency on a 1 ms LAN and ~50 s over 10 ms Wi-Fi,
stacked on top of the ~1 s the local walk costs anyway.

Incremental delivery buys nothing on Windows to offset that:
`GetData(CFSTR_FILEDESCRIPTOR)` is a **single synchronous call returning one
`HGLOBAL`**, and the format is flat (every file listed with its full relative
path, `bigdir\sub\a.txt`). There is no partial descriptor and no expand-later, so
Explorer sits in "Preparing to copy…" regardless. The only objective left is
minimum total wall time.

So the recursive walk is **streamed, not batched**: one request, and the origin
emits entries *as it walks*, terminated explicitly (with a distinguishable error
terminator if the walk dies at directory 4000). One round trip; no multi-MB
buffer on either side; and walk time overlaps transfer time instead of summing —
wall time is `max(walk, transfer)` plus an RTT, about as close to the storage
floor as this gets. macOS passes `maxDepth = 1` while browsing; Windows passes
unbounded at paste. Same frame, same channel, one code path.

### Data model — manifest rides the existing pipeline, bytes go out-of-band

`ClipboardPayload` holds the whole item in one `std::vector` — fine for a few KB
of text, impossible for a file tree. So **the bytes never become a payload.**
What becomes a payload is a small **manifest**:

- New format `CLIPP_FORMAT_FILES` (next free `ClipboardFormat.h` id). The
  payload bytes are the serialized **top-level** manifest only:
  `[ {name, isDir, size?, mtime, flags} ]` — what the user selected. Subdirectory
  contents are **not** in the manifest; they are discovered at paste via
  `listChildren`, so `bigdir` stays unrecursed until someone descends into it.
- The manifest is a few hundred bytes. It therefore flows through **every
  existing mechanism unchanged**: CLIP broadcast, `ClipboardActivityStore`
  (history + `ItemsSince`/`TailEventGuid` sync), the hash-guard echo suppression,
  origin stamping (`eventGuid`/`originHostId`/`originHostName`/`timestamp`). The
  `eventGuid` already on `meta` becomes the **bundle key** that `listChildren`/
  `readContents` are scoped by.
- `ClipboardActivityPayloadKind::Files` is added; `DisplayItem` previews it as
  e.g. "3 items · 2.4 GB" (sizes summed from the manifest).

The "ClipboardPayload can't hold trees" problem dissolves: it only ever holds the
manifest envelope, never the tree.

### Metadata fidelity — the lowest common denominator, deliberately

**A paste is a copy performed by the pasting user, on the destination machine.**
Not a restore, not a mirror. Everything below follows from that sentence.

**Carried** (in the manifest and in `listChildren` entries): name, size, isDir,
**mtime**, and a small portable flag set — **executable** and **read-only**.

The exec bit earns its place: macOS `.app` bundles are directories, so they
transfer structurally for free, but the binary in `Contents/MacOS/` must land
executable or the pasted app is a brick. Read-only comes along because it maps
cleanly both ways (`FILE_ATTRIBUTE_READONLY` ↔ removing the write bits).

**Dropped, and why each is *right* rather than merely easier:**

- **NTFS ACLs.** They are SIDs, which are machine- or domain-scoped: a local SID
  from machine A denotes nothing on machine B, or in the bad case denotes someone
  else. Preserving them imports a broken or actively wrong access decision.
  Destination inheritance applies — which is exactly what Explorer itself does
  for a cross-volume copy, and a cross-machine paste is a cross-volume copy.
- **Ownership (uid/gid).** Same argument, same conclusion. Pasted files are owned
  by whoever pasted them, under their umask.
- **Alternate data streams, macOS extended attributes, resource forks.** Not
  universally representable. Explorer drops ADS copying to a foreign filesystem;
  AppleDouble is the cautionary tale for the alternative (`._` files in every zip
  and USB stick, universally hated).
- **Hidden.** Dot-prefix vs an attribute bit is a *naming* convention vs
  *metadata*; reconciling them would mean renaming files, which is worse than
  losing the attribute.
- **Symlinks**: not followed, not recreated — skipped and counted. Following
  invites directory loops and accidentally copying a whole mounted volume;
  recreating needs privilege on Windows. Consistent with the path-escape guard,
  which already treats symlinks as a boundary rather than a thing to traverse.
- **Mark-of-the-web / `com.apple.quarantine`**: neither preserved (they are
  ADS/xattr) nor **synthesized**. A peer holding the group key is already as
  trusted as your own machine; flagging its files Internet-zone would be
  inconsistent with the text clipboard, which already carries anything.

Precedent for the whole position: **git carries exactly one permission bit** for
the same reason, and every cross-platform sync service (Dropbox, OneDrive)
converged on content + mtime + exec.

**Known limit, stated honestly:** the exec bit survives any single paste, and
survives mac↔mac and Linux→mac chains, because it travels in our manifest rather
than in a filesystem. It is **lost when a POSIX file round-trips through a
Windows disk** — paste onto Windows, copy it again in Explorer, and the new
manifest is built from a filesystem that never had anywhere to record it. Storing
it in an ADS would work and is exactly the cleverness that earns a bug report in
two years. Git on Windows (`core.fileMode=false`) and zip have the identical
limitation.

Other consequences worth one line each: **sparse files transfer dense** (a 40 GB
sparse image is 40 GB on the wire); **hard links become independent copies**;
**creation time is not carried** (mtime is the one that matters, and Linux mostly
lacks birthtime anyway).

### Conflict reporting — the toast, and nothing else

Some destination-side conflicts have no good resolution: a name that is illegal
on the destination (`a:b` from mac onto Windows, `con.txt`), or `A.txt` and
`a.txt` landing on a case-insensitive volume. **Fail those entries cleanly**
rather than silently renaming — a copy that quietly renames files breaks
references inside the content and is worse than one that admits it took fewer.

Implementation is pleasantly free: "not copied" is "**not offered**" — the entry
never enters the `FILEGROUPDESCRIPTOR` or the enumerator, so Explorer and Finder
copy the rest without ever knowing. It is therefore known at *listing* time,
before any byte moves.

Report via the **typing progress pill** (`TextTyper`'s toast, generalized): a
non-activating, topmost notice near the tray / under the menu bar. It is the only
correct surface — during a paste, Explorer or Finder owns the foreground, and
anything modal would steal focus mid-operation.

**The toast is for "you asked for something and didn't get it." Nothing else.**

> **Copied 7 of 10 items** — 3 names aren't valid on this device.

Deliberately NOT reported: stripped ACLs, dropped ADS/xattrs, skipped symlinks in
the aggregate sense. Those are documented, intended behavior that would fire on
nearly every paste; reporting them trains people to dismiss the toast, and then
the message that matters is dismissed with it. A useful consequence: since we
never report them, **we never have to detect them** — no `FindFirstStreamW` /
`listxattr` per file during the origin-side walk.

Per-file forensics (which file, which reason, what the OS returned) go to the
**log**. A passive badge on the activity row (count in the tooltip, reusing the
private-badge machinery) is an acceptable backstop, but it is explicitly not a
channel — nobody is expected to go find it.

Aggregate one toast per paste, debounced; the file channel's idle-close is a
serviceable "the paste is over" signal.

Component work required: a **transient** mode alongside today's sticky one, and a
rule that a paste notice arriving mid-typing-run queues rather than clobbers the
progress readout.

### Wire — mirror the register RPCs

Two new request/response frames over the existing `CryptoChannel`, modeled on
`RGET`/`RLST` and the one-shot **request/response ack** already proven for
register copy (see [[project-oneshot-relay-ack]] — fire-and-forget is not an
option here either):

- `FLST` — `listChildren`: request `(eventGuid, relPath, maxDepth)`, streamed
  entry response. Errors: unknown guid / not-serving / path-escaped / gone.
- `FGET` — `readContents`: request `(eventGuid, relPath, offset, length)`,
  streamed byte response. `length == 0` ⇒ whole file (macOS `fetchContents`);
  Windows `IStream` requests sequential ranges.

**Both travel on the file channel, always — never the control connection.** No
per-frame routing rule to get wrong, no two-mode testing. The cost is that a
macOS *browse* (enumerating a folder the user descended into, with no paste in
sight) also opens the channel; that is one handshake, amortized over the session,
in exchange for a single code path.

BE serialization like `RegisterWire`. New capability bit
`CryptoChannel::CAP0_SERVES_FILES = 0x10` — 0x01 RECENT, 0x02 REGISTERS, 0x04
PUT and 0x08 NETMAP are taken. That leaves three bits in `caps[0]`; if Phase 4
grows more verbs, decide then whether `caps[1]` is in play.

A receiver only writes file URLs to its OS clipboard / creates provider items if
the bundle's origin is reachable and advertises `CAP0_SERVES_FILES`.

### The file channel — one dedicated side channel, carrying everything

`FLST`/`FGET` must not ride the control connection. That connection's recv loop is
**strictly serial** (load-bearing elsewhere: it is what makes the `PING`/`PONG`
delivery fence a valid ack), so bulk bytes there would block clipboard sync,
register anti-entropy and keepalives for the duration of every chunk. Worse, it
would force small chunks, and small chunks over request/response make throughput
**RTT-bound** — a 10 GB file over 10 ms Wi-Fi spends minutes in pure latency.

A dedicated channel removes both constraints at once, and the second is the real
prize: with nothing else competing for the socket, the origin answers one `FGET`
by **streaming** frames until done, so there is no per-chunk round trip at all.
TCP backpressure supplies flow control for free — a slow receiver disk blocks the
origin's `send` and memory stays bounded with no windowing scheme of our own.

**Direction: the receiver dials, and sends `FGET` on the channel it opened.**
Consistent with the rest of the design (everything is pull; `FLST` already goes
receiver→origin). The receiver owning the socket means **cancellation is
`closesocket()`** — Explorer's Cancel button aborts a multi-GB stream instantly,
with no message and no cooperation from the origin. Dial-back would instead need
a correlation token, a timeout, and a new "the connection never arrived" failure
mode — i.e. FTP active mode, whose problems we would be re-importing.

Build it from **`OneShotPeer`**, which is already exactly this shape: connect,
handshake, verify identity against an expected `hostId`, do one job, tear down.

**The role must be announced in the handshake, not in a first frame.** The
listener registers a peer as part of accepting it and immediately fires
anti-entropy — precisely the `SYNC`/`RSYN` crosstalk the one-shot CLI path has to
drain today. Add a role byte the way `osType` was added: peeled off `caps[]` so
`HandshakePlaintext` keeps its on-wire size and older peers still interoperate.
Gate dialing on `CAP0_SERVES_FILES` so a role-tagged connection never reaches a
peer that predates the field.

**File channels are `PeerManager`'s to track, but not peers.** They must be filed
and managed as side channels — no broadcast queue, no anti-entropy, and invisible
to the outgoing-peer reconciler, whose job is reaping same-endpoint churn and
which would otherwise see a file channel as exactly that. `PeerManager.cpp`
currently drops a second connection from a known host as a duplicate, so this is
not optional. Tracking them there is also what gives quota a home: one file
channel per peer by default (a named constant in `PeerLimits.h`), rejected by
completing the handshake and answering with a busy error frame — one extra round
trip buys a legible "origin is busy" instead of a bare reset.

**Lifetime is per operation, not per file.** Explorer pastes a multi-file
selection as a sequence of separate `IStream`s; connect-per-file would turn a
500-file paste into 500 handshakes. Open lazily on the first `FLST`/`FGET`, close
after a few seconds idle.

**Requests queue on the channel.** macOS can have several `fetchContents` in
flight during a tree copy, and Finder interleaves enumeration with fetching. With
one channel those serialize. That costs approximately nothing — one link's
bandwidth is the constraint either way, so serializing changes ordering, not
throughput — and it avoids an in-protocol multiplexer. Queue on the receiver
side; do not open a second channel to parallelize.

**No fallback to file-RPCs-over-control.** It would mean maintaining the slow path and
its interaction with the fast one to serve a case that only exists in a network
where the mesh is already broken.

**Streamed responses must make truncation impossible to mistake for success** —
for a file copy that is the worst available failure, because it looks like it
worked. Declare the length up front, stream, then terminate explicitly; a
mid-stream failure (file deleted at 3 GB, read error, serve policy lapsing
underneath) sends a *distinguishable error terminator*. The receiver accepts the
file only on exact declared byte count **and** clean end marker.

**Bonus:** once connections declare a role, the one-shot CLI path stops needing
its crosstalk-draining loop — the gateway simply doesn't send anti-entropy to a
connection that announced itself as one-shot.

### Windows adapter (`src/platform/win32`)

- **Provide (paste target):** an `IDataObject` whose `EnumFormatEtc` offers
  `CFSTR_FILEDESCRIPTOR` + `CFSTR_FILECONTENTS`. `GetData(FILEDESCRIPTOR)` walks
  the tree via `FLST` (on the `IDataObjectAsyncCapability` thread) and emits the
  flat `FILEGROUPDESCRIPTOR`. `GetData(FILECONTENTS, lindex=i)` returns an
  `IStream` whose `Read` issues ranged `FGET` over the file channel.
  `OleSetClipboard`; the data object stays alive on the daemon.
  - Note this is a different clipboard-ownership model from the existing
    `SetClipboardData`/`WM_RENDERFORMAT` path — the files branch bypasses it
    rather than extending it.
- **Read (user copies in Explorer):** `CF_HDROP` → real paths → top-level
  manifest; the daemon becomes the origin and serves `FLST`/`FGET` from those
  paths. Long paths need `\\?\` prefixing on origin-side reads. (Virtual-file
  sources — `CFSTR_FILECONTENTS` from an archive — are out of scope v1.)

### macOS adapter — File Provider extension + daemon proxy

- One **replicated** provider domain (`NSFileProviderReplicatedExtension`),
  registered once via `NSFileProviderManager.add(domain:)`. Layout
  `~/Library/CloudStorage/clipp/<eventGuid>/…`. The domain is *ephemeral scratch*,
  not a product surface.
- **There is no lighter provider to trade down to.** `NSFileProviderExtension`
  (the older non-replicated, document-vending flavor) is the iOS Files-app model;
  macOS requires the replicated one. The alternatives are all disqualified:
  macFUSE needs a user-installed system extension (categorically not MAS), a
  loopback SMB/NFS mount needs a privileged mount and is outside the sandbox, and
  eager staging is the option already rejected. Replicated isn't the best choice
  — it's the only one.
- **The zero-extension fallback that already works is DRAG.** The 2026-06-17 probe
  showed `NSFilePromiseProvider` fails on Cmd-V but functions for drags, because a
  drop supplies a destination and a paste doesn't. Dragging an item out of the
  Clipp window into Finder is lazy and native with no extension at all. It isn't
  Cmd-V, so it can't replace the extension — but it's the shippable subset if the
  extension work stalls, and it's the same degraded mode the MAS read path names.
- Check whether `NSFileProviderDomain`'s hidden-domain support covers the
  deployment target; if it does, it retires the long-standing cosmetic landmine of
  the domain being visible under `~/Library/CloudStorage`.
- **Provide (paste target):** on receiving a manifest, the daemon writes the
  top-level entries as **dataless placeholder items** under `<eventGuid>/` into
  the app-group store and signals the enumerator; it puts `public.file-url`s for
  those items on `NSPasteboard.general`. `enumerateItems` → `FLST`;
  `fetchContents` → `FGET` (whole file → hand back a URL).
- **The extension is a separate process.** Peer connections, the sodium channel,
  and the network key live in the daemon. The extension is a thin shim that
  **proxies `enumerateItems`/`fetchContents` to the daemon over XPC**
  (`NSXPCConnection`), sharing the item-metadata store via an app group. Do not
  open peer sockets from the extension.
- **Read (user copies in Finder):** `public.file-url` → real paths → manifest.
  **Sandbox caveat (the standing #1 risk):** on MAS, opening pasteboard-origin
  file URLs likely needs `startAccessingSecurityScopedResource`, and it is
  undocumented whether a plain Cmd-C grants it (open/save panels and drag-to-dock
  do). Must spike before committing the MAS read path; if it fails, the MAS read
  side degrades to "drag into Clipp" while the write/paste side is unaffected.

### Serve policy and expiry

Bundle lifetime is a **single sender-side predicate**, `CanServe(eventGuid)`:

- **Default (non-private): serve from history**, bounded by TTL. Decoupled — no
  cross-device revocation — and consistent with how the synced clipboard is
  actually used: sync pushes on copy and the user pastes later, so a receiver's
  clipboard routinely holds a bundle the origin has already moved past.
- **`CLPM_FLAG_SOURCE_MARKED_PRIVATE` bundles**: served only while they are the
  origin's *active* clipboard item, never retained as a servable history entry.
  The privacy marker already means "be careful" and puts the strict policy
  exactly where it earns its cost.

**TTL is ABSOLUTE, measured from the item timestamp** — not idle/`touched` as
registers use. This is the one place files deliberately diverge from the register
model, and the reason is specific: a receiver must be able to compute expiry with
**zero coordination**, and there is no replicated touch clock for a bundle. Idle
expiry would make receivers pessimistic (the origin's clock moves when some
*other* device pastes). The consistent rule across object types is "expiry is
measured from the newest event both sides can observe" — for registers that is
the replicated `touched`; for bundles it is the timestamp.

Either way it is **by-reference**: bytes are read at pull time, so a changed or
deleted file surfaces as a clean OS error.

### Liveness — two free signals, everything else at runtime

Paths are relative to an `eventGuid` the origin must still bless, so **deleting
the item revokes universally**, enforced at the origin rather than by receiver
cooperation. (A receiver that was offline during the delete keeps a stale row
forever; it doesn't matter, its `FGET` is refused at the source.)

Render exactly two states, both free and non-racy:

- **Origin unreachable** — the mesh already knows.
- **Expired** — computable locally, now that the TTL is absolute.

**Everything below that is discovered at runtime, deliberately.** Whether the root
still exists, whether individual files were deleted, whether the origin's view
agrees with the receiver's — probing buys a fresher lie, not a true statement,
because the file can be deleted a millisecond after the check. A "still valid"
badge would be false by the time it is painted.

Runtime discovery is graceful, not a shrug: the first thing a paste does is a
metadata call (`GetData(CFSTR_FILEDESCRIPTOR)` / root `enumerateItems`), both of
which hit `FLST` on the root. **A dead bundle therefore fails before Explorer or
Finder has created a single file**, and the OS shows its own error.

**Badge, don't disable.** An offline origin may come back between looking and
pasting; a disabled row is a prediction that can be wrong in the direction that
annoys. Mark the state, don't gate the action.

**Mid-stream death: no rollback.** Completed files stay, the in-flight one is the
OS's problem, and the error surfaces through the existing copy UI — exactly what
happens when a network drive drops mid-copy, so it is a failure people already
understand. Transactional cleanup would be a lot of machinery to make our failure
*less* familiar than everyone else's.

### Lifetime / teardown / layering

- Receiver placeholders are torn down when the bundle stops being servable:
  immediately for private (active-only), at TTL for normal — **computed locally
  on each side from the item timestamp**, so no revocation message and no
  cross-device coupling. That local computation is what makes the history-served
  policy genuinely cheaper than active-only rather than active-only with extra
  steps.
- **Layering law (inherited from registers):** the replicated layer changes only
  for explicit user ops and the shared TTL policy. Per-device history limits and
  scratch GC are local and never emit replicated mutations. `CanServe` is a local
  policy, not replicated state.

### Security posture — a real change, document it in SECURITY-MODEL.md

**OPEN — SECURITY-MODEL.md has not been updated.** Today a mesh peer can read
exactly what you chose to copy. With files, a path-validation bug turns a peer
into **arbitrary file read of anything the daemon can open**. Group members are
already fully trusted, so this is not a new trust boundary — but the blast radius
of a *bug* grows enormously, which is a different thing and belongs in the
security doc rather than only here.

Validate at manifest-build time **and again on every `FLST`/`FGET`**, by
canonicalizing through the OS (`GetFinalPathNameByHandle` / `realpath`) and
prefix-checking against the pinned roots — not by string-inspecting for `..`.
Symlinks and junctions are what string checks miss.

## Limits — deliberately few

Per-`FGET` chunk size; max concurrent file channels per peer (`PeerLimits.h`).
Path-escape rejection on every `FLST`/`FGET` relPath. Refusals are origin-side,
never background eviction (layering law).

**No cap on bundle size or entry count.** The mechanism already has failure
handling; use it, and otherwise let people copy what they copy. Know how it fails
so the choice is informed rather than overlooked: the Windows descriptor is a
*single* `HGLOBAL` at roughly 600 bytes per entry, so an absurd bundle surfaces as
an allocation failure — `E_OUTOFMEMORY` out of `GetData`, an unhelpful Explorer
error rather than a sentence explaining itself. It still fails before a single
file is created, which is the property that matters.

**No caching of the walk, on either side.** File contents change with no
invalidation mechanism conceivable, and copy/paste is the most one-shot job there
is — it essentially never repeats. If Explorer asks for the descriptor twice, walk
twice. (Probe 3 is what could reopen this: if Explorer pulls the descriptor on
every right-click rather than at paste, a repeated multi-second walk becomes its
own problem and wants a different answer, not a cache.)

## Probes — gate the phases on these

1. **mac hydrate-on-paste** (low risk, cheap): a `swiftc` script sets a
   `public.file-url` for an **evicted iCloud Drive file** on `NSPasteboard.general`;
   Cmd-V into a local folder; confirm it downloads-then-copies. Proves the
   mechanism generically **before anyone builds an extension**. → gates Phase 3.
2. **mac sandbox read** (standing #1 risk): can a sandboxed (MAS) build open a
   pasteboard-origin `public.file-url` via `startAccessingSecurityScopedResource`?
   → gates the Phase 3 *read* path (write/paste is unaffected either way).
3. **win descriptor timing** (now the load-bearing one): does Explorer pull
   `CFSTR_FILEDESCRIPTOR` only at paste, or earlier (menu open / hover)? A cheap
   logging `IDataObject` that records every `GetData` answers it. If it fires at
   paste, everything above holds. If it fires on every right-click, a multi-second
   recursive walk runs when the user merely opens a context menu — which needs a
   lazier answer (refuse to produce the descriptor until a real paste, if that is
   even expressible), NOT a cache. → gates Phase 2 design.
4. **`fetchContents` whole-file**: confirm there is no byte-range hydration in the
   replicated API (so a huge single file double-stores transiently). Expected.

## Phases

1. **Engine + wire (pure, unit-tested).** Manifest type + serialization,
   streamed `FLST`/`FGET` frames (BE, `RegisterWire`-style), `CAP0_SERVES_FILES`,
   the handshake role byte, `CLIPP_FORMAT_FILES`, an origin-side `FileBundleSource`
   (disk → listChildren/readContents with path-escape guards), `CanServe`.
   doctest over a temp tree.
2. **Windows end-to-end.** `IDataObject`/`IStream` provider + `OleSetClipboard`,
   `CF_HDROP` read, `ReadClipboardData`/`SetClipboardData` files branch, the file
   channel + `PeerManager` side-channel tracking, activity `Files` kind + preview,
   conflict toast. Win→Win copy/paste. (No mac dependency — ship-able.)
3. **macOS end-to-end.** File Provider extension target + XPC proxy + app group,
   enumerator/`fetchContents` → daemon → `FLST`/`FGET`, `public.file-url` read +
   the sandbox spike. mac↔mac and mac↔win.
4. **Policy + reach.** TTL + private→A, teardown/GC, liveness badges, relay/
   one-shot CLI path (bytes pull from origin, not the gateway), iOS
   (`UIPasteboard` + `NSItemProvider.registerFileRepresentation` is genuinely
   lazy and system-honored — closer to Windows than mac).

## Notes that bit us / will bite

- The `IStream` (win, ranged) vs `fetchContents` (mac, whole-file) split is the
  one place the engine's `readContents(offset,length)` is exercised differently —
  keep the range params even though mac always asks for the whole file.
- Don't reconnect peers from the mac extension; proxy to the daemon.
- A file history entry is not self-contained the way a text entry is — it is a
  claim check on a live origin. The UI must not present it as guaranteed-available.
- File channels are connections that are *not* peers. Every place that enumerates
  connections needs to know the difference: `PeerManager` dedup, the outgoing-peer
  reconciler, broadcast fan-out, anti-entropy on accept.
- Windows' descriptor is one allocation and one synchronous call. That single fact
  drives the streamed recursive walk, the absence of a size cap, and the absence of
  progressive paste feedback — don't "improve" one of them without rechecking the
  other two.
- Truncation that looks like success is the one unacceptable failure. The declared
  length + explicit terminator check is not optional politeness.
