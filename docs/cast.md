# Google Cast

> Status: only the Android phone app casts, using Google's Cast SDK
> (`android/app/src/main/kotlin/ng/kithso/android/cast/`). The desktops don't cast, and the
> core has no Cast code. Change this document first, then the code.

## Goals

- Play from the phone to a Chromecast, a TV with Cast built in, or a Cast speaker. The phone
  still decides what plays:
  - the queue, shuffle and up next work the same as they do locally
  - plays still count and scrobble
  - you can set the device's volume
- We don't have our own receiver app. Google's Default Media Receiver (app id `CC1AD845`)
  plays a URL.

Not goals:

- casting from the desktops
- speaker groups set up in Google Home (to us they're ordinary devices, so they should work,
  but they're untested)
- casting video

## How it works

The phone sends the device one constant, live stream. It's a single WAV URL served from the
phone (`CastServer.kt`) for the whole cast, and it carries whatever the phone's player is
playing. The receiver doesn't know about tracks. It doesn't show track titles, and it has no
next or previous controls. Skipping, seeking, pausing and the queue are all handled on the
phone, which changes what goes down the stream. Plays are counted on the phone, the same as
without Cast.

The Cast SDK handles the rest of the session:

- picking a device
- the device's volume
- ending the session, which stops the receiver

Its own notification and media session are turned off, because the app's media session
already shows what's playing.
