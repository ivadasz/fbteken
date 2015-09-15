#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <event2/event.h>
#include <wayland-client.h>

struct event_base *evbase;
struct event *wldispev;

struct wl_display *disp;
struct wl_registry *registry;
struct wl_shm_pool *pool;

static void
registry_handle_global(void *data, struct wl_registry *reg, uint32_t id,
    const char *interface, uint32_t version)
{
	printf("interface added: %s id: 0x%08x version: 0x%08x\n",
	    interface, id, version);
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
    uint32_t name)
{
	printf("interface removed: 0x%08x\n", name);
}

static struct wl_shm_pool *
allocate_pool(size_t len)
{
	(void)len;

	/* XXX */

	return NULL;
}

static void
destroy_pool(struct wl_shm_pool *p)
{
	/* XXX */
}

static void
wldisp_read_handler(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	wl_display_dispatch(disp);
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-h]\n", getprogname());
	exit(1);
}

int
main(int argc, char *argv[])
{
	int ch;

	while ((ch = getopt(argc, argv, "h")) != -1) {
		switch (ch) {
		case 'h':
		default:
			usage();
		}
	}

	evbase = event_base_new();

	disp = wl_display_connect(NULL);
	if (disp == NULL)
		exit(1);
	registry = wl_display_get_registry(disp);
	if (registry == NULL) {
		perror("wl_display_get_registry");
		exit(1);
	}
	struct wl_registry_listener registry_listener = {
		registry_handle_global,
		registry_handle_global_remove
	};
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_flush(disp);

	pool = allocate_pool(400*300*4);

	wldispev = event_new(evbase, wl_display_get_fd(disp),
	    EV_READ | EV_PERSIST, wldisp_read_handler, NULL);
	event_add(wldispev, NULL);

	event_base_loop(evbase, 0);

	event_del(wldispev);
	event_free(wldispev);

	destroy_pool(pool);
	wl_display_disconnect(disp);

	event_base_free(evbase);

	exit(0);
}
