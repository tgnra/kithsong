// Tag reading/writing via TagLib's C++ API, exposed to the rest of core as plain C.
// We don't use TagLib's own C bindings: they take char* paths, which on Windows go through
// the ANSI code page and break on non-ASCII file names.
#include "musiccore/tagger.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <audioproperties.h>
#include <fileref.h>
#include <tpropertymap.h>

namespace {

char *dup_utf8(const TagLib::String &s) {
  std::string utf8 = s.to8Bit(true);
  char *out = static_cast<char *>(std::malloc(utf8.size() + 1));
  if (out) std::memcpy(out, utf8.c_str(), utf8.size() + 1);
  return out;
}

// First value of a property, or NULL.
char *prop_str(const TagLib::PropertyMap &props, const char *key) {
  auto it = props.find(key);
  if (it == props.end() || it->second.isEmpty()) return nullptr;
  return dup_utf8(it->second.front());
}

// Parses "3" or "3/12" into number and total.
void prop_pair(const TagLib::PropertyMap &props, const char *key, int *num, int *total) {
  auto it = props.find(key);
  if (it == props.end() || it->second.isEmpty()) return;
  std::string v = it->second.front().to8Bit(true);
  *num = std::atoi(v.c_str());
  auto slash = v.find('/');
  if (slash != std::string::npos) *total = std::atoi(v.c_str() + slash + 1);
}

int prop_int(const TagLib::PropertyMap &props, const char *key) {
  auto it = props.find(key);
  if (it == props.end() || it->second.isEmpty()) return 0;
  return std::atoi(it->second.front().toCString(true));  // "2003-05-01" -> 2003
}

// Opens a file with a UTF-8 path on every platform.
TagLib::FileRef open_file(const char *path, bool read_audio) {
#ifdef _WIN32
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
  if (n <= 0) return TagLib::FileRef();
  std::wstring wide(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), n);
  return TagLib::FileRef(wide.c_str(), read_audio, TagLib::AudioProperties::Average);
#else
  return TagLib::FileRef(path, read_audio, TagLib::AudioProperties::Average);
#endif
}

void set_str(TagLib::PropertyMap &props, const char *key, const char *value) {
  if (value && *value) props.replace(key, TagLib::StringList(TagLib::String(value, TagLib::String::UTF8)));
}

void set_pair(TagLib::PropertyMap &props, const char *key, int num, int total) {
  if (num <= 0) return;
  std::string v = std::to_string(num);
  if (total > 0) v += "/" + std::to_string(total);
  props.replace(key, TagLib::StringList(TagLib::String(v, TagLib::String::UTF8)));
}

}  // namespace

extern "C" mc_status mc_tags_read(const char *path, mc_tags *out) {
  if (!path || !out) return MC_ERR_INVALID_ARG;
  std::memset(out, 0, sizeof *out);

  try {
    TagLib::FileRef f = open_file(path, true);
    if (f.isNull()) return MC_ERR_FORMAT;

    TagLib::PropertyMap props = f.properties();
    out->title = prop_str(props, "TITLE");
    out->artist = prop_str(props, "ARTIST");
    out->album = prop_str(props, "ALBUM");
    out->album_artist = prop_str(props, "ALBUMARTIST");
    out->genre = prop_str(props, "GENRE");
    out->mb_trackid = prop_str(props, "MUSICBRAINZ_TRACKID");
    out->mb_releasetrackid = prop_str(props, "MUSICBRAINZ_RELEASETRACKID");
    out->mb_albumid = prop_str(props, "MUSICBRAINZ_ALBUMID");
    out->mb_artistid = prop_str(props, "MUSICBRAINZ_ARTISTID");
    out->year = prop_int(props, "DATE");
    prop_pair(props, "TRACKNUMBER", &out->track, &out->track_total);
    prop_pair(props, "DISCNUMBER", &out->disc, &out->disc_total);
    if (!out->track_total) out->track_total = prop_int(props, "TRACKTOTAL");
    if (!out->disc_total) out->disc_total = prop_int(props, "DISCTOTAL");

    if (const TagLib::AudioProperties *ap = f.audioProperties()) {
      out->duration_ms = ap->lengthInMilliseconds();
      out->bitrate_kbps = ap->bitrate();
      out->sample_rate = ap->sampleRate();
      out->channels = ap->channels();
    }
    return MC_OK;
  } catch (const std::bad_alloc &) {
    mc_tags_clear(out);
    return MC_ERR_NOMEM;
  } catch (...) {
    mc_tags_clear(out);
    return MC_ERR_FORMAT;
  }
}

extern "C" mc_status mc_tags_write(const char *path, const mc_tags *tags) {
  if (!path || !tags) return MC_ERR_INVALID_ARG;

  try {
    TagLib::FileRef f = open_file(path, false);
    if (f.isNull()) return MC_ERR_FORMAT;

    TagLib::PropertyMap props = f.properties();
    set_str(props, "TITLE", tags->title);
    set_str(props, "ARTIST", tags->artist);
    set_str(props, "ALBUM", tags->album);
    set_str(props, "ALBUMARTIST", tags->album_artist);
    set_str(props, "GENRE", tags->genre);
    set_str(props, "MUSICBRAINZ_TRACKID", tags->mb_trackid);
    set_str(props, "MUSICBRAINZ_RELEASETRACKID", tags->mb_releasetrackid);
    set_str(props, "MUSICBRAINZ_ALBUMID", tags->mb_albumid);
    set_str(props, "MUSICBRAINZ_ARTISTID", tags->mb_artistid);
    if (tags->year > 0) set_str(props, "DATE", std::to_string(tags->year).c_str());
    set_pair(props, "TRACKNUMBER", tags->track, tags->track_total);
    set_pair(props, "DISCNUMBER", tags->disc, tags->disc_total);

    f.setProperties(props);
    return f.save() ? MC_OK : MC_ERR_IO;
  } catch (const std::bad_alloc &) {
    return MC_ERR_NOMEM;
  } catch (...) {
    return MC_ERR_IO;
  }
}

extern "C" void mc_tags_clear(mc_tags *tags) {
  if (!tags) return;
  char **strs[] = {&tags->title,      &tags->artist,     &tags->album,
                   &tags->album_artist, &tags->genre,    &tags->mb_trackid,
                   &tags->mb_releasetrackid, &tags->mb_albumid, &tags->mb_artistid};
  for (char **s : strs) std::free(*s);
  std::memset(tags, 0, sizeof *tags);
}
