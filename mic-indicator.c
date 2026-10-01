/* mic-indicator.c — microphone activity dot, GTK4 layer-shell overlay.
 *
 * A 10px dot in the top-right corner, visible only while some application is
 * capturing from a non-monitor microphone source:
 *
 *   green  — capturing, default source unmuted
 *   peach  — capturing, default source muted
 *   hidden — not capturing
 *
 * Native libpipewire client (no pw-dump/pw-mon subprocesses): a pw_thread_loop
 * binds Node/Link/Metadata globals, maintains a small state store updated by
 * events, and recomputes the state on every change — a direct port of the
 * former mic-indicator.py compute_state(). State changes cross to the GTK
 * thread via g_idle_add.
 *
 * Requires: gtk4, gtk4-layer-shell, libpipewire.
 */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/utils/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AREA 20	  /* drawing-area / window size in px (dot sits 5px from edges) */
#define DOT_RADIUS 5 /* 10px dot */
static const double GREEN[] = { 0x57 / 255.0, 0xAB / 255.0, 0x5A / 255.0 };
static const double PEACH[] = { 0xCC / 255.0, 0x6B / 255.0, 0x2C / 255.0 };

enum state { ST_OFF, ST_ACTIVE, ST_MUTED };

struct data;
struct node;

static void node_free(struct node *n);
static gboolean ui_set_state(gpointer user_data);

/* ─────────────────────────────────────────────────────── detection ── */

struct node {
	struct data *d;
	char *media_class;
	char *node_name;
	bool muted;         /* Props:mute — unreliable with a hardware mute */
	uint32_t device_id; /* device.id global, 0 if none */
	struct pw_node *proxy;
	struct spa_hook listener;
};

/* Device route state. On ALSA sources with route.hw-mute the node's
 * Props:mute stays false; the active input route holds the real mute. */
struct device {
	struct data *d;
	struct pw_device *proxy;
	struct spa_hook listener;
	bool input_muted;
	bool input_mute_known;
};

struct link {
	uint32_t id;     /* global id */
	uint32_t output; /* output-node-id */
	uint32_t input;  /* input-node-id */
	struct spa_hook listener;
};

struct data {
	struct pw_thread_loop *loop;
	struct pw_context *ctx;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook core_listener, registry_listener, meta_listener;
	struct spa_source *reconnect_source;

	GHashTable *nodes;    /* global id -> struct node */
	GHashTable *devices;  /* global id -> struct device */
	GPtrArray *links;	  /* struct link */
	char *default_source;

	enum state last;
};

/* Emit state transitions to stdout (parity checks, journald) and hand the
 * new state to the GTK thread. Runs on the PipeWire thread. */
static void recompute(struct data *d)
{
	bool active = false;
	for (guint i = 0; i < d->links->len && !active; i++) {
		struct link *l = d->links->pdata[i];
		struct node *src = g_hash_table_lookup(d->nodes, GUINT_TO_POINTER(l->output));
		struct node *dst = g_hash_table_lookup(d->nodes, GUINT_TO_POINTER(l->input));
		if (!src || !dst)
			continue;
		if (!spa_streq(dst->media_class, "Stream/Input/Audio"))
			continue;
		if (!spa_streq(src->media_class, "Audio/Source"))
			continue;
		if (spa_strendswith(src->node_name, ".monitor"))
			continue;
		active = true;
	}

	enum state s = ST_OFF;
	if (active) {
		s = ST_ACTIVE;
		if (d->default_source) {
			GHashTableIter it;
			gpointer v;
			g_hash_table_iter_init(&it, d->nodes);
			while (g_hash_table_iter_next(&it, NULL, &v)) {
				struct node *n = v;
				if (spa_streq(n->node_name, d->default_source)) {
					/* Prefer the device's active input route:
					 * with a hardware mute the node's
					 * Props:mute stays false. */
					bool muted = n->muted;
					if (n->device_id) {
						struct device *dev = g_hash_table_lookup(
							d->devices,
							GUINT_TO_POINTER(n->device_id));
						if (dev && dev->input_mute_known)
							muted = dev->input_muted;
					}
					if (muted)
						s = ST_MUTED;
					break;
				}
			}
		}
	}

	if (s != d->last) {
		d->last = s;
		printf("%s\n", s == ST_OFF ? "off" : s == ST_ACTIVE ? "active" : "muted");
		fflush(stdout);
		g_idle_add(ui_set_state, GINT_TO_POINTER(s));
	}
}

static void node_free(struct node *n)
{
	g_free(n->media_class);
	g_free(n->node_name);
	g_free(n);
}

/* ── node events ── */

static void on_node_info(void *_n, const struct pw_node_info *info)
{
	struct node *n = _n;
	struct data *d = n->d;
	if (!g_hash_table_contains(d->nodes, GUINT_TO_POINTER(info->id)))
		return; /* global already removed */

	/* Info updates can carry an empty props dict ("unchanged") — only
	 * overwrite keys that are actually present, or the name gets wiped. */
	const char *v = spa_dict_lookup(info->props, "media.class");
	if (v) {
		g_free(n->media_class);
		n->media_class = g_strdup(v);
	}
	v = spa_dict_lookup(info->props, "node.name");
	if (v) {
		g_free(n->node_name);
		n->node_name = g_strdup(v);
	}
	v = spa_dict_lookup(info->props, "device.id");
	if (v)
		n->device_id = (uint32_t)strtoul(v, NULL, 10);

	/* Subscribe Props on every Audio/Source, not just the current default —
	 * otherwise switching defaults leaves the new source's mute unknown. */
	if (spa_streq(n->media_class, "Audio/Source")) {
		uint32_t params[1] = { SPA_PARAM_Props };
		pw_node_subscribe_params(n->proxy, params, 1);
	}
	recompute(d);
}

static void on_node_param(void *_n, int seq, uint32_t id, uint32_t index,
			  uint32_t next, const struct spa_pod *param)
{
	struct node *n = _n;
	(void)seq;
	(void)index;
	(void)next;
	if (!param || id != SPA_PARAM_Props)
		return;

	const struct spa_pod_prop *p = spa_pod_find_prop(param, NULL, SPA_PROP_mute);
	if (!p)
		return;
	spa_pod_get_bool(&p->value, &n->muted);
	recompute(n->d);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info,
	.param = on_node_param,
};

/* ── device events ── */

static void on_device_info(void *_dev, const struct pw_device_info *info)
{
	struct device *dev = _dev;
	if (!g_hash_table_contains(dev->d->devices, GUINT_TO_POINTER(info->id)))
		return; /* global already removed */
	uint32_t params[1] = { SPA_PARAM_Route };
	pw_device_subscribe_params(dev->proxy, params, 1);
}

static void on_device_param(void *_dev, int seq, uint32_t id, uint32_t index,
			    uint32_t next, const struct spa_pod *param)
{
	struct device *dev = _dev;
	(void)seq;
	(void)index;
	(void)next;
	if (!param || id != SPA_PARAM_Route)
		return;

	/* Only the active route per direction is reported; pick the input one
	 * and read its nested Props:mute. */
	const struct spa_pod_prop *p =
		spa_pod_find_prop(param, NULL, SPA_PARAM_ROUTE_direction);
	uint32_t direction;
	if (!p || spa_pod_get_id(&p->value, &direction) < 0 ||
	    direction != SPA_DIRECTION_INPUT)
		return;
	p = spa_pod_find_prop(param, NULL, SPA_PARAM_ROUTE_props);
	if (!p)
		return;
	const struct spa_pod_prop *m =
		spa_pod_find_prop(&p->value, NULL, SPA_PROP_mute);
	if (!m)
		return;
	spa_pod_get_bool(&m->value, &dev->input_muted);
	dev->input_mute_known = true;
	recompute(dev->d);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = on_device_info,
	.param = on_device_param,
};

/* ── link events ── */

static void on_link_info(void *_d, const struct pw_link_info *info)
{
	struct data *d = _d;
	struct link *l = NULL;
	for (guint i = 0; i < d->links->len; i++) {
		struct link *e = d->links->pdata[i];
		if (e->id == info->id) {
			l = e;
			break;
		}
	}
	if (!l) {
		l = g_new(struct link, 1);
		g_ptr_array_add(d->links, l);
	}
	l->id = info->id;
	l->output = info->output_node_id;
	l->input = info->input_node_id;
	recompute(d);
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = on_link_info,
};

/* ── metadata events ── */

static int on_meta_prop(void *_d, uint32_t subject, const char *key,
			const char *type, const char *value)
{
	struct data *d = _d;
	(void)subject;
	(void)type;
	if (!key || !spa_streq(key, "default.audio.source"))
		return 0;

	g_free(d->default_source);
	d->default_source = NULL;
	if (value) {
		/* Usually {"name":"..."} (Spa:String:JSON); accept a bare name too. */
		char name[256];
		if (spa_json_str_object_find(value, strlen(value), "name",
					      name, sizeof(name)) > 0)
			d->default_source = g_strdup(name);
		else
			d->default_source = g_strdup(value);
	}
	recompute(d);
	return 0;
}

static const struct pw_metadata_events meta_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = on_meta_prop,
};

/* ── registry events ── */

static void on_global(void *_d, uint32_t id, uint32_t permissions,
		      const char *type, uint32_t version,
		      const struct spa_dict *props)
{
	struct data *d = _d;
	(void)permissions;
	(void)version;

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		void *proxy = pw_registry_bind(d->registry, id, type,
					       PW_VERSION_NODE, 0);
		if (!proxy)
			return;
		struct node *n = g_new0(struct node, 1);
		n->d = d;
		n->proxy = proxy;
		n->muted = true; /* don't show "live" before the mute state is known */
		g_hash_table_insert(d->nodes, GUINT_TO_POINTER(id), n);
		pw_node_add_listener(proxy, &n->listener, &node_events, n);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Device)) {
		void *proxy = pw_registry_bind(d->registry, id, type,
					       PW_VERSION_DEVICE, 0);
		if (!proxy)
			return;
		struct device *dev = g_new0(struct device, 1);
		dev->d = d;
		dev->proxy = proxy;
		g_hash_table_insert(d->devices, GUINT_TO_POINTER(id), dev);
		pw_device_add_listener(proxy, &dev->listener, &device_events, dev);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
		void *proxy = pw_registry_bind(d->registry, id, type,
					       PW_VERSION_LINK, 0);
		if (!proxy)
			return;
		struct link *l = g_new0(struct link, 1);
		l->id = id;
		g_ptr_array_add(d->links, l);
		pw_link_add_listener(proxy, &l->listener, &link_events, d);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
		const char *mname = spa_dict_lookup(props, "metadata.name");
		if (!mname || !spa_streq(mname, "default"))
			return; /* settings/schema metadata: noise, don't bind */
		struct pw_metadata *meta = pw_registry_bind(d->registry, id, type,
						    PW_VERSION_METADATA, 0);
		if (!meta)
			return;
		pw_metadata_add_listener(meta, &d->meta_listener,
					 &meta_events, d);
	}
}

static void on_global_remove(void *_d, uint32_t id)
{
	struct data *d = _d;

	struct node *n = g_hash_table_lookup(d->nodes, GUINT_TO_POINTER(id));
	if (n) {
		pw_proxy_destroy((struct pw_proxy *)n->proxy);
		g_hash_table_remove(d->nodes, GUINT_TO_POINTER(id));
		recompute(d);
		return;
	}
	struct device *dev = g_hash_table_lookup(d->devices, GUINT_TO_POINTER(id));
	if (dev) {
		pw_proxy_destroy((struct pw_proxy *)dev->proxy);
		g_hash_table_remove(d->devices, GUINT_TO_POINTER(id));
		recompute(d);
		return;
	}
	for (guint i = 0; i < d->links->len; i++) {
		struct link *l = d->links->pdata[i];
		if (l->id == id) {
			g_ptr_array_remove_index(d->links, i);
			recompute(d);
			return;
		}
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
	.global_remove = on_global_remove,
};

/* ── core events / reconnect ── */

static void do_reconnect(void *_d);

static void clear_store(struct data *d)
{
	g_hash_table_remove_all(d->nodes);
	g_hash_table_remove_all(d->devices);
	while (d->links->len)
		g_ptr_array_remove_index(d->links, d->links->len - 1);
	g_clear_pointer(&d->default_source, g_free);
}

static void on_core_error(void *_d, uint32_t id, int seq, int res,
			  const char *message)
{
	struct data *d = _d;
	(void)seq;
	(void)res;
	if (id == PW_ID_CORE && !d->reconnect_source) {
		fprintf(stderr, "pipewire error: %s — reconnecting\n", message);
		d->reconnect_source = pw_loop_add_idle(
			pw_thread_loop_get_loop(d->loop), true,
			do_reconnect, d);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void connect_pw(struct data *d)
{
	while (true) {
		d->ctx = pw_context_new(pw_thread_loop_get_loop(d->loop), NULL, 0);
		d->core = d->ctx ? pw_context_connect(d->ctx, NULL, 0) : NULL;
		if (d->core)
			break;
		if (d->ctx)
			pw_context_destroy(d->ctx);
		sleep(1); /* ponytail: fixed 1s backoff, fine for a desktop app */
	}
	pw_core_add_listener(d->core, &d->core_listener, &core_events, d);
	d->registry = pw_core_get_registry(d->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(d->registry, &d->registry_listener,
				 &registry_events, d);
}

/* Runs as an idle source on the PipeWire loop (i.e. between event
 * dispatches), so destroying the old context from within a core error
 * callback is safe. */
static void do_reconnect(void *_d)
{
	struct data *d = _d;
	pw_loop_destroy_source(pw_thread_loop_get_loop(d->loop),
			       d->reconnect_source);
	d->reconnect_source = NULL;

	clear_store(d);
	if (d->ctx) {
		pw_context_destroy(d->ctx);
		d->ctx = NULL;
		d->core = NULL;
		d->registry = NULL;
	}
	connect_pw(d);
	recompute(d); /* empty store → off */
}

static void start_pw(struct data *d)
{
	pw_init(NULL, NULL);
	d->nodes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
					 (GDestroyNotify)node_free);
	d->devices = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
					   (GDestroyNotify)g_free);
	d->links = g_ptr_array_new_with_free_func(g_free);

	d->loop = pw_thread_loop_new("mic-indicator-pw", NULL);
	connect_pw(d); /* before start: no lock needed, pw thread owns it after */
	pw_thread_loop_start(d->loop);
}

/* ──────────────────────────────────────────────────────────── UI ── */

static GtkWidget *window; /* singleton: activate can fire more than once */
static GtkWidget *area;
static enum state ui_state = ST_OFF;

/* Runs on the GTK thread; state passed by value via the pointer. */
static gboolean ui_set_state(gpointer user_data)
{
	ui_state = GPOINTER_TO_INT(user_data);
	gtk_widget_queue_draw(area); /* the DrawingArea, not the window: queueing
				      * the window does not re-run the draw func */
	return G_SOURCE_REMOVE;
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		 gpointer user_data)
{
	(void)area;
	(void)user_data;
	if (ui_state == ST_OFF)
		return;
	const double *color = ui_state == ST_MUTED ? PEACH : GREEN;
	double cx = width / 2.0, cy = height / 2.0;
	/* Soft glow: translucent rings under the solid dot. */
	for (int i = 0; i < 2; i++) {
		double radius = DOT_RADIUS + 4 - 2 * i;
		double alpha = 0.10 + 0.08 * i;
		cairo_set_source_rgba(cr, color[0], color[1], color[2], alpha);
		cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
		cairo_fill(cr);
	}
	cairo_set_source_rgba(cr, color[0], color[1], color[2], 1.0);
	cairo_arc(cr, cx, cy, DOT_RADIUS, 0, 2 * G_PI);
	cairo_fill(cr);
}

static void build_window(GtkApplication *app)
{
	window = gtk_application_window_new(app);
	gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
	gtk_widget_set_can_focus(window, FALSE);
	gtk_widget_add_css_class(window, "mic-indicator");

	gtk_layer_init_for_window(GTK_WINDOW(window));
	gtk_layer_set_namespace(GTK_WINDOW(window), "mic-indicator");
	gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
	gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
				     GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
	gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1); /* never reserve space */
	gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
	gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_string(
		provider,
		".mic-indicator { background: transparent;"
		" background-image: none; box-shadow: none; }");
	gtk_style_context_add_provider_for_display(
		gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);

	area = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), AREA);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), AREA);
	gtk_widget_set_can_target(area, FALSE);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, NULL, NULL);
	gtk_window_set_child(GTK_WINDOW(window), area);
}

static void on_activate(GtkApplication *app, gpointer user_data)
{
	struct data *d = user_data;
	if (!window) {
		build_window(app);
		start_pw(d);
	}
	gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
	/* A 20x20 dot doesn't need GTK's Vulkan renderer: cairo is software but
	 * trivially cheap here, and drops the GPU stack (~38MB RSS, 5 threads,
	 * 12 fds) plus the VK_SUBOPTIMAL_KHR swapchain warning. Launched via the
	 * service, niri, or a shell — setting it here covers all three. An
	 * explicit GSK_RENDERER still wins. */
	setenv("GSK_RENDERER", "cairo", 0);

	struct data d = { .last = ST_OFF };
	GtkApplication *app = gtk_application_new("dev.local.mic-indicator",
						  G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), &d);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
