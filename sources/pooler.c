
/*
 * Odyssey.
 *
 * Advanced PostgreSQL connection pooler.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <machinarium.h>
#include <shapito.h>

#include "sources/macro.h"
#include "sources/version.h"
#include "sources/atomic.h"
#include "sources/util.h"
#include "sources/error.h"
#include "sources/list.h"
#include "sources/pid.h"
#include "sources/id.h"
#include "sources/logger.h"
#include "sources/daemon.h"
#include "sources/config.h"
#include "sources/config_mgr.h"
#include "sources/config_reader.h"
#include "sources/msg.h"
#include "sources/system.h"
#include "sources/server.h"
#include "sources/server_pool.h"
#include "sources/client.h"
#include "sources/client_pool.h"
#include "sources/route_id.h"
#include "sources/route.h"
#include "sources/route_pool.h"
#include "sources/io.h"
#include "sources/instance.h"
#include "sources/router_cancel.h"
#include "sources/router.h"
#include "sources/console.h"
#include "sources/worker.h"
#include "sources/worker_pool.h"
#include "sources/pooler.h"
#include "sources/cron.h"
#include "sources/tls.h"

static inline void
od_pooler_server(void *arg)
{
	od_poolerserver_t *server = arg;
	od_instance_t *instance = server->system->instance;

	for (;;)
	{
		/* accepted client io is not attached to epoll context yet */
		machine_io_t *client_io;
		int rc;
		rc = machine_accept(server->io, &client_io, server->config->backlog,
		                    0, UINT32_MAX);
		if (rc == -1) {
			od_error(&instance->logger, "server", NULL, NULL,
			         "accept failed: %s",
			         machine_error(server->io));
			int errno_ = machine_errno();
			if (errno_ == EADDRINUSE)
				break;
			continue;
		}

		/* set network options */
		machine_set_nodelay(client_io, instance->config.nodelay);
		if (instance->config.keepalive > 0)
			machine_set_keepalive(client_io, 1, instance->config.keepalive);
		rc = machine_set_readahead(client_io, instance->config.readahead);
		if (rc == -1) {
			od_error(&instance->logger, "server", NULL, NULL,
			         "failed to set client readahead: %s",
			         machine_error(client_io));
			machine_close(client_io);
			machine_io_free(client_io);
			continue;
		}

		/* allocate new client */
		od_client_t *client = od_client_allocate();
		if (client == NULL) {
			od_error(&instance->logger, "server", NULL, NULL,
			         "failed to allocate client object");
			machine_close(client_io);
			machine_io_free(client_io);
			continue;
		}
		od_idmgr_generate(&instance->id_mgr, &client->id, "c");
		client->io = client_io;
		client->config_listen = server->config;
		client->tls = server->tls;
		client->time_accept = machine_time();

		/* create new client event and pass it to worker pool */
		machine_msg_t *msg;
		msg = machine_msg_create(OD_MCLIENT_NEW, sizeof(od_client_t*));
		char *msg_data = machine_msg_get_data(msg);
		memcpy(msg_data, &client, sizeof(od_client_t*));
		od_workerpool_t *worker_pool = server->system->worker_pool;
		od_workerpool_feed(worker_pool, msg);
	}
}

static inline int
od_pooler_server_start(od_pooler_t *pooler, od_configlisten_t *config,
                       struct addrinfo *addr)
{
	od_instance_t *instance = pooler->system.instance;
	od_poolerserver_t *server;
	server = malloc(sizeof(od_poolerserver_t));
	if (server == NULL) {
		od_error(&instance->logger, "pooler", NULL, NULL,
		         "failed to allocate pooler server object");
		return -1;
	}
	server->config = config;
	server->addr = addr;
	server->system = &pooler->system;

	/* create server tls */
	if (server->config->tls_mode != OD_TLS_DISABLE) {
		server->tls = od_tls_frontend(server->config);
		if (server->tls == NULL) {
			od_error(&instance->logger, "server", NULL, NULL,
			         "failed to create tls handler");
			free(server);
			return -1;
		}
	}

	/* create server io */
	server->io = machine_io_create();
	if (server->io == NULL) {
		od_error(&instance->logger, "server", NULL, NULL,
		         "failed to create pooler io");
		if (server->tls)
			machine_tls_free(server->tls);
		free(server);
		return -1;
	}

	char addr_name[128];
	od_getaddrname(server->addr, addr_name, sizeof(addr_name), 1, 1);

	/* bind to listen address and port */
	int rc;
	rc = machine_bind(server->io, server->addr->ai_addr);
	if (rc == -1) {
		od_error(&instance->logger, "server", NULL, NULL,
		         "bind to %s failed: %s",
		         addr_name,
		         machine_error(server->io));
		if (server->tls)
			machine_tls_free(server->tls);
		machine_close(server->io);
		machine_io_free(server->io);
		free(server);
		return -1;
	}

	od_log(&instance->logger, "server", NULL, NULL,
	       "listening on %s", addr_name);

	int64_t coroutine_id;
	coroutine_id = machine_coroutine_create(od_pooler_server, server);
	if (coroutine_id == -1) {
		od_error(&instance->logger, "pooler", NULL, NULL,
		         "failed to start server coroutine");
		if (server->tls)
			machine_tls_free(server->tls);
		machine_close(server->io);
		machine_io_free(server->io);
		free(server);
		return -1;
	}
	return 0;
}

static inline int
od_pooler_main(od_pooler_t *pooler)
{
	od_instance_t *instance = pooler->system.instance;
	int binded = 0;
	od_list_t *i;
	od_list_foreach(&instance->config.listen, i)
	{
		od_configlisten_t *listen;
		listen = od_container_of(i, od_configlisten_t, link);

		/* listen '*' */
		struct addrinfo *hints_ptr = NULL;
		struct addrinfo  hints;
		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		hints.ai_protocol = IPPROTO_TCP;
		char *host = listen->host;
		if (strcmp(listen->host, "*") == 0) {
			hints_ptr = &hints;
			host = NULL;
		}

		/* resolve listen address and port */
		char port[16];
		od_snprintf(port, sizeof(port), "%d", listen->port);
		struct addrinfo *ai = NULL;
		int rc;
		rc = machine_getaddrinfo(host, port, hints_ptr, &ai, UINT32_MAX);
		if (rc != 0) {
			od_error(&instance->logger, "pooler", NULL, NULL,
			         "failed to resolve %s:%d",
			          listen->host,
			          listen->port);
			continue;
		}
		pooler->addr = ai;

		/* listen resolved addresses */
		if (host) {
			rc = od_pooler_server_start(pooler, listen, ai);
			if (rc == 0)
				binded++;
			continue;
		}
		while (ai) {
			rc = od_pooler_server_start(pooler, listen, ai);
			if (rc == 0)
				binded++;
			ai = ai->ai_next;
		}
	}

	return binded;
}

static inline void
od_pooler_config_import(od_pooler_t *pooler)
{
	od_instance_t *instance = pooler->system.instance;

	od_log(&instance->logger, "config", NULL, NULL, "importing changes from '%s'",
	       instance->config_file);

	od_config_t config;
	od_config_init(&config);
	uint64_t config_version;
	config_version = od_configmgr_version_next(&instance->config_mgr);

	od_error_t error;
	od_error_init(&error);
	int rc;
	rc = od_configreader_import(&config, &error, instance->config_file,
	                            config_version);
	if (rc == -1) {
		od_error(&instance->logger, "config", NULL, NULL,
		         "%s", error.error);
		od_config_free(&config);
		return;
	}
	rc = od_config_validate(&config, &instance->logger);
	if (rc == -1) {
		od_config_free(&config);
		return;
	}

	/* Merge configuration changes.
	 *
	 * Add new routes or obsolete previous ones which are updated or not
	 * present in new config file.
	*/
	int has_updates;
	has_updates = od_config_merge(&instance->config, &instance->logger, &config);

	/* free unused settings */
	od_config_free(&config);

	if (has_updates && instance->config.log_config)
		od_config_print(&instance->config, &instance->logger, 1);
}

static inline void
od_pooler_signal_handler(void *arg)
{
	od_pooler_t *pooler = arg;
	od_instance_t *instance = pooler->system.instance;

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	int rc;
	rc = machine_signal_init(&mask);
	if (rc == -1) {
		od_error(&instance->logger, "pooler", NULL, NULL,
		         "failed to init signal handler");
		return;
	}
	for (;;)
	{
		rc = machine_signal_wait(UINT32_MAX);
		if (rc == -1)
			break;
		switch (rc) {
		case SIGTERM:
			od_log(&instance->logger, "pooler", NULL, NULL,
			       "SIGTERM received, shutting down");
			exit(0);
			break;
		case SIGINT:
			od_log(&instance->logger, "pooler", NULL, NULL,
			       "SIGINT received, shutting down");
			exit(0);
			break;
		case SIGHUP:
			od_log(&instance->logger, "pooler", NULL, NULL,
			       "SIGHUP received");
			od_pooler_config_import(pooler);
			break;
		}
	}
}

static inline void
od_pooler(void *arg)
{
	od_pooler_t *pooler = arg;
	od_instance_t *instance = pooler->instance;

	/* start router coroutine */
	int rc;
	od_router_t *router = pooler->system.router;
	rc = od_router_start(router);
	if (rc == -1)
		return;

	/* start console coroutine */
	od_console_t *console = pooler->system.console;
	rc = od_console_start(console);
	if (rc == -1)
		return;

	/* start cron coroutine */
	od_cron_t *cron = pooler->system.cron;
	rc = od_cron_start(cron);
	if (rc == -1)
		return;

	/* start worker threads */
	od_workerpool_t *worker_pool = pooler->system.worker_pool;
	rc = od_workerpool_start(worker_pool, &pooler->system, instance->config.workers);
	if (rc == -1)
		return;

	/* start signal handler coroutine */
	int64_t coroutine_id;
	coroutine_id = machine_coroutine_create(od_pooler_signal_handler, pooler);
	if (coroutine_id == -1) {
		od_error(&instance->logger, "pooler", NULL, NULL,
		         "failed to start signal handler");
		return;
	}

	/* start pooler servers */
	rc = od_pooler_main(pooler);
	if (rc == 0) {
		od_error(&instance->logger, "pooler", NULL, NULL,
		         "failed to bind any listen address");
		exit(1);
	}
}

int od_pooler_init(od_pooler_t *pooler, od_instance_t *instance)
{
	pooler->machine  = -1;
	pooler->instance = instance;
	pooler->addr     = NULL;
	memset(&pooler->system, 0, sizeof(pooler->system));
	return 0;
}

int od_pooler_start(od_pooler_t *pooler)
{
	od_instance_t *instance = pooler->system.instance;
	pooler->machine = machine_create("pooler", od_pooler, pooler);
	if (pooler->machine == -1) {
		od_error(&instance->logger, "pooler", NULL, NULL,
		         "failed to create pooler thread");
		return -1;
	}
	return 0;
}
