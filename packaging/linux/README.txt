Kithsong for Linux
==================

https://kithso.ng

This is the plain build of Kithsong, for people who don't want the Flatpak. It doesn't
update by itself. To update, download the new tarball and unpack it over the old one.


Running it
----------

Run bin/kithsong from this folder. It can live anywhere.

To add it to your app menu, unpack it into ~/.local instead:

    tar -xzf kithsong-<version>-linux-<arch>.tar.gz --strip-components=1 \
        --exclude='*/README.txt' -C ~/.local

The menu entry runs "kithsong", so ~/.local/bin has to be on your PATH.


What your system needs
----------------------

Built on Ubuntu 24.04, so it needs that system or a newer one. It won't start on anything
older than these versions:

    GTK         4.14
    libadwaita  1.5
    GLib        2.80
    glibc       2.38
    libstdc++   from GCC 13

Most desktops have all of it already. If not, installing libadwaita brings in the rest:

    Debian, Ubuntu   sudo apt install libadwaita-1-0
    Fedora           sudo dnf install libadwaita
    Arch             sudo pacman -S libadwaita

The program links these libraries:

    libadwaita-1.so.0
    libgtk-4.so.1
    libgdk_pixbuf-2.0.so.0
    libgio-2.0.so.0
    libgobject-2.0.so.0
    libglib-2.0.so.0
    libstdc++.so.6
    libgcc_s.so.1
    libm.so.6
    libc.so.6

For sound it loads one of these when it starts, so it isn't linked and a missing one won't
stop it from starting. PipeWire desktops provide libpulse too.

    libpulse.so.0   package libpulse0, pulseaudio-libs or libpulse
    libasound.so.2  package libasound2, alsa-lib


What's built in
---------------

These are part of the program, so you don't need to install them, and a system update won't
change them:

    curl, Mbed TLS, the Mozilla CA certificates (from curl.se), SQLite, TagLib, utfcpp,
    miniaudio, dr_libs, stb, miniz, mdns, yyjson, Ogg, Opus, opusenc, opusfile

Their licences are in share/licenses/ng.kithso.Kithsong.
