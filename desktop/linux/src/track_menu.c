#include "track_menu.h"

void track_menu_append_with_key(GMenu *menu, enum mc_str_id label, const char *action, const char *accel) {
  GMenuItem *item = g_menu_item_new(mc_str_mnemonic(label), action);
  g_menu_item_set_attribute(item, "accel", "s", accel);
  g_menu_append_item(menu, item);
  g_object_unref(item);
}

/* Go to album and Go to artist, hidden while their actions are disabled. */
static void append_go_to(GMenu *model) {
  GMenu *section = g_menu_new();
  const struct {
    enum mc_str_id label;
    const char *action;
  } items[] = {{MC_STR_GO_TO_ALBUM, "track.go-to-album"}, {MC_STR_GO_TO_ARTIST, "track.go-to-artist"}};
  for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
    GMenuItem *item = g_menu_item_new(mc_str_mnemonic(items[i].label), items[i].action);
    g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
    g_menu_append_item(section, item);
    g_object_unref(item);
  }
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
}

GMenuModel *track_menu_model(gboolean in_playlist) {
  GMenu *model = g_menu_new();
  GMenu *section = g_menu_new();
  g_menu_append(section, mc_str_mnemonic(MC_STR_ADD_TO_PLAYLIST_MENU), "track.add-to-playlist");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  append_go_to(model);
  if (in_playlist) {
    section = g_menu_new();
    track_menu_append_with_key(section, MC_STR_MOVE_UP, "track.move-up", "<Alt>Up");
    track_menu_append_with_key(section, MC_STR_MOVE_DOWN, "track.move-down", "<Alt>Down");
    g_menu_append_section(model, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  section = g_menu_new();
  if (in_playlist) track_menu_append_with_key(section, MC_STR_REMOVE_FROM_PLAYLIST, "track.remove", "Delete");
  g_menu_append(section, mc_str_mnemonic(MC_STR_EDIT_MENU), "track.edit");
  g_menu_append(section, mc_str_mnemonic(MC_STR_DELETE_FROM_LIBRARY_MENU), "track.delete");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  return G_MENU_MODEL(model);
}

gboolean track_menu_album_has_page(mc_library *lib, const char *album_uid) {
  mc_track_list *list = NULL;
  if (!lib || !album_uid[0] || mc_library_list_album_tracks(lib, album_uid, &list) != MC_OK) return FALSE;
  gboolean more = list->count > 1;
  mc_track_list_free(list);
  return more;
}
