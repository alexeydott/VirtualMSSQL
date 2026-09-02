/* vms_log.h — logging with mandatory secret redaction (R2).
 * Log output never contains credentials, passwords or connection secrets:
 * every formatted string passes through vms_redact() first. */
#ifndef VIRTUALMSSQL_VMS_LOG_H
#define VIRTUALMSSQL_VMS_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VmsLogLevel {
    VMS_LOG_ERROR = 0,
    VMS_LOG_WARN,
    VMS_LOG_INFO,
    VMS_LOG_DEBUG
} VmsLogLevel;

/* Install a sink; passing NULL disables logging entirely (default). */
typedef void (*VmsLogSink)(void* user, int level, const char* line);
void vms_log_set_sink(VmsLogSink sink, void* user);
void vms_log_set_level(int max_level);

/* Format + redact + emit. Never writes credentials to the sink. */
void vms_log(int level, const char* fmt, ...);

/* Redact secret-looking fragments in place of a copy; returns a heap string
 * the caller must free(), or NULL on OOM. Patterns: PWD=..., PASSWORD=...,
 * UID=... (value only when it looks like a credential assignment). */
char* vms_redact(const char* text);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_LOG_H */
