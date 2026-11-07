---
title: Installing
order: 1
description: Where to get Kithsong for Windows, Linux and Android.
---

## Windows

Windows 10 version **1903** or newer is required to run this application.

The recommended way to install Kithsong on Windows is using the [Microsoft Store](ms-windows-store://pdp/?productid=9PFD50NWXCT5). This will give you automatic updates and the firewall is configured automatically. If you don't want to use the Microsoft Store app, you can install with Winget using the following command:

```powershell
winget install 9PFD50NWXCT5 --source msstore
```

Alternatively, there are portable zips with a .exe built for each release, available [here on Github](https://github.com/tgnra/kithsong/releases)

## Linux

The recommended way to install Kithsong is using a Flatpak. See installation instructions for Flatpak for your distro [at Flathub's website](https://flathub.org/en/setup)

Then run this command:

```bash
flatpak install --user https://kithso.ng/kithsong.flatpakref
```

Kithsong then updates with `flatpak update` or your software centre.

Alternatively, there is a `.flatpak` bundle, and a tarball built for each release, available [here on GitHub](https://github.com/tgnra/kithsong/releases). It doesn't update by itself.

## Android

TODO