/* Google Cast: playing to a Chromecast, a TV with Cast built in, or a Cast speaker, on the
 * desktops, which have no Google Play Services to do it for them. The core is the whole
 * sender: it finds devices on the network (mDNS, "_googlecast._tcp"), talks the Cast
 * protocol to them, and serves them the music. See docs/cast.md.
 *
 * Using it. One handle for the app's life, from mc_cast_new(). While a device picker is open,
 * mc_cast_start_discovery(), and mc_cast_devices() on each MC_CAST_EVENT_DEVICES; a pick is
 * mc_cast_connect() with the player; "Stop casting" is mc_cast_disconnect(). The rest of the
 * UI reads mc_cast_status_get() on MC_CAST_EVENT_STATE and MC_CAST_EVENT_VOLUME: whether it's
 * connecting or casting, the device's name, and its volume, which mc_cast_set_volume() sets.
 *
 * Playback while casting. Nothing changes for the frontend: it goes on using the player
 * (kithcore/playback.h) as ever, for what to play, play and pause, next, seeking, shuffle,
 * stations and up next, and reading mc_player_status_get() for what's playing and how far
 * in. The player sends it all to the device instead of the speakers, and its status is the
 * device's, so pausing from the TV's remote or another phone shows there too. Plays still
 * count and the play log still fills in (mc_player_take_played(), mc_player_take_ended()),
 * by what the device played. Only the volume is separate: mc_player_set_volume() stays the
 * computer's own, for when the cast ends, and the device's is mc_cast_set_volume().
 *
 * Connecting moves what's playing to the device at the same place, playing or paused as it
 * was. However a cast ends (mc_cast_disconnect(), the device going away, someone stopping
 * it from elsewhere), the player is left with the same queue at the place the device had got
 * to, paused. A cast left paused (or stopped) for MC_CAST_PAUSED_MS ends by itself, and
 * playback stops. Freeing the player ends the cast.
 *
 * Formats. FLAC, MP3, Opus and WAV are served as they are; AIFF, which Cast can't play, as
 * WAV, losslessly. Each track's title, artist, album and cover come from the file itself (its
 * tags, and the cover in its folder or embedded in it).
 *
 * Threading. Every function may be called from any thread, but not from inside the callback,
 * and mc_cast_free() not concurrently with the others. The callback runs on one of the
 * handle's own threads (the discovery thread, or the connection's), never inside an mc_cast_*
 * or mc_player_* call, so frontends marshal it to their UI thread and read the state there.
 * Events that happen together may come as one. */
#ifndef KITHCORE_CAST_H
#define KITHCORE_CAST_H

#include <stdint.h>

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_player mc_player;
typedef struct mc_cast mc_cast;

/* Room for a device's id, and for its name or model, NUL included. */
#define MC_CAST_ID_LEN 64
#define MC_CAST_NAME_LEN 128
/* Room for a numeric address, NUL included. */
#define MC_CAST_ADDR_LEN 46
/* How long a cast may stay paused before it ends: three minutes, as on Android. */
#define MC_CAST_PAUSED_MS (3 * 60 * 1000)

typedef enum mc_cast_event {
  MC_CAST_EVENT_DEVICES, /* the device list changed: read it with mc_cast_devices() */
  MC_CAST_EVENT_STATE,   /* connecting, casting, idle again, or the device's name changed */
  MC_CAST_EVENT_VOLUME   /* the device's volume or mute changed */
} mc_cast_event;

typedef void (*mc_cast_cb)(void *user, mc_cast_event event);

typedef struct mc_cast_device {
  char id[MC_CAST_ID_LEN];      /* the device's own id (TXT "id"): stays the same across restarts */
  char name[MC_CAST_NAME_LEN];  /* the name its owner gave it (TXT "fn"), e.g. "Living Room TV"; "" if none */
  char model[MC_CAST_NAME_LEN]; /* what it is (TXT "md"), e.g. "Chromecast", "Google Nest Mini"; "" if unknown */
  char host[MC_CAST_ADDR_LEN];  /* its numeric address */
  uint16_t port;                /* of the Cast protocol, 8009 on most */
} mc_cast_device;

typedef struct mc_cast_device_list {
  mc_cast_device *items; /* sorted by name */
  int count;
} mc_cast_device_list;

typedef enum mc_cast_state {
  MC_CAST_IDLE,       /* not casting */
  MC_CAST_CONNECTING, /* connecting, starting the receiver, or getting a dropped connection back */
  MC_CAST_CASTING     /* playback goes to the device */
} mc_cast_state;

/* Why the last cast ended. */
typedef enum mc_cast_end {
  MC_CAST_END_NONE,      /* it hasn't, or it ended as asked (mc_cast_disconnect()) */
  MC_CAST_END_PAUSED,    /* it stayed paused for MC_CAST_PAUSED_MS; playback has stopped */
  MC_CAST_END_FAILED,    /* the device couldn't be reached, or wouldn't start the receiver */
  MC_CAST_END_LOST,      /* the connection was lost and couldn't be got back */
  MC_CAST_END_REMOTE,    /* stopped from elsewhere: the TV, Google Home, another phone */
  MC_CAST_END_DISPLACED, /* another app cast to the device; its receiver is left running */
  MC_CAST_END_PLAYER     /* the player was freed */
} mc_cast_end;

typedef struct mc_cast_status {
  mc_cast_state state;
  char device_id[MC_CAST_ID_LEN]; /* while connecting or casting, else "" */
  /* What to call the device: its name, else its model, else "", for which frontends show
   * MC_STR_CAST_DEVICE_UNNAMED. While connecting or casting, else "". */
  char device_name[MC_CAST_NAME_LEN];
  int volume;        /* the device's, 0 to 100, as it last said (or as last set); 0 when idle */
  int muted;
  mc_cast_end ended; /* why the last cast ended; NONE again on connecting */
} mc_cast_status;

/* A handle for Cast: nothing runs until discovery starts or a device is connected. cb may
 * be NULL. */
mc_status mc_cast_new(mc_cast_cb cb, void *user, mc_cast **out);
/* Ends any cast as mc_cast_disconnect() does, waiting for it to hand back, stops discovery and
 * frees cast. Call it before freeing the player. NULL is a no-op. */
void mc_cast_free(mc_cast *cast);

/* Looks for devices on the network until mc_cast_stop_discovery(): at once and then every
 * few seconds, slowing to every 20. Devices appear in mc_cast_devices() as they answer, and go
 * once they stop answering for a minute or say goodbye. MC_ERR_NET only if the socket API
 * can't be initialised. Starting it again while it runs is a no-op. */
mc_status mc_cast_start_discovery(mc_cast *cast);
/* Stops looking. The devices found stay listed, so one can still be connected to; the next
 * discovery drops those that don't answer it within a few seconds. */
void mc_cast_stop_discovery(mc_cast *cast);
/* The devices found, as of now. Free with mc_cast_device_list_free(). */
mc_status mc_cast_devices(mc_cast *cast, mc_cast_device_list **out);
void mc_cast_device_list_free(mc_cast_device_list *list);

/* Casts to the device with id (from mc_cast_devices()), playing what player has, at once
 * if it's playing. Returns at once: the state goes to MC_CAST_CONNECTING, then
 * MC_CAST_CASTING once playback has moved over, or back to MC_CAST_IDLE with ended saying
 * why it didn't. A cast to another device ends first (which may take a moment); one to the
 * same device carries on. MC_ERR_NOT_FOUND for an id that isn't listed. The player must stay
 * alive until the cast ends (freeing it ends it). */
mc_status mc_cast_connect(mc_cast *cast, const char *device_id, mc_player *player);
/* Ends the cast, stopping playback on the device and leaving the receiver's screen. Returns
 * at once; the player is handed back before the state goes to MC_CAST_IDLE. A no-op when
 * idle. */
void mc_cast_disconnect(mc_cast *cast);

/* A consistent snapshot. Cheap enough for a UI timer. */
void mc_cast_status_get(mc_cast *cast, mc_cast_status *out);
/* Whether playback goes to a device now (MC_CAST_CASTING). */
int mc_cast_is_casting(mc_cast *cast);

/* Sets the device's volume, 0 to 100 (out of range is clamped), and unmutes it. The status
 * has the new volume at once, for a slider being dragged; the device's own word follows. A
 * no-op unless connecting or casting. */
mc_status mc_cast_set_volume(mc_cast *cast, int volume);
/* Mutes or unmutes the device. A no-op unless connecting or casting. */
mc_status mc_cast_set_muted(mc_cast *cast, int muted);

#ifdef __cplusplus
}
#endif

#endif
