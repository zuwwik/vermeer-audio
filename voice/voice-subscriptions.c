// SPDX-License-Identifier: GPL-2.0-only
/* Observe each modem voice subscription using private, explicitly bound clients.
 * These clients never dial, answer, hang up or change the modem's primary SIM.
 */
#define QMI_DISABLE_DEPRECATED
#include "voice-subscriptions.h"
#include <libqmi-glib.h>
#include <libqrtr-glib.h>

enum stage { ALLOCATE, BIND, REGISTER, SNAPSHOT };
struct subscription {
	struct voice_subscriptions *owner;
	QmiClientVoice *client;
	unsigned int index, priority, generation, query_generation;
	enum stage stage;
	bool known;
};

struct voice_subscriptions {
	QrtrBus *bus;
	QrtrNode *node;
	QmiDevice *device;
	GCancellable *cancel;
	struct subscription subscriptions[2];
	voice_subscription_changed changed;
	void *data;
	unsigned int pending, selected;
	bool stopping, failed;
};

static void publish(struct voice_subscriptions *self)
{
	unsigned int priorities[] = { self->subscriptions[0].priority, self->subscriptions[1].priority };
	bool ready = !self->failed && self->subscriptions[0].known && self->subscriptions[1].known;
	unsigned int selected = voice_subscription_select(priorities, self->selected);

	if (self->stopping)
		return;
	if (selected)
		self->selected = selected;
	self->changed(selected, ready, self->data);
}

static void failed(struct voice_subscriptions *self, const GError *error)
{
	if (self->stopping)
		return;
	self->failed = true;
	g_warning("Cannot observe voice subscriptions: %s", error ? error->message : "transport removed");
	publish(self);
}

static void all_calls(QmiClientVoice *client, QmiIndicationVoiceAllCallStatusOutput *output, void *data)
{
	struct subscription *sub = data;
	GArray *calls = NULL;
	(void)client;
	if (sub->owner->stopping ||
	    !qmi_indication_voice_all_call_status_output_get_call_information(output, &calls, NULL))
		return;
	sub->priority = 0;
	for (guint i = 0; i < calls->len; i++) {
		QmiIndicationVoiceAllCallStatusOutputCallInformationCall *call =
			&g_array_index(calls, QmiIndicationVoiceAllCallStatusOutputCallInformationCall, i);
		sub->priority = MAX(sub->priority, voice_call_priority(call->state));
	}
	sub->known = true;
	sub->generation++;
	publish(sub->owner);
}

static void next_stage(struct subscription *sub);

static void stage_done(GObject *object, GAsyncResult *result, void *data)
{
	struct subscription *sub = data;
	struct voice_subscriptions *self = sub->owner;
	g_autoptr(GError) error = NULL;
	bool success = false;
	(void)object;
	self->pending--;
	switch (sub->stage) {
	case ALLOCATE:
		sub->client = QMI_CLIENT_VOICE(qmi_device_allocate_client_finish(self->device, result, &error));
		success = sub->client != NULL;
		break;
	case BIND: {
		g_autoptr(QmiMessage) reply = qmi_device_command_full_finish(self->device, result, &error);
		guint16 length = 0;
		const guint8 *status = reply ? qmi_message_get_raw_tlv(reply, 2, &length) : NULL;
		if (status && length == 4) {
			guint16 code = status[2] | (status[3] << 8);
			success = !status[0] && !status[1];
			if (!success)
				g_set_error(&error, QMI_PROTOCOL_ERROR, code, "VOICE bind subscription %u: %s",
					    sub->index + 1, qmi_protocol_error_get_string(code));
		} else if (!error) {
			g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid VOICE binding reply");
		}
		break;
	}
	case REGISTER: {
		g_autoptr(QmiMessageVoiceIndicationRegisterOutput) output =
			qmi_client_voice_indication_register_finish(sub->client, result, &error);
		success = output && qmi_message_voice_indication_register_output_get_result(output, &error);
		break;
	}
	case SNAPSHOT: {
		g_autoptr(QmiMessageVoiceGetAllCallInfoOutput) output =
			qmi_client_voice_get_all_call_info_finish(sub->client, result, &error);
		GArray *calls = NULL;
		success = output && qmi_message_voice_get_all_call_info_output_get_result(output, &error);
		if (success && sub->generation == sub->query_generation) {
			sub->priority = 0;
			qmi_message_voice_get_all_call_info_output_get_call_information(output, &calls, NULL);
			for (guint i = 0; calls && i < calls->len; i++) {
				QmiMessageVoiceGetAllCallInfoOutputCallInformationCall *call =
					&g_array_index(calls, QmiMessageVoiceGetAllCallInfoOutputCallInformationCall, i);
				sub->priority = MAX(sub->priority, voice_call_priority(call->state));
			}
		}
		if (success) sub->known = true;
		break;
	}
	}
	if (self->stopping)
		return;
	if (!success) {
		failed(self, error);
		return;
	}
	if (sub->stage == SNAPSHOT) {
		publish(self);
		return;
	}
	sub->stage++;
	next_stage(sub);
}

static void next_stage(struct subscription *sub)
{
	struct voice_subscriptions *self = sub->owner;
	self->pending++;
	switch (sub->stage) {
	case ALLOCATE:
		qmi_device_allocate_client(self->device, QMI_SERVICE_VOICE, QMI_CID_NONE, 5,
					  self->cancel, stage_done, sub);
		break;
	case BIND: {
		/* Qualcomm VOICE_BIND_SUBSCRIPTION 0x0044: TLV 1 is a one-byte,
		 * zero-based logical subscription. The binding is client-local.
		 */
		g_autoptr(QmiMessage) request = qmi_message_new(QMI_SERVICE_VOICE,
			qmi_client_get_cid(QMI_CLIENT(sub->client)),
			qmi_client_get_next_transaction_id(QMI_CLIENT(sub->client)), 0x0044);
		guint8 value = sub->index;
		g_signal_connect(sub->client, "all-call-status", G_CALLBACK(all_calls), sub);
		qmi_message_add_raw_tlv(request, 1, &value, 1, NULL);
		qmi_device_command_full(self->device, request, NULL, 5, self->cancel, stage_done, sub);
		break;
	}
	case REGISTER: {
		g_autoptr(QmiMessageVoiceIndicationRegisterInput) input = qmi_message_voice_indication_register_input_new();
		qmi_message_voice_indication_register_input_set_call_notification_events(input, TRUE, NULL);
		qmi_client_voice_indication_register(sub->client, input, 5, self->cancel, stage_done, sub);
		break;
	}
	case SNAPSHOT:
		sub->query_generation = sub->generation;
		qmi_client_voice_get_all_call_info(sub->client, NULL, 5, self->cancel, stage_done, sub);
		break;
	}
}

static void transport_removed(QmiDevice *device, void *data)
{ (void)device; failed(data, NULL); }

static void opened(GObject *object, GAsyncResult *result, void *data)
{
	struct voice_subscriptions *self = data;
	g_autoptr(GError) error = NULL;
	bool success = qmi_device_open_finish(QMI_DEVICE(object), result, &error);
	self->pending--;
	if (self->stopping) return;
	if (!success) { failed(self, error); return; }
	for (unsigned int i = 0; i < 2; i++) next_stage(&self->subscriptions[i]);
}

static void device_created(GObject *object, GAsyncResult *result, void *data)
{
	struct voice_subscriptions *self = data;
	g_autoptr(GError) error = NULL;
	(void)object;
	self->device = qmi_device_new_finish(result, &error);
	self->pending--;
	if (self->stopping) return;
	if (!self->device) { failed(self, error); return; }
	g_signal_connect(self->device, QMI_DEVICE_SIGNAL_REMOVED, G_CALLBACK(transport_removed), self);
	self->pending++;
	qmi_device_open(self->device, QMI_DEVICE_OPEN_FLAGS_NONE, 5, self->cancel, opened, self);
}

static void node_found(GObject *object, GAsyncResult *result, void *data)
{
	struct voice_subscriptions *self = data;
	g_autoptr(GError) error = NULL;
	self->node = qrtr_bus_wait_for_node_finish(QRTR_BUS(object), result, &error);
	self->pending--;
	if (self->stopping) return;
	if (!self->node) { failed(self, error); return; }
	self->pending++;
	qmi_device_new_from_node(self->node, self->cancel, device_created, self);
}

static void bus_created(GObject *object, GAsyncResult *result, void *data)
{
	struct voice_subscriptions *self = data;
	g_autoptr(GError) error = NULL;
	(void)object;
	self->bus = qrtr_bus_new_finish(result, &error);
	self->pending--;
	if (self->stopping) return;
	if (!self->bus) { failed(self, error); return; }
	self->pending++;
	qrtr_bus_wait_for_node(self->bus, 0, 5000, self->cancel, node_found, self);
}

struct voice_subscriptions *voice_subscriptions_new(voice_subscription_changed changed, void *data)
{
	struct voice_subscriptions *self = g_new0(struct voice_subscriptions, 1);
	self->changed = changed;
	self->data = data;
	self->cancel = g_cancellable_new();
	for (unsigned int i = 0; i < 2; i++) {
		self->subscriptions[i].owner = self;
		self->subscriptions[i].index = i;
	}
	self->pending++;
	qrtr_bus_new(1000, self->cancel, bus_created, self);
	return self;
}

static void finished(GObject *object, GAsyncResult *result, void *data)
{ (void)object; *(GAsyncResult **)data = g_object_ref(result); }

void voice_subscriptions_free(struct voice_subscriptions *self)
{
	if (!self) return;
	self->stopping = true;
	g_cancellable_cancel(self->cancel);
	while (self->pending) g_main_context_iteration(NULL, TRUE);
	if (self->device) g_signal_handlers_disconnect_by_data(self->device, self);
	for (unsigned int i = 0; i < 2; i++) {
		QmiClientVoice *client = self->subscriptions[i].client;
		GAsyncResult *result = NULL;
		if (!client) continue;
		g_signal_handlers_disconnect_by_data(client, &self->subscriptions[i]);
		qmi_device_release_client(self->device, QMI_CLIENT(client), QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
					 5, NULL, finished, &result);
		while (!result) g_main_context_iteration(NULL, TRUE);
		qmi_device_release_client_finish(self->device, result, NULL);
		g_object_unref(result);
		g_object_unref(client);
	}
	if (self->device && qmi_device_is_open(self->device)) {
		GAsyncResult *result = NULL;
		qmi_device_close_async(self->device, 5, NULL, finished, &result);
		while (!result) g_main_context_iteration(NULL, TRUE);
		qmi_device_close_finish(self->device, result, NULL);
		g_object_unref(result);
	}
	g_clear_object(&self->device);
	g_clear_object(&self->node);
	g_clear_object(&self->bus);
	g_clear_object(&self->cancel);
	g_free(self);
}
