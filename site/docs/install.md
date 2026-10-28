---
title: Installing
order: 1
description: Where to get Kithsong for Windows, Linux and Android.
---

Kithsong will be available from each platform's store:

- **Windows:** the Microsoft Store
- **Linux:** Flathub
- **Android:** Google Play, for phones and Wear OS watches

The store listings aren't up yet.

## Building from source

Kithsong builds with CMake on Windows and Linux, and with Gradle for Android. The
[README on GitHub](https://github.com/tgnra/kithsong#readme) lists what each platform
needs. On Windows, with [w64devkit](https://github.com/skeeto/w64devkit), CMake and Ninja:

```sh
cmake --preset windows-w64devkit
cmake --build --preset windows-w64devkit
```

On Linux, with GTK 4.12 and libadwaita 1.4 or later:

```sh
cmake --preset linux-gtk
cmake --build --preset linux-gtk
```
