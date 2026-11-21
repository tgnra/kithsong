# Sync protocol

> Status: specification, being implemented in `core/src/sync`, `core/src/discovery` and
> `core/src/transcode`. Change this document first, then the code.

## Goals

- No server. Any two devices in a group can sync in either direction.
- Edits made offline on several devices merge without asking the user. The exception is a
  true conflict on the same field, which resolves deterministically.
- Each device has a sync profile (what it holds, and in what format). Any device in the
  group can edit it.
- At least one device, an archive, always holds the full library as originals.
- Every transferred file is verified.

## Two databases

| Database     | Where                                   | Holds |
|--------------|-----------------------------------------|-------|
| `library.db` | the library folder                      | artists, albums, tracks, playlists, recents, `files`, the library changelog |
| `sync.db`    | the per-device config dir, next to `settings.json` | this device's keys, the group, its devices, sync profiles, capabilities, sync status, the scrobbling account, the group changelog; and, never synced, this device's scrobble queue |

The rule only goes one way. The library holds no sync data, but sync can hold library data
(the group's `library_uid`, and each device's library `node_uid`).

Both databases use the same changelog shape and the same merge rules (below). Each has its
own hybrid logical clock. In the **group changelog**, the origin is the `device_uid`. In the
**library changelog**, the origin is the library's `node_uid`.

A group syncs exactly one library, identified by `meta.library_uid`. If a device's open
library has a different `library_uid`, it only takes part in group sync.

Version vectors are never stored. They're computed from the changelog
(`SELECT origin_uid, MAX(hlc) FROM changelog GROUP BY origin_uid`), so they can't drift from
the data after a crash.

## Discovery

- The mDNS/DNS-SD service is `_kithsong._tcp`. An embedded responder (mjansson/mdns)
  announces and browses it on a background thread.
  - The default scan interval is 30 s.
  - Each scan browses and also announces the device again. This keeps it online for others
    even when their queries don't reach it.
  - A device that hasn't been heard for two intervals is offline.
  - A socket is reopened if it fails to send, or if it hears nothing for 1.5 intervals, not
    even its own looped-back packets. A sleep or a Wi-Fi reconnect can leave it out of the
    multicast group while the interface's address stays the same.
- The SRV record carries the TCP port the device listens on. This is 47823 by default, so a
  firewall rule can allow it, or an ephemeral port if 47823 is taken. Hosts with a firewall
  must allow UDP 5353 (mDNS) and TCP 47823.
- TXT record:

  | Key    | Value |
  |--------|-------|
  | `v`    | protocol version, `2` (see [Versions](#versions)) |
  | `vmin` | the oldest protocol version it syncs with, `2` |
  | `app`  | its Kithsong version, such as `1.0.0` |
  | `dev`  | `device_uid` |
  | `name` | device name |
  | `kind` | `desktop` or `phone` |
  | `grp`  | first 8 bytes of SHA-256(`group_uid`), hex; empty when not in a group |
  | `host` | the machine's host name (`gethostname`, `GetComputerNameExW` on Windows), UTF-8, at most 63 bytes |

  `grp` lets devices tell members of their own group from strangers without broadcasting the
  group id. If the frontend doesn't give a device name, the default is the host name.

  Devices of every version are reported. That way the frontends can mark one that this
  version can't sync with before anyone tries (see [Versions](#versions)).

  `kind` is `desktop` or `phone`. An announcement with any other kind is ignored, and a peer
  that starts announcing one is dropped. When members open the group data, a `devices` row of
  kind `watch` is marked removed. So a group left with one device dissolves just like any
  other would.
- Android must hold a `WifiManager.MulticastLock` while discovery runs.

### Without multicast

Firewalls and some networks drop mDNS. Sync still works without it:

- **Pairing by address.** `mc_sync_pair_request_address(host, port)` runs the same pairing
  over a direct TCP connection to an IPv4/IPv6 literal or a host name. Port 0 means 47823.
- **Address hints.** Each device publishes where it listens as `capabilities.addresses`.
  It's a JSON array such as `["192.168.1.5:47823","[2001:db8::5]:47823"]`, holding the
  device's non-loopback IPv4 addresses and one global IPv6 address, each with its port. The
  device updates it when its interfaces or port change. It replicates like other group data.
- **Where to call a member.** A device tries these in order:
  1. the address discovery reports
  2. the member's address hints
  3. the address that last worked (`peer_contact`, local to this device's `sync.db`, never
     replicated).

  Once paired, devices reconnect without mDNS.

## Transport

All integers are big-endian.

### Connection

The initiator opens a TCP connection and sends a 4-byte preamble:

```
'M' 'A' <version: u8 = 1> <mode: u8>    mode 1 = pairing, 2 = session
```

The preamble's version stays 1, whatever the protocol version. The protocol version is given
in `HELLO` and `PAIR_HELLO` instead, where a refusal can say why (see [Versions](#versions)).

Every message after the preamble is a frame: `<len: u16> <bytes[len]>`. Before the Noise
handshake completes, frames carry handshake or pairing messages in the clear. After it, each
frame is one Noise transport message (ciphertext plus a 16-byte tag), so plaintext is at
most 65,519 bytes.

Inside the encrypted channel, each message is `<type: u8> <payload>`. Control payloads are
UTF-8 JSON objects. `ROWS` and `FILE_DATA` are binary.

A connection fails once a send or receive has made no progress for 30 s, or 3 s for a
connect. A peer can go silent without the connection being reset. For example, when Android
has blocked a phone's network, the phone drops packets. This timeout is how a session with
it ends, and the session is retried with backoff.

Some messages can legitimately take longer. For example, B waits up to 5 minutes for A's
first pairing handshake message while the user types the code. In cases like this, the
waiting side uses a longer timeout for that message only.

### Noise

`Noise_KK_25519_ChaChaPoly_SHA256` is used for sessions, and
`Noise_XXpsk3_25519_ChaChaPoly_SHA256` for pairing. The static keys are each device's X25519
key from `sync.db`. The prologue is the 4-byte preamble.

A session handshake fails unless the peer's static key belongs to a `devices` row in this
device's group. If that row is `removed`, the responder only completes the handshake so it
can send `ERROR {"reason": "removed"}` (see [Group membership](#group-membership)). It then
reads what the peer sent in the meantime, and closes. Nothing else is exchanged.

KK needs the initiator's static key before the first message, but nothing identifies the
initiator in the clear. So the responder tries message 1 against each group member's key in
turn (2 DH operations each) until one authenticates. If none does, it closes the connection
without replying.

Handshake payloads are empty. If the peer sends a non-empty payload, it's ignored.

## Pairing

The device that starts pairing is A, the initiator. The device it pairs with is B. B shows
the code, and A's user enters it.

1. A connects in pairing mode and sends `PAIR_HELLO` in the clear:
   `{"v", "v_min", "app", "dev", "name", "kind", "host", "grp", "library_uid", "tracks":
   <track count>, "port", "group_library_uid"}`.
   - `v`, `v_min` and `app` are the same as in `HELLO` (see [Versions](#versions)).
   - `port` is where A listens, so a device that can't see A through discovery can still
     ask back.
   - `library_uid` is A's open library.
   - `group_library_uid` is the library A's group syncs, which the join rules need. It's
     only sent when A is in a group.
2. B first checks A's versions. If the two can't sync, B replies `PAIR_REFUSE`
   `{"reason": "version", "v", "v_min", "app", "dev", "name"}` with its own values, and
   closes. This happens before the join rules, before a code and before any write.

   Then B checks the join rules (below). If pairing is impossible, B replies `PAIR_REFUSE`
   `{"reason": "in_group" | "library_mismatch" | "already_paired" | "busy" | "wrong_code"}` and closes.

   Every `PAIR_REFUSE` also carries the sender's `"v"`, `"v_min"` and `"app"`. If A gets one
   without `"v"` in answer to its `PAIR_HELLO`, it came from a peer too old to say its
   version, which refused by its own join rules. A reports `"version"` rather than the reason
   it gave.

   Otherwise B draws a 6-digit code (uniform, `000000`–`999999`), shows it through the
   `on_pair_code` callback, and replies `PAIR_READY` with the same fields as `PAIR_HELLO`.
   A then checks B's versions in turn. Since B accepted, a B that fails this check can only
   be too old to check versions itself. A sends it `PAIR_REFUSE` `"version"` as above,
   instead of `JPAKE_1`.

   B serves one requester at a time. Another device gets `busy` until that pairing ends or
   its code expires (after 5 minutes unused). If the same requester comes back after a wrong
   code, it keeps its code. If A gives up while B is waiting for `JPAKE_1`, A sends
   `PAIR_REFUSE` `{"reason": "cancelled"}` so B stops showing the code.
3. The user types the code on A. Both sides run EC J-PAKE (`mbedtls_ecjpake`, secp256r1,
   SHA-256), with the code's 6 ASCII digits as the shared secret. A is the client:
   - `JPAKE_1` goes from A first, then from B. This way B sends nothing but a refusal
     before the code is entered.
   - Then `JPAKE_2` goes both ways.

   Both sides derive the 32-byte secret.
4. They run `Noise_XXpsk3` with `psk = HKDF-SHA256(jpake_secret, info = "kithsong pair v1")`.

   A wrong code makes the handshake fail at the psk. A's side of XXpsk3 finishes first, so
   only B can tell. On a failure, B sends `PAIR_REFUSE` `{"reason": "wrong_code",
   "attempts_left": n}` in the clear and closes. A shows "Wrong code" and may reconnect to
   try again. This reveals nothing an attacker doesn't already know. B allows 3 failed
   attempts per code. After that it draws a new code and shows it, with `attempts_left`
   reset. So each attempt is one online guess at 1 in 10⁶.

   A 6-digit code must never key Noise directly. An active attacker could brute-force it
   offline from one handshake.

   A tells B's encrypted `PAIR_ACCEPT` apart from a clear `PAIR_REFUSE` by trying to decrypt
   the frame first.
5. Over the encrypted channel, B sends `PAIR_ACCEPT` with the resulting group (below). Both
   devices store each other's static key in their `devices` rows.
   - When the group is A's (A is in a group and B isn't), B doesn't know the group. So B
     sends an empty `PAIR_ACCEPT` `{}`, and A answers with its own `PAIR_ACCEPT`
     `{"group_uid", "library_uid"}`.
   - When an existing group's member sends it to a device joining that group, it adds
     `"archive_names"`. This holds the shown names of the group's current archives (a void
     demotion included), sorted by name. It lets the joiner say which device keeps the
     originals before the group data arrives.
   - The device whose group it is writes the other's `devices` row, with `added_by` set to
     itself. For a new group, B also writes `sync_group`, its own row and the archive's
     `profiles` row (`archive = 1`, mode `all`, format `original`).
   - The joiner adopts the library uid if it needs to, sets `local.group_uid` and writes the
     other's row (name, kind, host, key). That way it can reconnect even if the session
     breaks.
   - When the new group's library is the joiner's (A, joining with the library), the archive
     row B would write is A's own. A writes it, with `archive = 1`, and counts as having
     chosen.

   The connection then becomes an ordinary session, starting from its `HELLO` (step 1 of
   [Session](#session), without the handshake). So the new member gets the whole group and
   library straight away. No files move yet, because the joiner chooses its own profile
   first (see [Choosing a profile](#choosing-a-profile)). It starts with a budget of nine
   tenths of its free space, which the editor shows prefilled.

`PAIR_*` and `JPAKE_*` messages sent in the clear use the same `<type: u8> <payload>` form
inside the frame.

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

A device that joins with an empty library adopts the group's `library_uid` through
`mc_library_adopt_uid()`. This refuses a library that has tracks.

**Two libraries are never mixed.** If a device's library has music and isn't the group's,
the device is refused with `library_mismatch`. (When neither side is in a group, it's
compared with the other device's library instead.) Two devices in different groups are
refused with `in_group`, whatever their libraries hold. Neither device is changed.

To pair anyway, either the device opens a new, empty library, or the user leaves a group.
On the refusal, the desktops offer Create a New Library, then Pair Again. The device's music
then comes into the group's library by being imported there.

Only the library of the device that joins matters. A member that has another library open
(`MC_SYNC_LIBRARY_OTHER`) can still bring a device into its group. The joining device is
judged by the group's `library_uid`, and the member itself goes on syncing only group data.

Some library folders hold leftovers of merging two libraries:

- A `library-before-merge-YYYY-MM-DD.db` at the top of the library folder is a copy of a
  library that was set aside. It's left where it is, and the orphan sweep keeps every file
  its rows name (see [Files](#files)).
- `meta` keys for a merge (`merge_source`, `merge_wait` and the like) are deleted when the
  library opens.

The device that was asked also tells its frontend the address the other device came from,
and the `port` it sent. That way Create a New Library can offer Pair Again when discovery
doesn't show the device (because multicast is blocked).

## Group membership

- `devices` rows replicate like any other group data. So a pairing made between A and B
  reaches C on C's next session with either of them.
- Any member can add a member (by pairing) or remove one. Removal sets `removed = 1`, and
  members then refuse that key.

  The removed device clears its `local.group_uid` when it sees its own row removed. It sees
  this either in a session's group data, or from the `ERROR "removed"` that a member answers
  its next session with. It then deletes the group's data from its `sync.db` without logging
  it, keeping only its own `capabilities` row, so nothing of the old group reaches a later
  one. The exception is the scrobbling account (below), which stays, along with its changes.
  Leaving doesn't sign the device out, and the account goes with it.
- Leaving is a device removing itself. It marks its own row removed, tells one reachable
  member in a session, and forgets the group.
- A device can't be removed, or leave, while it's the only archive (`MC_ERR_LOCKED`).
- **A group of one goes.** Sometimes a device has no member left in its group except itself:
  every other `devices` row is `removed`, and its own isn't. It then forgets the group the
  same way as when leaving, and is no longer in a group. It pairs like a device that has
  never paired. The archive rule doesn't apply, since nobody else remains.

  The device checks for this when it removes the last other member, after every session and
  change of group data (the scheduler's poll), and at start. The check waits while a pairing
  is under way on this device or a session is running, and pairing waits for the check.
  Pairing writes the member it adds in the same transaction as the group, so a device that
  has just paired is never alone. The frontend is told once (`on_group_dissolved`).
  - When a device removes the last other member, it tells that member first. It connects to
    it and, after the handshake, sends `ERROR {"reason": "removed"}` itself. The member
    treats this the same as an `ERROR` from a responder, and forgets the group. This is best
    effort, as with leaving. A member that can't be reached keeps the group until its user
    leaves it, since nobody answers it as a member any more.
  - A device that was removed but hasn't heard yet still sees members, so it isn't alone.
- Each device keeps its own `devices` row up to date (`name`, `kind`, `host`, key, and
  `node_uid` once its library is the group's), and its own `capabilities`. The
  `confirmed_profile` column is left unused.
- **Names.** `profiles.name` is the device's friendly name in the group. Like the rest of a
  profile, only the device itself edits it. NULL means the device's own `devices.name`.

  The device's effective name is `profiles.name`, or `devices.name` if that's NULL. At
  start, after a session and after a local edit, the device checks whether its effective
  name differs from the one it's running under. If it does, the device switches to it,
  announces it through discovery and writes it to its own `devices.name`. Nearby devices and
  pairing then see the group's name for it.

### Scrobbling account

`scrobble_account` is the group's one account with Last.fm, or with a service that has the
same API (see `core/include/kithcore/scrobbler.h`). It holds:

- the service's addresses, API key and secret (NULL means Last.fm and the build's key)
- the session key
- the user name.

Its one row has the fixed uid `00000000-0000-0000-0000-000000000000` on every device. So if
a device signed in before it joined a group, its account merges with the group's by last
writer wins, field by field.

Signing in on any member signs in the whole group. Signing out, or changing the service,
signs the group out. Each device only scrobbles its own plays, from `scrobble_queue`, which
is never synced. Each device also decides for itself whether it scrobbles at all (`scrobble`
in its settings.json or app preferences, never synced).

The session key is a long-lived credential: it lasts until the user revokes the app on
Last.fm. So it only travels the way all group data does. That means in the changelog, inside
a session's Noise-encrypted channel, to devices that are members of the group (paired,
holding a key the group knows, and not removed).

It never goes into discovery announcements, or into the pairing messages themselves. A new
device first gets it in the session that follows a pairing the user confirmed with the code.
A removed device is refused before any group data is sent. The key is stored unencrypted in
each member's `sync.db`, like the device keys.

The build's own API key and secret are never written to `sync.db` (NULL there means "the
build's"), so they don't travel either. A service with its own key
(`mc_scrobbler_set_service()`) syncs that key and secret with the account.

### Archives

- `profiles.archive = 1` fixes that device's profile at "everything, originals", with no
  budget. Profile setters refuse other values with `MC_ERR_LOCKED`. The name, budget size
  and selection stay editable.

  The getter shows an archive's profile as everything, originals, no budget. But the stored
  `mode`, `format`, `bitrate_kbps` and `budget_enabled` are left as they were (setters don't
  write them). They apply again if the device stops being an archive.
- Only a device can set its own role, either way (`mc_sync_set_archive(self, 1)`). Setting
  another device's gives `MC_ERR_LOCKED`. This is the same as for the rest of its profile
  (see [Ownership](#ownership)). Making and clearing the role only write `archive`. A device
  with no `profiles` row gets a bare one first, as for any first edit. A device that steps
  down has chosen its profile as it currently stands.
- The group never loses its only full copy. A device may only clear its own `archive` when
  another archive's `sync_status` shows it's verified complete, with:
  - `missing = corrupt = 0`
  - `verified_ms` newer than the library's latest change. That's the wall time of the newest
    hlc in this device's library changelog, so the open library must be the group's.

  Otherwise clearing fails with `MC_ERR_LOCKED`.
- After applying remote group rows, every device checks the merged state. If no device is an
  archive, the demotion with the lowest `(hlc, origin_uid)` is void. Devices treat that
  device as an archive, and it writes `archive = 1` again the next time it applies the
  check. Every device evaluates the same rows, so they all reach the same answer.
  - A *demotion* is the winning `profiles.archive` change of a current (not removed)
    member's row, when its value is 0 and an earlier change to that field (by
    `(hlc, origin_uid)`) set 1. A 0 with no 1 before it (the defaults a logged insert
    writes) isn't a demotion.
  - The check is a function of the changelog, so it isn't stored. Everything that asks
    whether a device is an archive counts the void one: the profile getter, the device list,
    the plan, reconcile, removal and leaving, and the choice of transcoders.
  - A device applies the check right after each session's group exchange. It applies it
    again before the next session's `HELLO`, in case that write failed. A write after the
    exchange leaves the two devices' group data different until their next session. So, as
    with the node uid in step 3 of [Session](#session), the session asks for another one
    soon after.
  - It converges. Until the write reaches a device, that device evaluates the same rows to
    the same void demotion. Once the write arrives, the device is an archive by its stored
    flag, and the rule no longer applies. A demotion can only be voided while no device is
    an archive, and only the device itself can clear its flag again, under the rule above.
  - Only changes that count take part (each device's own, see [Ownership](#ownership)). The
    demotion and the 1 before it are the device's own writes.
- The only archive can't be removed or leave (`MC_ERR_LOCKED`). If it's lost for good (a
  dead disk, for example), another member makes itself an archive and fetches everything.
  Once it has verified that it holds it all, it removes the lost one.

## Clocks and conflict resolution

- Every change is stamped with a **hybrid logical clock** (`core/src/util/hlc.c`): 48 bits of
  wall-clock milliseconds plus a 16-bit counter. When a device receives changes, it calls
  `mc_hlc_observe()` so its own clock never falls behind what it has seen.
- **Fields** merge independently. For each `(entity, row_uid, field)`, the change with the
  highest `(hlc, origin_uid)` wins. If two devices edit different fields of the same row,
  both keep their edit. This applies to library and group data alike.
- **Deletes** are `_deleted` changes. A delete only wins over a concurrent edit if it has the
  higher hlc. Tombstones are kept until every group member has acknowledged them.
- **Sets** (`profile_items`, `playlist_entries`, `album_group_entries`) are one row per
  member, so concurrent adds and removes on two devices both apply.
- **Playlist order:** entries carry a fractional-index `position`. Inserting between A and B
  generates a key strictly between theirs, plus a random suffix so concurrent inserts don't
  collide. Moving an entry is a `position` change on that entry. If two devices insert at
  the same spot, both keep their entry, ordered by `(position, uid)`.
- **Recents** are keyed by the opened item's uid. So the same item opened on two devices is
  one row, and its newest open wins like any field.

  `kind` is an ordinary field holding a name. So a kind an app doesn't know needs no new
  protocol or schema version. The app applies, logs and passes such a row on like any other,
  and only its listing leaves the row out. A row of kind `'station'` is one of these: it's
  kept and synced, but not listed. The app can still remove the row, with Clear Recents.
  See [Recents](schema.md#recents).
- **Artist credits** (`track_artists`, `album_artists`) are one row per position. Each row is
  keyed by a uid derived from the owner and the position (see
  [Artist credits](schema.md#artist-credits)). So two devices crediting one track write the
  same rows and merge field by field: the newest artist and join phrase at each position
  win. A shorter credit deletes the rows past its end. If the same position is written again
  later, that's a newer insert of every field, which brings the row back. `artist_credit` on
  the owner is an ordinary field.

## Session

Message types:

| Type | Name          | Payload |
|------|---------------|---------|
| 0x01 | `HELLO`       | `{"v", "v_min", "app", "feat": [name], "dev", "library_uid", "node_uid", "covers": true, "files_plan": true, "cancel": true, "archive": bool, "caps": {...}}` (`v`, `v_min`, `app`, `feat`: see [Versions](#versions); `covers`: it serves `COVER_REQ`; `files_plan`: it takes `FILES_PLAN`; `cancel`: it takes `CANCEL`; `archive`: the sender counts itself an archive, see step 6) |
| 0x02 | `BYE`         | `{}` |
| 0x03 | `ERROR`       | `{"reason", "msg"}`; with `"reason": "version"`, also the sender's `"v"`, `"v_min"` and `"app"` |
| 0x04 | `CANCEL`      | `{}`: the sender's user cancelled the session, and the sender closes; only to a peer whose `HELLO` has `"cancel": true` (see [Scheduling](#scheduling)) |
| 0x10 | `VV`          | `{"db": "group" \| "library", "vv": {"<origin>": <hlc>}, "resend": true}` (`resend` only in the re-send round) |
| 0x11 | `ROWS`        | binary, below |
| 0x12 | `ROWS_END`    | `{"db", "digest"}` |
| 0x20 | `HAVE_REQ`    | `{"tracks": [uid], "format", "kbps"}` |
| 0x21 | `HAVE_RESP`   | `{"tracks": [{"uid", "original": bool, "cached": bool, "verified": bool, "size"?, "copy"?: bool, "copy_kbps"?}]}` |
| 0x22 | `FILE_REQ`    | `{"track", "format", "kbps", "est", "retry": true}`; `format` `"original"` or `"opus"`; `est` the size the receiver expects, for progress only; `retry` only on the second ask for a track after a checksum mismatch |
| 0x23 | `FILE_HDR`    | `{"track", "file", "format", "kbps", "ext", "size", "sha256", "source_sha256"?}`; `kbps` is the transcode's bitrate, or an original's own (0 if unknown) |
| 0x24 | `FILE_DATA`   | raw bytes, in order, until `size` bytes have been sent |
| 0x25 | `FILE_ERR`    | `{"track", "reason"}`, sent instead of `FILE_HDR` |
| 0x26 | `PREPARE_REQ` | `{"device", "tracks": [uid], "kbps"}`: transcode into the cache for a device that is offline, or (`device` the receiver itself) ahead of its `FILE_REQ`s; no reply |
| 0x27 | `FILES_DONE`  | `{}`: the receiver has asked for everything it wants; the turn passes |
| 0x28 | `COVER_REQ`   | `{"album", "sha256"?}`; only to a peer whose `HELLO` has `"covers": true` |
| 0x29 | `COVER_HDR`   | `{"album", "ext", "size", "sha256"}`, then the image as `FILE_DATA`; or `FILE_ERR {"album", "reason"}` instead |
| 0x2A | `FILES_PLAN`  | `{"files", "bytes"}`: how many tracks the receiver's turn will `FILE_REQ`, and their estimated size; only when both `HELLO`s have `"files_plan": true` |
| 0x40 | `PAIR_HELLO`  | pairing only; see [Pairing](#pairing) |
| 0x41 | `PAIR_READY`  | |
| 0x42 | `PAIR_REFUSE` | |
| 0x43 | `JPAKE_1`     | `mbedtls_ecjpake_write_round_one` output |
| 0x44 | `JPAKE_2`     | `mbedtls_ecjpake_write_round_two` output |
| 0x45 | `PAIR_ACCEPT` | `{"group_uid", "library_uid", "archive": dev \| null, "archive_names"?: [name]}`, or `{}`; see [Pairing](#pairing) |
| 0x46 | —             | reserved (`MERGE_REQ`); never sent |
| 0x47 | —             | reserved (`MERGE_FILES`); never sent |

Steps:

1. Handshake (Noise KK). Both sides send `HELLO`, then read the other's. Each checks the
   other's versions before anything else in it. If the two can't sync, each refuses with
   `ERROR "version"` (see [Versions](#versions)), and nothing else is exchanged.
   - `caps` is `{"cpu_score", "free_bytes", "on_power", "can_transcode"}`, as in the
     sender's `capabilities` row (see [Power and transcoding](#power-and-transcoding)).
     `free_bytes` is left out when the device can't measure its free space (`NULL` in the
     row, never 0).
   - The `dev` in a `HELLO` must be the device the handshake authenticated.
   - Before sending its `HELLO`, each device brings its own `devices` and `capabilities` rows
     up to date. It also writes the `sync_status` that its last file phase left pending (see
     [Files](#files)).
   - Protocol 1 sent `"user": true` when the user had pressed Sync. Nothing reads it any
     more.
2. **Group data.** Both sides send `VV` for `group`, the initiator first. Then each streams
   the group changelog rows the other is missing (`ROWS`, in hlc order, read from one
   snapshot), followed by `ROWS_END`. The exchange is half-duplex, so neither side's stream
   can block the other's:
   1. the initiator streams its rows, then `ROWS_END` with a provisional digest;
   2. the responder applies them, streams its own, then `ROWS_END` with its final digest;
   3. the initiator applies those and sends a second `ROWS_END` with its final digest.
3. **Library data.** This step only runs if both `library_uid`s equal the group's (from
   `sync_group`, just synced). It's the same as step 2, with `db = "library"`. The initiator
   decides, and a responder that disagrees answers `ERROR {"reason": "library"}`.

   Before this step, each device checks its library's `node_uid`. If another member's
   `devices.node_uid` already records it, or the peer's `HELLO` carries it, one of them
   rotates it (`mc_library_rotate_node_uid()`). The one that rotates is the one that hasn't
   recorded it. If both or neither have, it's the one with the higher device uid. Each then
   records its node uid in its own `devices` row. That write comes after the group exchange,
   so the peer gets it in the next session, which follows a few seconds later.
4. Each receiver applies rows in one transaction per batch, with `applying_remote = 1` so
   the triggers don't log them again. It inserts the received rows into its own changelog
   unchanged. See [Applying rows](#applying-rows).
5. **Convergence check.** `ROWS_END.digest` is the hex SHA-256 over the sender's version
   vector, after it has applied everything it received. Origins are sorted, and each is
   written as `<origin>=<hlc>\n`. Both sides compare the responder's final digest with the
   initiator's final one.

   If they differ, the session logs it and runs step 2 (or 3) once more with
   `"resend": true`. This time each side sends every row, from hlc 0, of each origin whose
   newest hlc differs between the two new `VV`s. It doesn't retry after that.
6. **Files**, receiver-driven, when step 3 ran. See [Files](#files). Each side is the
   receiver in turn. The receiver sends `HAVE_REQ`, `FILE_REQ` and `PREPARE_REQ` while the
   other only answers, and ends its turn with `FILES_DONE`. Once it knows what it will
   fetch, it says how much in `FILES_PLAN` (see [Files](#files)).

   When both `HELLO`s carry `archive` and exactly one says `true`, that side receives first.
   Otherwise the initiator does. So an original imported on the other device reaches the
   archive in the archive's turn. The other device's turn can then fetch it in its own
   format and drop the original (see Reconcile), all in the same session. The order comes
   from the `HELLO`s, not the group data, so both sides agree on it even if they disagree
   about who is an archive.
7. `BYE`, the initiator first, and the responder answers with `BYE`. Each side then
   reconciles its library folder, so the peer can no longer ask for a file it's about to
   delete.

Either side may send `ERROR` and close at any time. The reasons used are:

- `removed` (above)
- `busy`: a session between the two is already running, or this device is holding sync back
  (see [Scheduling](#scheduling))
- `library` and `version` (see [Versions](#versions)).

`busy` says why in `"hold"`:

- `"running"`: a session between the two is under way.
- `"cancelled"`: this device's user pressed Cancel. `"until"` is when the hold ends, in Unix
  ms.
- `"bin"`: its recycle bin holds deletes, until Sync is pressed there.

The refused device keeps the reason, shown as an [issue](#issues), until a session succeeds
or `until` passes. A `busy` without `"hold"` reads as `"running"`.

A device that refuses a session with `ERROR` reads what the peer sent in the meantime before
it closes. Otherwise the TCP reset of an unread close could destroy the `ERROR` in transit.
Sessions are retried with backoff (10 s, doubling, up to 5 minutes).

### Versions

Each device has a **protocol version**, and an **oldest protocol version** it still syncs
with. Two devices sync, or pair, only when each one's version is at least the other's oldest
(`v >= the other's v_min`, both ways). The protocol version is 2, and the oldest accepted
is 2.

- **Said in** `HELLO`, `PAIR_HELLO`, `PAIR_READY` and `PAIR_REFUSE`:
  - `"v"` and `"v_min"`, which are integers
  - `"app"`, the Kithsong version (`VERSION`), e.g. `"1.0.0"`, which is only for the user to
    read.

  Discovery announces the same as `v`, `vmin` and `app`. A missing or malformed `v` counts as
  0, and so does a missing `v_min`.
- **Checked first.** A device reads the peer's versions before anything else in its message.
  That's because a version it can't sync with may send things it doesn't expect. It then
  refuses:
  - in a session, with `ERROR {"reason": "version", "msg", "v", "v_min", "app"}` (its own
    values), after the `HELLO`s and before the group exchange
  - in pairing, with `PAIR_REFUSE` (see [Pairing](#pairing)), before a code is drawn or
    anything is written.

  Both sides judge the same numbers, so they agree. Each one knows which is outdated: the
  peer if its `v` is below this device's `v_min`, otherwise this device. If a refusal arrives
  for a reason the device doesn't follow (a later rule), the one with the higher `v` counts
  as the newer.
- **Shown.** Each device keeps in memory what it last learnt of each other device's version
  (`mc_sync_version` on `mc_sync_device` and `mc_sync_nearby`, `on_version` when it
  changes). It learns this from a refused session or pairing, or from discovery. A session
  that succeeds clears it.

  The device starts no automatic session with a member marked either way, so this side
  doesn't show an outdated peer a failure every few minutes. A press of Sync still tries, and
  so does news of another version from discovery. The device page and the nearby list say
  which device to update (`mc_sync_version_text()`).

**Changing the protocol.** A change that every peer in the accepted range can live with
needs nothing here. Unknown entities, fields and item kinds pass through, as described under
[Applying rows](#applying-rows), and optional messages are gated by a capability (below).

- *Bump the protocol version* (`MC_DISCOVERY_VERSION` in `kithcore/discovery.h`) whenever a
  release changes what goes over the wire or what the group's rows mean. Later releases can
  then tell it apart from earlier ones. A bump alone breaks nothing. Peers on the old version
  still sync, as long as it isn't below the new `v_min`.
- *Raise the oldest accepted version* (`MC_DISCOVERY_MIN_VERSION`) only for a breaking
  change. That's a change an older peer would get wrong rather than ignore, such as:
  - a field whose meaning changes
  - a message it would end the session on, with no flag to gate it
  - rows it would apply wrongly.

  Raise it to the version that introduced what the old ones can't handle. Say in the
  changelog that devices on earlier versions must be updated.
- *Capabilities, for optional features.* `HELLO`'s `"feat"` lists the optional features the
  sender supports, by name. A session only uses one when both `HELLO`s name it
  (`mc_session.features`, from `mc_feature` in `core/src/sync/version.h`). If older peers
  can simply go without a new feature, add a flag there instead of raising the minimum. The
  sender checks the flag before sending the new message or field, and a peer that doesn't
  name it carries on as before. No feature needs one yet (`test` only exists for tests). The
  `HELLO` keys `covers`, `files_plan` and `cancel` work the same way, and stay as they are.

### Applying rows

- A row that's already in the changelog (same hlc, origin, entity, row uid and field) is
  skipped.
- A row naming an entity or field this version doesn't know is logged, so it's passed on,
  but it's never applied. This is what lets versions mix, within the range of protocol
  versions they accept from each other (see [Versions](#versions)).

  A newer peer's tables and columns reach an older one, which keeps them and sends them on,
  with no error either way. When a migration later makes such rows known, they're applied
  then (see [Migrations](schema.md#migrations)).
- Readers take a credit's first artist from `tracks.artist_uid`, not from its row 0. So an
  edit that only changes `artist_uid` and leaves the credit rows as they were is followed.
  The featured artists past row 0 stay until a device writes the credit again.
- A mix detected by its length (v16, see [Album flags](schema.md#album-flags)) is never a
  row. Each device works it out from the album's synced tracks, so devices can't fight over
  it. Only a choice made by hand travels.
- A `play_events` row travels as one entry, not one per field. The field is `row`, and its
  value is a JSON array of the row's columns (see
  [Change tracking](schema.md#change-tracking)), applied all at once. So a row of this table
  can't half arrive. There's nothing to merge field by field either, since nobody edits one.

  A value that can't be unpacked is logged and passed on like any row, but makes no table
  row. `_deleted` works as it does for any row: if it's newer than the entry, it wins. A
  restore logs the row again, newer still.
- Nothing reads `track_analysis` (v19), but its rows still apply and pass on like any other
  (see [Audio analysis](schema.md#audio-analysis)).
- A row is *live* if one of its field changes is newer, by `(hlc, origin_uid)`, than its
  newest `_deleted`.
  - A dead row that exists is deleted.
  - A live row that exists gets each field that the batch brought a new winner for.
  - A live row that doesn't exist yet (new, or brought back by an edit newer than its
    delete) is inserted with every field's winner. This only happens once the changelog has
    a value for each `NOT NULL` column without a default. Until then the row waits, so its
    fields can arrive in any order and across batches.
- Tables are applied parents first, in an order derived from their foreign keys, with
  `defer_foreign_keys` on. ON DELETE actions still run, unlogged, as they did on the origin.

  Sometimes a reference points to a parent that doesn't exist at commit, because it was
  deleted concurrently. It's resolved the way ON DELETE would resolve it: set to NULL, or the
  row deleted where the column is `NOT NULL`. Every device resolves it the same way.
- Duplicates are merged as rows are applied. See [Duplicates](#duplicates).

### Duplicates

Two devices can import the same album independently. Each makes its own artist, album and
track rows, with their own uids. Once their rows meet, every device merges them in the same
way, without logging anything.

- **What is one.**
  - Two artists are one when they share an `mb_artistid`.
  - Two albums are one when they share an `mb_albumid`.
  - Two tracks are one when they share an `mb_releasetrackid`. That's a track on one
    release, so the same recording on two releases stays two tracks.

  Empty ids don't count. A single is an album like any other, and merges by its release id.
  Nothing merges by artist.
- **Without an id.** Rows with no id merge by name:
  - two artists with the same name
  - two albums with the same title and artist
  - two tracks with the same title, on the same disc and number, of such an album.

  "The same" means exactly the same, once case, accents, punctuation, the spacing between
  words and a leading "The" are set aside (`mc_name_key()`). So "The Beatles" and "beatles"
  are one, but "Beatle" isn't. There's no edit distance, because a wrong merge can't be
  undone. A name that's left with nothing ("!!!") matches nothing. "Unknown Album" and
  "Unknown Artist" don't count, because all sorts of files land there. A row with an id
  never merges by name with one without. A track with no album doesn't merge by name.

  Whether a row has an id can't be read from the changelog, since an id may arrive later
  than the name. So each device logs the name itself. Before sending rows, it logs a
  `merge_name` row for each such row (a field with no column; `mc_merge_note_names()`),
  with the name the row goes by at that point. From then on it's a key like the ids, so a
  row that gains an id later stays merged.
- **Sharing a key.** Two singles albums (`albums.kind = 'singles'`) by the same artist
  don't merge. This version doesn't log `kind`. A `kind` row is logged and passed on as an
  unknown field, but never applied. So such albums are ordinary albums here.

  "Share" means any value the changelog has ever held for the field, not only the current
  winner. So a class only ever grows as rows arrive, and a later edit that changes or clears
  an id doesn't split it. The classes are therefore a function of the set of changelog rows.
  They're the same on every device, whatever order the rows came in.
- **Under which uid.** A class is one row under its smallest uid (the canonical uid). Its
  fields, and whether it's live, are worked out from the changelog rows of every uid in the
  class together, as if they were one row's. Field by field, the highest
  `(hlc, origin_uid)` wins, and a delete of any of them is the class's delete. Recents rows,
  keyed by the item's uid, merge the same way.

  An album's flags are only logged when set (see [Album flags](schema.md#album-flags)), so a
  new row's 0 never beats a flag set on its duplicate. The newest thing anyone chose wins,
  and a flag set on one copy and never touched on the other stays set.
- **References.** These are moved to the canonical uid, so each device's files stay attached
  to the surviving track:
  - `tracks.album_uid` and `album_group_entries.album_uid`
  - `albums.artist_uid`, `tracks.artist_uid`, `track_artists.artist_uid` and
    `album_artists.artist_uid`
  - `playlist_entries.track_uid`, `track_plays.track_uid`, `play_events.track_uid` and the
    device-local `files.track_uid`.

  If two `track_plays` rows would then share a `(track_uid, node_uid)`, only the one with
  the smaller uid is kept. If the whole class is deleted, the references go as ON DELETE
  takes them. An album group or playlist that held both copies keeps the entry with the
  smaller uid, so it holds the survivor once.
- **Analysis.** `track_analysis.track_uid` is moved like those, so old results follow the
  track.
- **Credits.** A merged-away track's or album's credit rows (`track_artists`,
  `album_artists`) aren't moved. They go with the loser's row (ON DELETE CASCADE), and the
  survivor keeps its own credit rows. Their uids derive from the owner's uid, so the
  survivor's rows are its own, and moving the loser's would give it two rows at each
  position.

  The survivor's `artist_uid` and `artist_credit` are worked out over the class like any
  field. So they can come from the loser's changelog while the rows past 0 are the
  survivor's. The next import or refresh that writes the credit makes them one again. A late
  credit row naming a merged-away owner is deleted by the reference check, since its owner
  is `NOT NULL`.
- **Late rows.** A row naming a merged-away uid can come from a device that hasn't merged
  yet. It's logged unchanged and applied to the canonical row. A row whose reference names a
  merged-away uid is pointed at the canonical uid.
- **Sync profiles.** `profile_items` in `sync.db` aren't rewritten, since `sync.db` holds
  what its own changelog says. An `album` or `artist` item that names a merged-away uid is
  read as naming the canonical uid (by `mc_sync_get_profile()` and the wanted set). Removing
  that album or artist from a profile deletes every stored row that resolves to it.
- **Not logged.** The merge writes nothing to the changelog. Each device works it out from
  the same rows, as it does the reference resolution above. So its tables agree with every
  other device's once their changelogs do, and the convergence digest (over the version
  vectors) isn't affected.

  If the merge were logged instead, every device that merges would add its own copy of each
  rewrite. And a rewrite stamped later than a concurrent edit (moving a track to another
  album, say) would override that edit.
- **Local state.** `library.db` keeps `aliases` (each merged-away uid and its canonical uid)
  and `merge_keys` (the ids and names ever logged per uid). Neither is logged.
  `meta.merge_seq` is the changelog `seq` read so far. Each applied batch reads on from
  there, so rows written locally are taken into account at the next batch too.
- **Imports.** An import looks up albums by `mb_albumid` first, and artists by
  `mb_artistid`. So importing an album a device already has, merged or not, adds to the
  survivor rather than making a second copy.

### Scheduling

- A device syncs with each member:
  - when discovery reports it online
  - every 5 minutes
  - 3 seconds after a local change, or after a session brought it rows its other members may
    lack
  - at once with members waiting for Opus copies, when this device may start to transcode
    (see [Power and transcoding](#power-and-transcoding)).

  There's one session per pair of devices at a time. These automatic sessions always
  exchange group and library data. A device only fetches files in them once its user has
  chosen its profile (see [Choosing a profile](#choosing-a-profile)).
- The lower `device_uid` is the preferred initiator. On the online and periodic triggers, the
  higher one waits 30 s, and only initiates if it hasn't been contacted since. So a device
  behind a firewall still syncs through its own outgoing connection. A change that only one
  side knows about is sent by that side at once.
- If both still call each other at the same time, the session initiated by the lower uid
  wins. The other side's session is aborted, or refused with `ERROR {"reason": "busy"}`.
- **Rate limit.** After a complete session with a member, in either direction, a device
  starts no automatic session with it for an hour (`MC_SYNC_AUTO_INTERVAL_MS`). A complete
  session is one that succeeded, converged and left nothing to follow up.
  - Online, periodic, change and background (`mc_sync_background()`) triggers are held until
    the hour is up, then go ahead. So local changes wait too.
  - Sessions the member starts are still served, and `mc_sync_now()` isn't held.
  - A failed or incomplete session lifts the limit, so retries and follow-ups keep their
    schedule.

  The hour is counted from `peer_contact.last_sync_ms`, which only a complete session
  writes, so it survives a restart. (An incomplete session writes the address it reached and
  `seen_ms`.) The other device keeps its own limit, and nothing about it goes over the wire.
  So where only the member can connect (because of a firewall), a press of Sync here waits
  for a session the member starts. The member won't start one while its own limit holds.
  The press lapses after its 5½ minutes, and the user presses Sync there.
- **Cancel.** The user's Cancel (`mc_sync_cancel()`) aborts the connection of the session
  with a member, whichever side started it. Cancelling all aborts the session with every
  member. Cancel also holds that member back for an hour (`MC_SYNC_CANCEL_HOLD_MS`):
  - No session with it starts here, automatic or not.
  - A session it starts is refused with `ERROR {"reason": "busy"}`, so it keeps its usual
    schedule rather than backing off.

  A press of Sync (`mc_sync_now()`) lifts the hold, but background work doesn't.

  Once both `HELLO`s are in, and if the member's has `"cancel": true`, the device first
  sends `CANCEL`. (Older versions end the session on an unknown message, and send no such
  flag.) This is best effort. It's sent at once, or after the frame that's being sent, and
  only if it fits in the socket's buffer without waiting. A send stuck on a peer that isn't
  reading is cut off after a second.

  After `CANCEL`, the device only closes its sending side. It drops whatever still arrives
  until the member closes, for up to a second. This is also cut off after a second if its
  session is still waiting on the member. A connection closed with data unread is reset, and
  on Windows a reset drops what the member hadn't read yet, including the `CANCEL`.

  The member reports the session as cancelled by its peer
  (`mc_sync_result.peer_cancelled`), not as a failure. It starts no automatic session with
  the device for 10 minutes (`MC_SYNC_PEER_CANCEL_HOLD_MS`; shown in
  `mc_sync_device.auto_after_ms`): online, periodic, change and background triggers wait. A
  press of Sync on the member still starts one and lifts that hold, and so does any session
  between the two that succeeds. Sessions the device starts are served.

  A member that sent `"cancel": true` reads any message that has started to arrive before
  each message it sends. That way it finds a `CANCEL` even while it's only sending (a file,
  say). If its send fails on the closed connection, it also reads whatever is left to read,
  looking for a `CANCEL`. Otherwise (a member without `"cancel": true`, or a cancel before
  the `HELLO`s), the member sees a broken connection, then busy.

  A file being fetched when the connection goes is abandoned and its `.part` deleted, as for
  any failed transfer (see [Files](#files)). Files that arrived whole stay. Both holds are
  only kept in memory, so restarting the app ends them.
- **Automatic sync off.** `mc_sync_set_automatic(0)` stops a device starting sessions and
  reach checks by itself. Sessions under way carry on, sessions members start are served,
  and a press of Sync still starts one. Triggers that fall due in the meantime wait, and go
  ahead once it's back on.

  Android turns it off when the app leaves the screen. The foreground service then only
  finishes what's under way, and sync stops once it's idle. So a phone only syncs while the
  app is on screen.
- **Recycle bin.** While this device's recycle bin holds deletes (see
  [Recycle bin](#recycle-bin)), it only starts a session when Sync is pressed
  (`mc_sync_now()`). Every other trigger waits, as with automatic sync off. A session a
  member starts is refused with `ERROR {"reason": "busy"}`, so the member keeps its
  schedule. The exception is when this device's user pressed Sync for that member and no
  session has carried the press yet. Reach checks carry on.
- **Addresses.** A session tries the member's discovered address, its address hints and the
  last address it was reached at, in turn. If something answers at an address but the
  handshake fails, another device or program has that address now. The connection is closed
  and the next address is tried. After three such failures in a row, the member shows
  `MC_ISSUE_WRONG_DEVICE` with the address. A refusal as busy ends the round, since the
  member was reached.
- **Network changes.** Each platform's network monitor calls `mc_sync_network_changed()`.
  This:
  - reopens discovery's sockets, and announces and browses at once
  - forgets what was known about reachability
  - works out the address hints again
  - makes members whose last attempt failed due now, with their backoff reset.

  The holds above still apply.
- **Showing it.** Most automatic attempts only find out whether a member is there. So the
  frontends show a session once the member has answered, or at once if the user asked for
  it. They only report failures for those ("Showing sync" in `sync.h`).

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
  - `name`: its friendly name. NULL means it uses its own `devices.name` (see
    [Group membership](#group-membership)).
  - `mode`: `all` or `selected`.
  - `format`: `original` or `opus`. For `opus`, `bitrate_kbps` is one of 256, 192, 160, 128,
    96 or 64. It's NULL for `original`.
  - `budget_enabled` and `budget_bytes` (NULL when unset). A profile has a format or a
    budget, not both. With `budget_enabled` the format is **auto**: the budget picks it (see
    Budget mode), and editors store it as `original` with a NULL `bitrate_kbps`. Choosing a
    format turns the budget off but keeps `budget_bytes`. A row with both a format and a
    budget, which concurrent edits can leave, is read as auto. Nothing is rewritten.
  - `archive` (see [Archives](#archives)).
  - The selection, with one `profile_items` row per item. `kind` is `album`, `artist`,
    `playlist` (manual) or `album_group` (manual), and `ref_uid` is the library uid it points
    to. The selection is kept while the mode is `all`, so it comes back if the mode goes back
    to `selected`.
  - Its exclusions: `profile_items` rows of kind `exclude_album`, `exclude_artist` or
    `exclude_track`. These record what was removed from this one device (see
    [Removing from one device](#removing-from-one-device)). `exclude_track` only comes from
    the [Recycle bin](#recycle-bin). There are also `keep_album` rows, for albums brought back
    individually while their artist stays removed. Exclusions apply in either mode, and
    editors don't show them as part of the selection.

  A device with no `profiles` row doesn't receive any files until it has one (see below).
  Pairing writes the row straight away: the archive's for a new group, and the joiner's when
  it takes its name. So this only lasts until those rows arrive. For display, the profile
  getter returns what an editor would start from: `all`, `original`, no budget and no name.
  When an edit creates a row, it only logs the fields it changes, not the defaults, so it
  can't overwrite a member's concurrent edit to a different field.
- Only the device itself edits its profile (see [Ownership](#ownership)). Editors only write
  what differs from the stored profile: the changed fields, an insert (with a new uid) for
  each added item, and a delete for each removed one. A `(kind, ref_uid)` can end up stored
  more than once, for example when two members add it at once. It still counts once, and
  removing it deletes every copy. Items of a kind a version doesn't know are kept unchanged
  and ignored.
- The **wanted set** for a device is:
  - mode `all`: every track;
  - mode `selected`: the tracks of the selected albums ∪ the tracks whose `artist_uid` is a
    selected artist ∪ the tracks of the albums whose `artist_uid` is a selected artist ∪
    the tracks of the albums in the selected album groups ∪ the entries of the selected
    playlists.

  Then, in either mode, these steps are applied in order:

  1. Take away the tracks of each excluded artist (matched the same way as a selected
     artist).
  2. Add back the tracks of each `keep_album` that step 1 took out.
  3. Take away the tracks of each excluded album.
  4. In mode `selected`, add the entries of the selected playlists again.
  5. Take away each excluded track.

  So a ticked playlist keeps its tracks on the device when their album or artist is removed,
  until it's unticked. Sync everything doesn't tick anything individually, so with it no
  playlist keeps anything. An archive has no exclusions: it wants every track.

  Selections that point to a uid the library doesn't have are skipped, and so are items of
  kinds the device doesn't know.
- A device only holds files once it has a `profiles` row (written when it joins, see
  [Group membership](#group-membership)). Until then its file phase doesn't fetch or delete
  anything. An archive holds everything as originals, whatever the rest of its row says.
- **Target format** per track: `original`, or Opus at the profile's bitrate (for auto, the
  bitrate the budget picks). If a lossy original is already smaller than the Opus estimate,
  it's sent unchanged. Lossy means MP3 or Opus. The sender applies this rule, since it has
  the file, and answers a request for Opus with the original in that case. The receiver
  applies it too, to files it already holds.
- **Budget mode** (the auto format): each track is estimated at
  `duration_ms × kbps / 8 + 4096` bytes, or `files.size_bytes` for originals. The plan walks
  Original → 256 → 192 → 160 → 128 → 96 → 64 kbps and takes the highest that fits in
  `min(budget_bytes, free space + bytes already held of the set)`. If even 64 kbps doesn't
  fit, the profile is over budget. Nothing syncs: the file phase doesn't fetch or delete
  anything, so the device keeps what it holds. The profile editor shows the shortfall from
  `mc_sync_estimate_profile()` (on each frontend's device page), so the user can raise the
  budget or select less.
  - The walk always starts at originals, whatever `format` the row holds.
  - An original is the same file everywhere, so any device that holds it knows its size. If
    no device at hand holds it, it's estimated at 1000 kbps. Only the device itself knows the
    bytes it already holds (`files` is local), so a plan worked out for another device (for
    `PREPARE_REQ`) only counts its free space.
  - The profile editor's estimate (`mc_sync_estimate_profile()`) is the same plan, worked
    out for a profile that isn't stored yet, so it agrees with what the next sync will do. It
    gives the track count, the size, the bitrate the walk picks (or over budget and the
    shortfall), and the set's size at every step of the walk so the editor can show the
    options. Free space is measured on the device running it, and taken from
    `capabilities.free_bytes` for another device. A device with no `profiles` row is
    estimated as if the candidate were stored.
- **Reconcile** runs at the end of every session on the receiving device, after `BYE`, when
  its plan isn't over budget:
  - It deletes files of tracks that are no longer wanted, are in the wrong format, or have
    been deleted. It removes their `files` rows and any album folders left empty. A folder
    counts as empty if it holds nothing but a `cover.jpg` or `cover.png`, and those go too. A
    wrong-format copy of a wanted track stays until a copy in the right format has arrived,
    so the track stays playable.
  - It sweeps the library folder for orphans (`core/src/sync/orphans.c`, which lists what
    the app keeps there). An orphan is any file that nothing explains, of any type and
    hidden ones included, once it's an hour old by the newer of its modified and created
    times. A file is explained by:
    - a `files` row;
    - the rows of a `library-before-merge-*.db` (see Join rules);
    - being a cover in a folder with such a file in or under it;
    - a name the app or the system keeps there.

    Names are compared by `mc_path_key()`, which is loose on purpose: NFC and NFD, letter
    case and separators all match on every platform. A false match keeps a file, while a
    false mismatch would lose one. Orphans go to the Recycle Bin or trash, and stay where
    they are if it won't take them. On Android the library is the app's private folder and
    there's no trash, so they're deleted. Folders left empty are removed too, but never the
    library folder itself. The config folder keeps `orphans-removed.log`, with a line for
    each file removed, and `orphans-found.txt`, with what the latest sweep found. That's
    written whether the sweep removed them, only reported them or refused.
  - A sweep also runs after an import that went all the way through: every item settled, at
    least one imported, none failed, and not cancelled. It doesn't run:
    - while an item is waiting in review, for its other discs or for MusicBrainz;
    - if a source overlaps the library folder;
    - while sync isn't running on the library;
    - on a device whose user hasn't chosen its profile.
  - It only runs after a session that ended with BYE, ran this device's receive turn and had
    no checksum failures. It refuses, and logs it:
    - while anyone else is working in the folder. Imports, edits, deletes, library moves and
      other sessions hold it (see `library/folder_gate.h`), and one that starts meanwhile
      stops the sweep before the next removal;
    - if an edit's swap is unfinished;
    - if the folder can't be walked completely;
    - if the library knows no files, or none of them are there;
    - if orphans outnumber known files (`MC_ORPHANS_MAX_PER_KNOWN`), since an existing install
      may point at a folder of music that was never imported.

    A peer's next session is safe alongside it: a fetch needs `files_busy`, which the
    sweep's session still holds, and it only serves files that have rows.
  - Deleted tracks: `files` rows go with their track (`ON DELETE CASCADE`) when the library
    exchange applies the delete. So the session notes the rows it deletes and removes their
    files. Originals imported on this device go to its Recycle Bin or trash (or are removed
    if there's none), with a restore record like a delete made here, noted before the rows
    go. Received files are removed. If a file's row went some other way (a crash, an edit
    outside a session), the file is an orphan. Before looking for orphans, the session
    restores deleted tracks whose files the user took back out of the trash
    (`mc_delete_restore()`, `core/include/kithcore/delete.h`), so those aren't swept away.
  - Stale copies: when an import on some device replaces a track's original with a better
    file (see "Duplicates" in `core/include/kithcore/tagger.h`), the track gets a new
    `tracks.file_rev`, which syncs like any other field. Every `files` row records the
    `file_rev` it was made from. A received original or transcode whose `file_rev` differs
    from its track's is stale. It doesn't count as holding the track, so the new file is
    fetched. Reconcile then deletes the stale copy, just as it would one in the wrong format.
    An archive deletes it too, once the current original has arrived. If the stale copy has
    the file's name, the new copy lands as `<name> (2).<ext>`, and takes the name once the
    stale one is gone.

    Senders only offer originals and transcodes whose `file_rev` is current. The swapped file
    gets a new file uid, so no transcode cached for the old one is sent. An original imported
    on a device is never stale there: another device's better file doesn't replace it. An
    archive is the exception, because it's there to hold the best of everything. It fetches
    the current file anyway, and once that's arrived its own goes to the Recycle Bin or
    trash. It counts as holding the track meanwhile, so nothing waits on it.
  - A hand edit isn't a new file. It rewrites the tags of this device's originals and updates
    their `files` rows, `sha256` included, without a new `file_rev`. So nothing is fetched,
    and peers keep their copies with the old tags (see "Hand edits" in tagging-rules.md).
  - Exceptions: originals imported on the device are kept until an archive holds verified
    copies. The device learns this in a session with an archive. Its `HAVE_REQ` there also
    lists the tracks of such originals that it no longer wants in their current form, and the
    archive's `HAVE_RESP` says `original` and `verified`. Verified means the archive's copy
    has a recorded `sha256`: it arrived with a matching checksum, passed a deep check, or was
    hashed whole to be sent and hasn't changed since its row was written. If a wanted track's
    Opus is missing while such an original is here, the archive transcodes it whenever it may
    transcode, rather than it being split across transcoders as in [Files](#files), because
    the others may not hold the original yet. Since the archive receives first (step 6 of
    [Session](#session)), the original's swap then takes one session. On an archive, only
    deleted tracks' files are removed, and orphans are reported, not deleted.
  - All other deletes are permanent, and every delete is only of files inside the library
    folder. A path that's absolute, has a `.` or `..` part or a colon, or passes through a
    symlink or junction is left alone (its `files` row still goes). The sweep doesn't follow
    links or touch them. Library rows (tracks, albums, their art) are group data and stay: a
    device only drops its own `files` rows, and frontends hide tracks without a file.
- Every device receives the whole library database. Frontends only list tracks with a local
  file (`available_only`), except the profile picker, which lists everything.

### Ownership

Each device owns its profile: its `profiles` row and its `profile_items`. Other devices show
it read-only, and say where to change it.

- The setters (`mc_sync_set_profile()`, `mc_sync_select_items()`, `mc_sync_exclude_items()`,
  `mc_sync_set_archive()`) refuse to change another device's profile with `MC_ERR_LOCKED`.
- In the group changelog, a change to `profiles` only counts if its `origin_uid` is the row's
  `device_uid`. A change to `profile_items` only counts if it comes from the device the item
  belongs to: that device logged the item's `device_uid` field, naming itself, and the change
  has the same origin. A change that doesn't count is logged and passed on like any other,
  so version vectors stay complete and it isn't asked for again. But it never applies, and
  never wins over the owner's change.
- Every device evaluates the same rule on the same rows, so they all agree on each profile.
- Any member can still remove a device, because that's the `devices` row, not the profile.
- At start, a device re-logs these as its own:
  - the fields of its profile whose newest change came from another device;
  - items another device added to it;
  - deletes of its items that another device made.

  So it goes on holding what it held.

### Choosing a profile

A device only fetches and deletes files once its user has chosen its profile. That way,
pairing never moves files before the user has looked at what the device will keep.

- The choice is local (`local.profile_chosen`, never synced). An archive counts as having
  chosen. A device that joins a group starts unchosen, because joining clears it.
- When it joins, if the device has no profile, it prefills its own. The budget is on, at
  nine tenths of its free space, rounded down to a whole GiB and at least 1 GiB, and the
  format is auto. Its editor opens on that (each frontend's pairing flow offers it), and the
  user can edit it or keep it.
- **Pressing Sync** (`mc_sync_now()`) chooses the profile as it currently stands. Stepping
  down from archive does too.
- In its receiving turn (step 6 of [Session](#session)), a device that hasn't chosen doesn't
  fetch anything, doesn't send `PREPARE_REQ`, and skips reconcile and its `sync_status`. It
  still serves the peer. Files of tracks deleted from the library still go with their rows.
- Other devices can't see the choice, but they can see what follows from it: a device's first
  `sync_status` comes from its first file phase.

### What the next sync would copy

After each session, and whenever the group or library changes, a device works out a cheap
estimate of the next sync, along with the tracks [waiting](#waiting-for-a-device)
(`mc_sync_get_outlook()`, reported through `on_outlook`):

- **Receive**: the tracks its plan wants that it doesn't hold as wanted and that some member
  holds, with their estimated size. There are none until its user has chosen.
- **Send**: for each other member that has reported its files (`sync_status`), the tracks it
  says are missing, up to as many as its plan wants, and a size in proportion to its plan's.

`mc_sync_outlook_text()` words it as "↑ 10 GB · ↓ 100 tracks", or "Up to date".

### Removing from one device

"Remove from this PC" (computer, phone) on an album's or artist's menu takes it off this
device. It stays in the library and on every other device (`mc_sync_remove_here()`,
[kithcore/sync_here.h](../core/include/kithcore/sync_here.h)). It's only offered on a device
that's in a group and isn't an archive. An archive keeps the whole library, and without a
group the music isn't anywhere else.

- **The profile edit**: an exclusion row (`exclude_album` or `exclude_artist`, with `ref_uid`
  the album or artist). It's inserted like any other item, so it syncs and concurrent edits
  merge. In a selection, the album's or artist's own row is deleted as well. A profile can
  only include, so "Sync everything but this" needs the exclusion. Turning `all` into a long
  selection would also miss anything imported later. A ticked playlist keeps its tracks (see
  the wanted set): their files stay, and the question asked before the removal says how
  many.
- **Editors** show exclusions separately from the items. `mc_sync_get_profile()` lists them
  as `removed` (including the albums of a removed artist, but not those brought back on
  their own). `mc_sync_set_profile()` keeps stored rows of those kinds, so the editor keeps
  them through a read-modify-write. Adding an album or artist to a selection deletes its
  exclusion: what the user just chose to sync is synced. The editors mark such rows as
  removed.
- **Sync everything in an editor** shows every album and artist ticked, except what's in
  `removed`. Unticking one adds its exclusion, and ticking it removes the exclusion
  (`mc_sync_exclude_items()`). So the profile stays in mode `all`, and later imports still
  sync. The files go at the next sync, like any edit, not straight away. Playlists and album
  groups aren't listed under it: they wouldn't select anything more, and there's no
  exclusion for them.
- **Select all and Select none** (`mc_sync_select_items()`) act on one category's shown rows
  in a single write. Select all isn't a choice of each item, so it leaves out what's in
  `removed` and keeps the exclusions. Only ticking such a row by hand brings it back. Select
  none keeps them too. Neither changes the mode: Select all is a selection of what's there
  today, while Sync everything also includes later imports.
- **Bringing it back**: "Sync to this PC" on its menu (`mc_sync_restore_here()`) deletes the
  exclusion and, in a selection, adds the album or artist. The files arrive at the next sync.
  An album removed only through its artist can come back by itself, from its own menu or a
  tick. That adds a `keep_album` row, and the artist stays removed. The row goes when the
  album is removed again or that artist's exclusion changes.
- **Adding what a selection lacks**: an album or artist that a selection doesn't include
  offers "Add to this PC", which is the same call and adds its row. An artist counts as
  included if one of their albums is. A playlist holding some of an album's tracks doesn't
  make the album included.
- **The files go straight away**, not at the next session. The item's tracks' `files` rows
  here, and their files, are removed under the same one-receiver-at-a-time lock as the file
  phase. Files that sync brought are removed. Originals imported on this device only go to
  the Recycle Bin or trash if another archive's `sync_status` shows it verified complete
  after the library's latest change. That's the same test `mc_sync_set_archive()` uses to let
  an archive step down. Otherwise they stay, and reconcile removes them after a session with
  an archive, as it does for any original it no longer wants. A file that can't be removed
  (because it's in use) keeps its row and goes in a later reconcile. Album folders left with
  only a cover are removed, as in reconcile.
- Tracks can't be removed individually yet. But when a track's delete is taken back out of
  the recycle bin (below), it leaves an `exclude_track` row, which takes that track out of
  the wanted set like the other exclusions.

### Recycle bin

A device with no trash (Android) deletes a track's files outright, to free the space. In a
group, on a device that isn't an archive (`mc_sync_bin_offered()`), such a delete can still be
taken back until the next sync. `mc_delete_run_to_bin()` logs it like any other delete, and
also keeps it in the library's `recycle_bin` table (see [Recycle bin](schema.md#recycle-bin)
in schema.md): the changelog rows it logged, and the rows it removed.

- **Taking it back** (`mc_sync_bin_restore()`, kithcore/sync_here.h) relies on no other
  device having heard of the delete. It deletes the delete's changelog rows and puts the rows
  back without logging them. To every device it's as if nothing was deleted, and the version
  vectors never carried it. The device's own profile then excludes what it deleted:
  `exclude_album` for an album and `exclude_track` for a track, and in a selection the
  album's own row goes too. So the music counts as
  [removed from this device](#removing-from-one-device). Its files stay gone here (except
  what a ticked playlist has, as with any removal). It stays in the library and on the other
  devices, and can be synced here again with Sync to this device. The library is written
  first, then the profile. If the profile write fails, the music syncs back to the device at
  the next session rather than being lost. If an entry's rows need a row that a later
  entry's delete removed (a track's album, deleted with the album's last track), that entry
  is brought back too.
- **Sending it.** Before each library exchange (step 3 of [Session](#session)), each side
  empties its bin in a separate write. It does this again once the exchange has ended,
  however it ended. From then on a delete can't be taken back, since the peer may have it. A
  session cut off halfway just leaves it to go with the next one. A restore is refused while
  an exchange is under way, and an exchange waits for a restore that's under way. A library
  exchange only runs with anything in the bin in a session this device's user asked for
  ([Scheduling](#scheduling)). Otherwise an initiator leaves the library out (and the
  session doesn't count as complete), and a responder answers `ERROR {"reason": "busy"}`. A
  delete made while an exchange runs may go with it, and is then sealed with the rest.
  Nothing goes over the wire for the bin.
- **Frontends** show the bin's entries (`mc_delete_bin_list()`) with Restore. On the sync
  screen they say the user must press Sync, which makes the deletes permanent. On Android,
  while the bin holds anything and a member is online, there's a red dot on the main menu and
  on its Sync item.

## Files

1. The receiver works out what it's missing: wanted tracks without a `files` row in the
   target format.
2. It sends `HAVE_REQ` to the members it can reach and collects `HAVE_RESP`. A session is
   between two devices, so in practice it asks the session's peer, in batches of up to 400
   tracks (`format`/`kbps` are its profile's; a sender answers at most 600). Each member is
   asked in its own session, and whatever one can't give is asked of the next.
   - `original`: the sender has an original of the track (imported there or received as an
     original), and the file is there.
   - `cached`: it can send the requested Opus without encoding. That means from its
     transcode cache, a transcode it received at that bitrate, or a lossy original small
     enough to stand in. A transcode at another bitrate doesn't count, because it's never
     encoded again (see [Lossy copies](#lossy-copies)).
   - `verified`: its original has a recorded `sha256` (see Reconcile above).
   - `size`: the size of its original, only when that's the file it would send (the target
     is `original`, or it's a lossy original standing in for the Opus).
3. It chooses a source for each track:
   1. a device with a cached transcode, or with the original when the target is `original`;
   2. otherwise, a device holding the original with `can_transcode` and enough free space
      (256 MiB), ranked by `cpu_score`. The work is split across candidates in proportion
      to their score. `on_power` doesn't count here (see
      [Power and transcoding](#power-and-transcoding)).

   A receiver never fetches an original to encode it itself. A sender that may not transcode
   right now answers `FILE_ERR {"reason": "no_transcode"}`, and the receiver counts that as
   missing, not as failed or corrupt. For the split, the candidates are the session's peer
   and the other online members that may transcode and hold every original (an archive, or
   `all`/`original` without a budget). Each track goes to one of them by a hash of its uid,
   weighted by `cpu_score`, so it's the same every time. A track that falls to another member
   waits for the session with that member.
4. Before its first `FILE_REQ`, the receiver sends `FILES_PLAN`, with `files`, the tracks it
   has chosen to fetch, and `bytes`, their size. The size is `HAVE_RESP`'s `size` where
   there is one, and the plan's estimate otherwise. It's only used for progress. Both sides
   then show the turn as `files` requests to answer (a retry of a mismatch is the same
   track), and the sender doesn't check anything against it. It's only sent, even with
   `files` 0, when both `HELLO`s carry `"files_plan": true`. Older versions end the session
   on an unknown message, and don't send one themselves, so their peer shows the turn without
   a total, as before. The second turn's count is only known when it starts, since the first
   turn can change what it fetches.
5. `FILE_REQ` → `FILE_HDR` + `FILE_DATA` (or `FILE_ERR`, reasons `missing`, `no_transcode`,
   `io`, `format`).
   - Encode-ahead. Before its first `FILE_REQ`, the receiver sends `PREPARE_REQ` with its
     own uid as `device`. It lists the Opus tracks it's about to fetch that weren't `cached`
     and fall to the peer, in the order it will ask for them (up to 1000 per message, with a
     new message where `kbps` changes). The sender queues their encodes in that order, behind
     any `FILE_REQ` waiting for one and ahead of `PREPARE_REQ`s for offline members. That way
     it encodes while it sends, and is usually ahead of the requests. A `FILE_REQ` for a
     track whose encode is queued moves it to the front. A `FILE_REQ` for a track that's
     being encoded waits for that encode, so no file is encoded twice at once. Whatever is
     still queued when the turn ends is dropped, unless a `PREPARE_REQ` for an offline
     member wants it too. A sender that may not transcode ignores it, like any
     `PREPARE_REQ`.
   - The sender computes `sha256` over the exact bytes it sends. It uses the recorded hash
     if the file's size and mtime still match its row, or for a cached transcode, its
     sidecar's hash if they match the sidecar (see [Lossy copies](#lossy-copies)). A hash it
     had to compute is kept: in the sidecar for a cached transcode, and in `files.sha256` for
     an original that doesn't have one (one imported here), if the file still has its row's
     size and mtime. That's the same condition under which a deep check records it (see
     [Verification](#verification)).

     The hash is still worked out before `FILE_HDR`, which carries it. But for an original
     with no recorded hash, the sender usually has it already. It hashes ahead on a separate
     thread: the originals that the `HAVE_REQ`s before the requests asked about, in their
     order (which is the order they'll be requested), a few past the one it's sending. A
     hash worked out ahead is kept the same way, even for a file that isn't requested after
     all.

     `file` is the original's file uid: its own, or for a transcode, the original it was
     made from. `format` is `"original"` when a lossy original stands in for the requested
     Opus. `ext` is the file's extension, and a receiver refuses one that isn't an audio
     extension that import accepts. With an original (`format` `"original"`),
     `source_sha256` is its `files.source_sha256` when the sender has one. That's the hash of
     the bytes it was imported from, before the import tagged it (see "Duplicates" in
     tagger.h).
   - Requests are pipelined. The receiver keeps up to 3 outstanding (`FILE_REQ`s, and the
     `COVER_REQ`s below), and sends the next before it receives the current file, so the
     sender goes from one file to the next without waiting a round trip. The sender answers
     one request at a time, in the order they came. Each answer belongs to the oldest request
     still open. Its `track` (`album` for a cover) must match that request's, in `FILE_ERR`
     too, or the receiver ends the session. Requests are small, so sending them never waits
     on the sender's data.
   - The receiver waits up to 10 minutes for each `FILE_HDR`, counted from the end of the
     answer before it, since the sender may be encoding.
   - The receiver writes to `<dest>.part` while hashing. On a match it renames the file into
     place and inserts a `files` row with `sha256` and `source_peer_uid` (the sender), plus
     `source_file_uid` (`FILE_HDR.file`) for a transcode. A received original keeps
     `source_file_uid` NULL: it's still an original, and is served as one in turn. A row with
     `source_peer_uid` NULL too is an original imported on this device. The row also keeps
     `source_sha256`, so if that source file is imported on this device too, it's recognised
     as a file the library already has. Only the file being received has a part file, so a
     session that fails with requests outstanding doesn't leave any behind.
   - If the track's album has an `art_hash` and its folder doesn't have a cover yet (or a
     `COVER_REQ` outstanding for it), the receiver sends `COVER_REQ` for it straight away, in
     line with its outstanding `FILE_REQ`s. That way the album shows with its art while the
     rest of it is still coming. Pictures embedded in received files are never used.
   - Covers. After its fetches, the receiver sends `COVER_REQ` for each of its folders that
     holds files of an album with an `art_hash` and has no cover image
     (`mc_coverart_find()`). This also covers folders whose files were already here.

     The sender answers with its cover: the image file in a folder holding the album's files
     (`cover`, `folder` or `front`, `.jpg`/`.jpeg`/`.png`, any case), looking in originals'
     folders first. It never uses a picture embedded in a file, and the limit is 32 MiB. It
     answers `FILE_ERR` `missing` when it has none. The receiver checks `sha256` and saves
     the image as `cover.jpg` or `cover.png`, going by its magic bytes. If the peer's `HELLO`
     lacks `"covers"`, the device skips this, because a peer that doesn't serve `COVER_REQ`
     would end the session on an unknown message. A folder that already has a cover keeps
     it, even if the album's `art_hash` has changed since, unless the user chose the album's
     cover.
   - Chosen covers. `albums.art_chosen` is a logged field: the SHA-256 of an image the user
     chose (see "A cover the user chose" in tagging-rules.md). The cover a device shows must
     be that image. So fetch_covers hashes the cover of each folder of such an album (only
     those, since a library's other covers aren't read). Where it differs, it sends
     `COVER_REQ` with `sha256` set to it. A peer answers with a cover with that hash from any
     folder of the album, or `FILE_ERR` `missing`.
     - The image replaces the folder's cover. A `cover.*` with a different name goes too, or
       it could keep being found first.
     - A folder with no cover asks without `sha256`, like any other, since an older image is
       better than none until the chosen one arrives.
     - An image that isn't what `sha256` asked for is dropped, since a peer that ignores the
       field sends the cover it has.
     - A device that can't get it yet, because no peer in the session has it, keeps what it
       has and asks again next session.
     - If two devices choose different covers, the later `art_chosen` wins on both, like any
       field.
     - A later import elsewhere that writes `art_hash` before hearing of the choice doesn't
       change this. `art_chosen` says which image is meant, and `art_hash` is then out of
       step until the next choice. Only playlist pictures' cache keys would notice.
   - On a mismatch, the receiver deletes the part file and retries once from another source,
     then counts the track as corrupt. Within a session the only source is the peer, so the
     retry asks the peer again, behind the requests already outstanding. The track is tried
     again in the next session, possibly with another member.
6. The receiver chooses the path itself, with the same organising rules as imports, so paths
   are never taken from the peer: `<album artist>/<album> (<year>)/<track> <title>.<ext>`
   (singles too, since a single is an album), or `<artist>/<title>.<ext>` for a track with no
   album. If the path is taken on disk or in `files`, it becomes `<name> (2).<ext>` and so
   on. A "&lt;artist&gt; Singles" album is an album like any other, filed under that title.
7. After the file phase (and reconcile and the fast check), the receiver works out its
   `sync_status` (`expected`, `ok`, `missing`, `corrupt`, `bytes`, `verified_ms`). That row
   is group data, so every device can show "1,203 / 1,203 verified".
   - `corrupt` counts the tracks that failed twice in the session.
   - `missing` counts the other wanted tracks without a file in the target format.
   - `bytes` counts every file the device holds.

   Writing the row after the group exchange would leave the two devices' group data
   different until their next session. So the row is kept pending and written before the
   next session's `HELLO` (which follows a few seconds later, as for the node uid in step 3),
   and only when there's no row yet or a count changed. `mc_sync_verify()` writes it straight
   away, and also refreshes `verified_ms` when it's older than the library's latest change.

One receiver at a time: a device that's fetching in one session skips its receiving turn in
another session running at the same time (it still serves), and `mc_sync_verify()` waits for
it.

When a profile changes for a device that's offline, the best online holder of the originals is
sent `PREPARE_REQ` and fills its transcode cache. The target's next sync is then
transfer-only. Any member, in its receiving turn, works out each offline member's plan from its
profile and the free space in its `capabilities`. It sends `PREPARE_REQ`, in batches of up to
1000 tracks, if the session's peer is the best online holder. That means the peer may
transcode, holds every original, and has the highest `cpu_score` of those (the lower uid wins
a tie). It sends once per change in the Opus tracks that member wants, as far as this device
remembers. That's kept in memory, so after a restart it sends again, and cached files make
that cheap. The holder encodes in the background (on the pool, after any encode-ahead, see
[Lossy copies](#lossy-copies)), so the session carries on, and it keeps going after the
session ends. A holder that may not transcode ignores it.

### Showing speed and time left

Nothing goes over the wire for this. Each device works these out for itself from the bytes it
has moved (`speed` and `eta_seconds` in `mc_sync_progress`, worded by `mc_sync_pace_text()`,
in `core/src/sync/pace.c`), so every frontend shows the same thing. A session reports them
while a file's bytes are moving, and about twice a second while it waits for the peer or for
an encode, so a stall shows too.

Time is divided into one-second bins, starting with the first bytes.

- Speed is the mean of the last 10 bins, shown after 3, rounded to 0.1 MB/s (to a whole MB/s
  from 100 up; a MB is 1024 × 1024 bytes, as in every size the app shows). It changes at most
  every 2 s, and only by a tenth of itself or 0.15 MB/s. Below 0.05 MB/s nothing is shown.
- Time left is the bytes left over a longer mean, where each bin counts for less as it ages,
  with a horizon of 45 s. Stalls stay in that mean, since stalls that keep happening do take
  time. If the speed stays 30% away from the mean, in the same direction, for 20 s, the mean
  starts again from it. Time left is shown after 8 bins, on a grid that gets coarser: a minute
  up to 20 min, then 5, 10, 30 and 60 min from 1, 3 and 10 h. Under 50 s it's
  "less than a minute".
- It goes down a quarter step past where rounding would take it, at most every 5 s. It only
  goes up if the estimate stays 30% and a step above the value shown for 15 s.
- Nothing is shown when the size isn't known (a peer without `FILES_PLAN`), when nothing has
  moved for the 10 bins of the speed, or past 7 days. After a stall, the value held carries
  on. A session with another peer starts from nothing, because a different link tells you
  nothing about this one.
- The bytes left are those of the turns known so far. An original's size comes from
  `HAVE_RESP`, but for a track that's going to be encoded the plan only knows its length, and
  those estimates can be a third off. So what's left of a turn is scaled by how the files so
  far came out against their estimates (`est` in `FILE_REQ`, which the sender needs for
  this). When more work arrives (the second turn's `FILES_PLAN`), the time left starts over,
  with its warm-up.

## Lossy copies

- A transcode is only ever made from the track's original, whatever its format. A lossy
  original (an MP3, say) is still the original, so it's sent unchanged when it's small enough
  (see Target format), and encoded once otherwise. A transcode is never encoded again. A
  device holding only a transcode only sends it unchanged, to a device that wants that exact
  bitrate, and answers any other with `missing` (in `HAVE_RESP`, neither `original` nor
  `cached`). A device that wants the track at another bitrate doesn't fetch anything for it.
  The track stays missing in its `sync_status`, which isn't an error, until a session brings
  it from a device that has a copy at that bitrate, or holds the original and may transcode.
  Meanwhile, a copy it already holds in another format stays, as in Reconcile.
- Transcodes are Opus (libopusenc), cached on the transcoding device in
  `<config>/transcode-cache/<file_uid>/opus-<kbps>.opus`, with an LRU size cap of 8 GiB.
  Each encode writes its own part file and renames it into place, so two encodes of the same
  file can't corrupt it.
- Encodes run on a pool shared by every session, for `FILE_REQ`s and `PREPARE_REQ`s alike.
  It runs as many encodes at once as the device has logical cores, less one (at least one),
  on battery or on power. A device only transcodes on battery when its user allowed it, and
  then it should get done soon. While the device may not transcode, the pool takes no jobs
  (see [Power and transcoding](#power-and-transcoding)). Once the pool has encoded something,
  the cache is trimmed when no encode is queued or running and no cached file is being sent.
  That way the trim never deletes a file that's being written or about to be sent.
- Next to each cached file is its sidecar, `opus-<kbps>.sha256`. It holds one line,
  `<sha256> <size> <mtime_ms>`: the file's hash, and the size and mtime it had then. The hash
  is taken from the bytes as the encoder writes them. The sidecar is written atomically,
  through a `.part` file, once the file is in place, so a cached transcode is read once to be
  sent rather than twice. A sidecar whose size or mtime differ from the file's is stale and
  ignored: the sender hashes the file and writes the sidecar again. A cache hit that marks
  the file as recently used carries its sidecar's mtime along. The trim counts a file and its
  sidecar as one entry (the sidecar's few bytes aren't counted), deletes them together, and
  deletes any sidecar whose file is gone.
- Encoder settings: constrained VBR (libopus's default once VBR is on), signal music, and
  complexity by bitrate: 10 at 64 and 96 kbps, 8 at 128 and 160, 7 at 192 and 256. At the
  higher bitrates the spare bits leave nothing audible for the extra analysis to gain, so it
  only costs time. The CPU benchmark (`cpu_score`) always encodes at 128 kbps and complexity
  10, since it only ranks devices. It runs once and is kept, so it's timed on the thread's
  CPU clock rather than the wall clock, and time spent waiting for a core doesn't lower it.
  Other work sharing the core's caches and clock still does, by about a third with every core
  taken twice over.
- The receiver stores the copy as a `files` row with `source_file_uid` and `source_peer_uid`
  set. It's linked to the same `track_uid`, so it's never imported as a new track and never
  offered back to anyone as an original.
- Tag edits made on the phone are metadata changes and sync back normally. The lossy file's
  embedded tags are rewritten locally and aren't authoritative.

### Power and transcoding

Whether a device transcodes for others is up to its platform, which reports it with
`mc_sync_set_power(on_power, may_transcode)`. Until it does, a desktop counts as `1, 1` and a
phone as `0, 0`. The device writes both into its `capabilities` row (`on_power`,
`can_transcode`) before each `HELLO`, and straight away when they change, from the
scheduler's thread. A write after a session's group exchange reaches the peer in the next
session, like any group row. The receiver always reads the peer's row after that session's
group exchange, so within a session it chooses its sources from what the peer said at the
start of the session.

- **Android** reports `on_power` while the phone is plugged in. It reports `may_transcode`
  then too, and also:
  - while a page of the app's sync section is on screen with the app resumed (the Sync page,
    a device's page, the recycle bin opened from them, and their dialogs);
  - after Background was pressed on those pages.

  Leaving the section, leaving the app or turning the screen off withdraws it straight away,
  unless the phone is plugged in or Background was pressed. While the phone is transcoding on
  battery, those pages tell the user to stay on them or plug in a charger, and to keep the
  screen on, and they offer Background. A foreground service then keeps the session and its
  transcoding going off screen, with a notification to cancel it, until the session and its
  encodes are over. When the phone is plugged in, a session under way carries on in the same
  way after the user leaves the app.
- **Turning it on** makes some members due for a session straight away
  (`MC_TRIGGER_COPIES`): those that want Opus (`profiles.format = 'opus'` or a budget, not an
  archive) and whose `sync_status.missing` is above 0. The rate limit
  (`MC_SYNC_AUTO_INTERVAL_MS`) doesn't hold it back. Automatic sync being off, a cancel hold
  and the recycle bin do, as for any automatic trigger.
- **Withdrawing it** takes effect straight away, even mid-session:
  - The encode pool drops what's queued and takes no more.
  - A `FILE_REQ` waiting for an encode, queued or running, is answered
    `FILE_ERR {"reason": "no_transcode"}`, and so is every later one that would need an
    encode. Copies already in the cache, or received at that bitrate, still go.
  - An encode that's running finishes into the cache (libopusenc can't be cut short), and is
    sent from there in a later session.
  - A file that's being sent is sent to the end.

  The receiver counts the refused tracks as missing, so its `sync_status` says missing, not
  corrupt, and it fetches them from whichever device may transcode next.
- `mc_sync_transcoding()` (and `on_transcoding`) say whether the device is transcoding for
  others: a session it serves has asked the pool for an encode in its turn, or encodes are
  queued or running.

Older versions require `on_power` too, so they never ask a phone that transcodes on battery
for anything.

## Waiting for a device

A device shows which of its wanted tracks can only come from members it can't reach right
now: "12 tracks are waiting for Tania's desktop to come online.", or with several members
"... for Tania's desktop and 2 other devices ...", naming the one holding the most. It's a
calm notice at the top of the library on every platform, with no button, and it goes away once
the tracks arrive. `mc_sync_get_waiting()` gives it (total tracks and their uids, then each
member with the tracks it holds, most first), `mc_sync_waiting_text()` gives the sentence, and
`on_waiting` says when it changed. The rules are in `core/src/sync/waiting.h`:

- **Waiting tracks:** this device's plan, less the tracks it holds in the target format
  (counted the way `sync_status.missing` counts them), that some member could give it and no
  reachable member can. Nothing is worked out while the device isn't fetching anything:
  outside a group, with another library, with a profile its user hasn't chosen yet, or with
  an over-budget one.
- **Who holds a track.** `files` is local, so it's worked out from what the group records:
  - Each other current member's plan, from its profile: as an original, or as Opus at exactly
    this device's bitrate (a copy at another bitrate isn't a source, see
    [Lossy copies](#lossy-copies)). A member that has never reported its files
    (`sync_status`, after its first file phase) doesn't hold anything yet. For a track this
    device wants as Opus, an original only counts as a source on a member that may convert
    for others (`capabilities.can_transcode`, with `MC_SYNC_TRANSCODE_MIN_FREE` free or its
    free space unknown; a member with no row counts as able). A track whose holders can't
    convert it isn't waiting and isn't counted in the outlook: it's `MC_ISSUE_NO_TRANSCODER`.
  - The member that imported the track's current original. That's the origin of the newest
    `tracks.file_rev` change in the library changelog, or failing that, of the track's first
    change, mapped through `devices.node_uid`. It keeps that original until an archive holds
    a verified copy (Reconcile), and then the archive is a holder.

  Neither counts for a track that the member said it doesn't have (neither `original` nor
  `cached`) in `HAVE_RESP`, in its last session with this device. That's kept in
  `peer_lacks`, local to this device's `sync.db` and replaced each session. So a member
  that's behind on its own profile isn't named for what it hasn't fetched yet. A track no
  member is known to hold doesn't count: it isn't waiting for anyone.
- **Reachable** depends on how the last real session attempt with the member went, kept while
  the `mc_sync` exists (across stop and start): a session that completed, or one running now.
  A member is unreachable if the session never got past connecting (`connected` 0 in its
  result: no answer, or a failed handshake), or if a connection failed. A session that
  answered and then failed changes nothing until its retry. Discovery and reach checks don't
  count, so a member coming and going on the network, or a background check, never makes the
  notice flicker. A member that hasn't been tried yet counts as reachable:
  - while automatic sync is off (`mc_sync_set_automatic(0)`, as on a phone with the app off
    screen: nobody is tried, so nothing is learnt);
  - for the first minute after sync starts or automatic sync comes back on (the sessions it
    starts go first);
  - after that, if its last complete session (`peer_contact.last_sync_ms`) was within
    `MC_SYNC_AUTO_INTERVAL_MS`.
- **When.** Each member's plan is one read of every track, so the scheduler only works it out
  again when something it depends on has changed:
  - a session ended (files arrived or went, profiles or members changed);
  - a member that was reachable failed to connect;
  - the group's data changed here (a profile edit, a choice, a removal);
  - a check deleted files;
  - the first minute after start is over.

  `on_waiting` only fires when the answer is different.

## Issues

Sync never fails or stalls silently. Anything that stops it doing what it should is an
**issue**, with what happened and how to fix it, in one list (`mc_sync_problems.issues`, from
`mc_sync_get_problems()`; `on_problems` says when it changed). Each platform shows them in one
dialog. It opens from the problems banner (`mc_sync_problems_text()`: the first issue's title
and how many more), from a device's row or page, and from the bar of a sync that finished with
tracks still missing (`mc_sync_finished_text()`). `mc_sync_issue_describe()` gives an issue's
title, text and fix in the current language. When tracks share albums, they're named by the
albums most of them are on, and otherwise by title, three at most ("“Blue Train”, “Giant
Steps” and 4 other albums").

The scheduler works out the list along with the waiting tracks and the problems
(`core/src/sync/issues.c`, one function per source):

- **The checks** ([Verification](#verification)): files that failed a check, files an
  archive's sweep couldn't place, and tracks lost for good.
- **The last receive turn with each member**, kept in memory and replaced by the next turn
  with that member:
  - Before asking for any file, the receiver checks that its library folder accepts a new
    file, and that the turn's files fit in its free space with 64 MB to spare. If not, it
    doesn't fetch anything (`FILES_PLAN` of 0) and says why: `FOLDER_UNWRITABLE` (with the
    folder and the system's reason) or `DISK_FULL` (with the sizes).
  - Files that arrived but couldn't be written (`WRITE_FAILED`, with the last error).
  - A turn skipped because another session was fetching (`FILES_BUSY`).
  - Tracks the member refused with `FILE_ERR` (`REFUSED`), by its reason: `io`, `missing`,
    `no_transcode`, or anything else as `format`.
  - Tracks left to another transcoder (see [Lossy copies](#lossy-copies)) that this device
    can't reach (`TRANSCODER_AWAY`), only shown while it can't.
- **The wanted tracks that aren't waiting** (`waiting.h`):
  - held only as originals by members that can't convert (`NO_TRANSCODER`, by member);
  - held by nobody, and put down to a member that said it no longer has them
    (`DEVICE_LACKS`, the importer first), a removed device that imported them
    (`DEVICE_LEFT`), or nobody known (`NO_HOLDER`, only once every member has reported its
    files).

  Lost tracks aren't repeated.
- **The peer table:** the hold a member gave when it refused a session (`HELD`, see
  [Session](#session) for `ERROR`), and an address where something else answers
  (`WRONG_DEVICE`).
- **The members' capabilities:** `capabilities.state` is a JSON object each device writes
  about itself (a synced column, from protocol 2's schema v13). It holds `"hold"`, what's
  holding sync back on its side (`"bin"`, `"cancelled"`, `"auto_off"`, `"not_chosen"`, in
  that order of precedence), and `"until"` (Unix ms, 0 for no end). Readers ignore keys they
  don't know, so later versions can add more. A member's published hold is a `HELD` issue
  unless it has lapsed. A refusal from the member is newer, so it outweighs the published
  hold. A member that may convert for others but has less than `MC_SYNC_TRANSCODE_MIN_FREE`
  free is `LOW_SPACE` (this device included).
- **Archives** ([Taking over an archive](#taking-over-an-archive)):
  - the group's only archive away for `MC_SYNC_ARCHIVE_AWAY_DAYS` (7, `ARCHIVE_AWAY`) or lost
    for `MC_SYNC_ARCHIVE_LOST_DAYS` (30, `ARCHIVE_LOST`);
  - this device looking like it was reinstalled (`ARCHIVE_REINSTALL`);
  - a takeover under way here (`TAKEOVER`);
  - what each other archive publishes about itself: no room for what's new (`ARCHIVE_FULL`,
    with the tracks and the space it needs), and tracks whose files it lost
    (`ARCHIVE_DAMAGED`, only those that no other device holds the original of).
- **Notices** are things sync did by itself, kept in memory until the user has seen them
  (`mc_sync_dismiss_notices()`):
  - this device became the archive again (`mc_archive_restore()`);
  - another device stays the archive as the last one (a void demotion seen to start);
  - the storage limit set when this device joined (`mc_profile_prefill()`);
  - a takeover that finished.

  In the banner they only show when there's no problem.

When an issue lapses by itself (a hold's `until`), the list is worked out again at that point.
Problems come first, then notices. Within each, this device's own come first, then the rest by
device name.

The device rows cover the rest. `mc_sync_device.hold` and `next_auto_ms` say what's holding
automatic sync with that device back here, and when it next runs. `mc_sync_device_note()`
words it as "Next automatic sync after 14:30", or "the next time Kithsong is open after 14:30"
on Android. The rows also have `peer_hold` (the device's own hold, published or given in a
refusal), `low_space`, `wrong_address` and `last_seen_ms` (the newest of its last complete
session, discovery seeing it and a session with it, as `peer_contact.seen_ms`).
`mc_sync_device_warning()` gives a row's warning, the first that applies of: its version, its
hold, its reachability (`mc_sync_reach_text()`, also used on the device pages), something else
at its address, an archive away ("Keeps the only full copy of the library and last synced 12
days ago."), or lack of space. `mc_log_set_handler()` takes core's log lines (stderr
otherwise), and `mc_set_trash_handler()` lets a platform move files to the trash its own way
(a Flatpak does it through the portal).

## Taking over an archive

The group always has an archive, but a device can be lost for good: stolen, broken, or wiped
and set up again. Every other member warns about it and offers to take its place when the
group's only archive is another device, isn't online, and was last seen long ago.

**Last seen** is the newest of:

- this device's last complete session with it (`peer_contact.last_sync_ms`);
- this device seeing it by discovery, or a session that went through
  (`peer_contact.seen_ms`, written at most once a minute);
- what the archive last published about itself: `"seen"` in its `capabilities.state` (the
  start of the day, Unix ms, so it changes once a day and reaches devices that never meet it).

It's also `mc_sync_device.last_seen_ms`.

**What an archive publishes** in `capabilities.state`, besides its hold:

- `"seen"`: as above.
- `"full"`: `{"tracks": n, "bytes": b}`, while its last receive turn with some member found
  that the turn's files don't fit (see [Issues](#issues)). `n` is the number of files, and
  `b` the extra bytes needed, counting the 64 MB a fetch leaves free. Others show
  `ARCHIVE_FULL`, "Studio PC is full: 3 tracks aren't backed up".
- `"damaged"` and `"damaged_n"`: the uids of its tracks whose files failed a check (at most
  50), and how many there are in all. Others show `ARCHIVE_DAMAGED` for those that no other
  device holds the original of, and name them. The archive automatically takes a member's
  lossy copy of a track it has nothing left of (`copy_kbps`, see
  [Verification](#verification)), so the fix mentions that.

**Away and lost.** When the only archive hasn't been seen for 7 days, members show
`ARCHIVE_AWAY` ("Studio PC last synced 12 days ago"), and the archive's row shows it too. At
30 days it's `ARCHIVE_LOST`, and `mc_sync_get_archive_offer()` gives an offer:

- the space the whole library needs here as originals (less what this device holds already);
- the free space;
- whether this device has room for it with 64 MB to spare (`can_take`, the only case with a
  button);
- whether no current member has room (`nobody_can`: each member's published free space
  against the library, less the originals its profile holds). In that case the advice is to
  pair a device with that much room, and nothing is removed;
- the tracks that no device except the lost archive holds the original of. Holders are
  counted the same way as for the waiting tracks: a member whose profile holds the original,
  or that imported it, and hasn't said it lacks it.

Platforms show it once as a dialog (`mc_sync_archive_offer_seen()`, kept in
`peer_contact.offer_asked`), and then as a notice on the lost device's row and page. Nothing is
said while some other member is an archive. So a group isn't warned just for having one
archive, and once the first device to take over has its flag reach the others, the offer
disappears everywhere else.

**Reinstalled.** A device gets the same offer, without waiting 30 days, when it looks like the
lost archive set up again: the same kind, the same host name or device name, the group's
library open with its files in place, and it joined the group after the archive was last seen.
A name or host alone is never enough.

**Taking over** (`mc_sync_take_over_archive()`): the device writes the lost device's uid in
`local.takeover_uid`, sets its own archive flag, and syncs with every member. A worker then
checks once a second: tracks held here, tracks that some member other than the lost one could
still send, and the rest. While some can still come, it's `FETCHING`. When none can, it checks
its files (`mc_sync_verify()`, with a deep check first for a reinstall) and looks again. If
there are still none to come, it removes the lost device the way `mc_sync_remove_device()`
does, and is `DONE`, keeping the tracks it couldn't have as originals
(`mc_sync_takeover_get()`, and a `TAKEOVER` notice naming them). Those are then lost for good
on this archive, as [Verification](#verification) explains. A takeover survives a restart:
`local.takeover_uid` resumes it. If another device removes the lost one first, the takeover
finishes too.

## Verification

- Transit is authenticated by Noise. File checksums catch truncation and disk errors, and
  give a lasting record of what arrived.
- `mc_sync_verify(fast)` compares `size_bytes` and `mtime_ms`, and `mc_sync_verify(deep)`
  re-hashes against `files.sha256`. A fast check runs after each sync, and a deep one weekly
  or on demand. Failed copies from peers are deleted and fetched again in the next session.
  - The fast check after a session runs after reconcile, so a file it deletes is fetched
    again in the following session.
  - A check tells three cases apart:
    - **missing**: the file isn't there;
    - **changed**: its size or time doesn't match the row;
    - **corrupt**: its size and time match the row, but its hash doesn't.

    Bytes that changed along with the time may be the user's own edit, so only the last case
    is taken as damage. A file whose time alone changed, and whose hash didn't, was only
    touched: a deep check gives its row the new time.
  - A deep check records the hash of a file that doesn't have one (an imported original),
    which then counts as verified. Sending such a file records it too, under the same
    condition (the file's size and mtime still match its row), since it's read whole either
    way.

### Failed originals

A check never deletes originals imported on the device, since there may be no other copy. They
keep their rows, and are noted in `file_failed` in `library.db`, which belongs to this device
alone:

- A missing or corrupt one doesn't satisfy anything and isn't offered to anyone. So the track
  counts as missing here, and the next session fetches it from whichever member has it. Once
  a good copy in the wanted format is here, the failed row goes, along with a corrupt file,
  and a copy that landed as `<name> (2).<ext>` takes the name.
- One that only changed still counts as the track's file. It's reported, and nothing fetches
  over it. If a file comes back, it passes the next check and is taken off the list.
- A fast check can't see corruption, so what a deep check found stands until the row changes.

### The weekly deep check

It runs every `MC_SYNC_DEEP_CHECK_DAYS` (7). A desktop or laptop runs it automatically. The
scheduler checks 10 minutes after start and then every minute. The deep check runs on a
separate thread while `mc_power_on_ac()` reports mains power, which is always the case on a
desktop, and stops within one read of the battery taking over. A phone has no scheduler for
this. Instead, periodic work that needs a charger calls `mc_sync_verify_weekly()` in slices, as
a separate job, requested daily.

- `sync.db`'s `local` keeps when it last finished (`deep_check_ms`) and the last track it got
  through (`deep_check_at`, saved every 30 seconds and when it stops). So an interrupted run
  carries on from there, and a machine that's restarted daily still gets through a large
  library.
- It rests for as long as it worked, up to half a second per file, to leave the disk free for
  whatever's playing.
- Unlike `mc_sync_verify()`, it doesn't hold anything while it reads, so a session can carry
  on meanwhile. Only a file that seems wrong waits for the session's file phase. It's then
  checked once more, against a row that must still say the same thing, before anything is
  done about it.

### The card

`mc_sync_get_problems()` is what the frontends show at the top of the library. The scheduler
works it out along with the waiting tracks, and `on_problems` announces it when it changes. It
covers files that failed and weren't replaced, the unknown files an archive's latest sweep only
reported (`local.unknown_files`), and on an archive, the tracks that are lost. It goes away by
itself.

### Missing for good

`mc_sync_remove_lost()` deletes tracks from the library, and so from every device, and nothing
can fetch them back. So a track is only **lost** on clear evidence, and any doubt keeps it off
the list. All of these must be true:

- This device fetches files (see [Waiting for a device](#waiting-for-a-device)) and is an
  archive, so it wants every track.
- It holds nothing of the track: no file in the right format, and no other file on disk that
  wasn't found corrupt. A lossy copy, or a file that only changed, saves it.
- Every other current member said, in a session in the last `MC_SYNC_LOST_ASKED_DAYS` (7),
  that it has **no file of the track at all**. `HAVE_RESP` reports this. A track answered
  with neither `original` nor `cached` also carries `copy`, which says whether any file of it
  is there (a lossy or stale one included), and `copy_kbps`, the best transcode of its current
  file the member could send. `peer_lacks` keeps `has_copy` and `said_ms` from that. A member
  that was never asked while the track was missing has no row, and one that's been away for a
  week has an old one. Either of those keeps the track safe, as that member may be the last to
  hold it. Removing a member from the group is how a device that's gone for good stops holding
  the rest back.

What comes first, by the same rule: a member with the original gives it, as for any missing
track. A member with only a lossy copy gives that, unchanged at its bitrate, to an archive that
has nothing left of the track. The track stays playable, and isn't lost. It counts as a
transcode, so the original is still wanted if it ever turns up.

`mc_sync_remove_lost()` takes the uids the user confirmed and works out the list again itself,
so anything that came back since the card was drawn stays. It refuses with `MC_ERR_LOCKED`
while a session's file phase is running, as that may be bringing one of them back. The removal
is an ordinary delete (`kithcore/delete.h`). The frontends always ask first, and name the
tracks.

## Open questions

- Compaction must keep the rows that decide [Duplicates](#duplicates), including
  `merge_name`, or a device joining later can't derive the same classes.
- How long to keep changelog history once every member has acknowledged it (compaction).
