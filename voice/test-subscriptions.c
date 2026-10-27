// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>

#include "voice-subscriptions.h"

int main(void)
{
	unsigned int priorities[2] = { 0, 0 };

	assert(voice_subscription_select(priorities, 1) == 0);
	/* An active first call takes priority over an incoming second call. */
	priorities[0] = voice_call_priority(3);
	priorities[1] = voice_call_priority(2);
	assert(voice_subscription_select(priorities, 1) == 1);
	/* A secondary call selects SIM2 independently of the primary slot. */
	priorities[0] = 0;
	priorities[1] = voice_call_priority(3);
	assert(voice_subscription_select(priorities, 1) == 2);
	priorities[0] = voice_call_priority(6);
	assert(voice_subscription_select(priorities, 1) == 2);
	/* Retain the previous subscription when both calls have equal priority. */
	priorities[0] = voice_call_priority(3);
	assert(voice_subscription_select(priorities, 2) == 2);
	priorities[1] = voice_call_priority(9);
	assert(voice_subscription_select(priorities, 2) == 1);
	priorities[0] = voice_call_priority(8);
	assert(voice_subscription_select(priorities, 2) == 0);

	puts("PASS: logical voice subscription priority, ties, hold and call end");
	return 0;
}
