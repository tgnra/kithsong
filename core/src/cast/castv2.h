/* The Google Cast sender protocol (CASTV2), as far as Kithsong uses it: the message frame,
 * and the JSON messages of the four namespaces a sender of the Default Media Receiver needs.
 * Only building and parsing, no I/O, so it can be tested on its own. Internal to core.
 *
 * On the wire, every message is a 4-byte big-endian length and then a protobuf CastMessage:
 *
 *   message CastMessage {
 *     required ProtocolVersion protocol_version = 1;  // CASTV2_1_0 = 0
 *     required string source_id = 2;                  // "sender-0", or the receiver's
 *     required string destination_id = 3;             // "receiver-0", or an app's transportId
 *     required string namespace = 4;
 *     required PayloadType payload_type = 5;          // STRING = 0, BINARY = 1
 *     optional string payload_utf8 = 6;               // JSON
 *     optional bytes payload_binary = 7;              // only for device authentication
 *   }
 *
 * which is small enough to write and read by hand. A message is at most 64 KiB. */
#ifndef MC_CAST_CASTV2_H
#define MC_CAST_CASTV2_H

#include <stddef.h>
#include <stdint.h>

#define CAST_NS_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define CAST_NS_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define CAST_NS_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define CAST_NS_MEDIA "urn:x-cast:com.google.cast.media"

/* Google's Default Media Receiver: plays a URL, with no receiver app of our own. */
#define CAST_APP_DEFAULT_MEDIA "CC1AD845"
#define CAST_SENDER_ID "sender-0"
#define CAST_RECEIVER_ID "receiver-0"

enum {
  CAST_MAX_MESSAGE = 64 * 1024, /* the body, without the length; the device's own limit */
  CAST_ID_LEN = 128,            /* source, destination and namespace, NUL included */
  CAST_URL_LEN = 512,           /* a content id kept from a status, NUL included */
  CAST_MAX_APPS = 4,            /* apps kept from a receiver status */
  CAST_MAX_ITEMS = 16           /* queue items kept from a media status */
};

/* --- The frame --- */

typedef struct cast_message {
  char source[CAST_ID_LEN];
  char dest[CAST_ID_LEN];
  char ns[CAST_ID_LEN];
  int binary;                   /* payload_binary rather than payload_utf8 */
  const unsigned char *payload; /* into the body decoded; not NUL-terminated */
  size_t payload_len;
} cast_message;

/* Writes a string message, length prefix included, into out (cap bytes). Returns its length,
 * or 0 if it doesn't fit in cap or is over CAST_MAX_MESSAGE. */
size_t cast_frame_encode(const char *source, const char *dest, const char *ns, const char *payload,
                         unsigned char *out, size_t cap);
/* The body length a frame's 4-byte prefix gives. */
uint32_t cast_frame_length(const unsigned char prefix[4]);
/* Reads a body (what follows the prefix). Fields it doesn't know are skipped; ids longer
 * than CAST_ID_LEN are cut. 0, or -1 if it isn't a CastMessage or lacks a required field. */
int cast_message_decode(const unsigned char *body, size_t len, cast_message *out);

/* --- Messages received --- */

typedef enum cast_type {
  CAST_T_OTHER,
  CAST_T_PING,
  CAST_T_PONG,
  CAST_T_CLOSE, /* the receiver closed our virtual connection to it or to the app */
  CAST_T_RECEIVER_STATUS,
  CAST_T_LAUNCH_ERROR,
  CAST_T_MEDIA_STATUS,
  CAST_T_LOAD_FAILED,
  CAST_T_LOAD_CANCELLED,
  CAST_T_INVALID_REQUEST,
  CAST_T_INVALID_PLAYER_STATE,
  CAST_T_QUEUE_ITEM_IDS
} cast_type;

typedef struct cast_app {
  char app_id[32];
  char session_id[64];
  char transport_id[64];
} cast_app;

typedef struct cast_receiver_status {
  int has_volume;
  double level; /* 0 to 1 */
  int muted;
  int napps;    /* running apps: none when idle */
  cast_app apps[CAST_MAX_APPS];
} cast_receiver_status;

typedef enum cast_player_state {
  CAST_PLAYER_IDLE,
  CAST_PLAYER_PLAYING,
  CAST_PLAYER_PAUSED,
  CAST_PLAYER_BUFFERING,
  CAST_PLAYER_LOADING
} cast_player_state;

typedef enum cast_idle_reason {
  CAST_IDLE_NONE,
  CAST_IDLE_FINISHED,    /* played to the end, of the item and so of a queue with nothing after */
  CAST_IDLE_CANCELLED,   /* stopped by a sender */
  CAST_IDLE_INTERRUPTED, /* another load replaced it */
  CAST_IDLE_ERROR        /* couldn't be played */
} cast_idle_reason;

typedef struct cast_queue_item {
  int item_id;
  char content_id[CAST_URL_LEN]; /* "" when the status leaves the item's media out */
} cast_queue_item;

/* A media session's status. A MEDIA_STATUS with no session (an empty "status" list: nothing
 * loaded, or a queue that has played out) has present 0. */
typedef struct cast_media_status {
  int present;
  long long media_session_id;
  cast_player_state state;
  cast_idle_reason idle_reason;
  int has_time;
  double current_time; /* seconds */
  int current_item_id, loading_item_id, preloaded_item_id; /* 0 for none */
  char content_id[CAST_URL_LEN]; /* the current item's media: contentId, else contentUrl; or "" */
  int has_items; /* the status lists the queue (it does after the queue changes) */
  int nitems;
  cast_queue_item items[CAST_MAX_ITEMS];
} cast_media_status;

typedef struct cast_reply {
  cast_type type;
  long request_id;              /* 0 for a message nobody asked for */
  cast_receiver_status receiver; /* RECEIVER_STATUS */
  cast_media_status media;       /* MEDIA_STATUS, its first session */
  int nids;                      /* QUEUE_ITEM_IDS */
  int ids[CAST_MAX_ITEMS];
} cast_reply;

/* Parses a message's JSON payload. 0, or -1 if it isn't a JSON object with a "type". */
int cast_reply_parse(const char *json, size_t len, cast_reply *out);
/* The app running cast_receiver_status says with app_id, or NULL. */
const cast_app *cast_receiver_app(const cast_receiver_status *status, const char *app_id);

/* --- Messages sent ---
 *
 * Each returns the JSON, malloc'd, or NULL if out of memory. request_id 0 leaves it out, as
 * messages with no reply do. */

/* {"type": type}: CONNECT, CLOSE, PING, PONG, and the receiver's GET_STATUS with an id. */
char *cast_json_simple(const char *type, long request_id);
char *cast_json_launch(long request_id, const char *app_id);
/* The receiver's STOP: ends the app's session (session_id), which stops what it plays. */
char *cast_json_stop_app(long request_id, const char *session_id);
/* The receiver's SET_VOLUME, with the level, or muted, or both. */
char *cast_json_set_volume(long request_id, int set_level, double level, int set_muted, int muted);
/* A media command that takes nothing more: PLAY, PAUSE, STOP, GET_STATUS (media_session_id 0
 * leaves it out, for asking with no session). */
char *cast_json_media(long request_id, const char *type, long long media_session_id);
char *cast_json_seek(long request_id, long long media_session_id, double seconds);

typedef struct cast_media_item {
  const char *url;
  const char *content_type;
  const char *title, *artist, *album; /* NULL to leave out */
  int track_no;                       /* 0 to leave out */
  const char *image_url;              /* NULL for none */
  double duration_s;                  /* 0 if unknown */
  double start_s;                     /* where it starts playing */
  int autoplay;
} cast_media_item;

/* QUEUE_LOAD: replaces whatever the receiver has with items, playing the first. The
 * receiver fetches each next item preload_s before the one before it ends. session_id is
 * the app's. */
char *cast_json_queue_load(long request_id, const char *session_id, const cast_media_item *items, int n,
                           double preload_s);
/* QUEUE_INSERT: adds items at the end of the queue. */
char *cast_json_queue_insert(long request_id, long long media_session_id, const cast_media_item *items,
                             int n, double preload_s);
/* QUEUE_REMOVE: takes the items with those ids out of the queue. */
char *cast_json_queue_remove(long request_id, long long media_session_id, const int *item_ids, int n);

#endif
