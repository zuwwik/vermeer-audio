// SPDX-License-Identifier: GPL-2.0-only
/* Controller test doubles. No ALSA PCM or real modem is opened. */
#define _GNU_SOURCE
#include "voice-session.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <glib.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int card_events[2];

static void disconnect_card(int signal_number)
{
	(void)signal_number;
	close(card_events[1]);
}

int voice_session_init(struct voice_session *s, const char *d)
{
	(void)d;
	memset(s, 0, sizeof(*s));
	if (pipe2(card_events, O_NONBLOCK | O_CLOEXEC))
		return -errno;
	signal(SIGUSR1, disconnect_card);
	return 0;
}
int voice_session_event_fd(struct voice_session *s)
{ (void)s; return card_events[0]; }
int voice_session_read_event(struct voice_session *s)
{ (void)s; return -EAGAIN; }
int voice_session_start(struct voice_session *s, bool speaker)
{ s->speaker = speaker; return 0; }
void voice_session_stop(struct voice_session *s) { (void)s; }
void voice_session_destroy(struct voice_session *s)
{ (void)s; close(card_events[0]); close(card_events[1]); }
int voice_session_mute(struct voice_session *s, bool mute)
{ s->muted = mute; return 0; }
int voice_session_media(struct voice_session *s, bool enable)
{ (void)s; (void)enable; return 0; }
int voice_session_idle(struct voice_session *s)
{
	static unsigned int attempts;
	(void)s;
	return ++attempts % 3 ? -EBUSY : 0;
}
int voice_session_volume(struct voice_session *s, unsigned int level, bool mute)
{
	const char *path = g_getenv("VERMEER_TEST_VOLUME");
	(void)s;
	if (path) {
		g_autofree char *text = g_strdup_printf("%u %u\n", level, mute);
		g_file_set_contents(path, text, -1, NULL);
	}
	return 0;
}
int voice_session_capture(struct voice_session *s, unsigned int gain, bool mute)
{
	const char *path = g_getenv("VERMEER_TEST_CAPTURE");
	s->muted = mute;
	if (path) {
		g_autofree char *text = g_strdup_printf("%u %u\n", gain, mute);
		g_file_set_contents(path, text, -1, NULL);
	}
	return 0;
}

pa_operation *pa_context_load_module(pa_context *context, const char *name,
				   const char *args, pa_context_index_cb_t callback, void *data)
{
	pa_operation *(*real)(pa_context *, const char *, const char *, pa_context_index_cb_t, void *) =
		dlsym(RTLD_NEXT, "pa_context_load_module");
	if (!strcmp(name, "module-alsa-sink")) {
		g_assert_nonnull(strstr(args, "device=hw:SM8550VERMEER,0"));
		return real(context, "module-null-sink",
			"sink_name=vermeer_voice channels=2 sink_properties=state.restore-props=false", callback, data);
	}
	g_assert_cmpstr(name, ==, "module-alsa-source");
	g_assert_nonnull(strstr(args, "device=hw:SM8550VERMEER,2"));
	return real(context, "module-remap-source",
		"master=vermeer_voice.monitor source_name=vermeer_voice_mic channels=2"
		" source_properties=state.restore-props=false", callback, data);
}

/* The isolated server has no physical ALSA card. Emulate only card profile
 * discovery/switching; sink/default/volume/module operations use real libpulse
 * and a private PipeWire server. Persist profiles across the crash test.
 */
struct profile_callback {
	pa_context *context;
	pa_card_info_cb_t info;
	pa_context_success_cb_t success;
	void *data;
};

static void noop(pa_context *context, const pa_server_info *info, void *data)
{ (void)context; (void)info; (void)data; }

static gboolean profile_idle(gpointer data)
{
	struct profile_callback *cb = data;
	g_autoptr(GKeyFile) state = g_key_file_new();
	const char *names[] = { "alsa_card.platform-sound", "alsa_card.comprC0" };
	g_key_file_load_from_file(state, g_getenv("VERMEER_TEST_PROFILES"), 0, NULL);
	if (cb->info) {
		for (unsigned int i = 0; i < G_N_ELEMENTS(names); i++) {
			g_autofree char *name = g_key_file_get_string(state, "cards", names[i], NULL);
			pa_card_profile_info2 profile = { .name = name };
			pa_card_info info = { .name = names[i], .active_profile2 = &profile,
				.proplist = pa_proplist_new() };
			pa_proplist_sets(info.proplist, "alsa.id", "SM8550VERMEER");
			cb->info(cb->context, &info, 0, cb->data);
			pa_proplist_free(info.proplist);
		}
		cb->info(cb->context, NULL, 1, cb->data);
	} else {
		cb->success(cb->context, true, cb->data);
	}
	g_free(cb);
	return G_SOURCE_REMOVE;
}

pa_operation *pa_context_get_card_info_list(pa_context *context, pa_card_info_cb_t callback, void *data)
{
	if (!g_getenv("VERMEER_TEST_PROFILES")) {
		pa_operation *(*real)(pa_context *, pa_card_info_cb_t, void *) =
			dlsym(RTLD_NEXT, "pa_context_get_card_info_list");
		return real(context, callback, data);
	}
	struct profile_callback *cb = g_new0(struct profile_callback, 1);
	*cb = (struct profile_callback){ .context = context, .info = callback, .data = data };
	g_idle_add(profile_idle, cb);
	return pa_context_get_server_info(context, noop, NULL);
}

pa_operation *pa_context_set_card_profile_by_name(pa_context *context, const char *name,
					       const char *profile,
					       pa_context_success_cb_t callback, void *data)
{
	const char *path = g_getenv("VERMEER_TEST_PROFILES");
	if (!path) {
		pa_operation *(*real)(pa_context *, const char *, const char *,
			pa_context_success_cb_t, void *) = dlsym(RTLD_NEXT, "pa_context_set_card_profile_by_name");
		return real(context, name, profile, callback, data);
	}
	g_autoptr(GKeyFile) state = g_key_file_new();
	g_key_file_load_from_file(state, path, 0, NULL);
	g_key_file_set_string(state, "cards", name, profile);
	g_autofree char *text = g_key_file_to_data(state, NULL, NULL);
	g_file_set_contents(path, text, -1, NULL);
	struct profile_callback *cb = g_new0(struct profile_callback, 1);
	*cb = (struct profile_callback){ .context = context, .success = callback, .data = data };
	g_idle_add(profile_idle, cb);
	return pa_context_get_server_info(context, noop, NULL);
}
