/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VERMEER_VOICE_SESSION_H
#define VERMEER_VOICE_SESSION_H

#include <alsa/asoundlib.h>
#include <alsa/use-case.h>
#include <stdbool.h>

struct voice_session {
	snd_use_case_mgr_t *ucm;
	snd_ctl_t *ctl;
	snd_pcm_t *rx;
	snd_pcm_t *tx;
	char *pcm_name;
	bool verb;
	bool mic;
	bool output;
	bool speaker;
	bool muted;
	unsigned int subscription;
};

int voice_session_init(struct voice_session *session, const char *ucm_directory);
int voice_session_start(struct voice_session *session, bool speaker, unsigned int subscription);
void voice_session_stop(struct voice_session *session);
void voice_session_destroy(struct voice_session *session);
int voice_session_mute(struct voice_session *session, bool mute);
int voice_session_media(struct voice_session *session, bool enable);
int voice_session_volume(struct voice_session *session, unsigned int level, bool mute);
int voice_session_capture(struct voice_session *session, unsigned int gain, bool mute);
int voice_session_idle(struct voice_session *session);
int voice_session_event_fd(struct voice_session *session);
int voice_session_read_event(struct voice_session *session);

#endif
