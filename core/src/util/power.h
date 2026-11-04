/* Whether this machine runs on mains power, for work that should wait for it. Internal to core.
 * When in doubt the answer is yes: a desktop must never look like a laptop on battery, and
 * work held back for no reason is worse than a little battery spent. */
#ifndef MC_UTIL_POWER_H
#define MC_UTIL_POWER_H

/* 1 on AC, 0 on battery. Cheap enough to ask between tracks. Any thread. */
int mc_power_on_ac(void);

/* The two rules, apart from the machine they run on, so both can be tested on either.
 *
 * Windows, from SYSTEM_POWER_STATUS: ACLineStatus 1 is AC, a BatteryFlag of 128 (no system
 * battery) is AC whatever the line says, and an unknown line (255) is AC only where no battery
 * is known of (BatteryFlag 255). */
int mc_power_windows_on_ac(int ac_line_status, int battery_flag);
/* Linux, from a folder like /sys/class/power_supply: any Mains or USB supply online is AC, and
 * so is having no battery at all, as on a desktop or in a container, or a folder that can't be
 * read. A supply with scope Device belongs to a peripheral, a wireless mouse say, and doesn't
 * count. Nor does an empty battery bay. */
int mc_power_sysfs_on_ac(const char *root);

/* For tests: mc_power_on_ac() answers on_ac (0 or 1) from now on; -1 goes back to asking. */
void mc_power_set_override(int on_ac);
/* For tests: where Linux looks instead of /sys/class/power_supply; NULL goes back. The caller
 * keeps root alive. */
void mc_power_set_sysfs_root(const char *root);

#endif
