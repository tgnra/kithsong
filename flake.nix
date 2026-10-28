{
  description = "Kithsong: cross-platform music library, player and LAN sync";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # Dev shell for the linux-gtk / linux-gtk-release presets:
      #   nix develop -c cmake --preset linux-gtk
      # TagLib, curl, Mbed TLS etc. are still fetched by CMake at configure time.
      devShells = forAllSystems (pkgs:
        let
          lib = pkgs.lib;
          # miniaudio dlopen()s its backends at runtime instead of linking them.
          audioLibs = [ pkgs.alsa-lib pkgs.libpulseaudio pkgs.pipewire ];
          # Requires.private of the GTK stack's .pc files. Nothing links them, but CMake's
          # pkg_check_modules also resolves `--static` flags and warns when they are missing.
          staticPcDeps = [
            pkgs.libsysprof-capture pkgs.pcre2 pkgs.fribidi pkgs.appstream pkgs.libthai
            pkgs.util-linux pkgs.libselinux pkgs.expat pkgs.libdeflate pkgs.lerc pkgs.xz
            pkgs.zstd pkgs.libwebp pkgs.libxdmcp pkgs.libsepol pkgs.libdatrie
          ];
          schemaDir = p: "${p}/share/gsettings-schemas/${p.name}";
        in
        {
          default = pkgs.mkShell {
            # stdenv already provides gcc; gdb is for debugging.
            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.gdb
              pkgs.glib.bin # glib-compile-schemas, gsettings
            ];
            buildInputs = [
              pkgs.gtk4
              pkgs.libadwaita
            ] ++ staticPcDeps ++ audioLibs;

            # Nix's default _FORTIFY_SOURCE warns on -O0 Debug builds.
            hardeningDisable = [ "fortify" ];

            # What wrapGAppsHook4 would do for an installed app: schemas, icons, pixbuf
            # loaders and the dconf GSettings backend.
            shellHook = ''
              export LD_LIBRARY_PATH="${lib.makeLibraryPath audioLibs}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
              export XDG_DATA_DIRS="${schemaDir pkgs.gtk4}:${schemaDir pkgs.gsettings-desktop-schemas}:${pkgs.adwaita-icon-theme}/share:${pkgs.hicolor-icon-theme}/share''${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
              export GIO_EXTRA_MODULES="${pkgs.dconf.lib}/lib/gio/modules''${GIO_EXTRA_MODULES:+:$GIO_EXTRA_MODULES}"
              export GDK_PIXBUF_MODULE_FILE="${pkgs.librsvg}/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
            '';
          };
        });
    };
}
