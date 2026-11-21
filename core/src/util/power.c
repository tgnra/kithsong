/* See power.h. Both rules are plain code on every platform; only asking the machine isn't. */
#include "util/power.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "util/platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define NO_SYSTEM_BATTERY 128
#define UNKNOWN 255

static atomic_int g_override = -1;
static const char *g_sysfs_root = "/sys/class/power_supply";

void mc_power_set_override(int on_ac) {
  atomic_store(&g_override, on_ac);
}

void mc_power_set_sysfs_root(const char *root) {
  g_sysfs_root = root ? root : "/sys/class/power_supply";
}

int mc_power_windows_on_ac(int ac_line_status, int battery_flag) {
  if (ac_line_status == 1 || battery_flag == NO_SYSTEM_BATTERY) return 1;
  return ac_line_status == UNKNOWN && battery_flag == UNKNOWN;
}

typedef struct supplies {
  const char *root;
  int ac, battery;
} supplies;

/* The first line of root/name/file, without its newline; 0 if there's no such file. */
static int read_value(const char *root, const char *name, const char *file, char *out, int cap) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s/%s", root, name, file);
  FILE *f = mc_fopen(path, "rb");
  if (!f) return 0;
  char *line = fgets(out, cap, f);
  fclose(f);
  if (!line) return 0;
  out[strcspn(out, "\r\n")] = '\0';
  return 1;
}

/* Not by is_dir: the entries of the real folder are links to directories. */
static void look_at(void *user, const char *name, int is_dir) {
  (void)is_dir;
  supplies *s = user;
  char type[32], value[32];
  if (!read_value(s->root, name, "type", type, sizeof type)) return;
  if (read_value(s->root, name, "scope", value, sizeof value) && !strcmp(value, "Device")) return;
  if (!strcmp(type, "Battery")) {
    if (!read_value(s->root, name, "present", value, sizeof value) || strcmp(value, "0")) s->battery = 1;
  } else if (!strcmp(type, "Mains") || !strncmp(type, "USB", 3)) { /* and USB_C, USB_PD... */
    if (read_value(s->root, name, "online", value, sizeof value) && !strcmp(value, "1")) s->ac = 1;
  }
}

int mc_power_sysfs_on_ac(const char *root) {
  supplies s = {root, 0, 0};
  if (mc_list_dir(root, look_at, &s) != MC_OK) return 1;
  return s.ac || !s.battery;
}

int mc_power_on_ac(void) {
  int forced = atomic_load(&g_override);
  if (forced >= 0) return forced;
#ifdef _WIN32
  (void)g_sysfs_root;
  SYSTEM_POWER_STATUS status;
  if (!GetSystemPowerStatus(&status)) return 1;
  return mc_power_windows_on_ac(status.ACLineStatus, status.BatteryFlag);
#elif defined(__linux__)
  return mc_power_sysfs_on_ac(g_sysfs_root);
#else
  return 1;
#endif
}
