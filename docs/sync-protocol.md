# Sync protocol

> Status: specification, being implemented in `core/src/sync`, `core/src/discovery` and
> `core/src/transcode`. Change this document first, then the code.

## Goals

- No server. Any two devices in a group can sync in either direction.
- Edits made offline on several devices merge without asking the user, except for true
  conflicts on the same field, which resolve deterministically.
- Each device has a sync profile (what it holds, in what format) that any device in the
  group can edit.
- At least one device, an archive, always holds the full library as originals.
- Every transferred file is verified.

## Two databases

| Database     | Where                                   | Holds |
|--------------|-----------------------------------------|-------|
| `library.db` | the library folder                      | artists, albums, tracks, playlists, `files`, the library changelog |
| `sync.db`    | the per-device config dir, next to `settings.json` | this device's keys, the group, its devices, sync profiles, capabilities, sync status, the group changelog |

The rule is one-way: the library holds no sync data, but sync may hold library data (the
group's `library_uid`, each device's library `node_uid`).

Both databases use the same changelog shape and the same merge rules (below), each with
its own hybrid logical clock. The **group changelog**'s origin is the `device_uid`; the
**library changelog**'s origin is the library's `node_uid`.

A group syncs exactly one library, identified by `meta.library_uid`. A device whose open
library has a different `library_uid` takes part in group sync only.

Version vectors are never stored. They are computed from the changelog
(`SELECT origin_uid, MAX(hlc) FROM changelog GROUP BY origin_uid`), so they can't drift from
the data after a crash.

## Discovery

- mDNS/DNS-SD service `_musicapp._tcp`, announced and browsed by an embedded responder
  (mjansson/mdns) on a background thread. Default scan interval: 30 s. A device that hasn't
  answered for two intervals is offline.
- The SRV record carries the TCP port the device listens on: 47823 by default, so a firewall
  rule can allow it, or an ephemeral port if 47823 is taken. Hosts with a firewall must allow
  UDP 5353 (mDNS) and TCP 47823.
- TXT record:

  | Key    | Value |
  |--------|-------|
  | `v`    | protocol version, `1` |
  | `dev`  | `device_uid` |
  | `name` | device name |
  | `kind` | `desktop`, `phone` or `watch` |
  | `grp`  | first 8 bytes of SHA-256(`group_uid`), hex; empty when not in a group |

  `grp` lets devices tell members of their own group from strangers without broadcasting the
  group id.
- Android and Wear OS must hold a `WifiManager.MulticastLock` while discovery runs. A watch
  is discoverable only on Wi-Fi.

## Transport

All integers are big-endian.

### Connection

The initiator opens a TCP connection and sends a 4-byte preamble:

```
'M' 'A' <version: u8 = 1> <mode: u8>    mode 1 = pairing, 2 = session
```

Every message after the preamble is a frame: `<len: u16> <bytes[len]>`. Before the Noise
handshake completes, frames carry handshake or pairing messages in the clear. After it, each
frame is one Noise transport message (ciphertext plus 16-byte tag), so plaintext is at most
65,519 bytes.

Inside the encrypted channel each message is `<type: u8> <payload>`. Control payloads are
UTF-8 JSON objects. `ROWS` and `FILE_DATA` are binary.

### Noise

`Noise_KK_25519_ChaChaPoly_SHA256` for sessions, and `Noise_XXpsk3_25519_ChaChaPoly_SHA256`
for pairing. Static keys are each device's X25519 key from `sync.db`. The prologue is the
4-byte preamble. A session handshake fails unless the peer's static key belongs to a
non-removed `devices` row of this device's group.

KK needs the initiator's static key before the first message, and nothing identifies the
initiator in the clear. The responder tries message 1 against the key of each group member
in turn (2 DH operations each) until one authenticates. If none does, it closes the
connection without replying.

Handshake payloads are empty. A non-empty payload from the peer is ignored.

## Pairing

The device that starts pairing is A, the initiator. The device it pairs with is B. B shows
the code and A's user enters it.

1. A connects in pairing mode and sends `PAIR_HELLO` in the clear:
   `{"dev", "name", "kind", "grp", "library_uid", "tracks": <track count>}`.
2. B checks the join rules (below). If pairing is impossible, B replies `PAIR_REFUSE`
   `{"reason": "in_group" | "library_mismatch" | "already_paired" | "busy" | "wrong_code"}` and closes.
   Otherwise B draws a 6-digit code (uniform, `000000`–`999999`), shows it through the
   `on_pair_code` callback, and replies `PAIR_READY` `{"dev", "name", "kind", "grp",
   "library_uid", "tracks"}`.
3. The user types the code on A. Both sides run EC J-PAKE (`mbedtls_ecjpake`, secp256r1,
   SHA-256) with the code's 6 ASCII digits as the shared secret. A is the client:
   `JPAKE_1` both ways, then `JPAKE_2` both ways. Both derive the 32-byte secret.
4. They run `Noise_XXpsk3` with `psk = HKDF-SHA256(jpake_secret, info = "musicapp pair v1")`.
   A wrong code makes the handshake fail at the psk. A's side of XXpsk3 finishes first, so
   only B can tell. On a failure B sends `PAIR_REFUSE` `{"reason": "wrong_code",
   "attempts_left": n}` in the clear and closes. A shows "Wrong code" and may reconnect to try
   again. This reveals nothing an attacker doesn't already know. B allows 3 failed attempts
   per code, then draws a new one (and shows it, with `attempts_left` reset), so each attempt
   is one online guess at 1 in 10⁶.
   A 6-digit code must never key Noise directly: an active attacker could brute-force it
   offline from one handshake.
5. Over the encrypted channel B sends `PAIR_ACCEPT` with the resulting group (below), and
   both store each other's static key in their `devices` rows. The connection then becomes
   an ordinary session (from step 2 of [Session](#session)), so the new member receives the
   whole group and library straight away.

`PAIR_*` and `JPAKE_*` messages in the clear use the same `<type: u8> <payload>` form inside
the frame.

### Join rules

A device is *in a group* when `local.group_uid` is set. Its library is *empty* when it has
no tracks.

| A          | B          | Result |
|------------|------------|--------|
| no group   | no group   | New group. Its library is whichever side's is non-empty (B's if both are empty); the other side's library must be empty, else `library_mismatch`. The device holding the group library becomes the first archive. |
| no group   | in group   | A joins B's group. A's library must be empty or already have the group's `library_uid`. |
| in group   | no group   | B joins A's group, same condition for B. |
| same group | same group | `already_paired` |
| in group   | other group| `in_group`: one device must leave its group first. |

A device joining with an empty library adopts the group's `library_uid` through
`mc_library_adopt_uid()`, which refuses a library that has tracks.

## Group membership

- `devices` rows are replicated like any other group data, so a pairing made between A and
  B reaches C on C's next session with either of them.
- Any member can add (by pairing) or remove a member. Removal sets `removed = 1`. Members
  then refuse that key, and the removed device clears its `local.group_uid` when it sees its
  own row removed.
- A device can't be removed while it is the only archive.

### Archives

- `profiles.archive = 1` fixes that device's profile at "everything, originals". Profile
  setters refuse other values with `MC_ERR_LOCKED`.
- Any member can make a device an archive.
- Only the device itself can clear its own `archive`. It may do so only when another
  archive's `sync_status` shows it verified complete, with `missing = corrupt = 0` and
  `verified_ms` newer than the library's latest change.
- After applying remote group rows, every device checks the merged state. If no device is an
  archive, the demotion with the lowest `(hlc, origin_uid)` is void. Devices treat that
  device as an archive, and it writes `archive = 1` again when it next applies the check.
  Every device evaluates the same rows, so all reach the same answer.

## Clocks and conflict resolution

- Every change is stamped with a **hybrid logical clock** (`core/src/util/hlc.c`): 48 bits of
  wall-clock milliseconds plus a 16-bit counter. On receipt, a device calls
  `mc_hlc_observe()` so its own clock never falls behind what it has seen.
- **Fields** merge independently. For each `(entity, row_uid, field)`, the change with the
  highest `(hlc, origin_uid)` wins. Two devices editing different fields of the same row
  both keep their edit. This applies equally to library and group data.
- **Deletes** are `_deleted` changes. A delete wins over a concurrent edit only if it has the
  higher hlc. Tombstones are kept until every group member has acknowledged them.
- **Sets** (`profile_items`, `playlist_entries`) are one row per member, so concurrent adds
  and removes on two devices both apply.
- **Playlist order:** entries carry a fractional-index `position`. Inserting between A and B
  generates a key strictly between theirs, plus a random suffix so concurrent inserts don't
  collide. Moving an entry is a `position` change on that entry. Two devices inserting at
  the same spot both keep their entry, ordered by `(position, uid)`.

## Session

Message types:

| Type | Name          | Payload |
|------|---------------|---------|
| 0x01 | `HELLO`       | `{"v", "dev", "library_uid", "node_uid", "caps": {...}}` |
| 0x02 | `BYE`         | `{}` |
| 0x03 | `ERROR`       | `{"reason", "msg"}` |
| 0x10 | `VV`          | `{"db": "group" \| "library", "vv": {"<origin>": <hlc>}}` |
| 0x11 | `ROWS`        | binary, below |
| 0x12 | `ROWS_END`    | `{"db", "digest"}` |
| 0x20 | `HAVE_REQ`    | `{"tracks": [uid], "format", "kbps"}` |
| 0x21 | `HAVE_RESP`   | `{"tracks": [{"uid", "original": bool, "cached": bool}]}` |
| 0x22 | `FILE_REQ`    | `{"track", "format", "kbps"}`; `format` `"original"` or `"opus"` |
| 0x23 | `FILE_HDR`    | `{"track", "file", "format", "kbps", "ext", "size", "sha256"}` |
| 0x24 | `FILE_DATA`   | raw bytes, in order, until `size` bytes have been sent |
| 0x25 | `FILE_ERR`    | `{"track", "reason"}`, sent instead of `FILE_HDR` |
| 0x26 | `PREPARE_REQ` | `{"device", "tracks": [uid], "kbps"}`: transcode into the cache for a device that is offline; no reply |
| 0x40 | `PAIR_HELLO`  | pairing only; see [Pairing](#pairing) |
| 0x41 | `PAIR_READY`  | |
| 0x42 | `PAIR_REFUSE` | |
| 0x43 | `JPAKE_1`     | `mbedtls_ecjpake_write_round_one` output |
| 0x44 | `JPAKE_2`     | `mbedtls_ecjpake_write_round_two` output |
| 0x45 | `PAIR_ACCEPT` | `{"group_uid", "library_uid", "archive": dev}` |

Steps:

1. Handshake (Noise KK). Both sides send `HELLO`.
2. **Group data.** Both sides send `VV` for `group`, then stream the group changelog rows the
   other is missing (`ROWS`, in hlc order), then `ROWS_END`.
3. **Library data**, only if both `library_uid`s equal the group's. Same as step 2 with
   `db = "library"`. If a `HELLO`'s `node_uid` is already claimed by another member's
   `devices.node_uid`, the device with that node uid rotates it
   (`mc_library_rotate_node_uid()`) before sending its `VV`.
4. Each receiver applies rows in one transaction per batch with `applying_remote = 1`, so
   the triggers don't re-log them, and inserts the received rows into its own changelog
   unchanged.
5. **Convergence check.** `ROWS_END.digest` is the hex SHA-256 over the sender's version
   vector after applying everything it received: origins sorted, each as
   `<origin>=<hlc>\n`. If the two digests differ once both sides have finished, the session
   logs it and re-sends that database from hlc 0 for the differing origins.
6. **Files**, receiver-driven. See [Files](#files).
7. `BYE`.

Either side may send `ERROR` and close at any time. Sessions are retried with backoff.

### `ROWS`

```
<db: u8>        0 = group, 1 = library
<count: u16>
count × row:
  <hlc: u64>
  <origin: 36 bytes UUID text>
  <entity: u8 len> <bytes>
  <row_uid: 36 bytes>
  <field: u8 len> <bytes>
  <value tag: u8> <value>
```

Value tags: `0` NULL, `1` integer (`i64`), `2` real (IEEE-754 `f64`), `3` text
(`u32 len` + UTF-8), `4` blob (`u32 len` + bytes). Rows are batched so a frame stays under
the plaintext limit.

## Profiles and what a device holds

- The **wanted set** for a device is:
  - mode `all`: every track;
  - mode `selected`: the tracks of the selected albums ∪ the entries of the selected
    playlists.

  Selections that name a uid the library doesn't have are skipped.
- **Target format** per track: `original`, or Opus at the profile's bitrate. A lossy
  original that is already smaller than the Opus estimate is sent as-is.
- **Budget mode**: estimate each track as `duration_ms × kbps / 8 + 4096` bytes, or
  `files.size_bytes` for originals. Walk Original → 256 → 192 → 160 → 128 → 96 → 64 kbps and
  take the highest that fits in `min(budget_bytes, free space + bytes already held of the
  set)`. If even 64 kbps doesn't fit, the profile is over budget: nothing syncs and the UI
  shows the shortfall.
- **Reconcile**, at the end of every session on the receiving device:
  - It deletes files of tracks that are no longer wanted, are in the wrong format, or have
    been deleted. It removes their `files` rows and any album folders left empty.
  - It deletes files in the library folder that no `files` row knows about.
  - Exceptions: originals imported on that device are kept until an archive holds verified
    copies. On an archive, only deleted tracks' files are removed, and unknown files are
    reported, not deleted.
- Every device receives the whole library database. Frontends list only tracks with a local
  file (`available_only`), except the profile picker, which lists everything.

## Files

1. The receiver works out what it is missing: wanted tracks without a `files` row in the
   target format.
2. It sends `HAVE_REQ` to each online member and collects `HAVE_RESP`.
3. It chooses a source per track:
   1. a device with a cached transcode, or with the original when the target is `original`;
   2. otherwise a device holding the original with `can_transcode`, `on_power` and enough
      free space, ranked by `cpu_score`, with the work split across candidates in proportion
      to score.

   Phones on battery and watches never transcode for others.
4. `FILE_REQ` → `FILE_HDR` + `FILE_DATA` (or `FILE_ERR`).
   - The sender computes `sha256` over the exact bytes it sends.
   - The receiver writes to `<dest>.part` while hashing. On a match it renames the file into
     place and inserts a `files` row with `sha256`, `source_file_uid` and
     `source_peer_uid`.
   - On a mismatch it deletes the part file and retries once from another source, then
     counts the track as corrupt.
5. The receiver chooses the path itself, with the same organising rules as imports, so
   paths are never taken from the peer.
6. After the file phase the receiver writes its `sync_status` (`expected`, `ok`, `missing`,
   `corrupt`, `bytes`, `verified_ms`). That row is group data, so every device can show
   "1,203 / 1,203 verified".

When a profile changes for a device that is offline, the best online holder of the
originals is sent `PREPARE_REQ` and fills its transcode cache. The target's next sync is then
transfer-only.

## Lossy copies

- Transcodes are Opus (libopusenc), cached on the transcoding device in
  `<config>/transcode-cache/<file_uid>/opus-<kbps>.opus`, with an LRU size cap.
- The receiver stores the copy as a `files` row with `source_file_uid` and
  `source_peer_uid` set. It is linked to the same `track_uid`, so it is never imported as a
  new track and never offered back to anyone as an original.
- Tag edits made on the phone are metadata changes and sync back normally. The lossy file's
  embedded tags are rewritten locally and are not authoritative.

## Verification

- Transit is authenticated by Noise. File checksums catch truncation and disk errors, and
  give a durable record of what arrived.
- `mc_sync_verify(fast)` compares `size_bytes` and `mtime_ms`; `mc_sync_verify(deep)`
  re-hashes against `files.sha256`. A fast check runs after each sync and a deep one weekly
  or on demand. Failed files are deleted and fetched again on the next session.

## Open questions

- Duplicate merge: two devices import the same album independently. Proposal: when an
  incoming album has the same `mb_albumid` as a local one with a different uid, record an
  alias row and rewrite references (including `profile_items`) to the lexicographically
  smaller uid.
- How long to keep changelog history once every member has acknowledged it (compaction).
