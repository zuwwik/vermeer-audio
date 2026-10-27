/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VERMEER_VOICE_SUBSCRIPTIONS_H
#define VERMEER_VOICE_SUBSCRIPTIONS_H

#include <stdbool.h>

struct voice_subscriptions;
typedef void (*voice_subscription_changed)(unsigned int subscription, bool ready, void *data);

struct voice_subscriptions *voice_subscriptions_new(voice_subscription_changed changed, void *data);
void voice_subscriptions_free(struct voice_subscriptions *subscriptions);

/* QMI VOICE states, independent of physical SIM-slot mapping. */
static inline unsigned int voice_call_priority(unsigned int state)
{
	switch (state) {
	case 3: return 4; /* conversation */
	case 1: case 4: case 5: return 3; /* outgoing setup / alerting */
	case 2: case 7: case 10: return 2; /* incoming / waiting / setup */
	case 6: return 1; /* held */
	default: return 0;
	}
}

static inline unsigned int voice_subscription_select(const unsigned int priorities[2],
						     unsigned int previous)
{
	if (!priorities[0] && !priorities[1])
		return 0;
	if (priorities[0] == priorities[1] && previous >= 1 && previous <= 2)
		return previous;
	return priorities[1] > priorities[0] ? 2 : 1;
}

#endif
