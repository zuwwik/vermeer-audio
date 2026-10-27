// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "voice-session.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int voice_control(struct voice_session *session, const char *name, int value, bool write)
{
	snd_ctl_elem_value_t *control;
	int ret;

	snd_ctl_elem_value_alloca(&control);
	snd_ctl_elem_value_set_interface(control, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(control, name);
	ret = snd_ctl_elem_read(session->ctl, control);
	if (ret < 0)
		return ret;
	if (!write)
		return 0;
	snd_ctl_elem_value_set_integer(control, 0, value);
	return snd_ctl_elem_write(session->ctl, control);
}

int voice_session_init(struct voice_session *session, const char *ucm_directory)
{
	snd_ctl_card_info_t *card;
	snd_pcm_info_t *info;
	int ret, device = -1, voice_device = -1;

	memset(session, 0, sizeof(*session));
	ret = snd_ctl_open(&session->ctl, "hw:SM8550VERMEER", 0);
	if (ret < 0)
		return ret;
	snd_ctl_card_info_alloca(&card);
	ret = snd_ctl_card_info(session->ctl, card);
	if (ret < 0 || strcmp(snd_ctl_card_info_get_name(card), "SM8550-VERMEER")) {
		ret = ret < 0 ? ret : -ENODEV;
		goto error;
	}
	ret = voice_control(session, "Voice Call Switch", 0, false);
	if (ret < 0)
		goto error;
	ret = voice_control(session, "Voice Call Mic Switch", 0, false);
	if (ret < 0)
		goto error;
	ret = voice_control(session, "Voice Call Media Switch", 0, false);
	if (ret < 0)
		goto error;
	ret = voice_control(session, "Voice Call Capture Volume", 0, false);
	if (ret < 0)
		goto error;
	snd_pcm_info_alloca(&info);
	while (snd_ctl_pcm_next_device(session->ctl, &device) >= 0 && device >= 0) {
		snd_pcm_info_set_device(info, device);
		snd_pcm_info_set_subdevice(info, 0);
		snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);
		if (snd_ctl_pcm_info(session->ctl, info) >= 0 &&
		    !strncmp(snd_pcm_info_get_id(info), "Voice Call", 10)) {
			voice_device = device;
			break;
		}
	}
	if (voice_device < 0) {
		ret = -ENODEV;
		goto error;
	}
	if (asprintf(&session->pcm_name, "hw:SM8550VERMEER,%d", voice_device) < 0) {
		ret = -ENOMEM;
		goto error;
	}
	/* This process has a private UCM search root. The desktop sound server
	 * continues using the stock HiFi configuration.
	 */
	if (setenv("ALSA_CONFIG_UCM2", ucm_directory, 1)) {
		ret = -errno;
		goto error;
	}
	ret = snd_use_case_mgr_open(&session->ucm, "hw:SM8550VERMEER");
	if (ret < 0)
		goto error;
	return 0;
error:
	voice_session_destroy(session);
	return ret;
}

static int voice_pcm_parameters(snd_pcm_t *pcm)
{
	snd_pcm_hw_params_t *params;
	snd_pcm_sw_params_t *software;
	snd_pcm_uframes_t buffer = 1024, period = 512;
	snd_pcm_uframes_t boundary;
	int ret;

	snd_pcm_hw_params_alloca(&params);
#define PARAM(call) do { ret = (call); if (ret < 0) return ret; } while (0)
	PARAM(snd_pcm_hw_params_any(pcm, params));
	PARAM(snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED));
	PARAM(snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE));
	PARAM(snd_pcm_hw_params_set_channels(pcm, params, 2));
	PARAM(snd_pcm_hw_params_set_rate(pcm, params, 48000, 0));
	PARAM(snd_pcm_hw_params_set_period_size(pcm, params, period, 0));
	PARAM(snd_pcm_hw_params_set_buffer_size(pcm, params, buffer));
	PARAM(snd_pcm_hw_params(pcm, params));
	snd_pcm_sw_params_alloca(&software);
	PARAM(snd_pcm_sw_params_current(pcm, software));
	PARAM(snd_pcm_sw_params_get_boundary(software, &boundary));
	/* No AP samples are transferred. ALSA's documented free-running
	 * threshold permits START with an empty playback buffer and disables
	 * sample-based XRUN detection for this hostless control stream.
	 */
	PARAM(snd_pcm_sw_params_set_start_threshold(pcm, software, boundary));
	PARAM(snd_pcm_sw_params_set_stop_threshold(pcm, software, boundary));
	PARAM(snd_pcm_sw_params(pcm, software));
#undef PARAM
	return 0;
}

void voice_session_stop(struct voice_session *session)
{
	if (session->tx) {
		snd_pcm_close(session->tx);
		session->tx = NULL;
	}
	if (session->rx) {
		snd_pcm_close(session->rx);
		session->rx = NULL;
	}
	if (session->output) {
		snd_use_case_set(session->ucm, "_disdev", session->speaker ? "Speaker" : "Earpiece");
		session->output = false;
	}
	if (session->mic) {
		snd_use_case_set(session->ucm, "_disdev", "Mic");
		session->mic = false;
	}
	if (session->verb) {
		snd_use_case_set(session->ucm, "_verb", SND_USE_CASE_VERB_INACTIVE);
		session->verb = false;
	}
}

int voice_session_start(struct voice_session *session, bool speaker)
{
	int ret;

	if (session->rx || session->tx)
		return -EBUSY;
	session->speaker = speaker;
	/* Mark sequences before running them so partially applied controls are
	 * disabled as well when an individual ALSA control write fails.
	 */
	session->verb = true;
	ret = snd_use_case_set(session->ucm, "_verb", "Voice Call");
	if (ret < 0)
		goto error;
	session->mic = true;
	ret = snd_use_case_set(session->ucm, "_enadev", "Mic");
	if (ret < 0)
		goto error;
	session->output = true;
	ret = snd_use_case_set(session->ucm, "_enadev", speaker ? "Speaker" : "Earpiece");
	if (ret < 0)
		goto error;
	ret = voice_session_mute(session, session->muted);
	if (ret < 0)
		goto error;
	ret = snd_pcm_open(&session->rx, session->pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0)
		goto error;
	ret = snd_pcm_open(&session->tx, session->pcm_name, SND_PCM_STREAM_CAPTURE, 0);
	if (ret < 0)
		goto error;
	ret = voice_pcm_parameters(session->rx);
	if (ret < 0)
		goto error;
	ret = voice_pcm_parameters(session->tx);
	if (ret < 0)
		goto error;
	ret = snd_pcm_prepare(session->rx);
	if (ret < 0)
		goto error;
	ret = snd_pcm_prepare(session->tx);
	if (ret < 0)
		goto error;
	/* PREPARE starts the DSP graphs; START also invokes backend codec
	 * unmute. AW88261 deliberately defers unmute until this trigger.
	 * No samples, synthetic timers or silent playback are needed.
	 */
	ret = snd_pcm_start(session->rx);
	if (ret < 0)
		goto error;
	ret = snd_pcm_start(session->tx);
	if (ret < 0)
		goto error;
	return 0;
error:
	fprintf(stderr, "AudioReach voice start: %s\n", snd_strerror(ret));
	voice_session_stop(session);
	return ret;
}

int voice_session_mute(struct voice_session *session, bool mute)
{
	int ret = voice_control(session, "Voice Call Mic Switch", !mute, true);

	if (ret >= 0)
		session->muted = mute;
	return ret;
}

int voice_session_media(struct voice_session *session, bool enable)
{
	int ret;

	if (enable && (!session->rx || !session->tx))
		return -EACCES;
	ret = voice_control(session, "Voice Call Media Switch", enable, true);
	if (ret < 0)
		return ret;
	ret = voice_control(session, "Secondary TDM0 Audio Mixer MultiMedia1", enable, true);
	if (ret >= 0)
		ret = voice_control(session, "MultiMedia3 Mixer TX_CODEC_DMA_TX_3", enable, true);
	if (ret < 0 && enable) {
		voice_control(session, "Secondary TDM0 Audio Mixer MultiMedia1", 0, true);
		voice_control(session, "Voice Call Media Switch", 0, true);
	}
	return ret < 0 ? ret : 0;
}

int voice_session_capture(struct voice_session *session, unsigned int gain, bool mute)
{
	int ret;
	if (gain > 8192)
		return -EINVAL;
	ret = voice_control(session, "Voice Call Capture Volume", gain, true);
	if (ret < 0)
		return ret;
	return voice_session_mute(session, mute);
}

int voice_session_idle(struct voice_session *session)
{
	snd_pcm_info_t *info;
	int ret;
	snd_pcm_info_alloca(&info);
	for (unsigned int i = 0; i < 2; i++) {
		snd_pcm_info_set_device(info, i ? 2 : 0);
		snd_pcm_info_set_subdevice(info, 0);
		snd_pcm_info_set_stream(info, i ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK);
		ret = snd_ctl_pcm_info(session->ctl, info);
		if (ret < 0)
			return ret;
		if (!snd_pcm_info_get_subdevices_avail(info))
			return -EBUSY;
	}
	return 0;
}

int voice_session_event_fd(struct voice_session *session)
{
	struct pollfd descriptor;
	int ret;

	ret = snd_ctl_nonblock(session->ctl, 1);
	if (ret < 0)
		return ret;
	ret = snd_ctl_subscribe_events(session->ctl, 1);
	if (ret < 0)
		return ret;
	ret = snd_ctl_poll_descriptors(session->ctl, &descriptor, 1);
	if (ret != 1)
		return ret < 0 ? ret : -ENOTSUP;
	return descriptor.fd;
}

int voice_session_read_event(struct voice_session *session)
{
	snd_ctl_event_t *event;

	snd_ctl_event_alloca(&event);
	return snd_ctl_read(session->ctl, event);
}

int voice_session_volume(struct voice_session *session, unsigned int level, bool mute)
{
	int ret;

	if (level > 5)
		return -EINVAL;
	ret = voice_control(session, "Voice Call Volume", level, true);
	if (ret < 0)
		return ret;
	ret = voice_control(session, "Amplifier L PCM Playback Volume", mute ? 0 : 360, true);
	if (ret < 0)
		return ret;
	return voice_control(session, "Amplifier R PCM Playback Volume",
			     mute || !session->speaker ? 0 : 360, true);
}

void voice_session_destroy(struct voice_session *session)
{
	voice_session_stop(session);
	if (session->ucm)
		snd_use_case_mgr_close(session->ucm);
	if (session->ctl)
		snd_ctl_close(session->ctl);
	free(session->pcm_name);
	memset(session, 0, sizeof(*session));
}
