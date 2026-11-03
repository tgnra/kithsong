#!/usr/bin/env bash
# Writes the app icon, the wordmark and the Android drawables from one definition of the mark
# and its sky. Run by hand after editing; nothing in the build calls it. Then run render.py
# for the PNGs and kithsong.ico.
#
#   bash packaging/icons/generate.sh
#
# The mark: a sun of song rising over two lines of home ground. The sky behind it is the
# site's glows (site/assets/style.css) laid out as a sunrise: purple overhead, orange and
# amber at the horizon behind the sun, green ground below. Only gradients with alpha stops,
# no blur, so the same sky is an Android vector drawable.
#
# The wordmark is "Kithsong" in Merienda Bold (Eduardo Tunni, SIL OFL 1.1), outlined by
# wordmark.py into wordmark-paths.txt so nothing needs the font. Outlines drawn from a font
# are artwork rather than Font Software under the OFL, so nothing ships with them.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
APP_RES=$ROOT/android/app/src/main/res
WEAR_RES=$ROOT/android/wear/src/main/res
SITE=$ROOT/site/static

MARK="#FFF4E4"          # the sun and the ground lines
BASE="#3A2470"          # under the glows
# The glows in drawing order, in fractions of the square: "R colour cx cy r", a radial glow
# from full colour at the centre to clear at r; "L colour y0 y1", a level band from clear at
# y0 to full colour at y1, so the horizon stays flat. The sun's centre is at 0.52 of the
# adaptive icon and its ground lines at 0.59 and 0.66.
GLOWS=(
  "R #5B3FA8 0.50 -0.10 0.95"  # purple overhead
  "R #E8693A 0.50 0.60 0.62"   # orange along the horizon
  "R #FFB547 0.50 0.52 0.20"   # amber, the sun itself
  "L #2F6B45 0.56 0.92"        # green, the home ground
)
# The wordmark's "Kith" and "song", on a light and on a dark background.
WORD_LIGHT=("#1D1B20" "#A8401A")
WORD_DARK=("#F4EFF4" "#F4A06B")

{ read -r KITH; read -r SONG; } <"$HERE/wordmark-paths.txt"

# --- The mark, in a 0..100 box centred on 50,50 ---

# The three strokes as path data: the sun's disc (filled), its two rays, the ground.
SUN="M40,53 A10,10 0 0 1 60,53 Z"
RAYS="M28,53 A22,22 0 0 1 72,53 M16,53 A34,34 0 0 1 84,53"
GROUND="M14,65 H86 M30,77 H70"

svg_mark() { # <transform> <colour>
  cat <<EOF
<g transform="$1">
<path d="$SUN" fill="$2"/>
<path d="$RAYS $GROUND" fill="none" stroke="$2" stroke-width="7" stroke-linecap="round"/>
</g>
EOF
}
vd_mark() { # <tx> <ty> <scale> <colour> <indent>
  local i=$5
  cat <<EOF
$i<group android:translateX="$1" android:translateY="$2" android:scaleX="$3" android:scaleY="$3">
$i    <path android:fillColor="$4" android:pathData="$SUN" />
$i    <path android:strokeColor="$4" android:strokeWidth="7" android:strokeLineCap="round"
$i        android:pathData="$RAYS $GROUND" />
$i</group>
EOF
}

# --- The sky, as SVG over a box and as vector drawable paths in 108 units ---

svg_sky_defs() {
  local i=0 kind c a b r
  for g in "${GLOWS[@]}"; do
    read -r kind c a b r <<<"$g"
    if [ "$kind" = R ]; then
      echo "<radialGradient id=\"g$i\" cx=\"$a\" cy=\"$b\" r=\"$r\"><stop offset=\"0\" stop-color=\"$c\"/><stop offset=\"1\" stop-color=\"$c\" stop-opacity=\"0\"/></radialGradient>"
    else
      echo "<linearGradient id=\"g$i\" x1=\"0\" y1=\"$a\" x2=\"0\" y2=\"$b\"><stop offset=\"0\" stop-color=\"$c\" stop-opacity=\"0\"/><stop offset=\"1\" stop-color=\"$c\"/></linearGradient>"
    fi
    i=$((i + 1))
  done
}
svg_sky() { # <x> <y> <size> <corner radius>
  local i
  echo "<rect x=\"$1\" y=\"$2\" width=\"$3\" height=\"$3\" rx=\"$4\" fill=\"$BASE\"/>"
  for i in "${!GLOWS[@]}"; do
    echo "<rect x=\"$1\" y=\"$2\" width=\"$3\" height=\"$3\" rx=\"$4\" fill=\"url(#g$i)\"/>"
  done
}

px() { awk -v f="$1" 'BEGIN { printf "%g", f * 108 }'; }
vd_sky() { # <corner radius in 108 units> <indent>
  local r=$1 i=$2 shape kind c a b gr
  if [ "$r" = 0 ]; then
    shape="M0,0h108v108h-108z"
  else
    local e=$((108 - r))
    shape="M$r,0H$e A$r,$r 0 0 1 108,$r V$e A$r,$r 0 0 1 $e,108 H$r A$r,$r 0 0 1 0,$e V$r A$r,$r 0 0 1 $r,0 Z"
  fi
  echo "$i<path android:fillColor=\"$BASE\" android:pathData=\"$shape\" />"
  for g in "${GLOWS[@]}"; do
    read -r kind c a b gr <<<"$g"
    c=${c#\#}
    if [ "$kind" = R ]; then
      local grad="android:type=\"radial\" android:centerX=\"$(px "$a")\" android:centerY=\"$(px "$b")\" android:gradientRadius=\"$(px "$gr")\""
      local from="#FF$c" to="#00$c"
    else
      local grad="android:type=\"linear\" android:startX=\"54\" android:startY=\"$(px "$a")\" android:endX=\"54\" android:endY=\"$(px "$b")\""
      local from="#00$c" to="#FF$c"
    fi
    cat <<EOF
$i<path android:pathData="$shape">
$i    <aapt:attr name="android:fillColor">
$i        <gradient $grad>
$i            <item android:offset="0" android:color="$from" />
$i            <item android:offset="1" android:color="$to" />
$i        </gradient>
$i    </aapt:attr>
$i</path>
EOF
  done
}

vd_open() { # <width dp> <height dp> <viewport w> <viewport h> <comment>
  cat <<EOF
<?xml version="1.0" encoding="utf-8"?>
<!-- $5
     Generated by packaging/icons/generate.sh: edit that, not this. -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt"
    android:width="$1dp"
    android:height="$2dp"
    android:viewportWidth="$3"
    android:viewportHeight="$4">
EOF
}

# --- Desktop icon: Windows .ico (via render.py), Linux hicolor, the site's favicon ---

# The tile is 112 of 128 with the current icon's margin; the mark is 0.76 of the tile.
icon_svg() {
  cat <<EOF
<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
<!-- The app icon. Generated by generate.sh (edit that, then run render.py for the Windows
     .ico and the Linux PNGs). Keep <svg> as the very first bytes: the Flatpak CI image's
     gdk-pixbuf recognises an SVG only by that, and flatpak build-export rejects the icon
     otherwise. -->
<defs>
$(svg_sky_defs)
</defs>
$(svg_sky 8 8 112 26)
$(svg_mark "translate(26 26) scale(0.76)" "$MARK")
</svg>
EOF
}
icon_svg >"$HERE/kithsong.svg"
cp "$HERE/kithsong.svg" "$SITE/favicon.svg"

# --- Wordmark: the tile, then the name. 352 x 100 ---

wordmark_svg() { # <kith colour> <song colour>
  cat <<EOF
<svg xmlns="http://www.w3.org/2000/svg" width="352" height="100" viewBox="0 0 352 100">
<!-- The Kithsong wordmark. Generated by packaging/icons/generate.sh. -->
<defs>
$(svg_sky_defs)
</defs>
$(svg_sky 10 10 80 18.5)
$(svg_mark "translate(22.85 22.85) scale(0.543)" "$MARK")
<g transform="translate(108 75) scale(0.054)">
<path fill="$1" d="$KITH"/>
<path fill="$2" d="$SONG"/>
</g>
</svg>
EOF
}
wordmark_svg "${WORD_LIGHT[@]}" >"$HERE/kithsong-wordmark.svg"
wordmark_svg "${WORD_DARK[@]}" >"$HERE/kithsong-wordmark-dark.svg"
cp "$HERE/kithsong-wordmark.svg" "$SITE/logo.svg"
cp "$HERE/kithsong-wordmark-dark.svg" "$SITE/logo-dark.svg"

# --- Android: the adaptive icon's layers, for the phone and the watch ---

# The foreground sits in the 66dp safe circle: the mark at 0.65 is 65dp across.
for res in "$APP_RES" "$WEAR_RES"; do
  mkdir -p "$res/drawable"
  {
    vd_open 108 108 108 108 "The launcher icon's sky: the adaptive icon's background, and the launch screen's."
    vd_sky 0 "    "
    echo "</vector>"
  } >"$res/drawable/ic_launcher_background.xml"
  {
    vd_open 108 108 108 108 "The launcher icon's mark, inside the adaptive icon's 66dp safe zone."
    vd_mark 21.5 21.5 0.65 "$MARK" "    "
    echo "</vector>"
  } >"$res/drawable/ic_launcher_foreground.xml"
  {
    vd_open 108 108 108 108 "The themed icon (Android 13 on): the mark alone, which Android colours itself."
    vd_mark 21.5 21.5 0.65 "#FFFFFF" "    "
    echo "</vector>"
  } >"$res/drawable/ic_launcher_monochrome.xml"
done

# Android 7.0 and 7.1, before adaptive icons: the same layers in a circle. The 72dp that an
# adaptive icon's mask shows (18..90) fills the 44dp circle.
mkdir -p "$APP_RES/mipmap"
{
  vd_open 48 48 48 48 "Android 7.0 and 7.1, before adaptive icons: the adaptive icon's layers in a circle."
  cat <<EOF
    <group>
        <clip-path android:pathData="M24,2a22,22 0,1 1,0 44a22,22 0,1 1,0 -44z" />
        <group android:translateX="-9" android:translateY="-9" android:scaleX="0.6111" android:scaleY="0.6111">
$(vd_sky 0 "            ")
$(vd_mark 21.5 21.5 0.65 "$MARK" "            ")
        </group>
    </group>
</vector>
EOF
} >"$APP_RES/mipmap/ic_launcher.xml"

# The status bar icon: the mark alone, 22dp across, as Android draws only its shape.
{
  vd_open 24 24 24 24 "The status bar icon for playback, import and sync: white on transparent, as Android draws only its shape."
  vd_mark -1.924 -1.37 0.2785 "#FFFFFF" "    "
  echo "</vector>"
} >"$APP_RES/drawable/ic_notification.xml"

# The header's and About's wordmark, ic_wordmark: drawable for light, drawable-night for dark,
# as the theme follows the system. 141 x 40dp; the tile's corners match the icon's (25 of 108).
for theme in light dark; do
  if [ $theme = light ]; then word=("${WORD_LIGHT[@]}"); dir=drawable; else word=("${WORD_DARK[@]}"); dir=drawable-night; fi
  mkdir -p "$APP_RES/$dir"
  {
    vd_open 140.8 40 352 100 "The Kithsong wordmark for a $theme surface: the icon's tile, then the name in Merienda Bold."
    cat <<EOF
    <group android:translateX="10" android:translateY="10" android:scaleX="0.7407" android:scaleY="0.7407">
$(vd_sky 25 "        ")
$(vd_mark 17.36 17.36 0.733 "$MARK" "        ")
    </group>
    <group android:translateX="108" android:translateY="75" android:scaleX="0.054" android:scaleY="0.054">
        <path android:fillColor="${word[0]}" android:pathData="$KITH" />
        <path android:fillColor="${word[1]}" android:pathData="$SONG" />
    </group>
</vector>
EOF
  } >"$APP_RES/$dir/ic_wordmark.xml"
done

echo "Wrote the icon, wordmark, site images and Android drawables. Now run render.py."
