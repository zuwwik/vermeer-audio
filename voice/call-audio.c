// SPDX-License-Identifier: GPL-2.0-only
/* GNOME Calls' CallAudio interface backed by native AudioReach voice.
 * ModemManager is observed read-only; this daemon never places calls.
 */
#include "voice-session.h"
#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <signal.h>

#define BUS_NAME "org.mobian_project.CallAudio"
#define OBJECT_PATH "/org/mobian_project/CallAudio"
#define MM_NAME "org.freedesktop.ModemManager1"
#define CALL_IFACE MM_NAME ".Call"
#define VOICE_SINK "vermeer_voice"
#define VOICE_SOURCE "vermeer_voice_mic"

enum action { MODE, SPEAKER, MUTE };

struct request {
	enum action action;
	bool value;
	GDBusMethodInvocation *invocation;
};

struct card {
	char *name;
	char *profile;
};

struct app {
	struct voice_session voice;
	GMainLoop *loop;
	guint hardware_watch;
	GDBusConnection *bus;
	GDBusObjectManager *modems;
	GHashTable *calls;
	pa_glib_mainloop *pulse_loop;
	pa_context *pulse;
	GPtrArray *cards;
	GQueue requests;
	struct request *current;
	char *state_file;
	char *default_sink;
	char *default_source;
	char *restored_sink;
	char *restored_source;
	GKeyFile *remembering;
	char *desktop_state;
	unsigned int restore_attempts;
	uint32_t sink_index;
	uint32_t sink_module;
	uint32_t source_index;
	uint32_t source_module;
	pa_volume_t volume;
	pa_volume_t mic_volume;
	bool output_muted;
	bool sink_ready;
	bool source_ready;
	bool changing_route;
	unsigned int media_stop_attempts;
	unsigned int card_index;
	unsigned int start_attempts;
	unsigned int source_start_attempts;
	unsigned int sink_start_attempts;
	bool restoring;
	bool restoring_off;
	unsigned int release_attempts;
	bool release_pending;
	bool result;
	bool restore_ok;
	bool active;
	bool speaker;
	bool muted;
	bool automatic_active;
	bool automatic_connected;
	bool quitting;
	bool hardware_lost;
};

static const char introspection[] =
	"<node><interface name='" BUS_NAME "'>"
	"<method name='SelectMode'><arg direction='in' type='u'/>"
	"<arg direction='out' type='b'/></method>"
	"<method name='EnableSpeaker'><arg direction='in' type='b'/>"
	"<arg direction='out' type='b'/></method>"
	"<method name='MuteMic'><arg direction='in' type='b'/>"
	"<arg direction='out' type='b'/></method>"
	"<property name='AudioMode' type='u' access='read'/>"
	"<property name='SpeakerState' type='u' access='read'/>"
	"<property name='MicState' type='u' access='read'/>"
	"</interface></node>";

static void process_requests(struct app *app);
static void profile_next(struct app *app);
static void restore_profiles(struct app *app, bool result);
static void media_start(struct app *app);
static void media_stop(struct app *app);
static void remember_desktop(struct app *app);

static void card_free(gpointer pointer)
{
	struct card *card = pointer;
	g_free(card->name);
	g_free(card->profile);
	g_free(card);
}

static void notify(struct app *app)
{
	GVariantBuilder properties;

	g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&properties, "{sv}", "AudioMode", g_variant_new_uint32(app->active));
	g_variant_builder_add(&properties, "{sv}", "SpeakerState", g_variant_new_uint32(app->speaker));
	g_variant_builder_add(&properties, "{sv}", "MicState", g_variant_new_uint32(!app->muted));
	g_dbus_connection_emit_signal(app->bus, NULL, OBJECT_PATH,
		"org.freedesktop.DBus.Properties", "PropertiesChanged",
		g_variant_new("(s@a{sv}@as)", BUS_NAME, g_variant_builder_end(&properties),
			      g_variant_new_strv(NULL, 0)), NULL);
}

static gboolean process_idle(gpointer data)
{
	process_requests(data);
	return G_SOURCE_REMOVE;
}

static void complete_request(struct app *app, bool success)
{
	struct request *request = app->current;

	if (app->hardware_lost)
		return;
	notify(app);
	if (request->invocation) {
		g_dbus_method_invocation_return_value(request->invocation,
						     g_variant_new("(b)", success));
		g_object_unref(request->invocation);
	}
	if (!success)
		g_warning("Call audio operation %u failed", request->action);
	g_free(request);
	app->current = NULL;
	remember_desktop(app);
	if (app->quitting && !app->active && g_queue_is_empty(&app->requests)) {
		g_main_loop_quit(app->loop);
		return;
	}
	g_idle_add(process_idle, app);
}

static bool save_cards(struct app *app)
{
	g_autoptr(GKeyFile) state = g_key_file_new();
	g_autoptr(GError) error = NULL;
	g_autofree char *contents = NULL;
	gsize length;

	for (unsigned int i = 0; i < app->cards->len; i++) {
		struct card *card = g_ptr_array_index(app->cards, i);
		g_autofree char *group = g_strdup_printf("card%u", i);
		g_key_file_set_string(state, group, "name", card->name);
		g_key_file_set_string(state, group, "profile", card->profile);
	}
	if (app->default_sink)
		g_key_file_set_string(state, "desktop", "default-sink", app->default_sink);
	if (app->default_source)
		g_key_file_set_string(state, "desktop", "default-source", app->default_source);
	contents = g_key_file_to_data(state, &length, &error);
	if (!contents || !g_file_set_contents_full(app->state_file, contents, length,
				G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
				0600, &error)) {
		g_warning("Cannot save audio restoration state: %s", error->message);
		return false;
	}
	return true;
}

static void load_cards(struct app *app)
{
	g_autoptr(GKeyFile) state = g_key_file_new();
	g_auto(GStrv) groups = NULL;
	gsize count;

	if (!g_key_file_load_from_file(state, app->state_file, 0, NULL))
		return;
	app->default_sink = g_key_file_get_string(state, "desktop", "default-sink", NULL);
	app->default_source = g_key_file_get_string(state, "desktop", "default-source", NULL);
	groups = g_key_file_get_groups(state, &count);
	for (gsize i = 0; i < count && i < 8; i++) {
		struct card *card = g_new0(struct card, 1);
		card->name = g_key_file_get_string(state, groups[i], "name", NULL);
		card->profile = g_key_file_get_string(state, groups[i], "profile", NULL);
		if (!card->name || !card->profile) {
			card_free(card);
			continue;
		}
		g_ptr_array_add(app->cards, card);
	}
}

static void restore_after_media(struct app *app)
{
	voice_session_stop(&app->voice);
	app->active = false;
	app->speaker = false;
	app->muted = false;
	app->voice.muted = false;
	app->restoring = true;
	app->restoring_off = true;
	app->restore_ok = true;
	app->card_index = 0;
	profile_next(app);
}

static void restore_profiles(struct app *app, bool result)
{
	app->result = result;
	app->changing_route = false;
	media_stop(app);
}

static void pulse_operation(pa_operation *operation)
{
	if (operation)
		pa_operation_unref(operation);
}

static bool desktop_idle(struct app *app)
{
	return !app->active && !app->current && !app->cards->len &&
		!app->hardware_lost && !app->quitting;
}

static void remember_source(pa_context *context, const pa_source_info *info, int end, void *data)
{
	struct app *app = data;
	(void)context;
	if (!end && info) {
		if (info->monitor_of_sink == PA_INVALID_INDEX && strcmp(info->name, VOICE_SOURCE))
			g_key_file_set_string(app->remembering, "desktop", "default-source", info->name);
		return;
	}
	if (!end)
		return;
	/* Never replace a working snapshot with the transient dummy output
	 * exposed while the ALSA device disappears or changes profiles.
	 */
	if (desktop_idle(app) &&
	    g_key_file_has_key(app->remembering, "desktop", "default-sink", NULL)) {
		g_free(app->desktop_state);
		app->desktop_state = g_key_file_to_data(app->remembering, NULL, NULL);
	}
	g_clear_pointer(&app->remembering, g_key_file_unref);
}

static void remember_sink(pa_context *context, const pa_sink_info *info, int end, void *data)
{
	struct app *app = data;
	pa_operation *operation;
	if (!end && info) {
		if (strcmp(info->name, "auto_null") && strcmp(info->name, VOICE_SINK))
			g_key_file_set_string(app->remembering, "desktop", "default-sink", info->name);
		return;
	}
	if (!end)
		return;
	operation = pa_context_get_source_info_by_name(context, NULL, remember_source, app);
	if (operation)
		pa_operation_unref(operation);
	else
		remember_source(context, NULL, -1, app);
}

static void remember_card(pa_context *context, const pa_card_info *info, int end, void *data)
{
	struct app *app = data;
	const char *id;
	g_autofree char *group = NULL;
	pa_operation *operation;
	if (!end && info) {
		id = pa_proplist_gets(info->proplist, "alsa.id");
		if (id && !strcmp(id, "SM8550VERMEER") && info->active_profile2) {
			g_key_file_set_string(app->remembering, info->name, "name", info->name);
			g_key_file_set_string(app->remembering, info->name, "profile", info->active_profile2->name);
		}
		return;
	}
	if (!end)
		return;
	group = g_key_file_get_start_group(app->remembering);
	if (end < 0 || !group) {
		g_clear_pointer(&app->remembering, g_key_file_unref);
		return;
	}
	operation = pa_context_get_sink_info_by_name(context, NULL, remember_sink, app);
	if (operation)
		pa_operation_unref(operation);
	else
		remember_sink(context, NULL, -1, app);
}

static void remember_desktop(struct app *app)
{
	pa_operation *operation;
	if (!desktop_idle(app) || app->remembering ||
	    pa_context_get_state(app->pulse) != PA_CONTEXT_READY)
		return;
	app->remembering = g_key_file_new();
	operation = pa_context_get_card_info_list(app->pulse, remember_card, app);
	if (operation)
		pa_operation_unref(operation);
	else
		g_clear_pointer(&app->remembering, g_key_file_unref);
}

static bool application_stream(pa_proplist *properties)
{
	const char *media_class = pa_proplist_gets(properties, "media.class");
	const char *loopback = pa_proplist_gets(properties, "bluez5.loopback");
	return !(media_class && g_str_has_suffix(media_class, "/Internal")) &&
		!(loopback && !strcmp(loopback, "true"));
}

static void move_playback(pa_context *context, const pa_sink_input_info *info, int end, void *data)
{
	char *target = data;
	if (end) {
		g_free(target);
		return;
	}
	if (info && info->client != PA_INVALID_INDEX && info->owner_module == PA_INVALID_INDEX &&
	    application_stream(info->proplist))
		pulse_operation(pa_context_move_sink_input_by_name(context, info->index, target, NULL, NULL));
}

static void move_recording(pa_context *context, const pa_source_output_info *info, int end, void *data)
{
	char *target = data;
	if (end) {
		g_free(target);
		return;
	}
	if (info && info->client != PA_INVALID_INDEX && info->owner_module == PA_INVALID_INDEX &&
	    application_stream(info->proplist))
		pulse_operation(pa_context_move_source_output_by_name(context, info->index, target, NULL, NULL));
}

static void restore_done(pa_context *context, int success, void *data)
{
	struct app *app = data;
	if (!success) {
		g_warning("Cannot restore default audio device; keeping recovery state");
		complete_request(app, false);
		return;
	}
	pulse_operation(pa_context_get_sink_input_info_list(context, move_playback, g_strdup(app->restored_sink)));
	if (app->restored_source)
		pulse_operation(pa_context_get_source_output_info_list(context, move_recording,
							      g_strdup(app->restored_source)));
	g_ptr_array_set_size(app->cards, 0);
	if (remove(app->state_file) && errno != ENOENT)
		g_warning("Could not remove saved audio state");
	g_clear_pointer(&app->default_sink, g_free);
	g_clear_pointer(&app->default_source, g_free);
	g_message("Desktop output and microphone restored");
	complete_request(app, app->result);
}

static void restore_sink_default(pa_context *context, int success, void *data)
{
	struct app *app = data;
	pa_operation *operation;
	if (!success || !app->restored_source) {
		restore_done(context, success, app);
		return;
	}
	operation = pa_context_set_default_source(context, app->restored_source, restore_done, app);
	if (operation)
		pa_operation_unref(operation);
	else
		restore_done(context, false, app);
}

static gboolean restore_probe(gpointer data);

static void restore_sources(pa_context *context, const pa_source_info *info, int end, void *data)
{
	struct app *app = data;
	const char *id;
	pa_operation *operation;
	if (!end && info) {
		id = pa_proplist_gets(info->proplist, "alsa.id");
		if ((app->default_source && !strcmp(info->name, app->default_source)) ||
		    (!app->restored_source && info->monitor_of_sink == PA_INVALID_INDEX &&
		     id && !strcmp(id, "SM8550VERMEER"))) {
			g_free(app->restored_source);
			app->restored_source = g_strdup(info->name);
		}
		return;
	}
	if (!end)
		return;
	/* Profile acknowledgement can precede WirePlumber's format negotiation.
	 * Wait for actual Pulse devices, then change defaults and move streams.
	 */
	if (!app->restored_sink || (app->default_source && !app->restored_source)) {
		if (!app->restore_attempts)
			g_message("Waiting for desktop output '%s' and microphone '%s'",
				  app->default_sink ?: "built-in", app->default_source ?: "built-in");
		if (++app->restore_attempts < 50)
			g_timeout_add(100, restore_probe, app);
		else {
			g_warning("Desktop audio devices did not become ready; keeping recovery state");
			complete_request(app, false);
		}
		return;
	}
	operation = pa_context_set_default_sink(context, app->restored_sink, restore_sink_default, app);
	if (operation)
		pa_operation_unref(operation);
	else
		restore_done(context, false, app);
}

static void restore_sinks(pa_context *context, const pa_sink_info *info, int end, void *data)
{
	struct app *app = data;
	const char *id;
	pa_operation *operation;
	if (!end && info) {
		id = pa_proplist_gets(info->proplist, "alsa.id");
		if ((app->default_sink && !strcmp(info->name, app->default_sink)) ||
		    (!app->restored_sink && id && !strcmp(id, "SM8550VERMEER"))) {
			g_free(app->restored_sink);
			app->restored_sink = g_strdup(info->name);
		}
		return;
	}
	if (!end)
		return;
	operation = pa_context_get_source_info_list(context, restore_sources, app);
	if (operation)
		pa_operation_unref(operation);
	else
		restore_sources(context, NULL, -1, app);
}

static gboolean restore_probe(gpointer data)
{
	struct app *app = data;
	pa_operation *operation;
	g_clear_pointer(&app->restored_sink, g_free);
	g_clear_pointer(&app->restored_source, g_free);
	operation = pa_context_get_sink_info_list(app->pulse, restore_sinks, app);
	if (operation)
		pa_operation_unref(operation);
	else
		restore_sinks(app->pulse, NULL, -1, app);
	return G_SOURCE_REMOVE;
}

static void sink_volume(pa_context *context, const pa_sink_info *info, int end, void *data)
{
	struct app *app = data;
	unsigned int level;
	(void)context;
	if (end || !info || strcmp(info->name, VOICE_SINK))
		return;
	app->sink_index = info->index;
	if (!app->active || !app->sink_ready)
		return;
	app->volume = MIN(pa_cvolume_avg(&info->volume), PA_VOLUME_NORM);
	app->output_muted = info->mute;
	level = ((uint64_t)app->volume * 5 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM;
	if (voice_session_volume(&app->voice, level, info->mute || !app->volume) < 0)
		g_warning("Cannot update modem call volume");
}

static void source_volume(pa_context *context, const pa_source_info *info, int end, void *data)
{
	struct app *app = data;
	bool previous = app->muted;
	unsigned int gain;
	(void)context;
	if (end || !info || strcmp(info->name, VOICE_SOURCE))
		return;
	app->source_index = info->index;
	if (!app->active || !app->source_ready)
		return;
	app->mic_volume = MIN(pa_cvolume_avg(&info->volume), PA_VOLUME_NORM);
	g_debug("Microphone event mute=%d, request=%d", info->mute,
		app->current ? (int)app->current->action : -1);
	app->muted = app->current && app->current->action == MUTE ? app->current->value : info->mute;
	gain = pa_sw_volume_to_linear(app->mic_volume) * 8192 + 0.5;
	g_debug("Microphone volume=%u, gain=%u", app->mic_volume, gain);
	if (voice_session_capture(&app->voice, gain, app->muted) < 0)
		g_warning("Cannot update modem microphone gain/mute");
	if (previous != app->muted)
		notify(app);
}

static void pulse_event(pa_context *context, pa_subscription_event_type_t event,
			uint32_t index, void *data)
{
	struct app *app = data;
	if ((event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) != PA_SUBSCRIPTION_EVENT_REMOVE)
		remember_desktop(app);
	if ((event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK &&
	    (event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) != PA_SUBSCRIPTION_EVENT_REMOVE &&
	    app->sink_ready && (app->sink_index == PA_INVALID_INDEX || index == app->sink_index))
		pulse_operation(pa_context_get_sink_info_by_index(context, index, sink_volume, app));
	if ((event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE &&
	    (event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) != PA_SUBSCRIPTION_EVENT_REMOVE &&
	    app->source_ready && (app->source_index == PA_INVALID_INDEX || index == app->source_index))
		pulse_operation(pa_context_get_source_info_by_index(context, index, source_volume, app));
}

static void move_input(pa_context *context, const pa_sink_input_info *info, int end, void *data)
{
	struct app *app = data;
	if (!end && info && info->client != PA_INVALID_INDEX && info->owner_module == PA_INVALID_INDEX &&
	    application_stream(info->proplist) && app->active && app->sink_ready)
		pulse_operation(pa_context_move_sink_input_by_name(context, info->index,
							 VOICE_SINK, NULL, NULL));
}

static void source_initialized(pa_context *context, int success, void *data)
{
	struct app *app = data;
	(void)success;
	if (app->hardware_lost)
		return;
	app->source_ready = true;
	pulse_operation(pa_context_get_source_info_by_name(context, VOICE_SOURCE, source_volume, app));
	pulse_operation(pa_context_get_source_output_info_list(context, move_recording, g_strdup(VOICE_SOURCE)));
	g_message("PipeWire call output and microphone available");
	complete_request(app, app->result);
}

static gboolean source_probe(gpointer data);

static void source_probe_result(pa_context *context, const pa_source_info *info, int end, void *data)
{
	struct app *app = data;
	pa_cvolume volume;
	pa_operation *operation;
	if (app->hardware_lost)
		return;
	if (end) {
		if (app->source_index != PA_INVALID_INDEX)
			return;
		if (++app->source_start_attempts < 50)
			g_timeout_add(100, source_probe, app);
		else {
			g_warning("PipeWire call microphone did not become ready");
			complete_request(app, false);
		}
		return;
	}
	if (!info || strcmp(info->name, VOICE_SOURCE))
		return;
	app->source_index = info->index;
	pulse_operation(pa_context_set_source_volume_by_name(context, VOICE_SOURCE,
		pa_cvolume_set(&volume, 2, app->mic_volume), NULL, NULL));
	operation = pa_context_set_source_mute_by_name(context, VOICE_SOURCE, app->muted, source_initialized, app);
	if (operation)
		pa_operation_unref(operation);
	else
		source_initialized(context, false, app);
}

static gboolean source_probe(gpointer data)
{
	struct app *app = data;
	pa_operation *operation = pa_context_get_source_info_by_name(app->pulse,
		VOICE_SOURCE, source_probe_result, app);
	if (operation)
		pa_operation_unref(operation);
	else
		source_probe_result(app->pulse, NULL, -1, app);
	return G_SOURCE_REMOVE;
}

static void source_loaded(pa_context *context, uint32_t index, void *data)
{
	struct app *app = data;
	if (app->hardware_lost) {
		if (index != PA_INVALID_INDEX)
			pulse_operation(pa_context_unload_module(context, index, NULL, NULL));
		return;
	}
	if (index == PA_INVALID_INDEX) {
		g_warning("Cannot create PipeWire call microphone: %s", pa_strerror(pa_context_errno(context)));
		complete_request(app, app->result);
		return;
	}
	app->source_module = index;
	app->source_start_attempts = 0;
	pulse_operation(pa_context_set_default_source(context, VOICE_SOURCE, NULL, NULL));
	source_probe(app);
}

static void sink_initialized(pa_context *context, int success, void *data)
{
	struct app *app = data;
	pa_operation *operation;
	(void)success;
	if (app->hardware_lost)
		return;
	app->sink_ready = true;
	pulse_operation(pa_context_get_sink_info_by_name(context, VOICE_SINK, sink_volume, app));
	pulse_operation(pa_context_get_sink_input_info_list(context, move_input, app));
	operation = pa_context_load_module(context, "module-alsa-source",
		"device=hw:SM8550VERMEER,2 source_name=" VOICE_SOURCE
		" format=s16le rate=48000 channels=2 channel_map=front-left,front-right"
		" source_properties='device.description=Phone-call-microphone api.alsa.soft-mixer=true state.restore-props=false'",
		source_loaded, app);
	if (operation)
		pa_operation_unref(operation);
	else
		source_loaded(context, PA_INVALID_INDEX, app);
}

static gboolean sink_probe(gpointer data);

static void sink_probe_result(pa_context *context, const pa_sink_info *info, int end, void *data)
{
	struct app *app = data;
	pa_cvolume volume;
	pa_operation *operation;
	if (app->hardware_lost)
		return;
	if (end) {
		if (app->sink_index != PA_INVALID_INDEX)
			return;
		if (++app->sink_start_attempts < 50)
			g_timeout_add(100, sink_probe, app);
		else {
			g_warning("PipeWire call output did not become ready");
			complete_request(app, false);
		}
		return;
	}
	if (!info || strcmp(info->name, VOICE_SINK))
		return;
	app->sink_index = info->index;
	pulse_operation(pa_context_set_sink_volume_by_name(context, VOICE_SINK,
		pa_cvolume_set(&volume, 2, app->volume), NULL, NULL));
	operation = pa_context_set_sink_mute_by_name(context, VOICE_SINK,
						    app->output_muted, sink_initialized, app);
	if (operation)
		pa_operation_unref(operation);
	else
		sink_initialized(context, false, app);
}

static gboolean sink_probe(gpointer data)
{
	struct app *app = data;
	pa_operation *operation = pa_context_get_sink_info_by_name(app->pulse,
		VOICE_SINK, sink_probe_result, app);
	if (operation)
		pa_operation_unref(operation);
	else
		sink_probe_result(app->pulse, NULL, -1, app);
	return G_SOURCE_REMOVE;
}

static void sink_loaded(pa_context *context, uint32_t index, void *data)
{
	struct app *app = data;
	if (app->hardware_lost) {
		if (index != PA_INVALID_INDEX)
			pulse_operation(pa_context_unload_module(context, index, NULL, NULL));
		return;
	}
	if (index == PA_INVALID_INDEX) {
		g_warning("Cannot create PipeWire call output: %s", pa_strerror(pa_context_errno(context)));
		voice_session_media(&app->voice, false);
		complete_request(app, app->result);
		return;
	}
	app->sink_module = index;
	app->sink_start_attempts = 0;
	pulse_operation(pa_context_set_default_sink(context, VOICE_SINK, NULL, NULL));
	sink_probe(app);
}

static void media_start(struct app *app)
{
	pa_operation *operation;
	int ret;
	if (app->hardware_lost)
		return;
	ret = voice_session_media(&app->voice, true);

	/* Modem speech remains usable if desktop output creation fails. */
	if (ret < 0) {
		g_warning("Cannot enable call/media mixing: %s", snd_strerror(ret));
		complete_request(app, app->result);
		return;
	}
	voice_session_volume(&app->voice,
		((uint64_t)app->volume * 5 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM,
		app->output_muted || !app->volume);
	operation = pa_context_load_module(app->pulse, "module-alsa-sink",
		"device=hw:SM8550VERMEER,0 sink_name=" VOICE_SINK
		" format=s16le rate=48000 channels=2 channel_map=front-left,front-right"
		" sink_properties='device.description=Phone-call api.alsa.soft-mixer=true state.restore-props=false'",
		sink_loaded, app);
	if (operation)
		pa_operation_unref(operation);
	else
		sink_loaded(app->pulse, PA_INVALID_INDEX, app);
}

static gboolean media_stopped(gpointer data)
{
	struct app *app = data;
	bool previous = app->speaker;
	int ret;
	if (app->hardware_lost) {
		g_main_loop_quit(app->loop);
		return G_SOURCE_REMOVE;
	}
	ret = voice_session_media(&app->voice, false);

	/* Server module removal may precede the last ALSA close. Do not
	 * replace the RX graph while its multimedia frontend still uses it.
	 */
	if (ret == -EBUSY && ++app->media_stop_attempts < 100)
		return G_SOURCE_CONTINUE;
	if (ret < 0 && ret != -EACCES)
		g_warning("Cannot release call/media mixer: %s", snd_strerror(ret));
	if (ret < 0) {
		app->changing_route = false;
		complete_request(app, false);
		return G_SOURCE_REMOVE;
	}
	if (!app->changing_route) {
		restore_after_media(app);
		return G_SOURCE_REMOVE;
	}
	app->changing_route = false;
	voice_session_stop(&app->voice);
	ret = voice_session_start(&app->voice, app->current->value);
	app->result = !ret;
	if (!ret)
		app->speaker = app->current->value;
	else if (voice_session_start(&app->voice, previous) < 0) {
		restore_profiles(app, false);
		return G_SOURCE_REMOVE;
	}
	media_start(app);
	return G_SOURCE_REMOVE;
}

static void sink_unloaded(pa_context *context, int success, void *data)
{
	struct app *app = data;
	(void)context;
	if (!success)
		g_warning("Could not remove PipeWire call output");
	app->sink_module = PA_INVALID_INDEX;
	app->media_stop_attempts = 0;
	g_timeout_add(20, media_stopped, app);
}

static void find_old_sink(pa_context *context, const pa_sink_info *info, int end, void *data)
{
	struct app *app = data;
	pa_operation *operation;

	if (!end && info && !strcmp(info->name, VOICE_SINK))
		app->sink_module = info->owner_module;
	if (!end)
		return;
	if (app->sink_module == PA_INVALID_INDEX) {
		sink_unloaded(context, true, app);
		return;
	}
	operation = pa_context_unload_module(context, app->sink_module, sink_unloaded, app);
	if (operation)
		pa_operation_unref(operation);
	else
		sink_unloaded(context, false, app);
}

static void source_unloaded(pa_context *context, int success, void *data)
{
	struct app *app = data;
	pa_operation *operation;
	if (!success)
		g_warning("Could not remove PipeWire call microphone");
	app->source_module = PA_INVALID_INDEX;
	operation = pa_context_get_sink_info_by_name(context, VOICE_SINK, find_old_sink, app);
	if (operation)
		pa_operation_unref(operation);
	else
		find_old_sink(context, NULL, -1, app);
}

static void find_old_source(pa_context *context, const pa_source_info *info, int end, void *data)
{
	struct app *app = data;
	pa_operation *operation;
	if (!end && info && !strcmp(info->name, VOICE_SOURCE))
		app->source_module = info->owner_module;
	if (!end)
		return;
	if (app->source_module == PA_INVALID_INDEX) {
		source_unloaded(context, true, app);
		return;
	}
	operation = pa_context_unload_module(context, app->source_module, source_unloaded, app);
	if (operation)
		pa_operation_unref(operation);
	else
		source_unloaded(context, false, app);
}

static void media_stop(struct app *app)
{
	pa_operation *operation;
	app->sink_ready = false;
	app->source_ready = false;
	app->sink_index = PA_INVALID_INDEX;
	app->source_index = PA_INVALID_INDEX;
	operation = pa_context_get_source_info_by_name(app->pulse, VOICE_SOURCE, find_old_source, app);
	if (operation)
		pa_operation_unref(operation);
	else
		find_old_source(app->pulse, NULL, -1, app);
}

static void profile_result(pa_context *context, int success, void *data)
{
	struct app *app = data;
	(void)context;

	if (!success) {
		if (!app->restoring) {
			g_warning("Could not release the ALSA card from PipeWire");
			restore_profiles(app, false);
			return;
		}
		app->restore_ok = false;
	}
	app->card_index++;
	profile_next(app);
}

static gboolean start_voice(gpointer data)
{
	struct app *app = data;
	int ret;

	if (app->hardware_lost)
		return G_SOURCE_REMOVE;
	if (app->quitting) {
		restore_profiles(app, false);
		return G_SOURCE_REMOVE;
	}
	ret = voice_session_start(&app->voice, app->speaker);
	/* A PipeWire profile acknowledgement can precede closing its last
	 * ALSA handle. Retry only this resource conflict, never a DSP error.
	 */
	if (ret == -EBUSY && ++app->start_attempts < 10)
		return G_SOURCE_CONTINUE;
	if (ret < 0) {
		restore_profiles(app, false);
		return G_SOURCE_REMOVE;
	}
	app->active = true;
	g_message("AudioReach voice active (%s)", app->speaker ? "speaker" : "receiver");
	app->result = true;
	media_start(app);
	return G_SOURCE_REMOVE;
}

static gboolean release_probe(gpointer data);

static void release_info(pa_context *context, const pa_card_info *info, int end, void *data)
{
	struct app *app = data;
	const char *id;
	int ret;
	(void)context;
	if (!end && info) {
		id = pa_proplist_gets(info->proplist, "alsa.id");
		if (id && !strcmp(id, "SM8550VERMEER") && info->active_profile2 &&
		    strcmp(info->active_profile2->name, "off"))
			app->release_pending = true;
		return;
	}
	if (!end)
		return;
	ret = voice_session_idle(&app->voice);
	if (app->release_pending || ret == -EBUSY) {
		if (++app->release_attempts < 50)
			g_timeout_add(100, release_probe, app);
		else {
			g_warning("ALSA profile/PCM release did not complete");
			complete_request(app, false);
		}
		return;
	}
	if (ret < 0 || end < 0) {
		g_warning("Cannot verify ALSA profile/PCM release");
		complete_request(app, false);
		return;
	}
	if (app->restoring) {
		app->restoring_off = false;
		app->card_index = 0;
		profile_next(app);
	} else {
		app->start_attempts = 0;
		g_timeout_add(100, start_voice, app);
	}
}

static gboolean release_probe(gpointer data)
{
	struct app *app = data;
	pa_operation *operation;
	app->release_pending = false;
	operation = pa_context_get_card_info_list(app->pulse, release_info, app);
	if (operation)
		pa_operation_unref(operation);
	else
		release_info(app->pulse, NULL, -1, app);
	return G_SOURCE_REMOVE;
}

static void profile_next(struct app *app)
{
	pa_operation *operation;
	struct card *card;
	const char *profile;

	if (app->hardware_lost)
		return;
	if (app->card_index == app->cards->len) {
		if (app->restoring) {
			if (app->restoring_off) {
				app->release_attempts = 0;
				release_probe(app);
				return;
			}
			if (app->restore_ok) {
				app->restore_attempts = 0;
				restore_probe(app);
			} else {
				complete_request(app, false);
			}
			return;
		}
		app->release_attempts = 0;
		release_probe(app);
		return;
	}
	card = g_ptr_array_index(app->cards, app->card_index);
	profile = app->restoring && !app->restoring_off ? card->profile : "off";
	operation = pa_context_set_card_profile_by_name(app->pulse, card->name, profile,
						      profile_result, app);
	if (operation)
		pa_operation_unref(operation);
	else
		profile_result(app->pulse, 0, app);
}

static void card_info(pa_context *context, const pa_card_info *info, int end, void *data)
{
	struct app *app = data;
	struct card *card;
	const char *id;
	(void)context;

	if (end) {
		if (end < 0 || !app->cards->len || !save_cards(app)) {
			g_ptr_array_set_size(app->cards, 0);
			complete_request(app, false);
			return;
		}
		app->restoring = false;
		app->card_index = 0;
		profile_next(app);
		return;
	}
	id = pa_proplist_gets(info->proplist, "alsa.id");
	if (!id || strcmp(id, "SM8550VERMEER") || !info->active_profile2)
		return;
	card = g_new0(struct card, 1);
	card->name = g_strdup(info->name);
	card->profile = g_strdup(info->active_profile2->name);
	g_ptr_array_add(app->cards, card);
}

static void desktop_source(pa_context *context, const pa_source_info *info, int end, void *data)
{
	struct app *app = data;
	pa_operation *operation;
	if (!end && info) {
		g_free(app->default_source);
		app->default_source = g_strdup(info->name);
		return;
	}
	if (!end)
		return;
	operation = pa_context_get_card_info_list(context, card_info, app);
	if (operation)
		pa_operation_unref(operation);
	else
		complete_request(app, false);
}

static void desktop_sink(pa_context *context, const pa_sink_info *info, int end, void *data)
{
	struct app *app = data;
	pa_operation *operation;
	if (!end && info) {
		g_free(app->default_sink);
		app->default_sink = g_strdup(info->name);
		return;
	}
	if (!end)
		return;
	operation = pa_context_get_source_info_by_name(context, app->default_source, desktop_source, app);
	if (operation)
		pa_operation_unref(operation);
	else
		desktop_source(context, NULL, -1, app);
}

static void desktop_info(pa_context *context, const pa_server_info *info, void *data)
{
	struct app *app = data;
	pa_operation *operation;

	if (!info) {
		complete_request(app, false);
		return;
	}
	g_free(app->default_sink);
	app->default_sink = g_strdup(info->default_sink_name);
	g_free(app->default_source);
	app->default_source = g_strdup(info->default_source_name);
	/* PipeWire may return @DEFAULT_*@ aliases here. Resolve them before
	 * replacing defaults, otherwise restoration would select our own sink.
	 */
	operation = pa_context_get_sink_info_by_name(context, app->default_sink, desktop_sink, app);
	if (operation)
		pa_operation_unref(operation);
	else
		complete_request(app, false);
}

static void mute_result(pa_context *context, int success, void *data)
{
	struct app *app = data;
	(void)context;
	g_debug("Microphone mute acknowledgement: %d, state=%d", success, app->muted);
	complete_request(app, success);
}

static void process_requests(struct app *app)
{
	struct request *request;
	pa_operation *operation;
	int ret;

	if (app->hardware_lost || app->current || g_queue_is_empty(&app->requests) ||
	    pa_context_get_state(app->pulse) != PA_CONTEXT_READY)
		return;
	request = g_queue_pop_head(&app->requests);
	app->current = request;
	if (request->action == MODE) {
		if (!request->value) {
			if (!app->active && !app->cards->len)
				complete_request(app, true);
			else
				restore_profiles(app, true);
		} else if (app->active) {
			complete_request(app, true);
		} else if (app->cards->len) {
			/* A previous interrupted restoration must finish first. */
			restore_profiles(app, false);
		} else {
			operation = pa_context_get_server_info(app->pulse, desktop_info, app);
			if (operation)
				pa_operation_unref(operation);
			else
				complete_request(app, false);
		}
	} else if (request->action == MUTE) {
		g_debug("Microphone mute requested: %d", request->value);
		ret = voice_session_mute(&app->voice, request->value);
		if (ret >= 0)
			app->muted = request->value;
		if (ret >= 0 && app->source_ready) {
			operation = pa_context_set_source_mute_by_name(app->pulse, VOICE_SOURCE,
							      app->muted, mute_result, app);
			if (operation)
				pa_operation_unref(operation);
			else
				complete_request(app, false);
		} else {
			complete_request(app, ret >= 0);
		}
	} else {
		if (!app->active || app->speaker == request->value) {
			app->speaker = request->value;
			complete_request(app, true);
			return;
		}
		app->changing_route = true;
		media_stop(app);
	}
}

static void enqueue(struct app *app, enum action action, bool value,
		    GDBusMethodInvocation *invocation)
{
	struct request *request;

	if (g_queue_get_length(&app->requests) >= 16) {
		if (invocation)
			g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_LIMITS_EXCEEDED, "Audio operation queue is full");
		return;
	}
	request = g_new0(struct request, 1);
	request->action = action;
	request->value = value;
	request->invocation = invocation ? g_object_ref(invocation) : NULL;
	g_queue_push_tail(&app->requests, request);
	process_requests(app);
}

static void method_call(GDBusConnection *connection, const char *sender,
			const char *path, const char *interface, const char *method,
			GVariant *parameters, GDBusMethodInvocation *invocation, void *data)
{
	struct app *app = data;
	guint mode;
	gboolean enabled;
	(void)connection; (void)sender; (void)path; (void)interface;

	if (!strcmp(method, "SelectMode")) {
		g_variant_get(parameters, "(u)", &mode);
		if (mode > 1) {
			g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS, "Invalid audio mode");
			return;
		}
		enqueue(app, MODE, mode, invocation);
	} else {
		g_variant_get(parameters, "(b)", &enabled);
		enqueue(app, !strcmp(method, "MuteMic") ? MUTE : SPEAKER, enabled, invocation);
	}
}

static GVariant *get_property(GDBusConnection *connection, const char *sender,
			     const char *path, const char *interface, const char *property,
			     GError **error, void *data)
{
	struct app *app = data;
	(void)connection; (void)sender; (void)path; (void)interface; (void)error;

	if (!strcmp(property, "AudioMode"))
		return g_variant_new_uint32(app->active);
	if (!strcmp(property, "SpeakerState"))
		return g_variant_new_uint32(app->speaker);
	return g_variant_new_uint32(!app->muted);
}

static const GDBusInterfaceVTable vtable = {
	.method_call = method_call,
	.get_property = get_property,
};

static void call_states(struct app *app)
{
	GHashTableIter iterator;
	gpointer proxy;
	bool active = false, connected = false;

	g_hash_table_iter_init(&iterator, app->calls);
	while (g_hash_table_iter_next(&iterator, NULL, &proxy)) {
		g_autoptr(GVariant) state = g_dbus_proxy_get_cached_property(proxy, "State");
		int value;

		if (!state || !g_variant_is_of_type(state, G_VARIANT_TYPE_INT32))
			continue;
		value = g_variant_get_int32(state);
		if (value == 1 || value == 2 || value == 4 || value == 5)
			active = true;
		if (value == 4 || value == 5)
			connected = true;
	}
	if (app->automatic_active != active ||
	    (active && connected && !app->automatic_connected)) {
		app->automatic_active = active;
		g_message("Modem voice route %s (%s)", active ? "requested" : "released",
			  connected ? "connected" : "dialing or idle");
		enqueue(app, MODE, active, NULL);
	}
	app->automatic_connected = connected;
}

static void call_properties(GDBusProxy *proxy, GVariant *changed,
			    const char * const *invalidated, void *data)
{
	(void)proxy; (void)changed; (void)invalidated;
	call_states(data);
}

static void call_signal(GDBusProxy *proxy, const char *sender, const char *signal_name,
			GVariant *parameters, void *data)
{
	(void)sender;
	if (!strcmp(signal_name, "StateChanged") &&
	    g_variant_is_of_type(parameters, G_VARIANT_TYPE("(iiu)"))) {
		int old_state, new_state;
		guint reason;
		g_autoptr(GVariant) state = NULL;
		g_variant_get(parameters, "(iiu)", &old_state, &new_state, &reason);
		state = g_variant_ref_sink(g_variant_new_int32(new_state));
		g_dbus_proxy_set_cached_property(proxy, "State", state);
		call_states(data);
	}
}

static void update_calls(struct app *app)
{
	GList *objects = g_dbus_object_manager_get_objects(app->modems);
	g_autoptr(GHashTable) present = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GHashTableIter iterator;
	gpointer path;

	/* MM exports Call objects separately from its ObjectManager. Discover
	 * them through each modem's Voice.Calls property, then track their own
	 * StateChanged and PropertiesChanged signals.
	 */
	for (GList *entry = objects; entry; entry = entry->next) {
		g_autoptr(GDBusInterface) interface =
			g_dbus_object_get_interface(entry->data, MM_NAME ".Modem.Voice");
		g_autoptr(GVariant) calls = NULL;
		GVariantIter paths;
		const char *name;

		if (!interface)
			continue;
		calls = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(interface), "Calls");
		if (!calls || !g_variant_is_of_type(calls, G_VARIANT_TYPE("ao")))
			continue;
		g_variant_iter_init(&paths, calls);
		while (g_variant_iter_next(&paths, "&o", &name)) {
			GDBusProxy *proxy;
			g_autoptr(GError) error = NULL;

			g_hash_table_add(present, g_strdup(name));
			if (g_hash_table_contains(app->calls, name))
				continue;
			proxy = g_dbus_proxy_new_sync(
				g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(app->modems)),
				G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, MM_NAME, name, CALL_IFACE, NULL, &error);
			if (!proxy) {
				g_warning("Cannot observe call state: %s", error->message);
				continue;
			}
			g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(call_properties), app);
			g_signal_connect(proxy, "g-signal", G_CALLBACK(call_signal), app);
			g_hash_table_insert(app->calls, g_strdup(name), proxy);
		}
	}
	g_list_free_full(objects, g_object_unref);
	g_hash_table_iter_init(&iterator, app->calls);
	while (g_hash_table_iter_next(&iterator, &path, NULL))
		if (!g_hash_table_contains(present, path))
			g_hash_table_iter_remove(&iterator);
	call_states(app);
}

static void object_changed(GDBusObjectManager *manager, GDBusObject *object, void *data)
{
	(void)manager; (void)object;
	update_calls(data);
}

static void interface_changed(GDBusObjectManager *manager, GDBusObject *object,
			      GDBusInterface *interface, void *data)
{
	(void)manager; (void)object; (void)interface;
	update_calls(data);
}

static void properties_changed(GDBusObjectManagerClient *manager, GDBusObjectProxy *object,
			       GDBusProxy *interface, GVariant *changed,
			       const char * const *invalidated, void *data)
{
	(void)manager; (void)object; (void)interface; (void)changed; (void)invalidated;
	update_calls(data);
}

static void modem_owner_changed(GObject *object, GParamSpec *property, void *data)
{
	struct app *app = data;
	g_autofree char *owner =
		g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(object));
	(void)property;

	if (!owner) {
		g_hash_table_remove_all(app->calls);
		app->automatic_active = false;
		app->automatic_connected = false;
		enqueue(app, MODE, false, NULL);
	} else {
		update_calls(app);
	}
}

static void pulse_state(pa_context *context, void *data)
{
	struct app *app = data;
	pa_context_state_t state = pa_context_get_state(context);

	if (state == PA_CONTEXT_READY) {
		pa_context_set_subscribe_callback(context, pulse_event, app);
		pulse_operation(pa_context_subscribe(context,
			PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
			PA_SUBSCRIPTION_MASK_CARD | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL));
		process_requests(app);
		remember_desktop(app);
	} else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
		g_warning("Desktop audio connection lost; stopping voice, retaining restoration state");
		voice_session_stop(&app->voice);
		g_main_loop_quit(app->loop);
	}
}

static gboolean hardware_event(gint fd, GIOCondition condition, gpointer data)
{
	struct app *app = data;
	int ret = 0;
	(void)fd;

	if (condition & G_IO_IN) {
		do {
			ret = voice_session_read_event(&app->voice);
		} while (ret > 0);
	}
	if (!(condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) &&
	    (ret >= 0 || ret == -EAGAIN))
		return G_SOURCE_CONTINUE;

	/* ASoC waits for every open control/PCM file before ADSP teardown.
	 * Release our control and UCM handles even while idle, otherwise this
	 * pins the entire DSP recovery and its unrelated charger service.
	 * Keep the restoration journal for the new card after service restart.
	 */
	g_warning("ALSA card disconnected; releasing handles for ADSP recovery");
	app->hardware_watch = 0;
	app->hardware_lost = true;
	if (!app->cards->len && app->desktop_state) {
		g_autoptr(GError) error = NULL;
		if (!g_file_set_contents_full(app->state_file, app->desktop_state, -1,
				G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
				0600, &error))
			g_warning("Cannot save disconnected desktop audio state: %s", error->message);
	}
	/* Active module-alsa adapters can keep disconnected PCM files open
	 * indefinitely while application streams are still linked to them.
	 * Unload both adapters before exiting, even though the card is gone.
	 */
	media_stop(app);
	return G_SOURCE_REMOVE;
}

static gboolean terminate(gpointer data)
{
	struct app *app = data;
	app->quitting = true;
	/* A queued start must not run after shutdown restoration. */
	while (!g_queue_is_empty(&app->requests)) {
		struct request *request = g_queue_pop_head(&app->requests);
		if (request->invocation) {
			g_dbus_method_invocation_return_value(request->invocation, g_variant_new("(b)", false));
			g_object_unref(request->invocation);
		}
		g_free(request);
	}
	if (pa_context_get_state(app->pulse) != PA_CONTEXT_READY) {
		voice_session_stop(&app->voice);
		g_main_loop_quit(app->loop);
	} else {
		enqueue(app, MODE, false, NULL);
	}
	return G_SOURCE_REMOVE;
}

static void name_lost(GDBusConnection *connection, const char *name, void *data)
{
	(void)connection;
	g_warning("Cannot own %s; stopping to avoid competing call audio controllers", name);
	terminate(data);
}

int main(int argc, char **argv)
{
	struct app app = { 0 };
	g_autoptr(GError) error = NULL;
	g_autoptr(GDBusNodeInfo) node = NULL;
	guint registration, owner;
	int ret, exit_status;

	if (argc != 2) {
		g_printerr("Usage: %s PRIVATE_UCM_DIRECTORY\n", argv[0]);
		return 2;
	}
	app.sink_module = PA_INVALID_INDEX;
	app.sink_index = PA_INVALID_INDEX;
	app.source_module = PA_INVALID_INDEX;
	app.source_index = PA_INVALID_INDEX;
	app.volume = PA_VOLUME_NORM * 3 / 5;
	app.mic_volume = PA_VOLUME_NORM;
	ret = voice_session_init(&app.voice, argv[1]);
	if (ret < 0) {
		g_printerr("AudioReach voice initialization: %s\n", snd_strerror(ret));
		return 1;
	}
	app.loop = g_main_loop_new(NULL, false);
	ret = voice_session_event_fd(&app.voice);
	if (ret < 0) {
		g_printerr("Cannot monitor ALSA card removal: %s\n", snd_strerror(ret));
		voice_session_destroy(&app.voice);
		g_main_loop_unref(app.loop);
		return 1;
	}
	app.hardware_watch = g_unix_fd_add(ret, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
					 hardware_event, &app);
	app.cards = g_ptr_array_new_with_free_func(card_free);
	app.calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	app.state_file = g_build_filename(g_get_user_runtime_dir(), "vermeer-voice-restore.ini", NULL);
	app.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (!app.bus)
		g_error("Cannot connect to session bus: %s", error->message);
	node = g_dbus_node_info_new_for_xml(introspection, &error);
	registration = g_dbus_connection_register_object(app.bus, OBJECT_PATH,
							 node->interfaces[0], &vtable, &app, NULL, &error);
	if (!registration)
		g_error("Cannot register CallAudio interface: %s", error->message);
	owner = g_bus_own_name_on_connection(app.bus, BUS_NAME,
		G_BUS_NAME_OWNER_FLAGS_NONE, NULL, name_lost, &app, NULL);
	app.modems = g_dbus_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
		G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, MM_NAME,
		"/org/freedesktop/ModemManager1", NULL, NULL, NULL, NULL, &error);
	if (!app.modems)
		g_error("Cannot observe ModemManager: %s", error->message);
	g_signal_connect(app.modems, "object-added", G_CALLBACK(object_changed), &app);
	g_signal_connect(app.modems, "object-removed", G_CALLBACK(object_changed), &app);
	g_signal_connect(app.modems, "interface-added", G_CALLBACK(interface_changed), &app);
	g_signal_connect(app.modems, "interface-removed", G_CALLBACK(interface_changed), &app);
	g_signal_connect(app.modems, "interface-proxy-properties-changed",
			 G_CALLBACK(properties_changed), &app);
	g_signal_connect(app.modems, "notify::name-owner", G_CALLBACK(modem_owner_changed), &app);
	app.pulse_loop = pa_glib_mainloop_new(NULL);
	app.pulse = pa_context_new(pa_glib_mainloop_get_api(app.pulse_loop), "Vermeer call audio");
	pa_context_set_state_callback(app.pulse, pulse_state, &app);
	load_cards(&app);
	if (app.cards->len)
		enqueue(&app, MODE, false, NULL);
	update_calls(&app);
	if (pa_context_connect(app.pulse, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
		g_error("Cannot connect to desktop audio");
	g_unix_signal_add(SIGINT, terminate, &app);
	g_unix_signal_add(SIGTERM, terminate, &app);
	g_main_loop_run(app.loop);
	exit_status = app.quitting && !app.cards->len ? 0 : 1;
	if (app.hardware_watch)
		g_source_remove(app.hardware_watch);
	voice_session_destroy(&app.voice);
	g_bus_unown_name(owner);
	g_dbus_connection_unregister_object(app.bus, registration);
	pa_context_disconnect(app.pulse);
	pa_context_unref(app.pulse);
	pa_glib_mainloop_free(app.pulse_loop);
	g_clear_object(&app.modems);
	g_hash_table_unref(app.calls);
	g_clear_object(&app.bus);
	g_ptr_array_unref(app.cards);
	g_main_loop_unref(app.loop);
	g_free(app.state_file);
	g_free(app.default_sink);
	g_free(app.default_source);
	g_free(app.restored_sink);
	g_free(app.restored_source);
	g_clear_pointer(&app.remembering, g_key_file_unref);
	g_free(app.desktop_state);
	return exit_status;
}
