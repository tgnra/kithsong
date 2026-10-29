/* WinRT from C: the interfaces come from mingw-w64's windows.media.h, and the two event
 * handlers are small hand-made COM objects. */
#define COBJMACROS
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Media
#define WIDL_using_Windows_Storage_Streams
#include "media_controls.h"

#include "cover.h" /* before initguid.h, which would define WIC's IIDs here too */

#include <initguid.h> /* defines the WinRT IIDs below, which no import library has */
#include <roapi.h>
#include <shcore.h>
#include <shlwapi.h>
#include <stdio.h>
#include <systemmediatransportcontrolsinterop.h>
#include <windows.media.h>
#include <windows.storage.streams.h>
#include <winstring.h>

typedef __FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CSystemMediaTransportControlsButtonPressedEventArgs
  button_handler;
typedef __FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CPlaybackPositionChangeRequestedEventArgs
  seek_handler;
typedef __FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CShuffleEnabledChangeRequestedEventArgs
  shuffle_handler;

/* {94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90}: handlers that say they're agile are called on
 * whichever thread raised the event, rather than marshalled to the UI thread. */
static const GUID agile_iid = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

static struct {
  ISystemMediaTransportControls *smtc;
  ISystemMediaTransportControls2 *smtc2; /* timeline; NULL before Windows 10 1607 */
  ISystemMediaTransportControlsTimelineProperties *timeline;
  EventRegistrationToken button_token, seek_token, shuffle_token;
  HWND notify;
  UINT msg;
  BOOL shown;
} s;

/* --- Event handlers --- */

/* The handlers are statics that live as long as the app, so they aren't reference counted. */
static HRESULT handler_query(IUnknown *self, REFIID iid, const IID *own, void **out) {
  if (IsEqualIID(iid, own) || IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &agile_iid)) {
    *out = self;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE button_addref(button_handler *self) {
  (void)self;
  return 2;
}

static ULONG STDMETHODCALLTYPE button_release(button_handler *self) {
  (void)self;
  return 1;
}

static HRESULT STDMETHODCALLTYPE button_query(button_handler *self, REFIID iid, void **out) {
  return handler_query((IUnknown *)self, iid, &IID_ITypedEventHandler_SystemMediaTransportControls_SystemMediaTransportControlsButtonPressedEventArgs, out);
}

static HRESULT STDMETHODCALLTYPE button_invoke(button_handler *self, ISystemMediaTransportControls *sender,
                                               ISystemMediaTransportControlsButtonPressedEventArgs *args) {
  (void)self;
  (void)sender;
  SystemMediaTransportControlsButton button;
  if (FAILED(ISystemMediaTransportControlsButtonPressedEventArgs_get_Button(args, &button))) return S_OK;
  int b = -1;
  switch (button) {
    case SystemMediaTransportControlsButton_Play: b = MEDIA_PLAY; break;
    case SystemMediaTransportControlsButton_Pause: b = MEDIA_PAUSE; break;
    case SystemMediaTransportControlsButton_Stop: b = MEDIA_STOP; break;
    case SystemMediaTransportControlsButton_Next: b = MEDIA_NEXT; break;
    case SystemMediaTransportControlsButton_Previous: b = MEDIA_PREVIOUS; break;
    default: break;
  }
  if (b >= 0) PostMessageW(s.notify, s.msg, (WPARAM)b, 0);
  return S_OK;
}

static ULONG STDMETHODCALLTYPE seek_addref(seek_handler *self) {
  (void)self;
  return 2;
}

static ULONG STDMETHODCALLTYPE seek_release(seek_handler *self) {
  (void)self;
  return 1;
}

static HRESULT STDMETHODCALLTYPE seek_query(seek_handler *self, REFIID iid, void **out) {
  return handler_query((IUnknown *)self, iid, &IID_ITypedEventHandler_SystemMediaTransportControls_PlaybackPositionChangeRequestedEventArgs, out);
}

static HRESULT STDMETHODCALLTYPE seek_invoke(seek_handler *self, ISystemMediaTransportControls *sender,
                                             IPlaybackPositionChangeRequestedEventArgs *args) {
  (void)self;
  (void)sender;
  TimeSpan at;
  if (SUCCEEDED(IPlaybackPositionChangeRequestedEventArgs_get_RequestedPlaybackPosition(args, &at)))
    PostMessageW(s.notify, s.msg, MEDIA_SEEK, (LPARAM)(at.Duration / 10000));
  return S_OK;
}

static ULONG STDMETHODCALLTYPE shuffle_addref(shuffle_handler *self) {
  (void)self;
  return 2;
}

static ULONG STDMETHODCALLTYPE shuffle_release(shuffle_handler *self) {
  (void)self;
  return 1;
}

static HRESULT STDMETHODCALLTYPE shuffle_query(shuffle_handler *self, REFIID iid, void **out) {
  return handler_query((IUnknown *)self, iid, &IID_ITypedEventHandler_SystemMediaTransportControls_ShuffleEnabledChangeRequestedEventArgs, out);
}

static HRESULT STDMETHODCALLTYPE shuffle_invoke(shuffle_handler *self, ISystemMediaTransportControls *sender,
                                                IShuffleEnabledChangeRequestedEventArgs *args) {
  (void)self;
  (void)sender;
  boolean on;
  if (SUCCEEDED(IShuffleEnabledChangeRequestedEventArgs_get_RequestedShuffleEnabled(args, &on)))
    PostMessageW(s.notify, s.msg, MEDIA_SHUFFLE, on ? 1 : 0);
  return S_OK;
}

static __FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CSystemMediaTransportControlsButtonPressedEventArgsVtbl
  button_vtbl = {button_query, button_addref, button_release, button_invoke};
static __FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CPlaybackPositionChangeRequestedEventArgsVtbl
  seek_vtbl = {seek_query, seek_addref, seek_release, seek_invoke};
static __FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CShuffleEnabledChangeRequestedEventArgsVtbl
  shuffle_vtbl = {shuffle_query, shuffle_addref, shuffle_release, shuffle_invoke};
static button_handler button_obj = {&button_vtbl};
static seek_handler seek_obj = {&seek_vtbl};
static shuffle_handler shuffle_obj = {&shuffle_vtbl};

/* --- Setup --- */

static HRESULT activation_factory(const wchar_t *cls, REFIID iid, void **out) {
  HSTRING name;
  HRESULT hr = WindowsCreateString(cls, (UINT32)wcslen(cls), &name);
  if (FAILED(hr)) return hr;
  hr = RoGetActivationFactory(name, iid, out);
  WindowsDeleteString(name);
  return hr;
}

BOOL media_controls_init(HWND wnd, HWND notify, UINT msg) {
  s.notify = notify;
  s.msg = msg;
  ISystemMediaTransportControlsInterop *interop = NULL;
  if (FAILED(activation_factory(RuntimeClass_Windows_Media_SystemMediaTransportControls,
                                &IID_ISystemMediaTransportControlsInterop, (void **)&interop)))
    return FALSE;
  HRESULT hr = ISystemMediaTransportControlsInterop_GetForWindow(interop, wnd, &IID_ISystemMediaTransportControls,
                                                                 (void **)&s.smtc);
  ISystemMediaTransportControlsInterop_Release(interop);
  if (FAILED(hr)) {
    s.smtc = NULL;
    return FALSE;
  }
  ISystemMediaTransportControls_put_IsEnabled(s.smtc, FALSE); /* until something plays */
  ISystemMediaTransportControls_add_ButtonPressed(s.smtc, &button_obj, &s.button_token);

  if (SUCCEEDED(ISystemMediaTransportControls_QueryInterface(s.smtc, &IID_ISystemMediaTransportControls2,
                                                             (void **)&s.smtc2))) {
    HSTRING name;
    const wchar_t *cls = RuntimeClass_Windows_Media_SystemMediaTransportControlsTimelineProperties;
    IInspectable *obj = NULL;
    if (SUCCEEDED(WindowsCreateString(cls, (UINT32)wcslen(cls), &name))) {
      if (SUCCEEDED(RoActivateInstance(name, &obj))) {
        IInspectable_QueryInterface(obj, &IID_ISystemMediaTransportControlsTimelineProperties, (void **)&s.timeline);
        IInspectable_Release(obj);
      }
      WindowsDeleteString(name);
    }
    ISystemMediaTransportControls2_add_PlaybackPositionChangeRequested(s.smtc2, &seek_obj, &s.seek_token);
    ISystemMediaTransportControls2_add_ShuffleEnabledChangeRequested(s.smtc2, &shuffle_obj, &s.shuffle_token);
  } else {
    s.smtc2 = NULL;
  }
  return TRUE;
}

void media_controls_free(void) {
  if (s.smtc2) {
    ISystemMediaTransportControls2_remove_PlaybackPositionChangeRequested(s.smtc2, s.seek_token);
    ISystemMediaTransportControls2_remove_ShuffleEnabledChangeRequested(s.smtc2, s.shuffle_token);
    ISystemMediaTransportControls2_Release(s.smtc2);
  }
  if (s.timeline) ISystemMediaTransportControlsTimelineProperties_Release(s.timeline);
  if (s.smtc) {
    ISystemMediaTransportControls_remove_ButtonPressed(s.smtc, s.button_token);
    ISystemMediaTransportControls_put_IsEnabled(s.smtc, FALSE);
    ISystemMediaTransportControls_Release(s.smtc);
  }
  memset(&s, 0, sizeof s);
}

/* --- Updates --- */

/* NULL is WinRT's empty string, so NULL text needs no special case. */
static HSTRING hstring(const wchar_t *text) {
  HSTRING h = NULL;
  if (text && *text && FAILED(WindowsCreateString(text, (UINT32)wcslen(text), &h))) h = NULL;
  return h;
}

/* The album's cover as a stream reference, or NULL. */
static IRandomAccessStreamReference *cover_stream(const wchar_t *dir) {
  wchar_t path[MAX_PATH * 2];
  IStream *file = NULL;
  if (!cover_find(dir, path, MAX_PATH * 2) ||
      FAILED(SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, NULL, &file)))
    return NULL;
  IRandomAccessStream *stream = NULL;
  IRandomAccessStreamReferenceStatics *statics = NULL;
  IRandomAccessStreamReference *ref = NULL;
  if (SUCCEEDED(CreateRandomAccessStreamOverStream(file, BSOS_DEFAULT, &IID_IRandomAccessStream, (void **)&stream)) &&
      SUCCEEDED(activation_factory(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference,
                                   &IID_IRandomAccessStreamReferenceStatics, (void **)&statics)))
    if (FAILED(IRandomAccessStreamReferenceStatics_CreateFromStream(statics, stream, &ref))) ref = NULL;
  if (statics) IRandomAccessStreamReferenceStatics_Release(statics);
  if (stream) IRandomAccessStream_Release(stream);
  IStream_Release(file);
  return ref;
}

void media_controls_set_track(const wchar_t *title, const wchar_t *artist, const wchar_t *album,
                              const wchar_t *cover_dir) {
  if (!s.smtc) return;
  ISystemMediaTransportControlsDisplayUpdater *updater = NULL;
  if (FAILED(ISystemMediaTransportControls_get_DisplayUpdater(s.smtc, &updater))) return;
  ISystemMediaTransportControlsDisplayUpdater_ClearAll(updater);
  if (title) {
    if (!s.shown) ISystemMediaTransportControls_put_IsEnabled(s.smtc, TRUE);
    s.shown = TRUE;
    ISystemMediaTransportControlsDisplayUpdater_put_Type(updater, MediaPlaybackType_Music);
    IMusicDisplayProperties *music = NULL;
    if (SUCCEEDED(ISystemMediaTransportControlsDisplayUpdater_get_MusicProperties(updater, &music))) {
      HSTRING t = hstring(title), a = hstring(artist), al = hstring(album);
      IMusicDisplayProperties_put_Title(music, t);
      IMusicDisplayProperties_put_Artist(music, a);
      IMusicDisplayProperties_put_AlbumArtist(music, a);
      IMusicDisplayProperties2 *music2 = NULL;
      if (SUCCEEDED(IMusicDisplayProperties_QueryInterface(music, &IID_IMusicDisplayProperties2, (void **)&music2))) {
        IMusicDisplayProperties2_put_AlbumTitle(music2, al);
        IMusicDisplayProperties2_Release(music2);
      }
      WindowsDeleteString(t);
      WindowsDeleteString(a);
      WindowsDeleteString(al);
      IMusicDisplayProperties_Release(music);
    }
    IRandomAccessStreamReference *cover = cover_dir ? cover_stream(cover_dir) : NULL;
    if (cover) {
      ISystemMediaTransportControlsDisplayUpdater_put_Thumbnail(updater, cover);
      IRandomAccessStreamReference_Release(cover);
    }
  }
  ISystemMediaTransportControlsDisplayUpdater_Update(updater);
  ISystemMediaTransportControlsDisplayUpdater_Release(updater);
}

void media_controls_set_state(mc_player_state state, BOOL has_previous, BOOL has_next, BOOL shuffle) {
  if (!s.smtc || !s.shown) return;
  MediaPlaybackStatus status = state == MC_PLAYER_PLAYING ? MediaPlaybackStatus_Playing
                               : state == MC_PLAYER_PAUSED ? MediaPlaybackStatus_Paused
                                                           : MediaPlaybackStatus_Stopped;
  ISystemMediaTransportControls_put_PlaybackStatus(s.smtc, status);
  ISystemMediaTransportControls_put_IsPlayEnabled(s.smtc, TRUE);
  ISystemMediaTransportControls_put_IsPauseEnabled(s.smtc, TRUE);
  ISystemMediaTransportControls_put_IsStopEnabled(s.smtc, state != MC_PLAYER_STOPPED ? TRUE : FALSE);
  ISystemMediaTransportControls_put_IsPreviousEnabled(s.smtc, has_previous ? TRUE : FALSE);
  ISystemMediaTransportControls_put_IsNextEnabled(s.smtc, has_next ? TRUE : FALSE);
  if (s.smtc2) ISystemMediaTransportControls2_put_ShuffleEnabled(s.smtc2, shuffle ? TRUE : FALSE);
}

void media_controls_set_timeline(int64_t position_ms, int64_t duration_ms) {
  if (!s.smtc2 || !s.timeline || !s.shown) return;
  TimeSpan zero = {0}, end = {duration_ms * 10000}, at = {position_ms * 10000};
  ISystemMediaTransportControlsTimelineProperties_put_StartTime(s.timeline, zero);
  ISystemMediaTransportControlsTimelineProperties_put_MinSeekTime(s.timeline, zero);
  ISystemMediaTransportControlsTimelineProperties_put_EndTime(s.timeline, end);
  ISystemMediaTransportControlsTimelineProperties_put_MaxSeekTime(s.timeline, end);
  ISystemMediaTransportControlsTimelineProperties_put_Position(s.timeline, at);
  ISystemMediaTransportControls2_UpdateTimelineProperties(s.smtc2, s.timeline);
}
