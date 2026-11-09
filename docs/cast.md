# Google Cast

> Status: Android casts through Google's Cast SDK (`android/.../cast/`). On Windows and Linux
> the core is the sender (`core/include/kithcore/cast.h`, `core/src/cast/`); the frontends'
> cast button and picker are still to come. Change this document first, then the code.

## Goals

- Play to a Chromecast, a TV with Cast built in, or a Cast speaker, with the same behaviour
  on every platform: the queue, shuffle, stations and up next carry on as they do locally,
  plays still count and scrobble, the device's volume can be set, and a cast left paused for
  three minutes ends and playback stops.
- No receiver app of our own: the Default Media Receiver (app id `CC1AD845`) plays a URL,
  with title, artist, album and cover.
- The desktops have no Play Services, so there the core does what the Cast SDK does on
  Android: finding devices, the protocol, and serving the music.

Not goals, for now: groups of speakers set up in Google Home (they're ordinary devices to us
and should work, untested), casting video, and Google's device authentication (below).

## How playback moves

The player stays what decides what plays. While casting, it's in remote mode
(`core/src/playback/remote.h`): every `mc_player_*` call works as always, but instead of
decoding, the player tells the device what to do, and takes what the device reports as
what's playing. Frontends keep using `kithcore/playback.h` and need nothing else for
playback; Android's `CastPlayer.kt` does the same job in Kotlin around a muted core.

- The device's queue is only ever two items: the player's current one, and the one the
  player says comes after it, which the receiver fetches 20 seconds before the first ends
  and goes on to by itself, without a gap of any length. When what comes next changes (up
  next, shuffle, a station adding tracks), the second item is swapped.
- Next, previous and the rest go through the player's own order, so moving on from the last
  track of up next goes to the list's next, as it does locally.
- Pausing or seeking from the TV's remote or another phone shows in the player. So does the
  device going on to the queued item.
- Time listened is the time the device reported playing, so plays count by the core's rule
  (`mc_play_counts()`) and the play log fills in, row for row, as it would locally.
- Connecting moves what's playing to the device at the same place, playing or paused. However
  a cast ends, the player is left on the item the device was on, at the place it had got to,
  paused; after the three-minute pause, stopped.
- The device's volume is its own (`mc_cast_set_volume()`); the player's stays the computer's.

## Finding devices

Devices announce `_googlecast._tcp.local.` over mDNS. The browser (`cast_browse.c`) runs on
the same sockets as Kithsong's own discovery (`discovery/msock.c`: one per interface, reopened
when they go deaf), asks at once and then 1, 2, 4... seconds apart up to every 20 s, and asks
for SRV, TXT or A records an answer left out. From TXT it takes `id` (the device's stable id),
`fn` (its name) and `md` (its model). A device not heard for a minute, or that says goodbye,
is dropped.

## The protocol (CASTV2)

TLS to port 8009 (Mbed TLS, TLS 1.2). Devices present a self-signed certificate, which isn't
checked: senders may skip Google's device authentication (`urn:x-cast:com.google.cast.tp.deviceauth`),
and do without it, so the connection is private but the device is whoever answered at its
address.

Each message is a 4-byte big-endian length, then a protobuf `CastMessage` (written and read by
hand in `castv2.c`) carrying a JSON payload between `sender-0` and `receiver-0` or the app's
`transportId`:

| Namespace | Messages |
|---|---|
| `tp.connection` | `CONNECT`, `CLOSE`, to the receiver and to the app |
| `tp.heartbeat` | `PING` every 5 s, `PONG` back; nothing heard for 15 s is a lost connection |
| `receiver` | `GET_STATUS`, `LAUNCH` (when the Default Media Receiver isn't running), `SET_VOLUME`, `STOP` |
| `media` | `QUEUE_LOAD` (the current item and the next), `QUEUE_INSERT`, `QUEUE_REMOVE`, `QUEUE_GET_ITEM_IDS`, `PLAY`, `PAUSE`, `SEEK`, `STOP`, `GET_STATUS` |

Each item's URL names the player's id for it, which is how a `MEDIA_STATUS` is tied to an
item. While requests are unanswered, statuses can be from before them, so only the device's
going on to the queued item counts then.

A cast ends when:

- the user stops it (`mc_cast_disconnect()`): the receiver is stopped;
- it's been paused (or stopped) for three minutes: the receiver is stopped, and playback;
- another app casts to the device (its media replaces ours, or another receiver app starts):
  the receiver is left to them;
- the receiver stops or closes our connection (the TV, Google Home);
- the connection drops and three tries to get it back (1, 2 and 4 s apart) fail. A try that
  works joins the running receiver, and if its media is still ours, carries on where it was;
  else the player loads it again.

## Serving the music

The device fetches by URL, so a small HTTP server (`cast_http.c`) runs while a cast is
connected, on the computer's address on the network the device was reached on (the local end
of the TLS connection), at a port the system picks. Every URL is `/<token>/<name>`, with a
random token per server, and only the current and next items and their covers are up, so
nothing else on the computer can be fetched. Tracks have their type and length, and byte
ranges (206, 416), which the device uses to seek; answers allow cross-origin fetching, as the
receiver is a web page.

| File | Served as |
|---|---|
| FLAC, MP3, WAV | as it is |
| Opus | as it is, `audio/ogg` |
| AIFF | 24-bit PCM WAV, decoded as it's sent: lossless, with its length known beforehand so ranges work |

Each item's title, artist, album and track number come from the file's tags (the file's name
when it has no title), and the cover from the cover in its folder (`mc_coverart_find()`, as the
library has it), else the picture embedded in the file.

## Threading

`mc_cast_*` may be called from any thread, but not from the callback. The callback runs on
the discovery thread or a cast's own thread, never inside an `mc_cast_*` or `mc_player_*`
call, so frontends marshal it to their UI thread. Inside, a cast's thread holds the TLS
connection; the player's thread queues what it asks of the device for it; locks are taken
in the order: the cast's player lock (`use`), the player's, then the handle's.

## Frontends

On the desktops, one `mc_cast` for the app's life, freed before the player:

- A cast button, shown always (there's no telling ahead whether devices exist), named
  `MC_STR_CAST` while idle, `MC_STR_CAST_CONNECTING_TO` while connecting, and
  `MC_STR_CAST_CASTING_TO` while casting, with the device's name or `MC_STR_CAST_DEVICE_UNNAMED`.
- It opens a picker headed `MC_STR_CAST_TO`, running discovery while open, with
  `MC_STR_CAST_LOOKING` and `MC_STR_CAST_LOOKING_TEXT` until a device shows up. While
  connected, the picker has the device's volume and `MC_STR_CAST_STOP` instead.
- After a cast ends with `MC_CAST_END_FAILED` or `MC_CAST_END_LOST`, say so:
  `MC_STR_CAST_FAILED`, `MC_STR_CAST_LOST`.
