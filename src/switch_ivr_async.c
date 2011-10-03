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
 * Bret McDanel <bret AT 0xdecafbad dot com>
 * Luke Dashjr <luke@openmethods.com> (OpenMethods, LLC)
 *
 * switch_ivr_async.c -- IVR Library (async operations)
 *
 */

#include <switch.h>
#include <speex/speex_preprocess.h>
#include <speex/speex_echo.h>

struct switch_ivr_dmachine_binding {
	char *digits;
	int32_t key;
	switch_ivr_dmachine_callback_t callback;
	switch_byte_t is_regex;
	void *user_data;
	struct switch_ivr_dmachine_binding *next;
};
typedef struct switch_ivr_dmachine_binding switch_ivr_dmachine_binding_t;

typedef struct {
	switch_ivr_dmachine_binding_t *binding_list;
	switch_ivr_dmachine_binding_t *tail;
} dm_binding_head_t;

struct switch_ivr_dmachine {
	switch_memory_pool_t *pool;
	switch_byte_t my_pool;
	char *name;
	uint32_t digit_timeout_ms;
	uint32_t input_timeout_ms;
	switch_hash_t *binding_hash;
	switch_ivr_dmachine_match_t match;
	switch_digit_action_target_t target;
	char digits[DMACHINE_MAX_DIGIT_LEN];
	char last_matching_digits[DMACHINE_MAX_DIGIT_LEN];
	char last_failed_digits[DMACHINE_MAX_DIGIT_LEN];
	uint32_t cur_digit_len;
	uint32_t max_digit_len;
	switch_time_t last_digit_time;
	switch_byte_t is_match;
	switch_ivr_dmachine_callback_t match_callback;
	switch_ivr_dmachine_callback_t nonmatch_callback;
	dm_binding_head_t *realm;
	switch_ivr_dmachine_binding_t *last_matching_binding;
	void *user_data;
	switch_mutex_t *mutex;
};


SWITCH_DECLARE(switch_digit_action_target_t) switch_ivr_dmachine_get_target(switch_ivr_dmachine_t *dmachine)
{
	return dmachine->target;
}

SWITCH_DECLARE(void) switch_ivr_dmachine_set_target(switch_ivr_dmachine_t *dmachine, switch_digit_action_target_t target)
{
	dmachine->target = target;
}


SWITCH_DECLARE(void) switch_ivr_dmachine_set_match_callback(switch_ivr_dmachine_t *dmachine, switch_ivr_dmachine_callback_t match_callback)
{

	dmachine->match_callback = match_callback;

}

SWITCH_DECLARE(void) switch_ivr_dmachine_set_nonmatch_callback(switch_ivr_dmachine_t *dmachine, switch_ivr_dmachine_callback_t nonmatch_callback)
{

	dmachine->nonmatch_callback = nonmatch_callback;

}

SWITCH_DECLARE(switch_status_t) switch_ivr_dmachine_create(switch_ivr_dmachine_t **dmachine_p, 
														   const char *name,
														   switch_memory_pool_t *pool,
														   uint32_t digit_timeout_ms, 
														   uint32_t input_timeout_ms,
														   switch_ivr_dmachine_callback_t match_callback,
														   switch_ivr_dmachine_callback_t nonmatch_callback,
														   void *user_data)
{
	switch_byte_t my_pool = 0;
	switch_ivr_dmachine_t *dmachine;

	if (!pool) {
		switch_core_new_memory_pool(&pool);
		my_pool = 1;
	}

	dmachine = switch_core_alloc(pool, sizeof(*dmachine));
	dmachine->pool = pool;
	dmachine->my_pool = my_pool;
	dmachine->digit_timeout_ms = digit_timeout_ms;
	dmachine->input_timeout_ms = input_timeout_ms;
	dmachine->match.dmachine = dmachine;
	dmachine->name = switch_core_strdup(dmachine->pool, name);
	switch_mutex_init(&dmachine->mutex, SWITCH_MUTEX_NESTED, dmachine->pool);
	
	switch_core_hash_init(&dmachine->binding_hash, dmachine->pool);
	
	if (match_callback) {
		dmachine->match_callback = match_callback;
	}

	if (nonmatch_callback) {
		dmachine->nonmatch_callback = nonmatch_callback;
	}

	dmachine->user_data = user_data;
	
	*dmachine_p = dmachine;
	
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_DECLARE(void) switch_ivr_dmachine_set_digit_timeout_ms(switch_ivr_dmachine_t *dmachine, uint32_t digit_timeout_ms)
{
	dmachine->digit_timeout_ms = digit_timeout_ms;
}

SWITCH_DECLARE(void) switch_ivr_dmachine_set_input_timeout_ms(switch_ivr_dmachine_t *dmachine, uint32_t input_timeout_ms)
{
	dmachine->input_timeout_ms = input_timeout_ms;
}

SWITCH_DECLARE(void) switch_ivr_dmachine_destroy(switch_ivr_dmachine_t **dmachine)
{
	switch_memory_pool_t *pool;

	if (!(dmachine && *dmachine)) return;
	
	pool = (*dmachine)->pool;

	switch_core_hash_destroy(&(*dmachine)->binding_hash);
	
	if ((*dmachine)->my_pool) {
		switch_core_destroy_memory_pool(&pool);
	}
}

SWITCH_DECLARE(switch_status_t) switch_ivr_dmachine_set_realm(switch_ivr_dmachine_t *dmachine, const char *realm)
{
	dm_binding_head_t *headp = switch_core_hash_find(dmachine->binding_hash, realm);

	if (headp) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Digit parser %s: Setting realm to %s\n", dmachine->name, realm);
		dmachine->realm = headp;
		return SWITCH_STATUS_SUCCESS;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Digit parser %s: Error Setting realm to %s\n", dmachine->name, realm);

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_dmachine_clear_realm(switch_ivr_dmachine_t *dmachine, const char *realm)
{
	if (zstr(realm)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Digit parser %s: Error unknown realm: %s\n", dmachine->name, realm);
		return SWITCH_STATUS_FALSE;
	}

	/* pool alloc'd just ditch it and it will give back the memory when we destroy ourselves */
	switch_core_hash_delete(dmachine->binding_hash, realm);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_dmachine_bind(switch_ivr_dmachine_t *dmachine, 
														 const char *realm,
														 const char *digits, 
														 int32_t key,
														 switch_ivr_dmachine_callback_t callback,
														 void *user_data)
{
	switch_ivr_dmachine_binding_t *binding;
	switch_size_t len;
	dm_binding_head_t *headp;

	if (strlen(digits) > DMACHINE_MAX_DIGIT_LEN -1) {
		return SWITCH_STATUS_FALSE;
	}

	if (zstr(realm)) {
		realm = "default";
	}

	if (!(headp = switch_core_hash_find(dmachine->binding_hash, realm))) {
		headp = switch_core_alloc(dmachine->pool, sizeof(*headp));
		switch_core_hash_insert(dmachine->binding_hash, realm, headp);
	}
	
	binding = switch_core_alloc(dmachine->pool, sizeof(*binding));

	if (*digits == '~') {
		binding->is_regex = 1;
		digits++;
	}

	binding->key = key;
	binding->digits = switch_core_strdup(dmachine->pool, digits);
	binding->callback = callback;
	binding->user_data = user_data;

	if (headp->tail) {
		headp->tail->next = binding;
	} else {
		headp->binding_list = binding;
	}

	headp->tail = binding;

	len = strlen(digits);

	if (dmachine->realm != headp) {
		switch_ivr_dmachine_set_realm(dmachine, realm);
	}

	if (binding->is_regex && dmachine->max_digit_len != DMACHINE_MAX_DIGIT_LEN -1) { 
		dmachine->max_digit_len = DMACHINE_MAX_DIGIT_LEN -1;
	} else if (len > dmachine->max_digit_len) {
		dmachine->max_digit_len = (uint32_t) len;
	}
	
	if (binding->is_regex) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Digit parser %s: binding %s/%s/%d callback: %p data: %p\n", 
						  dmachine->name, digits, realm, key, (void *)(intptr_t) callback, user_data);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Digit parser %s: binding %s/%s/%d callback: %p data: %p\n", 
						  dmachine->name, digits, realm, key, (void *)(intptr_t) callback, user_data);
	}

	return SWITCH_STATUS_SUCCESS;
}

typedef enum {
	DM_MATCH_NONE,
	DM_MATCH_EXACT,
	DM_MATCH_PARTIAL,
	DM_MATCH_BOTH
} dm_match_t;


static dm_match_t switch_ivr_dmachine_check_match(switch_ivr_dmachine_t *dmachine, switch_bool_t is_timeout)
{
	dm_match_t best = DM_MATCH_NONE;
	switch_ivr_dmachine_binding_t *bp, *exact_bp = NULL, *partial_bp = NULL, *both_bp = NULL, *r_bp = NULL;
	
	if (!dmachine->cur_digit_len || !dmachine->realm) goto end;

	for(bp = dmachine->realm->binding_list; bp; bp = bp->next) {
		if (bp->is_regex) {
			switch_status_t r_status = switch_regex_match(dmachine->digits, bp->digits);

			if (r_status == SWITCH_STATUS_SUCCESS) {
				if (is_timeout) {
					best = DM_MATCH_EXACT;
					exact_bp = bp;
					break;
				}

				best = DM_MATCH_PARTIAL;
			}
		} else {

			if (!exact_bp && !strcmp(bp->digits, dmachine->digits)) {
				best = DM_MATCH_EXACT;
				exact_bp = bp;
				if (dmachine->cur_digit_len == dmachine->max_digit_len) break;
			} 

			if (!(both_bp && partial_bp) && strlen(bp->digits) != strlen(dmachine->digits) && 
				!strncmp(dmachine->digits, bp->digits, strlen(dmachine->digits))) {
				if (exact_bp) {
					best = DM_MATCH_BOTH;
					both_bp = bp;
				} else {
					best = DM_MATCH_PARTIAL;
					partial_bp = bp;
				}
			}

			if (both_bp && exact_bp && partial_bp) break;
		}
	}
	
 end:

	if (is_timeout) {
		if (both_bp) {
			r_bp = exact_bp ? exact_bp : both_bp;
		}
	} 

	if (best == DM_MATCH_EXACT && exact_bp) {
		r_bp = exact_bp;
	}
	
	
	if (r_bp) {
		dmachine->last_matching_binding = r_bp;
		switch_set_string(dmachine->last_matching_digits, dmachine->digits);
		best = DM_MATCH_EXACT;
	}

	return best;
	
}

static switch_bool_t switch_ivr_dmachine_check_timeout(switch_ivr_dmachine_t *dmachine)
{
	switch_time_t now = switch_time_now();
	uint32_t timeout = dmachine->cur_digit_len ? dmachine->digit_timeout_ms : dmachine->input_timeout_ms;

	if (!dmachine->last_digit_time) dmachine->last_digit_time = now;

	if (timeout) {
		if ((uint32_t)((now - dmachine->last_digit_time) / 1000) > timeout) {
			return SWITCH_TRUE;
		}
	}

	return SWITCH_FALSE;
}

SWITCH_DECLARE(switch_ivr_dmachine_match_t *) switch_ivr_dmachine_get_match(switch_ivr_dmachine_t *dmachine)
{
	if (dmachine->is_match) {
		dmachine->is_match = 0;
		return &dmachine->match;
	}

	return NULL;
}

SWITCH_DECLARE(const char *) switch_ivr_dmachine_get_failed_digits(switch_ivr_dmachine_t *dmachine)
{

	return dmachine->last_failed_digits;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_dmachine_ping(switch_ivr_dmachine_t *dmachine, switch_ivr_dmachine_match_t **match_p)
{
	switch_bool_t is_timeout = switch_ivr_dmachine_check_timeout(dmachine);
	dm_match_t is_match = switch_ivr_dmachine_check_match(dmachine, is_timeout);
	switch_status_t r, s;
	int clear = 0;

	switch_mutex_lock(dmachine->mutex);

	if (zstr(dmachine->digits) && !is_timeout) {
		r = SWITCH_STATUS_SUCCESS;
	} else if (dmachine->cur_digit_len > dmachine->max_digit_len) {
		r = SWITCH_STATUS_FALSE;
	} else if (is_match == DM_MATCH_EXACT || (is_match == DM_MATCH_BOTH && is_timeout)) {
		r = SWITCH_STATUS_FOUND;
		
		dmachine->match.match_digits = dmachine->last_matching_digits;
		dmachine->match.match_key = dmachine->last_matching_binding->key;
		dmachine->match.user_data = dmachine->last_matching_binding->user_data;
		
		if (match_p) {
			*match_p = &dmachine->match;
		}

		dmachine->is_match = 1;

		dmachine->match.type = DM_MATCH_POSITIVE;
		
		if (dmachine->last_matching_binding->callback) {
			s = dmachine->last_matching_binding->callback(&dmachine->match);
			
			switch(s) {
			case SWITCH_STATUS_CONTINUE:
				r = SWITCH_STATUS_SUCCESS;
				break;
			case SWITCH_STATUS_SUCCESS:
				break;
			default:
				r = SWITCH_STATUS_BREAK;
				break;
			}
		}

		if (dmachine->match_callback) {
			dmachine->match.user_data = dmachine->user_data;
			s = dmachine->match_callback(&dmachine->match);

			switch(s) {
			case SWITCH_STATUS_CONTINUE:
				r = SWITCH_STATUS_SUCCESS;
				break;
			case SWITCH_STATUS_SUCCESS:
				break;
			default:
				r = SWITCH_STATUS_BREAK;
				break;
			}

		}

		clear++;
	} else if (is_timeout) {
		r = SWITCH_STATUS_TIMEOUT;
	} else if (is_match == DM_MATCH_NONE && dmachine->cur_digit_len == dmachine->max_digit_len) {
		r = SWITCH_STATUS_NOTFOUND;
	} else {
		r = SWITCH_STATUS_SUCCESS;
	}
	
	if (r != SWITCH_STATUS_FOUND && r != SWITCH_STATUS_SUCCESS && r != SWITCH_STATUS_BREAK) {
		switch_set_string(dmachine->last_failed_digits, dmachine->digits);
		dmachine->match.match_digits = dmachine->last_failed_digits;
		
		dmachine->match.type = DM_MATCH_NEGATIVE;
		
		if (dmachine->nonmatch_callback) {
			dmachine->match.user_data = dmachine->user_data;
			s = dmachine->nonmatch_callback(&dmachine->match);

			switch(s) {
			case SWITCH_STATUS_CONTINUE:
				r = SWITCH_STATUS_SUCCESS;
				break;
			case SWITCH_STATUS_SUCCESS:
				break;
			default:
				r = SWITCH_STATUS_BREAK;
				break;
			}

		}
		
		clear++;
	}
	
	if (clear) {
		switch_ivr_dmachine_clear(dmachine);
	}

	switch_mutex_unlock(dmachine->mutex);

	return r;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_dmachine_feed(switch_ivr_dmachine_t *dmachine, const char *digits, switch_ivr_dmachine_match_t **match)
{
	if (strlen(digits) + strlen(dmachine->digits) > dmachine->max_digit_len) {
		return SWITCH_STATUS_FALSE;
	}
	
	switch_mutex_lock(dmachine->mutex);
	strncat(dmachine->digits, digits, dmachine->max_digit_len);
	switch_mutex_unlock(dmachine->mutex);
	dmachine->cur_digit_len = strlen(dmachine->digits);
	dmachine->last_digit_time = switch_time_now();
	
	return switch_ivr_dmachine_ping(dmachine, match);
}

SWITCH_DECLARE(switch_status_t) switch_ivr_dmachine_clear(switch_ivr_dmachine_t *dmachine)
{

	memset(dmachine->digits, 0, sizeof(dmachine->digits));
	dmachine->cur_digit_len = 0;
	dmachine->last_digit_time = 0;
	return SWITCH_STATUS_SUCCESS;
}


#ifdef SWITCH_VIDEO_IN_THREADS
struct echo_helper {
	switch_core_session_t *session;
	int up;
};

static void *SWITCH_THREAD_FUNC echo_video_thread(switch_thread_t *thread, void *obj)
{
	struct echo_helper *eh = obj;
	switch_core_session_t *session = eh->session;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_status_t status;
	switch_frame_t *read_frame;

	eh->up = 1;
	while (switch_channel_ready(channel)) {
		status = switch_core_session_read_video_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (switch_test_flag(read_frame, SFF_CNG)) {
			continue;
		}

		switch_core_session_write_video_frame(session, read_frame, SWITCH_IO_FLAG_NONE, 0);

	}
	eh->up = 0;
	return NULL;
}
#endif

SWITCH_DECLARE(void) switch_ivr_session_echo(switch_core_session_t *session, switch_input_args_t *args)
{
	switch_status_t status;
	switch_frame_t *read_frame;
	switch_channel_t *channel = switch_core_session_get_channel(session);
#ifdef SWITCH_VIDEO_IN_THREADS
	struct echo_helper eh = { 0 };
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
#endif

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return;
	}
#ifdef SWITCH_VIDEO_IN_THREADS
	if (switch_channel_test_flag(channel, CF_VIDEO)) {
		eh.session = session;
		switch_threadattr_create(&thd_attr, switch_core_session_get_pool(session));
		switch_threadattr_detach_set(thd_attr, 1);
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		switch_thread_create(&thread, thd_attr, echo_video_thread, &eh, switch_core_session_get_pool(session));
	}
#endif

	while (switch_channel_ready(channel)) {
		status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		switch_ivr_parse_all_events(session);

		if (args && (args->input_callback || args->buf || args->buflen)) {
			switch_dtmf_t dtmf = {0};

			/*
			   dtmf handler function you can hook up to be executed when a digit is dialed during playback 
			   if you return anything but SWITCH_STATUS_SUCCESS the playback will stop.
			 */
			if (switch_channel_has_dtmf(channel)) {
				if (!args->input_callback && !args->buf) {
					status = SWITCH_STATUS_BREAK;
					break;
				}
				switch_channel_dequeue_dtmf(channel, &dtmf);
				if (args->input_callback) {
					status = args->input_callback(session, (void *) &dtmf, SWITCH_INPUT_TYPE_DTMF, args->buf, args->buflen);
				} else {
					switch_copy_string((char *) args->buf, (void *) &dtmf, args->buflen);
					status = SWITCH_STATUS_BREAK;
				}
			}

			if (args->input_callback) {
				switch_event_t *event = NULL;

				if (switch_core_session_dequeue_event(session, &event, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
					status = args->input_callback(session, event, SWITCH_INPUT_TYPE_EVENT, args->buf, args->buflen);
					switch_event_destroy(&event);
				}
			}

			if (status != SWITCH_STATUS_SUCCESS) {
				break;
			}
		}


		switch_core_session_write_frame(session, read_frame, SWITCH_IO_FLAG_NONE, 0);

#ifndef SWITCH_VIDEO_IN_THREADS
		status = switch_core_session_read_video_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (switch_test_flag(read_frame, SFF_CNG)) {
			continue;
		}

		switch_core_session_write_video_frame(session, read_frame, SWITCH_IO_FLAG_NONE, 0);
#endif

		if (switch_channel_test_flag(channel, CF_BREAK)) {
			switch_channel_clear_flag(channel, CF_BREAK);
			break;
		}
	}

#ifdef SWITCH_VIDEO_IN_THREADS
	if (eh.up) {
		while (eh.up) {
			switch_cond_next();
		}
	}
#endif

}

typedef struct {
	switch_file_handle_t fh;
	int mux;
	int loop;
	char *file;
} displace_helper_t;

static switch_bool_t write_displace_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	displace_helper_t *dh = (displace_helper_t *) user_data;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		if (dh) {
			switch_core_session_t *session = switch_core_media_bug_get_session(bug);
			switch_channel_t *channel;

			switch_core_file_close(&dh->fh);

			if (session && (channel = switch_core_session_get_channel(session))) {
				switch_channel_set_private(channel, dh->file, NULL);
			}
		}
		break;
	case SWITCH_ABC_TYPE_READ_REPLACE:
		{
			switch_frame_t *rframe = switch_core_media_bug_get_read_replace_frame(bug);
			if (dh && !dh->mux) {
				memset(rframe->data, 255, rframe->datalen);
			}
			switch_core_media_bug_set_read_replace_frame(bug, rframe);
		}
		break;
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		if (dh) {
			switch_frame_t *rframe = NULL;
			switch_size_t len;
			switch_status_t st;

			rframe = switch_core_media_bug_get_write_replace_frame(bug);
			len = rframe->samples;

			if (dh->mux) {
				int16_t buf[SWITCH_RECOMMENDED_BUFFER_SIZE / 2];
				int16_t *fp = rframe->data;
				uint32_t x;

				st = switch_core_file_read(&dh->fh, buf, &len);

				for (x = 0; x < (uint32_t) len; x++) {
					int32_t mixed = fp[x] + buf[x];
					switch_normalize_to_16bit(mixed);
					fp[x] = (int16_t) mixed;
				}
			} else {
				st = switch_core_file_read(&dh->fh, rframe->data, &len);
				rframe->samples = (uint32_t) len;
				rframe->datalen = rframe->samples * 2;
			}

			if (st != SWITCH_STATUS_SUCCESS || len == 0) {
				if (dh->loop) {
					uint32_t pos = 0;
					switch_core_file_seek(&dh->fh, &pos, 0, SEEK_SET);
				} else {
					switch_core_session_t *session = switch_core_media_bug_get_session(bug);
					switch_channel_t *channel;

					if (session && (channel = switch_core_session_get_channel(session))) {
						switch_channel_set_private(channel, dh->file, NULL);
					}
					return SWITCH_FALSE;
				}
			}

			switch_core_media_bug_set_write_replace_frame(bug, rframe);
		}
		break;
	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_bool_t read_displace_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	displace_helper_t *dh = (displace_helper_t *) user_data;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		if (dh) {
			switch_core_session_t *session = switch_core_media_bug_get_session(bug);
			switch_channel_t *channel;

			switch_core_file_close(&dh->fh);

			if (session && (channel = switch_core_session_get_channel(session))) {
				switch_channel_set_private(channel, dh->file, NULL);
			}
		}
		break;
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		{
			switch_frame_t *rframe = switch_core_media_bug_get_write_replace_frame(bug);
			if (dh && !dh->mux) {
				memset(rframe->data, 255, rframe->datalen);
			}
			switch_core_media_bug_set_write_replace_frame(bug, rframe);
		}
		break;
	case SWITCH_ABC_TYPE_READ_REPLACE:
		if (dh) {
			switch_frame_t *rframe = NULL;
			switch_size_t len;
			switch_status_t st;
			rframe = switch_core_media_bug_get_read_replace_frame(bug);
			len = rframe->samples;

			if (dh->mux) {
				int16_t buf[SWITCH_RECOMMENDED_BUFFER_SIZE / 2];
				int16_t *fp = rframe->data;
				uint32_t x;

				st = switch_core_file_read(&dh->fh, buf, &len);

				for (x = 0; x < (uint32_t) len; x++) {
					int32_t mixed = fp[x] + buf[x];
					switch_normalize_to_16bit(mixed);
					fp[x] = (int16_t) mixed;
				}
			} else {
				st = switch_core_file_read(&dh->fh, rframe->data, &len);
				rframe->samples = (uint32_t) len;
				rframe->datalen = rframe->samples * 2;
			}

			if (st != SWITCH_STATUS_SUCCESS || len == 0) {
				if (dh->loop) {
					uint32_t pos = 0;
					switch_core_file_seek(&dh->fh, &pos, 0, SEEK_SET);
				} else {
					switch_core_session_t *session = switch_core_media_bug_get_session(bug);
					switch_channel_t *channel;

					if (session && (channel = switch_core_session_get_channel(session))) {
						switch_channel_set_private(channel, dh->file, NULL);
					}
					return SWITCH_FALSE;
				}
			}

			switch_core_media_bug_set_read_replace_frame(bug, rframe);
		}
		break;
	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_stop_displace_session(switch_core_session_t *session, const char *file)
{
	switch_media_bug_t *bug;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if ((bug = switch_channel_get_private(channel, file))) {
		switch_channel_set_private(channel, file, NULL);
		switch_core_media_bug_remove(session, &bug);
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_displace_session(switch_core_session_t *session, const char *file, uint32_t limit, const char *flags)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	time_t to = 0;
	char *ext;
	const char *prefix;
	displace_helper_t *dh;
	switch_codec_implementation_t read_impl = { 0 };
	switch_core_session_get_read_impl(session, &read_impl);

	if (zstr(file)) {
		return SWITCH_STATUS_FALSE;
	}

	if ((status = switch_channel_pre_answer(channel)) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_channel_media_ready(channel) || !switch_core_session_get_read_codec(session)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can not displace session.  Media not enabled on channel\n");
		return SWITCH_STATUS_FALSE;
	}

	if ((bug = switch_channel_get_private(channel, file))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Only 1 of the same file per channel please!\n");
		return SWITCH_STATUS_FALSE;
	}

	if (!(dh = switch_core_session_alloc(session, sizeof(*dh)))) {
		return SWITCH_STATUS_MEMERR;
	}

	if (!(prefix = switch_channel_get_variable(channel, "sound_prefix"))) {
		prefix = SWITCH_GLOBAL_dirs.base_dir;
	}

	if (!strstr(file, SWITCH_URL_SEPARATOR)) {
		if (!switch_is_file_path(file)) {
			char *tfile = NULL;
			char *e;

			if (*file == '[') {
				tfile = switch_core_session_strdup(session, file);
				if ((e = switch_find_end_paren(tfile, '[', ']'))) {
					*e = '\0';
					file = e + 1;
				} else {
					tfile = NULL;
				}
			}

			file = switch_core_session_sprintf(session, "%s%s%s%s%s", switch_str_nil(tfile), tfile ? "]" : "", prefix, SWITCH_PATH_SEPARATOR, file);
		}
		if ((ext = strrchr(file, '.'))) {
			ext++;
		} else {
			ext = read_impl.iananame;
			file = switch_core_session_sprintf(session, "%s.%s", file, ext);
		}
	}

	dh->fh.channels = read_impl.number_of_channels;
	dh->fh.samplerate = read_impl.actual_samples_per_second;
	dh->file = switch_core_session_strdup(session, file);

	if (switch_core_file_open(&dh->fh,
							  file,
							  read_impl.number_of_channels,
							  read_impl.actual_samples_per_second, SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
		switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
		return SWITCH_STATUS_GENERR;
	}

	if (limit) {
		to = switch_epoch_time_now(NULL) + limit;
	}

	if (flags && strchr(flags, 'm')) {
		dh->mux++;
	}

	if (flags && strchr(flags, 'l')) {
		dh->loop++;
	}

	if (flags && strchr(flags, 'r')) {
		status = switch_core_media_bug_add(session, "displace", file,
										   read_displace_callback, dh, to, SMBF_WRITE_REPLACE | SMBF_READ_REPLACE | SMBF_NO_PAUSE, &bug);
	} else {
		status = switch_core_media_bug_add(session, "displace", file,
										   write_displace_callback, dh, to, SMBF_WRITE_REPLACE | SMBF_READ_REPLACE | SMBF_NO_PAUSE, &bug);
	}

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_core_file_close(&dh->fh);
		return status;
	}

	switch_channel_set_private(channel, file, bug);

	return SWITCH_STATUS_SUCCESS;
}


struct record_helper {
	char *file;
	switch_file_handle_t *fh;
	uint32_t packet_len;
	int min_sec;
	switch_bool_t hangup_on_error;
};

static switch_bool_t record_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct record_helper *rh = (struct record_helper *) user_data;
	switch_event_t *event;


	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		if (switch_event_create(&event, SWITCH_EVENT_RECORD_START) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Record-File-Path", rh->file);
			switch_channel_event_set_data(channel, event);
			switch_event_fire(&event);
		}

		break;
	case SWITCH_ABC_TYPE_CLOSE:
		{
			const char *var;

			switch_codec_implementation_t read_impl = { 0 };
			switch_core_session_get_read_impl(session, &read_impl);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Stop recording file %s\n", rh->file);
			switch_channel_set_private(channel, rh->file, NULL);

			if (rh->fh) {
				switch_size_t len;
				uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
				switch_frame_t frame = { 0 };

				frame.data = data;
				frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

				while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
					len = (switch_size_t) frame.datalen / 2;

					if (len && switch_core_file_write(rh->fh, data, &len) != SWITCH_STATUS_SUCCESS && rh->hangup_on_error) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error writing %s\n", rh->file);
						switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
						switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
						return SWITCH_FALSE;
					}
				}


				switch_core_file_close(rh->fh);
				if (rh->fh->samples_out < rh->fh->samplerate * rh->min_sec) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Discarding short file %s\n", rh->file);
					switch_channel_set_variable(channel, "RECORD_DISCARDED", "true");
					switch_file_remove(rh->file, switch_core_session_get_pool(session));
				}
			}

			if (switch_event_create(&event, SWITCH_EVENT_RECORD_STOP) == SWITCH_STATUS_SUCCESS) {
				switch_channel_event_set_data(channel, event);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Record-File-Path", rh->file);
				switch_event_fire(&event);
			}

			if ((var = switch_channel_get_variable(channel, "record_post_process_exec_app"))) {
				char *app = switch_core_session_strdup(session, var);
				char *data;

				if ((data = strchr(app, ':'))) {
					*data++ = '\0';
				}

				switch_core_session_execute_application(session, app, data);
			}

			if ((var = switch_channel_get_variable(channel, "record_post_process_exec_api"))) {
				char *cmd = switch_core_session_strdup(session, var);
				char *data, *expanded = NULL;
				switch_stream_handle_t stream = { 0 };

				SWITCH_STANDARD_STREAM(stream);

				if ((data = strchr(cmd, ':'))) {
					*data++ = '\0';
					expanded = switch_channel_expand_variables(channel, data);
				}

				switch_api_execute(cmd, expanded, session, &stream);

				if (expanded && expanded != data) {
					free(expanded);
				}

				switch_safe_free(stream.data);

			}


		}

		break;
	case SWITCH_ABC_TYPE_READ:

		if (rh->fh) {
			switch_size_t len;
			uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
			switch_frame_t frame = { 0 };

			frame.data = data;
			frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

			while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
				len = (switch_size_t) frame.datalen / 2;
				if (len && switch_core_file_write(rh->fh, data, &len) != SWITCH_STATUS_SUCCESS && rh->hangup_on_error) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error writing %s\n", rh->file);
					switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
					switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
					return SWITCH_FALSE;
				}
			}

		}
		break;
	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_stop_record_session(switch_core_session_t *session, const char *file)
{
	switch_media_bug_t *bug;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (!strcasecmp(file, "all")) {
		return switch_core_media_bug_remove_callback(session, record_callback);
	} else if ((bug = switch_channel_get_private(channel, file))) {
		switch_core_media_bug_remove(session, &bug);
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

struct eavesdrop_pvt {
	switch_buffer_t *buffer;
	switch_mutex_t *mutex;
	switch_buffer_t *r_buffer;
	switch_mutex_t *r_mutex;
	switch_buffer_t *w_buffer;
	switch_mutex_t *w_mutex;
	uint32_t flags;
};

static switch_bool_t eavesdrop_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	struct eavesdrop_pvt *ep = (struct eavesdrop_pvt *) user_data;
	uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
	switch_frame_t frame = { 0 };

	frame.data = data;
	frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		break;
	case SWITCH_ABC_TYPE_WRITE:
		break;
	case SWITCH_ABC_TYPE_READ_PING:
		if (ep->buffer) {
			if (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
				switch_buffer_lock(ep->buffer);
				switch_buffer_zwrite(ep->buffer, frame.data, frame.datalen);
				switch_buffer_unlock(ep->buffer);
			}
		} else {
			return SWITCH_FALSE;
		}
		break;
	case SWITCH_ABC_TYPE_READ:
		break;

	case SWITCH_ABC_TYPE_READ_REPLACE:
		{

			if (switch_test_flag(ep, ED_MUX_READ)) {
				switch_frame_t *rframe = switch_core_media_bug_get_read_replace_frame(bug);

				if (switch_buffer_inuse(ep->r_buffer) >= rframe->datalen) {
					uint32_t bytes;
					switch_buffer_lock(ep->r_buffer);
					bytes = (uint32_t) switch_buffer_read(ep->r_buffer, data, rframe->datalen);

					rframe->datalen = switch_merge_sln(rframe->data, rframe->samples, (int16_t *) data, bytes / 2) * 2;
					rframe->samples = rframe->datalen / 2;

					switch_buffer_unlock(ep->r_buffer);
					switch_core_media_bug_set_read_replace_frame(bug, rframe);
				}
			}
		}
		break;

	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		{
			if (switch_test_flag(ep, ED_MUX_WRITE)) {
				switch_frame_t *rframe = switch_core_media_bug_get_write_replace_frame(bug);

				if (switch_buffer_inuse(ep->w_buffer) >= rframe->datalen) {
					uint32_t bytes;
					switch_buffer_lock(ep->w_buffer);
					bytes = (uint32_t) switch_buffer_read(ep->w_buffer, data, rframe->datalen);

					rframe->datalen = switch_merge_sln(rframe->data, rframe->samples, (int16_t *) data, bytes / 2) * 2;
					rframe->samples = rframe->datalen / 2;

					switch_buffer_unlock(ep->w_buffer);
					switch_core_media_bug_set_write_replace_frame(bug, rframe);
				}
			}
		}
		break;

	default:
		break;
	}

	return SWITCH_TRUE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_eavesdrop_session(switch_core_session_t *session,
															 const char *uuid, const char *require_group, switch_eavesdrop_flag_t flags)
{
	switch_core_session_t *tsession;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int codec_initialized = 0;

	if ((tsession = switch_core_session_locate(uuid))) {
		struct eavesdrop_pvt *ep = NULL;
		switch_media_bug_t *bug = NULL;
		switch_channel_t *tchannel = switch_core_session_get_channel(tsession);
		switch_frame_t *read_frame, write_frame = { 0 };
		switch_codec_t codec = { 0 };
		int16_t buf[SWITCH_RECOMMENDED_BUFFER_SIZE / 2];
		uint32_t tlen;
		const char *macro_name = "eavesdrop_announce";
		const char *id_name = NULL;
		switch_codec_implementation_t tread_impl = { 0 }, read_impl = { 0 };
		switch_core_session_message_t msg = { 0 };
		char cid_buf[1024] = "";
		switch_caller_profile_t *cp = NULL;
		uint32_t sanity = 600;

		if (!switch_channel_media_ready(channel)) {
			goto end;
		}

		while(switch_channel_state_change_pending(tchannel) || !switch_channel_media_ready(tchannel)) {
			switch_yield(10000);
			if (!--sanity) break;
		}

		if (!switch_channel_media_up(tchannel)) {
			goto end;
		}

		switch_core_session_get_read_impl(tsession, &tread_impl);
		switch_core_session_get_read_impl(session, &read_impl);

		if ((id_name = switch_channel_get_variable(tchannel, "eavesdrop_announce_id"))) {
			const char *tmp = switch_channel_get_variable(tchannel, "eavesdrop_annnounce_macro");
			if (tmp) {
				macro_name = tmp;
			}

			switch_ivr_phrase_macro(session, macro_name, id_name, NULL, NULL);
		}


		if (!zstr(require_group)) {
			int argc, i;
			int ok = 0;
			char *argv[10] = { 0 };
			char *data;

			const char *group_name = switch_channel_get_variable(tchannel, "eavesdrop_group");
			/* If we don't have a group, then return */
			if (!group_name) {
				status = SWITCH_STATUS_BREAK;
				goto end;
			}
			/* Separate the group */
			data = strdup(group_name);
			if ((argc = switch_separate_string(data, ',', argv, (sizeof(argv) / sizeof(argv[0]))))) {
				for (i = 0; i < argc; i++) {
					/* If one of the group matches, then ok */
					if (argv[i] && !strcmp(argv[i], require_group)) {
						ok = 1;
					}
				}
			}
			switch_safe_free(data);
			/* If we didn't find any match, then end */
			if (!ok) {
				status = SWITCH_STATUS_BREAK;
				goto end;
			}
		}


		ep = switch_core_session_alloc(session, sizeof(*ep));

		tlen = tread_impl.decoded_bytes_per_packet;


		if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
			goto end;
		}


		if (switch_core_codec_init(&codec,
								   "L16",
								   NULL,
								   tread_impl.actual_samples_per_second,
								   tread_impl.microseconds_per_packet / 1000,
								   tread_impl.number_of_channels,
								   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
								   NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot init codec\n");
			switch_core_session_rwunlock(tsession);
			goto end;
		}

		codec_initialized = 1;

		switch_core_session_set_read_codec(session, &codec);
		write_frame.codec = &codec;
		write_frame.data = buf;
		write_frame.buflen = sizeof(buf);
		write_frame.rate = codec.implementation->actual_samples_per_second;

		ep->flags = flags;
		switch_mutex_init(&ep->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(tsession));
		switch_buffer_create_dynamic(&ep->buffer, 2048, 2048, 8192);
		switch_buffer_add_mutex(ep->buffer, ep->mutex);

		switch_mutex_init(&ep->w_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(tsession));
		switch_buffer_create_dynamic(&ep->w_buffer, 2048, 2048, 8192);
		switch_buffer_add_mutex(ep->w_buffer, ep->w_mutex);

		switch_mutex_init(&ep->r_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(tsession));
		switch_buffer_create_dynamic(&ep->r_buffer, 2048, 2048, 8192);
		switch_buffer_add_mutex(ep->r_buffer, ep->r_mutex);


		if (switch_core_media_bug_add(tsession, "eavesdrop", uuid,
									  eavesdrop_callback, ep, 0,
									  SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_REPLACE | SMBF_WRITE_REPLACE | 
									  SMBF_READ_PING | SMBF_THREAD_LOCK | SMBF_NO_PAUSE,
									  &bug) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot attach bug\n");
			goto end;
		}


		msg.from = __FILE__;

		/* Tell the channel we are going to be in a bridge */
		msg.message_id = SWITCH_MESSAGE_INDICATE_BRIDGE;
		switch_core_session_receive_message(session, &msg);
		
		cp = switch_channel_get_caller_profile(tchannel);
		switch_snprintf(cid_buf, sizeof(cid_buf), "%s|%s", cp->caller_id_number, cp->caller_id_name);
		msg.string_arg = cid_buf;
		msg.message_id = SWITCH_MESSAGE_INDICATE_DISPLAY;
		switch_core_session_receive_message(session, &msg);

		while (switch_channel_up(tchannel) && switch_channel_ready(channel)) {
			uint32_t len = sizeof(buf);
			switch_event_t *event = NULL;
			char *fcommand = NULL;
			char db[2] = "";

			status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

			if (!SWITCH_READ_ACCEPTABLE(status)) {
				goto end_loop;
			}

			if (switch_core_session_dequeue_event(session, &event, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
				char *command = switch_event_get_header(event, "eavesdrop-command");
				if (command) {
					fcommand = switch_core_session_strdup(session, command);
				}
				switch_event_destroy(&event);
			}

			if ((flags & ED_DTMF) && switch_channel_has_dtmf(channel)) {
				switch_dtmf_t dtmf = { 0 };
				switch_channel_dequeue_dtmf(channel, &dtmf);
				db[0] = dtmf.digit;
				fcommand = db;
			}

			if (fcommand) {
				char *d;
				for (d = fcommand; *d; d++) {
					int z = 1;

					switch (*d) {
					case '1':
						switch_set_flag(ep, ED_MUX_READ);
						switch_clear_flag(ep, ED_MUX_WRITE);
						break;
					case '2':
						switch_set_flag(ep, ED_MUX_WRITE);
						switch_clear_flag(ep, ED_MUX_READ);
						break;
					case '3':
						switch_set_flag(ep, ED_MUX_READ);
						switch_set_flag(ep, ED_MUX_WRITE);
						break;
					case '0':
						switch_clear_flag(ep, ED_MUX_READ);
						switch_clear_flag(ep, ED_MUX_WRITE);
						break;
					case '*':
						goto end_loop;
					default:
						z = 0;
						break;

					}

					if (z) {
						if (ep->r_buffer) {
							switch_buffer_lock(ep->r_buffer);
							switch_buffer_zero(ep->r_buffer);
							switch_buffer_unlock(ep->r_buffer);
						}

						if (ep->w_buffer) {
							switch_buffer_lock(ep->w_buffer);
							switch_buffer_zero(ep->w_buffer);
							switch_buffer_unlock(ep->w_buffer);
						}
					}
				}
			}

			if (!switch_test_flag(read_frame, SFF_CNG)) {
				switch_buffer_lock(ep->r_buffer);
				switch_buffer_zwrite(ep->r_buffer, read_frame->data, read_frame->datalen);
				switch_buffer_unlock(ep->r_buffer);

				switch_buffer_lock(ep->w_buffer);
				switch_buffer_zwrite(ep->w_buffer, read_frame->data, read_frame->datalen);
				switch_buffer_unlock(ep->w_buffer);
			}

			if (len > tlen) {
				len = tlen;
			}

			if (switch_buffer_inuse(ep->buffer) >= len) {
				switch_buffer_lock(ep->buffer);
				while (switch_buffer_inuse(ep->buffer) >= len) {
					write_frame.datalen = (uint32_t) switch_buffer_read(ep->buffer, buf, len);
					write_frame.samples = write_frame.datalen / 2;

					if ((status = switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0)) != SWITCH_STATUS_SUCCESS) {
						break;
					}
				}
				switch_buffer_unlock(ep->buffer);
			}

		}

	  end_loop:

		/* Tell the channel we are no longer going to be in a bridge */
		msg.message_id = SWITCH_MESSAGE_INDICATE_UNBRIDGE;
		switch_core_session_receive_message(session, &msg);



	  end:

		if (codec_initialized)
			switch_core_codec_destroy(&codec);

		if (bug) {
			switch_core_media_bug_remove(tsession, &bug);
		}

		if (ep) {
			if (ep->buffer) {
				switch_buffer_destroy(&ep->buffer);
			}

			if (ep->r_buffer) {
				switch_buffer_destroy(&ep->r_buffer);
			}

			if (ep->w_buffer) {
				switch_buffer_destroy(&ep->w_buffer);
			}
		}

		switch_core_session_rwunlock(tsession);
		status = SWITCH_STATUS_SUCCESS;

		switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
	}

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_record_session(switch_core_session_t *session, char *file, uint32_t limit, switch_file_handle_t *fh)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *p;
	const char *vval;
	switch_media_bug_t *bug;
	switch_status_t status;
	time_t to = 0;
	switch_media_bug_flag_t flags = SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING;
	uint8_t channels;
	switch_codec_implementation_t read_impl = { 0 };
	struct record_helper *rh = NULL;
	int file_flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT;
	switch_bool_t hangup_on_error = SWITCH_FALSE;
	char *file_path = NULL;
	
	if ((p = switch_channel_get_variable(channel, "RECORD_HANGUP_ON_ERROR"))) {
		hangup_on_error = switch_true(p);
	}

	switch_core_session_get_read_impl(session, &read_impl);

	if ((status = switch_channel_pre_answer(channel)) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_channel_media_ready(channel) || !switch_core_session_get_read_codec(session)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can not record session.  Media not enabled on channel\n");
		return SWITCH_STATUS_FALSE;
	}

	channels = read_impl.number_of_channels;

	if ((bug = switch_channel_get_private(channel, file))) {
		if (switch_true(switch_channel_get_variable(channel, "RECORD_TOGGLE_ON_REPEAT"))) {
			return switch_ivr_stop_record_session(session, file);
		}
		
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Already recording [%s]\n", file);
		return SWITCH_STATUS_SUCCESS;
	}

	
	if ((p = switch_channel_get_variable(channel, "RECORD_CHECK_BRIDGE")) && switch_true(p)) {
		switch_core_session_t *other_session;
		int exist = 0;
		switch_status_t rstatus = SWITCH_STATUS_SUCCESS;
		
		if (switch_core_session_get_partner(session, &other_session) == SWITCH_STATUS_SUCCESS) {
			switch_channel_t *other_channel = switch_core_session_get_channel(other_session);
			if ((bug = switch_channel_get_private(other_channel, file))) {
				if (switch_true(switch_channel_get_variable(other_channel, "RECORD_TOGGLE_ON_REPEAT"))) {
					rstatus = switch_ivr_stop_record_session(other_session, file);
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(other_session), SWITCH_LOG_WARNING, "Already recording [%s]\n", file);
				}
				exist = 1;
			}
			switch_core_session_rwunlock(other_session);
		}
		
		if (exist) {
			return rstatus;
		}
	}

	if (!fh) {
		if (!(fh = switch_core_session_alloc(session, sizeof(*fh)))) {
			return SWITCH_STATUS_MEMERR;
		}
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_WRITE_ONLY")) && switch_true(p)) {
		flags &= ~SMBF_READ_STREAM;
		flags |= SMBF_WRITE_STREAM;
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_READ_ONLY")) && switch_true(p)) {
		flags &= ~SMBF_WRITE_STREAM;
		flags |= SMBF_READ_STREAM;
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_STEREO")) && switch_true(p)) {
		flags |= SMBF_STEREO;
		channels = 2;
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_ANSWER_REQ")) && switch_true(p)) {
		flags |= SMBF_ANSWER_REQ;
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_APPEND")) && switch_true(p)) {
		file_flags |= SWITCH_FILE_WRITE_APPEND;
	}


	fh->samplerate = 0;
	if ((vval = switch_channel_get_variable(channel, "record_sample_rate"))) {
		int tmp = 0;

		tmp = atoi(vval);

		if (switch_is_valid_rate(tmp)) {
			fh->samplerate = tmp;
		}
	}

	if (!fh->samplerate) {
		fh->samplerate = read_impl.actual_samples_per_second;
	}

	fh->channels = channels;
	fh->pre_buffer_datalen = SWITCH_DEFAULT_FILE_BUFFER_LEN;



	if (!switch_is_file_path(file)) {
		char *tfile = NULL;
		char *e;
		const char *prefix;

		prefix = switch_channel_get_variable(channel, "sound_prefix");

		if (!prefix) {
			prefix = SWITCH_GLOBAL_dirs.base_dir;
		}

		if (*file == '[') {
			tfile = switch_core_session_strdup(session, file);
			if ((e = switch_find_end_paren(tfile, '[', ']'))) {
				*e = '\0';
				file = e + 1;
			} else {
				tfile = NULL;
			}
		} else {
			file_path = switch_core_session_sprintf(session, "%s%s%s", prefix, SWITCH_PATH_SEPARATOR, file);
		}

		file = switch_core_session_sprintf(session, "%s%s%s%s%s", switch_str_nil(tfile), tfile ? "]" : "", prefix, SWITCH_PATH_SEPARATOR, file);
	} else {
		file_path = switch_core_session_strdup(session, file);
	}

	if (file_path) {
		char *p;
		char *path = switch_core_session_strdup(session, file_path);

		if ((p = strrchr(path, *SWITCH_PATH_SEPARATOR))) {
			*p = '\0';
			if (switch_dir_make_recursive(path, SWITCH_DEFAULT_DIR_PERMS, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error creating %s\n", path);
				return SWITCH_STATUS_GENERR;
			}

		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error finding the folder path section in '%s'\n", path);
			path = NULL;
		}
	}

	if (switch_core_file_open(fh, file, channels, read_impl.actual_samples_per_second, file_flags, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error opening %s\n", file);
		if (hangup_on_error) {
			switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
		}
		return SWITCH_STATUS_GENERR;
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_TITLE"))) {
		vval = (const char *) switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_TITLE, vval);
		switch_channel_set_variable(channel, "RECORD_TITLE", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_COPYRIGHT"))) {
		vval = (const char *) switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_COPYRIGHT, vval);
		switch_channel_set_variable(channel, "RECORD_COPYRIGHT", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_SOFTWARE"))) {
		vval = (const char *) switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_SOFTWARE, vval);
		switch_channel_set_variable(channel, "RECORD_SOFTWARE", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_ARTIST"))) {
		vval = (const char *) switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_ARTIST, vval);
		switch_channel_set_variable(channel, "RECORD_ARTIST", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_COMMENT"))) {
		vval = (const char *) switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_COMMENT, vval);
		switch_channel_set_variable(channel, "RECORD_COMMENT", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_DATE"))) {
		vval = (const char *) switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_DATE, vval);
		switch_channel_set_variable(channel, "RECORD_DATE", NULL);
	}

	if (limit) {
		to = switch_epoch_time_now(NULL) + limit;
	}

	rh = switch_core_session_alloc(session, sizeof(*rh));
	rh->fh = fh;
	rh->file = switch_core_session_strdup(session, file);
	rh->packet_len = read_impl.decoded_bytes_per_packet;

	if (file_flags & SWITCH_FILE_WRITE_APPEND) {
		rh->min_sec = 3;
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_MIN_SEC"))) {
		int tmp = atoi(p);
		if (tmp >= 0) {
			rh->min_sec = tmp;
		}
	}

	rh->hangup_on_error = hangup_on_error;
	
	if ((status = switch_core_media_bug_add(session, "session_record", file,
											record_callback, rh, to, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error adding media bug for file %s\n", file);
		switch_core_file_close(fh);
		return status;
	}

	switch_channel_set_private(channel, file, bug);

	return SWITCH_STATUS_SUCCESS;
}


typedef struct {
	SpeexPreprocessState *read_st;
	SpeexPreprocessState *write_st;
	SpeexEchoState *read_ec;
	SpeexEchoState *write_ec;
	switch_byte_t read_data[2048];
	switch_byte_t write_data[2048];
	switch_byte_t read_out[2048];
	switch_byte_t write_out[2048];
	switch_mutex_t *read_mutex;
	switch_mutex_t *write_mutex;
	int done;
} pp_cb_t;

static switch_bool_t preprocess_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	pp_cb_t *cb = (pp_cb_t *) user_data;
	switch_codec_implementation_t read_impl = { 0 };
	switch_frame_t *frame = NULL;

	switch_core_session_get_read_impl(session, &read_impl);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		{
			switch_mutex_init(&cb->read_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
			switch_mutex_init(&cb->write_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		{
			if (cb->read_st) {
				speex_preprocess_state_destroy(cb->read_st);
			}

			if (cb->write_st) {
				speex_preprocess_state_destroy(cb->write_st);
			}

			if (cb->read_ec) {
				speex_echo_state_destroy(cb->read_ec);
			}

			if (cb->write_ec) {
				speex_echo_state_destroy(cb->write_ec);
			}

			switch_channel_set_private(channel, "_preprocess", NULL);
		}
		break;
	case SWITCH_ABC_TYPE_READ_REPLACE:
		{
			if (cb->done)
				return SWITCH_FALSE;
			frame = switch_core_media_bug_get_read_replace_frame(bug);

			if (cb->read_st) {

				if (cb->read_ec) {
					speex_echo_cancellation(cb->read_ec, (int16_t *) frame->data, (int16_t *) cb->write_data, (int16_t *) cb->read_out);
					memcpy(frame->data, cb->read_out, frame->datalen);
				}

				speex_preprocess_run(cb->read_st, frame->data);
			}

			if (cb->write_ec) {
				memcpy(cb->read_data, frame->data, frame->datalen);
			}
		}
		break;
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		{
			if (cb->done)
				return SWITCH_FALSE;
			frame = switch_core_media_bug_get_write_replace_frame(bug);

			if (cb->write_st) {

				if (cb->write_ec) {
					speex_echo_cancellation(cb->write_ec, (int16_t *) frame->data, (int16_t *) cb->read_data, (int16_t *) cb->write_out);
					memcpy(frame->data, cb->write_out, frame->datalen);
				}

				speex_preprocess_run(cb->write_st, frame->data);
			}

			if (cb->read_ec) {
				memcpy(cb->write_data, frame->data, frame->datalen);
			}
		}
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_preprocess_session(switch_core_session_t *session, const char *cmds)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	time_t to = 0;
	switch_media_bug_flag_t flags = SMBF_NO_PAUSE;
	switch_codec_implementation_t read_impl = { 0 };
	pp_cb_t *cb;
	int update = 0;
	int argc;
	char *mydata = NULL, *argv[5];
	int i = 0;

	switch_core_session_get_read_impl(session, &read_impl);

	if ((cb = switch_channel_get_private(channel, "_preprocess"))) {
		update = 1;
	} else {
		cb = switch_core_session_alloc(session, sizeof(*cb));
	}


	if (update) {
		if (!strcasecmp(cmds, "stop")) {
			cb->done = 1;
			return SWITCH_STATUS_SUCCESS;
		}
	}

	mydata = strdup(cmds);
	argc = switch_separate_string(mydata, ',', argv, (sizeof(argv) / sizeof(argv[0])));

	for (i = 0; i < argc; i++) {
		char *var = argv[i];
		char *val = NULL;
		char rw;
		int tr;
		int err = 1;
		SpeexPreprocessState *st = NULL;
		SpeexEchoState *ec = NULL;
		switch_mutex_t *mutex = NULL;
		int r = 0;

		if (var) {
			if ((val = strchr(var, '='))) {
				*val++ = '\0';

				rw = *var++;
				while (*var == '.' || *var == '_') {
					var++;
				}

				if (rw == 'r') {
					if (!cb->read_st) {
						cb->read_st = speex_preprocess_state_init(read_impl.samples_per_packet, read_impl.samples_per_second);
						flags |= SMBF_READ_REPLACE;
					}
					st = cb->read_st;
					ec = cb->read_ec;
					mutex = cb->read_mutex;
				} else if (rw == 'w') {
					if (!cb->write_st) {
						cb->write_st = speex_preprocess_state_init(read_impl.samples_per_packet, read_impl.samples_per_second);
						flags |= SMBF_WRITE_REPLACE;
					}
					st = cb->write_st;
					ec = cb->write_ec;
					mutex = cb->write_mutex;
				}

				if (mutex)
					switch_mutex_lock(mutex);

				if (st) {
					err = 0;
					tr = switch_true(val);
					if (!strcasecmp(var, "agc")) {
						int l = read_impl.samples_per_second;
						int tmp = atoi(val);

						if (!tr) {
							l = tmp;
						}

						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Setting AGC on %c to %d\n", rw, tr);
						speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_AGC, &tr);
						speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_AGC_LEVEL, &l);

					} else if (!strcasecmp(var, "noise_suppress")) {
						int db = atoi(val);
						if (db < 0) {
							r = speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &db);
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Setting NOISE_SUPPRESS on %c to %d [%d]\n", rw, db,
											  r);
						} else {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Syntax error noise_suppress should be in -db\n");
						}
					} else if (!strcasecmp(var, "echo_cancel")) {
						int tail = 1024;
						int tmp = atoi(val);

						if (!tr && tmp > 0) {
							tail = tmp;
						} else if (!tr) {
							if (ec) {
								if (rw == 'r') {
									speex_echo_state_destroy(cb->read_ec);
									cb->read_ec = NULL;
								} else {
									speex_echo_state_destroy(cb->write_ec);
									cb->write_ec = NULL;
								}
							}

							ec = NULL;
						}

						if (!ec) {
							if (rw == 'r') {
								ec = cb->read_ec = speex_echo_state_init(read_impl.samples_per_packet, tail);
								speex_echo_ctl(ec, SPEEX_ECHO_SET_SAMPLING_RATE, &read_impl.samples_per_second);
								flags |= SMBF_WRITE_REPLACE;
							} else {
								ec = cb->write_ec = speex_echo_state_init(read_impl.samples_per_packet, tail);
								speex_echo_ctl(ec, SPEEX_ECHO_SET_SAMPLING_RATE, &read_impl.samples_per_second);
								flags |= SMBF_READ_REPLACE;
							}
							speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_ECHO_STATE, ec);
						}


					} else if (!strcasecmp(var, "echo_suppress")) {
						int db = atoi(val);
						if (db < 0) {
							speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &db);
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Setting ECHO_SUPPRESS on %c to %d [%d]\n", rw, db,
											  r);
						} else {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Syntax error echo_suppress should be in -db\n");
						}
					} else {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Warning unknown parameter [%s] \n", var);
					}
				}
			}

			if (mutex)
				switch_mutex_unlock(mutex);

			if (err) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Syntax error parsing preprocessor commands\n");
			}

		} else {
			break;
		}
	}


	switch_safe_free(mydata);

	if (update) {
		return SWITCH_STATUS_SUCCESS;
	}


	if ((status = switch_core_media_bug_add(session, "preprocess", NULL,
											preprocess_callback, cb, to, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error adding media bug.\n");
		if (cb->read_st) {
			speex_preprocess_state_destroy(cb->read_st);
		}

		if (cb->write_st) {
			speex_preprocess_state_destroy(cb->write_st);
		}

		if (cb->read_ec) {
			speex_echo_state_destroy(cb->read_ec);
		}

		if (cb->write_ec) {
			speex_echo_state_destroy(cb->write_ec);
		}

		return status;
	}

	switch_channel_set_private(channel, "_preprocess", cb);

	return SWITCH_STATUS_SUCCESS;
}


typedef struct {
	switch_core_session_t *session;
	int mute;
	int read_level;
	int write_level;
	int read_mute;
	int write_mute;
} switch_session_audio_t;

static switch_bool_t session_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_session_audio_t *pvt = (switch_session_audio_t *) user_data;
	switch_frame_t *frame = NULL;
	int level = 0, mute = 0;


	if (type == SWITCH_ABC_TYPE_READ_REPLACE || type == SWITCH_ABC_TYPE_WRITE_REPLACE) {
		if (!(pvt->read_level || pvt->write_level || pvt->read_mute || pvt->write_mute)) {
			switch_channel_set_private(switch_core_session_get_channel(pvt->session), "__audio", NULL);
			return SWITCH_FALSE;
		}
	}

	if (type == SWITCH_ABC_TYPE_READ_REPLACE) {
		level = pvt->read_level;
		mute = pvt->read_mute;
		frame = switch_core_media_bug_get_read_replace_frame(bug);
	} else if (type == SWITCH_ABC_TYPE_WRITE_REPLACE) {
		level = pvt->write_level;
		mute = pvt->write_mute;
		frame = switch_core_media_bug_get_write_replace_frame(bug);
	}

	if (frame) {
		if (mute) {
			if (mute > 1) {
				switch_generate_sln_silence(frame->data, frame->datalen / 2, mute);
			} else {
				memset(frame->data, 0, frame->datalen);
			}
		} else if (level) {
			switch_change_sln_volume(frame->data, frame->datalen / 2, level);
		}

		if (type == SWITCH_ABC_TYPE_READ_REPLACE) {
			switch_core_media_bug_set_read_replace_frame(bug, frame);
		} else if (type == SWITCH_ABC_TYPE_WRITE_REPLACE) {
			switch_core_media_bug_set_write_replace_frame(bug, frame);
		}
	}

	return SWITCH_TRUE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_stop_session_audio(switch_core_session_t *session)
{
	switch_media_bug_t *bug;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if ((bug = switch_channel_get_private(channel, "__audio"))) {
		switch_channel_set_private(channel, "__audio", NULL);
		switch_core_media_bug_remove(session, &bug);
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_session_audio(switch_core_session_t *session, const char *cmd, const char *direction, int level)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_session_audio_t *pvt;
	switch_codec_implementation_t read_impl = { 0 };
	int existing = 0, c_read = 0, c_write = 0, flags = SMBF_NO_PAUSE;

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	switch_core_session_get_read_impl(session, &read_impl);


	if ((bug = switch_channel_get_private(channel, "__audio"))) {
		pvt = switch_core_media_bug_get_user_data(bug);
		existing = 1;
	} else {
		if (!(pvt = switch_core_session_alloc(session, sizeof(*pvt)))) {
			return SWITCH_STATUS_MEMERR;
		}

		pvt->session = session;
	}


	if (!strcasecmp(direction, "write")) {
		flags = SMBF_WRITE_REPLACE;
		c_write = 1;
	} else if (!strcasecmp(direction, "read")) {
		flags = SMBF_READ_REPLACE;
		c_read = 1;
	} else if (!strcasecmp(direction, "both")) {
		flags = SMBF_READ_REPLACE | SMBF_WRITE_REPLACE;
		c_read = c_write = 1;
	}


	if (!strcasecmp(cmd, "mute")) {
		if (c_read) {
			pvt->read_mute = level;
			pvt->read_level = 0;
		}
		if (c_write) {
			pvt->write_mute = level;
			pvt->write_level = 0;
		}
	} else if (!strcasecmp(cmd, "level")) {
		if (level < 5 && level > -5) {
			if (c_read) {
				pvt->read_level = level;
			}
			if (c_write) {
				pvt->write_level = level;
			}
		}
	}

	if (existing) {
		switch_core_media_bug_set_flag(bug, flags);
	} else {
		if ((status = switch_core_media_bug_add(session, "audio", cmd,
												session_audio_callback, pvt, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
			return status;
		}

		switch_channel_set_private(channel, "__audio", bug);
	}


	return SWITCH_STATUS_SUCCESS;
}


typedef struct {
	switch_core_session_t *session;
	teletone_dtmf_detect_state_t dtmf_detect;
} switch_inband_dtmf_t;

static switch_bool_t inband_dtmf_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_inband_dtmf_t *pvt = (switch_inband_dtmf_t *) user_data;
	switch_frame_t *frame = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(pvt->session);
	teletone_hit_type_t hit;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		break;
	case SWITCH_ABC_TYPE_READ_REPLACE:
		if ((frame = switch_core_media_bug_get_read_replace_frame(bug))) {
			if ((hit = teletone_dtmf_detect(&pvt->dtmf_detect, frame->data, frame->samples)) == TT_HIT_END) {
				switch_dtmf_t dtmf = {0};

				teletone_dtmf_get(&pvt->dtmf_detect, &dtmf.digit, &dtmf.duration);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_DEBUG, "DTMF DETECTED: [%c][%d]\n",
								  dtmf.digit, dtmf.duration);
				dtmf.source = SWITCH_DTMF_INBAND_AUDIO;
				switch_channel_queue_dtmf(channel, &dtmf);
			}
			switch_core_media_bug_set_read_replace_frame(bug, frame);
		}
		break;
	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_stop_inband_dtmf_session(switch_core_session_t *session)
{
	switch_media_bug_t *bug;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if ((bug = switch_channel_get_private(channel, "dtmf"))) {
		switch_channel_set_private(channel, "dtmf", NULL);
		switch_core_media_bug_remove(session, &bug);
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_inband_dtmf_session(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_inband_dtmf_t *pvt;
	switch_codec_implementation_t read_impl = { 0 };

	switch_core_session_get_read_impl(session, &read_impl);

	if (!(pvt = switch_core_session_alloc(session, sizeof(*pvt)))) {
		return SWITCH_STATUS_MEMERR;
	}

	teletone_dtmf_detect_init(&pvt->dtmf_detect, read_impl.actual_samples_per_second);

	pvt->session = session;


	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if ((status = switch_core_media_bug_add(session, "inband_dtmf", NULL,
											inband_dtmf_callback, pvt, 0, SMBF_READ_REPLACE | SMBF_NO_PAUSE, &bug)) != SWITCH_STATUS_SUCCESS) {
		return status;
	}

	switch_channel_set_private(channel, "dtmf", bug);

	return SWITCH_STATUS_SUCCESS;
}

typedef struct {
	switch_core_session_t *session;
	teletone_generation_session_t ts;
	switch_queue_t *digit_queue;
	switch_buffer_t *audio_buffer;
	switch_mutex_t *mutex;
	int read;
	int ready;
	int skip;
} switch_inband_dtmf_generate_t;

static int teletone_dtmf_generate_handler(teletone_generation_session_t *ts, teletone_tone_map_t *map)
{
	switch_buffer_t *audio_buffer = ts->user_data;
	int wrote;

	if (!audio_buffer) {
		return -1;
	}

	wrote = teletone_mux_tones(ts, map);
	switch_buffer_write(audio_buffer, ts->buffer, wrote * 2);

	return 0;
}

static switch_status_t generate_on_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf, switch_dtmf_direction_t direction)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, "dtmf_generate");
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (bug) {
		switch_inband_dtmf_generate_t *pvt = (switch_inband_dtmf_generate_t *) switch_core_media_bug_get_user_data(bug);

		if (pvt) {
			switch_mutex_lock(pvt->mutex);
			
			if (pvt->ready) {
				switch_dtmf_t *dt = NULL;
				switch_zmalloc(dt, sizeof(*dt));
				*dt = *dtmf;
				if (!switch_buffer_inuse(pvt->audio_buffer)) {
					pvt->skip = 10;
				}
				if (switch_queue_trypush(pvt->digit_queue, dt) == SWITCH_STATUS_SUCCESS) {
					switch_event_t *event;

					if (switch_event_create(&event, SWITCH_EVENT_DTMF) == SWITCH_STATUS_SUCCESS) {
						switch_channel_event_set_data(channel, event);
						switch_event_add_header(event, SWITCH_STACK_BOTTOM, "DTMF-Digit", "%c", dtmf->digit);
						switch_event_add_header(event, SWITCH_STACK_BOTTOM, "DTMF-Duration", "%u", dtmf->duration);
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "DTMF-Conversion", "native:inband");
						if (switch_channel_test_flag(channel, CF_DIVERT_EVENTS)) {
							switch_core_session_queue_event(session, &event);
						} else {
							switch_event_fire(&event);
						}
					}
					
					dt = NULL;
					/* 
					   SWITCH_STATUS_FALSE indicates pretend there never was a DTMF
					   since we will be generating it inband now.
					 */
					status = SWITCH_STATUS_FALSE;
				} else {
					free(dt);
				}
			}
			switch_mutex_unlock(pvt->mutex);
		}
	}

	return status;
}


static switch_bool_t inband_dtmf_generate_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_inband_dtmf_generate_t *pvt = (switch_inband_dtmf_generate_t *) user_data;
	switch_frame_t *frame;
	switch_codec_implementation_t read_impl = { 0 };
	switch_core_session_get_read_impl(pvt->session, &read_impl);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		{
			switch_queue_create(&pvt->digit_queue, 100, switch_core_session_get_pool(pvt->session));
			switch_buffer_create_dynamic(&pvt->audio_buffer, 512, 1024, 0);
			teletone_init_session(&pvt->ts, 0, teletone_dtmf_generate_handler, pvt->audio_buffer);
			pvt->ts.rate = read_impl.actual_samples_per_second;
			pvt->ts.channels = 1;
			switch_mutex_init(&pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(pvt->session));
			if (pvt->read) {
				switch_core_event_hook_add_recv_dtmf(pvt->session, generate_on_dtmf);
			} else {
				switch_core_event_hook_add_send_dtmf(pvt->session, generate_on_dtmf);
			}
			switch_mutex_lock(pvt->mutex);
			pvt->ready = 1;
			switch_mutex_unlock(pvt->mutex);
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		{
			switch_mutex_lock(pvt->mutex);
			pvt->ready = 0;
			switch_core_event_hook_remove_recv_dtmf(pvt->session, generate_on_dtmf);
			switch_buffer_destroy(&pvt->audio_buffer);
			teletone_destroy_session(&pvt->ts);
			switch_mutex_unlock(pvt->mutex);
		}
		break;
	case SWITCH_ABC_TYPE_READ_REPLACE:
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		{
			switch_size_t bytes;
			void *pop;
			
			if (pvt->skip) {
				pvt->skip--;
				return SWITCH_TRUE;
			}
			

			switch_mutex_lock(pvt->mutex);

			if (!pvt->ready) {
				switch_mutex_unlock(pvt->mutex);
				return SWITCH_FALSE;
			}

			if (pvt->read) {
				frame = switch_core_media_bug_get_read_replace_frame(bug);
			} else {
				frame = switch_core_media_bug_get_write_replace_frame(bug);
			}

			if (!switch_buffer_inuse(pvt->audio_buffer)) {
				if (switch_queue_trypop(pvt->digit_queue, &pop) == SWITCH_STATUS_SUCCESS) {
					switch_dtmf_t *dtmf = (switch_dtmf_t *) pop;
					

					if (dtmf->source != SWITCH_DTMF_INBAND_AUDIO) {
						char buf[2] = "";
						int duration = dtmf->duration;

						buf[0] = dtmf->digit;
						if (duration > (int)switch_core_max_dtmf_duration(0)) {
							duration = switch_core_default_dtmf_duration(0);
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)),
										  SWITCH_LOG_WARNING, "%s Truncating DTMF duration %d ms to %d ms\n",
											  switch_channel_get_name(switch_core_session_get_channel(pvt->session)), dtmf->duration / 8, duration);
						}
						

						pvt->ts.duration = duration;
						teletone_run(&pvt->ts, buf);
					}
					free(pop);
				}
			}

			if (switch_buffer_inuse(pvt->audio_buffer) && (bytes = switch_buffer_read(pvt->audio_buffer, frame->data, frame->datalen))) {
				if (bytes < frame->datalen) {
					switch_byte_t *dp = frame->data;
					memset(dp + bytes, 0, frame->datalen - bytes);
				}
			}

			if (pvt->read) {
				switch_core_media_bug_set_read_replace_frame(bug, frame);
			} else {
				switch_core_media_bug_set_write_replace_frame(bug, frame);
			}

			switch_mutex_unlock(pvt->mutex);
		}
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_stop_inband_dtmf_generate_session(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, "dtmf_generate");

	if (bug) {
		switch_channel_set_private(channel, "dtmf_generate", NULL);
		switch_core_media_bug_remove(session, &bug);
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;

}

SWITCH_DECLARE(switch_status_t) switch_ivr_inband_dtmf_generate_session(switch_core_session_t *session, switch_bool_t read_stream)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_inband_dtmf_generate_t *pvt;

	if ((status = switch_channel_pre_answer(channel)) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_channel_media_ready(channel) || !switch_core_session_get_read_codec(session)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can not install inband dtmf generate.  Media not enabled on channel\n");
		return status;
	}

	if (!(pvt = switch_core_session_alloc(session, sizeof(*pvt)))) {
		return SWITCH_STATUS_MEMERR;
	}

	pvt->session = session;
	pvt->read = !!read_stream;

	if ((status = switch_core_media_bug_add(session, "inband_dtmf_generate", NULL,
											inband_dtmf_generate_callback, pvt, 0,
											SMBF_NO_PAUSE | (pvt->read ? SMBF_READ_REPLACE : SMBF_WRITE_REPLACE) , &bug)) != SWITCH_STATUS_SUCCESS) {
		return status;
	}

	switch_channel_set_private(channel, "dtmf_generate", bug);

	return SWITCH_STATUS_SUCCESS;
}

#define MAX_TONES 16
typedef struct {
	teletone_multi_tone_t mt;
	char *app;
	char *data;
	char *key;
	teletone_tone_map_t map;
	int up;
	int total_hits;
	int hits;
	int sleep;
	int expires;
	int default_sleep;
	int default_expires;
	int once;
	switch_tone_detect_callback_t callback;
} switch_tone_detect_t;

typedef struct {
	switch_tone_detect_t list[MAX_TONES + 1];
	int index;
	switch_media_bug_t *bug;
	switch_core_session_t *session;
	int bug_running;
	int detect_fax;
} switch_tone_container_t;

static switch_status_t tone_on_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf, switch_dtmf_direction_t direction)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_tone_container_t *cont = switch_channel_get_private(channel, "_tone_detect_");
	int i;

	if (!cont || !cont->detect_fax || dtmf->digit != 'f') {
		return SWITCH_STATUS_SUCCESS;
	}

	i = cont->detect_fax;

	if (cont->list[i].callback) {
		cont->list[i].callback(cont->session, cont->list[i].app, cont->list[i].data);
	} else {
		switch_channel_execute_on(switch_core_session_get_channel(cont->session), SWITCH_CHANNEL_EXECUTE_ON_TONE_DETECT_VARIABLE);
		switch_channel_api_on(switch_core_session_get_channel(cont->session), SWITCH_CHANNEL_API_ON_TONE_DETECT_VARIABLE);

		if (cont->list[i].app) {
			switch_core_session_execute_application_async(cont->session, cont->list[i].app, cont->list[i].data);
		}
	}
		
	return SWITCH_STATUS_SUCCESS;

}


static switch_bool_t tone_detect_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_tone_container_t *cont = (switch_tone_container_t *) user_data;
	switch_frame_t *frame = NULL;
	int i = 0;
	switch_bool_t rval = SWITCH_TRUE;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		if (cont) {
			cont->bug_running = 1;
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		break;
	case SWITCH_ABC_TYPE_READ_REPLACE:
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		{
			int skip = 0;

			if (type == SWITCH_ABC_TYPE_READ_REPLACE) {
				frame = switch_core_media_bug_get_read_replace_frame(bug);
			} else {
				frame = switch_core_media_bug_get_write_replace_frame(bug);
			}

			for (i = 0; i < cont->index; i++) {

				if (cont->list[i].sleep) {
					cont->list[i].sleep--;
					if (cont->list[i].sleep) {
						skip = 1;
					}
				}

				if (cont->list[i].expires) {
					cont->list[i].expires--;
					if (!cont->list[i].expires) {
						cont->list[i].hits = 0;
						cont->list[i].sleep = 0;
						cont->list[i].expires = 0;
					}
				}

				if (!cont->list[i].up)
					skip = 1;

				if (skip)
					return SWITCH_TRUE;

				if (teletone_multi_tone_detect(&cont->list[i].mt, frame->data, frame->samples)) {
					switch_event_t *event;
					cont->list[i].hits++;

					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_DEBUG, "TONE %s HIT %d/%d\n",
									  cont->list[i].key, cont->list[i].hits, cont->list[i].total_hits);
					cont->list[i].sleep = cont->list[i].default_sleep;
					cont->list[i].expires = cont->list[i].default_expires;

					if (cont->list[i].hits >= cont->list[i].total_hits) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_DEBUG, "TONE %s DETECTED\n",
										  cont->list[i].key);
						cont->list[i].up = 0;

						if (cont->list[i].callback) {
							if ((rval = cont->list[i].callback(cont->session, cont->list[i].app, cont->list[i].data)) == SWITCH_TRUE) {
								switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_DEBUG, "Re-enabling %s\n",
												  cont->list[i].key);
								cont->list[i].up = 1;
								cont->list[i].hits = 0;
								cont->list[i].sleep = 0;
								cont->list[i].expires = 0;
							}
						} else {
							switch_channel_execute_on(switch_core_session_get_channel(cont->session), SWITCH_CHANNEL_EXECUTE_ON_TONE_DETECT_VARIABLE);
							if (cont->list[i].app) {
								switch_core_session_execute_application_async(cont->session, cont->list[i].app, cont->list[i].data);
							}
						}

						if (cont->list[i].once) {
							rval = SWITCH_FALSE;
						}

						if (switch_event_create(&event, SWITCH_EVENT_DETECTED_TONE) == SWITCH_STATUS_SUCCESS) {
							switch_event_t *dup;
							switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detected-Tone", cont->list[i].key);

							if (switch_event_dup(&dup, event) == SWITCH_STATUS_SUCCESS) {
								switch_event_fire(&dup);
							}

							if (switch_core_session_queue_event(cont->session, &event) != SWITCH_STATUS_SUCCESS) {
								switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_ERROR,
												  "Event queue failed!\n");
								switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "delivery-failure", "true");
								switch_event_fire(&event);
							}
						}
					}
				}
			}
		}
		break;
	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	if (rval == SWITCH_FALSE) {
		cont->bug_running = 0;
	}

	return rval;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_stop_tone_detect_session(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_tone_container_t *cont = switch_channel_get_private(channel, "_tone_detect_");
	int i = 0;

	if (cont) {
		switch_channel_set_private(channel, "_tone_detect_", NULL);
		for (i = 0; i < cont->index; i++) {
			cont->list[i].up = 0;
		}
		switch_core_media_bug_remove(session, &cont->bug);
		if (cont->detect_fax) {
			cont->detect_fax = 0;
		}
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_tone_detect_session(switch_core_session_t *session,
															   const char *key, const char *tone_spec,
															   const char *flags, time_t timeout,
															   int hits, const char *app, const char *data, switch_tone_detect_callback_t callback)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_status_t status;
	switch_tone_container_t *cont = switch_channel_get_private(channel, "_tone_detect_");
	char *p, *next;
	int i = 0, ok = 0, detect_fax = 0;
	switch_media_bug_flag_t bflags = 0;
	const char *var;
	switch_codec_implementation_t read_impl = { 0 };
	switch_core_session_get_read_impl(session, &read_impl);


	if (zstr(key)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "No Key Specified!\n");
		return SWITCH_STATUS_FALSE;
	}

	if (cont) {
		if (cont->index >= MAX_TONES) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Max Tones Reached!\n");
			return SWITCH_STATUS_FALSE;
		}

		for (i = 0; i < cont->index; i++) {
			if (!zstr(cont->list[i].key) && !strcasecmp(key, cont->list[i].key)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Re-enabling %s\n", key);
				cont->list[i].up = 1;
				cont->list[i].hits = 0;
				cont->list[i].sleep = 0;
				cont->list[i].expires = 0;
				return SWITCH_STATUS_SUCCESS;
			}
		}
	}

	if (zstr(tone_spec)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "No Spec Specified!\n");
		return SWITCH_STATUS_FALSE;
	}

	if (!cont && !(cont = switch_core_session_alloc(session, sizeof(*cont)))) {
		return SWITCH_STATUS_MEMERR;
	}

	if ((var = switch_channel_get_variable(channel, "tone_detect_hits"))) {
		int tmp = atoi(var);
		if (tmp > 0) {
			hits = tmp;
		}
	}

	if (!hits) hits = 1;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Adding tone spec %s index %d hits %d\n", tone_spec, cont->index, hits);

	i = 0;
	p = (char *) tone_spec;

	do {
		teletone_process_t this;
		next = strchr(p, ',');
		while (*p == ' ')
			p++;
		if ((this = (teletone_process_t) atof(p))) {
			ok++;
			cont->list[cont->index].map.freqs[i++] = this;
		}
		if (!strncasecmp(p, "1100", 4)) {
			detect_fax = cont->index;
		}

		if (next) {
			p = next + 1;
		}
	} while (next);
	cont->list[cont->index].map.freqs[i++] = 0;

	if (!ok) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid tone spec!\n");
		return SWITCH_STATUS_FALSE;
	}

	cont->detect_fax = detect_fax;

	cont->list[cont->index].key = switch_core_session_strdup(session, key);

	if (app) {
		cont->list[cont->index].app = switch_core_session_strdup(session, app);
	}

	if (data) {
		cont->list[cont->index].data = switch_core_session_strdup(session, data);
	}

	cont->list[cont->index].callback = callback;

	if (!hits)
		hits = 1;

	cont->list[cont->index].hits = 0;
	cont->list[cont->index].total_hits = hits;

	cont->list[cont->index].up = 1;
	memset(&cont->list[cont->index].mt, 0, sizeof(cont->list[cont->index].mt));
	cont->list[cont->index].mt.sample_rate = read_impl.actual_samples_per_second;
	teletone_multi_tone_init(&cont->list[cont->index].mt, &cont->list[cont->index].map);
	cont->session = session;

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	cont->list[cont->index].default_sleep = 25;
	cont->list[cont->index].default_expires = 250;

	if ((var = switch_channel_get_variable(channel, "tone_detect_sleep"))) {
		int tmp = atoi(var);
		if (tmp > 0) {
			cont->list[cont->index].default_sleep = tmp;
		}
	}

	if ((var = switch_channel_get_variable(channel, "tone_detect_expires"))) {
		int tmp = atoi(var);
		if (tmp > 0) {
			cont->list[cont->index].default_expires = tmp;
		}
	}


	if (zstr(flags)) {
		bflags = SMBF_READ_REPLACE;
	} else {
		if (strchr(flags, 'o')) {
			cont->list[cont->index].once = 1;
		}

		if (strchr(flags, 'r')) {
			bflags |= SMBF_READ_REPLACE;
		} else if (strchr(flags, 'w')) {
			bflags |= SMBF_WRITE_REPLACE;
		}
	}

	bflags |= SMBF_NO_PAUSE;

	if (cont->bug_running) {
		status = SWITCH_STATUS_SUCCESS;
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s bug already running\n", switch_channel_get_name(channel));
	} else {
		cont->bug_running = 1;
		if (cont->detect_fax) {
			switch_core_event_hook_add_send_dtmf(session, tone_on_dtmf);
			switch_core_event_hook_add_recv_dtmf(session, tone_on_dtmf);
		}

		if ((status = switch_core_media_bug_add(session, "tone_detect", key,
												tone_detect_callback, cont, timeout, bflags, &cont->bug)) != SWITCH_STATUS_SUCCESS) {
			cont->bug_running = 0;
			return status;
		}
		switch_channel_set_private(channel, "_tone_detect_", cont);
	}

	cont->index++;

	return SWITCH_STATUS_SUCCESS;
}

typedef struct {
	const char *app;
	uint32_t flags;
	switch_bind_flag_t bind_flags;
} dtmf_meta_app_t;

typedef struct {
	dtmf_meta_app_t map[14];
	time_t last_digit;
	switch_bool_t meta_on;
	char meta;
	int up;
} dtmf_meta_settings_t;

typedef struct {
	dtmf_meta_settings_t sr[3];
} dtmf_meta_data_t;

#define SWITCH_META_VAR_KEY "__dtmf_meta"
#define SWITCH_BLOCK_DTMF_KEY "__dtmf_block"

typedef struct {
	switch_core_session_t *session;
	const char *app;
	int flags;
} bch_t;

static void *SWITCH_THREAD_FUNC bcast_thread(switch_thread_t *thread, void *obj)
{
	bch_t *bch = (bch_t *) obj;

	if (!bch->session) {
		return NULL;
	}

	switch_core_session_read_lock(bch->session);
	switch_ivr_broadcast(switch_core_session_get_uuid(bch->session), bch->app, bch->flags);
	switch_core_session_rwunlock(bch->session);

	return NULL;

}
SWITCH_DECLARE(void) switch_ivr_broadcast_in_thread(switch_core_session_t *session, const char *app, int flags)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
	switch_memory_pool_t *pool;
	bch_t *bch;

	switch_assert(session);

	pool = switch_core_session_get_pool(session);

	bch = switch_core_session_alloc(session, sizeof(*bch));
	bch->session = session;
	bch->app = app;
	bch->flags = flags;


	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&thread, thd_attr, bcast_thread, bch, pool);
}

static switch_status_t meta_on_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf, switch_dtmf_direction_t direction)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	dtmf_meta_data_t *md = switch_channel_get_private(channel, SWITCH_META_VAR_KEY);
	time_t now = switch_epoch_time_now(NULL);
	char digit[2] = "";
	int dval;

	if (!md || switch_channel_test_flag(channel, CF_INNER_BRIDGE)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (direction == SWITCH_DTMF_RECV && !md->sr[SWITCH_DTMF_RECV].up) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (direction == SWITCH_DTMF_SEND && !md->sr[SWITCH_DTMF_SEND].up) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (md->sr[direction].meta_on && now - md->sr[direction].last_digit > 5) {
		md->sr[direction].meta_on = SWITCH_FALSE;
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s Meta digit timeout parsing %c\n", switch_channel_get_name(channel),
						  dtmf->digit);
		return SWITCH_STATUS_SUCCESS;
	}

	md->sr[direction].last_digit = now;

	if (dtmf->digit == md->sr[direction].meta) {
		if (md->sr[direction].meta_on) {
			md->sr[direction].meta_on = SWITCH_FALSE;
			return SWITCH_STATUS_SUCCESS;
		} else {
			md->sr[direction].meta_on = SWITCH_TRUE;
			return SWITCH_STATUS_FALSE;
		}
	}

	if (md->sr[direction].meta_on) {
		if (is_dtmf(dtmf->digit)) {
			int ok = 0;
			*digit = dtmf->digit;
			dval = switch_dtmftoi(digit);
			
			if (direction == SWITCH_DTMF_RECV && (md->sr[direction].map[dval].bind_flags & SBF_DIAL_ALEG)) {
				ok = 1;
			} else if (direction == SWITCH_DTMF_SEND && (md->sr[direction].map[dval].bind_flags & SBF_DIAL_BLEG)) {
				ok = 1;
			}

			if (ok && md->sr[direction].map[dval].app) {
				uint32_t flags = md->sr[direction].map[dval].flags;

				if ((md->sr[direction].map[dval].bind_flags & SBF_EXEC_OPPOSITE)) {
					if (direction == SWITCH_DTMF_SEND) {
						flags |= SMF_ECHO_ALEG;
					} else {
						flags |= SMF_ECHO_BLEG;
					}
				} else if ((md->sr[direction].map[dval].bind_flags & SBF_EXEC_SAME)) {
					if (direction == SWITCH_DTMF_SEND) {
						flags |= SMF_ECHO_BLEG;
					} else {
						flags |= SMF_ECHO_ALEG;
					}
				} else if ((md->sr[direction].map[dval].bind_flags & SBF_EXEC_ALEG)) {
					flags |= SMF_ECHO_ALEG;
				} else if ((md->sr[direction].map[dval].bind_flags & SBF_EXEC_BLEG)) {
					flags |= SMF_ECHO_BLEG;
				} else {
					flags |= SMF_ECHO_ALEG;
				}

				if ((md->sr[direction].map[dval].bind_flags & SBF_EXEC_INLINE)) {
					flags |= SMF_EXEC_INLINE;
				}

				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Processing meta digit '%c' [%s]\n",
								  switch_channel_get_name(channel), dtmf->digit, md->sr[direction].map[dval].app);

				if (switch_channel_test_flag(channel, CF_PROXY_MODE)) {
					switch_ivr_broadcast_in_thread(session, md->sr[direction].map[dval].app, flags | SMF_REBRIDGE);
				} else {
					switch_ivr_broadcast(switch_core_session_get_uuid(session), md->sr[direction].map[dval].app, flags);
				}

				if ((md->sr[direction].map[dval].bind_flags & SBF_ONCE)) {
					memset(&md->sr[direction].map[dval], 0, sizeof(md->sr[direction].map[dval]));
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Unbinding meta digit '%c'\n",
									  switch_channel_get_name(channel), dtmf->digit);
				}

			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "%s Ignoring meta digit '%c' not mapped\n",
								  switch_channel_get_name(channel), dtmf->digit);

			}
		}
		md->sr[direction].meta_on = SWITCH_FALSE;
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_unbind_dtmf_meta_session(switch_core_session_t *session, uint32_t key)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (key) {
		dtmf_meta_data_t *md = switch_channel_get_private(channel, SWITCH_META_VAR_KEY);

		if (!md || key > 9) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid key %u\n", key);
			return SWITCH_STATUS_FALSE;
		}

		memset(&md->sr[SWITCH_DTMF_RECV].map[key], 0, sizeof(md->sr[SWITCH_DTMF_RECV].map[key]));
		memset(&md->sr[SWITCH_DTMF_SEND].map[key], 0, sizeof(md->sr[SWITCH_DTMF_SEND].map[key]));
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "UnBound A-Leg: %d\n", key);

	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "UnBound A-Leg: ALL\n");
		switch_channel_set_private(channel, SWITCH_META_VAR_KEY, NULL);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t block_on_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf, switch_dtmf_direction_t direction)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	uint8_t enabled = (uint8_t)(intptr_t)switch_channel_get_private(channel, SWITCH_BLOCK_DTMF_KEY);

	if (!enabled || switch_channel_test_flag(channel, CF_INNER_BRIDGE)) {
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_unblock_dtmf_session(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	uint8_t enabled = (uint8_t)(intptr_t)switch_channel_get_private(channel, SWITCH_BLOCK_DTMF_KEY);
	
	if (enabled) {
		switch_channel_set_private(channel, SWITCH_BLOCK_DTMF_KEY, NULL);
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_block_dtmf_session(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	uint8_t enabled = (uint8_t)(intptr_t)switch_channel_get_private(channel, SWITCH_BLOCK_DTMF_KEY);

	if (!enabled) {
		switch_channel_set_private(channel, SWITCH_BLOCK_DTMF_KEY, (void *)(intptr_t)1);
		switch_core_event_hook_add_send_dtmf(session, block_on_dtmf);
		switch_core_event_hook_add_recv_dtmf(session, block_on_dtmf);
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_bind_dtmf_meta_session(switch_core_session_t *session, uint32_t key,
																  switch_bind_flag_t bind_flags, const char *app)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	dtmf_meta_data_t *md = switch_channel_get_private(channel, SWITCH_META_VAR_KEY);
	const char *meta_var = switch_channel_get_variable(channel, "bind_meta_key");
	char meta = '*';
	char str[2] = "";

	if (meta_var) {
		char t_meta = *meta_var;
		if (is_dtmf(t_meta)) {
			meta = t_meta;
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Invalid META KEY %c\n", t_meta);
		}
	}

	if (meta != '*' && meta != '#') {
		str[0] = meta;

		if (switch_dtmftoi(str) == (char)key) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid key %u, same as META CHAR\n", key);
			return SWITCH_STATUS_FALSE;
		}
	}


	if (key > 13) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid key %u\n", key);
		return SWITCH_STATUS_FALSE;
	}

	if (!md) {
		md = switch_core_session_alloc(session, sizeof(*md));
		switch_channel_set_private(channel, SWITCH_META_VAR_KEY, md);
		switch_core_event_hook_add_send_dtmf(session, meta_on_dtmf);
		switch_core_event_hook_add_recv_dtmf(session, meta_on_dtmf);
	}

	if (!zstr(app)) {
		if ((bind_flags & SBF_DIAL_ALEG)) {
			md->sr[SWITCH_DTMF_RECV].meta = meta;
			md->sr[SWITCH_DTMF_RECV].up = 1;
			md->sr[SWITCH_DTMF_RECV].map[key].app = switch_core_session_strdup(session, app);
			md->sr[SWITCH_DTMF_RECV].map[key].flags |= SMF_HOLD_BLEG;
			md->sr[SWITCH_DTMF_RECV].map[key].bind_flags = bind_flags;
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Bound A-Leg: %c%c %s\n", meta, switch_itodtmf((char)key), app);
		}
		if ((bind_flags & SBF_DIAL_BLEG)) {
			md->sr[SWITCH_DTMF_SEND].meta = meta;
			md->sr[SWITCH_DTMF_SEND].up = 1;
			md->sr[SWITCH_DTMF_SEND].map[key].app = switch_core_session_strdup(session, app);
			md->sr[SWITCH_DTMF_SEND].map[key].flags |= SMF_HOLD_BLEG;
			md->sr[SWITCH_DTMF_SEND].map[key].bind_flags = bind_flags;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Bound B-Leg: %c%c %s\n", meta, switch_itodtmf((char)key), app);
		}

	} else {
		if ((bind_flags & SBF_DIAL_ALEG)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "UnBound A-Leg: %c%c\n", meta, switch_itodtmf((char)key));
			md->sr[SWITCH_DTMF_SEND].map[key].app = NULL;
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "UnBound: B-Leg %c%d\n", meta, key);
			md->sr[SWITCH_DTMF_SEND].map[key].app = NULL;
		}
	}

	return SWITCH_STATUS_SUCCESS;
}



struct speech_thread_handle {
	switch_core_session_t *session;
	switch_asr_handle_t *ah;
	switch_media_bug_t *bug;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	switch_memory_pool_t *pool;
	int ready;
};

static void *SWITCH_THREAD_FUNC speech_thread(switch_thread_t *thread, void *obj)
{
	struct speech_thread_handle *sth = (struct speech_thread_handle *) obj;
	switch_channel_t *channel = switch_core_session_get_channel(sth->session);
	switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
	switch_status_t status;

	switch_thread_cond_create(&sth->cond, sth->pool);
	switch_mutex_init(&sth->mutex, SWITCH_MUTEX_NESTED, sth->pool);

	if (switch_core_session_read_lock(sth->session) != SWITCH_STATUS_SUCCESS) {
		sth->ready = 0;
		return NULL;
	}

	switch_mutex_lock(sth->mutex);

	sth->ready = 1;

	while (switch_channel_up(channel) && !switch_test_flag(sth->ah, SWITCH_ASR_FLAG_CLOSED)) {
		char *xmlstr = NULL;

		switch_thread_cond_wait(sth->cond, sth->mutex);

		if (switch_channel_down(channel) || switch_test_flag(sth->ah, SWITCH_ASR_FLAG_CLOSED)) {
			break;
		}

		if (switch_core_asr_check_results(sth->ah, &flags) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *event;

			status = switch_core_asr_get_results(sth->ah, &xmlstr, &flags);

			if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
				goto done;
			}

			if (status == SWITCH_STATUS_SUCCESS && switch_true(switch_channel_get_variable(channel, "asr_intercept_dtmf"))) {
				const char *p;
				
				if ((p = switch_stristr("<input>", xmlstr))) {
					p += 7;
				}

				while (p && *p) {
					char c;

					if (*p == '<') {
						break;
					}

					if (!strncasecmp(p, "pound", 5)) {
						c = '#';
						p += 5;
					} else if (!strncasecmp(p, "hash", 4)) {
						c = '#';
						p += 4;
					} else if (!strncasecmp(p, "star", 4)) {
						c = '*';
						p += 4;
					} else if (!strncasecmp(p, "asterisk", 8)) {
						c = '*';
						p += 8;
					} else {
						c = *p;
						p++;
					}

					if (is_dtmf(c)) {
						switch_dtmf_t dtmf = {0};
						dtmf.digit = c;
						dtmf.duration = switch_core_default_dtmf_duration(0);
						dtmf.source = SWITCH_DTMF_INBAND_AUDIO;
						switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG, "Queue speech detected dtmf %c\n", c);
						switch_channel_queue_dtmf(channel, &dtmf);
					}

				}
				switch_ivr_resume_detect_speech(sth->session);
			}

			if (switch_event_create(&event, SWITCH_EVENT_DETECTED_SPEECH) == SWITCH_STATUS_SUCCESS) {
				if (status == SWITCH_STATUS_SUCCESS) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Speech-Type", "detected-speech");
					switch_event_add_body(event, "%s", xmlstr);
				} else {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Speech-Type", "begin-speaking");
				}

				if (switch_test_flag(sth->ah, SWITCH_ASR_FLAG_FIRE_EVENTS)) {
					switch_event_t *dup;

					if (switch_event_dup(&dup, event) == SWITCH_STATUS_SUCCESS) {
						switch_channel_event_set_data(channel, dup);
						switch_event_fire(&dup);
					}

				}

				if (switch_core_session_queue_event(sth->session, &event) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_ERROR, "Event queue failed!\n");
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "delivery-failure", "true");
					switch_event_fire(&event);
				}
			}

			switch_safe_free(xmlstr);
		}
	}
  done:

	switch_mutex_unlock(sth->mutex);
	switch_core_session_rwunlock(sth->session);

	return NULL;
}

static switch_bool_t speech_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	struct speech_thread_handle *sth = (struct speech_thread_handle *) user_data;
	uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
	switch_frame_t frame = { 0 };
	switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;

	frame.data = data;
	frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:{
			switch_thread_t *thread;
			switch_threadattr_t *thd_attr = NULL;

			switch_threadattr_create(&thd_attr, sth->pool);
			switch_threadattr_detach_set(thd_attr, 1);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			switch_thread_create(&thread, thd_attr, speech_thread, sth, sth->pool);
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:{
			switch_core_asr_close(sth->ah, &flags);
			if (sth->mutex && sth->cond && sth->ready) {
				switch_mutex_lock(sth->mutex);
				switch_thread_cond_signal(sth->cond);
				switch_mutex_unlock(sth->mutex);
			}
		}
		break;
	case SWITCH_ABC_TYPE_READ:
		if (sth->ah) {
			if (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
				if (switch_core_asr_feed(sth->ah, frame.data, frame.datalen, &flags) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_DEBUG, "Error Feeding Data\n");
					return SWITCH_FALSE;
				}
				if (switch_core_asr_check_results(sth->ah, &flags) == SWITCH_STATUS_SUCCESS) {
					if (sth->mutex && sth->cond && sth->ready) {
						switch_mutex_lock(sth->mutex);
						switch_thread_cond_signal(sth->cond);
						switch_mutex_unlock(sth->mutex);
					}
				}
			}
		}
		break;
	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t speech_on_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf, switch_dtmf_direction_t direction)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;

	if (sth) {
		if (switch_core_asr_feed_dtmf(sth->ah, dtmf, &flags) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error Feeding DTMF\n");
		}
	}

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_stop_detect_speech(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth;

	switch_assert(channel != NULL);
	if ((sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY))) {
		switch_channel_set_private(channel, SWITCH_SPEECH_KEY, NULL);
		switch_core_event_hook_remove_recv_dtmf(session, speech_on_dtmf);
		switch_core_media_bug_remove(session, &sth->bug);
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_pause_detect_speech(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);

	if (sth) {
		switch_core_asr_pause(sth->ah);
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_resume_detect_speech(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);

	if (sth) {
		switch_core_asr_resume(sth->ah);
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_detect_speech_load_grammar(switch_core_session_t *session, char *grammar, char *name)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);
	switch_status_t status;

	if (sth) {
		if ((status = switch_core_asr_load_grammar(sth->ah, grammar, name)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Error loading Grammar\n");
			switch_ivr_stop_detect_speech(session);
		}
		return status;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_set_param_detect_speech(switch_core_session_t *session, const char *name, const char *val) 
{
	struct speech_thread_handle *sth = switch_channel_get_private(switch_core_session_get_channel(session), SWITCH_SPEECH_KEY);
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (sth && sth->ah && name && val) {
		switch_core_asr_text_param(sth->ah, (char *) name, val);
		status = SWITCH_STATUS_SUCCESS;
	}

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_detect_speech_start_input_timers(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);

	if (sth) {
		switch_core_asr_start_input_timers(sth->ah);
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_detect_speech_unload_grammar(switch_core_session_t *session, const char *name)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);
	switch_status_t status;

	if (sth) {
		if ((status = switch_core_asr_unload_grammar(sth->ah, name)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Error unloading Grammar\n");
			switch_ivr_stop_detect_speech(session);
		}
		return status;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_detect_speech_enable_grammar(switch_core_session_t *session, const char *name)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);
	switch_status_t status;

	if (sth) {
		if ((status = switch_core_asr_enable_grammar(sth->ah, name)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Error enabling Grammar\n");
			switch_ivr_stop_detect_speech(session);
		}
		return status;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_detect_speech_disable_grammar(switch_core_session_t *session, const char *name)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);
	switch_status_t status;

	if (sth) {
		if ((status = switch_core_asr_disable_grammar(sth->ah, name)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Error disabling Grammar\n");
			switch_ivr_stop_detect_speech(session);
		}
		return status;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_detect_speech_disable_all_grammars(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);
	switch_status_t status;

	if (sth) {
		if ((status = switch_core_asr_disable_all_grammars(sth->ah)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Error disabling all Grammars\n");
			switch_ivr_stop_detect_speech(session);
		}
		return status;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_detect_speech(switch_core_session_t *session,
														 const char *mod_name,
														 const char *grammar, const char *name, const char *dest, switch_asr_handle_t *ah)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_status_t status;
	switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
	struct speech_thread_handle *sth = switch_channel_get_private(channel, SWITCH_SPEECH_KEY);
	switch_codec_implementation_t read_impl = { 0 };
	const char *p;
	char key[512] = "";

	switch_core_session_get_read_impl(session, &read_impl);

	switch_snprintf(key, sizeof(key), "%s/%s/%s/%s", mod_name, grammar, name, dest);

	if (!ah) {
		if (!(ah = switch_core_session_alloc(session, sizeof(*ah)))) {
			return SWITCH_STATUS_MEMERR;
		}
	}

	if (sth) {
		if (switch_core_asr_load_grammar(sth->ah, grammar, name) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Error loading Grammar\n");
			switch_ivr_stop_detect_speech(session);
			return SWITCH_STATUS_FALSE;
		}

		if ((p = switch_channel_get_variable(channel, "fire_asr_events")) && switch_true(p)) {
			switch_set_flag(sth->ah, SWITCH_ASR_FLAG_FIRE_EVENTS);
		}

		return SWITCH_STATUS_SUCCESS;
	}

	if ((status = switch_core_asr_open(ah,
									   mod_name,
									   "L16",
									   read_impl.actual_samples_per_second, dest, &flags,
									   switch_core_session_get_pool(session))) == SWITCH_STATUS_SUCCESS) {

		if (switch_core_asr_load_grammar(ah, grammar, name) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Error loading Grammar\n");
			switch_core_asr_close(ah, &flags);
			return SWITCH_STATUS_FALSE;
		}
	} else {
		return status;
	}

	sth = switch_core_session_alloc(session, sizeof(*sth));
	sth->pool = switch_core_session_get_pool(session);
	sth->session = session;
	sth->ah = ah;

	if ((p = switch_channel_get_variable(channel, "fire_asr_events")) && switch_true(p)) {
		switch_set_flag(ah, SWITCH_ASR_FLAG_FIRE_EVENTS);
	}

	if ((status = switch_core_media_bug_add(session, "detect_speech", key,
											speech_callback, sth, 0, SMBF_READ_STREAM | SMBF_NO_PAUSE, &sth->bug)) != SWITCH_STATUS_SUCCESS) {
		switch_core_asr_close(ah, &flags);
		return status;
	}

	if ((status = switch_core_event_hook_add_recv_dtmf(session, speech_on_dtmf)) != SWITCH_STATUS_SUCCESS) {
		switch_ivr_stop_detect_speech(session);
		return status;
	}

	switch_channel_set_private(channel, SWITCH_SPEECH_KEY, sth);

	return SWITCH_STATUS_SUCCESS;
}

struct hangup_helper {
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	switch_bool_t bleg;
	switch_call_cause_t cause;
};

SWITCH_STANDARD_SCHED_FUNC(sch_hangup_callback)
{
	struct hangup_helper *helper;
	switch_core_session_t *session, *other_session;
	const char *other_uuid;

	switch_assert(task);

	helper = (struct hangup_helper *) task->cmd_arg;

	if ((session = switch_core_session_locate(helper->uuid_str))) {
		switch_channel_t *channel = switch_core_session_get_channel(session);

		if (helper->bleg) {
			if ((other_uuid = switch_channel_get_variable(channel, SWITCH_BRIDGE_VARIABLE)) && (other_session = switch_core_session_locate(other_uuid))) {
				switch_channel_t *other_channel = switch_core_session_get_channel(other_session);
				switch_channel_hangup(other_channel, helper->cause);
				switch_core_session_rwunlock(other_session);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "No channel to hangup\n");
			}
		} else {
			switch_channel_hangup(channel, helper->cause);
		}

		switch_core_session_rwunlock(session);
	}
}

SWITCH_DECLARE(uint32_t) switch_ivr_schedule_hangup(time_t runtime, const char *uuid, switch_call_cause_t cause, switch_bool_t bleg)
{
	struct hangup_helper *helper;
	size_t len = sizeof(*helper);

	switch_zmalloc(helper, len);

	switch_copy_string(helper->uuid_str, uuid, sizeof(helper->uuid_str));
	helper->cause = cause;
	helper->bleg = bleg;

	return switch_scheduler_add_task(runtime, sch_hangup_callback, (char *) __SWITCH_FUNC__, uuid, 0, helper, SSHF_FREE_ARG);
}

struct transfer_helper {
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char *extension;
	char *dialplan;
	char *context;
};

SWITCH_STANDARD_SCHED_FUNC(sch_transfer_callback)
{
	struct transfer_helper *helper;
	switch_core_session_t *session;

	switch_assert(task);

	helper = (struct transfer_helper *) task->cmd_arg;

	if ((session = switch_core_session_locate(helper->uuid_str))) {
		switch_ivr_session_transfer(session, helper->extension, helper->dialplan, helper->context);
		switch_core_session_rwunlock(session);
	}

}

SWITCH_DECLARE(uint32_t) switch_ivr_schedule_transfer(time_t runtime, const char *uuid, char *extension, char *dialplan, char *context)
{
	struct transfer_helper *helper;
	size_t len = sizeof(*helper);
	char *cur = NULL;

	if (extension) {
		len += strlen(extension) + 1;
	}

	if (dialplan) {
		len += strlen(dialplan) + 1;
	}

	if (context) {
		len += strlen(context) + 1;
	}

	switch_zmalloc(cur, len);
	helper = (struct transfer_helper *) cur;

	switch_copy_string(helper->uuid_str, uuid, sizeof(helper->uuid_str));

	cur += sizeof(*helper);

	if (extension) {
		switch_copy_string(cur, extension, strlen(extension) + 1);
		helper->extension = cur;
		cur += strlen(helper->extension) + 1;
	}

	if (dialplan) {
		switch_copy_string(cur, dialplan, strlen(dialplan) + 1);
		helper->dialplan = cur;
		cur += strlen(helper->dialplan) + 1;
	}

	if (context) {
		switch_copy_string(cur, context, strlen(context) + 1);
		helper->context = cur;
	}

	return switch_scheduler_add_task(runtime, sch_transfer_callback, (char *) __SWITCH_FUNC__, uuid, 0, helper, SSHF_FREE_ARG);
}

struct broadcast_helper {
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char *path;
	switch_media_flag_t flags;
};

SWITCH_STANDARD_SCHED_FUNC(sch_broadcast_callback)
{
	struct broadcast_helper *helper;
	switch_assert(task);

	helper = (struct broadcast_helper *) task->cmd_arg;
	switch_ivr_broadcast(helper->uuid_str, helper->path, helper->flags);
}

SWITCH_DECLARE(uint32_t) switch_ivr_schedule_broadcast(time_t runtime, const char *uuid, const char *path, switch_media_flag_t flags)
{
	struct broadcast_helper *helper;
	size_t len = sizeof(*helper) + strlen(path) + 1;
	char *cur = NULL;

	switch_zmalloc(cur, len);
	helper = (struct broadcast_helper *) cur;

	cur += sizeof(*helper);
	switch_copy_string(helper->uuid_str, uuid, sizeof(helper->uuid_str));
	helper->flags = flags;

	switch_copy_string(cur, path, len - sizeof(helper));
	helper->path = cur;

	return switch_scheduler_add_task(runtime, sch_broadcast_callback, (char *) __SWITCH_FUNC__, uuid, 0, helper, SSHF_FREE_ARG);
}

SWITCH_DECLARE(switch_status_t) switch_ivr_broadcast(const char *uuid, const char *path, switch_media_flag_t flags)
{
	switch_channel_t *channel;
	switch_core_session_t *session, *master;
	switch_event_t *event;
	switch_core_session_t *other_session = NULL;
	const char *other_uuid = NULL;
	char *app = "playback";
	char *cause = NULL;
	char *mypath;
	char *p;
	int app_flags = 0, nomedia = 0;

	switch_assert(path);

	if (!(master = session = switch_core_session_locate(uuid))) {
		return SWITCH_STATUS_FALSE;
	}

	channel = switch_core_session_get_channel(session);

	mypath = strdup(path);
	assert(mypath);

	if ((p = strchr(mypath, ':')) && *(p + 1) == ':') {
		app = mypath;
		*p++ = '\0';
		*p++ = '\0';
		path = p;
	}

	if (switch_channel_test_flag(channel, CF_PROXY_MODE)) {
		nomedia = 1;
		switch_ivr_media(uuid, SMF_REBRIDGE);
	}

	if ((cause = strchr(app, '!'))) {
		*cause++ = '\0';
		if (!cause) {
			cause = "normal_clearing";
		}
	}

	if ((flags & SMF_ECHO_BLEG) && (other_uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))
		&& (other_session = switch_core_session_locate(other_uuid))) {
		if ((flags & SMF_EXEC_INLINE)) {
			switch_core_session_execute_application_get_flags(other_session, app, path, &app_flags);
			nomedia = 0;
		} else {
			switch_core_session_get_app_flags(app, &app_flags);
			if (switch_event_create(&event, SWITCH_EVENT_COMMAND) == SWITCH_STATUS_SUCCESS) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", app);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", path);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, (flags & SMF_PRIORITY) ? "event-lock-pri" : "event-lock", "true");

				switch_event_add_header(event, SWITCH_STACK_BOTTOM, "lead-frames", "%d", 5);

				if ((flags & SMF_LOOP)) {
					switch_event_add_header(event, SWITCH_STACK_BOTTOM, "loops", "%d", -1);
				}

				if ((flags & SMF_HOLD_BLEG)) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "hold-bleg", "true");
				}

				switch_core_session_queue_private_event(other_session, &event, (flags & SMF_PRIORITY));
			}
		}

		switch_core_session_rwunlock(other_session);
		master = other_session;
		other_session = NULL;
	}

	if ((app_flags & SAF_MEDIA_TAP)) {
		nomedia = 0;
	}

	if ((flags & SMF_ECHO_ALEG)) {
		if ((flags & SMF_EXEC_INLINE)) {
			nomedia = 0;
			switch_core_session_execute_application(session, app, path);
		} else {
			if (switch_event_create(&event, SWITCH_EVENT_COMMAND) == SWITCH_STATUS_SUCCESS) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", app);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", path);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, (flags & SMF_PRIORITY) ? "event-lock-pri" : "event-lock", "true");
				switch_event_add_header(event, SWITCH_STACK_BOTTOM, "lead-frames", "%d", 5);

				if ((flags & SMF_LOOP)) {
					switch_event_add_header(event, SWITCH_STACK_BOTTOM, "loops", "%d", -1);
				}
				if ((flags & SMF_HOLD_BLEG)) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "hold-bleg", "true");
				}

				switch_core_session_queue_private_event(session, &event, (flags & SMF_PRIORITY));

				if (nomedia)
					switch_channel_set_flag(channel, CF_BROADCAST_DROP_MEDIA);
			}
		}
		master = session;
	}

	if (cause) {
		if (switch_event_create(&event, SWITCH_EVENT_COMMAND) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "hangup");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", cause);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, (flags & SMF_PRIORITY) ? "event-lock-pri" : "event-lock", "true");
			switch_core_session_queue_private_event(session, &event, (flags & SMF_PRIORITY));
		}
	}

	switch_core_session_rwunlock(session);
	switch_safe_free(mypath);

	return SWITCH_STATUS_SUCCESS;
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
