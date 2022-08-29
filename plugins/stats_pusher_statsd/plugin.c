#include <uwsgi.h>

/*

this is a stats pusher plugin for the statsd server:

--stats-push statsd:address[,prefix]

example:

--stats-push statsd:127.0.0.1:8125,myinstance

it exports values exposed by the metric subsystem

*/

extern struct uwsgi_server uwsgi;

struct uwsgi_stats_pusher_statsd {
    int no_workers;
    int all_gauges;
} u_stats_pusher_statsd;

static struct uwsgi_option stats_pusher_statsd_options[] = {
	{"statsd-no-workers", no_argument, 0, "disable generation of single worker metrics", uwsgi_opt_true, &u_stats_pusher_statsd.no_workers, 0},
	{"statsd-all-gauges", no_argument, 0, "push all metrics to statsd as gauges", uwsgi_opt_true, &u_stats_pusher_statsd.all_gauges, 0},
	UWSGI_END_OF_OPTIONS
};

// configuration of a statsd node
struct statsd_node {
	int fd;
	union uwsgi_sockaddr addr;
	socklen_t addr_len;
	char *prefix;
	uint16_t prefix_len;
};

static int statsd_send_metric(struct uwsgi_buffer *ub, struct uwsgi_stats_pusher_instance *uspi, char *metric, size_t metric_len, int64_t value, char type[2]) {
	struct statsd_node *sn = (struct statsd_node *) uspi->data;
	// reset the buffer
        ub->pos = 0;
	if (uwsgi_buffer_append(ub, sn->prefix, sn->prefix_len)) return -1;
	if (uwsgi_buffer_append(ub, ".", 1)) return -1;
	if (uwsgi_buffer_append(ub, metric, metric_len)) return -1;
	if (uwsgi_buffer_append(ub, ":", 1)) return -1;
        if (uwsgi_buffer_num64(ub, value)) return -1;
	if (uwsgi_buffer_append(ub, type, 2)) return -1;

        if (sendto(sn->fd, ub->buf, ub->pos, 0, (struct sockaddr *) &sn->addr.sa, sn->addr_len) < 0) {
			if (errno != EAGAIN)	// drop if we were to block
				uwsgi_error("statsd_send_metric()/sendto()");
        }

        return 0;

}


static void stats_pusher_statsd(struct uwsgi_stats_pusher_instance *uspi, time_t now, char *json, size_t json_len) {

	if (!uspi->configured) {
		struct statsd_node *sn = uwsgi_calloc(sizeof(struct statsd_node));
		char *comma = strchr(uspi->arg, ',');
		if (comma) {
			sn->prefix = comma+1;
			sn->prefix_len = strlen(sn->prefix);
			*comma = 0;
		}
		else {
			sn->prefix = "uwsgi";
			sn->prefix_len = 5;
		}

		sn->fd = uwsgi_socket_from_addr(&sn->addr, &sn->addr_len, uspi->arg, SOCK_DGRAM);
		if (sn->fd < -1) {
			if (comma) *comma = ',';
			free(sn);
			return;
		}

        uwsgi_socket_nb(sn->fd);

		if (comma) *comma = ',';
		uspi->data = sn;
		uspi->configured = 1;
	}

	// we use the same buffer for all of the packets
	struct uwsgi_buffer *ub = uwsgi_buffer_new(uwsgi.page_size);
	struct uwsgi_metric *um = uwsgi.metrics;
	while(um) {
		if (u_stats_pusher_statsd.no_workers && !uwsgi_starts_with(um->name, um->name_len, "worker.", 7)) {
		    goto next;
		}
		uwsgi_rlock(uwsgi.metrics_lock);
		// ignore return value
		if (u_stats_pusher_statsd.all_gauges || um->type == UWSGI_METRIC_GAUGE) {
			statsd_send_metric(ub, uspi, um->name, um->name_len, *um->value, "|g");
		}
		else {
			statsd_send_metric(ub, uspi, um->name, um->name_len, *um->value, "|c");
		}
		uwsgi_rwunlock(uwsgi.metrics_lock);
		if (um->reset_after_push){
			uwsgi_wlock(uwsgi.metrics_lock);
			*um->value = um->initial_value;
			uwsgi_rwunlock(uwsgi.metrics_lock);
		}
		next:
		um = um->next;
	}
	uwsgi_buffer_destroy(ub);
}

static void stats_pusher_statsd_init(void) {
        struct uwsgi_stats_pusher *usp = uwsgi_register_stats_pusher("statsd", stats_pusher_statsd);
	// we use a custom format not the JSON one
	usp->raw = 1;
}

struct uwsgi_plugin stats_pusher_statsd_plugin = {

        .name = "stats_pusher_statsd",
        .options = stats_pusher_statsd_options,
        .on_load = stats_pusher_statsd_init,
};

