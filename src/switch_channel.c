/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2011, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthm@freeswitch.org>
 * Michael Jerris <mike@jerris.com>
 *
 *
 * switch_channel.c -- Media Channel Interface
 *
 */

#include <switch.h>
#include <switch_channel.h>

struct switch_cause_table {
	const char *name;
	switch_call_cause_t cause;
};

static struct switch_cause_table CAUSE_CHART[] = {
	{"NONE", SWITCH_CAUSE_NONE},
	{"UNALLOCATED_NUMBER", SWITCH_CAUSE_UNALLOCATED_NUMBER},
	{"NO_ROUTE_TRANSIT_NET", SWITCH_CAUSE_NO_ROUTE_TRANSIT_NET},
	{"NO_ROUTE_DESTINATION", SWITCH_CAUSE_NO_ROUTE_DESTINATION},
	{"CHANNEL_UNACCEPTABLE", SWITCH_CAUSE_CHANNEL_UNACCEPTABLE},
	{"CALL_AWARDED_DELIVERED", SWITCH_CAUSE_CALL_AWARDED_DELIVERED},
	{"NORMAL_CLEARING", SWITCH_CAUSE_NORMAL_CLEARING},
	{"USER_BUSY", SWITCH_CAUSE_USER_BUSY},
	{"NO_USER_RESPONSE", SWITCH_CAUSE_NO_USER_RESPONSE},
	{"NO_ANSWER", SWITCH_CAUSE_NO_ANSWER},
	{"SUBSCRIBER_ABSENT", SWITCH_CAUSE_SUBSCRIBER_ABSENT},
	{"CALL_REJECTED", SWITCH_CAUSE_CALL_REJECTED},
	{"NUMBER_CHANGED", SWITCH_CAUSE_NUMBER_CHANGED},
	{"REDIRECTION_TO_NEW_DESTINATION", SWITCH_CAUSE_REDIRECTION_TO_NEW_DESTINATION},
	{"EXCHANGE_ROUTING_ERROR", SWITCH_CAUSE_EXCHANGE_ROUTING_ERROR},
	{"DESTINATION_OUT_OF_ORDER", SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER},
	{"INVALID_NUMBER_FORMAT", SWITCH_CAUSE_INVALID_NUMBER_FORMAT},
	{"FACILITY_REJECTED", SWITCH_CAUSE_FACILITY_REJECTED},
	{"RESPONSE_TO_STATUS_ENQUIRY", SWITCH_CAUSE_RESPONSE_TO_STATUS_ENQUIRY},
	{"NORMAL_UNSPECIFIED", SWITCH_CAUSE_NORMAL_UNSPECIFIED},
	{"NORMAL_CIRCUIT_CONGESTION", SWITCH_CAUSE_NORMAL_CIRCUIT_CONGESTION},
	{"NETWORK_OUT_OF_ORDER", SWITCH_CAUSE_NETWORK_OUT_OF_ORDER},
	{"NORMAL_TEMPORARY_FAILURE", SWITCH_CAUSE_NORMAL_TEMPORARY_FAILURE},
	{"SWITCH_CONGESTION", SWITCH_CAUSE_SWITCH_CONGESTION},
	{"ACCESS_INFO_DISCARDED", SWITCH_CAUSE_ACCESS_INFO_DISCARDED},
	{"REQUESTED_CHAN_UNAVAIL", SWITCH_CAUSE_REQUESTED_CHAN_UNAVAIL},
	{"PRE_EMPTED", SWITCH_CAUSE_PRE_EMPTED},
	{"FACILITY_NOT_SUBSCRIBED", SWITCH_CAUSE_FACILITY_NOT_SUBSCRIBED},
	{"OUTGOING_CALL_BARRED", SWITCH_CAUSE_OUTGOING_CALL_BARRED},
	{"INCOMING_CALL_BARRED", SWITCH_CAUSE_INCOMING_CALL_BARRED},
	{"BEARERCAPABILITY_NOTAUTH", SWITCH_CAUSE_BEARERCAPABILITY_NOTAUTH},
	{"BEARERCAPABILITY_NOTAVAIL", SWITCH_CAUSE_BEARERCAPABILITY_NOTAVAIL},
	{"SERVICE_UNAVAILABLE", SWITCH_CAUSE_SERVICE_UNAVAILABLE},
	{"CHAN_NOT_IMPLEMENTED", SWITCH_CAUSE_CHAN_NOT_IMPLEMENTED},
	{"FACILITY_NOT_IMPLEMENTED", SWITCH_CAUSE_FACILITY_NOT_IMPLEMENTED},
	{"SERVICE_NOT_IMPLEMENTED", SWITCH_CAUSE_SERVICE_NOT_IMPLEMENTED},
	{"INVALID_CALL_REFERENCE", SWITCH_CAUSE_INVALID_CALL_REFERENCE},
	{"INCOMPATIBLE_DESTINATION", SWITCH_CAUSE_INCOMPATIBLE_DESTINATION},
	{"INVALID_MSG_UNSPECIFIED", SWITCH_CAUSE_INVALID_MSG_UNSPECIFIED},
	{"MANDATORY_IE_MISSING", SWITCH_CAUSE_MANDATORY_IE_MISSING},
	{"MESSAGE_TYPE_NONEXIST", SWITCH_CAUSE_MESSAGE_TYPE_NONEXIST},
	{"WRONG_MESSAGE", SWITCH_CAUSE_WRONG_MESSAGE},
	{"IE_NONEXIST", SWITCH_CAUSE_IE_NONEXIST},
	{"INVALID_IE_CONTENTS", SWITCH_CAUSE_INVALID_IE_CONTENTS},
	{"WRONG_CALL_STATE", SWITCH_CAUSE_WRONG_CALL_STATE},
	{"RECOVERY_ON_TIMER_EXPIRE", SWITCH_CAUSE_RECOVERY_ON_TIMER_EXPIRE},
	{"MANDATORY_IE_LENGTH_ERROR", SWITCH_CAUSE_MANDATORY_IE_LENGTH_ERROR},
	{"PROTOCOL_ERROR", SWITCH_CAUSE_PROTOCOL_ERROR},
	{"INTERWORKING", SWITCH_CAUSE_INTERWORKING},
	{"SUCCESS", SWITCH_CAUSE_SUCCESS},
	{"ORIGINATOR_CANCEL", SWITCH_CAUSE_ORIGINATOR_CANCEL},
	{"CRASH", SWITCH_CAUSE_CRASH},
	{"SYSTEM_SHUTDOWN", SWITCH_CAUSE_SYSTEM_SHUTDOWN},
	{"LOSE_RACE", SWITCH_CAUSE_LOSE_RACE},
	{"MANAGER_REQUEST", SWITCH_CAUSE_MANAGER_REQUEST},
	{"BLIND_TRANSFER", SWITCH_CAUSE_BLIND_TRANSFER},
	{"ATTENDED_TRANSFER", SWITCH_CAUSE_ATTENDED_TRANSFER},
	{"ALLOTTED_TIMEOUT", SWITCH_CAUSE_ALLOTTED_TIMEOUT},
	{"USER_CHALLENGE", SWITCH_CAUSE_USER_CHALLENGE},
	{"MEDIA_TIMEOUT", SWITCH_CAUSE_MEDIA_TIMEOUT},
	{"PICKED_OFF", SWITCH_CAUSE_PICKED_OFF},
	{"USER_NOT_REGISTERED", SWITCH_CAUSE_USER_NOT_REGISTERED},
	{"PROGRESS_TIMEOUT", SWITCH_CAUSE_PROGRESS_TIMEOUT},
	{NULL, 0}
};

typedef enum {
	OCF_HANGUP = (1 << 0)
} opaque_channel_flag_t;

typedef enum {
	LP_NEITHER,
	LP_ORIGINATOR,
	LP_ORIGINATEE
} switch_originator_type_t;

struct switch_channel {
	char *name;
	switch_call_direction_t direction;
	switch_queue_t *dtmf_queue;
	switch_queue_t *dtmf_log_queue;
	switch_mutex_t *dtmf_mutex;
	switch_mutex_t *flag_mutex;
	switch_mutex_t *state_mutex;
	switch_mutex_t *profile_mutex;
	switch_core_session_t *session;
	switch_channel_state_t state;
	switch_channel_state_t running_state;
	switch_channel_callstate_t callstate;
	uint32_t flags[CF_FLAG_MAX];
	uint32_t caps[CC_FLAG_MAX];
	uint8_t state_flags[CF_FLAG_MAX];
	uint32_t private_flags;
	switch_caller_profile_t *caller_profile;
	const switch_state_handler_table_t *state_handlers[SWITCH_MAX_STATE_HANDLERS];
	int state_handler_index;
	switch_event_t *variables;
	switch_event_t *scope_variables;
	switch_hash_t *private_hash;
	switch_hash_t *app_flag_hash;
	switch_call_cause_t hangup_cause;
	int vi;
	int event_count;
	int profile_index;
	opaque_channel_flag_t opaque_flags;
	switch_originator_type_t last_profile_type;
	switch_caller_extension_t *queued_extension;
};


SWITCH_DECLARE(const char *) switch_channel_cause2str(switch_call_cause_t cause)
{
	uint8_t x;
	const char *str = "UNKNOWN";

	for (x = 0; x < (sizeof(CAUSE_CHART) / sizeof(struct switch_cause_table)) - 1; x++) {
		if (CAUSE_CHART[x].cause == cause) {
			str = CAUSE_CHART[x].name;
			break;
		}
	}

	return str;
}

SWITCH_DECLARE(switch_call_cause_t) switch_channel_str2cause(const char *str)
{
	uint8_t x;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;

	if (*str > 47 && *str < 58) {
		cause = atoi(str);
	} else {
		for (x = 0; x < (sizeof(CAUSE_CHART) / sizeof(struct switch_cause_table)) - 1 && CAUSE_CHART[x].name; x++) {
			if (!strcasecmp(CAUSE_CHART[x].name, str)) {
				cause = CAUSE_CHART[x].cause;
				break;
			}
		}
	}
	return cause;
}

SWITCH_DECLARE(switch_call_cause_t) switch_channel_get_cause(switch_channel_t *channel)
{
	return channel->hangup_cause;
}


SWITCH_DECLARE(switch_call_cause_t *) switch_channel_get_cause_ptr(switch_channel_t *channel)
{
	return &channel->hangup_cause;
}


struct switch_callstate_table {
	const char *name;
	switch_channel_callstate_t callstate;
};
static struct switch_callstate_table CALLSTATE_CHART[] = {
    {"DOWN", CCS_DOWN},
    {"DIALING", CCS_DIALING},
    {"RINGING", CCS_RINGING},
    {"EARLY", CCS_EARLY},
    {"ACTIVE", CCS_ACTIVE},
    {"HELD", CCS_HELD},
    {"HANGUP", CCS_HANGUP},
    {NULL, 0}
};


SWITCH_DECLARE(void) switch_channel_perform_set_callstate(switch_channel_t *channel, switch_channel_callstate_t callstate, 
														  const char *file, const char *func, int line)
{
	switch_event_t *event;
	switch_channel_callstate_t o_callstate = channel->callstate;

	if (o_callstate == callstate) return;
	
	channel->callstate = callstate;
	
	switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_DEBUG,
					  "(%s) Callstate Change %s -> %s\n", channel->name, 
					  switch_channel_callstate2str(o_callstate), switch_channel_callstate2str(callstate));
	
	if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_CALLSTATE) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Original-Channel-Call-State", switch_channel_callstate2str(o_callstate));
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Channel-Call-State-Number", "%d", callstate);
		switch_channel_event_set_data(channel, event);
		switch_event_fire(&event);
	}
}

SWITCH_DECLARE(switch_channel_callstate_t) switch_channel_get_callstate(switch_channel_t *channel)
{
	return channel->callstate;
}


SWITCH_DECLARE(const char *) switch_channel_callstate2str(switch_channel_callstate_t callstate)
{
	uint8_t x;
	const char *str = "UNKNOWN";

	for (x = 0; x < (sizeof(CALLSTATE_CHART) / sizeof(struct switch_cause_table)) - 1; x++) {
		if (CALLSTATE_CHART[x].callstate == callstate) {
			str = CALLSTATE_CHART[x].name;
			break;
		}
	}

	return str;
}


SWITCH_DECLARE(switch_call_cause_t) switch_channel_str2callstate(const char *str)
{
	uint8_t x;
	switch_channel_callstate_t callstate = SWITCH_CAUSE_NONE;

	if (*str > 47 && *str < 58) {
		callstate = atoi(str);
	} else {
		for (x = 0; x < (sizeof(CALLSTATE_CHART) / sizeof(struct switch_callstate_table)) - 1 && CALLSTATE_CHART[x].name; x++) {
			if (!strcasecmp(CALLSTATE_CHART[x].name, str)) {
				callstate = CALLSTATE_CHART[x].callstate;
				break;
			}
		}
	}
	return callstate;
}



SWITCH_DECLARE(void) switch_channel_perform_audio_sync(switch_channel_t *channel, const char *file, const char *func, int line)
{
	if (switch_channel_media_ready(channel)) {
		switch_core_session_message_t msg = { 0 };
		msg.message_id = SWITCH_MESSAGE_INDICATE_AUDIO_SYNC;
		msg.from = channel->name;
		msg._file = file;
		msg._func = func;
		msg._line = line;
		switch_core_session_receive_message(channel->session, &msg);
	}
}



SWITCH_DECLARE(switch_call_cause_t) switch_channel_cause_q850(switch_call_cause_t cause)
{
	if (cause <= SWITCH_CAUSE_INTERWORKING) {
		return cause;
	} else {
		return SWITCH_CAUSE_NORMAL_CLEARING;
	}
}

SWITCH_DECLARE(switch_call_cause_t) switch_channel_get_cause_q850(switch_channel_t *channel)
{
	return switch_channel_cause_q850(channel->hangup_cause);
}

SWITCH_DECLARE(switch_channel_timetable_t *) switch_channel_get_timetable(switch_channel_t *channel)
{
	switch_channel_timetable_t *times = NULL;

	if (channel->caller_profile) {
		switch_mutex_lock(channel->profile_mutex);
		times = channel->caller_profile->times;
		switch_mutex_unlock(channel->profile_mutex);
	}

	return times;
}

SWITCH_DECLARE(switch_call_direction_t) switch_channel_direction(switch_channel_t *channel)
{
	return channel->direction;
}

SWITCH_DECLARE(switch_status_t) switch_channel_alloc(switch_channel_t **channel, switch_call_direction_t direction, switch_memory_pool_t *pool)
{
	switch_assert(pool != NULL);

	if (((*channel) = switch_core_alloc(pool, sizeof(switch_channel_t))) == 0) {
		return SWITCH_STATUS_MEMERR;
	}

	switch_event_create_plain(&(*channel)->variables, SWITCH_EVENT_CHANNEL_DATA);

	switch_core_hash_init(&(*channel)->private_hash, pool);
	switch_queue_create(&(*channel)->dtmf_queue, SWITCH_DTMF_LOG_LEN, pool);
	switch_queue_create(&(*channel)->dtmf_log_queue, SWITCH_DTMF_LOG_LEN, pool);

	switch_mutex_init(&(*channel)->dtmf_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&(*channel)->flag_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&(*channel)->state_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&(*channel)->profile_mutex, SWITCH_MUTEX_NESTED, pool);
	(*channel)->hangup_cause = SWITCH_CAUSE_NONE;
	(*channel)->name = "";
	(*channel)->direction = direction;
	switch_channel_set_variable(*channel, "direction", switch_channel_direction(*channel) == SWITCH_CALL_DIRECTION_OUTBOUND ? "outbound" : "inbound");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_size_t) switch_channel_has_dtmf(switch_channel_t *channel)
{
	switch_size_t has;

	switch_mutex_lock(channel->dtmf_mutex);
	has = switch_queue_size(channel->dtmf_queue);
	switch_mutex_unlock(channel->dtmf_mutex);

	return has;
}

SWITCH_DECLARE(switch_status_t) switch_channel_queue_dtmf(switch_channel_t *channel, const switch_dtmf_t *dtmf)
{
	switch_status_t status;
	void *pop;
	switch_dtmf_t new_dtmf = { 0 };

	switch_assert(dtmf);

	switch_mutex_lock(channel->dtmf_mutex);
	new_dtmf = *dtmf;

	if ((status = switch_core_session_recv_dtmf(channel->session, dtmf) != SWITCH_STATUS_SUCCESS)) {
		goto done;
	}

	if (is_dtmf(new_dtmf.digit)) {
		switch_dtmf_t *dt;
		int x = 0;
		char str[2] = "";

		str[0] = new_dtmf.digit;
		
		if (new_dtmf.duration > switch_core_max_dtmf_duration(0)) {
			switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG1, "%s EXCESSIVE DTMF DIGIT [%s] LEN [%d]\n",
							  switch_channel_get_name(channel), str, new_dtmf.duration);
			new_dtmf.duration = switch_core_max_dtmf_duration(0);
		} else if (new_dtmf.duration < switch_core_min_dtmf_duration(0)) {
			switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG1, "%s SHORT DTMF DIGIT [%s] LEN [%d]\n",
							  switch_channel_get_name(channel), str, new_dtmf.duration);
			new_dtmf.duration = switch_core_min_dtmf_duration(0);
		} else if (!new_dtmf.duration) {
			new_dtmf.duration = switch_core_default_dtmf_duration(0);
		}

		switch_zmalloc(dt, sizeof(*dt));
		*dt = new_dtmf;

		while (switch_queue_trypush(channel->dtmf_queue, dt) != SWITCH_STATUS_SUCCESS) {
			if (switch_queue_trypop(channel->dtmf_queue, &pop) == SWITCH_STATUS_SUCCESS) {
				free(pop);
			}
			if (++x > 100) {
				status = SWITCH_STATUS_FALSE;
				free(dt);
				goto done;
			}
		}
	}

	status = SWITCH_STATUS_SUCCESS;

  done:

	switch_mutex_unlock(channel->dtmf_mutex);

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_channel_queue_dtmf_string(switch_channel_t *channel, const char *dtmf_string)
{
	char *p;
	switch_dtmf_t dtmf = { 0, switch_core_default_dtmf_duration(0), 0, SWITCH_DTMF_APP };
	int sent = 0, dur;
	char *string;
	int i, argc;
	char *argv[256];

	if (zstr(dtmf_string)) {
		return SWITCH_STATUS_FALSE;
	}


	dtmf.flags = DTMF_FLAG_SKIP_PROCESS;

	if (*dtmf_string == '~') {
		dtmf_string++;
		dtmf.flags = 0;
	}

	string = switch_core_session_strdup(channel->session, dtmf_string);
	argc = switch_separate_string(string, '+', argv, (sizeof(argv) / sizeof(argv[0])));

	for (i = 0; i < argc; i++) {
		dtmf.duration = switch_core_default_dtmf_duration(0);
		dur = switch_core_default_dtmf_duration(0) / 8;
		if ((p = strchr(argv[i], '@'))) {
			*p++ = '\0';
			if ((dur = atoi(p)) > (int)switch_core_min_dtmf_duration(0) / 8) {
				dtmf.duration = dur * 8;
			}
		}

		for (p = argv[i]; p && *p; p++) {
			if (is_dtmf(*p)) {
				dtmf.digit = *p;

				if (dtmf.duration > switch_core_max_dtmf_duration(0)) {
					switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_WARNING, "EXCESSIVE DTMF DIGIT LEN %c %d\n", dtmf.digit, dtmf.duration);
					dtmf.duration = switch_core_max_dtmf_duration(0);
				} else if (dtmf.duration < switch_core_min_dtmf_duration(0)) {
					switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_WARNING, "SHORT DTMF DIGIT LEN %c %d\n", dtmf.digit, dtmf.duration);
					dtmf.duration = switch_core_min_dtmf_duration(0);
				} else if (!dtmf.duration) {
					dtmf.duration = switch_core_default_dtmf_duration(0);
				}

				if (switch_channel_queue_dtmf(channel, &dtmf) == SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG, "%s Queue dtmf\ndigit=%c ms=%u samples=%u\n",
									  switch_channel_get_name(channel), dtmf.digit, dur, dtmf.duration);
					sent++;
				}
			}
		}

	}

	return sent ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_channel_dequeue_dtmf(switch_channel_t *channel, switch_dtmf_t *dtmf)
{
	switch_event_t *event;
	void *pop;
	switch_dtmf_t *dt;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_mutex_lock(channel->dtmf_mutex);

	if (switch_queue_trypop(channel->dtmf_queue, &pop) == SWITCH_STATUS_SUCCESS) {
		dt = (switch_dtmf_t *) pop;
		*dtmf = *dt;

		if (switch_queue_trypush(channel->dtmf_log_queue, dt) != SWITCH_STATUS_SUCCESS) {
			free(dt);
		}

		dt = NULL;

		if (dtmf->duration > switch_core_max_dtmf_duration(0)) {
			switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_WARNING, "%s EXCESSIVE DTMF DIGIT [%c] LEN [%d]\n",
							  switch_channel_get_name(channel), dtmf->digit, dtmf->duration);
			dtmf->duration = switch_core_max_dtmf_duration(0);
		} else if (dtmf->duration < switch_core_min_dtmf_duration(0)) {
			switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_WARNING, "%s SHORT DTMF DIGIT [%c] LEN [%d]\n",
							  switch_channel_get_name(channel), dtmf->digit, dtmf->duration);
			dtmf->duration = switch_core_min_dtmf_duration(0);
		} else if (!dtmf->duration) {
			dtmf->duration = switch_core_default_dtmf_duration(0);
		}

		status = SWITCH_STATUS_SUCCESS;
	}
	switch_mutex_unlock(channel->dtmf_mutex);

	if (status == SWITCH_STATUS_SUCCESS && switch_event_create(&event, SWITCH_EVENT_DTMF) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "DTMF-Digit", "%c", dtmf->digit);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "DTMF-Duration", "%u", dtmf->duration);
		if (switch_channel_test_flag(channel, CF_DIVERT_EVENTS)) {
			switch_core_session_queue_event(channel->session, &event);
		} else {
			switch_event_fire(&event);
		}
	}

	return status;
}

SWITCH_DECLARE(switch_size_t) switch_channel_dequeue_dtmf_string(switch_channel_t *channel, char *dtmf_str, switch_size_t len)
{
	switch_size_t x = 0;
	switch_dtmf_t dtmf = { 0 };

	memset(dtmf_str, 0, len);

	while (x < len - 1 && switch_channel_dequeue_dtmf(channel, &dtmf) == SWITCH_STATUS_SUCCESS) {
		dtmf_str[x++] = dtmf.digit;
	}

	return x;

}

SWITCH_DECLARE(void) switch_channel_flush_dtmf(switch_channel_t *channel)
{
	void *pop;

	switch_mutex_lock(channel->dtmf_mutex);
	while (switch_queue_trypop(channel->dtmf_queue, &pop) == SWITCH_STATUS_SUCCESS) {
		switch_dtmf_t *dt = (switch_dtmf_t *) pop;
		if (channel->state >= CS_HANGUP || switch_queue_trypush(channel->dtmf_log_queue, dt) != SWITCH_STATUS_SUCCESS) {
			free(dt);
		}
	}
	switch_mutex_unlock(channel->dtmf_mutex);
}

SWITCH_DECLARE(void) switch_channel_uninit(switch_channel_t *channel)
{
	void *pop;
	switch_channel_flush_dtmf(channel);
	while (switch_queue_trypop(channel->dtmf_log_queue, &pop) == SWITCH_STATUS_SUCCESS) {
		switch_safe_free(pop);
	}
	switch_core_hash_destroy(&channel->private_hash);
	if (channel->app_flag_hash) {
		switch_core_hash_destroy(&channel->app_flag_hash);
	}
	switch_mutex_lock(channel->profile_mutex);
	switch_event_destroy(&channel->variables);
	switch_mutex_unlock(channel->profile_mutex);
}

SWITCH_DECLARE(switch_status_t) switch_channel_init(switch_channel_t *channel, switch_core_session_t *session, switch_channel_state_t state,
													switch_channel_flag_t flag)
{
	switch_assert(channel != NULL);
	channel->state = state;
	switch_channel_set_flag(channel, flag);
	channel->session = session;
	channel->running_state = CS_NONE;
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(void) switch_channel_perform_presence(switch_channel_t *channel, const char *rpid, const char *status, const char *id,
													 const char *file, const char *func, int line)
{
	switch_event_t *event;
	switch_event_types_t type = SWITCH_EVENT_PRESENCE_IN;
	const char *call_info = NULL;

	if (!status) {
		type = SWITCH_EVENT_PRESENCE_OUT;
	}

	if (!id) {
		id = switch_channel_get_variable(channel, "presence_id");
	}

	if (!id) {
		return;
	}

	call_info = switch_channel_get_variable(channel, "presence_call_info");

	if (switch_event_create(&event, type) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", __FILE__);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", __FILE__);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "from", id);
		if (type == SWITCH_EVENT_PRESENCE_IN) {
			if (!rpid) {
				rpid = "unknown";
			}
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "rpid", rpid);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "status", status);
		}
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");

		if (call_info) {
			char *call_info_state = "active";

			if (!switch_channel_up(channel)) {
				call_info_state = "idle";
			} else if (!strcasecmp(status, "hold-private")) {
				call_info_state = "held-private";
			} else if (!strcasecmp(status, "hold")) {
				call_info_state = "held";
			} else if (!switch_channel_test_flag(channel, CF_ANSWERED)) {
				if (channel->direction == SWITCH_CALL_DIRECTION_OUTBOUND) {
					call_info_state = "progressing";
				} else {
					call_info_state = "alerting";
				}
			}

			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "presence-call-info-state", call_info_state);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "presence-call-info", call_info);
		}

		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "presence-call-direction",
									   channel->direction == SWITCH_CALL_DIRECTION_OUTBOUND ? "outbound" : "inbound");

		
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "event_count", "%d", channel->event_count++);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Presence-Calling-File", file);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Presence-Calling-Function", func);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Presence-Calling-Line", "%d", line);

		if (switch_true(switch_channel_get_variable(channel, "presence_privacy"))) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Presence-Privacy", "true");
		}

		switch_event_fire(&event);
	}
}

SWITCH_DECLARE(void) switch_channel_mark_hold(switch_channel_t *channel, switch_bool_t on)
{
	switch_event_t *event;

	if (!!on == !!switch_channel_test_flag(channel, CF_LEG_HOLDING)) {
		goto end;
	}
	
	if (on) {
		switch_channel_set_flag(channel, CF_LEG_HOLDING);
	} else {
		switch_channel_clear_flag(channel, CF_LEG_HOLDING);
	}

	if (switch_event_create(&event, on ? SWITCH_EVENT_CHANNEL_HOLD : SWITCH_EVENT_CHANNEL_UNHOLD) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_fire(&event);
	}

 end:

	if (on) {
		if (switch_true(switch_channel_get_variable(channel, "flip_record_on_hold"))) {
			switch_core_session_t *other_session;
			if (switch_core_session_get_partner(channel->session, &other_session) == SWITCH_STATUS_SUCCESS) {
				switch_core_media_bug_transfer_recordings(channel->session, other_session);
				switch_core_session_rwunlock(other_session);
			}
		}
	}

}

SWITCH_DECLARE(const char *) switch_channel_get_hold_music(switch_channel_t *channel)
{
	const char *var;

	if (!(var = switch_channel_get_variable(channel, SWITCH_TEMP_HOLD_MUSIC_VARIABLE))) {
		var = switch_channel_get_variable(channel, SWITCH_HOLD_MUSIC_VARIABLE);
	}

	return var;
}

SWITCH_DECLARE(const char *) switch_channel_get_hold_music_partner(switch_channel_t *channel)
{
	switch_core_session_t *session;
	const char *r = NULL;

	if (switch_core_session_get_partner(channel->session, &session) == SWITCH_STATUS_SUCCESS) {
		r = switch_channel_get_hold_music(switch_core_session_get_channel(session));
		switch_core_session_rwunlock(session);
	}

	return r;
}

SWITCH_DECLARE(void) switch_channel_set_scope_variables(switch_channel_t *channel, switch_event_t **event)
{
	switch_mutex_lock(channel->profile_mutex);

	if (event && *event) { /* push */
		(*event)->next = channel->scope_variables;
		channel->scope_variables = *event;
		*event = NULL;
	} else if (channel->scope_variables) { /* pop */
		switch_event_t *top_event = channel->scope_variables;
		channel->scope_variables = channel->scope_variables->next;
		switch_event_destroy(&top_event);
	}

	switch_mutex_unlock(channel->profile_mutex);
	
}

SWITCH_DECLARE(switch_status_t) switch_channel_get_scope_variables(switch_channel_t *channel, switch_event_t **event)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_event_t *new_event;

	switch_mutex_lock(channel->profile_mutex);
	if (channel->scope_variables) {
		switch_event_t *ep;
		switch_event_header_t *hp;

		switch_event_create_plain(&new_event, SWITCH_EVENT_CHANNEL_DATA);
		status = SWITCH_STATUS_SUCCESS;
		*event = new_event;

		for (ep = channel->scope_variables; ep; ep = ep->next) {
			for (hp = ep->headers; hp; hp = hp->next) {
				if (!switch_event_get_header(new_event, hp->value)) {
					switch_event_add_header_string(new_event, SWITCH_STACK_BOTTOM, hp->name, hp->value);
				}
			}
		}
	}
	switch_mutex_unlock(channel->profile_mutex);

	return status;
}

SWITCH_DECLARE(const char *) switch_channel_get_variable_dup(switch_channel_t *channel, const char *varname, switch_bool_t dup, int idx)
{
	const char *v = NULL, *r = NULL, *vdup = NULL;
	switch_assert(channel != NULL);

	switch_mutex_lock(channel->profile_mutex);

	if (channel->scope_variables) {
		switch_event_t *ep;

		for (ep = channel->scope_variables; ep; ep = ep->next) {
			if ((v = switch_event_get_header_idx(ep, varname, idx))) {
				break;
			}
		}
	}

	if (!v && (!channel->variables || !(v = switch_event_get_header_idx(channel->variables, varname, idx)))) {
		switch_caller_profile_t *cp = switch_channel_get_caller_profile(channel);

		if (cp) {
			if (!strncmp(varname, "aleg_", 5)) {
				cp = cp->originator_caller_profile;
				varname += 5;
			} else if (!strncmp(varname, "bleg_", 5)) {
				cp = cp->originatee_caller_profile;
				varname += 5;
			}
		}

		if (!cp || !(v = switch_caller_get_field_by_name(cp, varname))) {
			if ((vdup = switch_core_get_variable_pdup(varname, switch_core_session_get_pool(channel->session)))) {
				v = vdup;
			}
		}
	}

	if (dup && v != vdup) {
		if (v) {
			r = switch_core_session_strdup(channel->session, v);
		}
	} else {
		r = v;
	}

	switch_mutex_unlock(channel->profile_mutex);

	return r;
}

SWITCH_DECLARE(const char *) switch_channel_get_variable_partner(switch_channel_t *channel, const char *varname)
{
	const char *uuid;
	const char *val = NULL, *r = NULL;
	switch_assert(channel != NULL);

	if (!zstr(varname)) {
		if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
			switch_core_session_t *session;
			if ((session = switch_core_session_locate(uuid))) {
				switch_channel_t *tchannel = switch_core_session_get_channel(session);
				val = switch_channel_get_variable(tchannel, varname);
				switch_core_session_rwunlock(session);
			}
		}
	}

	if (val)
		r = switch_core_session_strdup(channel->session, val);

	return r;
}


SWITCH_DECLARE(void) switch_channel_variable_last(switch_channel_t *channel)
{
	switch_assert(channel != NULL);
	if (!channel->vi) {
		return;
	}
	channel->vi = 0;
	switch_mutex_unlock(channel->profile_mutex);

}

SWITCH_DECLARE(switch_event_header_t *) switch_channel_variable_first(switch_channel_t *channel)
{
	switch_event_header_t *hi = NULL;

	switch_assert(channel != NULL);
	switch_mutex_lock(channel->profile_mutex);
	if (channel->variables && (hi = channel->variables->headers)) {
		channel->vi = 1;
	} else {
		switch_mutex_unlock(channel->profile_mutex);
	}

	return hi;
}

SWITCH_DECLARE(switch_status_t) switch_channel_set_private(switch_channel_t *channel, const char *key, const void *private_info)
{
	switch_assert(channel != NULL);
	switch_core_hash_insert_locked(channel->private_hash, key, private_info, channel->profile_mutex);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(void *) switch_channel_get_private(switch_channel_t *channel, const char *key)
{
	void *val;
	switch_assert(channel != NULL);
	val = switch_core_hash_find_locked(channel->private_hash, key, channel->profile_mutex);
	return val;
}

SWITCH_DECLARE(void *) switch_channel_get_private_partner(switch_channel_t *channel, const char *key)
{
	const char *uuid;
	void *val = NULL;

	switch_assert(channel != NULL);

	if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
		switch_core_session_t *session;
		if ((session = switch_core_session_locate(uuid))) {
			val = switch_core_hash_find_locked(channel->private_hash, key, channel->profile_mutex);
			switch_core_session_rwunlock(session);
		}
	}

	return val;
}

SWITCH_DECLARE(switch_status_t) switch_channel_set_name(switch_channel_t *channel, const char *name)
{
	const char *old = NULL;

	switch_assert(channel != NULL);
	if (!zstr(channel->name)) {
		old = channel->name;
	}
	channel->name = NULL;
	if (name) {
		char *uuid = switch_core_session_get_uuid(channel->session);
		channel->name = switch_core_session_strdup(channel->session, name);
		switch_channel_set_variable(channel, SWITCH_CHANNEL_NAME_VARIABLE, name);
		if (old) {
			switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_NOTICE, "Rename Channel %s->%s [%s]\n", old, name, uuid);
		} else {
			switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_NOTICE, "New Channel %s [%s]\n", name, uuid);
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(char *) switch_channel_get_name(switch_channel_t *channel)
{
	switch_assert(channel != NULL);
	return (!zstr(channel->name)) ? channel->name : "N/A";
}

SWITCH_DECLARE(switch_status_t) switch_channel_set_profile_var(switch_channel_t *channel, const char *name, const char *val)
{
	char *v;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_mutex_lock(channel->profile_mutex);

	if (!zstr(val)) {
		v = switch_core_strdup(channel->caller_profile->pool, val);
	} else {
		v = SWITCH_BLANK_STRING;
	}

	if (!strcasecmp(name, "dialplan")) {
		channel->caller_profile->dialplan = v;
	} else if (!strcasecmp(name, "username")) {
		channel->caller_profile->username = v;
	} else if (!strcasecmp(name, "caller_id_name")) {
		channel->caller_profile->caller_id_name = v;
	} else if (!strcasecmp(name, "caller_id_number")) {
		channel->caller_profile->caller_id_number = v;
	} else if (!strcasecmp(name, "callee_id_name")) {
		channel->caller_profile->callee_id_name = v;
	} else if (!strcasecmp(name, "callee_id_number")) {
		channel->caller_profile->callee_id_number = v;
	} else if (val && !strcasecmp(name, "caller_ton")) {
		channel->caller_profile->caller_ton = (uint8_t) atoi(v);
	} else if (val && !strcasecmp(name, "caller_numplan")) {
		channel->caller_profile->caller_numplan = (uint8_t) atoi(v);
	} else if (val && !strcasecmp(name, "destination_number_ton")) {
		channel->caller_profile->destination_number_ton = (uint8_t) atoi(v);
	} else if (val && !strcasecmp(name, "destination_number_numplan")) {
		channel->caller_profile->destination_number_numplan = (uint8_t) atoi(v);
	} else if (!strcasecmp(name, "ani")) {
		channel->caller_profile->ani = v;
	} else if (!strcasecmp(name, "aniii")) {
		channel->caller_profile->aniii = v;
	} else if (!strcasecmp(name, "network_addr")) {
		channel->caller_profile->network_addr = v;
	} else if (!strcasecmp(name, "rdnis")) {
		channel->caller_profile->rdnis = v;
	} else if (!strcasecmp(name, "destination_number")) {
		channel->caller_profile->destination_number = v;
	} else if (!strcasecmp(name, "uuid")) {
		channel->caller_profile->uuid = v;
	} else if (!strcasecmp(name, "source")) {
		channel->caller_profile->source = v;
	} else if (!strcasecmp(name, "context")) {
		channel->caller_profile->context = v;
	} else if (!strcasecmp(name, "chan_name")) {
		channel->caller_profile->chan_name = v;
	} else {
		profile_node_t *pn, *n = switch_core_alloc(channel->caller_profile->pool, sizeof(*n));
		
		n->var = switch_core_strdup(channel->caller_profile->pool, name);
		n->val = v;

		if (!channel->caller_profile->soft) {
			channel->caller_profile->soft = n;
		} else {
			for(pn = channel->caller_profile->soft; pn && pn->next; pn = pn->next);
			
			if (pn) {
				pn->next = n;
			}
		}
	}
	switch_mutex_unlock(channel->profile_mutex);

	return status;
}


SWITCH_DECLARE(void) switch_channel_process_export(switch_channel_t *channel, switch_channel_t *peer_channel, 
												   switch_event_t *var_event, const char *export_varname)
{

	const char *export_vars = switch_channel_get_variable(channel, export_varname);
	char *cptmp = switch_core_session_strdup(channel->session, export_vars);
	int argc;
	char *argv[256];

	if (zstr(export_vars)) return;


	if (var_event) {
		switch_event_del_header(var_event, export_varname);
		switch_event_add_header_string(var_event, SWITCH_STACK_BOTTOM, export_varname, export_vars);
	}
	
	if (peer_channel) {
		switch_channel_set_variable(peer_channel, export_varname, export_vars);
	}

	if ((argc = switch_separate_string(cptmp, ',', argv, (sizeof(argv) / sizeof(argv[0]))))) {
		int x;
		
		for (x = 0; x < argc; x++) {
			const char *vval;
			if ((vval = switch_channel_get_variable(channel, argv[x]))) {
				char *vvar = argv[x];
				if (!strncasecmp(vvar, "nolocal:", 8)) { /* remove this later ? */
					vvar += 8;
				} else if (!strncasecmp(vvar, "_nolocal_", 9)) {
					vvar += 9;
				}
				if (var_event) {
					switch_event_del_header(var_event, vvar);
					switch_event_add_header_string(var_event, SWITCH_STACK_BOTTOM, vvar, vval);
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(channel->session), SWITCH_LOG_DEBUG, 
									  "%s EXPORTING[%s] [%s]=[%s] to event\n", 
									  switch_channel_get_name(channel), 
									  export_varname,
									  vvar, vval);
				}
				if (peer_channel) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(channel->session), SWITCH_LOG_DEBUG, 
									  "%s EXPORTING[%s] [%s]=[%s] to %s\n", 
									  switch_channel_get_name(channel), 
									  export_varname,
									  vvar, vval, switch_channel_get_name(peer_channel));
					switch_channel_set_variable(peer_channel, vvar, vval);
				}
			}
		}
	}


}

SWITCH_DECLARE(switch_status_t) switch_channel_export_variable_var_check(switch_channel_t *channel, 
																		 const char *varname, const char *val, 
																		 const char *export_varname, switch_bool_t var_check)
{
	char *var_name = NULL;
	const char *exports;
	char *var, *new_exports, *new_exports_d = NULL;
	int local = 1;

	exports = switch_channel_get_variable(channel, export_varname);

	var = switch_core_session_strdup(channel->session, varname);

	if (var) {
		if (!strncasecmp(var, "nolocal:", 8)) { /* remove this later ? */
			var_name = var + 8;
			local = 0;
		} else if (!strncasecmp(var, "_nolocal_", 9)) {
			var_name = var + 9;
			local = 0;
		} else {
			var_name = var;
		}
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(channel->session), SWITCH_LOG_DEBUG, "EXPORT (%s) %s[%s]=[%s]\n", 
					  export_varname, local ? "" : "(REMOTE ONLY) ",
					  var_name ? var_name : "", val ? val : "UNDEF");

	
	switch_channel_set_variable_var_check(channel, var, val, var_check);

	if (var && val) {
		if (exports) {
			new_exports_d = switch_mprintf("%s,%s", exports, var);
			new_exports = new_exports_d;
		} else {
			new_exports = var;
		}

		switch_channel_set_variable(channel, export_varname, new_exports);

		switch_safe_free(new_exports_d);
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_channel_export_variable_printf(switch_channel_t *channel, const char *varname, 
																	  const char *export_varname, const char *fmt, ...)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	char *data = NULL;
	va_list ap;
	int ret;
	
	switch_assert(channel != NULL);
	
	va_start(ap, fmt);
	ret = switch_vasprintf(&data, fmt, ap);
	va_end(ap);
	
	if (ret == -1) {
		return SWITCH_STATUS_FALSE;
	}
	
	status = switch_channel_export_variable(channel, varname, export_varname, data);
	
	free(data);
	
	return status;
}

SWITCH_DECLARE(switch_status_t) switch_channel_set_variable_var_check(switch_channel_t *channel,
																	  const char *varname, const char *value, switch_bool_t var_check)
{
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_assert(channel != NULL);

	switch_mutex_lock(channel->profile_mutex);
	if (channel->variables && !zstr(varname)) {
		if (zstr(value)) {
			switch_event_del_header(channel->variables, varname);
		} else {
			int ok = 1;

			if (var_check) {
				ok = !switch_string_var_check_const(value);
			}
			if (ok) {
				switch_event_add_header_string(channel->variables, SWITCH_STACK_BOTTOM, varname, value);
			} else {
				switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_CRIT, "Invalid data (${%s} contains a variable)\n", varname);
			}
		}
		status = SWITCH_STATUS_SUCCESS;
	}
	switch_mutex_unlock(channel->profile_mutex);

	return status;
}


SWITCH_DECLARE(switch_status_t) switch_channel_add_variable_var_check(switch_channel_t *channel,
																	  const char *varname, const char *value, switch_bool_t var_check, switch_stack_t stack)
{
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_assert(channel != NULL);

	switch_mutex_lock(channel->profile_mutex);
	if (channel->variables && !zstr(varname)) {
		if (zstr(value)) {
			switch_event_del_header(channel->variables, varname);
		} else {
			int ok = 1;

			if (var_check) {
				ok = !switch_string_var_check_const(value);
			}
			if (ok) {
				switch_event_add_header_string(channel->variables, stack, varname, value);
			} else {
				switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_CRIT, "Invalid data (${%s} contains a variable)\n", varname);
			}
		}
		status = SWITCH_STATUS_SUCCESS;
	}
	switch_mutex_unlock(channel->profile_mutex);

	return status;
}


switch_status_t switch_event_base_add_header(switch_event_t *event, switch_stack_t stack, const char *header_name, char *data);

SWITCH_DECLARE(switch_status_t) switch_channel_set_variable_printf(switch_channel_t *channel, const char *varname, const char *fmt, ...)
{
	int ret = 0;
	char *data;
	va_list ap;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_assert(channel != NULL);

	switch_mutex_lock(channel->profile_mutex);
	if (channel->variables && !zstr(varname)) {
		switch_event_del_header(channel->variables, varname);

		va_start(ap, fmt);
		ret = switch_vasprintf(&data, fmt, ap);
		va_end(ap);

		if (ret == -1) {
			switch_mutex_unlock(channel->profile_mutex);
			return SWITCH_STATUS_MEMERR;
		}

		status = switch_channel_set_variable(channel, varname, data);
		free(data);
	}
	switch_mutex_unlock(channel->profile_mutex);

	return status;
}


SWITCH_DECLARE(switch_status_t) switch_channel_set_variable_name_printf(switch_channel_t *channel, const char *val, const char *fmt, ...)
{
	int ret = 0;
	char *varname;
	va_list ap;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_assert(channel != NULL);

	switch_mutex_lock(channel->profile_mutex);

	va_start(ap, fmt);
	ret = switch_vasprintf(&varname, fmt, ap);
	va_end(ap);

	if (ret == -1) {
		switch_mutex_unlock(channel->profile_mutex);
		return SWITCH_STATUS_MEMERR;
	}

	status = switch_channel_set_variable(channel, varname, val);

	free(varname);

	switch_mutex_unlock(channel->profile_mutex);

	return status;
}


SWITCH_DECLARE(switch_status_t) switch_channel_set_variable_partner_var_check(switch_channel_t *channel,
																			  const char *varname, const char *value, switch_bool_t var_check)
{
	const char *uuid;
	switch_assert(channel != NULL);

	if (!zstr(varname)) {
		if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
			switch_core_session_t *session;
			if ((session = switch_core_session_locate(uuid))) {
				switch_channel_t *tchannel = switch_core_session_get_channel(session);
				switch_channel_set_variable_var_check(tchannel, varname, value, var_check);
				switch_core_session_rwunlock(session);
			}
			return SWITCH_STATUS_SUCCESS;
		}
	}

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(uint32_t) switch_channel_test_flag(switch_channel_t *channel, switch_channel_flag_t flag)
{
	uint32_t r = 0;

	switch_assert(channel != NULL);

	switch_mutex_lock(channel->flag_mutex);
	r = channel->flags[flag];
	switch_mutex_unlock(channel->flag_mutex);

	return r;
}

SWITCH_DECLARE(switch_bool_t) switch_channel_set_flag_partner(switch_channel_t *channel, switch_channel_flag_t flag)
{
	const char *uuid;

	switch_assert(channel != NULL);

	if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
		switch_core_session_t *session;
		if ((session = switch_core_session_locate(uuid))) {
			switch_channel_set_flag(switch_core_session_get_channel(session), flag);
			switch_core_session_rwunlock(session);
			return SWITCH_TRUE;
		}
	}

	return SWITCH_FALSE;
}

SWITCH_DECLARE(uint32_t) switch_channel_test_flag_partner(switch_channel_t *channel, switch_channel_flag_t flag)
{
	const char *uuid;
	int r = 0;

	switch_assert(channel != NULL);

	if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
		switch_core_session_t *session;
		if ((session = switch_core_session_locate(uuid))) {
			r = switch_channel_test_flag(switch_core_session_get_channel(session), flag);
			switch_core_session_rwunlock(session);
		}
	}

	return r;
}

SWITCH_DECLARE(switch_bool_t) switch_channel_clear_flag_partner(switch_channel_t *channel, switch_channel_flag_t flag)
{
	const char *uuid;

	switch_assert(channel != NULL);

	if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
		switch_core_session_t *session;
		if ((session = switch_core_session_locate(uuid))) {
			switch_channel_clear_flag(switch_core_session_get_channel(session), flag);
			switch_core_session_rwunlock(session);
			return SWITCH_TRUE;
		}
	}

	return SWITCH_FALSE;
}

SWITCH_DECLARE(void) switch_channel_wait_for_state(switch_channel_t *channel, switch_channel_t *other_channel, switch_channel_state_t want_state)
{

	switch_assert(channel);
	
	for (;;) {
		if ((channel->state == channel->running_state && channel->running_state == want_state) ||
			(other_channel && switch_channel_down(other_channel)) || switch_channel_down(channel)) {
			break;
		}
		switch_yield(20000);
	}
}


SWITCH_DECLARE(void) switch_channel_wait_for_state_timeout(switch_channel_t *channel, switch_channel_state_t want_state, uint32_t timeout)
{

	uint32_t count = 0;

	for (;;) {

		if ((channel->state == channel->running_state && channel->running_state == want_state) || channel->state >= CS_HANGUP) {
			break;
		}

		switch_cond_next();

		if (++count >= timeout) {
			break;
		}
	}
}

SWITCH_DECLARE(switch_status_t) switch_channel_wait_for_flag(switch_channel_t *channel,
															 switch_channel_flag_t want_flag,
															 switch_bool_t pres, uint32_t to, switch_channel_t *super_channel)
{

	if (to) {
		to++;
	}

	for (;;) {
		if (pres) {
			if (switch_channel_test_flag(channel, want_flag)) {
				break;
			}
		} else {
			if (!switch_channel_test_flag(channel, want_flag)) {
				break;
			}
		}

		switch_cond_next();

		if (super_channel && !switch_channel_ready(super_channel)) {
			return SWITCH_STATUS_FALSE;
		}

		if (switch_channel_down(channel)) {
			return SWITCH_STATUS_FALSE;
		}

		if (to && !--to) {
			return SWITCH_STATUS_TIMEOUT;
		}
	}

	return SWITCH_STATUS_SUCCESS;
}


SWITCH_DECLARE(void) switch_channel_set_cap_value(switch_channel_t *channel, switch_channel_cap_t cap, uint32_t value)
{
	switch_assert(channel);
	switch_assert(channel->flag_mutex);

	switch_mutex_lock(channel->flag_mutex);
	channel->caps[cap] = value;
	switch_mutex_unlock(channel->flag_mutex);
}

SWITCH_DECLARE(void) switch_channel_clear_cap(switch_channel_t *channel, switch_channel_cap_t cap)
{
	switch_assert(channel != NULL);
	switch_assert(channel->flag_mutex);

	switch_mutex_lock(channel->flag_mutex);
	channel->caps[cap] = 0;
	switch_mutex_unlock(channel->flag_mutex);
}

SWITCH_DECLARE(uint32_t) switch_channel_test_cap(switch_channel_t *channel, switch_channel_cap_t cap)
{
	switch_assert(channel != NULL);
	return channel->caps[cap] ? 1 : 0;
}

SWITCH_DECLARE(uint32_t) switch_channel_test_cap_partner(switch_channel_t *channel, switch_channel_cap_t cap)
{
	const char *uuid;
	int r = 0;

	switch_assert(channel != NULL);

	if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
		switch_core_session_t *session;
		if ((session = switch_core_session_locate(uuid))) {
			r = switch_channel_test_cap(switch_core_session_get_channel(session), cap);
			switch_core_session_rwunlock(session);
		}
	}

	return r;
}

SWITCH_DECLARE(char *) switch_channel_get_flag_string(switch_channel_t *channel)
{
	switch_stream_handle_t stream = { 0 };
	char *r;
	int i = 0;

	SWITCH_STANDARD_STREAM(stream);

	switch_mutex_lock(channel->flag_mutex);
	for (i = 0; i < CF_FLAG_MAX; i++) {
		if (channel->flags[i]) {
			stream.write_function(&stream, "%d=%d;", i, channel->flags[i]);
		}
	}
	switch_mutex_unlock(channel->flag_mutex);

	r = (char *) stream.data;

	if (end_of(r) == ';') {
		end_of(r) = '\0';
	}

	return r;

}

SWITCH_DECLARE(char *) switch_channel_get_cap_string(switch_channel_t *channel)
{
	switch_stream_handle_t stream = { 0 };
	char *r;
	int i = 0;

	SWITCH_STANDARD_STREAM(stream);

	switch_mutex_lock(channel->flag_mutex);
	for (i = 0; i < CC_FLAG_MAX; i++) {
		if (channel->caps[i]) {
			stream.write_function(&stream, "%d=%d;", i, channel->caps[i]);
		}
	}
	switch_mutex_unlock(channel->flag_mutex);

	r = (char *) stream.data;

	if (end_of(r) == ';') {
		end_of(r) = '\0';
	}

	return r;

}

SWITCH_DECLARE(void) switch_channel_set_flag_value(switch_channel_t *channel, switch_channel_flag_t flag, uint32_t value)
{
	int HELD = 0;
	
	switch_assert(channel);
	switch_assert(channel->flag_mutex);

	switch_mutex_lock(channel->flag_mutex);
	if (flag == CF_LEG_HOLDING && !channel->flags[flag] && channel->flags[CF_ANSWERED]) {
		HELD = 1;
	}
	channel->flags[flag] = value;
	switch_mutex_unlock(channel->flag_mutex);

	if (HELD) {
		switch_channel_set_callstate(channel, CCS_HELD);
		switch_mutex_lock(channel->profile_mutex);
		channel->caller_profile->times->last_hold = switch_time_now();
		switch_mutex_unlock(channel->profile_mutex);
	}

	if (flag == CF_OUTBOUND) {
		switch_channel_set_variable(channel, "is_outbound", "true");
	}

	if (flag == CF_RECOVERED) {
		switch_channel_set_variable(channel, "recovered", "true");
	}

}

SWITCH_DECLARE(void) switch_channel_set_flag_recursive(switch_channel_t *channel, switch_channel_flag_t flag)
{
	switch_assert(channel);
	switch_assert(channel->flag_mutex);

	switch_mutex_lock(channel->flag_mutex);
	channel->flags[flag]++;
	switch_mutex_unlock(channel->flag_mutex);

	if (flag == CF_OUTBOUND) {
		switch_channel_set_variable(channel, "is_outbound", "true");
	}

	if (flag == CF_RECOVERED) {
		switch_channel_set_variable(channel, "recovered", "true");
	}
}


SWITCH_DECLARE(void) switch_channel_set_private_flag(switch_channel_t *channel, uint32_t flags)
{
	switch_assert(channel != NULL);
	switch_mutex_lock(channel->flag_mutex);
	channel->private_flags |= flags;
	switch_mutex_unlock(channel->flag_mutex);
}

SWITCH_DECLARE(void) switch_channel_clear_private_flag(switch_channel_t *channel, uint32_t flags)
{
	switch_assert(channel != NULL);
	switch_mutex_lock(channel->flag_mutex);
	channel->private_flags &= ~flags;
	switch_mutex_unlock(channel->flag_mutex);
}

SWITCH_DECLARE(int) switch_channel_test_private_flag(switch_channel_t *channel, uint32_t flags)
{
	switch_assert(channel != NULL);
	return (channel->private_flags & flags);
}

SWITCH_DECLARE(void) switch_channel_set_app_flag_key(const char *key, switch_channel_t *channel, uint32_t flags)
{
	uint32_t *flagp = NULL;
	switch_byte_t new = 0;

	switch_assert(channel != NULL);
	switch_mutex_lock(channel->flag_mutex);

	if (!channel->app_flag_hash) {
		switch_core_hash_init(&channel->app_flag_hash, switch_core_session_get_pool(channel->session));
		new++;
	}
	
	if (new || !(flagp = switch_core_hash_find(channel->app_flag_hash, key))) {
		flagp = switch_core_session_alloc(channel->session, sizeof(uint32_t));
		switch_core_hash_insert(channel->app_flag_hash, key, flagp);
	}

	switch_assert(flagp);
	*flagp |= flags;

	switch_mutex_unlock(channel->flag_mutex);
}

SWITCH_DECLARE(void) switch_channel_clear_app_flag_key(const char *key, switch_channel_t *channel, uint32_t flags)
{
	uint32_t *flagp = NULL;
	
	switch_assert(channel != NULL);
	switch_mutex_lock(channel->flag_mutex);
	if (channel->app_flag_hash && (flagp = switch_core_hash_find(channel->app_flag_hash, key))) {
		if (!flags) {
			*flagp = 0;
		} else {
			*flagp &= ~flags;
		}
	}
	switch_mutex_unlock(channel->flag_mutex);
}

SWITCH_DECLARE(int) switch_channel_test_app_flag_key(const char *key, switch_channel_t *channel, uint32_t flags)
{
	int r = 0;
	uint32_t *flagp = NULL;
	switch_assert(channel != NULL);

	switch_mutex_lock(channel->flag_mutex);
	if (channel->app_flag_hash && (flagp = switch_core_hash_find(channel->app_flag_hash, key))) {
		r = (*flagp & flags);
	}
	switch_mutex_unlock(channel->flag_mutex);
	

	return r;
}

SWITCH_DECLARE(void) switch_channel_set_state_flag(switch_channel_t *channel, switch_channel_flag_t flag)
{
	switch_assert(channel != NULL);

	switch_mutex_lock(channel->flag_mutex);
	channel->state_flags[0] = 1;
	channel->state_flags[flag] = 1;
	switch_mutex_unlock(channel->flag_mutex);
}

SWITCH_DECLARE(void) switch_channel_clear_flag(switch_channel_t *channel, switch_channel_flag_t flag)
{
	int ACTIVE = 0;

	switch_assert(channel != NULL);
	switch_assert(channel->flag_mutex);

	switch_mutex_lock(channel->flag_mutex);
	if (flag == CF_LEG_HOLDING && channel->flags[flag] && channel->flags[CF_ANSWERED]) {
		ACTIVE = 1;
	}
	channel->flags[flag] = 0;
	switch_mutex_unlock(channel->flag_mutex);

	if (ACTIVE) {
		switch_channel_set_callstate(channel, CCS_ACTIVE);
		switch_mutex_lock(channel->profile_mutex);
		if (channel->caller_profile->times->last_hold) {
			channel->caller_profile->times->hold_accum += (switch_time_now() - channel->caller_profile->times->last_hold);
		}
		switch_mutex_unlock(channel->profile_mutex);
	}

	if (flag == CF_OUTBOUND) {
		switch_channel_set_variable(channel, "is_outbound", NULL);
	}

	if (flag == CF_RECOVERED) {
		switch_channel_set_variable(channel, "recovered", NULL);
	}
}


SWITCH_DECLARE(void) switch_channel_clear_flag_recursive(switch_channel_t *channel, switch_channel_flag_t flag)
{
	switch_assert(channel != NULL);
	switch_assert(channel->flag_mutex);

	switch_mutex_lock(channel->flag_mutex);
	if (channel->flags[flag]) {
		channel->flags[flag]--;
	}
	switch_mutex_unlock(channel->flag_mutex);

	if (flag == CF_OUTBOUND) {
		switch_channel_set_variable(channel, "is_outbound", NULL);
	}
}

SWITCH_DECLARE(switch_channel_state_t) switch_channel_get_state(switch_channel_t *channel)
{
	switch_channel_state_t state;
	switch_assert(channel != NULL);

	state = channel->state;

	return state;
}

SWITCH_DECLARE(switch_channel_state_t) switch_channel_get_running_state(switch_channel_t *channel)
{
	switch_channel_state_t state;
	switch_assert(channel != NULL);

	state = channel->running_state;

	return state;
}

SWITCH_DECLARE(int) switch_channel_state_change_pending(switch_channel_t *channel) 
{
	if (switch_channel_down(channel) || !switch_core_session_in_thread(channel->session)) {
		return 0;
	}

	return channel->running_state != channel->state;
}

SWITCH_DECLARE(int) switch_channel_check_signal(switch_channel_t *channel, switch_bool_t in_thread_only)
{
	if (!in_thread_only || switch_core_session_in_thread(channel->session)) {
		switch_ivr_parse_all_signal_data(channel->session);
	}

	return 0;
}

SWITCH_DECLARE(int) switch_channel_test_ready(switch_channel_t *channel, switch_bool_t check_ready, switch_bool_t check_media)
{
	int ret = 0;

	switch_assert(channel != NULL);

	switch_channel_check_signal(channel, SWITCH_TRUE);

	if (check_media) {
		ret = ((switch_channel_test_flag(channel, CF_ANSWERED) ||
				switch_channel_test_flag(channel, CF_EARLY_MEDIA)) && !switch_channel_test_flag(channel, CF_PROXY_MODE) &&
			   switch_core_session_get_read_codec(channel->session) && switch_core_session_get_write_codec(channel->session));
			   

		if (!ret)
			return ret;
	}

	if (!check_ready)
		return ret;

	ret = 0;

	if (!channel->hangup_cause && channel->state > CS_ROUTING && channel->state < CS_HANGUP && channel->state != CS_RESET &&
		!switch_channel_test_flag(channel, CF_TRANSFER) && !switch_channel_test_flag(channel, CF_NOT_READY) && 
		!switch_channel_state_change_pending(channel)) {
		ret++;
	}



	return ret;
}

static const char *state_names[] = {
	"CS_NEW",
	"CS_INIT",
	"CS_ROUTING",
	"CS_SOFT_EXECUTE",
	"CS_EXECUTE",
	"CS_EXCHANGE_MEDIA",
	"CS_PARK",
	"CS_CONSUME_MEDIA",
	"CS_HIBERNATE",
	"CS_RESET",
	"CS_HANGUP",
	"CS_REPORTING",
	"CS_DESTROY",
	"CS_NONE",
	NULL
};

SWITCH_DECLARE(const char *) switch_channel_state_name(switch_channel_state_t state)
{
	return state_names[state];
}


SWITCH_DECLARE(switch_channel_state_t) switch_channel_name_state(const char *name)
{
	uint32_t x = 0;
	for (x = 0; state_names[x]; x++) {
		if (!strcasecmp(state_names[x], name)) {
			return (switch_channel_state_t) x;
		}
	}

	return CS_DESTROY;
}

SWITCH_DECLARE(switch_channel_state_t) switch_channel_perform_set_running_state(switch_channel_t *channel, switch_channel_state_t state,
																				const char *file, const char *func, int line)
{
	int x;

	switch_mutex_lock(channel->flag_mutex);
	if (channel->state_flags[0]) {
		for (x = 1; x < CF_FLAG_MAX; x++) {
			if (channel->state_flags[x]) {
				channel->flags[x] = 1;
				channel->state_flags[x] = 0;
			}
		}
		channel->state_flags[0] = 0;
	}
	switch_mutex_unlock(channel->flag_mutex);

	switch_channel_clear_flag(channel, CF_TAGGED);
	


	switch_mutex_lock(channel->state_mutex);

	switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_DEBUG, "(%s) Running State Change %s\n",
					  channel->name, state_names[state]);

	channel->running_state = state;

	if (channel->state == CS_ROUTING || channel->state == CS_HANGUP) {
		switch_channel_presence(channel, "unknown", (const char *) state_names[state], NULL);
	}

	if (state <= CS_DESTROY) {
		switch_event_t *event;

		if (state < CS_HANGUP) {
			if (state == CS_ROUTING) {
				switch_channel_set_callstate(channel, CCS_RINGING);
			} else if (switch_channel_test_flag(channel, CF_ANSWERED)) {
				switch_channel_set_callstate(channel, CCS_ACTIVE);
			} else if (switch_channel_test_flag(channel, CF_EARLY_MEDIA)) {
				switch_channel_set_callstate(channel, CCS_EARLY);
			}
		}

		if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_STATE) == SWITCH_STATUS_SUCCESS) {
			if (state == CS_ROUTING) {
				switch_channel_event_set_data(channel, event);
			} else {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-State", switch_channel_state_name(state));
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Call-State", switch_channel_callstate2str(channel->callstate));
				switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Channel-State-Number", "%d", state);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Name", channel->name);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(channel->session));
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Call-Direction",
											   channel->direction == SWITCH_CALL_DIRECTION_OUTBOUND ? "outbound" : "inbound");
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Presence-Call-Direction",
											   channel->direction == SWITCH_CALL_DIRECTION_OUTBOUND ? "outbound" : "inbound");

				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-HIT-Dialplan", 
											   switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_INBOUND ||
											   switch_channel_test_flag(channel, CF_DIALPLAN) ? "true" : "false");
				
				if (switch_channel_down_nosig(channel)) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Answer-State", "hangup");
				} else if (switch_channel_test_flag(channel, CF_ANSWERED)) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Answer-State", "answered");
				} else if (switch_channel_test_flag(channel, CF_EARLY_MEDIA)) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Answer-State", "early");
				} else {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Answer-State", "ringing");
				}
			}
			switch_event_fire(&event);
		}
	}

	switch_mutex_unlock(channel->state_mutex);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_channel_state_t) switch_channel_perform_set_state(switch_channel_t *channel,
																		const char *file, const char *func, int line, switch_channel_state_t state)
{
	switch_channel_state_t last_state;
	int ok = 0;

	switch_assert(channel != NULL);
	switch_assert(state <= CS_DESTROY);
	switch_mutex_lock(channel->state_mutex);

	last_state = channel->state;
	switch_assert(last_state <= CS_DESTROY);

	if (last_state == state) {
		goto done;
	}

	if (last_state >= CS_HANGUP && state < last_state) {
		goto done;
	}

	/* STUB for more dev
	   case CS_INIT:
	   switch(state) {

	   case CS_NEW:
	   case CS_INIT:
	   case CS_EXCHANGE_MEDIA:
	   case CS_SOFT_EXECUTE:
	   case CS_ROUTING:
	   case CS_EXECUTE:
	   case CS_HANGUP:
	   case CS_DESTROY:

	   default:
	   break;
	   }
	   break;
	 */

	switch (last_state) {
	case CS_NEW:
	case CS_RESET:
		switch (state) {
		default:
			ok++;
			break;
		}
		break;

	case CS_INIT:
		switch (state) {
		case CS_EXCHANGE_MEDIA:
		case CS_SOFT_EXECUTE:
		case CS_ROUTING:
		case CS_EXECUTE:
		case CS_PARK:
		case CS_CONSUME_MEDIA:
		case CS_HIBERNATE:
		case CS_RESET:
			ok++;
		default:
			break;
		}
		break;

	case CS_EXCHANGE_MEDIA:
		switch (state) {
		case CS_SOFT_EXECUTE:
		case CS_ROUTING:
		case CS_EXECUTE:
		case CS_PARK:
		case CS_CONSUME_MEDIA:
		case CS_HIBERNATE:
		case CS_RESET:
			ok++;
		default:
			break;
		}
		break;

	case CS_SOFT_EXECUTE:
		switch (state) {
		case CS_EXCHANGE_MEDIA:
		case CS_ROUTING:
		case CS_EXECUTE:
		case CS_PARK:
		case CS_CONSUME_MEDIA:
		case CS_HIBERNATE:
		case CS_RESET:
			ok++;
		default:
			break;
		}
		break;

	case CS_PARK:
		switch (state) {
		case CS_EXCHANGE_MEDIA:
		case CS_ROUTING:
		case CS_EXECUTE:
		case CS_SOFT_EXECUTE:
		case CS_HIBERNATE:
		case CS_RESET:
		case CS_CONSUME_MEDIA:
			ok++;
		default:
			break;
		}
		break;

	case CS_CONSUME_MEDIA:
		switch (state) {
		case CS_EXCHANGE_MEDIA:
		case CS_ROUTING:
		case CS_EXECUTE:
		case CS_SOFT_EXECUTE:
		case CS_HIBERNATE:
		case CS_RESET:
		case CS_PARK:
			ok++;
		default:
			break;
		}
		break;
	case CS_HIBERNATE:
		switch (state) {
		case CS_EXCHANGE_MEDIA:
		case CS_INIT:
		case CS_ROUTING:
		case CS_EXECUTE:
		case CS_SOFT_EXECUTE:
		case CS_PARK:
		case CS_CONSUME_MEDIA:
		case CS_RESET:
			ok++;
		default:
			break;
		}
		break;

	case CS_ROUTING:
		switch (state) {
		case CS_EXCHANGE_MEDIA:
		case CS_EXECUTE:
		case CS_SOFT_EXECUTE:
		case CS_PARK:
		case CS_CONSUME_MEDIA:
		case CS_HIBERNATE:
		case CS_RESET:
			ok++;
		default:
			break;
		}
		break;

	case CS_EXECUTE:
		switch (state) {
		case CS_EXCHANGE_MEDIA:
		case CS_SOFT_EXECUTE:
		case CS_ROUTING:
		case CS_PARK:
		case CS_CONSUME_MEDIA:
		case CS_HIBERNATE:
		case CS_RESET:
			ok++;
		default:
			break;
		}
		break;

	case CS_HANGUP:
		switch (state) {
		case CS_REPORTING:
		case CS_DESTROY:
			ok++;
		default:
			break;
		}
		break;

	case CS_REPORTING:
		switch (state) {
		case CS_DESTROY:
			ok++;
		default:
			break;
		}
		break;

	default:
		break;

	}

	if (ok) {
		switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_DEBUG, "(%s) State Change %s -> %s\n",
						  channel->name, state_names[last_state], state_names[state]);

		channel->state = state;

		if (state == CS_HANGUP && !channel->hangup_cause) {
			channel->hangup_cause = SWITCH_CAUSE_NORMAL_CLEARING;
		}

		if (state <= CS_DESTROY) {
			switch_core_session_signal_state_change(channel->session);
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_WARNING,
						  "(%s) Invalid State Change %s -> %s\n", channel->name, state_names[last_state], state_names[state]);
		/* we won't tolerate an invalid state change so we can make sure we are as robust as a nice cup of dark coffee! */
		/* not cool lets crash this bad boy and figure out wtf is going on */
		switch_assert(channel->state >= CS_HANGUP);
	}
  done:

	switch_mutex_unlock(channel->state_mutex);
	return channel->state;
}

SWITCH_DECLARE(void) switch_channel_event_set_basic_data(switch_channel_t *channel, switch_event_t *event)
{
	switch_caller_profile_t *caller_profile, *originator_caller_profile = NULL, *originatee_caller_profile = NULL;
	switch_codec_implementation_t impl = { 0 };
	char state_num[25];
	const char *v;

	switch_mutex_lock(channel->profile_mutex);

	if ((caller_profile = channel->caller_profile)) {
		originator_caller_profile = caller_profile->originator_caller_profile;
		originatee_caller_profile = caller_profile->originatee_caller_profile;
	}

	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-State", switch_channel_state_name(channel->state));
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Call-State", switch_channel_callstate2str(channel->callstate));
	switch_snprintf(state_num, sizeof(state_num), "%d", channel->state);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-State-Number", state_num);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Name", switch_channel_get_name(channel));
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(channel->session));

	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Call-Direction",
								   channel->direction == SWITCH_CALL_DIRECTION_OUTBOUND ? "outbound" : "inbound");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Presence-Call-Direction",
								   channel->direction == SWITCH_CALL_DIRECTION_OUTBOUND ? "outbound" : "inbound");

	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-HIT-Dialplan", 
								   switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_INBOUND ||
								   switch_channel_test_flag(channel, CF_DIALPLAN) ? "true" : "false");


	if ((v = switch_channel_get_variable(channel, "presence_id"))) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Presence-ID", v);
	}

	if ((v = switch_channel_get_variable(channel, "presence_data"))) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Presence-Data", v);
	}


	if ((v = switch_channel_get_variable(channel, "presence_data_cols"))) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Presence-Data-Cols", v);
	}

	if ((v = switch_channel_get_variable(channel, "call_uuid"))) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Call-UUID", v);
	} else {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Call-UUID", switch_core_session_get_uuid(channel->session));
	}

	if (switch_channel_down_nosig(channel)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Answer-State", "hangup");
	} else if (switch_channel_test_flag(channel, CF_ANSWERED)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Answer-State", "answered");
	} else if (switch_channel_test_flag(channel, CF_EARLY_MEDIA)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Answer-State", "early");
	} else {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Answer-State", "ringing");
	}

	switch_core_session_get_read_impl(channel->session, &impl);

	if (impl.iananame) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Read-Codec-Name", impl.iananame);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Channel-Read-Codec-Rate", "%u", impl.actual_samples_per_second);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Channel-Read-Codec-Bit-Rate", "%d", impl.bits_per_second);
	}

	switch_core_session_get_write_impl(channel->session, &impl);

	if (impl.iananame) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Write-Codec-Name", impl.iananame);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Channel-Write-Codec-Rate", "%u", impl.actual_samples_per_second);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Channel-Write-Codec-Bit-Rate", "%d", impl.bits_per_second);
	}

	/* Index Caller's Profile */
	if (caller_profile) {
		switch_caller_profile_event_set_data(caller_profile, "Caller", event);
	}

	/* Index Originator/ee's Profile */
	if (originator_caller_profile && channel->last_profile_type == LP_ORIGINATOR) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Other-Type", "originator");
		switch_caller_profile_event_set_data(originator_caller_profile, "Other-Leg", event);
	} else if (originatee_caller_profile && channel->last_profile_type == LP_ORIGINATEE) {	/* Index Originatee's Profile */
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Other-Type", "originatee");
		switch_caller_profile_event_set_data(originatee_caller_profile, "Other-Leg", event);
	}


	switch_mutex_unlock(channel->profile_mutex);
}

SWITCH_DECLARE(void) switch_channel_event_set_extended_data(switch_channel_t *channel, switch_event_t *event)
{
	switch_event_header_t *hi;
	int global_verbose_events = -1;

	switch_mutex_lock(channel->profile_mutex);

	switch_core_session_ctl(SCSC_VERBOSE_EVENTS, &global_verbose_events);

	if (global_verbose_events || 
		switch_channel_test_flag(channel, CF_VERBOSE_EVENTS) ||
		switch_event_get_header(event, "presence-data-cols") ||
		event->event_id == SWITCH_EVENT_CHANNEL_CREATE ||
		event->event_id == SWITCH_EVENT_CHANNEL_ORIGINATE ||
		event->event_id == SWITCH_EVENT_CHANNEL_UUID ||
		event->event_id == SWITCH_EVENT_CHANNEL_ANSWER ||
		event->event_id == SWITCH_EVENT_CHANNEL_PARK ||
		event->event_id == SWITCH_EVENT_CHANNEL_UNPARK ||
		event->event_id == SWITCH_EVENT_CHANNEL_BRIDGE ||
		event->event_id == SWITCH_EVENT_CHANNEL_UNBRIDGE ||
		event->event_id == SWITCH_EVENT_CHANNEL_PROGRESS ||
		event->event_id == SWITCH_EVENT_CHANNEL_PROGRESS_MEDIA ||
		event->event_id == SWITCH_EVENT_CHANNEL_HANGUP ||
		event->event_id == SWITCH_EVENT_CHANNEL_HANGUP_COMPLETE ||
		event->event_id == SWITCH_EVENT_REQUEST_PARAMS ||
		event->event_id == SWITCH_EVENT_CHANNEL_DATA ||
		event->event_id == SWITCH_EVENT_CHANNEL_EXECUTE_COMPLETE ||
		event->event_id == SWITCH_EVENT_CHANNEL_DESTROY ||
		event->event_id == SWITCH_EVENT_SESSION_HEARTBEAT ||
		event->event_id == SWITCH_EVENT_API ||
		event->event_id == SWITCH_EVENT_RECORD_START ||
		event->event_id == SWITCH_EVENT_RECORD_STOP || 
		event->event_id == SWITCH_EVENT_PLAYBACK_START ||
		event->event_id == SWITCH_EVENT_PLAYBACK_STOP ||
		event->event_id == SWITCH_EVENT_CALL_UPDATE || 
		event->event_id == SWITCH_EVENT_MEDIA_BUG_START || 
		event->event_id == SWITCH_EVENT_MEDIA_BUG_STOP || 
		event->event_id == SWITCH_EVENT_CUSTOM) {

		/* Index Variables */

		if (channel->scope_variables) {
			switch_event_t *ep;

			for (ep = channel->scope_variables; ep; ep = ep->next) {
				for (hi = ep->headers; hi; hi = hi->next) {
					char buf[1024];
					char *vvar = NULL, *vval = NULL;
					
					vvar = (char *) hi->name;
					vval = (char *) hi->value;
						
					switch_assert(vvar && vval);
					switch_snprintf(buf, sizeof(buf), "scope_variable_%s", vvar);
					
					if (!switch_event_get_header(event, buf)) {
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, buf, vval);
					}
				}
			}
		}

		if (channel->variables) {
			for (hi = channel->variables->headers; hi; hi = hi->next) {
				char buf[1024];
				char *vvar = NULL, *vval = NULL;

				vvar = (char *) hi->name;
				vval = (char *) hi->value;
				
				switch_assert(vvar && vval);
				switch_snprintf(buf, sizeof(buf), "variable_%s", vvar);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, buf, vval);
			}
		}
	}

	switch_mutex_unlock(channel->profile_mutex);
}


SWITCH_DECLARE(void) switch_channel_event_set_data(switch_channel_t *channel, switch_event_t *event)
{
	switch_mutex_lock(channel->profile_mutex);
	switch_channel_event_set_basic_data(channel, event);
	switch_channel_event_set_extended_data(channel, event);
	switch_mutex_unlock(channel->profile_mutex);
}



SWITCH_DECLARE(void) switch_channel_set_caller_profile(switch_channel_t *channel, switch_caller_profile_t *caller_profile)
{
	char *uuid = NULL;
	switch_assert(channel != NULL);
	switch_assert(channel->session != NULL);
	switch_mutex_lock(channel->profile_mutex);
	switch_assert(caller_profile != NULL);

	caller_profile->direction = channel->direction;
	uuid = switch_core_session_get_uuid(channel->session);

	if (!caller_profile->uuid || strcasecmp(caller_profile->uuid, uuid)) {
		caller_profile->uuid = switch_core_session_strdup(channel->session, uuid);
	}

	if (!caller_profile->chan_name || strcasecmp(caller_profile->chan_name, channel->name)) {
		caller_profile->chan_name = switch_core_session_strdup(channel->session, channel->name);
	}

	if (!caller_profile->context) {
		caller_profile->context = switch_core_session_strdup(channel->session, "default");
	}

	if (!caller_profile->times) {
		caller_profile->times = (switch_channel_timetable_t *) switch_core_session_alloc(channel->session, sizeof(*caller_profile->times));
		caller_profile->times->profile_created = switch_micro_time_now();
	}

	if (channel->caller_profile && channel->caller_profile->times) {
		channel->caller_profile->times->transferred = caller_profile->times->profile_created;
		caller_profile->times->answered = channel->caller_profile->times->answered;
		caller_profile->times->progress = channel->caller_profile->times->progress;
		caller_profile->times->progress_media = channel->caller_profile->times->progress_media;
		caller_profile->times->created = channel->caller_profile->times->created;
		caller_profile->times->hungup = channel->caller_profile->times->hungup;
		if (channel->caller_profile->caller_extension) {
			switch_caller_extension_clone(&caller_profile->caller_extension, channel->caller_profile->caller_extension, caller_profile->pool);
		}
	} else {
		caller_profile->times->created = switch_micro_time_now();
	}


	caller_profile->next = channel->caller_profile;
	channel->caller_profile = caller_profile;
	caller_profile->profile_index = switch_core_sprintf(caller_profile->pool, "%d", ++channel->profile_index);

	switch_mutex_unlock(channel->profile_mutex);
}

SWITCH_DECLARE(switch_caller_profile_t *) switch_channel_get_caller_profile(switch_channel_t *channel)
{
	switch_caller_profile_t *profile;
	switch_assert(channel != NULL);
	switch_mutex_lock(channel->profile_mutex);
	if ((profile = channel->caller_profile) && profile->hunt_caller_profile) {
		profile = profile->hunt_caller_profile;
	}
	switch_mutex_unlock(channel->profile_mutex);
	return profile;
}

SWITCH_DECLARE(void) switch_channel_set_originator_caller_profile(switch_channel_t *channel, switch_caller_profile_t *caller_profile)
{
	switch_assert(channel != NULL);
	switch_assert(channel->caller_profile != NULL);
	switch_mutex_lock(channel->profile_mutex);

	if (!caller_profile->times) {
		caller_profile->times = (switch_channel_timetable_t *) switch_core_alloc(caller_profile->pool, sizeof(*caller_profile->times));
	}

	if (channel->caller_profile) {
		caller_profile->next = channel->caller_profile->originator_caller_profile;
		channel->caller_profile->originator_caller_profile = caller_profile;
		channel->last_profile_type = LP_ORIGINATOR;
	}
	switch_assert(channel->caller_profile->originator_caller_profile->next != channel->caller_profile->originator_caller_profile);
	switch_mutex_unlock(channel->profile_mutex);
}

SWITCH_DECLARE(void) switch_channel_set_hunt_caller_profile(switch_channel_t *channel, switch_caller_profile_t *caller_profile)
{
	switch_assert(channel != NULL);
	switch_assert(channel->caller_profile != NULL);

	switch_mutex_lock(channel->profile_mutex);

	channel->caller_profile->hunt_caller_profile = NULL;
	if (channel->caller_profile && caller_profile) {
		caller_profile->direction = channel->direction;
		channel->caller_profile->hunt_caller_profile = caller_profile;
	}
	switch_mutex_unlock(channel->profile_mutex);
}

SWITCH_DECLARE(void) switch_channel_set_origination_caller_profile(switch_channel_t *channel, switch_caller_profile_t *caller_profile)
{
	switch_assert(channel != NULL);
	switch_assert(channel->caller_profile != NULL);

	switch_mutex_lock(channel->profile_mutex);

	if (channel->caller_profile) {
		caller_profile->next = channel->caller_profile->origination_caller_profile;
		channel->caller_profile->origination_caller_profile = caller_profile;
	}
	switch_assert(channel->caller_profile->origination_caller_profile->next != channel->caller_profile->origination_caller_profile);
	switch_mutex_unlock(channel->profile_mutex);
}

SWITCH_DECLARE(switch_caller_profile_t *) switch_channel_get_origination_caller_profile(switch_channel_t *channel)
{
	switch_caller_profile_t *profile = NULL;
	switch_assert(channel != NULL);

	switch_mutex_lock(channel->profile_mutex);
	if (channel->caller_profile) {
		profile = channel->caller_profile->origination_caller_profile;
	}
	switch_mutex_unlock(channel->profile_mutex);

	return profile;
}


SWITCH_DECLARE(void) switch_channel_set_originatee_caller_profile(switch_channel_t *channel, switch_caller_profile_t *caller_profile)
{
	switch_assert(channel != NULL);
	switch_assert(channel->caller_profile != NULL);

	switch_mutex_lock(channel->profile_mutex);

	if (channel->caller_profile) {
		caller_profile->next = channel->caller_profile->originatee_caller_profile;
		channel->caller_profile->originatee_caller_profile = caller_profile;
		channel->last_profile_type = LP_ORIGINATEE;
	}
	switch_assert(channel->caller_profile->originatee_caller_profile->next != channel->caller_profile->originatee_caller_profile);
	switch_mutex_unlock(channel->profile_mutex);
}

SWITCH_DECLARE(switch_caller_profile_t *) switch_channel_get_originator_caller_profile(switch_channel_t *channel)
{
	switch_caller_profile_t *profile = NULL;
	switch_assert(channel != NULL);

	switch_mutex_lock(channel->profile_mutex);
	
	if (channel->caller_profile) {
		profile = channel->caller_profile->originator_caller_profile;
	}
	switch_mutex_unlock(channel->profile_mutex);

	return profile;
}

SWITCH_DECLARE(switch_caller_profile_t *) switch_channel_get_originatee_caller_profile(switch_channel_t *channel)
{
	switch_caller_profile_t *profile = NULL;
	switch_assert(channel != NULL);

	switch_mutex_lock(channel->profile_mutex);
	if (channel->caller_profile) {
		profile = channel->caller_profile->originatee_caller_profile;
	}
	switch_mutex_unlock(channel->profile_mutex);

	return profile;
}

SWITCH_DECLARE(char *) switch_channel_get_uuid(switch_channel_t *channel)
{
	switch_assert(channel != NULL);
	switch_assert(channel->session != NULL);
	return switch_core_session_get_uuid(channel->session);
}

SWITCH_DECLARE(int) switch_channel_add_state_handler(switch_channel_t *channel, const switch_state_handler_table_t *state_handler)
{
	int x, index;

	switch_assert(channel != NULL);
	switch_mutex_lock(channel->state_mutex);
	for (x = 0; x < SWITCH_MAX_STATE_HANDLERS; x++) {
		if (channel->state_handlers[x] == state_handler) {
			index = x;
			goto end;
		}
	}
	index = channel->state_handler_index++;

	if (channel->state_handler_index >= SWITCH_MAX_STATE_HANDLERS) {
		index = -1;
		goto end;
	}

	channel->state_handlers[index] = state_handler;

  end:
	switch_mutex_unlock(channel->state_mutex);
	return index;
}

SWITCH_DECLARE(const switch_state_handler_table_t *) switch_channel_get_state_handler(switch_channel_t *channel, int index)
{
	const switch_state_handler_table_t *h = NULL;

	switch_assert(channel != NULL);

	if (index >= SWITCH_MAX_STATE_HANDLERS || index > channel->state_handler_index) {
		return NULL;
	}

	switch_mutex_lock(channel->state_mutex);
	h = channel->state_handlers[index];
	switch_mutex_unlock(channel->state_mutex);

	return h;
}

SWITCH_DECLARE(void) switch_channel_clear_state_handler(switch_channel_t *channel, const switch_state_handler_table_t *state_handler)
{
	int index, i = channel->state_handler_index;
	const switch_state_handler_table_t *new_handlers[SWITCH_MAX_STATE_HANDLERS] = { 0 };

	switch_assert(channel != NULL);

	switch_mutex_lock(channel->state_mutex);
	channel->state_handler_index = 0;

	if (state_handler) {
		for (index = 0; index < i; index++) {
			if (channel->state_handlers[index] != state_handler) {
				new_handlers[channel->state_handler_index++] = channel->state_handlers[index];
			}
		}
	} else {
		for (index = 0; index < i; index++) {
			if (channel->state_handlers[index] && switch_test_flag(channel->state_handlers[index], SSH_FLAG_STICKY)) {
				new_handlers[channel->state_handler_index++] = channel->state_handlers[index];
			}
		}
	}

	for (index = 0; index < SWITCH_MAX_STATE_HANDLERS; index++) {
		channel->state_handlers[index] = NULL;
	}

	if (channel->state_handler_index > 0) {
		for (index = 0; index < channel->state_handler_index; index++) {
			channel->state_handlers[index] = new_handlers[index];
		}
	}

	switch_mutex_unlock(channel->state_mutex);
}

SWITCH_DECLARE(void) switch_channel_restart(switch_channel_t *channel)
{
	switch_channel_set_state(channel, CS_RESET);
	switch_channel_wait_for_state_timeout(channel, CS_RESET, 5000);
	switch_channel_set_state(channel, CS_EXECUTE);
}

/* XXX This is a somewhat simple operation.  Were essentially taking the extension that one channel 
   was executing and generating a new extension for another channel that starts out where the 
   original one left off with an optional forward offset.  Since all we are really doing is 
   copying a few basic pool-allocated structures from one channel to another there really is 
   not much to worry about here in terms of threading since we use read-write locks. 
   While the features are nice, they only really are needed in one specific crazy attended 
   transfer scenario where one channel was in the middle of calling a particular extension
   when it was rudely cut off by a transfer key press. XXX */

SWITCH_DECLARE(switch_status_t) switch_channel_caller_extension_masquerade(switch_channel_t *orig_channel, switch_channel_t *new_channel, uint32_t offset)
{
	switch_caller_profile_t *caller_profile;
	switch_caller_extension_t *extension = NULL, *orig_extension = NULL;
	switch_caller_application_t *ap;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_event_header_t *hi = NULL;
	const char *no_copy = switch_channel_get_variable(orig_channel, "attended_transfer_no_copy");
	char *dup;
	int i, argc = 0;
	char *argv[128];

	if (no_copy) {
		dup = switch_core_session_strdup(new_channel->session, no_copy);
		argc = switch_separate_string(dup, ',', argv, (sizeof(argv) / sizeof(argv[0])));
	}


	switch_mutex_lock(orig_channel->profile_mutex);
	switch_mutex_lock(new_channel->profile_mutex);


	caller_profile = switch_caller_profile_clone(new_channel->session, new_channel->caller_profile);
	switch_assert(caller_profile);
	extension = switch_caller_extension_new(new_channel->session, caller_profile->destination_number, caller_profile->destination_number);
	orig_extension = switch_channel_get_caller_extension(orig_channel);


	if (extension && orig_extension) {
		for (ap = orig_extension->current_application; ap && offset > 0; offset--) {
			ap = ap->next;
		}

		for (; ap; ap = ap->next) {
			switch_caller_extension_add_application(new_channel->session, extension, ap->application_name, ap->application_data);
		}

		caller_profile->destination_number = switch_core_strdup(caller_profile->pool, orig_channel->caller_profile->destination_number);
		switch_channel_set_caller_profile(new_channel, caller_profile);
		switch_channel_set_caller_extension(new_channel, extension);

		for (hi = orig_channel->variables->headers; hi; hi = hi->next) {
			int ok = 1;
			for (i = 0; i < argc; i++) {
				if (!strcasecmp(argv[i], hi->name)) {
					ok = 0;
					break;
				}
			}

			if (!ok)
				continue;

			switch_channel_set_variable(new_channel, hi->name, hi->value);
		}

		status = SWITCH_STATUS_SUCCESS;
	}


	switch_mutex_unlock(new_channel->profile_mutex);
	switch_mutex_unlock(orig_channel->profile_mutex);


	return status;
}

SWITCH_DECLARE(void) switch_channel_flip_cid(switch_channel_t *channel)
{
	switch_event_t *event;

	switch_mutex_lock(channel->profile_mutex);
	if (channel->caller_profile->callee_id_name) {
		switch_channel_set_variable(channel, "pre_transfer_caller_id_name", channel->caller_profile->caller_id_name);
		channel->caller_profile->caller_id_name = switch_core_strdup(channel->caller_profile->pool, channel->caller_profile->callee_id_name);
	}
	channel->caller_profile->callee_id_name = SWITCH_BLANK_STRING;
	
	if (channel->caller_profile->callee_id_number) {
		switch_channel_set_variable(channel, "pre_transfer_caller_id_number", channel->caller_profile->caller_id_number);
		channel->caller_profile->caller_id_number = switch_core_strdup(channel->caller_profile->pool, channel->caller_profile->callee_id_number);
	}
	channel->caller_profile->callee_id_number = SWITCH_BLANK_STRING;
	switch_mutex_unlock(channel->profile_mutex);


	if (switch_event_create(&event, SWITCH_EVENT_CALL_UPDATE) == SWITCH_STATUS_SUCCESS) {
		const char *uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Direction", "RECV");

		if (uuid) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Bridged-To", uuid);
		}
		switch_channel_event_set_data(channel, event);
		switch_event_fire(&event);
	}


	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(channel->session), SWITCH_LOG_INFO, "%s Flipping CID from \"%s\" <%s> to \"%s\" <%s>\n", 
					  switch_channel_get_name(channel),
					  switch_str_nil(switch_channel_get_variable(channel, "pre_transfer_caller_id_name")),
					  switch_str_nil(switch_channel_get_variable(channel, "pre_transfer_caller_id_number")),
					  channel->caller_profile->caller_id_name,
					  channel->caller_profile->caller_id_number
					  );

}

SWITCH_DECLARE(void) switch_channel_sort_cid(switch_channel_t *channel, switch_bool_t in)
{

	if (in) {
		if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND && !switch_channel_test_flag(channel, CF_DIALPLAN)) {
			switch_channel_set_flag(channel, CF_DIALPLAN);
			switch_channel_flip_cid(channel);
		}

		return;
	}

	if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND && switch_channel_test_flag(channel, CF_DIALPLAN)) {
		switch_channel_clear_flag(channel, CF_DIALPLAN);
		switch_mutex_lock(channel->profile_mutex);
		channel->caller_profile->callee_id_name = SWITCH_BLANK_STRING;
		channel->caller_profile->callee_id_number = SWITCH_BLANK_STRING;
		switch_mutex_unlock(channel->profile_mutex);
	}
	
}

SWITCH_DECLARE(switch_caller_extension_t *) switch_channel_get_queued_extension(switch_channel_t *channel)
{
	switch_caller_extension_t *caller_extension;

	switch_mutex_lock(channel->profile_mutex);
	caller_extension = channel->queued_extension;
	channel->queued_extension = NULL;
	switch_mutex_unlock(channel->profile_mutex);

	return caller_extension;
}

SWITCH_DECLARE(void) switch_channel_transfer_to_extension(switch_channel_t *channel, switch_caller_extension_t *caller_extension)
{
	switch_mutex_lock(channel->profile_mutex);
	channel->queued_extension = caller_extension;
	switch_mutex_unlock(channel->profile_mutex);

	switch_channel_set_flag(channel, CF_TRANSFER);
	switch_channel_set_state(channel, CS_ROUTING);	
}

SWITCH_DECLARE(void) switch_channel_set_caller_extension(switch_channel_t *channel, switch_caller_extension_t *caller_extension)
{
	switch_assert(channel != NULL);

	switch_channel_sort_cid(channel, SWITCH_TRUE);
	
	switch_mutex_lock(channel->profile_mutex);
	caller_extension->next = channel->caller_profile->caller_extension;
	channel->caller_profile->caller_extension = caller_extension;
	switch_mutex_unlock(channel->profile_mutex);
}


SWITCH_DECLARE(switch_caller_extension_t *) switch_channel_get_caller_extension(switch_channel_t *channel)
{
	switch_caller_extension_t *extension = NULL;

	switch_assert(channel != NULL);
	switch_mutex_lock(channel->profile_mutex);
	if (channel->caller_profile) {
		extension = channel->caller_profile->caller_extension;
	}
	switch_mutex_unlock(channel->profile_mutex);
	return extension;
}


SWITCH_DECLARE(void) switch_channel_set_bridge_time(switch_channel_t *channel)
{
	switch_mutex_lock(channel->profile_mutex);
	if (channel->caller_profile && channel->caller_profile->times) {
		channel->caller_profile->times->bridged = switch_micro_time_now();
	}
	switch_mutex_unlock(channel->profile_mutex);
}


SWITCH_DECLARE(void) switch_channel_set_hangup_time(switch_channel_t *channel)
{
	if (channel->caller_profile && channel->caller_profile->times && !channel->caller_profile->times->hungup) {
		switch_mutex_lock(channel->profile_mutex);
		channel->caller_profile->times->hungup = switch_micro_time_now();
		switch_mutex_unlock(channel->profile_mutex);
	}
}


SWITCH_DECLARE(switch_channel_state_t) switch_channel_perform_hangup(switch_channel_t *channel,
																	 const char *file, const char *func, int line, switch_call_cause_t hangup_cause)
{
	int ok = 0;

	switch_assert(channel != NULL);

	/* one per customer */
	switch_mutex_lock(channel->state_mutex);
	if (!(channel->opaque_flags & OCF_HANGUP)) {
		channel->opaque_flags |= OCF_HANGUP;
		ok = 1;
	}
	switch_mutex_unlock(channel->state_mutex);

	if (!ok) {
		return channel->state;
	}

	switch_channel_clear_flag(channel, CF_BLOCK_STATE);

	if (channel->state < CS_HANGUP) {
		switch_channel_state_t last_state;
		switch_event_t *event;

		switch_mutex_lock(channel->state_mutex);
		last_state = channel->state;
		channel->state = CS_HANGUP;
		switch_mutex_unlock(channel->state_mutex);


		if (hangup_cause == SWITCH_CAUSE_LOSE_RACE) {
			switch_channel_presence(channel, "unknown", "cancelled", NULL);
			switch_channel_set_variable(channel, "presence_call_info", NULL);
		}

		switch_channel_set_callstate(channel, CCS_HANGUP);
		channel->hangup_cause = hangup_cause;
		switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_NOTICE, "Hangup %s [%s] [%s]\n",
						  channel->name, state_names[last_state], switch_channel_cause2str(channel->hangup_cause));


		if (!switch_core_session_running(channel->session) && !switch_core_session_started(channel->session)) {
			switch_core_session_thread_launch(channel->session);
		}

		if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_HANGUP) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Hangup-Cause", switch_channel_cause2str(hangup_cause));
			switch_channel_event_set_data(channel, event);
			switch_event_fire(&event);
		}

		switch_core_session_kill_channel(channel->session, SWITCH_SIG_KILL);
		switch_core_session_signal_state_change(channel->session);
		switch_core_session_hangup_state(channel->session, SWITCH_FALSE);
	}

	return channel->state;
}

SWITCH_DECLARE(switch_status_t) switch_channel_perform_mark_ring_ready_value(switch_channel_t *channel, 
																			 switch_ring_ready_t rv,
																			 const char *file, const char *func, int line)
{
	switch_event_t *event;

	if (!switch_channel_test_flag(channel, CF_RING_READY) && 
		!switch_channel_test_flag(channel, CF_EARLY_MEDIA) && !switch_channel_test_flag(channel, CF_ANSWERED)) {
		switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_NOTICE, "Ring-Ready %s!\n", channel->name);
		switch_channel_set_flag_value(channel, CF_RING_READY, rv);
		if (channel->caller_profile && channel->caller_profile->times) {
			switch_mutex_lock(channel->profile_mutex);
			channel->caller_profile->times->progress = switch_micro_time_now();
			if (channel->caller_profile->originator_caller_profile) {
				switch_core_session_t *other_session;
				if ((other_session = switch_core_session_locate(channel->caller_profile->originator_caller_profile->uuid))) {
					switch_channel_t *other_channel;
					other_channel = switch_core_session_get_channel(other_session);
					if (other_channel->caller_profile) {
						other_channel->caller_profile->times->progress = channel->caller_profile->times->progress;
					}
					switch_core_session_rwunlock(other_session);
				}
				channel->caller_profile->originator_caller_profile->times->progress = channel->caller_profile->times->progress;
			}
			switch_mutex_unlock(channel->profile_mutex);
		}

		if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_PROGRESS) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, event);
			switch_event_fire(&event);
		}

		switch_channel_execute_on(channel, SWITCH_CHANNEL_EXECUTE_ON_RING_VARIABLE);
		switch_channel_api_on(channel, SWITCH_CHANNEL_API_ON_RING_VARIABLE);

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_channel_perform_mark_pre_answered(switch_channel_t *channel, const char *file, const char *func, int line)
{
	switch_event_t *event;
	const char *var = NULL;

	if (!switch_channel_test_flag(channel, CF_EARLY_MEDIA) && !switch_channel_test_flag(channel, CF_ANSWERED)) {
		const char *uuid;
		switch_core_session_t *other_session;

		switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_NOTICE, "Pre-Answer %s!\n", channel->name);
		switch_channel_set_flag(channel, CF_EARLY_MEDIA);
		switch_channel_set_callstate(channel, CCS_EARLY);
		switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "EARLY MEDIA");

		if (channel->caller_profile && channel->caller_profile->times) {
			switch_mutex_lock(channel->profile_mutex);
			channel->caller_profile->times->progress_media = switch_micro_time_now();
			if (channel->caller_profile->originator_caller_profile) {
				switch_core_session_t *osession;
				if ((osession = switch_core_session_locate(channel->caller_profile->originator_caller_profile->uuid))) {
					switch_channel_t *other_channel;
					other_channel = switch_core_session_get_channel(osession);
					if (other_channel->caller_profile) {
						other_channel->caller_profile->times->progress_media = channel->caller_profile->times->progress_media;
					}
					switch_core_session_rwunlock(osession);
				}
				channel->caller_profile->originator_caller_profile->times->progress_media = channel->caller_profile->times->progress_media;
			}
			switch_mutex_unlock(channel->profile_mutex);
		}

		if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_PROGRESS_MEDIA) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, event);
			switch_event_fire(&event);
		}

		switch_channel_execute_on(channel, SWITCH_CHANNEL_EXECUTE_ON_PRE_ANSWER_VARIABLE);
		switch_channel_execute_on(channel, SWITCH_CHANNEL_EXECUTE_ON_MEDIA_VARIABLE);

		switch_channel_api_on(channel, SWITCH_CHANNEL_API_ON_PRE_ANSWER_VARIABLE);
		switch_channel_api_on(channel, SWITCH_CHANNEL_API_ON_MEDIA_VARIABLE);

		if ((var = switch_channel_get_variable(channel, SWITCH_PASSTHRU_PTIME_MISMATCH_VARIABLE))) {
			switch_channel_set_flag(channel, CF_PASSTHRU_PTIME_MISMATCH);
		}


		/* if we're the child of another channel and the other channel is in a blocking read they will never realize we have answered so send 
		   a SWITCH_SIG_BREAK to interrupt any blocking reads on that channel
		 */
		if ((uuid = switch_channel_get_variable(channel, SWITCH_ORIGINATOR_VARIABLE))
			&& (other_session = switch_core_session_locate(uuid))) {
			switch_core_session_kill_channel(other_session, SWITCH_SIG_BREAK);
			switch_core_session_rwunlock(other_session);
		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_channel_perform_pre_answer(switch_channel_t *channel, const char *file, const char *func, int line)
{
	switch_core_session_message_t msg = { 0 };
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_assert(channel != NULL);

	if (channel->hangup_cause || channel->state >= CS_HANGUP) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_test_flag(channel, CF_ANSWERED)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (switch_channel_test_flag(channel, CF_EARLY_MEDIA)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_INBOUND) {
		msg.message_id = SWITCH_MESSAGE_INDICATE_PROGRESS;
		msg.from = channel->name;
		status = switch_core_session_perform_receive_message(channel->session, &msg, file, func, line);
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		switch_channel_perform_mark_pre_answered(channel, file, func, line);
	} else {
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
	}

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_channel_perform_ring_ready_value(switch_channel_t *channel, switch_ring_ready_t rv, 
																		const char *file, const char *func, int line)
{
	switch_core_session_message_t msg = { 0 };
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_assert(channel != NULL);

	if (channel->hangup_cause || channel->state >= CS_HANGUP) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_test_flag(channel, CF_ANSWERED)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (switch_channel_test_flag(channel, CF_EARLY_MEDIA)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_INBOUND) {
		msg.message_id = SWITCH_MESSAGE_INDICATE_RINGING;
		msg.from = channel->name;
		msg.numeric_arg = rv;
		status = switch_core_session_perform_receive_message(channel->session, &msg, file, func, line);
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_NOTICE, "Ring Ready %s!\n", channel->name);
		switch_channel_perform_mark_ring_ready_value(channel, rv, file, func, line);
	} else {
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
	}

	return status;
}

static void do_api_on(switch_channel_t *channel, const char *variable)
{
	char *app;
	char *arg = NULL;	
	switch_stream_handle_t stream = { 0 };

	app = switch_core_session_strdup(channel->session, variable);
	
	if ((arg = strchr(app, ' '))) {
		*arg++ = '\0';
	}
	
	SWITCH_STANDARD_STREAM(stream);
	switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG, "%s process %s: %s(%s)\n%s\n",
					  channel->name, variable, app, switch_str_nil(arg), (char *) stream.data);
	switch_api_execute(app, arg, NULL, &stream);
	free(stream.data);
}


SWITCH_DECLARE(switch_status_t) switch_channel_api_on(switch_channel_t *channel, const char *variable_prefix)
{
	switch_event_header_t *hp;
	switch_event_t *event;
	int x = 0;

								
	switch_channel_get_variables(channel, &event);
	
	for (hp = event->headers; hp; hp = hp->next) {
		char *var = hp->name;
		char *val = hp->value;

		if (!strncasecmp(var, variable_prefix, strlen(variable_prefix))) {
			if (hp->idx) {
				int i;
				for (i = 0; i < hp->idx; i++) {
					x++;
					do_api_on(channel, hp->array[i]);					
				}
			} else {
				x++;
				do_api_on(channel, val);
			}
		}
	}
	
	switch_event_destroy(&event);

	return x ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

static void do_execute_on(switch_channel_t *channel, const char *variable)
{
	char *arg = NULL;
	char *p;
	int bg = 0;
	char *app;

	app = switch_core_session_strdup(channel->session, variable);
	
	for(p = app; p && *p; p++) {
		if (*p == ' ') {
			*p++ = '\0';
			arg = p;
			break;
		} else if (*p == ':' && (*(p+1) == ':')) {
			bg++;
			break;
		}
	}
	
	
	if (bg) {
		switch_core_session_execute_application_async(channel->session, app, arg);
	} else {
		switch_core_session_execute_application(channel->session, app, arg);
	}
}

SWITCH_DECLARE(switch_status_t) switch_channel_execute_on(switch_channel_t *channel, const char *variable_prefix)
{
	switch_event_header_t *hp;
	switch_event_t *event;
	int x = 0;

	switch_channel_get_variables(channel, &event);
	
	for (hp = event->headers; hp; hp = hp->next) {
		char *var = hp->name;
		char *val = hp->value;

		if (!strncasecmp(var, variable_prefix, strlen(variable_prefix))) {
			if (hp->idx) {
				int i;
				for (i = 0; i < hp->idx; i++) {
					x++;
					do_execute_on(channel, hp->array[i]);					
				}
			} else {
				x++;
				do_execute_on(channel, val);
			}
		}
	}
	
	switch_event_destroy(&event);

	return x ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_channel_perform_mark_answered(switch_channel_t *channel, const char *file, const char *func, int line)
{
	switch_event_t *event;
	const char *uuid;
	switch_core_session_t *other_session;
	const char *var;

	switch_assert(channel != NULL);

	if (channel->hangup_cause || channel->state >= CS_HANGUP) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_test_flag(channel, CF_ANSWERED)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (channel->caller_profile && channel->caller_profile->times) {
		switch_mutex_lock(channel->profile_mutex);
		channel->caller_profile->times->answered = switch_micro_time_now();
		switch_mutex_unlock(channel->profile_mutex);
	}

	switch_channel_set_flag(channel, CF_ANSWERED);
	switch_channel_set_callstate(channel, CCS_ACTIVE);

	if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_ANSWER) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_fire(&event);
	}

	/* if we're the child of another channel and the other channel is in a blocking read they will never realize we have answered so send 
	   a SWITCH_SIG_BREAK to interrupt any blocking reads on that channel
	 */
	if ((uuid = switch_channel_get_variable(channel, SWITCH_ORIGINATOR_VARIABLE))
		&& (other_session = switch_core_session_locate(uuid))) {
		switch_core_session_kill_channel(other_session, SWITCH_SIG_BREAK);
		switch_core_session_rwunlock(other_session);
	}

	if ((var = switch_channel_get_variable(channel, SWITCH_PASSTHRU_PTIME_MISMATCH_VARIABLE))) {
		switch_channel_set_flag(channel, CF_PASSTHRU_PTIME_MISMATCH);
	}

	if ((var = switch_channel_get_variable(channel, SWITCH_ENABLE_HEARTBEAT_EVENTS_VARIABLE))) {
		uint32_t seconds = 60;
		int tmp;

		if (switch_is_number(var)) {
			tmp = atoi(var);
			if (tmp > 0) {
				seconds = tmp;
			}
		} else if (!switch_true(var)) {
			seconds = 0;
		}

		if (seconds) {
			switch_core_session_enable_heartbeat(channel->session, seconds);
		}
	}

	switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ANSWER");
	switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_NOTICE, "Channel [%s] has been answered\n",
					  channel->name);


	switch_channel_execute_on(channel, SWITCH_CHANNEL_EXECUTE_ON_ANSWER_VARIABLE);

	if (!switch_channel_test_flag(channel, CF_EARLY_MEDIA)) {
		switch_channel_execute_on(channel, SWITCH_CHANNEL_EXECUTE_ON_MEDIA_VARIABLE);
		switch_channel_api_on(channel, SWITCH_CHANNEL_API_ON_MEDIA_VARIABLE);
	}

	switch_channel_api_on(channel, SWITCH_CHANNEL_API_ON_ANSWER_VARIABLE);

	switch_channel_presence(channel, "unknown", "answered", NULL);

	switch_channel_audio_sync(channel);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_channel_perform_answer(switch_channel_t *channel, const char *file, const char *func, int line)
{
	switch_core_session_message_t msg = { 0 };
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_assert(channel != NULL);

	if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (channel->hangup_cause || channel->state >= CS_HANGUP) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_test_flag(channel, CF_ANSWERED)) {
		return SWITCH_STATUS_SUCCESS;
	}
	
	msg.message_id = SWITCH_MESSAGE_INDICATE_ANSWER;
	msg.from = channel->name;
	status = switch_core_session_perform_receive_message(channel->session, &msg, file, func, line);


	if (status == SWITCH_STATUS_SUCCESS) {
		switch_channel_perform_mark_answered(channel, file, func, line);
	} else {
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
	}

	return status;
}

#define resize(l) {\
	char *dp;\
	olen += (len + l + block);\
	cpos = c - data;\
	if ((dp = realloc(data, olen))) {\
	data = dp;\
	c = data + cpos;\
	memset(c, 0, olen - cpos);\
	}}                           \

SWITCH_DECLARE(char *) switch_channel_expand_variables(switch_channel_t *channel, const char *in)
{
	char *p, *c = NULL;
	char *data, *indup, *endof_indup;
	size_t sp = 0, len = 0, olen = 0, vtype = 0, br = 0, cpos, block = 128;
	char *cloned_sub_val = NULL, *sub_val = NULL;
	char *func_val = NULL, *sb = NULL;
	int nv = 0;

	if (zstr(in)) {
		return (char *) in;
	}

	nv = switch_string_var_check_const(in) || switch_string_has_escaped_data(in);

	if (!nv) {
		return (char *) in;
	}


	nv = 0;
	olen = strlen(in) + 1;
	indup = strdup(in);
	endof_indup = end_of_p(indup) + 1;

	if ((data = malloc(olen))) {
		memset(data, 0, olen);
		c = data;
		for (p = indup; p && p < endof_indup && *p; p++) {
			vtype = 0;

			if (*p == '\\') {
				if (*(p + 1) == '$') {
					nv = 1;
					p++;
				} else if (*(p + 1) == '\'') {
					p++;
					continue;
				} else if (*(p + 1) == '\\') {
					*c++ = *p++;
					len++;
					continue;
				}
			}

			if (*p == '$' && !nv) {
				if (*(p + 1)) {
					if (*(p + 1) == '{') {
						vtype = 1;
					} else {
						nv = 1;
					}
				} else {
					nv = 1;
				}
			}

			if (nv) {
				*c++ = *p;
				len++;
				nv = 0;
				continue;
			}

			if (vtype) {
				char *s = p, *e, *vname, *vval = NULL;
				size_t nlen;

				s++;

				if (vtype == 1 && *s == '{') {
					br = 1;
					s++;
				}

				e = s;
				vname = s;
				while (*e) {
					if (br == 1 && *e == '}') {
						br = 0;
						*e++ = '\0';
						break;
					}

					if (br > 0) {
						if (e != s && *e == '{') {
							br++;
						} else if (br > 1 && *e == '}') {
							br--;
						}
					}

					e++;
				}
				p = e > endof_indup ? endof_indup : e;

				vval = NULL;
				for(sb = vname; sb && *sb; sb++) {
					if (*sb == ' ') {
						vval = sb;
						break;
					} else if (*sb == '(') {
						vval = sb;
						br = 1;
						break;
					}
				}

				if (vval) {
					e = vval - 1;
					*vval++ = '\0';
					while (*e == ' ') {
						*e-- = '\0';
					}
					e = vval;

					while (e && *e) {
						if (*e == '(') {
							br++;
						} else if (br > 1 && *e == ')') {
							br--;
						} else if (br == 1 && *e == ')') {
							*e = '\0';
							break;
						}
						e++;
					}

					vtype = 2;
				}

				if (vtype == 1) {
					char *expanded = NULL;
					int offset = 0;
					int ooffset = 0;
					char *ptr;
					int idx = -1;

					if ((expanded = switch_channel_expand_variables(channel, (char *) vname)) == vname) {
						expanded = NULL;
					} else {
						vname = expanded;
					}

					if ((ptr = strchr(vname, ':'))) {
						*ptr++ = '\0';
						offset = atoi(ptr);
						if ((ptr = strchr(ptr, ':'))) {
							ptr++;
							ooffset = atoi(ptr);
						}
					}

					if ((ptr = strchr(vname, '[')) && strchr(ptr, ']')) {
						*ptr++ = '\0';
						idx = atoi(ptr);
					}
					
					if ((sub_val = (char *) switch_channel_get_variable_dup(channel, vname, SWITCH_TRUE, idx))) {
						if (offset || ooffset) {
							cloned_sub_val = strdup(sub_val);
							switch_assert(cloned_sub_val);
							sub_val = cloned_sub_val;
						}

						if (offset >= 0) {
							if ((size_t) offset > strlen(sub_val)) {
								*sub_val = '\0';
							} else {
								sub_val += offset;
							}
						} else if ((size_t) abs(offset) <= strlen(sub_val)) {
							sub_val = cloned_sub_val + (strlen(cloned_sub_val) + offset);
						}

						if (ooffset > 0 && (size_t) ooffset < strlen(sub_val)) {
							if ((ptr = (char *) sub_val + ooffset)) {
								*ptr = '\0';
							}
						}
					}

					switch_safe_free(expanded);
				} else {
					switch_stream_handle_t stream = { 0 };
					char *expanded = NULL;

					SWITCH_STANDARD_STREAM(stream);

					if (stream.data) {
						char *expanded_vname = NULL;

						if ((expanded_vname = switch_channel_expand_variables(channel, (char *) vname)) == vname) {
							expanded_vname = NULL;
						} else {
							vname = expanded_vname;
						}

						if ((expanded = switch_channel_expand_variables(channel, vval)) == vval) {
							expanded = NULL;
						} else {
							vval = expanded;
						}

						if (switch_api_execute(vname, vval, channel->session, &stream) == SWITCH_STATUS_SUCCESS) {
							func_val = stream.data;
							sub_val = func_val;
						} else {
							free(stream.data);
						}

						switch_safe_free(expanded);
						switch_safe_free(expanded_vname);

					} else {
						switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_CRIT, "Memory Error!\n");
						free(data);
						free(indup);
						return (char *) in;
					}
				}
				if ((nlen = sub_val ? strlen(sub_val) : 0)) {
					if (len + nlen >= olen) {
						resize(nlen);
					}

					len += nlen;
					strcat(c, sub_val);
					c += nlen;
				}

				switch_safe_free(func_val);
				switch_safe_free(cloned_sub_val);
				sub_val = NULL;
				vname = NULL;
				vtype = 0;
				br = 0;
			}
			if (len + 1 >= olen) {
				resize(1);
			}

			if (sp) {
				*c++ = ' ';
				sp = 0;
				len++;
			}

			if (*p == '$') {
				p--;
			} else {
				*c++ = *p;
				len++;
			}
		}
	}
	free(indup);

	return data;
}

SWITCH_DECLARE(char *) switch_channel_build_param_string(switch_channel_t *channel, switch_caller_profile_t *caller_profile, const char *prefix)
{
	switch_stream_handle_t stream = { 0 };
	switch_size_t encode_len = 1024, new_len = 0;
	char *encode_buf = NULL;
	const char *prof[12] = { 0 }, *prof_names[12] = {
	0};
	char *e = NULL;
	switch_event_header_t *hi;
	uint32_t x = 0;

	SWITCH_STANDARD_STREAM(stream);

	if (prefix) {
		stream.write_function(&stream, "%s&", prefix);
	}

	encode_buf = malloc(encode_len);
	switch_assert(encode_buf);

	if (!caller_profile) {
		caller_profile = switch_channel_get_caller_profile(channel);
	}

	switch_assert(caller_profile != NULL);

	prof[0] = caller_profile->context;
	prof[1] = caller_profile->destination_number;
	prof[2] = caller_profile->caller_id_name;
	prof[3] = caller_profile->caller_id_number;
	prof[4] = caller_profile->network_addr;
	prof[5] = caller_profile->ani;
	prof[6] = caller_profile->aniii;
	prof[7] = caller_profile->rdnis;
	prof[8] = caller_profile->source;
	prof[9] = caller_profile->chan_name;
	prof[10] = caller_profile->uuid;

	prof_names[0] = "context";
	prof_names[1] = "destination_number";
	prof_names[2] = "caller_id_name";
	prof_names[3] = "caller_id_number";
	prof_names[4] = "network_addr";
	prof_names[5] = "ani";
	prof_names[6] = "aniii";
	prof_names[7] = "rdnis";
	prof_names[8] = "source";
	prof_names[9] = "chan_name";
	prof_names[10] = "uuid";

	for (x = 0; prof[x]; x++) {
		if (zstr(prof[x])) {
			continue;
		}
		new_len = (strlen(prof[x]) * 3) + 1;
		if (encode_len < new_len) {
			char *tmp;
			
			encode_len = new_len;

			if (!(tmp = realloc(encode_buf, encode_len))) {
				abort();
			}

			encode_buf = tmp;
		}
		switch_url_encode(prof[x], encode_buf, encode_len);
		stream.write_function(&stream, "%s=%s&", prof_names[x], encode_buf);
	}

	if ((hi = switch_channel_variable_first(channel))) {
		for (; hi; hi = hi->next) {
			char *var = hi->name;
			char *val = hi->value;

			new_len = (strlen((char *) var) * 3) + 1;
			if (encode_len < new_len) {
				char *tmp;

				encode_len = new_len;

				tmp = realloc(encode_buf, encode_len);
				switch_assert(tmp);
				encode_buf = tmp;
			}

			switch_url_encode((char *) val, encode_buf, encode_len);
			stream.write_function(&stream, "%s=%s&", (char *) var, encode_buf);

		}
		switch_channel_variable_last(channel);
	}

	e = (char *) stream.data + (strlen((char *) stream.data) - 1);

	if (e && *e == '&') {
		*e = '\0';
	}

	switch_safe_free(encode_buf);

	return stream.data;
}

SWITCH_DECLARE(switch_status_t) switch_channel_pass_callee_id(switch_channel_t *channel, switch_channel_t *other_channel)
{
	int x = 0;

	switch_assert(channel);
	switch_assert(other_channel);

	switch_mutex_lock(channel->profile_mutex);
	switch_mutex_lock(other_channel->profile_mutex);

	if (!zstr(channel->caller_profile->callee_id_name)) {
		other_channel->caller_profile->callee_id_name = switch_core_strdup(other_channel->caller_profile->pool, channel->caller_profile->callee_id_name);
		x++;
	}

	if (!zstr(channel->caller_profile->callee_id_number)) {
		other_channel->caller_profile->callee_id_number = switch_core_strdup(other_channel->caller_profile->pool, channel->caller_profile->callee_id_number);
		x++;
	}

	switch_mutex_unlock(other_channel->profile_mutex);
	switch_mutex_unlock(channel->profile_mutex);

	return x ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_channel_get_variables(switch_channel_t *channel, switch_event_t **event)
{
	switch_status_t status;
	switch_mutex_lock(channel->profile_mutex);
	status = switch_event_dup(event, channel->variables);
	switch_mutex_unlock(channel->profile_mutex);
	return status;
}

SWITCH_DECLARE(switch_core_session_t *) switch_channel_get_session(switch_channel_t *channel)
{
	switch_assert(channel);
	return channel->session;
}

SWITCH_DECLARE(switch_status_t) switch_channel_set_timestamps(switch_channel_t *channel)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	const char *cid_buf = NULL;
	switch_caller_profile_t *caller_profile, *ocp;
	switch_app_log_t *app_log, *ap;
	char *last_app = NULL, *last_arg = NULL;
	char start[80] = "", resurrect[80] = "", answer[80] = "", hold[80], 
		bridge[80] = "", progress[80] = "", progress_media[80] = "", end[80] = "", tmp[80] = "",
		profile_start[80] =	"";
	int32_t duration = 0, legbillsec = 0, billsec = 0, mduration = 0, billmsec = 0, legbillmsec = 0, progressmsec = 0, progress_mediamsec = 0;
	int32_t answersec = 0, answermsec = 0, waitsec = 0, waitmsec = 0;
	switch_time_t answerusec = 0;
	switch_time_t uduration = 0, legbillusec = 0, billusec = 0, progresssec = 0, progressusec = 0, progress_mediasec = 0, progress_mediausec = 0, waitusec = 0;
	time_t tt_created = 0, tt_answered = 0, tt_resurrected = 0, tt_bridged, tt_last_hold, tt_hold_accum,
		tt_progress = 0, tt_progress_media = 0, tt_hungup = 0, mtt_created = 0, mtt_answered = 0, mtt_bridged = 0,
		mtt_hungup = 0, tt_prof_created, mtt_prof_created, mtt_progress = 0, mtt_progress_media = 0;
	void *pop;
	char dtstr[SWITCH_DTMF_LOG_LEN + 1] = "";
	int x = 0;

	switch_mutex_lock(channel->profile_mutex);

	if (switch_channel_test_flag(channel, CF_TIMESTAMP_SET)) {
		switch_mutex_unlock(channel->profile_mutex);
		return SWITCH_STATUS_FALSE;
	}

	if (!(caller_profile = channel->caller_profile) || !channel->variables) {
		switch_mutex_unlock(channel->profile_mutex);
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_set_flag(channel, CF_TIMESTAMP_SET);

	if ((app_log = switch_core_session_get_app_log(channel->session))) {
		for (ap = app_log; ap && ap->next; ap = ap->next);
		last_app = ap->app;
		last_arg = ap->arg;
	}

	if (!(ocp = switch_channel_get_originatee_caller_profile(channel))) {
		ocp = switch_channel_get_originator_caller_profile(channel);
	}

	if (!zstr(caller_profile->caller_id_name)) {
		cid_buf = switch_core_session_sprintf(channel->session, "\"%s\" <%s>", caller_profile->caller_id_name,
											  switch_str_nil(caller_profile->caller_id_number));
	} else {
		cid_buf = caller_profile->caller_id_number;
	}

	while (x < SWITCH_DTMF_LOG_LEN && switch_queue_trypop(channel->dtmf_log_queue, &pop) == SWITCH_STATUS_SUCCESS) {
		switch_dtmf_t *dt = (switch_dtmf_t *) pop;

		if (dt) {
			dtstr[x++] = dt->digit;
			free(dt);
			dt = NULL;
		}
	}

	if (x) {
		switch_channel_set_variable(channel, "digits_dialed", dtstr);
	} else {
		switch_channel_set_variable(channel, "digits_dialed", "none");
	}

	if (caller_profile->times) {
		switch_time_exp_t tm;
		switch_size_t retsize;
		const char *fmt = "%Y-%m-%d %T";

		switch_time_exp_lt(&tm, caller_profile->times->created);
		switch_strftime_nocheck(start, &retsize, sizeof(start), fmt, &tm);
		switch_channel_set_variable(channel, "start_stamp", start);

		switch_time_exp_lt(&tm, caller_profile->times->profile_created);
		switch_strftime_nocheck(profile_start, &retsize, sizeof(profile_start), fmt, &tm);
		switch_channel_set_variable(channel, "profile_start_stamp", profile_start);

		if (caller_profile->times->answered) {
			switch_time_exp_lt(&tm, caller_profile->times->answered);
			switch_strftime_nocheck(answer, &retsize, sizeof(answer), fmt, &tm);
			switch_channel_set_variable(channel, "answer_stamp", answer);
		}

		if (caller_profile->times->bridged) {
			switch_time_exp_lt(&tm, caller_profile->times->bridged);
			switch_strftime_nocheck(bridge, &retsize, sizeof(bridge), fmt, &tm);
			switch_channel_set_variable(channel, "bridge_stamp", bridge);
		}

		if (caller_profile->times->last_hold) {
			switch_time_exp_lt(&tm, caller_profile->times->last_hold);
			switch_strftime_nocheck(hold, &retsize, sizeof(hold), fmt, &tm);
			switch_channel_set_variable(channel, "hold_stamp", hold);
		}

		if (caller_profile->times->resurrected) {
			switch_time_exp_lt(&tm, caller_profile->times->resurrected);
			switch_strftime_nocheck(resurrect, &retsize, sizeof(resurrect), fmt, &tm);
			switch_channel_set_variable(channel, "resurrect_stamp", resurrect);
		}

		if (caller_profile->times->progress) {
			switch_time_exp_lt(&tm, caller_profile->times->progress);
			switch_strftime_nocheck(progress, &retsize, sizeof(progress), fmt, &tm);
			switch_channel_set_variable(channel, "progress_stamp", progress);
		}

		if (caller_profile->times->progress_media) {
			switch_time_exp_lt(&tm, caller_profile->times->progress_media);
			switch_strftime_nocheck(progress_media, &retsize, sizeof(progress_media), fmt, &tm);
			switch_channel_set_variable(channel, "progress_media_stamp", progress_media);
		}

		switch_time_exp_lt(&tm, caller_profile->times->hungup);
		switch_strftime_nocheck(end, &retsize, sizeof(end), fmt, &tm);
		switch_channel_set_variable(channel, "end_stamp", end);

		tt_created = (time_t) (caller_profile->times->created / 1000000);
		mtt_created = (time_t) (caller_profile->times->created / 1000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_created);
		switch_channel_set_variable(channel, "start_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->created);
		switch_channel_set_variable(channel, "start_uepoch", tmp);

		tt_prof_created = (time_t) (caller_profile->times->profile_created / 1000000);
		mtt_prof_created = (time_t) (caller_profile->times->profile_created / 1000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_prof_created);
		switch_channel_set_variable(channel, "profile_start_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->profile_created);
		switch_channel_set_variable(channel, "profile_start_uepoch", tmp);

		tt_answered = (time_t) (caller_profile->times->answered / 1000000);
		mtt_answered = (time_t) (caller_profile->times->answered / 1000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_answered);
		switch_channel_set_variable(channel, "answer_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->answered);
		switch_channel_set_variable(channel, "answer_uepoch", tmp);

		tt_bridged = (time_t) (caller_profile->times->bridged / 1000000);
		mtt_bridged = (time_t) (caller_profile->times->bridged / 1000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_bridged);
		switch_channel_set_variable(channel, "bridge_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->bridged);
		switch_channel_set_variable(channel, "bridge_uepoch", tmp);

		tt_last_hold = (time_t) (caller_profile->times->last_hold / 1000000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_last_hold);
		switch_channel_set_variable(channel, "last_hold_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->last_hold);
		switch_channel_set_variable(channel, "last_hold_uepoch", tmp);

		tt_hold_accum = (time_t) (caller_profile->times->hold_accum / 1000000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_hold_accum);
		switch_channel_set_variable(channel, "hold_accum_seconds", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->hold_accum);
		switch_channel_set_variable(channel, "hold_accum_usec", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->hold_accum / 1000);
		switch_channel_set_variable(channel, "hold_accum_ms", tmp);

		tt_resurrected = (time_t) (caller_profile->times->resurrected / 1000000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_resurrected);
		switch_channel_set_variable(channel, "resurrect_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->resurrected);
		switch_channel_set_variable(channel, "resurrect_uepoch", tmp);

		tt_progress = (time_t) (caller_profile->times->progress / 1000000);
		mtt_progress = (time_t) (caller_profile->times->progress / 1000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_progress);
		switch_channel_set_variable(channel, "progress_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->progress);
		switch_channel_set_variable(channel, "progress_uepoch", tmp);

		tt_progress_media = (time_t) (caller_profile->times->progress_media / 1000000);
		mtt_progress_media = (time_t) (caller_profile->times->progress_media / 1000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_progress_media);
		switch_channel_set_variable(channel, "progress_media_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->progress_media);
		switch_channel_set_variable(channel, "progress_media_uepoch", tmp);

		tt_hungup = (time_t) (caller_profile->times->hungup / 1000000);
		mtt_hungup = (time_t) (caller_profile->times->hungup / 1000);
		switch_snprintf(tmp, sizeof(tmp), "%" TIME_T_FMT, tt_hungup);
		switch_channel_set_variable(channel, "end_epoch", tmp);
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, caller_profile->times->hungup);
		switch_channel_set_variable(channel, "end_uepoch", tmp);

		duration = (int32_t) (tt_hungup - tt_created);
		mduration = (int32_t) (mtt_hungup - mtt_created);
		uduration = caller_profile->times->hungup - caller_profile->times->created;

		if (caller_profile->times->bridged > caller_profile->times->created) {
			waitsec = (int32_t) (tt_bridged - tt_created);
			waitmsec = (int32_t) (mtt_bridged - mtt_created);
			waitusec = caller_profile->times->bridged - caller_profile->times->created;
		} else {
			waitsec = 0;
			waitmsec = 0;
			waitusec = 0;
		}

		if (caller_profile->times->answered) {
			billsec = (int32_t) (tt_hungup - tt_answered);
			billmsec = (int32_t) (mtt_hungup - mtt_answered);
			billusec = caller_profile->times->hungup - caller_profile->times->answered;

			legbillsec = (int32_t) (tt_hungup - tt_prof_created);
			legbillmsec = (int32_t) (mtt_hungup - mtt_prof_created);
			legbillusec = caller_profile->times->hungup - caller_profile->times->profile_created;

			answersec = (int32_t) (tt_answered - tt_prof_created);
			answermsec = (int32_t) (mtt_answered - mtt_prof_created);
			answerusec = caller_profile->times->answered - caller_profile->times->profile_created;
		}

		if (caller_profile->times->progress) {
			progresssec = (int32_t) (tt_progress - tt_created);
			progressmsec = (int32_t) (mtt_progress - mtt_created);
			progressusec = caller_profile->times->progress - caller_profile->times->created;
		}

		if (caller_profile->times->progress_media) {
			progress_mediasec = (int32_t) (tt_progress_media - tt_created);
			progress_mediamsec = (int32_t) (mtt_progress_media - mtt_created);
			progress_mediausec = caller_profile->times->progress_media - caller_profile->times->created;
		}

	}

	switch_channel_set_variable(channel, "last_app", last_app);
	switch_channel_set_variable(channel, "last_arg", last_arg);
	switch_channel_set_variable(channel, "caller_id", cid_buf);

	switch_snprintf(tmp, sizeof(tmp), "%d", duration);
	switch_channel_set_variable(channel, "duration", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", billsec);
	switch_channel_set_variable(channel, "billsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%"SWITCH_TIME_T_FMT, progresssec);
	switch_channel_set_variable(channel, "progresssec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", answersec);
	switch_channel_set_variable(channel, "answersec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", waitsec);
	switch_channel_set_variable(channel, "waitsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%"SWITCH_TIME_T_FMT, progress_mediasec);
	switch_channel_set_variable(channel, "progress_mediasec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", legbillsec);
	switch_channel_set_variable(channel, "flow_billsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", mduration);
	switch_channel_set_variable(channel, "mduration", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", billmsec);
	switch_channel_set_variable(channel, "billmsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", progressmsec);
	switch_channel_set_variable(channel, "progressmsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", answermsec);
	switch_channel_set_variable(channel, "answermsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", waitmsec);
	switch_channel_set_variable(channel, "waitmsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", progress_mediamsec);
	switch_channel_set_variable(channel, "progress_mediamsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%d", legbillmsec);
	switch_channel_set_variable(channel, "flow_billmsec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, uduration);
	switch_channel_set_variable(channel, "uduration", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, billusec);
	switch_channel_set_variable(channel, "billusec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, progressusec);
	switch_channel_set_variable(channel, "progressusec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, answerusec);
	switch_channel_set_variable(channel, "answerusec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, waitusec);
	switch_channel_set_variable(channel, "waitusec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, progress_mediausec);
	switch_channel_set_variable(channel, "progress_mediausec", tmp);

	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, legbillusec);
	switch_channel_set_variable(channel, "flow_billusec", tmp);

	switch_mutex_unlock(channel->profile_mutex);

	return status;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
