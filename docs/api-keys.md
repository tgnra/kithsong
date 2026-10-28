# API keys and contact info

The app talks to four online services. Two of them need credentials that you, as the person
building the app, register for once and bake into every build:

| Service | What you need | Build setting | State in the code |
|---------|---------------|---------------|-------------------|
| MusicBrainz (and the Cover Art Archive) | no key; a contact URL or email for the User-Agent | `KITHSONG_CONTACT` | used by the importer on every platform |
| Last.fm | an API key **and** a shared secret | `KITHSONG_LASTFM_KEY`, `KITHSONG_LASTFM_SECRET` (or a `last.fm.api` file) | done: sign-in in Settings on Windows, Linux and Android; every platform sends plays |
| AcoustID | an *application* (client) key | `KITHSONG_ACOUSTID_KEY` | **not implemented**: the key is plumbed through the build but nothing reads it |
| iTunes Search (cover art fallback) | nothing | - | used by the importer |

All of these are CMake cache variables, declared in [`CMakeLists.txt`](../CMakeLists.txt)
and [`core/CMakeLists.txt`](../core/CMakeLists.txt). CMake writes the contact and the AcoustID
key into a generated header, `build/<preset>/core/generated/config.h`, from
[`core/src/config.h.in`](../core/src/config.h.in):

```c
#define MC_CONTACT "@KITHSONG_CONTACT@"
#define MC_ACOUSTID_KEY "@KITHSONG_ACOUSTID_KEY@"
```

The Last.fm key and secret go into `build/<preset>/core/generated/lastfm_key.h` instead,
**obscured** ([`cmake/LastfmKey.cmake`](../cmake/LastfmKey.cmake)): each is XORed with a
random mask made once per build folder and split into its even and odd bytes, and
[`core/src/scrobble/lastfm_key.c`](../core/src/scrobble/lastfm_key.c) decodes them onto the
stack only while a request is signed, then wipes them. So `strings` on the binary doesn't show
them. That's obscurity, not protection (see [The secret is in the binary](#3-the-secret-is-in-the-binary)).
The Last.fm pair can also come from the environment or a file, without `-D`:

1. `-DKITHSONG_LASTFM_KEY=...` and `-DKITHSONG_LASTFM_SECRET=...` (presets, command line);
2. else the environment variables `KITHSONG_LASTFM_KEY` and `KITHSONG_LASTFM_SECRET` at
   configure time;
3. else the file `KITHSONG_LASTFM_FILE` names, by default `last.fm.api` at the top of the
   repo (gitignored): the text of Last.fm's "API account" page pasted as it is, which has an
   `API key <hex>` line and a `Shared secret <hex>` line.

Values from 2 and 3 aren't stored in `CMakeCache.txt`. Configure prints which one it used
("Last.fm: API key from ...") or "no API key". They are compile-time constants either way:
nothing is read at run time.

An empty value is always allowed and never breaks the build. It turns the feature off
(Last.fm, AcoustID) or sends a placeholder contact (MusicBrainz, with a configure warning).

---

## Where the values go

Set the four values in one of these places. **None of them is committed**: `/build/`,
`CMakeUserPresets.json`, `*.local.cmake` and `android/local.properties` are all in
[`.gitignore`](../.gitignore). Never put a key in `CMakePresets.json`, `android/gradle.properties`,
a workflow file or any other tracked file.

### Windows and Linux, local builds: `CMakeUserPresets.json`

Create `CMakeUserPresets.json` next to `CMakePresets.json` (it's gitignored). It inherits the
committed presets and adds the secrets:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "windows-keys",
      "inherits": "windows-w64devkit",
      "cacheVariables": {
        "KITHSONG_CONTACT": "https://github.com/<you>/kithsong",
        "KITHSONG_LASTFM_KEY": "<32 hex chars>",
        "KITHSONG_LASTFM_SECRET": "<32 hex chars>",
        "KITHSONG_ACOUSTID_KEY": "<key>"
      }
    }
  ],
  "buildPresets": [{ "name": "windows-keys", "configurePreset": "windows-keys", "jobs": 8 }]
}
```

Use `"inherits": "linux-gtk"` on Linux. Then `cmake --preset windows-keys` and
`cmake --build --preset windows-keys`.

Or pass them once on the command line, e.g.
`cmake --preset windows-w64devkit -DKITHSONG_LASTFM_KEY=... -DKITHSONG_LASTFM_SECRET=...`.
Two things to know about that:

- They're cache variables, so they **stick** in `build/<preset>/CMakeCache.txt` for later
  configures. To drop one, configure with `-DKITHSONG_LASTFM_KEY=` (empty) or delete the
  cache.
- `-D` values end up in your shell history. The presets file avoids that.

`-D` values sit in plain text in `build/<preset>/CMakeCache.txt`, which is gitignored, and
`lastfm_key.h` holds them obscured. Don't zip up and share a build folder.

### Any platform, local builds: `last.fm.api`

Copy the text of your application's page at <https://www.last.fm/api/accounts> into a file
named `last.fm.api` at the top of the repo. It's gitignored. A worktree or a build elsewhere
can point at it with `-DKITHSONG_LASTFM_FILE=<path>` (Gradle: `-PkithsongLastfmFile=<path>`).

### Android: Gradle properties

[`android/native/build.gradle.kts`](../android/native/build.gradle.kts) (lines 18-27) reads
four Gradle project properties and passes them to CMake:

| Gradle property | CMake variable |
|-----------------|----------------|
| `kithsongContact` | `KITHSONG_CONTACT` |
| `kithsongAcoustidKey` | `KITHSONG_ACOUSTID_KEY` |
| `kithsongLastfmKey` | `KITHSONG_LASTFM_KEY` |
| `kithsongLastfmSecret` | `KITHSONG_LASTFM_SECRET` |

`kithsongLastfmFile` passes `KITHSONG_LASTFM_FILE`. With none of them set, the native build
still looks for `KITHSONG_LASTFM_KEY`/`_SECRET` in the environment and then for
`last.fm.api` at the top of the repo, as above.

Either pass them on the command line
(`gradlew.bat assembleDebug -PkithsongContact=... -PkithsongLastfmKey=...`), or put them in
your **user-level** `gradle.properties`, outside the repo
(`%USERPROFILE%\.gradle\gradle.properties` on Windows, `~/.gradle/gradle.properties`
elsewhere):

```properties
kithsongContact=https://github.com/<you>/kithsong
kithsongLastfmKey=<32 hex chars>
kithsongLastfmSecret=<32 hex chars>
kithsongAcoustidKey=<key>
```

Not `android/gradle.properties`: that file is tracked. Not `android/local.properties`
either: Gradle doesn't read it as project properties, so `findProperty()` wouldn't see the
values there.

### Release builds: environment variables and GitHub secrets

The release scripts read `KITHSONG_CONTACT`, `KITHSONG_ACOUSTID_KEY`, `KITHSONG_LASTFM_KEY`
and `KITHSONG_LASTFM_SECRET` from the environment and warn about any that are missing:

- [`scripts/release-windows.ps1`](../scripts/release-windows.ps1) turns them into `-D` options.
- [`scripts/release-linux.sh`](../scripts/release-linux.sh) writes them to
  `packaging/flatpak/secrets.local.cmake` (gitignored, deleted again on exit), which the Flatpak
  manifest loads with `cmake -C`.
- [`scripts/release-android.sh`](../scripts/release-android.sh) and
  [`release-android.ps1`](../scripts/release-android.ps1) turn them into the `-P` properties
  above.

The Last.fm pair is only passed when **both** are set.

In CI, [`.github/workflows/release.yml`](../.github/workflows/release.yml) (lines 19-23) maps
four repository secrets of the same names into the environment. Set them with `gh secret set`;
see [RELEASING.md](RELEASING.md), step 4.

---

## MusicBrainz: contact info, no key

MusicBrainz needs no key. It does require every request to carry a **User-Agent that says who
you are and how to reach you**, and it allows **1 request per second** on average per IP.
Over that, it answers `503 Service Unavailable` and refuses everything from that IP until the
rate drops. A User-Agent without contact info counts as "anonymous" and gets throttled. The
format it asks for is:

```
Application name/<version> ( contact-url )
Application name/<version> ( contact-email )
```

(Checked against <https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting> on 2026-10-28.)

### How the app does it

- **User-Agent**: [`core/src/net/http.c`](../core/src/net/http.c) line 62 sets it on every
  request libcurl makes:

  ```c
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Kithsong/" MC_VERSION_STRING " ( " MC_CONTACT " )");
  ```

  That gives e.g. `Kithsong/0.1.0 ( https://github.com/you/kithsong )`, which is the
  recommended format. It's the one HTTP client for the whole core, so the same header also
  goes to the Cover Art Archive, iTunes, Last.fm and (later) AcoustID.
- **Rate limit**: [`core/src/tagger/musicbrainz.c`](../core/src/tagger/musicbrainz.c) line 26
  spaces requests 1.1 s apart, shared across every thread in the process. On a 503 it retries up
  to 3 times, waiting 2, 4 and 8 s (lines 39-45). Nothing to change here.

### The placeholder

**`KITHSONG_CONTACT` defaults to empty.** CMake then prints a warning and substitutes the
literal string `no contact configured`
([`core/CMakeLists.txt`](../core/CMakeLists.txt) line 6). The User-Agent then reads
`Kithsong/<version> ( no contact configured )`, which has no contact info in it at all. That
is what every build gets today unless you set the variable, including Android builds without
`-PkithsongContact` and release builds without the secret.

**What to set it to**: something that reaches you and stays valid as long as old builds are
out there:

- the project's URL (e.g. `https://github.com/<you>/kithsong`), which is good if the issue
  tracker is open; or
- an email address you read, e.g. `kithsong@yourdomain`. It's sent in clear text with every
  request (to MusicBrainz, Last.fm, Apple and so on), so use one you don't mind being public.

Don't use `example.com` or a personal address you might drop. MusicBrainz uses the contact
to reach you *before* blocking the app if a bug starts flooding them.

### Cover Art Archive

The importer fetches front covers from `https://coverartarchive.org/{release|release-group}/<mbid>/front-1200`
([`core/src/tagger/coverart.c`](../core/src/tagger/coverart.c) lines 16, 44-48 and 114-115). It needs
no key, and its docs say there are no rate limits at the moment
(<https://musicbrainz.org/doc/Cover_Art_Archive/API>, checked 2026-10-28). They don't mention
a User-Agent requirement, but the app sends the same one anyway. Nothing to configure. The
iTunes fallback has its own limiter, 3 s between requests (line 24), since Apple publishes
no limit.

### Testing

```
build/<preset>/core/tests/manual_mb_ping
```

It should print `MusicBrainz OK: Radiohead`
([`core/tests/manual_mb_ping.c`](../core/tests/manual_mb_ping.c)). That proves HTTPS works,
not that your contact is right. To check the header, look at the generated
`build/<preset>/core/generated/config.h` for `MC_CONTACT`.

---

## Last.fm scrobbling

### 1. Create an API account

1. Sign in to Last.fm (any account; consider a project account rather than your personal
   one), then go to <https://www.last.fm/api/account/create>.
2. Fill in the form. It asks for an application name and description, a contact email, and
   an optional callback URL and homepage. **Leave the callback URL empty**: the app uses the
   desktop flow (below), which doesn't redirect anywhere. *(Unverified: the page needs a
   login, so its fields couldn't be checked for this doc.)*
3. You get two 32-character hex strings:
   - **API key**: identifies the app. Sent in clear text with every call and in the
     browser sign-in URL. It's not secret as such, but treat it carefully anyway.
   - **Shared secret**: used only to **sign** calls (`api_sig`). It is never sent over the
     network. It's the one that must not end up in git.

Your applications are listed at <https://www.last.fm/api/accounts>, where you can see both
values again.

### 2. How the app signs in: the desktop auth flow

The code uses Last.fm's **desktop application** flow
(<https://www.last.fm/api/desktopauth>, checked 2026-10-28). It doesn't use the mobile
flow (`auth.getMobileSession`, which sends the user's password) or the web flow (which needs a
callback URL):

1. Settings' **Sign in** calls `mc_scrobbler_auth_begin()`, which calls **`auth.getToken`**
   ([`core/src/scrobble/lastfm.c`](../core/src/scrobble/lastfm.c) `mc_lastfm_get_token`) and
   returns `https://www.last.fm/api/auth/?api_key=<key>&token=<token>`, which the app opens in
   the browser (Windows: `settings_view.c`, Linux: `settings_view.c`, Android:
   `ScrobblingSettings.kt` with `playback/Scrobbling.kt`).
2. The user approves the app on that page. The app never sees their password.
3. The app polls `mc_scrobbler_auth_finish()` every 3 seconds, for up to 10 minutes, on a
   thread of its own (Cancel stops it). It calls
   **`auth.getSession`**, which fails with error 14 ("token not yet authorised", reported as
   `MC_ERR_AUTH`) until the user has approved. Tokens last 60 minutes.
4. The session key it gets never expires unless the user revokes the app in their Last.fm
   settings. It's stored with the user name in the `scrobble_account` table of `sync.db`
   and synced to the rest of the device group
   ([sync-protocol.md](sync-protocol.md)), so signing in on one device signs in all of them.

Every call is a form POST to `https://ws.audioscrobbler.com/2.0/`, signed as Last.fm specifies:
the MD5 of all parameters sorted by name, concatenated name-value, followed by the secret
(`mc_lastfm_sign`, [`lastfm.c`](../core/src/scrobble/lastfm.c)). The addresses are in
[`core/src/scrobble/lastfm.h`](../core/src/scrobble/lastfm.h) lines 11-12.

How the build's key is used: [`scrobbler.c`](../core/src/scrobble/scrobbler.c) lines 85-88
fill in `MC_LASTFM_KEY` and `MC_LASTFM_SECRET` when the synced account names no key of its
own. A service chosen with `mc_scrobbler_set_service()` (Libre.fm, say, which takes any key)
can bring its own key and secret instead. If the build has none and the service is Last.fm,
every call fails with `MC_ERR_AUTH` before touching the network
([`lastfm.c`](../core/src/scrobble/lastfm.c) line 120). That is "scrobbling off".

Rules the code already follows, so you don't have to: plays of tracks under 30 s and plays
with no artist aren't sent. Batches hold at most 50. Plays older than 14 days are dropped. Now
playing isn't retried. Errors 11, 16, 8 and 29 back off (30 s up to 30 min), and a play
ignored with code 5 (daily limit) stays queued. When a play counts (half the track or 4
minutes) is decided by the player. These match <https://www.last.fm/api/scrobbling>
(checked 2026-10-28).

### 3. The secret is in the binary

A desktop or mobile app can't truly keep the shared secret secret. It's compiled into
the binary. It's stored obscured (XORed and split, see [above](#api-keys-and-contact-info)),
so `strings` or a hex editor doesn't show it, but anyone with the binary and a debugger can
still get it, since the app has to decode it to sign requests. Last.fm knows this about
desktop apps. The realistic goals are:

- keep it **out of git** and out of logs and screenshots (use the places in
  [Where the values go](#where-the-values-go));
- if it leaks somewhere public, or someone abuses your key, **reset or replace it** at
  <https://www.last.fm/api/accounts> and rebuild. Old builds then stop scrobbling until
  users update, since their signatures stop matching (error 13, "invalid signature").

Tests that go in the repo should use made-up values, as
[`core/tests/test_scrobbler.c`](../core/tests/test_scrobbler.c) does (it talks to a fake
server).

Also worth knowing from the API terms (<https://www.last.fm/api/tos>, checked 2026-10-28):
use is non-commercial unless you get a commercial agreement from Last.fm, and apps that
*show* Last.fm data must credit and link to Last.fm. Scrobbling alone doesn't show any.

### 4. Test it

Sign in from Settings > Scrobbling on any platform. A build without a key says so there
("built without a Last.fm API key") and greys out Sign in. Every platform sends now playing
and plays while signed in and this device's "Scrobble what I play on this device" switch is on
(`desktop/*/src/playback.c`, Android's `PlaybackService`). To test without the app, sign in with
the manual test program, which is built with the tests (`KITHSONG_BUILD_TESTS`, on by
default):

1. Configure and build with your key and secret (e.g. the `windows-keys` preset above).
   `test_scrobbler` checks that a build *without* a key refuses to sign in, so it's fine
   either way.
2. Make a scratch library folder holding a few tagged tracks, and an empty scratch config
   folder. Don't use your real ones: the sign-in is saved in the config folder's `sync.db`.
3. Run:

   ```
   build/windows-keys/core/tests/manual_scrobble <library folder> <config folder>
   ```

   ([`core/tests/manual_scrobble.c`](../core/tests/manual_scrobble.c)). It prints
   `Approve at https://www.last.fm/api/auth/?api_key=...&token=...`. Open it, approve, and it
   continues. It sends the library's first track as now playing, then as a play from five
   minutes ago, and should end with `Scrobbled as <your user>`.
4. Check <https://www.last.fm/user/<you>> for the scrobble.
5. To test the real app end to end, start it on the same config folder
   (`kithsong.exe --config-dir <config folder>` on Windows). The sign-in is already in that
   folder's `sync.db`. When it asks for a library folder, pick the scratch library, not your
   Music folder. Play a track past halfway (or for 4 minutes) and check Last.fm again.

What failures mean:

| Symptom | Likely cause |
|---------|--------------|
| `auth_begin` gives `MC_ERR_AUTH` straight away, no network traffic | the build has no key or no secret (configure says "Last.fm: no API key") |
| error 10, "Invalid API key" | wrong key, or a typo in the key |
| error 13, "Invalid method signature" | wrong secret, or the key and secret are from different accounts |
| stuck "Waiting..." | the approval page wasn't opened or wasn't approved; the token runs out after an hour |
| error 26, "Suspended API key" | Last.fm disabled the key |

To try it without a Last.fm account at all, Libre.fm speaks the same API and takes any key:
`manual_scrobble <lib> <cfg> https://libre.fm/2.0/ https://libre.fm/api/auth/ anykey anysecret`.

---

## AcoustID fingerprinting

### What it is and how it would fit

[AcoustID](https://acoustid.org) identifies a recording by **how it sounds**, not by its tags.
[Chromaprint](https://github.com/acoustid/chromaprint) decodes about the first two minutes of
the audio and computes a compact fingerprint. The AcoustID web service matches it against its
database and returns AcoustID track ids, each linked to one or more **MusicBrainz recording
ids**. From a recording id, the existing MusicBrainz code can find the releases it's on.

In this app's importer ([tagging-rules.md](tagging-rules.md), step 3 "Identify"), it's meant
for the files that tags can't place: untagged or badly tagged files, where today the only
clues are the file name (`03 - Artist - Title.mp3`) and its length. A fingerprint hit would
give a recording id to look up, the way a `MUSICBRAINZ_TRACKID` tag does now.

### How far it's implemented: not at all, apart from the key

Being specific about it:

- **There is a build setting for the key.** `KITHSONG_ACOUSTID_KEY` is in `CMakeLists.txt`
  line 42, the Gradle property `kithsongAcoustidKey`, the release scripts, CI and RELEASING.md.
  It ends up as `MC_ACOUSTID_KEY` in `config.h` ([`core/src/config.h.in`](../core/src/config.h.in)
  line 11).
- **No code reads `MC_ACOUSTID_KEY`.** There's no AcoustID client, no fingerprinting, and no
  Chromaprint or FFmpeg in `third_party/`. Chromaprint is listed under "Planned" in
  [LICENSES.md](../LICENSES.md). [TODO.md](../TODO.md) line 53, `docs/architecture.md`
  ("no AcoustID yet"), `docs/tagging-rules.md` (*not yet*) and the README ("planned") all
  agree.
- The config header comment ("empty disables fingerprinting") and the release scripts'
  warnings ("fingerprinting will be off in this build") describe the intended behaviour. Today
  fingerprinting is off whatever you set.
- The Android manifests and the Flatpak manifest mention AcoustID in comments about network
  access. That's forward-looking too.

So registering a key now is optional. It costs nothing and means release builds are ready
when the feature lands.

### Getting a key

1. Sign in at <https://acoustid.org/new-application> with a MusicBrainz account (recommended),
   a Google account or OpenID.
2. Register an application. It gives you an **application API key**. In API calls it's the
   `client` parameter, and every call needs it.
3. Don't confuse it with a **user API key** (the `user` parameter). That one belongs to an
   individual AcoustID user, is needed only to *submit* new fingerprints, and must never be
   baked into the app. Each user would enter their own. The app only needs the application
   key for lookups.

Put the key where the others go: `KITHSONG_ACOUSTID_KEY` / `kithsongAcoustidKey` /
the `KITHSONG_ACOUSTID_KEY` secret. Like the Last.fm secret it ends up readable in the binary,
which is normal for AcoustID client keys. Keep it out of git anyway, and re-register if it's
abused.

Service rules (<https://acoustid.org/webservice>, checked 2026-10-28):

- **No more than 3 requests per second.** The implementation should add a limiter for it, as
  `musicbrainz.c` has (`mc_ratelimit_init(&rate, 334)` or a little more), shared across
  threads.
- The lookup endpoint is `https://api.acoustid.org/v2/lookup` with `client`, `duration` and
  `fingerprint`. Add `meta=recordingids` to get MusicBrainz recording ids. `recordings`,
  `releasegroups` and `compress` add more metadata.
- The free service is for **non-commercial use only**. Commercial use goes through
  acoustid.biz. Tell the AcoustID team in advance if you expect a lot of traffic.

Each AcoustID hit will usually be followed by MusicBrainz lookups, and those stay under the 1
request per second MusicBrainz limit. So in practice MusicBrainz, not AcoustID, sets the pace.

### Chromaprint and its license

There are two ways to get fingerprints:

- **Link the Chromaprint library** (`libchromaprint`, C API `chromaprint_feed()` and so on)
  and feed it PCM from the decoders the core already has (miniaudio, dr_libs, opusfile).
  Doing your own decoding means fpcalc's FFmpeg isn't needed.
- **Run `fpcalc`**, Chromaprint's command-line tool, which prints `DURATION=` and
  `FINGERPRINT=`. It's simple, but it's an extra executable to ship on each platform, it's
  awkward on Android, and fpcalc decodes through FFmpeg.

Licensing (from Chromaprint's
[LICENSE.md](https://github.com/acoustid/chromaprint/blob/master/LICENSE.md), checked
2026-10-28):

- Chromaprint's own code is **MIT**, but it includes parts of FFmpeg under **LGPL-2.1**, so
  "as a whole, Chromaprint should be therefore considered to be licensed under the LGPL 2.1
  license". This is what [LICENSES.md](../LICENSES.md) records: LGPL-2.1, MIT for parts.
- The **FFT backend** matters too. **FFTW3 is GPL** and makes the resulting Chromaprint
  binary GPL, so avoid it. KissFFT (bundled, permissive) is the safe choice. FFmpeg's FFT
  keeps things at LGPL if FFmpeg is built without its GPL parts. On Apple there's vDSP.
- This project links everything statically (see `BUILD_SHARED_LIBS OFF` in `CMakeLists.txt`).
  LGPL-2.1 allows static linking only if you let users relink, e.g. by shipping the app's
  object files. Otherwise link Chromaprint dynamically. LICENSES.md already says "link
  dynamically or provide relinkable objects". Decide this before adding the dependency, then
  move Chromaprint from "Planned" to the main table in LICENSES.md and add its license text
  to the shipped licenses.
- If you ship `fpcalc` instead, its FFmpeg build's license (LGPL or GPL, depending on how it
  was configured) comes along with it.

(This is a summary, not legal advice.)

---

## Checklist

- [ ] **Contact**: choose a long-lived URL or email. Set `KITHSONG_CONTACT` locally
      (CMakeUserPresets.json), as `kithsongContact` in `~/.gradle/gradle.properties`, and as
      the `KITHSONG_CONTACT` GitHub secret. Without it every build says
      `( no contact configured )` (`core/CMakeLists.txt` line 6).
- [ ] Check `build/<preset>/core/generated/config.h` shows your contact. Run `manual_mb_ping`.
- [ ] **Last.fm**: create an API account at <https://www.last.fm/api/account/create>, with no
      callback URL. Note the API key and shared secret.
- [ ] Put both in `last.fm.api` at the top of the repo (or CMakeUserPresets.json:
      `KITHSONG_LASTFM_KEY`, `KITHSONG_LASTFM_SECRET`; `~/.gradle/gradle.properties`:
      `kithsongLastfmKey`, `kithsongLastfmSecret`) and the matching GitHub secrets. Set both
      or neither.
- [ ] Check neither shows in the binary, e.g. `strings kithsong.exe | grep -i <first 8 hex of the key>`
      (and the same for `libkithsong_jni.so` in the APK).
- [ ] Run `manual_scrobble` against scratch folders. It should end with
      "Scrobbled as <user>", and the play should show on your Last.fm profile.
- [ ] **AcoustID** (optional until it's implemented): register at
      <https://acoustid.org/new-application>. Set `KITHSONG_ACOUSTID_KEY` /
      `kithsongAcoustidKey` / the secret. It's the application key, never a user key.
- [ ] `git status` shows no key anywhere. `gh secret list` shows all four.
- [ ] Before AcoustID work starts: decide between linking Chromaprint dynamically and
      shipping relinkable objects, use KissFFT (not FFTW3), and update LICENSES.md.
