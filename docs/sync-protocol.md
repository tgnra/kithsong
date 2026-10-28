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
| `library.db` | the library folder                      | artists, albums, tracks, playlists, recents, `files`, the library changelog |
| `sync.db`    | the per-device config dir, next to `settings.json` | this device's keys, the group, its devices, sync profiles, capabilities, sync status, the scrobbling account, the group changelog; and, never synced, this device's scrobble queue |

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

- mDNS/DNS-SD service `_kithsong._tcp`, announced and browsed by an embedded responder
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
  | `host` | the machine's host name (`gethostname`, `GetComputerNameExW` on Windows), UTF-8, at most 63 bytes |

  `grp` lets devices tell members of their own group from strangers without broadcasting the
  group id. The default device name, when the frontend gives none, is the host name.
- Android and Wear OS must hold a `WifiManager.MulticastLock` while discovery runs. A watch
  is discoverable only on Wi-Fi.

### Without multicast

Firewalls and some networks drop mDNS. Sync works without it:

- **Pairing by address.** `mc_sync_pair_request_address(host, port)` runs the same pairing
  over a direct TCP connection to an IPv4/IPv6 literal or host name (port 0 means 47823).
- **Address hints.** Each device publishes where it listens as `capabilities.addresses`, a
  JSON array such as `["192.168.1.5:47823","[2001:db8::5]:47823"]` (its non-loopback IPv4
  addresses and one global IPv6 address, with its port), and updates it when its interfaces
  or port change. It replicates like other group data.
- **Where to call a member,** in order: the address discovery reports, then its hints, then
  the address that last worked (`peer_contact`, local to this device's `sync.db`, never
  replicated). Once paired, devices reconnect without mDNS.

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

A connection fails once a send or receive has made no progress for 30 s (a connect, after
3 s). A peer can go silent without the connection being reset, e.g. a phone whose network
Android has blocked drops packets, so this is how a session with it ends, to be retried with
backoff. Where a message can legitimately take longer (B waits up to 5 minutes for A's
first pairing handshake message while the user types the code), the waiting side uses a
longer timeout for that message only.

### Noise

`Noise_KK_25519_ChaChaPoly_SHA256` for sessions, and `Noise_XXpsk3_25519_ChaChaPoly_SHA256`
for pairing. Static keys are each device's X25519 key from `sync.db`. The prologue is the
4-byte preamble. A session handshake fails unless the peer's static key belongs to a
`devices` row of this device's group. If that row is `removed`, the responder completes the
handshake only to send `ERROR {"reason": "removed"}` (see [Group membership](#group-membership)),
reads what the peer sent meanwhile, and closes; nothing else is exchanged.

KK needs the initiator's static key before the first message, and nothing identifies the
initiator in the clear. The responder tries message 1 against the key of each group member
in turn (2 DH operations each) until one authenticates. If none does, it closes the
connection without replying.

Handshake payloads are empty. A non-empty payload from the peer is ignored.

## Pairing

The device that starts pairing is A, the initiator. The device it pairs with is B. B shows
the code and A's user enters it.

1. A connects in pairing mode and sends `PAIR_HELLO` in the clear:
   `{"dev", "name", "kind", "host", "grp", "library_uid", "tracks": <track count>,
   "group_library_uid"}`. `library_uid` is A's open library; `group_library_uid`, sent only
   when A is in a group, is the library its group syncs, which the join rules need.
2. B checks the join rules (below). If pairing is impossible, B replies `PAIR_REFUSE`
   `{"reason": "in_group" | "library_mismatch" | "already_paired" | "busy" | "wrong_code"}` and closes.
   Otherwise B draws a 6-digit code (uniform, `000000`–`999999`), shows it through the
   `on_pair_code` callback, and replies `PAIR_READY` with the same fields as `PAIR_HELLO`.
   B serves one requester at a time: another device gets `busy` until that pairing ends
   or its code expires (5 minutes unused). The same requester coming back after a wrong code
   keeps its code. A that gives up while B waits for `JPAKE_1` sends `PAIR_REFUSE`
   `{"reason": "cancelled"}`, so B stops showing the code.
3. The user types the code on A. Both sides run EC J-PAKE (`mbedtls_ecjpake`, secp256r1,
   SHA-256) with the code's 6 ASCII digits as the shared secret. A is the client:
   `JPAKE_1` both ways, then `JPAKE_2` both ways. Both derive the 32-byte secret.
4. They run `Noise_XXpsk3` with `psk = HKDF-SHA256(jpake_secret, info = "kithsong pair v1")`.
   A wrong code makes the handshake fail at the psk. A's side of XXpsk3 finishes first, so
   only B can tell. On a failure B sends `PAIR_REFUSE` `{"reason": "wrong_code",
   "attempts_left": n}` in the clear and closes. A shows "Wrong code" and may reconnect to try
   again. This reveals nothing an attacker doesn't already know. B allows 3 failed attempts
   per code, then draws a new one (and shows it, with `attempts_left` reset), so each attempt
   is one online guess at 1 in 10⁶.
   A 6-digit code must never key Noise directly: an active attacker could brute-force it
   offline from one handshake.
   A tells B's encrypted `PAIR_ACCEPT` from a clear `PAIR_REFUSE` by trying to decrypt the
   frame first.
5. Over the encrypted channel B sends `PAIR_ACCEPT` with the resulting group (below), and
   both store each other's static key in their `devices` rows. When the group is A's (A in a
   group, B not), B doesn't know it: B sends an empty `PAIR_ACCEPT` `{}` and A answers with
   its own `PAIR_ACCEPT` `{"group_uid", "library_uid"}`. The device whose group it is writes
   the other's `devices` row with `added_by` set to itself; for a new group B also writes
   `sync_group`, its own row and the archive's `profiles` row (`archive = 1`, mode `all`,
   format `original`). The joiner adopts the library uid if needed, sets `local.group_uid`
   and writes the other's row (name, kind, host, key) so it can reconnect even if the
   session breaks. The connection then becomes an ordinary session from its `HELLO` (step 1
   of [Session](#session), without the handshake), so the new member receives the whole
   group and library straight away. No files move yet: the new member has never confirmed
   its profile (see [Confirming profiles](#confirming-profiles)).

`PAIR_*` and `JPAKE_*` messages in the clear use the same `<type: u8> <payload>` form inside
the frame.

### Join rules

A device is *in a group* when `local.group_uid` is set. Its library is *empty* when it has
no tracks.

| A          | B          | Result |
|------------|------------|--------|
| no group   | no group   | New group. Its library is whichever side's is non-empty (B's if both are empty); the other side's library must be empty, else `library_mismatch`, unless both have the same `library_uid` (a copied library folder), which is one library: B's side holds it. The device holding the group library becomes the first archive. |
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
  own row removed: either in a session's group data, or from the `ERROR "removed"` a
  member answers its next session with. It then deletes the group's data from its `sync.db`
  without logging it (keeping only its own `capabilities` row), so nothing of the old group
  reaches a later one. The exception is the scrobbling account (below), which stays with
  its changes: leaving doesn't sign the device out, and the account goes with it.
- Leaving is removing oneself: the device marks its own row removed, tells one reachable
  member in a session, and forgets the group.
- A device can't be removed, or leave, while it is the only archive (`MC_ERR_LOCKED`).
- Each device keeps its own `devices` row up to date (`name`, `kind`, `host`, key, and
  `node_uid` once its library is the group's, `confirmed_profile` when its user confirms
  its profile) and its own `capabilities`.
- **Names.** `profiles.name` is the device's friendly name in the group. Like the rest of a
  profile, any member may edit it, an archive's included. NULL means the device's own
  `devices.name`. The device takes the name on: when its effective name (`profiles.name`,
  else `devices.name`) differs from the one it runs under, at start, after a session or
  after a local edit, it switches to it, announces it through discovery and writes it to its
  own `devices.name`, so nearby devices and pairing see the group's name for it.

### Scrobbling account

`scrobble_account` is the group's one account with Last.fm or a service with the same API
(see `core/include/musiccore/scrobbler.h`): the service's addresses, API key and secret
(NULL: Last.fm and the build's key), the session key and the user name. Its one row has the
fixed uid `00000000-0000-0000-0000-000000000000` on every device, so a device that signed in
before it joined a group merges with the group's account by last writer wins, field by field.
Signing in on any member signs in the group; signing out, or changing the service, signs out
the group. Each device scrobbles only its own plays, from `scrobble_queue`, which is never
synced.

### Archives


- `profiles.archive = 1` fixes that device's profile at "everything, originals", no budget.
  Profile setters refuse other values with `MC_ERR_LOCKED`; the name, budget size and
  selection stay editable. The getter shows an archive's profile as everything, originals,
  no budget, but the stored `mode`, `format`, `bitrate_kbps` and `budget_enabled` are left
  as they were (setters don't write them), and apply again if the device stops being an
  archive.
- Any member can make a device an archive (`mc_sync_set_archive(dev, 1)`). Making and
  clearing write only `archive` (a device with no `profiles` row gets a bare one first, as
  for any first edit), so concurrent edits to the rest of the profile all apply.
- Only the device itself can clear its own `archive`. It may do so only when another
  archive's `sync_status` shows it verified complete, with `missing = corrupt = 0` and
  `verified_ms` newer than the library's latest change (the wall time of the newest hlc in
  this device's library changelog, so the open library must be the group's). Otherwise
  clearing fails with `MC_ERR_LOCKED`.
- After applying remote group rows, every device checks the merged state. If no device is an
  archive, the demotion with the lowest `(hlc, origin_uid)` is void. Devices treat that
  device as an archive, and it writes `archive = 1` again when it next applies the check.
  Every device evaluates the same rows, so all reach the same answer.
  - A *demotion* is the winning `profiles.archive` change of a current (not removed)
    member's row when its value is 0 and an earlier change (by `(hlc, origin_uid)`) to that
    field set 1. A 0 with no 1 before it (the defaults a logged insert writes) is not one.
  - The check is a function of the changelog, so it isn't stored: everything that asks
    whether a device is an archive (the profile getter, the device list, the plan, reconcile,
    removal and leaving, the choice of transcoders) counts the void one.
  - A device applies the check right after each session's group exchange, and again before
    the next session's `HELLO` in case that write failed. A write after the exchange leaves
    the two devices' group data different until their next session, so as for the node uid
    in step 3 of [Session](#session) the session asks for another one soon after.
  - It converges: until the write reaches a device, that device evaluates the same rows to
    the same void demotion; once it arrives, the device is an archive by its stored flag and
    the rule no longer applies. A demotion can only be voided while no device is an archive,
    and only the device itself can clear its flag again, under the rule above.

## Clocks and conflict resolution

- Every change is stamped with a **hybrid logical clock** (`core/src/util/hlc.c`): 48 bits of
  wall-clock milliseconds plus a 16-bit counter. On receipt, a device calls
  `mc_hlc_observe()` so its own clock never falls behind what it has seen.
- **Fields** merge independently. For each `(entity, row_uid, field)`, the change with the
  highest `(hlc, origin_uid)` wins. Two devices editing different fields of the same row
  both keep their edit. This applies equally to library and group data.
- **Deletes** are `_deleted` changes. A delete wins over a concurrent edit only if it has the
  higher hlc. Tombstones are kept until every group member has acknowledged them.
- **Sets** (`profile_items`, `playlist_entries`, `album_group_entries`) are one row per
  member, so concurrent adds
  and removes on two devices both apply.
- **Playlist order:** entries carry a fractional-index `position`. Inserting between A and B
  generates a key strictly between theirs, plus a random suffix so concurrent inserts don't
  collide. Moving an entry is a `position` change on that entry. Two devices inserting at
  the same spot both keep their entry, ordered by `(position, uid)`.
- **Recents** are keyed by the opened item's uid, so the same item opened on two devices is
  one row, and its newest open wins like any field.

## Session

Message types:

| Type | Name          | Payload |
|------|---------------|---------|
| 0x01 | `HELLO`       | `{"v", "dev", "library_uid", "node_uid", "user": true, "covers": true, "caps": {...}}` (`user` only when pressed for; `covers`: it serves `COVER_REQ`) |
| 0x02 | `BYE`         | `{}` |
| 0x03 | `ERROR`       | `{"reason", "msg"}` |
| 0x10 | `VV`          | `{"db": "group" \| "library", "vv": {"<origin>": <hlc>}, "resend": true}` (`resend` only in the re-send round) |
| 0x11 | `ROWS`        | binary, below |
| 0x12 | `ROWS_END`    | `{"db", "digest"}` |
| 0x20 | `HAVE_REQ`    | `{"tracks": [uid], "format", "kbps"}` |
| 0x21 | `HAVE_RESP`   | `{"tracks": [{"uid", "original": bool, "cached": bool, "verified": bool}]}` |
| 0x22 | `FILE_REQ`    | `{"track", "format", "kbps"}`; `format` `"original"` or `"opus"` |
| 0x23 | `FILE_HDR`    | `{"track", "file", "format", "kbps", "ext", "size", "sha256"}` |
| 0x24 | `FILE_DATA`   | raw bytes, in order, until `size` bytes have been sent |
| 0x25 | `FILE_ERR`    | `{"track", "reason"}`, sent instead of `FILE_HDR` |
| 0x26 | `PREPARE_REQ` | `{"device", "tracks": [uid], "kbps"}`: transcode into the cache for a device that is offline; no reply |
| 0x27 | `FILES_DONE`  | `{}`: the receiver has asked for everything it wants; the turn passes |
| 0x28 | `COVER_REQ`   | `{"album"}`; only to a peer whose `HELLO` has `"covers": true` |
| 0x29 | `COVER_HDR`   | `{"album", "ext", "size", "sha256"}`, then the image as `FILE_DATA`; or `FILE_ERR {"album", "reason"}` instead |
| 0x40 | `PAIR_HELLO`  | pairing only; see [Pairing](#pairing) |
| 0x41 | `PAIR_READY`  | |
| 0x42 | `PAIR_REFUSE` | |
| 0x43 | `JPAKE_1`     | `mbedtls_ecjpake_write_round_one` output |
| 0x44 | `JPAKE_2`     | `mbedtls_ecjpake_write_round_two` output |
| 0x45 | `PAIR_ACCEPT` | `{"group_uid", "library_uid", "archive": dev \| null}`, or `{}`; see [Pairing](#pairing) |

Steps:

1. Handshake (Noise KK). Both sides send `HELLO`; `caps` is
   `{"cpu_score", "free_bytes", "on_power", "can_transcode"}`. The `dev` in a `HELLO` must
   be the device the handshake authenticated. Before its `HELLO`, each device brings its own
   `devices` and `capabilities` rows up to date, and writes the `sync_status` its last file
   phase left pending (see [Files](#files)). `"user": true` says this device's user pressed
   Sync for the peer (see [Confirming profiles](#confirming-profiles)); older versions
   ignore it.
2. **Group data.** Both sides send `VV` for `group` (the initiator first), then stream the
   group changelog rows the other is missing (`ROWS`, in hlc order, read from one snapshot)
   and `ROWS_END`. The exchange is half-duplex, so neither side's stream can block the
   other's:
   1. the initiator streams its rows, then `ROWS_END` with a provisional digest;
   2. the responder applies them, streams its own, then `ROWS_END` with its final digest;
   3. the initiator applies those and sends a second `ROWS_END` with its final digest.
3. **Library data**, only if both `library_uid`s equal the group's (from `sync_group`, just
   synced). Same as step 2 with `db = "library"`; the initiator decides, and a responder that
   disagrees answers `ERROR {"reason": "library"}`. Before it, each device checks its
   library's `node_uid`: if another member's `devices.node_uid` already records it, or the
   peer's `HELLO` carries it, one of them rotates it (`mc_library_rotate_node_uid()`): the
   one that hasn't recorded it, or if both or neither have, the higher device uid. Each
   then records its node uid in its own `devices` row. That write comes after the group
   exchange, so the peer gets it in the next session, which follows a few seconds later.
4. Each receiver applies rows in one transaction per batch with `applying_remote = 1`, so
   the triggers don't re-log them, and inserts the received rows into its own changelog
   unchanged. See [Applying rows](#applying-rows).
5. **Convergence check.** `ROWS_END.digest` is the hex SHA-256 over the sender's version
   vector after applying everything it received: origins sorted, each as
   `<origin>=<hlc>\n`. Both sides compare the responder's final digest with the initiator's
   final one. If they differ, the session logs it and runs step 2 (or 3) once more with
   `"resend": true`: each side sends every row, from hlc 0, of each origin whose newest hlc
   differs between the two new `VV`s. It doesn't retry further.
6. **Files**, receiver-driven, when step 3 ran. See [Files](#files). Each side is the
   receiver in turn, the initiator first: the receiver sends `HAVE_REQ`, `FILE_REQ` and
   `PREPARE_REQ` while the other only answers, and ends its turn with `FILES_DONE`.
7. `BYE`, the initiator first; the responder answers with `BYE`. Each side then reconciles
   its library folder, so the peer can no longer ask for a file it is about to delete.

Either side may send `ERROR` and close at any time. Reasons used: `removed` (above),
`busy` (a session between the two is already running; see [Scheduling](#scheduling)) and
`library`. A device that refuses a session with `ERROR` reads what the peer sent meanwhile
before closing, so the TCP reset of an unread close can't destroy the `ERROR` in transit.
Sessions are retried with backoff (10 s, doubling, up to 5 minutes).

### Applying rows

- A row already in the changelog (same hlc, origin, entity, row uid and field) is skipped.
- A row naming an entity or field this version doesn't know is logged, so it is passed on,
  but never applied.
- A row is *live* if one of its field changes is newer, by `(hlc, origin_uid)`, than its
  newest `_deleted`. A dead row that exists is deleted. A live row that exists gets each
  field the batch brought a new winner for. A live row that doesn't exist yet (new, or
  brought back by an edit newer than its delete) is inserted with every field's winner, once
  the changelog has a value for each `NOT NULL` column without a default; until then it
  waits, so a row's fields may arrive in any order and across batches.
- Tables are applied parents first, in an order derived from their foreign keys, with
  `defer_foreign_keys` on. ON DELETE actions still run, unlogged, as they did on the origin.
  A reference to a parent that doesn't exist at commit (it was deleted concurrently) is
  resolved as ON DELETE would: set to NULL, or the row deleted where the column is
  `NOT NULL`. Every device resolves it the same way.
- Duplicates are merged as rows are applied; see [Duplicates](#duplicates).

### Duplicates

Two devices can import the same album independently, each making its own artist, album and
track rows with uids of their own. Once their rows meet, every device merges them, the same
way, without logging anything.

- **What is one.** Two artists are one when they share an `mb_artistid`, two albums when
  they share an `mb_albumid`, two tracks when they share an `mb_releasetrackid` (a track on
  one release, so the same recording on two releases stays two tracks), and two singles
  albums (`kind = 'singles'`) when their artists are one. Empty ids don't count. "Share"
  means any value the changelog has ever held for the field, not only the current winner,
  so a class only ever grows as rows arrive: a later edit that changes or clears an id
  doesn't split it. The classes are therefore a function of the set of changelog rows, the
  same on every device whatever order the rows came in.
- **Under which uid.** A class is one row under its smallest uid (the canonical uid). Its
  fields and whether it is live are worked out from the changelog rows of every uid in the
  class together, as if they were one row's: field by field the highest `(hlc, origin_uid)`
  wins, and a delete of any of them is the class's delete. Recents rows, keyed by the
  item's uid, merge the same way.
- **References.** `tracks.album_uid` and `album_group_entries.album_uid`, `albums.artist_uid`
  and `tracks.artist_uid`, and `playlist_entries.track_uid`, `track_plays.track_uid` and
  the device-local `files.track_uid` are moved to the canonical uid, so each device's files
  stay attached to the surviving track. Two `track_plays` rows that would then share a
  `(track_uid, node_uid)` keep only the smaller uid. If the whole class is deleted, the
  references go as ON DELETE takes them. Album groups or playlists that held both copies
  hold the survivor twice.
- **Late rows.** A row naming a merged-away uid, from a device that hasn't merged yet, is
  logged as it is and applied to the canonical row; a row whose reference names one is
  pointed at the canonical uid.
- **Sync profiles.** `profile_items` in `sync.db` are not rewritten, since `sync.db` holds
  what its own changelog says. An `album` or `artist` item that names a merged-away uid is
  read as naming the canonical uid (by `mc_sync_get_profile()` and the wanted set), and
  removing that album or artist from a profile deletes every stored row that resolves to
  it.
- **Not logged.** The merge writes nothing to the changelog. Each device derives it from
  the same rows, as it does the reference resolution above, so its tables agree with every
  other device's once their changelogs do, and the convergence digest (over the version
  vectors) is unaffected. Logging it instead would have every device that merges add its
  own copy of each rewrite, and a rewrite stamped later than a concurrent edit (moving a
  track to another album, say) would override it.
- **Local state.** `library.db` keeps `aliases` (each merged-away uid and its canonical uid)
  and `merge_keys` (the ids ever logged per uid); neither is logged. `meta.merge_seq` is the
  changelog `seq` read so far, and each applied batch reads on from there, so rows written
  locally are taken into account at the next batch too.
- **Imports.** An import looks albums up by `mb_albumid` first and artists by
  `mb_artistid`, so importing an album a device already has, merged or not, adds to the
  survivor rather than making a second copy.

### Scheduling

- A device syncs with each member when discovery reports it online, every 5 minutes, and
  3 seconds after a local change (or after a session brought it rows its other members may
  lack). One session per pair of devices at a time. These automatic sessions always
  exchange group and library data; files move in them only for confirmed profiles (see
  [Confirming profiles](#confirming-profiles)).
- The lower `device_uid` is the preferred initiator. On the online and periodic triggers the
  higher one waits 30 s and initiates only if it hasn't been contacted since, so a device
  behind a firewall still syncs through its own outgoing connection. A change only one side
  knows about is sent by that side at once.
- If both still call each other at once, the session initiated by the lower uid wins: the
  other side's is aborted, or refused with `ERROR {"reason": "busy"}`.
- **Rate limit.** After a complete session with a member (it succeeded, converged and left
  nothing to follow up), in either direction, a device starts no automatic session with it
  for an hour (`MC_SYNC_AUTO_INTERVAL_MS`): online, periodic, change and background
  (`mc_sync_background()`) triggers are held until the hour is up, then go ahead, so local
  changes wait too. Sessions the member starts are served as ever, and `mc_sync_now()` and
  `mc_sync_confirm_now()` aren't held. A failed or incomplete session lifts the limit, so
  retries and follow-ups keep their schedule. The hour is counted from
  `peer_contact.last_sync_ms`, so it survives a restart. The other device keeps its own
  limit; nothing about it goes over the wire. So where only the member can connect (a
  firewall), a press of Sync here waits for a session it starts, which it won't while its
  own limit holds; the press lapses after its 5½ minutes, and the user presses Sync there.

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

- A device's **profile** is its `profiles` row plus its `profile_items`:
  - `name`: its friendly name, NULL for its own `devices.name` (see
    [Group membership](#group-membership));
  - `mode`: `all` or `selected`;
  - `format`: `original` or `opus`, with `bitrate_kbps` one of 256, 192, 160, 128, 96, 64
    (NULL for `original`);
  - `budget_enabled` and `budget_bytes` (NULL when unset). The format and the budget
    exclude each other: with `budget_enabled` the format is **auto**, picked by the budget
    (see Budget mode), and editors store it as `original` with a NULL `bitrate_kbps`.
    Choosing a format turns the budget off, keeping `budget_bytes`. Older versions let a
    profile have both, the budget walking down from the format; such a row, or one a
    concurrent edit left with both, reads as auto. Nothing is rewritten: an auto row is one
    those versions read the same way, as originals walking down;
  - `archive` (see [Archives](#archives));
  - the selection, one `profile_items` row per item: `kind` is `album`, `artist`,
    `playlist` (manual) or `album_group` (manual), and `ref_uid` the library uid it names.
    The selection is kept while the mode is `all`, so it comes back when the mode returns to
    `selected`.

  A device with no `profiles` row receives no files until it has one (see below). Pairing
  writes the row straight away (the archive's for a new group, and the joiner's when it
  takes its name), so this only lasts until those rows arrive. For display, the profile
  getter returns what an editor would start from: `all`, `original`, no budget, no name. A
  row created by an edit logs only the fields it changes (not the defaults), so it can't
  overwrite a member's concurrent edit to another field.
- Any member may edit any member's profile. Editors write only what differs from the stored
  profile: changed fields, and an insert (with a new uid) per added item or a delete per
  removed one, so concurrent edits on two devices both apply. A `(kind, ref_uid)` stored more
  than once (two members adding it at once) counts once; removing it deletes every copy.
  Items of a kind a version doesn't know are kept as they are and ignored.
- The **wanted set** for a device is:
  - mode `all`: every track;
  - mode `selected`: the tracks of the selected albums ∪ the tracks whose `artist_uid` is a
    selected artist ∪ the tracks of the albums whose `artist_uid` is a selected artist ∪
    the tracks of the albums in the selected album groups ∪ the entries of the selected
    playlists.

  Selections that name a uid the library doesn't have are skipped, and so are items of
  kinds the device doesn't know.
- A device holds files only once it has a `profiles` row (written when it joins, see
  [Group membership](#group-membership)): until then its file phase fetches and deletes
  nothing. An archive holds everything as originals whatever the rest of its row says.
- **Target format** per track: `original`, or Opus at the profile's bitrate (for auto, the
  bitrate the budget picks). A lossy original that is already smaller than the Opus
  estimate is sent as-is. Lossy means MP3, AAC, Ogg Vorbis, Opus, WMA or Musepack, or an
  `.m4a`/`.mp4` with a known bitrate under 500 kbps (otherwise it may be ALAC). The sender
  applies the rule (it has the file), and answers a request for Opus with the original
  then; the receiver applies it too, to files it already holds.
- **Budget mode** (the auto format): estimate each track as `duration_ms × kbps / 8 + 4096`
  bytes, or `files.size_bytes` for originals. Walk Original → 256 → 192 → 160 → 128 → 96 → 64 kbps and
  take the highest that fits in `min(budget_bytes, free space + bytes already held of the
  set)`. If even 64 kbps doesn't fit, the profile is over budget: nothing syncs (the file
  phase fetches and deletes nothing, so what the device holds stays) and the profile editor
  shows the shortfall from `mc_sync_estimate_profile()` (on Windows' device page; the other
  frontends have no profile editor yet), for the user to raise the budget or select less.
  - The walk always starts at originals, whatever `format` the row holds.
  - An original's size is the same file everywhere, so any device that holds it knows it;
    one no device at hand holds is estimated at 1000 kbps. The bytes already held are only
    known to the device itself (`files` is local), so a plan worked out for another device
    (for `PREPARE_REQ`) counts its free space alone.
  - The profile editor's estimate (`mc_sync_estimate_profile()`) is the same plan worked
    out for a profile that isn't stored yet, so it agrees with what the next sync does: the
    track count, the size, the bitrate the walk picks, or over budget and the shortfall,
    plus the set's size at every step of the walk so the editor can show the options. Free
    space is measured for the device running it and taken from `capabilities.free_bytes`
    for another. A device with no `profiles` row is estimated as if the candidate were
    stored.
- **Reconcile**, at the end of every session on the receiving device, after `BYE`, when its
  plan is not over budget:
  - It deletes files of tracks that are no longer wanted, are in the wrong format, or have
    been deleted. It removes their `files` rows and any album folders left empty (holding
    nothing but a `cover.jpg` or `cover.png`, which go too). A wrong-format copy of a wanted
    track stays until a copy in the right format has arrived, so the track stays playable.
  - It deletes files in the library folder that no `files` row knows about. Only audio
    files (the extensions import takes) and stale `.part` files count, and only once they
    are an hour old, so an import that is copying files in isn't raced.
  - Deleted tracks: `files` rows go with their track (`ON DELETE CASCADE`) when the library
    exchange applies the delete, so the session notes the rows it deletes and removes their
    files. A file whose row went otherwise (a crash, an edit outside a session) is an
    unknown file.
  - Stale copies: when an import on some device replaces a track's original with a
    better file (see "Duplicates" in `core/include/musiccore/tagger.h`), the track gets a
    new `tracks.file_rev`, which syncs like any other field. Every `files` row records the
    `file_rev` it was made from. A received original or transcode whose `file_rev` differs
    from its track's is stale: it doesn't count as holding the track, so the new file is
    fetched, and reconcile then deletes the stale copy as it would one in the wrong format
    (an archive deletes it too, once the current original has arrived). Senders only offer
    originals and transcodes whose `file_rev` is current, and the swapped file gets a new
    file uid, so no transcode cached for the old one is sent. An original imported on a
    device is never stale there: another device's better file doesn't replace it.
  - Exceptions: originals imported on that device are kept until an archive holds verified
    copies. The device learns that in a session with an archive: its `HAVE_REQ` there also
    lists the tracks of such originals it no longer wants as they are, and the archive's
    `HAVE_RESP` says `original` and `verified` (its copy has a recorded `sha256`, i.e. it
    arrived with a matching checksum or passed a deep check). On an archive, only deleted
    tracks' files are removed, and unknown files are reported, not deleted.
  - Deletes are permanent, and only of files inside the library folder: a path that is
    absolute, has a `.` or `..` part or a colon, or passes through a symlink or junction is
    left alone (its `files` row still goes), and the scan for unknown files doesn't follow
    links. Library rows (tracks, albums, their art) are group data and stay: a device drops
    only its own `files` rows, and frontends hide tracks without a file.
- Every device receives the whole library database. Frontends list only tracks with a local
  file (`available_only`), except the profile picker, which lists everything.

### Confirming profiles

A device fetches and deletes files only for a profile its user has confirmed, so pairing,
or an edit made on another member, never moves files before someone has looked at the
profile and pressed Sync.

- A profile's **fingerprint** is the hex SHA-256 of what decides what the device holds, as
  stored: `archive` (an archive, counting a void demotion, is just that), else one line
  `<mode> <format> <kbps> <budget> <budget_bytes>` (`kbps` 0 for originals, `budget` `-` and
  `budget_bytes` 0 when the budget is off), then for mode `selected` one line
  `<kind> <ref_uid>` per distinct `profile_items` row, sorted. Names, the selection while the
  mode is `all`, and an archive's other fields leave it as it is. With no `profiles` row it
  is the hash of `none`. Every device holding the same rows computes the same one.
- `devices.confirmed_profile` is the fingerprint of the device's profile when it was last
  confirmed, written by the device itself (NULL until then). The device is *confirmed* while
  its current fingerprint equals it, *unconfirmed* while it is NULL (a new member) and
  *changed* otherwise. Any member can work this out for any other, so each can show a
  device as paused; an edit this device made that the other hasn't received yet already
  counts.
- In its receiving turn (step 6 of [Session](#session)) a device that isn't confirmed
  fetches nothing, sends no `PREPARE_REQ`, and skips reconcile and its `sync_status`; it
  still serves the peer. Files of tracks deleted from the library still go with their rows.
- **Pressing Sync** (`mc_sync_confirm_now()`) confirms this device's profile at once, and
  marks the peers it was pressed for (one, or every member). The next session with such a
  peer that starts within 5½ minutes, from either side, carries `"user": true` in this
  device's `HELLO`. In a session where either `HELLO` has it, each device confirms its own
  profile after the group exchange (so as it stands with the peer's edits), then runs its
  receiving turn. That write comes after the group exchange, so the peer hears of it in the
  next session, as for the node uid. A sync started otherwise (`mc_sync_now()`, the
  scheduler, background work with `mc_sync_background()`) confirms nothing.

## Files

1. The receiver works out what it is missing: wanted tracks without a `files` row in the
   target format.
2. It sends `HAVE_REQ` to the members it can reach and collects `HAVE_RESP`. A session is
   between two devices, so in practice it asks the session's peer, in batches of up to 400
   tracks (`format`/`kbps` are its profile's; a sender answers at most 600). Each member is
   asked in its own session, and what one can't give is asked of the next.
   - `original`: the sender has an original of the track (imported there or received as
     an original), and the file is there.
   - `cached`: it can send the requested Opus without encoding: from its transcode cache,
     a transcode it received at that bitrate, or a lossy original small enough to stand in.
   - `verified`: its original has a recorded `sha256` (see Reconcile above).
3. It chooses a source per track:
   1. a device with a cached transcode, or with the original when the target is `original`;
   2. otherwise a device holding the original with `can_transcode`, `on_power` and enough
      free space (256 MiB), ranked by `cpu_score`, with the work split across candidates in
      proportion to score.

   Phones on battery and watches never transcode for others; a sender that may not answers
   `FILE_ERR {"reason": "no_transcode"}`. The split: the candidates are the session's peer
   and the other online members that may transcode and hold every original (an archive, or
   `all`/`original` without a budget). Each track goes to one of them by a hash of its uid,
   weighted by `cpu_score`, the same every time; one that falls to another member waits for
   the session with that member.
4. `FILE_REQ` → `FILE_HDR` + `FILE_DATA` (or `FILE_ERR`, reasons `missing`, `no_transcode`,
   `io`, `format`).
   - The sender computes `sha256` over the exact bytes it sends (the recorded one if the
     file's size and mtime still match its row). `file` is the original's file uid: its own,
     or for a transcode the original it was made from. `format` is `"original"` when a lossy
     original stands in for the requested Opus. `ext` is the file's extension; a receiver
     refuses one that isn't an audio extension import takes.
   - The receiver waits up to 10 minutes for `FILE_HDR`, since the sender may be encoding.
   - The receiver writes to `<dest>.part` while hashing. On a match it renames the file into
     place and inserts a `files` row with `sha256` and `source_peer_uid` (the sender), plus
     for a transcode `source_file_uid` (`FILE_HDR.file`). A received original keeps
     `source_file_uid` NULL: it is still an original, served as one in turn, while a row
     with `source_peer_uid` NULL too is an original imported on this device.
   - When the track's album has an `art_hash` (not singles) and its folder no cover yet,
     the receiver sends `COVER_REQ` for it right away, so the album shows with its art while
     the rest of it is still coming. Pictures embedded in received files are never used.
   - Covers. After its fetches, the receiver sends `COVER_REQ` for each folder of its own
     that holds files of an album with an `art_hash` (not singles) and has no cover image
     (`mc_coverart_find()`), for folders whose files were already here, and so a device that
     synced before this catches up in its next session. The sender answers with its cover:
     the image file in a folder holding the album's files (`cover`, `folder` or `front`,
     `.jpg`/`.jpeg`/`.png`, any case), originals' folders first (never a picture embedded
     in a file); at most 32 MiB. It answers `FILE_ERR` `missing` when it has none. The receiver checks
     `sha256` and saves the image as `cover.jpg` or `cover.png` by its magic bytes. A
     device whose peer's `HELLO` lacks `"covers"` (older versions end the session on an
     unknown message) skips this. A folder that already has a cover keeps it, even when
     the album's `art_hash` has since changed.
   - On a mismatch it deletes the part file and retries once from another source, then
     counts the track as corrupt. Within a session the only source is the peer, so the
     retry asks it again; the track is tried anew in the next session, possibly with
     another member.
5. The receiver chooses the path itself, with the same organising rules as imports, so
   paths are never taken from the peer: `<album artist>/<album> (<year>)/<track> <title>.<ext>`,
   or `<artist>/<artist> Singles/<title>.<ext>` for singles, as `<name> (2).<ext>` and so on
   if the path is taken on disk or in `files`.
6. After the file phase (and reconcile and the fast check) the receiver works out its
   `sync_status` (`expected`, `ok`, `missing`, `corrupt`, `bytes`, `verified_ms`). That row
   is group data, so every device can show "1,203 / 1,203 verified". `corrupt` counts the
   tracks that failed twice in the session; `missing` the other wanted tracks without a
   file in the target format; `bytes` every file the device holds. A write after the group
   exchange would leave the two devices' group data different until their next session, so
   the row is kept pending and written before the next session's `HELLO` (which follows a
   few seconds later, as for the node uid in step 3), and only when there is none yet or a
   count changed. `mc_sync_verify()` writes it at once, and also refreshes `verified_ms`
   when it is older than the library's latest change.

One receiver at a time: a device fetching in one session skips its receiving turn in
another that runs meanwhile (it still serves), and `mc_sync_verify()` waits for it.

When a profile changes for a device that is offline, the best online holder of the
originals is sent `PREPARE_REQ` and fills its transcode cache. The target's next sync is then
transfer-only. Any member, in its receiving turn, works out each offline member's plan (from
its profile and the free space in its `capabilities`) and sends `PREPARE_REQ`, in batches of
up to 1000 tracks, if the session's peer is the best online holder: it may transcode, holds
every original, and has the highest `cpu_score` of those (the lower uid on a tie). It sends
once per change in the Opus tracks that member wants, as far as this device remembers
(in memory: after a restart it sends again, and cached files make that cheap). The holder
encodes in the background, so the session goes on; one that may not transcode ignores it.

## Lossy copies

- Transcodes are Opus (libopusenc), cached on the transcoding device in
  `<config>/transcode-cache/<file_uid>/opus-<kbps>.opus`, with an LRU size cap of 8 GiB,
  trimmed after a session or `PREPARE_REQ` that encoded something. One encode runs at a
  time per device.
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
  - The fast check after a session runs after reconcile, so a file it deletes is fetched
    again in the following session.
  - Originals imported on the device are never deleted by a check (there may be no other
    copy): they are only counted as failed.
  - A deep check records the hash of a file that has none (an imported original), which
    then counts as verified.
  - Scheduling the weekly deep check is left to the frontend.

## Open questions

- Duplicates without MusicBrainz ids (untagged imports of one album on two devices) stay
  two. Compaction must keep the rows that decide [Duplicates](#duplicates), or a device
  joining later can't derive the same classes.
- How long to keep changelog history once every member has acknowledged it (compaction).
