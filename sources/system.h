#ifndef OD_SYSTEM_H
#define OD_SYSTEM_H

/*
 * Odyssey.
 *
 * Advanced PostgreSQL connection pooler.
*/

typedef struct od_system od_system_t;

struct od_system
{
	void *instance;
	void *pooler;
	void *router;
	void *console;
	void *cron;
	void *worker_pool;
};

#endif /* OD_SYSTEM_H */
