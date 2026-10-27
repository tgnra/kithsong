# UI strings

Every user-visible string of every platform is in `en.json`, with translations in
`<lang>.json` next to it. The build turns them into each platform's own form; nothing
generated is checked in. `MERGES.md` records how the platforms' original strings were
merged.

## Format

```json
{
  "shared":  { "cancel": "Cancel" },
  "linux":   { "nothing_playing": "Nothing playing" },
  "windows": { "col_year": "Year" },
  "android": { "back": "Back" }
}
```

- **Sections**: `shared` for anything used by more than one platform or by the core,
  `linux`, `windows`, `android` for the rest. Keys are snake_case, never reused across
  sections, and stable: renaming one means changing its users.
- **Values** are a string, or an object with:
  - `text`: the string;
  - `note`: a comment for translators (also written into Android's strings.xml);
  - `translatable`: `false` for names like `app_name`, which translations must leave out;
  - plural forms instead of `text`, by CLDR category: `zero`, `one`, `two`, `few`,
    `many`, `other`. `other` is required; English needs `one` and `other`.
- **Placeholders**: `{0}`, `{1}`, … numbered from 0 with no gaps, in any order. `{0:d}`
  marks a number (Android needs to know: `%1$d` rather than `%1$s`). `{{` and `}}` are
  literal braces. A plural's count is passed as an argument like any other, usually `{0:d}`.
- **Mnemonics**: `_` before the access-key letter, `S_kip`; `__` is a literal underscore.
  Platforms that don't use mnemonics drop the marker.

The generator, `cmake/gen_strings.cmake`, rejects unknown sections or fields, keys that
aren't snake_case or appear twice, malformed or gapped placeholders, plurals without
`other`, and translations of keys English doesn't have or with placeholders it doesn't take.

## Adding a string

Add it to `en.json` in the section that uses it (move it to `shared` once a second
platform does), with a `note` if the context isn't obvious from the key and text. Before
adding, look for an existing string that says the same thing: similar strings are merged
into one. Then use it as below; the build picks it up.

## Adding a language

Create `<lang>.json` named by its BCP 47 tag (`fr.json`, `pt-BR.json`) with the same
shape, translating any subset of the keys: whatever is missing falls back to the parent
language (`fr-CA` → `fr`), then English. If the language's plural rules differ from
English's, add a case to `plural_category()` in `core/src/util/i18n.c`.

## How each platform uses them

- **Core and desktops (C)**: `core/CMakeLists.txt` runs the generator (`MODE=c`) into the
  build directory: `mc_strings_gen.h` has `enum mc_str_id`, one id per string of every
  section (`MC_STR_<KEY>` for shared ones, `MC_STR_<SECTION>_<KEY>` for the others,
  alphabetical within a section), and `mc_strings_gen.c` a table per language. The API is
  in `core/include/musiccore/i18n.h`: `mc_strings_set_locale()`, `mc_str()`,
  `mc_str_mnemonic()`, `mc_str_plural()` and `mc_format()`, all UTF-8.
- **Android**: `android/app/build.gradle.kts` runs the generator (`MODE=android`) with the
  SDK's CMake before resources are merged, into `app/build/generated/`: the shared and
  android sections become `values/strings.xml` and `values-<lang>/strings.xml`, with
  `{0}` as `%1$s`, mnemonics dropped and Android's escaping applied. Code uses
  `R.string.<key>` and `R.plurals.<key>` as usual. Strings the core produces itself (import
  errors, status reasons) come through the JNI in the language set with
  `mc_strings_set_locale()`.
