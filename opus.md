# Opus sync speed

## The short version

Opus syncs were slow because the encoder was the limit, not encode-ahead. The sender spent
about 80% of a sync waiting for encodes, and all 11 workers were busy for nearly all of it.
Nearly half of that encoding time wasn't Opus at all: it was the step that converts 44.1 kHz
audio to 48 kHz, and every track in this library is 44.1 kHz.

That step was running in a slow low-memory mode. One compile flag fixes it, and the same sync
now takes about 35% less time and 33% less CPU, with the same audio coming out.

| 128 kbps, this PC (i5-10400F, 6 cores, 12 threads) | Before | After |
|---|---|---|
| One thread, FLAC in | 5.5 MB/s | **7.9 MB/s** |
| One thread, Opus out | 0.75 MB/s | **1.08 MB/s** |
| One thread, speed against real time | 41x | **59x** |
| Real 96-file sync, best run, FLAC in (other work holding 7-10 cores) | 28.2 MB/s | **44.8 MB/s** |
| Real 96-file sync, best run, Opus out | 3.8 MB/s | **6.0 MB/s** |
| Real 96-file sync, CPU | 535 s | **363 s** |
| Estimate for an idle PC, 11 workers, FLAC in | about 60 MB/s | **about 87 MB/s** |
| Estimate for an idle PC, 11 workers, Opus out | about 8 MB/s | **about 12 MB/s** |

The idle-PC rows are estimates, not measurements: the machine sat at 90-100% busy with other
builds throughout. See [How far to trust these numbers](#how-far-to-trust-these-numbers).

Two things are worth keeping in mind when reading a speed off the screen:

- **Opus MB/s looks low even when it's going well.** At 128 kbps a megabyte of Opus stands for
  about 7.5 MB of FLAC, so 6 MB/s of Opus is 45 MB/s of music. An originals sync can't be
  compared with it byte for byte.
- **The jumping around is expected, and fixing the speed doesn't remove it.** It doesn't cost
  time overall. See [Why the rate jumps around](#why-the-rate-jumps-around).

## What changed

- `third_party/CMakeLists.txt`: `RESAMPLE_FULL_SINC_TABLE` on `mc_opusenc`. This is the fix.
- `core/src/sync/encode.c`: trace lines when an encode starts and ends.
- `core/tests/manual_opus_bench.c`, `core/tests/manual_opus_sync.c`: the tools behind this
  page. They aren't tests and don't run under ctest.

### The fix

libopusenc has its own copy of the Speex resampler. Speex picks whichever filter table takes
the least memory. Going from 44.1 to 48 kHz needs 160 filter phases, and storing them all
fails its test (`80*160 <= 80*16+8`), so it keeps a small table and works out every tap by
cubic interpolation, about four times the arithmetic. The flag makes it keep the full table.

| One track, thread CPU | Old | New |
|---|---|---|
| Share of the encode in the resampler (sampled profile) | 45% | 13% |
| Resampling alone, 244.6 s of audio | 2.40-2.52 s | 0.52-0.58 s |
| Speed of resampling alone | 100x | 450x |

It costs 50 KB per running encode at 44.1 kHz. An odd sample rate could need about 15 MB (the
table is 80 taps by up to 48000 phases), or somewhat more if it's also far above 48 kHz. That
only happens for rates sharing few factors with 48000, and it's freed when the encode ends.

### It doesn't change the audio

- **Quality.** Sines through each resampler, measured as what's left after removing the best
  fit (lower is cleaner):

  | Frequency | Old | New |
  |---|---|---|
  | 100 Hz | -112.3 dB | -112.2 dB |
  | 1 kHz | -112.4 dB | -112.4 dB |
  | 5 kHz | -118.9 dB | -119.1 dB |
  | 10 kHz | -110.2 dB | -110.2 dB |
  | 15 kHz | -111.4 dB | -117.0 dB |
  | 18 kHz | -105.7 dB | -114.7 dB |

  Gain stays within 0.0002 dB. The new one is equal at low frequencies and up to 9 dB cleaner
  near the top, because the interpolation error is gone.
- **Output.** One 48 s track encoded both ways: same file size (778,520 bytes), same length,
  no shift, and the decoded audio differs by -103 dB, about the size of 16-bit rounding.
- **Resyncs.** It forces none. A transcode is identified by source file and bitrate, not by
  its bytes or the encoder's settings: the cache is `<file_uid>/opus-<kbps>.opus`, and
  `mc_plan_file_satisfies` compares the bitrate. Files already cached or on a device stay
  valid. Encodes were never byte-for-byte repeatable anyway, because each gets a random Ogg
  serial, so hashes differ between any two encodes of a file with or without this change.

## Method

- Release build, own `-B` directory and own `FETCHCONTENT_BASE_DIR` (see the warning below).
- Old and new binaries differ by one object file (`resample.c`, relinked) and took turns, so
  both saw the same load.
- 24 real tracks copied from the library, chosen evenly across its size range: 720 MB of FLAC,
  1.5 hours of audio, 2.7-58.5 MB each, each with its cover. 98 of 99 sampled library tracks
  are 44.1 kHz 16-bit stereo (the other is 24-bit); the library averages 1069 kbps and 4.4 minutes a track.
- MP3 fixtures: 12 of those tracks through ffmpeg at 320 kbps. ffmpeg turned out to be
  installed here (under WinGet), so nothing needed synthesising except the silence, tone and
  noise inputs.
- Each number is CPU time alongside wall time. CPU time holds steady under load (see below);
  wall time doesn't.
- The real sync is `manual_opus_sync`: two `mc_sync` instances with their own libraries,
  paired, talking over encrypted TCP on loopback. Both run in one process, not two `.exe`s, so
  the UI isn't involved and the process CPU figures include the receiver.

## Where the time goes

One 244.6 s track at 128 kbps, complexity 8, mean thread CPU seconds over 3 runs. The stages
were timed one at a time. Resampling and encoding are also shown together, because that's how
they really run.

| Stage | FLAC old | FLAC new | MP3 old | MP3 new |
|---|---|---|---|---|
| Decode | 0.43 | 0.47 | 0.48 | 0.42 |
| Tags and cover | under 0.02 | under 0.02 | under 0.02 | under 0.02 |
| Resample | 2.40 | 0.55 | 2.62 | 0.54 |
| Opus encode | 2.76 | 2.98 | 2.94 | 2.84 |
| Resample and encode together | 5.10 | 3.47 | 5.39 | 3.53 |
| Hash and write to disk | 0.03 | 0.05 | 0.03 | 0.05 |
| **Whole transcode** | **5.91** | **4.13** | **5.97** | **3.90** |
| Speed | 41x | 59x | 41x | 63x |
| In | 5.5 MB/s | 7.8 MB/s | 1.6 MB/s | 2.5 MB/s |
| Out | 0.67 MB/s | 0.96 MB/s | 0.66 MB/s | 1.01 MB/s |

Disk, hashing, tags and covers are negligible. What's left is spread across libopus, with no
hot spot: the largest entries in the new profile are the resampler at 13%, the PVQ search at
10%, and `celt_encode_with_ec` at 6%.

What the audio contains does matter. 60 s of each at 128 kbps, resample and encode together:

| Input | Old | New |
|---|---|---|
| Silence | 70x | 154x |
| Tones with a little noise | 45x | 71x |
| White noise | 51x | 79x |

Real music ran close to the tones and noise here, and silence is about twice as fast. A
benchmark on silent fixtures would flatter the encoder.

## Across all threads

24 tracks at 128 kbps through `mc_transcode_cached`, longest first.

| Threads | Old wall | Old FLAC in | Old CPU | New wall | New FLAC in | New CPU |
|---|---|---|---|---|---|---|
| 1 | 151.5 s | 4.8 MB/s | 130.9 s | 122.8 s | 5.9 MB/s | 91.8 s |
| 2 | 88.5 s | 8.1 MB/s | 133.3 s | 60.5 s | 11.9 MB/s | 89.2 s |
| 4 | 58.9 s | 12.2 MB/s | 132.5 s | 35.4 s | 20.3 MB/s | 88.6 s |
| 6 | 35.5, 33.2 s | 20.3, 21.7 MB/s | 130.9, 131.5 s | 38.7, 37.8 s | 18.6, 19.1 MB/s | 90.3, 91.0 s |
| 8 | 29.7 s | 24.3 MB/s | 132.5 s | 81.9 s | 8.8 MB/s | 92.5 s |
| 11 | 33.1, 24.9 s | 21.8, 28.9 MB/s | 131.6, 130.7 s | 37.7, 33.5 s | 19.1, 21.5 MB/s | 93.3, 92.5 s |
| 12 | 24.3 s | 29.6 MB/s | 131.6 s | 31.4 s | 22.9 MB/s | 91.1 s |

**Don't read the wall columns as a scaling curve.** Other work held 6.6-11.3 cores during these
runs, and the two halves ran at different times, so the 8-thread new row at 81.9 s says
nothing about 8 threads. The CPU columns are the result: 130.7-133.3 s old, 88.6-93.3 s new,
at any thread count. That's 30% less work, and threads don't get in each other's way (no
locks or disk stalls show up as extra CPU).

By bitrate, at 11 threads, old and new one after the other. Complexity follows the app's table
(10 at 64 and 96, 8 at 128 and 160, 7 at 192 and 256):

| kbps | Old CPU | New CPU | Saved | New FLAC in, per CPU-second | New wall: FLAC in | New wall: Opus out |
|---|---|---|---|---|---|---|
| 64 | 115.9 s | 72.8 s | 37% | 9.9 MB | 34.6 MB/s | 2.6 MB/s |
| 96 | 124.3 s | 81.2 s | 35% | 8.9 MB | 43.0 MB/s | 4.6 MB/s |
| 128 | 131 s | 92 s | 30% | 7.8 MB | 19-21 MB/s | 2.6-2.9 MB/s |
| 160 | 138.3 s | 95.3 s | 31% | 7.6 MB | 36.4 MB/s | 6.1 MB/s |
| 192 | 120.1 s | 75.2 s | 37% | 9.6 MB | 49.4 MB/s | 9.7 MB/s |
| 256 | 122.8 s | 78.5 s | 36% | 9.2 MB | 38.9 MB/s | 10.0 MB/s |

Wall figures here are one run each. The 128 row's wall came from a busier stretch; its real
sync below reached 6.0 MB/s.

MP3 at 128 kbps, 11 threads, two runs each: CPU 64.9 and 63.3 s old, 42.6 and 42.2 s new. Wall
was 24.8 and 16.1 s old, 13.1 and 14.5 s new, or 7.2-7.9 MB/s of MP3 in. MP3 files are smaller,
so the same audio is fewer MB in.

## Build flags

libopus is built properly on every platform. A wrong flag wasn't the answer.

| Platform | Build | Optimisation | SIMD | How it was checked |
|---|---|---|---|---|
| Windows (w64devkit) | Release | `-O3` | SSE/SSE2 presumed; SSE4.1 and AVX2 by run-time detection | Build files read, objects listed, and `opus_select_arch()` returns 4 (AVX2) here |
| Linux | Release (flatpak) | `-O3` | As Windows | Configured in Docker with gcc, build files read. **Not run** |
| Android arm64 | RelWithDebInfo (Gradle's release) | `-O2` | NEON presumed, intrinsics on | Read from an existing release build's CMake files. **Not run** |
| Android armv7 (watch) | RelWithDebInfo | `-O2` | NEON by run-time detection | As above |

Trying other libopus builds on the same track, 5 rounds taking turns, thread CPU:

| libopus build | Median transcode | Range | Speed change |
|---|---|---|---|
| As shipped | 3.89 s | 3.66-4.09 | |
| No stack protector or fortify | 3.97 s | 3.78-4.06 | -2.0% |
| `OPUS_FLOAT_APPROX` | 3.91 s | 3.89-3.94 | -0.4% |
| All of it compiled for AVX2 and FMA | 3.73 s | 3.59-3.86 | +4.2% |

These are all inside or near the spread. The AVX2 build would also stop the app running on
older CPUs, so it isn't worth having.

One gap on Android: libopusenc has SSE code for the resampler but no NEON, so ARM uses plain C.
The fix cuts the arithmetic there too, and I'd expect it to help, but that's unmeasured. It
hardly matters today, since phones and watches don't encode (see below).

## A real sync against the ceiling

128 kbps, cold cache, all from traces. Three 24-file runs and two 96-file runs of each build,
old and new taking turns. All received every file with none corrupt.

| Run | Old wall | New wall | Old CPU | New CPU |
|---|---|---|---|---|
| 24 files | 32.5-33.1 s | 21.1-23.8 s | 134 s | 90 s |
| 96 files, round 1 | 148.2 s | 85.6 s | 537 s | 366 s |
| 96 files, round 2 | 102.1 s | 64.3 s | 535 s | 360 s |

| 96-file run, new build | Round 1 | Round 2 |
|---|---|---|
| FLAC in | 33.7 MB/s | 44.8 MB/s |
| Opus out | 4.5 MB/s | 6.0 MB/s |
| Speed | 253x | 336x |

What the traces answer:

- **How many workers run?** 11, as designed (logical cores less one). They averaged 10.0-10.1
  busy across the 96-file runs.
- **How far ahead do they get?** As far as they can. The receiver asks for everything in its
  `PREPARE_REQ` up front, in fetch order, so nothing limits the queue. More encode-ahead
  wouldn't help: the queue never ran dry until the end of the sync.
- **Does the sender stall on the encoder?** Yes, for most of the sync: 45.6-66.9 s of waiting
  and 17.3-17.5 s of sending in the new 96-file runs (84-119 s of waiting in the old ones).
- **Does the encoder stall on anything?** No. The sync used about the same CPU as the bare
  encoder doing the same work (about 363 s, against 4 x 92 s), so encryption, the database and
  the receiver add almost nothing, and there's no sign of workers blocking on anything.
- **Is any file encoded twice?** No. The 96-file traces show one start per file, by full uid.
  The first 24-file run, checked on disk, had 24 starts, 24 cache folders and no part files.
- **What's cached?** `transcode-cache/<file_uid>/opus-<kbps>.opus` with a sidecar holding its
  hash, up to 8 GiB, least recently used goes first. This library at 128 kbps would be about
  3.3 GB, so it fits. Another device at the same bitrate needs no encoding, an interrupted sync
  keeps what finished, and a new bitrate encodes everything again.
- **What does a cached sync look like?** 24 files in 1.75 s, 55 MB/s of Opus, 2 s of CPU. With
  the encoder out of the way the sender runs about ten times faster, which shows again where
  the limit is.

### Why the rate jumps around

Landed MB in each 4 s of the new 96-file sync, round 2:

```
6.8 4.3 0.0 6.7 3.8 48.8 51.6 25.5 9.4 22.3 35.9 11.9 55.2 0.0 16.8 73.6 14.2
```

Three things combine:

1. **A file can only be sent once it's finished.** `FILE_HDR` carries the size and hash of the
   whole file, so nothing goes out while it's encoding.
2. **Files go in order, but encodes run side by side.** 11 start together and share the CPU, so
   they finish around the same time. The sender waits for the next file in line (17.6 s for one
   file in an old trace, with 10 finished files stuck behind it), then sends the whole batch
   in a moment (49 MB in 2 s in that trace), and then waits for the next batch.
3. **The screen only updates at file boundaries.** `progress()` runs once a file is done, so
   the numbers step even when bytes are flowing smoothly.

So the encoder's output is steady and the jumps are in how it's delivered. The average is
what counts, and it's the encoder's speed. The fix makes the gaps shorter but doesn't remove
them, and I didn't try to, since evening it out wouldn't finish a sync any sooner. The one
real cost is at the edges: nothing arrives until the first files finish, and the last file
encodes alone (3.6 s in the first old trace).

## Phones and watches

- **As receivers**, the PC does the encoding, so the numbers above apply. Earlier testing put
  this PC's Wi-Fi link to the phone at about 9 MB/s. At 128 kbps the encoder (6 MB/s under load
  here, perhaps 12 MB/s idle) is around that level, so either could be the limit. At
  192 and 256 kbps the encoder reached 10 MB/s even with other work running, so the Wi-Fi link
  is likely to be the limit. I didn't test this on a phone.
- **As senders**, they don't encode at all right now: `on_power` and `can_transcode` are only
  true for desktops (session.c calls them placeholders). If that changes, a phone's encoding
  speed will need measuring.

## Choices left to you

I left these alone because they trade quality or change output, and the brief was to keep both.

- **Complexity 7 at 128 and 160 kbps.** This is the biggest speedup left. Memory-only encodes
  of one track went from 75x to 101x at 128 kbps, and from 66x to 85x at 160. It turns off
  libopus's theta RDO and second MDCT (the `complexity >= 8` checks), and 192 and 256 kbps
  already run this way. It should be safe for existing files, since transcodes aren't identified
  by settings, but it's a quality call that wants a listening test.
- **Constrained VBR.** The docs say VBR, but the app never clears libopus's
  `OPUS_SET_VBR_CONSTRAINT` default of 1. Output lands almost exactly on the nominal rate
  whatever the content: 129 kbps for both pure tones and white noise. That's a size and quality
  matter rather than speed. Real VBR might sound better for the same average, but file sizes
  would vary more, and the size budget's estimates assume they don't.
- **Showing progress within a file.** Updating `progress()` during a send would make the screen
  smoother. It wouldn't make syncs faster.

## How far to trust these numbers

- **The machine was never idle.** It was 90-100% busy throughout, with other work holding 6.6-11.3
  cores. Wall times and MB/s are what was seen under that load, so a quiet PC should do better.
  I checked again at the end and it was still at 100%.
- **The idle estimate** is 11 workers times the measured rate per CPU-second: 7.9 MB in and 1.08
  MB out. Those rates were measured with the neighbouring hyperthreads busy, which is what 11
  workers would do to each other, so the sum should be fair. It assumes nothing else needs the
  CPU and that memory bandwidth and turbo clocks hold up. Treat it as a guide until someone
  runs `manual_opus_bench pool` on an idle PC.
- **Per-thread figures are not single-core speed.** They were measured with sibling threads
  busy, so one thread on an idle PC should be faster.
- **Run to run,** CPU varied about 2-5% within a build, while wall varied by up to 45% (96-file
  syncs at 102-148 s old).
- **Loopback** has no network limit, so these syncs show the encoder's ceiling. They aren't a
  prediction for Wi-Fi.
- **The library sample** leans towards dense electronic music. Quieter or sparser material may
  encode a little faster.
- **Not run:** Android and Linux builds with the new flag. Linux compiled `resample.c` with it
  and got the direct path; Android wasn't built.

## Reproducing

```sh
cmake --preset windows-w64devkit-release -B build/r2 \
  -DFETCHCONTENT_BASE_DIR=<a folder of your own holding copies of the *-src folders> \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build/r2 -j 4 --target manual_opus_bench manual_opus_sync

manual_opus_bench info
manual_opus_bench resq
manual_opus_bench stages <file> 128 8 3
manual_opus_bench pool <dir of tNN/track.flac> 11 128 <scratch cache> 1
KITHSONG_SYNC_TRACE=1 manual_opus_sync <dir of tNN/> <scratch root> 128 96 cold 2>&1 | cat > trace.txt
```

**Use your own `FETCHCONTENT_BASE_DIR` for anything you time.** A shared one shares the
dependencies' object files too (libopus's land in `<base>/opus-build`), so a debug build in
another tree can end up inside a "release" benchmark with no warning.

The sampling profile (`prof`) records addresses for mapping with `nm`, on Windows only. It
samples about every 15.6 ms, so its percentages are rough.
