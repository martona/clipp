# File Clipboard — Design & Implementation Plan (draft 1)

Goal: copy files/folders in Explorer/Finder on one device and paste them on
another, with **no eager transfer** — bytes move only for files actually pasted,
and only when pasted. The synced clipboard already does this for text/image
by-value; files are the first **by-reference** clipboard type.

Design is converging; the one open call (serve policy) is flagged inline.

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

## Settled design

### The two recursions are not the same shape, the two RPCs are

Both OS mechanisms are pull-based and collapse to two operations against the
**origin** peer (the device that owns the real files on disk):

```
listChildren(eventGuid, relPath) -> [ {name, isDir, size} ]   // one directory level
readContents(eventGuid, relPath, offset, length) -> bytes      // a byte range of one file
```

| Engine op | Windows adapter | macOS adapter |
|---|---|---|
| `listChildren` | `GetData(CFSTR_FILEDESCRIPTOR)` builds the **flat, complete** descriptor → drives a full recursive walk at paste | `NSFileProviderEnumerator.enumerateItems` → **one container at a time**, lazily, as Finder descends |
| `readContents` | `CFSTR_FILECONTENTS[i]` → `IStream::Read` (ranged) | `fetchContents` → **whole file** (no byte-range in the replicated API) |

The residual asymmetry to keep in mind: **Windows demands the entire tree's
*metadata* at paste** (one flat `FILEGROUPDESCRIPTOR`), so building the
descriptor recurses `listChildren` down the whole tree before the first byte
moves (do it on the async thread). macOS enumerates per-container on demand, and
hydrates each file whole. Both transfer **zero** if the bundle is never pasted on
that device. Both express the identical two RPCs — the adapter is thin.

### Data model — manifest rides the existing pipeline, bytes go out-of-band

`ClipboardPayload` holds the whole item in one `std::vector` — fine for a few KB
of text, impossible for a file tree. So **the bytes never become a payload.**
What becomes a payload is a small **manifest**:

- New format `CLIPP_FORMAT_FILES` (next free `ClipboardFormat.h` id). The
  payload bytes are the serialized **top-level** manifest only:
  `[ {name, isDir, size?} ]` — what the user selected (`file1`, `file2`,
  `bigdir`). Subdirectory contents are **not** in the manifest; they are
  discovered at paste via `listChildren`. So `bigdir` stays unrecursed until
  someone actually descends into it on a receiver.
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

### Wire — mirror the register RPCs

Two new request/response frames over the existing `CryptoChannel`, modeled on
`RGET`/`RLST` and the one-shot **request/response ack** already proven for
register copy (see [[project-oneshot-relay-ack]] — fire-and-forget is not an
option here either):

- `FLST` — `listChildren`: request `(eventGuid, relPath)`, response
  `[ {name, isDir, size} ]` for that one level. Errors: unknown guid / not-
  serving / path-escaped / gone.
- `FGET` — `readContents`: request `(eventGuid, relPath, offset, length)`,
  response bytes. `length == 0` ⇒ whole file (macOS `fetchContents`); Windows
  `IStream` requests sequential ranges. Big-file transfers chunk so a single
  `FGET` never holds a multi-GB file in memory.

BE serialization like `RegisterWire`. New capability bit
`CryptoChannel::CAP0_SERVES_FILES = 0x04` (0x01 RECENT, 0x02 REGISTERS taken).
A receiver only writes file URLs to its OS clipboard / creates provider items if
some reachable peer advertises `CAP0_SERVES_FILES` for the bundle's origin.

### Windows adapter (`src/platform/win32`)

- **Provide (paste target):** an `IDataObject` whose `EnumFormatEtc` offers
  `CFSTR_FILEDESCRIPTOR` + `CFSTR_FILECONTENTS`. `GetData(FILEDESCRIPTOR)` walks
  the tree via `FLST` (recursively, on the `IDataObjectAsyncCapability` thread)
  and emits the flat `FILEGROUPDESCRIPTOR`; mark sizes unknown (`FD_FILESIZE`
  omitted) for entries we haven't `listChildren`'d yet if we want first paint
  faster. `GetData(FILECONTENTS, lindex=i)` returns an `IStream` whose `Read`
  issues ranged `FGET`. `OleSetClipboard`; the data object stays alive on the
  daemon (it already is).
- **Read (user copies in Explorer):** `CF_HDROP` → real paths → build the
  top-level manifest; the daemon becomes the origin and serves `FLST`/`FGET` by
  reading those paths off disk. (Virtual-file sources — `CFSTR_FILECONTENTS` from
  an archive — are out of scope v1; only `CF_HDROP` is read.)

### macOS adapter — File Provider extension + daemon proxy

- One **replicated** provider domain (`NSFileProviderReplicatedExtension`),
  registered once via `NSFileProviderManager.add(domain:)`. Layout
  `~/Library/CloudStorage/clipp/<eventGuid>/…`. The domain is *ephemeral scratch*,
  not a product surface.
- **Provide (paste target):** on receiving a manifest, the daemon writes the
  top-level entries as **dataless placeholder items** under `<eventGuid>/` into
  the app-group store and signals the enumerator; it puts `public.file-url`s for
  those items on `NSPasteboard.general` (this becomes the receiver's active
  clipboard, mirroring text/image sync). `enumerateItems` → `FLST`;
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

### Serve policy — the one open decision

Everything above is identical regardless of how long the origin honors `FLST`/
`FGET` for a bundle. That lifetime is a **single sender-side predicate**:

```
CanServe(eventGuid) -> bool
```

- **Option A — active-only (both sides):** serve iff `eventGuid` is the origin's
  *current* clipboard item. Most secure, smallest staleness window, but couples
  the two devices' clipboard states: when the origin copies something else, every
  receiver holding those URLs must be told to tear down (a distributed
  revocation), or their paste fails late.
- **Option B — history-served (receiver-active only):** serve while the bundle is
  live in the origin's `ClipboardActivityStore`. Decoupled (no cross-device
  revocation), and **consistent with the synced clipboard**: because sync pushes
  on copy and the user pastes later, a receiver's clipboard routinely holds a
  bundle the origin has already moved past — under A that common case breaks,
  under B it works. The cost is that file history entries are *claim checks* that
  can go stale (file changed/deleted, origin offline) and a longer serving
  surface.

**Recommendation — B by default, A for private, both TTL-bounded:**

- Default (non-private): serve from history, bounded by an idle **TTL** reusing
  the registers `touched + TTL` expiry mindset (drop-on-access, no timer). Bounds
  the security/staleness window without the revocation coordination of A.
- `CLPM_FLAG_SOURCE_MARKED_PRIVATE` bundles ⇒ **A semantics**: served only while
  the active clipboard item, never retained as a servable history entry. The
  privacy marker already means "be careful"; it is the natural selector, and it
  puts the strict policy exactly where it earns its cost.
- Either way it is **by-reference**: bytes are read at pull time; a changed/
  deleted file or offline origin surfaces as a clean OS error (the free failure
  UI). The TTL keeps stale claim-checks from lingering as paste-time landmines.

This is localized to `CanServe` + an item flag, so it is reversible — ship B,
revisit if the staleness bites.

### Lifetime / teardown / layering

- Receiver placeholders (mac items / the win `IDataObject`) are torn down when the
  origin's bundle stops being servable: immediately for private (active-only),
  at TTL for normal. The daemon already watches clipboard changes — drive
  teardown off that.
- **Layering law (inherited from registers):** the replicated layer changes only
  for explicit user ops and the shared TTL policy. Per-device history limits and
  scratch GC are local and never emit replicated mutations. `CanServe` is a local
  policy, not replicated state.

## Open questions / probes

1. **mac hydrate-on-paste** (low risk, cheap): a `swiftc` script sets a
   `public.file-url` for an **evicted iCloud Drive file** on `NSPasteboard.general`;
   Cmd-V into a local folder; confirm it downloads-then-copies. Proves the
   "clipboard URL into a dataless domain hydrates on paste" mechanism generically
   without building an extension.
2. **mac sandbox read** (standing #1 risk): can a sandboxed (MAS) build open a
   pasteboard-origin `public.file-url` via `startAccessingSecurityScopedResource`?
3. **win descriptor timing**: does Explorer pull `CFSTR_FILEDESCRIPTOR` only at
   paste, or earlier (menu open / hover)? Affects when the `FLST` tree-walk fires.
   A Win32 `IDataObject` probe logging each `GetData` answers it.
4. **`fetchContents` whole-file**: confirm there is no byte-range hydration in the
   replicated API (so a huge single file double-stores transiently). Expected.

## Limits (extend `ClipboardLimits.h`)

- Max top-level entries per bundle; max total declared size before we refuse to
  advertise; per-`FGET` chunk size. Path-escape rejection on every `FLST`/`FGET`
  relPath (no `..`, no absolute, no symlink-out). Refusals are origin-side, never
  background eviction (layering law).

## Phases

1. **Engine + wire (pure, unit-tested).** Manifest type + serialization,
   `FLST`/`FGET` frames (BE, `RegisterWire`-style), `CAP0_SERVES_FILES`,
   `CLIPP_FORMAT_FILES`, an origin-side `FileBundleSource` (disk → listChildren/
   readContents with path-escape guards), `CanServe`. doctest over a temp tree.
2. **Windows end-to-end.** `IDataObject`/`IStream` provider + `OleSetClipboard`,
   `CF_HDROP` read, `ReadClipboardData`/`SetClipboardData` files branch, activity
   `Files` kind + preview. Win→Win copy/paste. (No mac dependency — ship-able.)
3. **macOS end-to-end.** File Provider extension target + XPC proxy + app group,
   enumerator/`fetchContents` → daemon → `FLST`/`FGET`, `public.file-url` read +
   the sandbox spike. mac↔mac and mac↔win.
4. **Policy + reach.** TTL serve-policy + private→A, teardown/GC, history-UI
   integration, relay/one-shot CLI path (bytes pull from origin, not the gateway),
   iOS (`UIPasteboard` + `NSItemProvider.registerFileRepresentation` is genuinely
   lazy and system-honored — closer to Windows than mac).

## Notes that bit us / will bite

- The `IStream` (win, ranged) vs `fetchContents` (mac, whole-file) split is the
  one place the engine's `readContents(offset,length)` is exercised differently —
  keep the range params even though mac always asks for the whole file.
- Don't reconnect peers from the mac extension; proxy to the daemon.
- A file history entry is not self-contained the way a text entry is — it is a
  claim check on a live origin. The UI must not present it as guaranteed-available.
