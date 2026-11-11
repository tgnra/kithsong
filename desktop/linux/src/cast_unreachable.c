#include "cast_unreachable.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>

#include "kithcore/kithcore.h"

/* The interface that has address (numeric), into if_name (IF_NAMESIZE bytes), and for IPv4 its
 * network, "192.168.0.0/24", into network (INET_ADDRSTRLEN + 3 bytes, "" for IPv6). FALSE if
 * no interface has it. */
static gboolean find_interface(const char *address, char *if_name, char *network) {
  struct in_addr want4;
  struct in6_addr want6;
  int v4 = inet_pton(AF_INET, address, &want4) == 1;
  if (!v4 && inet_pton(AF_INET6, address, &want6) != 1) return FALSE;
  struct ifaddrs *list = NULL;
  if (getifaddrs(&list) != 0) return FALSE;
  gboolean found = FALSE;
  for (struct ifaddrs *i = list; i && !found; i = i->ifa_next) {
    if (!i->ifa_addr || !i->ifa_name) continue;
    if (v4 && i->ifa_addr->sa_family == AF_INET) {
      const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
      if (a->sin_addr.s_addr != want4.s_addr) continue;
      found = TRUE;
      network[0] = '\0';
      if (i->ifa_netmask) {
        uint32_t mask = ntohl(((const struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        int bits = 0;
        while (bits < 32 && (mask & (0x80000000u >> bits))) bits++;
        struct in_addr net = {htonl(ntohl(want4.s_addr) & mask)};
        char text[INET_ADDRSTRLEN];
        if (bits > 0 && inet_ntop(AF_INET, &net, text, sizeof text))
          g_snprintf(network, INET_ADDRSTRLEN + 3, "%s/%d", text, bits);
      }
    } else if (!v4 && i->ifa_addr->sa_family == AF_INET6) {
      const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)i->ifa_addr;
      if (memcmp(&a->sin6_addr, &want6, sizeof want6) != 0) continue;
      found = TRUE;
      network[0] = '\0';
    }
    if (found) g_strlcpy(if_name, i->ifa_name, IF_NAMESIZE);
  }
  freeifaddrs(list);
  return found;
}

/* The commands that allow port: firewalld's into the zone of if_name ("": the default zone),
 * and ufw's from network ("": from anywhere). g_free them. */
static char *firewalld_command(const char *if_name, unsigned port) {
  if (*if_name)
    return g_strdup_printf("sudo firewall-cmd --permanent --zone=\"$(firewall-cmd --get-zone-of-interface=%s)\" "
                           "--add-port=%u/tcp && sudo firewall-cmd --reload",
                           if_name, port);
  return g_strdup_printf("sudo firewall-cmd --permanent --add-port=%u/tcp && sudo firewall-cmd --reload", port);
}

static char *ufw_command(const char *network, unsigned port) {
  if (*network) return g_strdup_printf("sudo ufw allow from %s to any port %u proto tcp", network, port);
  return g_strdup_printf("sudo ufw allow %u/tcp", port);
}

static void on_copy(GtkButton *button, gpointer data) {
  (void)data;
  const char *text = g_object_get_data(G_OBJECT(button), "command");
  if (text) gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), text);
}

/* A row for a firewall: its name, the command, and a button that copies it. Takes command. */
static GtkWidget *command_row(const char *firewall, char *command) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), firewall);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), command);
  gtk_widget_add_css_class(row, "property"); /* the command is what's read, the name its label */
  gtk_widget_add_css_class(row, "monospace");
  GtkWidget *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
  gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(copy, "flat");
  gtk_widget_set_tooltip_text(copy, mc_str(MC_STR_LINUX_COPY_COMMAND));
  gtk_accessible_update_property(GTK_ACCESSIBLE(copy), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_LINUX_COPY_COMMAND),
                                 -1);
  g_object_set_data_full(G_OBJECT(copy), "command", command, g_free);
  g_signal_connect(copy, "clicked", G_CALLBACK(on_copy), NULL);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), copy);
  return row;
}

void cast_unreachable_show(GtkWidget *parent, const char *name, const char *address, unsigned port) {
  const char *who = name && *name ? name : mc_str(MC_STR_CAST_DEVICE_UNNAMED);
  char *heading = mc_format_alloc(mc_str(MC_STR_LINUX_CAST_UNREACHABLE_TITLE), MC_S(who));
  char *body = mc_format_alloc(mc_str(MC_STR_LINUX_CAST_UNREACHABLE_TEXT), MC_S(who), MC_D(port));
  AdwDialog *dialog = adw_alert_dialog_new(heading, body);
  mc_free(heading);
  mc_free(body);

  char if_name[IF_NAMESIZE] = "", network[INET_ADDRSTRLEN + 3] = "";
  if (!address || !*address || !find_interface(address, if_name, network)) if_name[0] = network[0] = '\0';

  GtkWidget *caption = gtk_label_new(mc_str(MC_STR_LINUX_CAST_UNREACHABLE_COMMANDS));
  gtk_label_set_wrap(GTK_LABEL(caption), TRUE);
  gtk_label_set_xalign(GTK_LABEL(caption), 0);
  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_list_box_append(GTK_LIST_BOX(list), command_row("firewalld", firewalld_command(if_name, port)));
  gtk_list_box_append(GTK_LIST_BOX(list), command_row("ufw", ufw_command(network, port)));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_box_append(GTK_BOX(box), caption);
  gtk_box_append(GTK_BOX(box), list);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);

  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "close");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
  adw_dialog_present(dialog, parent);
}
