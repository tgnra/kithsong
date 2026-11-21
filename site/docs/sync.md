---
title: Syncing devices
order: 3
description: How Kithsong devices find each other, pair and sync over your local network.
---

## How to sync

Other Kithsong instances on the local network will appear in the left panel on desktop and in the Sync menu on Android. They're discovered through mDNS discovery so a firewall may block this. If so, you can add a device manually with the `+` button.

Adding a device initiates a pairing request and the other device will display a six digit code that needs to be entered on the pairing device. Once paired, choose what you want to sync and press Sync.

You can selectively sync Albums, Playlists, Artists, and Album Groups. You can also select an Opus preset to sync lossy files at a lower quality to your device to save space, or choose a storage budget and the Opus preset will be set automatically (or original files if they'll fit).

## How sync works

Kithsong apps will periodically ping each other and sync once an hour if they're left open. Android will only do this in the foreground, but sync jobs will continue in the background if started. Sync jobs can be started manually using the button in the Sync window.