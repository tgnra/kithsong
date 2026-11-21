/* Core's diagnostic lines ("kithcore: ..."), to stderr or to the frontend's handler
 * (mc_log_set_handler() in kithcore.h). Internal to core. */
#ifndef MC_UTIL_LOG_H
#define MC_UTIL_LOG_H

#if defined(__GNUC__) || defined(__clang__)
#define MC_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define MC_PRINTF(f, a)
#endif

/* As fprintf(stderr, ...); a handler gets the line without its trailing newline. Any
 * thread. */
void mc_log(const char *fmt, ...) MC_PRINTF(1, 2);

#endif
