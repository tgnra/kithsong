/* Core's log lines (mc_log_set_handler()), to the debugger (OutputDebugString) and to
 * kithsong.log in the config dir, timestamped. Past LOG_FILE_MAX bytes the file moves to
 * kithsong.1.log, replacing the one there, and a new one starts. */
#ifndef KITHSONG_LOG_FILE_H
#define KITHSONG_LOG_FILE_H

/* config_dir: UTF-8, copied; NULL logs to the debugger only. Call once, before sync starts. */
void log_file_start(const char *config_dir);

#endif
