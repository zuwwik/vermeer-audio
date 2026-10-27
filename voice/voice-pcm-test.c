// SPDX-License-Identifier: GPL-2.0-only
/* Starts only the local audio route. Never originates a call. */
#define _POSIX_C_SOURCE 200809L
#include "voice-session.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;

static void stop(int signal)
{
	(void)signal;
	stopped = 1;
}

int main(int argc, char **argv)
{
	struct voice_session session;
	int ret, seconds;

	if (argc != 4 || (strcmp(argv[2], "handset") && strcmp(argv[2], "speaker"))) {
		fprintf(stderr, "Usage: %s UCM_DIRECTORY handset|speaker SECONDS\n", argv[0]);
		return 2;
	}
	seconds = atoi(argv[3]);
	if (seconds < 0 || seconds > 3600)
		return 2;
	ret = voice_session_init(&session, argv[1]);
	if (ret < 0) {
		fprintf(stderr, "Voice PCM initialization: %s\n", snd_strerror(ret));
		return 1;
	}
	if (!seconds) {
		printf("Voice PCM and private UCM found: %s\n", session.pcm_name);
		voice_session_destroy(&session);
		return 0;
	}
	signal(SIGINT, stop);
	signal(SIGTERM, stop);
	ret = voice_session_start(&session, !strcmp(argv[2], "speaker"), 1);
	if (!ret) {
		puts("Local voice route prepared; no call was originated.");
		fflush(stdout);
		while (!stopped && seconds--)
			sleep(1);
	}
	voice_session_destroy(&session);
	return ret < 0;
}
