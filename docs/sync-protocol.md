# Sync protocol (draft)

> Status: design notes. Nothing here is implemented yet. Write the wire format down here
> before writing `core/src/sync`.

## Goals

- No server. Any two paired devices can sync in either direction.
- Edits made offline on several devices merge without asking the user, except for true
  conflicts on the same field, which resolve deterministically.
- A device can ask for lossy copies (e.g. a phone wanting Opus 128) instead of originals.

## Discovery

- mDNS/DNS-SD service `_musicapp._tcp`, announced and browsed by an embedded responder
  (mjansson/mdns) on a background thread. Default scan interval: 30 s.
- The TXT record carries `node=<node_uid>`, `name=<device name>` and `v=<protocol version>`.
- Android must hold a `WifiManager.MulticastLock` while discovery runs.
- The UI shows paired peers that are online, and paired peers that are currently
  unreachable ("offline").

## Pairing and transport

- Each device has a long-term static key pair, created with the library.
- Pairing: device A shows a 6-digit code, and the user types it on device B. A
  PAKE-authenticated handshake (e.g. Noise `XXpsk0`, with the PSK derived from the code)
  exchanges static public keys, which both sides store in `peers.public_key`.
- Later sessions: a Noise `KK` handshake over TCP, so both sides already know each other's
  keys. Everything after the handshake is encrypted.

## Clocks and conflict resolution

- Every change is stamped with a **hybrid logical clock** (`core/src/util/hlc.c`): 48 bits of
  wall-clock milliseconds plus a 16-bit counter. On receipt, a device calls
  `mc_hlc_observe()` so its own clock never falls behind what it has seen.
- **Metadata (artists, albums, tracks, playlists):** each field merges independently. For
  each `(entity, row_uid, field)` the change with the highest `(hlc, origin_uid)` wins. Two
  devices editing different fields of the same track both keep their edit.
- **Deletes** are `_deleted` changes. A delete wins over a concurrent edit only if it has the
  higher hlc. Tombstones are kept until every paired peer has acknowledged them.
- **Playlist order:** entries carry a fractional-index `position`. Inserting between A and B
  generates a key strictly between theirs, plus a random suffix so concurrent inserts don't
  collide. Moving an entry is a `position` change on that entry. Two devices inserting at
  the same spot both keep their entry, ordered by `(position, uid)`.

## Session

1. Handshake.
2. Exchange cursors. Each side sends, for every origin node it knows, the highest hlc it has
   applied from that node (a version vector).
3. Each side streams the changelog rows the other is missing, in hlc order.
4. The receiver applies them in a single transaction with `applying_remote = 1`, so the
   triggers don't re-log them, and inserts the received rows into its own changelog
   unchanged.
5. Audio files: the receiver requests files for tracks it has no `files` row for, in its
   `preferred_format`.

## Lossy-format sync

- The sender transcodes on demand and caches the output, keyed by
  `(file uid, format, quality)`.
- The receiver stores the copy as a `files` row with `source_file_uid` and
  `source_peer_uid` set. It is linked to the same `track_uid`, so it is never imported as a
  new track and never offered back to anyone as an original.
- Tag edits made on the phone are metadata changes and sync back normally. The lossy file's
  embedded tags are rewritten locally and are not authoritative.
- Changing the preferred format invalidates the local copies, which are re-fetched lazily.

## Open questions

- Duplicate merge: two devices import the same album independently. Proposal: when an
  incoming album has the same `mb_albumid` as a local one with a different uid, record an
  alias row and rewrite references to the lexicographically smaller uid.
- How long to keep changelog history once every peer has acknowledged it (compaction).
