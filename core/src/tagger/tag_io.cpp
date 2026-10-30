// Tag reading/writing via TagLib's C++ API, exposed to the rest of core as plain C.
// We don't use TagLib's own C bindings: they take char* paths, which on Windows go through
// the ANSI code page and break on non-ASCII file names.
#include "musiccore/tagger.h"
#include "tagger/tag_io.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" {
#include "util/platform.h"
}
#endif

#include <audioproperties.h>
#include <fileref.h>
#include <flac/flacproperties.h>
#include <riff/aiff/aiffproperties.h>
#include <riff/wav/wavproperties.h>
#include <tpropertymap.h>
#include <tvariant.h>

namespace {

// Bits per sample of the lossless formats that have one; 0 for the rest.
int bits_per_sample(const TagLib::AudioProperties *ap) {
  if (auto *p = dynamic_cast<const TagLib::FLAC::Properties *>(ap)) return p->bitsPerSample();
  if (auto *p = dynamic_cast<const TagLib::RIFF::WAV::Properties *>(ap)) return p->bitsPerSample();
  if (auto *p = dynamic_cast<const TagLib::RIFF::AIFF::Properties *>(ap)) return p->bitsPerSample();
  return 0;
}

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

// Every value of a property, joined with '\n'; NULL if it has none.
char *prop_list(const TagLib::PropertyMap &props, const char *key) {
  auto it = props.find(key);
  if (it == props.end() || it->second.isEmpty()) return nullptr;
  std::string joined;
  for (const TagLib::String &v : it->second) {
    std::string utf8 = v.to8Bit(true);
    if (utf8.empty() || utf8.find('\n') != std::string::npos) continue;  // can't be told apart
    if (!joined.empty()) joined += '\n';
    joined += utf8;
  }
  if (joined.empty()) return nullptr;
  char *out = static_cast<char *>(std::malloc(joined.size() + 1));
  if (out) std::memcpy(out, joined.c_str(), joined.size() + 1);
  return out;
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

// Opens a file with a UTF-8 path on every platform, of any length on Windows.
TagLib::FileRef open_file(const char *path, bool read_audio) {
#ifdef _WIN32
  wchar_t *wide = mc_win_path(path);
  if (!wide) return TagLib::FileRef();
  // TagLib::FileName keeps its own copy of the name.
  TagLib::FileRef f(wide, read_audio, TagLib::AudioProperties::Average);
  std::free(wide);
  return f;
#else
  return TagLib::FileRef(path, read_audio, TagLib::AudioProperties::Average);
#endif
}

// NULL leaves the tag alone; "" removes it.
void set_str(TagLib::PropertyMap &props, const char *key, const char *value) {
  if (!value) return;
  if (!*value) props.erase(key);
  else props.replace(key, TagLib::StringList(TagLib::String(value, TagLib::String::UTF8)));
}

// A '\n'-separated list as the property's values; NULL leaves it alone, "" removes it.
void set_list(TagLib::PropertyMap &props, const char *key, const char *value) {
  if (!value) return;
  TagLib::StringList values;
  for (const char *p = value; *p;) {
    const char *nl = std::strchr(p, '\n');
    size_t n = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
    if (n) values.append(TagLib::String(std::string(p, n), TagLib::String::UTF8));
    p += n + (nl ? 1 : 0);
  }
  if (values.isEmpty()) props.erase(key);
  else props.replace(key, values);
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
    out->mb_albumartistid = prop_str(props, "MUSICBRAINZ_ALBUMARTISTID");
    out->mb_releasegroupid = prop_str(props, "MUSICBRAINZ_RELEASEGROUPID");
    out->artists = prop_list(props, "ARTISTS");
    out->album_artists = prop_list(props, "ALBUMARTISTS");
    out->mb_artistids = prop_list(props, "MUSICBRAINZ_ARTISTID");
    out->mb_albumartistids = prop_list(props, "MUSICBRAINZ_ALBUMARTISTID");
    for (const char *key : {"LABEL", "ORGANIZATION", "PUBLISHER"})
      if (!out->label) out->label = prop_str(props, key);
    for (const char *key : {"CATALOGNUMBER", "CATALOGID", "CATALOG", "LABELNO"})
      if (!out->catalog) out->catalog = prop_str(props, key);
    out->year = prop_int(props, "DATE");
    out->original_year = prop_int(props, "ORIGINALDATE");
    prop_pair(props, "TRACKNUMBER", &out->track, &out->track_total);
    prop_pair(props, "DISCNUMBER", &out->disc, &out->disc_total);
    if (!out->track_total) out->track_total = prop_int(props, "TRACKTOTAL");
    if (!out->disc_total) out->disc_total = prop_int(props, "DISCTOTAL");

    if (const TagLib::AudioProperties *ap = f.audioProperties()) {
      out->duration_ms = ap->lengthInMilliseconds();
      out->bitrate_kbps = ap->bitrate();
      out->sample_rate = ap->sampleRate();
      out->channels = ap->channels();
      out->bits_per_sample = bits_per_sample(ap);
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
    if (tags->mb_artistids) set_list(props, "MUSICBRAINZ_ARTISTID", tags->mb_artistids);
    else set_str(props, "MUSICBRAINZ_ARTISTID", tags->mb_artistid);
    if (tags->mb_albumartistids) set_list(props, "MUSICBRAINZ_ALBUMARTISTID", tags->mb_albumartistids);
    else set_str(props, "MUSICBRAINZ_ALBUMARTISTID", tags->mb_albumartistid);
    set_list(props, "ARTISTS", tags->artists);
    set_list(props, "ALBUMARTISTS", tags->album_artists);
    set_str(props, "MUSICBRAINZ_RELEASEGROUPID", tags->mb_releasegroupid);
    if (tags->year > 0) set_str(props, "DATE", std::to_string(tags->year).c_str());
    if (tags->original_year > 0)
      set_str(props, "ORIGINALDATE", std::to_string(tags->original_year).c_str());
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

extern "C" mc_status mc_tags_read_picture(const char *path, unsigned char **data, size_t *len) {
  if (!path || !data || !len) return MC_ERR_INVALID_ARG;
  *data = nullptr;
  *len = 0;

  try {
    TagLib::FileRef f = open_file(path, false);
    if (f.isNull()) return MC_ERR_FORMAT;

    TagLib::List<TagLib::VariantMap> pictures = f.complexProperties("PICTURE");
    const TagLib::VariantMap *best = nullptr;
    for (const TagLib::VariantMap &p : pictures) {
      if (p.value("data").value<TagLib::ByteVector>().isEmpty()) continue;
      if (!best) best = &p;
      if (p.value("pictureType").value<TagLib::String>() == "Front Cover") {
        best = &p;
        break;
      }
    }
    if (!best) return MC_ERR_NOT_FOUND;

    TagLib::ByteVector bytes = best->value("data").value<TagLib::ByteVector>();
    unsigned char *copy = static_cast<unsigned char *>(std::malloc(bytes.size()));
    if (!copy) return MC_ERR_NOMEM;
    std::memcpy(copy, bytes.data(), bytes.size());
    *data = copy;
    *len = bytes.size();
    return MC_OK;
  } catch (const std::bad_alloc &) {
    return MC_ERR_NOMEM;
  } catch (...) {
    return MC_ERR_FORMAT;
  }
}

extern "C" void mc_tags_clear(mc_tags *tags) {
  if (!tags) return;
  char **strs[] = {&tags->title,      &tags->artist,     &tags->album,
                   &tags->album_artist, &tags->genre,    &tags->mb_trackid,
                   &tags->mb_releasetrackid, &tags->mb_albumid, &tags->mb_artistid,
                   &tags->mb_albumartistid, &tags->mb_releasegroupid, &tags->artists,
                   &tags->album_artists, &tags->mb_artistids, &tags->mb_albumartistids,
                   &tags->label, &tags->catalog};
  for (char **s : strs) std::free(*s);
  std::memset(tags, 0, sizeof *tags);
}
