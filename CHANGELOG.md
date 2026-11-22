# Changelog

What has been changed in each release of Kithsong, newest first.

## 1.0.0

### All Platforms

### Desktop

- Now dismisses "<device> has cancelled the sync session" banner when a sync session starts, and allows dismissing it.

### Android

- Fixes some issues with the Sync UI
    - The Sync Now button would become unresponsive after cancelling
    - Shows more information about sync state in the pill
    - Shows the Sync pill in device pages
- Returning to the app from the app switcher now triggers a sync rescan of network devices so the UI doesn't get stuck
