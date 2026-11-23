# Changelog

What has been changed in each release of Kithsong, newest first.

## 1.0.0

### All Platforms

- Fixes an issue where devices who want transcoded files are prepared for even when they're full
- Redesigns the sync page

### Desktop

- Now dismisses "<device> has cancelled the sync session" banner when a sync session starts, and allows dismissing it.

### Android

- Fixes some issues with the Sync UI
    - The Sync Now button would become unresponsive after cancelling
    - Shows more information about sync state in the pill
    - Shows the Sync pill in device pages
- Returning to the app from the app switcher now triggers a sync rescan of network devices so the UI doesn't get stuck
- The transcoding banner now shows who for and how many tracks
- Adds the ability to export the library from app storage
- Adjusts the header to remove the need for a meatball menu
- Adds the ability to change the size of the album grid
- Folds away artists not credited for an entire release in the library