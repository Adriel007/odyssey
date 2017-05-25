#ifndef OD_LOG_H
#define OD_LOG_H

/*
 * odissey.
 *
 * PostgreSQL connection pooler and request router.
*/

typedef struct od_log od_log_t;

struct od_log
{
	int          fd;
	int          verbosity;
	od_pid_t    *pid;
	od_syslog_t *syslog;
};

int od_log_init(od_log_t*, od_pid_t*, od_syslog_t*);
int od_log_open(od_log_t*, char*);
int od_log_close(od_log_t*);
int od_logv(od_log_t*, od_syslogprio_t, machine_io_t, char*, char*, va_list);

static inline void
od_logset_verbosity(od_log_t *l, int level) {
	l->verbosity = level;
}

static inline int
od_log(od_log_t *l, machine_io_t peer, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int rc = od_logv(l, OD_SYSLOG_INFO, peer, NULL, fmt, args);
	va_end(args);
	return rc;
}

static inline int
od_debug(od_log_t *l, machine_io_t peer, char *fmt, ...)
{
	if (l->verbosity < 2)
		return 0;
	va_list args;
	va_start(args, fmt);
	int rc = od_logv(l, OD_SYSLOG_DEBUG, peer, "debug:", fmt, args);
	va_end(args);
	return rc;
}

static inline int
od_error(od_log_t *l, machine_io_t peer, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int rc = od_logv(l, OD_SYSLOG_ERROR, peer, "error:", fmt, args);
	va_end(args);
	return rc;
}

#endif /* OD_LOG_H */