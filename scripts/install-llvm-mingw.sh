#!/bin/sh
# The Linux counterpart of install-llvm-mingw.ps1: downloads llvm-mingw's Linux build of the
# release pinned in cmake/llvm-mingw.pin and unpacks it under the given folder (~/Tools by
# default), in a folder named after the release, where cmake/llvm-mingw.cmake finds it. With it
# the windows-x64-cross preset builds the Windows app here; Wine, if installed, runs its tests.
# Prints the folder.
#
#   scripts/install-llvm-mingw.sh [<parent folder>]
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$HOME/Tools}
release=$(head -n 1 "$root/cmake/llvm-mingw.pin" | tr -d '[:space:]')
case $(uname -m) in
  x86_64) arch=x86_64 ;;
  *) echo "No pinned llvm-mingw for a $(uname -m) Linux machine." >&2; exit 1 ;;
esac
sha256=$(awk -v k="ubuntu-22.04-$arch" '$1 == k { print $2 }' "$root/cmake/llvm-mingw.pin")
name=llvm-mingw-$release-ucrt-ubuntu-22.04-$arch
target=$dir/$name
if [ -x "$target/bin/x86_64-w64-mingw32-clang" ]; then
  echo "$target"
  exit 0
fi

# Unpacked in a new folder next to the target and renamed at the end, so an interrupted run
# never leaves a half-unpacked toolchain under the final name.
mkdir -p "$dir"
work=$(mktemp -d "$dir/$name.tmp-XXXXXX")
trap 'rm -rf "$work"' EXIT
curl -fsSL -o "$work/$name.tar.xz" "https://github.com/mstorsjo/llvm-mingw/releases/download/$release/$name.tar.xz"
echo "$sha256  $work/$name.tar.xz" | sha256sum -c --quiet -
tar -xf "$work/$name.tar.xz" -C "$work"
mv "$work/$name" "$target"
echo "$target"
