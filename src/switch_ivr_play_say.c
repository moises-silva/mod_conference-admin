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
 * Paul D. Tinsley <pdt at jackhammer.org>
 * Neal Horman <neal at wanlink dot com>
 * Matt Klein <mklein@nmedia.net>
 * Michael Jerris <mike@jerris.com>
 * Marc Olivier Chouinard <mochouinard@moctel.com>
 *
 * switch_ivr_play_say.c -- IVR Library (functions to play or say audio)
 *
 */

#include <switch.h>

SWITCH_DECLARE(switch_status_t) switch_ivr_phrase_macro_event(switch_core_session_t *session, const char *macro_name, const char *data, switch_event_t *event, const char *lang,
														switch_input_args_t *args)
{
	switch_event_t *hint_data;
	switch_xml_t cfg, xml = NULL, language = NULL, macros = NULL, phrases = NULL, macro, input, action;
	switch_status_t status = SWITCH_STATUS_GENERR;
	const char *old_sound_prefix = NULL, *sound_path = NULL, *tts_engine = NULL, *tts_voice = NULL;
	const char *module_name = NULL, *chan_lang = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	uint8_t done = 0;
	int matches = 0;
	const char *pause_val;
	int pause = 100;
	const char *group_macro_name = NULL;
	const char *local_macro_name = macro_name;
	switch_bool_t sound_prefix_enforced = switch_true(switch_channel_get_variable(channel, "sound_prefix_enforced"));
	switch_bool_t local_sound_prefix_enforced = SWITCH_FALSE;

	if (!macro_name) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "No phrase macro specified.\n");
		return status;
	}

	if (!lang) {
		chan_lang = switch_channel_get_variable(channel, "default_language");
		if (!chan_lang) {
			chan_lang = "en";
		}
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "No language specified - Using [%s]\n", chan_lang);
	} else {
		chan_lang = lang;
	}

	switch_event_create(&hint_data, SWITCH_EVENT_REQUEST_PARAMS);
	switch_assert(hint_data);

	switch_event_add_header_string(hint_data, SWITCH_STACK_BOTTOM, "macro_name", macro_name);
	switch_event_add_header_string(hint_data, SWITCH_STACK_BOTTOM, "lang", chan_lang);
	if (data) {
		switch_event_add_header_string(hint_data, SWITCH_STACK_BOTTOM, "data", data);
		if (event) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "data", data);
		}
	} else {
		data = "";
	}
	switch_channel_event_set_data(channel, hint_data);

	if (switch_xml_locate_language(&xml, &cfg, hint_data, &language, &phrases, &macros, chan_lang) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	if ((module_name = switch_xml_attr(language, "say-module"))) {
	} else if ((module_name = switch_xml_attr(language, "module"))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Deprecated usage of module attribute. Use say-module instead\n");
	} else {
		module_name = chan_lang;
	}

	if (!(sound_path = (char *) switch_xml_attr(language, "sound-prefix"))) {
		if (!(sound_path = (char *) switch_xml_attr(language, "sound-path"))) {
			sound_path = (char *) switch_xml_attr(language, "sound_path");
		}
	}

	if (!(tts_engine = (char *) switch_xml_attr(language, "tts-engine"))) {
		tts_engine = (char *) switch_xml_attr(language, "tts_engine");
	}

	if (!(tts_voice = (char *) switch_xml_attr(language, "tts-voice"))) {
		tts_voice = (char *) switch_xml_attr(language, "tts_voice");
	}

	/* If we use the new structure, check for a group name */
	if (language != macros) {
		char *p;
		char *macro_name_dup = switch_core_session_strdup(session, macro_name);
		const char *group_sound_path;
		const char *sound_prefix_enforced_str;

		if ((p = strchr(macro_name_dup, '@'))) {
			*p++ = '\0';
			local_macro_name = macro_name_dup;
			group_macro_name = p;

			if (!(macros = switch_xml_find_child(phrases, "macros", "name", group_macro_name))) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can't find macros group %s.\n", group_macro_name);
				goto done;
			}
		}
		/* Support override of certain language attribute */
		if ((group_sound_path = (char *) switch_xml_attr(macros, "sound-prefix")) || (group_sound_path = (char *) switch_xml_attr(macros, "sound-path")) || (group_sound_path = (char *) switch_xml_attr(macros, "sound_path"))) {
			sound_path = group_sound_path;
		}

		if (sound_prefix_enforced == SWITCH_FALSE && (sound_prefix_enforced_str = switch_xml_attr(macros, "sound-prefix-enforced"))
				&& (local_sound_prefix_enforced = switch_true(sound_prefix_enforced_str)) == SWITCH_TRUE) {
			switch_channel_set_variable(channel, "sound_prefix_enforced", sound_prefix_enforced_str);
		}

	}

	if (!(macro = switch_xml_find_child(macros, "macro", "name", local_macro_name))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can't find macro %s.\n", macro_name);
		goto done;
	}

	if (sound_path && sound_prefix_enforced == SWITCH_FALSE) {
		char *p;
		old_sound_prefix = switch_str_nil(switch_channel_get_variable(channel, "sound_prefix"));
		p = switch_core_session_strdup(session, old_sound_prefix);
		old_sound_prefix = p;
		switch_channel_set_variable(channel, "sound_prefix", sound_path);
	}

	if ((pause_val = switch_xml_attr(macro, "pause"))) {
		int tmp = atoi(pause_val);
		if (tmp >= 0) {
			pause = tmp;
		}
	}

	if (!(input = switch_xml_child(macro, "input"))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can't find any input tags.\n");
		goto done;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	while (input && !done) {
		char *field = (char *) switch_xml_attr(input, "field");
		char *pattern = (char *) switch_xml_attr(input, "pattern");
		const char *do_break = switch_xml_attr_soft(input, "break_on_match");
		char *field_expanded = NULL;
		char *field_expanded_alloc = NULL;

		if (!field) {
			field = (char *) data;
		}
		if (event) {
			field_expanded_alloc = switch_event_expand_headers(event, field);
		} else {
			field_expanded_alloc = switch_channel_expand_variables(channel, field);
		}

		if (field_expanded_alloc == field) {
			field_expanded_alloc = NULL;
			field_expanded = field;
		} else {
			field_expanded = field_expanded_alloc;
		}

		if (pattern) {
			switch_regex_t *re = NULL;
			int proceed = 0, ovector[100];
			char *substituted = NULL;
			uint32_t len = 0;
			char *odata = NULL;
			char *expanded = NULL;
			switch_xml_t match = NULL;

			status = SWITCH_STATUS_SUCCESS;

			if ((proceed = switch_regex_perform(field_expanded, pattern, &re, ovector, sizeof(ovector) / sizeof(ovector[0])))) {
				match = switch_xml_child(input, "match");
			} else {
				match = switch_xml_child(input, "nomatch");
			}

			if (match) {
				matches++;
				for (action = switch_xml_child(match, "action"); action && status == SWITCH_STATUS_SUCCESS; action = action->next) {
					char *adata = (char *) switch_xml_attr_soft(action, "data");
					char *func = (char *) switch_xml_attr_soft(action, "function");

					if (strchr(pattern, '(') && strchr(adata, '$') && proceed > 0) {
						len = (uint32_t) (strlen(data) + strlen(adata) + 10) * proceed;
						if (!(substituted = malloc(len))) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Memory Error!\n");
							switch_regex_safe_free(re);
							switch_safe_free(expanded);
							goto done;
						}
						memset(substituted, 0, len);
						switch_perform_substitution(re, proceed, adata, field_expanded, substituted, len, ovector);
						odata = substituted;
					} else {
						odata = adata;
					}

					if (event) {
						expanded = switch_event_expand_headers(event, odata);
					} else {
						expanded = switch_channel_expand_variables(channel, odata);
					}

					if (expanded == odata) {
						expanded = NULL;
					} else {
						odata = expanded;
					}

					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Handle %s:[%s] (%s:%s)\n", func, odata, chan_lang,
									  module_name);

					if (!strcasecmp(func, "play-file")) {
						status = switch_ivr_play_file(session, NULL, odata, args);
					} else if (!strcasecmp(func, "phrase")) {
						char *name = (char *) switch_xml_attr_soft(action, "phrase");
						status = switch_ivr_phrase_macro(session, name, odata, chan_lang, args);
					} else if (!strcasecmp(func, "break")) {
						done = 1;
						/* must allow the switch_safe_free below to execute or we leak - do not break here */
					} else if (!strcasecmp(func, "execute")) {
						switch_application_interface_t *app;
						char *cmd, *cmd_args;
						status = SWITCH_STATUS_FALSE;

						cmd = switch_core_session_strdup(session, odata);
						cmd_args = switch_separate_paren_args(cmd);

						if (!cmd_args) {
							cmd_args = "";
						}

						if ((app = switch_loadable_module_get_application_interface(cmd)) != NULL) {
							status = switch_core_session_exec(session, app, cmd_args);
							UNPROTECT_INTERFACE(app);
						} else {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Application %s\n", cmd);
						}
					} else if (!strcasecmp(func, "say")) {
						switch_say_interface_t *si;
						if ((si = switch_loadable_module_get_say_interface(module_name))) {
							char *say_type = (char *) switch_xml_attr_soft(action, "type");
							char *say_method = (char *) switch_xml_attr_soft(action, "method");
							char *say_gender = (char *) switch_xml_attr_soft(action, "gender");
							switch_say_args_t say_args = {0};

							say_args.type = switch_ivr_get_say_type_by_name(say_type);
							say_args.method = switch_ivr_get_say_method_by_name(say_method);
							say_args.gender = switch_ivr_get_say_gender_by_name(say_gender);

							status = si->say_function(session, odata, &say_args, args);
						} else {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid SAY Interface [%s]!\n", module_name);
						}
					} else if (!strcasecmp(func, "speak-text")) {
						const char *my_tts_engine = switch_xml_attr(action, "tts-engine");
						const char *my_tts_voice = switch_xml_attr(action, "tts-voice");

						if (!my_tts_engine) {
							my_tts_engine = tts_engine;
						}

						if (!my_tts_voice) {
							my_tts_voice = tts_voice;
						}
						if (zstr(tts_engine) || zstr(tts_voice)) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "TTS is not configured\n");
						} else {
							status = switch_ivr_speak_text(session, my_tts_engine, my_tts_voice, odata, args);
						}
					}

					switch_ivr_sleep(session, pause, SWITCH_FALSE, NULL);
					switch_safe_free(expanded);
					switch_safe_free(substituted);
					
				}
			}

			switch_regex_safe_free(re);
			
			if ((match && do_break && switch_true(do_break)) || status == SWITCH_STATUS_BREAK) {
				break;
			}

		}

		switch_safe_free(field_expanded_alloc);

		if (status != SWITCH_STATUS_SUCCESS) {
			done = 1;
			break;
		}

		input = input->next;
	}

  done:

	if (hint_data) {
		switch_event_destroy(&hint_data);
	}

	if (!matches) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Macro [%s]: '%s' did not match any patterns\n", macro_name, data);
	}

	if (old_sound_prefix) {
		switch_channel_set_variable(channel, "sound_prefix", old_sound_prefix);
	}
	if (local_sound_prefix_enforced == SWITCH_TRUE) {
		switch_channel_set_variable(channel, "sound_prefix_enforced", NULL);
	}

	if (xml) {
		switch_xml_free(xml);
	}

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_record_file(switch_core_session_t *session,
													   switch_file_handle_t *fh, const char *file, switch_input_args_t *args, uint32_t limit)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_dtmf_t dtmf = { 0 };
	switch_file_handle_t lfh = { 0 };
	switch_frame_t *read_frame;
	switch_codec_t codec, write_codec = { 0 };
	char *codec_name;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	const char *p;
	const char *vval;
	time_t start = 0;
	uint32_t org_silence_hits = 0;
	int asis = 0;
	int32_t sample_start = 0;
	int waste_resources = 0, fill_cng = 0;
	switch_codec_implementation_t read_impl = { 0 };
	switch_frame_t write_frame = { 0 };
	unsigned char write_buf[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	switch_event_t *event;
	int divisor = 0;
	int file_flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT;
	int restart_limit_on_dtmf = 0;
	const char *prefix, *var;

	prefix = switch_channel_get_variable(channel, "sound_prefix");

	if (!prefix) {
		prefix = SWITCH_GLOBAL_dirs.sounds_dir;
	}

	switch_core_session_get_read_impl(session, &read_impl);

	if (!(divisor = read_impl.actual_samples_per_second / 8000)) {
		divisor = 1;
	}


	if (!switch_channel_ready(channel)) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_channel_media_ready(channel)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!fh) {
		fh = &lfh;
	}

	fh->channels = read_impl.number_of_channels;
	fh->native_rate = read_impl.actual_samples_per_second;

	if (fh->samples > 0) {
		sample_start = fh->samples;
		fh->samples = 0;
	}

	if ((vval = switch_channel_get_variable(channel, "record_sample_rate"))) {
		int tmp = 0;

		tmp = atoi(vval);

		if (switch_is_valid_rate(tmp)) {
			fh->samplerate = tmp;
		}
	}

	
	if ((vval = switch_channel_get_variable(channel, "record_fill_cng"))) {

		if (!strcasecmp(vval, "true")) {
			fill_cng = 1400;
		} else {
			if ((fill_cng = atoi(vval)) < 0) {
				fill_cng = 0;
			}
		}
	}
	

	if ((vval = switch_channel_get_variable(channel, "record_waste_resources"))) {

		if (!strcasecmp(vval, "true")) {
			waste_resources = 1400;
		} else {
			if ((waste_resources = atoi(vval)) < 0) {
				waste_resources = 0;
			}
		}
	}


	if (fill_cng || waste_resources) {
		if (switch_core_codec_init(&write_codec,
								   "L16",
								   NULL,
								   read_impl.actual_samples_per_second,
								   read_impl.microseconds_per_packet / 1000,
								   read_impl.number_of_channels,
								   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
								   switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Raw Codec Activated, ready to waste resources!\n");
			write_frame.data = write_buf;
			write_frame.buflen = sizeof(write_buf);
			write_frame.datalen = read_impl.decoded_bytes_per_packet;
			write_frame.samples = write_frame.datalen / 2;
			write_frame.codec = &write_codec;
		} else {
			return SWITCH_STATUS_FALSE;
		}
	}

	if (!strstr(file, SWITCH_URL_SEPARATOR)) {
		char *ext;

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
			asis = 1;
		}
	}

	vval = switch_channel_get_variable(channel, "enable_file_write_buffering");
	if (!vval || switch_true(vval)) {
		fh->pre_buffer_datalen = SWITCH_DEFAULT_FILE_BUFFER_LEN;
	}

	if (switch_test_flag(fh, SWITCH_FILE_WRITE_APPEND) || ((p = switch_channel_get_variable(channel, "RECORD_APPEND")) && switch_true(p))) {
		file_flags |= SWITCH_FILE_WRITE_APPEND;
	}

	if (switch_test_flag(fh, SWITCH_FILE_WRITE_OVER) || ((p = switch_channel_get_variable(channel, "RECORD_WRITE_OVER")) && switch_true(p))) {
		file_flags |= SWITCH_FILE_WRITE_OVER;
	}

	if (!fh->prefix) {
		fh->prefix = prefix;
	}

	if (switch_core_file_open(fh, file, fh->channels, read_impl.actual_samples_per_second, file_flags, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
		switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
		return SWITCH_STATUS_GENERR;
	}

	if (sample_start > 0) {
		uint32_t pos = 0;
		switch_core_file_seek(fh, &pos, sample_start, SEEK_SET);
		switch_clear_flag(fh, SWITCH_FILE_SEEK);	
		fh->samples = 0;
	}


	if (switch_test_flag(fh, SWITCH_FILE_NATIVE)) {
		asis = 1;
	}
	
	restart_limit_on_dtmf = switch_true(switch_channel_get_variable(channel, "record_restart_limit_on_dtmf"));

	if ((p = switch_channel_get_variable(channel, "RECORD_TITLE"))) {
		vval = switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_TITLE, vval);
		switch_channel_set_variable(channel, "RECORD_TITLE", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_COPYRIGHT"))) {
		vval = switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_COPYRIGHT, vval);
		switch_channel_set_variable(channel, "RECORD_COPYRIGHT", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_SOFTWARE"))) {
		vval = switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_SOFTWARE, vval);
		switch_channel_set_variable(channel, "RECORD_SOFTWARE", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_ARTIST"))) {
		vval = switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_ARTIST, vval);
		switch_channel_set_variable(channel, "RECORD_ARTIST", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_COMMENT"))) {
		vval = switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_COMMENT, vval);
		switch_channel_set_variable(channel, "RECORD_COMMENT", NULL);
	}

	if ((p = switch_channel_get_variable(channel, "RECORD_DATE"))) {
		vval = switch_core_session_strdup(session, p);
		switch_core_file_set_string(fh, SWITCH_AUDIO_COL_STR_DATE, vval);
		switch_channel_set_variable(channel, "RECORD_DATE", NULL);
	}

	if (!asis) {
		codec_name = "L16";
		if (switch_core_codec_init(&codec,
								   codec_name,
								   NULL,
								   read_impl.actual_samples_per_second,
								   read_impl.microseconds_per_packet / 1000,
								   read_impl.number_of_channels,
								   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
								   switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Raw Codec Activated\n");
			switch_core_session_set_read_codec(session, &codec);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
							  "Raw Codec Activation Failed %s@%uhz %u channels %dms\n", codec_name, fh->samplerate,
							  fh->channels, read_impl.microseconds_per_packet / 1000);
			switch_core_file_close(fh);
			switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
			return SWITCH_STATUS_GENERR;
		}
	}

	if (limit) {
		start = switch_epoch_time_now(NULL);
	}

	if (fh->thresh) {
		if (asis) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Can't detect silence on a native recording.\n");
		} else {
			if (fh->silence_hits) {
				fh->silence_hits = fh->samplerate * fh->silence_hits / read_impl.samples_per_packet;
			} else {
				fh->silence_hits = fh->samplerate * 3 / read_impl.samples_per_packet;
			}
			org_silence_hits = fh->silence_hits;
		}
	}


	if (switch_event_create(&event, SWITCH_EVENT_RECORD_START) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Record-File-Path", file);
		switch_event_fire(&event);
	}

	for (;;) {
		switch_size_t len;

		if (!switch_channel_ready(channel)) {
			status = SWITCH_STATUS_FALSE;
			break;
		}

		if (switch_channel_test_flag(channel, CF_BREAK)) {
			switch_channel_clear_flag(channel, CF_BREAK);
			status = SWITCH_STATUS_BREAK;
			break;
		}

		switch_ivr_parse_all_events(session);

		if (start && (switch_epoch_time_now(NULL) - start) > limit) {
			break;
		}

		if (args) {
			/*
			   dtmf handler function you can hook up to be executed when a digit is dialed during playback 
			   if you return anything but SWITCH_STATUS_SUCCESS the playback will stop.
			 */
			if (switch_channel_has_dtmf(channel)) {

				if (limit && restart_limit_on_dtmf) {
					start = switch_epoch_time_now(NULL);
				}

				if (!args->input_callback && !args->buf && !args->dmachine) {
					status = SWITCH_STATUS_BREAK;
					break;
				}
				switch_channel_dequeue_dtmf(channel, &dtmf);

				if (args->dmachine) {
					char ds[2] = {dtmf.digit, '\0'};
					if ((status = switch_ivr_dmachine_feed(args->dmachine, ds, NULL)) != SWITCH_STATUS_SUCCESS) {
						break;
					}
				} else if (args->input_callback) {
					status = args->input_callback(session, (void *) &dtmf, SWITCH_INPUT_TYPE_DTMF, args->buf, args->buflen);
				} else {
					*((char *) args->buf) = dtmf.digit;
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

		status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (args && args->dmachine) {
			if ((status = switch_ivr_dmachine_ping(args->dmachine, NULL)) != SWITCH_STATUS_SUCCESS) {
				break;
			}
		}

		if (args && (args->read_frame_callback)) {
			if ((status = args->read_frame_callback(session, read_frame, args->user_data)) != SWITCH_STATUS_SUCCESS) {
				break;
			}
		}

		if (!asis && fh->thresh) {
			int16_t *fdata = (int16_t *) read_frame->data;
			uint32_t samples = read_frame->datalen / sizeof(*fdata);
			uint32_t score, count = 0, j = 0;
			double energy = 0;


			for (count = 0; count < samples; count++) {
				energy += abs(fdata[j]);
				j += read_impl.number_of_channels;
			}

			score = (uint32_t) (energy / (samples / divisor));

			if (score < fh->thresh) {
				if (!--fh->silence_hits) {
					break;
				}
			} else {
				fh->silence_hits = org_silence_hits;
			}
		}

		if (fill_cng) {
			switch_generate_sln_silence((int16_t *) write_frame.data, write_frame.samples, fill_cng);
		} else if (waste_resources) {
			switch_generate_sln_silence((int16_t *) write_frame.data, write_frame.samples, waste_resources);
		}

		if (!switch_test_flag(fh, SWITCH_FILE_PAUSE) && !switch_test_flag(read_frame, SFF_CNG)) {
			int16_t *data = read_frame->data;
			len = (switch_size_t) asis ? read_frame->datalen : read_frame->datalen / 2;

			if (switch_core_file_write(fh, data, &len) != SWITCH_STATUS_SUCCESS) {
				break;
			}
		} else if (switch_test_flag(read_frame, SFF_CNG) && fill_cng) {
			len = write_frame.datalen / 2;
			if (switch_core_file_write(fh, write_frame.data, &len) != SWITCH_STATUS_SUCCESS) {
				break;
			}			
		}

		if (waste_resources) {
			if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
				break;
			}
		}

	}

	if (fill_cng || waste_resources) {
		switch_core_codec_destroy(&write_codec);
	}

	switch_core_file_close(fh);


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

	if (read_impl.actual_samples_per_second) {
		switch_channel_set_variable_printf(channel, "record_seconds", "%d", fh->samples_out / read_impl.actual_samples_per_second);
		switch_channel_set_variable_printf(channel, "record_ms", "%d", fh->samples_out / (read_impl.actual_samples_per_second / 1000));

	}

	switch_channel_set_variable_printf(channel, "record_samples", "%d", fh->samples_out);

	if (switch_event_create(&event, SWITCH_EVENT_RECORD_STOP) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Record-File-Path", file);
		switch_event_fire(&event);
	}

	switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
	return status;
}

static int teletone_handler(teletone_generation_session_t *ts, teletone_tone_map_t *map)
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

SWITCH_DECLARE(switch_status_t) switch_ivr_gentones(switch_core_session_t *session, const char *script, int32_t loops, switch_input_args_t *args)
{
	teletone_generation_session_t ts;
	switch_dtmf_t dtmf = { 0 };
	switch_buffer_t *audio_buffer;
	switch_frame_t *read_frame = NULL;
	switch_codec_t write_codec = { 0 };
	switch_frame_t write_frame = { 0 };
	switch_byte_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_codec_implementation_t read_impl = { 0 };
	switch_core_session_get_read_impl(session, &read_impl);

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_codec_init(&write_codec,
							   "L16",
							   NULL,
							   read_impl.actual_samples_per_second,
							   read_impl.microseconds_per_packet / 1000,
							   1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {

		return SWITCH_STATUS_FALSE;
	}
	memset(&ts, 0, sizeof(ts));
	write_frame.codec = &write_codec;
	write_frame.data = data;
	write_frame.buflen = sizeof(data);

	switch_buffer_create_dynamic(&audio_buffer, 512, 1024, 0);
	teletone_init_session(&ts, 0, teletone_handler, audio_buffer);
	ts.rate = read_impl.actual_samples_per_second;
	ts.channels = 1;
	teletone_run(&ts, script);

	if (loops) {
		switch_buffer_set_loops(audio_buffer, loops);
	}

	for (;;) {
		switch_status_t status;

		if (!switch_channel_ready(channel)) {
			status = SWITCH_STATUS_FALSE;
			break;
		}

		if (switch_channel_test_flag(channel, CF_BREAK)) {
			switch_channel_clear_flag(channel, CF_BREAK);
			status = SWITCH_STATUS_BREAK;
			break;
		}

		status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (args && args->dmachine) {
			if ((status = switch_ivr_dmachine_ping(args->dmachine, NULL)) != SWITCH_STATUS_SUCCESS) {
				break;
			}
		}

		if (args && (args->read_frame_callback)) {
			if ((status = args->read_frame_callback(session, read_frame, args->user_data)) != SWITCH_STATUS_SUCCESS) {
				break;
			}
		}

		switch_ivr_parse_all_events(session);

		if (args) {
			/*
			   dtmf handler function you can hook up to be executed when a digit is dialed during gentones 
			   if you return anything but SWITCH_STATUS_SUCCESS the playback will stop.
			 */
			if (switch_channel_has_dtmf(channel)) {
				if (!args->input_callback && !args->buf && !args->dmachine) {
					status = SWITCH_STATUS_BREAK;
					break;
				}
				switch_channel_dequeue_dtmf(channel, &dtmf);

				if (args->dmachine) {
					char ds[2] = {dtmf.digit, '\0'};
					if ((status = switch_ivr_dmachine_feed(args->dmachine, ds, NULL)) != SWITCH_STATUS_SUCCESS) {
						break;
					}
				} else if (args->input_callback) {
					status = args->input_callback(session, (void *) &dtmf, SWITCH_INPUT_TYPE_DTMF, args->buf, args->buflen);
				} else {
					*((char *) args->buf) = dtmf.digit;
					status = SWITCH_STATUS_BREAK;
				}
			}

			if (args->input_callback) {
				switch_event_t *event;
				
				if (switch_core_session_dequeue_event(session, &event, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
					status = args->input_callback(session, event, SWITCH_INPUT_TYPE_EVENT, args->buf, args->buflen);
					switch_event_destroy(&event);
				}
			}

			if (status != SWITCH_STATUS_SUCCESS) {
				break;
			}
		}

		if ((write_frame.datalen = (uint32_t) switch_buffer_read_loop(audio_buffer, write_frame.data, read_impl.decoded_bytes_per_packet)) <= 0) {
			break;
		}

		write_frame.samples = write_frame.datalen / 2;

		if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
			break;
		}
	}

	switch_core_codec_destroy(&write_codec);
	switch_buffer_destroy(&audio_buffer);
	teletone_destroy_session(&ts);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_get_file_handle(switch_core_session_t *session, switch_file_handle_t **fh)
{
	switch_file_handle_t *fhp;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	*fh = NULL;
	switch_core_session_io_read_lock(session);
	
	if ((fhp = switch_channel_get_private(channel, "__fh"))) {
		*fh = fhp;
		return SWITCH_STATUS_SUCCESS;
	}

	switch_core_session_io_rwunlock(session);

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_release_file_handle(switch_core_session_t *session, switch_file_handle_t **fh)
{
	*fh = NULL;
	switch_core_session_io_rwunlock(session);

	return SWITCH_STATUS_SUCCESS;
}

#define FILE_STARTSAMPLES 1024 * 32
#define FILE_BLOCKSIZE 1024 * 8
#define FILE_BUFSIZE 1024 * 64

static void add_playback_vars_to_event(switch_core_session_t *session, switch_event_t *event, char *vars)
{
	char *tmp;

	if (!session || !event || !vars)
		return;

	if ((tmp = switch_core_session_strdup(session, vars))) {
		char *argv[128] = { 0 };
		int argc, i;

		if (!(argc = switch_separate_string(tmp, ',', argv, (sizeof(argv) / sizeof(argv[0])))))
			return;

		for (i = 0; i < argc; i++) {
			char *var, *val;

			if ((var = strchr(argv[i], '='))) {
				*var = '\0';
				val = var+1;
				var = argv[i];

				if (var && *var && val && *val) {
					if ((var = switch_core_session_sprintf(session, "playback_variable_%s", var))) {
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, var, val);
					}
				}
			}
		}
	}
}

SWITCH_DECLARE(switch_status_t) switch_ivr_play_file(switch_core_session_t *session, switch_file_handle_t *fh, const char *file, switch_input_args_t *args)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int16_t *abuf = NULL;
	switch_dtmf_t dtmf = { 0 };
	uint32_t interval = 0, samples = 0, framelen, sample_start = 0;
	uint32_t ilen = 0;
	switch_size_t olen = 0, llen = 0;
	switch_frame_t write_frame = { 0 };
	switch_timer_t timer = { 0 };
	switch_codec_t codec = { 0 };
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);
	char *codec_name;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_file_handle_t lfh;
	const char *p;
	//char *title = "", *copyright = "", *software = "", *artist = "", *comment = "", *date = "";
	char *ext;
	const char *prefix;
	const char *timer_name;
	const char *prebuf;
	const char *alt = NULL;
	const char *sleep_val;
	const char *play_delimiter_val;
	char play_delimiter = 0;
	int sleep_val_i = 250;
	int eof = 0;
	switch_size_t bread = 0;
	int l16 = 0;
	switch_codec_implementation_t read_impl = { 0 };
	char *file_dup;
	char *argv[128] = { 0 };
	int argc;
	int cur;
	int done = 0;
	int timeout_samples = 0;
	const char *var;
	int more_data = 0;
	char *playback_vars, *tmp;
	switch_event_t *event;
	uint32_t test_native = 0, last_native = 0;

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	switch_core_session_get_read_impl(session, &read_impl);

	if ((var = switch_channel_get_variable(channel, "playback_timeout_sec"))) {
		int tmp = atoi(var);
		if (tmp > 1) {
			timeout_samples = read_impl.actual_samples_per_second * tmp;
		}
	}

	if ((play_delimiter_val = switch_channel_get_variable(channel, "playback_delimiter"))) {
		play_delimiter = *play_delimiter_val;

		if ((sleep_val = switch_channel_get_variable(channel, "playback_sleep_val"))) {
			int tmp = atoi(sleep_val);
			if (tmp >= 0) {
				sleep_val_i = tmp;
			}
		}
	}

	prefix = switch_channel_get_variable(channel, "sound_prefix");
	timer_name = switch_channel_get_variable(channel, "timer_name");

	if (zstr(file) || !switch_channel_media_ready(channel)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!strcasecmp(read_impl.iananame, "l16")) {
		l16++;
	}

	if (play_delimiter) {
		file_dup = switch_core_session_strdup(session, file);
		argc = switch_separate_string(file_dup, play_delimiter, argv, (sizeof(argv) / sizeof(argv[0])));
	} else {
		argc = 1;
		argv[0] = (char *) file;
	}

	if (!fh) {
		fh = &lfh;
		memset(fh, 0, sizeof(lfh));
	}
	
	if (fh->samples > 0) {
		sample_start = fh->samples;
		fh->samples = 0;
	}
	
	for (cur = 0; switch_channel_ready(channel) && !done && cur < argc; cur++) {
		file = argv[cur];
		eof = 0;

		if (cur) {
			fh->samples = sample_start = 0;
			if (sleep_val_i) {
				switch_ivr_sleep(session, sleep_val_i, SWITCH_FALSE, args);
			}
		}

		status = SWITCH_STATUS_SUCCESS;

		if ((alt = strchr(file, ':'))) {
			char *dup;

			if (!strncasecmp(file, "phrase:", 7)) {
				char *arg = NULL;
				const char *lang = switch_channel_get_variable(channel, "language");
				alt = file + 7;
				dup = switch_core_session_strdup(session, alt);

				if (dup) {
					if ((arg = strchr(dup, ':'))) {
						*arg++ = '\0';
					}
					if ((status = switch_ivr_phrase_macro(session, dup, arg, lang, args)) != SWITCH_STATUS_SUCCESS) {
						return status;
					}
					continue;
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Args\n");
					continue;
				}
			} else if (!strncasecmp(file, "say:", 4)) {
				char *engine = NULL, *voice = NULL, *text = NULL;
				alt = file + 4;
				dup = switch_core_session_strdup(session, alt);
				engine = dup;

				if (!zstr(engine)) {
					if ((voice = strchr(engine, ':'))) {
						*voice++ = '\0';
						if (!zstr(voice) && (text = strchr(voice, ':'))) {
							*text++ = '\0';
						}
					}
				}

				if (!zstr(engine) && !zstr(voice) && !zstr(text)) {
					if ((status = switch_ivr_speak_text(session, engine, voice, text, args)) != SWITCH_STATUS_SUCCESS) {
						return status;
					}
				} else {
					text = engine;
					engine = (char *) switch_channel_get_variable(channel, "tts_engine");
					voice = (char *) switch_channel_get_variable(channel, "tts_voice");
					if (engine && text) {
						if ((status = switch_ivr_speak_text(session, engine, voice, text, args)) != SWITCH_STATUS_SUCCESS) {
							return status;
						}
					} else {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Args\n");
					}
				}
				continue;
			}

		}

		if (!prefix) {
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

		/* Try to parse extra parameters for this playback (parameters within {} at the end of the filename */
		playback_vars = NULL;
		if ((tmp = strchr(file, '{'))) {
			char *tfile, *e;
			
			if ((tfile = switch_core_session_strdup(session, tmp))) {
				if ((e = switch_find_end_paren(tfile, '{', '}')) && *(e + 1) == '\0') {
					*tmp = '\0';
					*e = '\0';
					playback_vars = tfile+1;
				}
			}
		}

		if ((prebuf = switch_channel_get_variable(channel, "stream_prebuffer"))) {
			int maybe = atoi(prebuf);
			if (maybe > 0) {
				fh->prebuf = maybe;
			}
		}


		if (!fh->prefix) {
			fh->prefix = prefix;
		}

		if (switch_core_file_open(fh,
								  file,
								  read_impl.number_of_channels,
								  read_impl.actual_samples_per_second, SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, NULL) != SWITCH_STATUS_SUCCESS) {
			switch_core_session_reset(session, SWITCH_TRUE, SWITCH_FALSE);
			status = SWITCH_STATUS_NOTFOUND;
			continue;
		}

		switch_core_session_io_write_lock(session);
		switch_channel_set_private(channel, "__fh", fh);
		switch_core_session_io_rwunlock(session);


		if (!abuf) {
			switch_zmalloc(abuf, FILE_STARTSAMPLES * sizeof(*abuf));
			write_frame.data = abuf;
			write_frame.buflen = FILE_STARTSAMPLES;
		}

		if (sample_start > 0) {
			uint32_t pos = 0;
			switch_core_file_seek(fh, &pos, 0, SEEK_SET);
			switch_core_file_seek(fh, &pos, sample_start, SEEK_CUR);
			switch_clear_flag(fh, SWITCH_FILE_SEEK);
		}

		if (switch_core_file_get_string(fh, SWITCH_AUDIO_COL_STR_TITLE, &p) == SWITCH_STATUS_SUCCESS) {
			//title = switch_core_session_strdup(session, p);
			switch_channel_set_variable(channel, "RECORD_TITLE", p);
		}

		if (switch_core_file_get_string(fh, SWITCH_AUDIO_COL_STR_COPYRIGHT, &p) == SWITCH_STATUS_SUCCESS) {
			//copyright = switch_core_session_strdup(session, p);
			switch_channel_set_variable(channel, "RECORD_COPYRIGHT", p);
		}

		if (switch_core_file_get_string(fh, SWITCH_AUDIO_COL_STR_SOFTWARE, &p) == SWITCH_STATUS_SUCCESS) {
			//software = switch_core_session_strdup(session, p);
			switch_channel_set_variable(channel, "RECORD_SOFTWARE", p);
		}

		if (switch_core_file_get_string(fh, SWITCH_AUDIO_COL_STR_ARTIST, &p) == SWITCH_STATUS_SUCCESS) {
			//artist = switch_core_session_strdup(session, p);
			switch_channel_set_variable(channel, "RECORD_ARTIST", p);
		}

		if (switch_core_file_get_string(fh, SWITCH_AUDIO_COL_STR_COMMENT, &p) == SWITCH_STATUS_SUCCESS) {
			//comment = switch_core_session_strdup(session, p);
			switch_channel_set_variable(channel, "RECORD_COMMENT", p);
		}

		if (switch_core_file_get_string(fh, SWITCH_AUDIO_COL_STR_DATE, &p) == SWITCH_STATUS_SUCCESS) {
			//date = switch_core_session_strdup(session, p);
			switch_channel_set_variable(channel, "RECORD_DATE", p);
		}
		
		interval = read_impl.microseconds_per_packet / 1000;

		if (!fh->audio_buffer) {
			switch_buffer_create_dynamic(&fh->audio_buffer, FILE_BLOCKSIZE, FILE_BUFSIZE, 0);
			switch_assert(fh->audio_buffer);
		}

		codec_name = "L16";
		
		if (!switch_core_codec_ready((&codec))) {
			if (switch_core_codec_init(&codec,
									   codec_name,
									   NULL,
									   fh->samplerate,
									   interval, 1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, pool) == SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
								  SWITCH_LOG_DEBUG, "Codec Activated %s@%uhz %u channels %dms\n", codec_name, fh->samplerate, fh->channels, interval);
				
				
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
								  "Raw Codec Activation Failed %s@%uhz %u channels %dms\n", codec_name, fh->samplerate, fh->channels, interval);
				switch_core_session_io_write_lock(session);
				switch_channel_set_private(channel, "__fh", NULL);
				switch_core_session_io_rwunlock(session);
				
				switch_core_file_close(fh);
				
				switch_core_session_reset(session, SWITCH_TRUE, SWITCH_FALSE);
				status = SWITCH_STATUS_GENERR;
				continue;
			}
		}

		test_native = switch_test_flag(fh, SWITCH_FILE_NATIVE);

		if (test_native) {
			write_frame.codec = switch_core_session_get_read_codec(session);
			samples = read_impl.samples_per_packet;
			framelen = read_impl.encoded_bytes_per_packet;
		} else {
			write_frame.codec = &codec;
			samples = codec.implementation->samples_per_packet;
			framelen = codec.implementation->decoded_bytes_per_packet;
		}
		
		last_native = test_native;

		if (timer_name && !timer.samplecount) {
			uint32_t len;

			len = samples * 2;
			if (switch_core_timer_init(&timer, timer_name, interval, samples, pool) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Setup timer failed!\n");
				switch_core_codec_destroy(&codec);
				switch_core_session_io_write_lock(session);
				switch_channel_set_private(channel, "__fh", NULL);
				switch_core_session_io_rwunlock(session);
				switch_core_file_close(fh);
				switch_core_session_reset(session, SWITCH_TRUE, SWITCH_FALSE);
				status = SWITCH_STATUS_GENERR;
				continue;
			}
			switch_core_timer_sync(&timer); // Sync timer
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Setup timer success %u bytes per %d ms!\n", len, interval);
		}
		write_frame.rate = fh->samplerate;

		if (timer_name) {
			/* start a thread to absorb incoming audio */
			switch_core_service_session(session);
		}

		ilen = samples;

		if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_START) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Path", file);
			add_playback_vars_to_event(session, event, playback_vars); 
			switch_event_fire(&event);
		}

		for (;;) {
			int do_speed = 1;
			int last_speed = -1;
			int f;
			
			if (!switch_channel_ready(channel)) {
				status = SWITCH_STATUS_FALSE;
				break;
			}

			if ((f = switch_channel_test_flag(channel, CF_BREAK))) {
				switch_channel_clear_flag(channel, CF_BREAK);
				if (f == 2) {
					done = 1;
				}
				status = SWITCH_STATUS_BREAK;
				break;
			}

			switch_ivr_parse_all_events(session);

			if (args) {
				/*
				   dtmf handler function you can hook up to be executed when a digit is dialed during playback 
				   if you return anything but SWITCH_STATUS_SUCCESS the playback will stop.
				 */
				if (switch_channel_has_dtmf(channel)) {
					if (!args->input_callback && !args->buf && !args->dmachine) {
						status = SWITCH_STATUS_BREAK;
						done = 1;
						break;
					}
					switch_channel_dequeue_dtmf(channel, &dtmf);

					if (args->dmachine) {
						char ds[2] = {dtmf.digit, '\0'};
						if ((status = switch_ivr_dmachine_feed(args->dmachine, ds, NULL)) != SWITCH_STATUS_SUCCESS) {
							break;
						}
					} else if (args->input_callback) {
						status = args->input_callback(session, (void *) &dtmf, SWITCH_INPUT_TYPE_DTMF, args->buf, args->buflen);
					} else {
						*((char *) args->buf) = dtmf.digit;
						status = SWITCH_STATUS_BREAK;
					}
				}

				if (args->input_callback) {
					switch_event_t *event;

					if (switch_core_session_dequeue_event(session, &event, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
						status = args->input_callback(session, event, SWITCH_INPUT_TYPE_EVENT, args->buf, args->buflen);
						switch_event_destroy(&event);
					}
				}

				if (status != SWITCH_STATUS_SUCCESS) {
					done = 1;
					break;
				}
			}

			if (switch_test_flag(fh, SWITCH_FILE_PAUSE)) {
				if (framelen > FILE_STARTSAMPLES) {
					framelen = FILE_STARTSAMPLES;
				}
				memset(abuf, 255, framelen);
				olen = ilen;
				do_speed = 0;
			} else if (fh->sp_audio_buffer && (eof || (switch_buffer_inuse(fh->sp_audio_buffer) > (switch_size_t) (framelen)))) {
				if (!(bread = switch_buffer_read(fh->sp_audio_buffer, abuf, framelen))) {
					if (eof) {
						continue;
					} else {
						break;
					}
				}

				if (bread < framelen) {
					memset(abuf + bread, 255, framelen - bread);
				}

				olen = switch_test_flag(fh, SWITCH_FILE_NATIVE) ? framelen : ilen;
				do_speed = 0;
			} else if (fh->audio_buffer && (eof || (switch_buffer_inuse(fh->audio_buffer) > (switch_size_t) (framelen)))) {
				if (!(bread = switch_buffer_read(fh->audio_buffer, abuf, framelen))) {
					if (eof) {
						break;
					} else {
						continue;
					}
				}

				fh->offset_pos += switch_test_flag(fh, SWITCH_FILE_NATIVE) ? bread : bread / 2;

				if (bread < framelen) {
					memset(abuf + bread, 255, framelen - bread);
				}

				olen = switch_test_flag(fh, SWITCH_FILE_NATIVE) ? framelen : ilen;
			} else {
				if (eof) {
					break;
				}
				olen = FILE_STARTSAMPLES;
				if (!switch_test_flag(fh, SWITCH_FILE_NATIVE)) {
					olen /= 2;
				}
				if (switch_core_file_read(fh, abuf, &olen) != SWITCH_STATUS_SUCCESS) {
					eof++;
					continue;
				}
				
				test_native = switch_test_flag(fh, SWITCH_FILE_NATIVE);

				if (test_native != last_native) {
					if (test_native) {
						write_frame.codec = switch_core_session_get_read_codec(session);
						samples = read_impl.samples_per_packet;
						framelen = read_impl.encoded_bytes_per_packet;						
					} else {
						write_frame.codec = &codec;
						samples = codec.implementation->samples_per_packet;
						framelen = codec.implementation->decoded_bytes_per_packet;
					}
					switch_buffer_zero(fh->audio_buffer);
				}

				last_native = test_native;

				switch_buffer_write(fh->audio_buffer, abuf, switch_test_flag(fh, SWITCH_FILE_NATIVE) ? olen : olen * 2);
				olen = switch_buffer_read(fh->audio_buffer, abuf, framelen);
				fh->offset_pos += olen / 2;

				if (!switch_test_flag(fh, SWITCH_FILE_NATIVE)) {
					olen /= 2;
				}

			}

			if (done || olen <= 0) {
				break;
			}

			if (!switch_test_flag(fh, SWITCH_FILE_NATIVE)) {
				if (fh->speed > 2) {
					fh->speed = 2;
				} else if (fh->speed < -2) {
					fh->speed = -2;
				}
			}

			if (!switch_test_flag(fh, SWITCH_FILE_NATIVE) && fh->audio_buffer && last_speed > -1 && last_speed != fh->speed) {
				switch_buffer_zero(fh->sp_audio_buffer);
			}

			if (switch_test_flag(fh, SWITCH_FILE_SEEK)) {
				/* file position has changed flush the buffer */
				switch_buffer_zero(fh->audio_buffer);
				switch_clear_flag(fh, SWITCH_FILE_SEEK);
			}


			if (!switch_test_flag(fh, SWITCH_FILE_NATIVE) && fh->speed && do_speed) {
				float factor = 0.25f * abs(fh->speed);
				switch_size_t newlen, supplement, step;
				short *bp = write_frame.data;
				switch_size_t wrote = 0;

				supplement = (int) (factor * olen);
				if (!supplement) {
					supplement = 1;
				}
				newlen = (fh->speed > 0) ? olen - supplement : olen + supplement;

				step = (fh->speed > 0) ? (newlen / supplement) : (olen / supplement);

				if (!fh->sp_audio_buffer) {
					switch_buffer_create_dynamic(&fh->sp_audio_buffer, 1024, 1024, 0);
				}

				while ((wrote + step) < newlen) {
					switch_buffer_write(fh->sp_audio_buffer, bp, step * 2);
					wrote += step;
					bp += step;
					if (fh->speed > 0) {
						bp++;
					} else {
						float f;
						short s;
						f = (float) (*bp + *(bp + 1) + *(bp - 1));
						f /= 3;
						s = (short) f;
						switch_buffer_write(fh->sp_audio_buffer, &s, 2);
						wrote++;
					}
				}
				if (wrote < newlen) {
					switch_size_t r = newlen - wrote;
					switch_buffer_write(fh->sp_audio_buffer, bp, r * 2);
					wrote += r;
				}
				last_speed = fh->speed;
				continue;
			}
			
			if (olen < llen) {
				uint8_t *dp = (uint8_t *) write_frame.data;
				memset(dp + (int) olen, 255, (int) (llen - olen));
				olen = llen;
			}

			if (!more_data) {
				if (timer_name) {
					if (switch_core_timer_next(&timer) != SWITCH_STATUS_SUCCESS) {
						break;
					}
				} else {			/* time off the channel (if you must) */
					switch_frame_t *read_frame;
					switch_status_t tstatus;
					
					while (switch_channel_ready(channel) && switch_channel_test_flag(channel, CF_HOLD)) {
						switch_yield(10000);
					}

					tstatus = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_SINGLE_READ, 0);

					if (!SWITCH_READ_ACCEPTABLE(tstatus)) {
						break;
					}

					if (args && args->dmachine) {
						if ((status = switch_ivr_dmachine_ping(args->dmachine, NULL)) != SWITCH_STATUS_SUCCESS) {
							break;
						}
					}

					if (args && (args->read_frame_callback)) {
						int ok = 1;
						switch_set_flag(fh, SWITCH_FILE_CALLBACK);
						if ((status = args->read_frame_callback(session, read_frame, args->user_data)) != SWITCH_STATUS_SUCCESS) {
							ok = 0;
						}
						switch_clear_flag(fh, SWITCH_FILE_CALLBACK);
						if (!ok) {
							break;
						}
					}
				}
			}

			more_data = 0;
			write_frame.samples = (uint32_t) olen;

			if (switch_test_flag(fh, SWITCH_FILE_NATIVE)) {
				write_frame.datalen = (uint32_t) olen;
			} else {
				write_frame.datalen = write_frame.samples * 2;
			}

			llen = olen;

			if (timer_name) {
				write_frame.timestamp = timer.samplecount;
			}
#ifndef WIN32
#if SWITCH_BYTE_ORDER == __BIG_ENDIAN
			if (!switch_test_flag(fh, SWITCH_FILE_NATIVE) && l16) {
				switch_swap_linear(write_frame.data, (int) write_frame.datalen / 2);
			}
#endif
#endif
			if (!switch_test_flag(fh, SWITCH_FILE_NATIVE) && fh->vol) {
				switch_change_sln_volume(write_frame.data, write_frame.datalen / 2, fh->vol);
			}

			status = switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0);

			if (timeout_samples) {
				timeout_samples -= write_frame.samples;
				if (timeout_samples <= 0) {
					timeout_samples = 0;
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "timeout reached playing file\n");
					status = SWITCH_STATUS_TIMEOUT;
					break;
				}
			}
			

			if (status == SWITCH_STATUS_MORE_DATA) {
				status = SWITCH_STATUS_SUCCESS;
				more_data = 1;
				continue;
			} else if (status != SWITCH_STATUS_SUCCESS) {
				done = 1;
				break;
			}

			if (done) {
				break;
			}
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "done playing file\n");

		if (read_impl.samples_per_second) {
			switch_channel_set_variable_printf(channel, "playback_seconds", "%d", fh->samples_in / read_impl.samples_per_second);
			switch_channel_set_variable_printf(channel, "playback_ms", "%d", fh->samples_in / (read_impl.samples_per_second / 1000));
		}
		switch_channel_set_variable_printf(channel, "playback_samples", "%d", fh->samples_in);

		if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_STOP) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Path", file);
			if (status == SWITCH_STATUS_BREAK) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-Status", "break");
			} else {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-Status", "done");
			}
			add_playback_vars_to_event(session, event, playback_vars); 
			switch_event_fire(&event);
		}

		switch_core_session_io_write_lock(session);
		switch_channel_set_private(channel, "__fh", NULL);
		switch_core_session_io_rwunlock(session);

		switch_core_file_close(fh);

		if (fh->audio_buffer) {
			switch_buffer_destroy(&fh->audio_buffer);
		}

		if (fh->sp_audio_buffer) {
			switch_buffer_destroy(&fh->sp_audio_buffer);
		}
	}

	if (switch_core_codec_ready((&codec))) {
		switch_core_codec_destroy(&codec);
	}

	if (timer.samplecount) {
		/* End the audio absorbing thread */
		switch_core_thread_session_end(session);
		switch_core_timer_destroy(&timer);
	}

	switch_safe_free(abuf);

	switch_core_session_reset(session, SWITCH_FALSE, SWITCH_FALSE);
	return status;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_wait_for_silence(switch_core_session_t *session, uint32_t thresh,
															uint32_t silence_hits, uint32_t listen_hits, uint32_t timeout_ms, const char *file)
{
	uint32_t score, count = 0, j = 0;
	double energy = 0;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int divisor = 0;
	uint32_t org_silence_hits = silence_hits;
	uint32_t channels;
	switch_frame_t *read_frame;
	switch_status_t status = SWITCH_STATUS_FALSE;
	int16_t *data;
	uint32_t listening = 0;
	int countdown = 0;
	switch_codec_t raw_codec = { 0 };
	int16_t *abuf = NULL;
	switch_frame_t write_frame = { 0 };
	switch_file_handle_t fh = { 0 };
	int32_t sample_count = 0;
	switch_codec_implementation_t read_impl = { 0 };
	switch_core_session_get_read_impl(session, &read_impl);


	if (timeout_ms) {
		sample_count = (read_impl.actual_samples_per_second / 1000) * timeout_ms;
	}

	if (file) {
		if (switch_core_file_open(&fh,
								  file,
								  read_impl.number_of_channels,
								  read_impl.actual_samples_per_second, SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, NULL) != SWITCH_STATUS_SUCCESS) {
			switch_core_session_reset(session, SWITCH_TRUE, SWITCH_FALSE);
			return SWITCH_STATUS_NOTFOUND;
		}
		switch_zmalloc(abuf, SWITCH_RECOMMENDED_BUFFER_SIZE);
		write_frame.data = abuf;
		write_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
	}


	if (switch_core_codec_init(&raw_codec,
							   "L16",
							   NULL,
							   read_impl.actual_samples_per_second,
							   read_impl.microseconds_per_packet / 1000,
							   1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {

		status = SWITCH_STATUS_FALSE;
		goto end;
	}

	write_frame.codec = &raw_codec;

	divisor = read_impl.actual_samples_per_second / 8000;
	channels = read_impl.number_of_channels;

	switch_core_session_set_read_codec(session, &raw_codec);

	while (switch_channel_ready(channel)) {

		status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (sample_count) {
			sample_count -= raw_codec.implementation->samples_per_packet;
			if (sample_count <= 0) {
				break;
			}
		}

		if (abuf) {
			switch_size_t olen = raw_codec.implementation->samples_per_packet;

			if (switch_core_file_read(&fh, abuf, &olen) != SWITCH_STATUS_SUCCESS) {
				break;
			}

			write_frame.samples = (uint32_t) olen;
			write_frame.datalen = (uint32_t) (olen * sizeof(int16_t));
			if ((status = switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0)) != SWITCH_STATUS_SUCCESS) {
				break;
			}
		}

		if (countdown) {
			if (!--countdown) {
				break;
			} else {
				continue;
			}
		}

		data = (int16_t *) read_frame->data;

		for (energy = 0, j = 0, count = 0; count < read_frame->samples; count++) {
			energy += abs(data[j++]);
			j += channels;
		}

		score = (uint32_t) (energy / (read_frame->samples / divisor));

		if (score >= thresh) {
			listening++;
		}

		if (listening > listen_hits && score < thresh) {
			if (!--silence_hits) {
				countdown = 25;
			}
		} else {
			silence_hits = org_silence_hits;
		}
	}

	switch_core_session_reset(session, SWITCH_FALSE, SWITCH_TRUE);
	switch_core_codec_destroy(&raw_codec);

  end:

	if (abuf) {
		switch_core_file_close(&fh);
		free(abuf);
	}

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_read(switch_core_session_t *session,
												uint32_t min_digits,
												uint32_t max_digits,
												const char *prompt_audio_file,
												const char *var_name,
												char *digit_buffer, 
												switch_size_t digit_buffer_length, 
												uint32_t timeout, 
												const char *valid_terminators,
												uint32_t digit_timeout)

{
	switch_channel_t *channel;
	switch_input_args_t args = { 0 };
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	size_t len = 0;
	char tb[2] = "";

	switch_assert(session);

	if (!digit_timeout) {
		digit_timeout = timeout;
	}

	if (max_digits < min_digits) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "Max digits %u is less than Min %u, forcing Max to %u\n", max_digits, min_digits, min_digits);
		max_digits = min_digits;
	}

	channel = switch_core_session_get_channel(session);
	switch_channel_set_variable(channel, SWITCH_READ_RESULT_VARIABLE, NULL);

	if (var_name) {
		switch_channel_set_variable(channel, var_name, NULL);
	}

	if ((min_digits && digit_buffer_length < min_digits) || digit_buffer_length < max_digits) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Buffer too small!\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	memset(digit_buffer, 0, digit_buffer_length);
	args.buf = digit_buffer;
	args.buflen = (uint32_t) digit_buffer_length;

	if (!zstr(prompt_audio_file) && strcasecmp(prompt_audio_file, "silence")) {
		if ((status = switch_ivr_play_file(session, NULL, prompt_audio_file, &args)) == SWITCH_STATUS_BREAK) {
			status = SWITCH_STATUS_SUCCESS;
		}
	}

	if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
		goto end;
	}

	len = strlen(digit_buffer);

	if ((min_digits && len < min_digits) || len < max_digits) {
		args.buf = digit_buffer + len;
		args.buflen = (uint32_t) (digit_buffer_length - len);
		status = switch_ivr_collect_digits_count(session, digit_buffer, digit_buffer_length, max_digits, valid_terminators, &tb[0], 
												 len ? digit_timeout : timeout, digit_timeout, 0);
	}


	if (tb[0]) {
		char *p;

		switch_channel_set_variable(channel, SWITCH_READ_TERMINATOR_USED_VARIABLE, tb);

		if ((p = strchr(valid_terminators, tb[0]))) {
			if (p >= (valid_terminators + 1) && (*(p - 1) == '+' || *(p - 1) == 'x')) {
				switch_snprintf(digit_buffer + strlen(digit_buffer), digit_buffer_length - strlen(digit_buffer), "%s", tb);
				if (*(p - 1) == 'x') {
					status = SWITCH_STATUS_RESTART;
				}
			}
		}
	}

	len = strlen(digit_buffer);
	if ((min_digits && len < min_digits)) {
		status = SWITCH_STATUS_TOO_SMALL;
	}

	switch (status) {
	case SWITCH_STATUS_SUCCESS:
		switch_channel_set_variable(channel, SWITCH_READ_RESULT_VARIABLE, "success");
		break;
	case SWITCH_STATUS_TIMEOUT:
		switch_channel_set_variable(channel, SWITCH_READ_RESULT_VARIABLE, "timeout");
		break;
	default:
		switch_channel_set_variable(channel, SWITCH_READ_RESULT_VARIABLE, "failure");
		break;

	}

  end:

	if (status != SWITCH_STATUS_RESTART && max_digits == 1 && len == 1 && valid_terminators && strchr(valid_terminators, *digit_buffer)) {
		*digit_buffer = '\0';
	}

	if (var_name && !zstr(digit_buffer)) {
		switch_channel_set_variable(channel, var_name, digit_buffer);
	}

	return status;

}

SWITCH_DECLARE(switch_status_t) switch_play_and_get_digits(switch_core_session_t *session,
														   uint32_t min_digits,
														   uint32_t max_digits,
														   uint32_t max_tries,
														   uint32_t timeout,
														   const char *valid_terminators,
														   const char *prompt_audio_file,
														   const char *bad_input_audio_file,
														   const char *var_name,
														   char *digit_buffer, 
														   uint32_t digit_buffer_length, 
														   const char *digits_regex,
														   uint32_t digit_timeout,
														   const char *transfer_on_failure)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	while (switch_channel_ready(channel) && max_tries) {
		switch_status_t status;

		memset(digit_buffer, 0, digit_buffer_length);

		status = switch_ivr_read(session, min_digits, max_digits, prompt_audio_file, var_name,
								 digit_buffer, digit_buffer_length, timeout, valid_terminators, digit_timeout);

		if (status == SWITCH_STATUS_RESTART) {
			return status;
		}

		if (status == SWITCH_STATUS_TIMEOUT && strlen(digit_buffer) >= min_digits) {
			status = SWITCH_STATUS_SUCCESS;
		}

		if ((min_digits == 0) && (strlen(digit_buffer) == 0) && switch_channel_get_variable(channel, SWITCH_READ_TERMINATOR_USED_VARIABLE) != 0)
		{
			return SWITCH_STATUS_SUCCESS;
		}

		if (!(status == SWITCH_STATUS_TOO_SMALL && strlen(digit_buffer) == 0)) {
			if (status == SWITCH_STATUS_SUCCESS) {
				if (!zstr(digit_buffer)) {
					if (zstr(digits_regex)) {
						return SWITCH_STATUS_SUCCESS;
					}
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Test Regex [%s][%s]\n", digit_buffer, digits_regex);
					if (switch_regex_match(digit_buffer, digits_regex) == SWITCH_STATUS_SUCCESS) {
						return SWITCH_STATUS_SUCCESS;
					} else {
						switch_channel_set_variable(channel, var_name, NULL);
					}
				}
			}
		}

		if (!switch_channel_ready(channel)) {
			break;
		}

		switch_ivr_play_file(session, NULL, bad_input_audio_file, NULL);
		max_tries--;
	}

	memset(digit_buffer, 0, digit_buffer_length);
	
	/* If we get here then check for transfer-on-failure ext/dp/context */
	/* split this arg on spaces to get ext, dp, and context */
		
	if (!zstr(transfer_on_failure)) {
		const char *failure_ext = NULL;
		const char *failure_dialplan = NULL;
		const char *failure_context = NULL;
		char *target[4];
		char *mydata = switch_core_session_strdup(session, transfer_on_failure);
		int argc;
		
		argc = switch_separate_string(mydata, ' ', target, (sizeof(target) / sizeof(target[0])));
		
		if ( argc < 1 ) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,"Bad target for PAGD failure: [%s]\n", transfer_on_failure);
			return SWITCH_STATUS_FALSE;
		}
		
		if ( argc > 0 ) {
			failure_ext = target[0];
		}
		
		if ( argc > 1 ) {
			failure_dialplan = target[1];
		}

		if ( argc > 2 ) {
			failure_context = target[2];
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, 
			"PAGD failure! Transfer to: %s / %s / %s\n", failure_ext, failure_dialplan, failure_context);
			
		switch_ivr_session_transfer(session,failure_ext, failure_dialplan, failure_context);
		return SWITCH_STATUS_FALSE;
	} 
	
	return SWITCH_STATUS_FALSE;	
}

SWITCH_DECLARE(switch_status_t) switch_ivr_speak_text_handle(switch_core_session_t *session,
															 switch_speech_handle_t *sh,
															 switch_codec_t *codec, switch_timer_t *timer, char *text, switch_input_args_t *args)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	short abuf[960];
	switch_dtmf_t dtmf = { 0 };
	uint32_t len = 0;
	switch_size_t ilen = 0;
	switch_frame_t write_frame = { 0 };
	int x;
	int done = 0;
	int lead_in_out = 10;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_NONE;
	switch_size_t extra = 0;
	char *p, *tmp = NULL;
	const char *star, *pound;
	switch_size_t starlen, poundlen;

	if (!sh) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	write_frame.data = abuf;
	write_frame.buflen = sizeof(abuf);

	len = sh->samples * 2;

	flags = 0;

	if (!(star = switch_channel_get_variable(channel, "star_replace"))) {
		star = "star";
	}
	if (!(pound = switch_channel_get_variable(channel, "pound_replace"))) {
		pound = "pound";
	}
	starlen = strlen(star);
	poundlen = strlen(pound);


	for (p = text; p && *p; p++) {
		if (*p == '*') {
			extra += starlen;
		} else if (*p == '#') {
			extra += poundlen;
		}
	}

	if (extra) {
		char *tp;
		switch_size_t mylen = strlen(text) + extra + 1;
		tmp = malloc(mylen);
		if (!tmp) {
			return SWITCH_STATUS_MEMERR;
		}
		memset(tmp, 0, mylen);
		tp = tmp;
		for (p = text; p && *p; p++) {
			if (*p == '*') {
				strncat(tp, star, starlen);
				tp += starlen;
			} else if (*p == '#') {
				strncat(tp, pound, poundlen);
				tp += poundlen;
			} else {
				*tp++ = *p;
			}
		}

		text = tmp;
	}

	switch_core_speech_feed_tts(sh, text, &flags);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Speaking text: %s\n", text);
	switch_safe_free(tmp);
	text = NULL;

	write_frame.rate = sh->rate;

	memset(write_frame.data, 0, len);
	write_frame.datalen = len;
	write_frame.samples = len / 2;
	write_frame.codec = codec;

	switch_assert(codec->implementation != NULL);

	for (x = 0; !done && x < lead_in_out; x++) {
		switch_yield(codec->implementation->microseconds_per_packet);
		if (timer) {
			write_frame.timestamp = timer->samplecount;
		}
		if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
			done = 1;
			break;
		}
	}

	ilen = len;
	for (;;) {
		switch_event_t *event;

		if (!switch_channel_ready(channel)) {
			status = SWITCH_STATUS_FALSE;
			break;
		}

		if (switch_channel_test_flag(channel, CF_BREAK)) {
			switch_channel_clear_flag(channel, CF_BREAK);
			status = SWITCH_STATUS_BREAK;
			break;
		}

		if (switch_core_session_dequeue_private_event(session, &event) == SWITCH_STATUS_SUCCESS) {
			switch_ivr_parse_event(session, event);
			switch_event_destroy(&event);
		}

		if (args) {
			/* dtmf handler function you can hook up to be executed when a digit is dialed during playback 
			 * if you return anything but SWITCH_STATUS_SUCCESS the playback will stop.
			 */
			if (switch_channel_has_dtmf(channel)) {
				if (!args->input_callback && !args->buf && !args->dmachine) {
					status = SWITCH_STATUS_BREAK;
					done = 1;
					break;
				}
				if (args->buf && !strcasecmp(args->buf, "_break_")) {
					status = SWITCH_STATUS_BREAK;
				} else {
					switch_channel_dequeue_dtmf(channel, &dtmf);

					if (args->dmachine) {
						char ds[2] = {dtmf.digit, '\0'};
						if ((status = switch_ivr_dmachine_feed(args->dmachine, ds, NULL)) != SWITCH_STATUS_SUCCESS) {
							break;
						}
					} else if (args->input_callback) {
						status = args->input_callback(session, (void *) &dtmf, SWITCH_INPUT_TYPE_DTMF, args->buf, args->buflen);
					} else {
						*((char *) args->buf) = dtmf.digit;
						status = SWITCH_STATUS_BREAK;
					}
				}
			}

			if (args->input_callback) {
				if (switch_core_session_dequeue_event(session, &event, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
					status = args->input_callback(session, event, SWITCH_INPUT_TYPE_EVENT, args->buf, args->buflen);
					switch_event_destroy(&event);
				}
			}

			if (status != SWITCH_STATUS_SUCCESS) {
				done = 1;
				break;
			}
		}

		if (switch_test_flag(sh, SWITCH_SPEECH_FLAG_PAUSE)) {
			if (timer) {
				if (switch_core_timer_next(timer) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			} else {
				switch_frame_t *read_frame;
				switch_status_t tstatus = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

				while (switch_channel_ready(channel) && switch_channel_test_flag(channel, CF_HOLD)) {
					switch_yield(10000);
				}

				if (!SWITCH_READ_ACCEPTABLE(tstatus)) {
					break;
				}

				if (args && args->dmachine) {
					if ((status = switch_ivr_dmachine_ping(args->dmachine, NULL)) != SWITCH_STATUS_SUCCESS) {
						goto done;
					}
				}

				if (args && (args->read_frame_callback)) {
					if ((status = args->read_frame_callback(session, read_frame, args->user_data)) != SWITCH_STATUS_SUCCESS) {
						goto done;
					}
				}
			}
			continue;
		}

		flags = SWITCH_SPEECH_FLAG_BLOCKING;
		status = switch_core_speech_read_tts(sh, abuf, &ilen, &flags);

		if (status != SWITCH_STATUS_SUCCESS) {
			write_frame.datalen = (uint32_t) codec->implementation->decoded_bytes_per_packet;
			write_frame.samples = (uint32_t) (write_frame.datalen / 2);
			memset(write_frame.data, 0, write_frame.datalen);
			for (x = 0; !done && x < lead_in_out; x++) {
				switch_yield(codec->implementation->microseconds_per_packet);
				if (timer) {
					write_frame.timestamp = timer->samplecount;
				}
				if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
					done = 1;
					break;
				}
			}
			if (status == SWITCH_STATUS_BREAK) {
				status = SWITCH_STATUS_SUCCESS;
			}
			done = 1;
		}

		if (done) {
			break;
		}

		write_frame.datalen = (uint32_t) ilen;
		write_frame.samples = (uint32_t) (ilen / 2);
		if (timer) {
			write_frame.timestamp = timer->samplecount;
		}
		if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
			done = 1;
			break;
		}

		if (done) {
			break;
		}

		if (timer) {
			if (switch_core_timer_next(timer) != SWITCH_STATUS_SUCCESS) {
				break;
			}
		} else {				/* time off the channel (if you must) */
			switch_frame_t *read_frame;
			switch_status_t tstatus = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

			while (switch_channel_ready(channel) && switch_channel_test_flag(channel, CF_HOLD)) {
				switch_yield(10000);
			}

			if (!SWITCH_READ_ACCEPTABLE(tstatus)) {
				break;
			}

			if (args && args->dmachine) {
				if ((status = switch_ivr_dmachine_ping(args->dmachine, NULL)) != SWITCH_STATUS_SUCCESS) {
					goto done;
				}
			}

			if (args && (args->read_frame_callback)) {
				if ((status = args->read_frame_callback(session, read_frame, args->user_data)) != SWITCH_STATUS_SUCCESS) {
					goto done;
				}
			}
		}

	}

 done:

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "done speaking text\n");
	flags = 0;
	switch_core_speech_flush_tts(sh);
	return status;
}

struct cached_speech_handle {
	char tts_name[80];
	char voice_name[80];
	switch_speech_handle_t sh;
	switch_codec_t codec;
	switch_timer_t timer;
};

typedef struct cached_speech_handle cached_speech_handle_t;

SWITCH_DECLARE(void) switch_ivr_clear_speech_cache(switch_core_session_t *session)
{
	cached_speech_handle_t *cache_obj = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if ((cache_obj = switch_channel_get_private(channel, SWITCH_CACHE_SPEECH_HANDLES_OBJ_NAME))) {
		switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_NONE;
		if (cache_obj->timer.interval) {
			switch_core_timer_destroy(&cache_obj->timer);
		}
		switch_core_speech_close(&cache_obj->sh, &flags);
		switch_core_codec_destroy(&cache_obj->codec);
		switch_channel_set_private(channel, SWITCH_CACHE_SPEECH_HANDLES_OBJ_NAME, NULL);
	}
}

SWITCH_DECLARE(switch_status_t) switch_ivr_speak_text(switch_core_session_t *session,
													  const char *tts_name, const char *voice_name, char *text, switch_input_args_t *args)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	uint32_t rate = 0;
	int interval = 0;
	switch_frame_t write_frame = { 0 };
	switch_timer_t ltimer, *timer;
	switch_codec_t lcodec, *codec;
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);
	char *codec_name;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_speech_handle_t lsh, *sh;
	switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_NONE;
	const char *timer_name, *var;
	cached_speech_handle_t *cache_obj = NULL;
	int need_create = 1, need_alloc = 1;
	switch_codec_implementation_t read_impl = { 0 };
	switch_core_session_get_read_impl(session, &read_impl);

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	sh = &lsh;
	codec = &lcodec;
	timer = &ltimer;

	if ((var = switch_channel_get_variable(channel, SWITCH_CACHE_SPEECH_HANDLES_VARIABLE)) && switch_true(var)) {
		if ((cache_obj = switch_channel_get_private(channel, SWITCH_CACHE_SPEECH_HANDLES_OBJ_NAME))) {
			need_create = 0;
			if (!strcasecmp(cache_obj->tts_name, tts_name)) {
				need_alloc = 0;
			} else {
				switch_ivr_clear_speech_cache(session);
			}
		}

		if (!cache_obj) {
			cache_obj = switch_core_session_alloc(session, sizeof(*cache_obj));
		}
		if (need_alloc) {
			switch_copy_string(cache_obj->tts_name, tts_name, sizeof(cache_obj->tts_name));
			switch_copy_string(cache_obj->voice_name, voice_name, sizeof(cache_obj->voice_name));
			switch_channel_set_private(channel, SWITCH_CACHE_SPEECH_HANDLES_OBJ_NAME, cache_obj);
		}
		sh = &cache_obj->sh;
		codec = &cache_obj->codec;
		timer = &cache_obj->timer;
	}

	timer_name = switch_channel_get_variable(channel, "timer_name");

	switch_core_session_reset(session, SWITCH_FALSE, SWITCH_FALSE);

	rate = read_impl.actual_samples_per_second;
	interval = read_impl.microseconds_per_packet / 1000;

	if (need_create) {
		memset(sh, 0, sizeof(*sh));
		if ((status = switch_core_speech_open(sh, tts_name, voice_name, (uint32_t) rate, interval, &flags, NULL)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid TTS module!\n");
			switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
			switch_ivr_clear_speech_cache(session);
			return status;
		}
	} else if (cache_obj && strcasecmp(cache_obj->voice_name, voice_name)) {
		switch_copy_string(cache_obj->voice_name, voice_name, sizeof(cache_obj->voice_name));
		switch_core_speech_text_param_tts(sh, "voice", voice_name);
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		flags = 0;
		switch_core_speech_close(sh, &flags);
		return SWITCH_STATUS_FALSE;
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "OPEN TTS %s\n", tts_name);

	codec_name = "L16";

	if (need_create) {
		if (switch_core_codec_init(codec,
								   codec_name,
								   NULL, (int) rate, interval, 1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
								   pool) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Raw Codec Activated\n");
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Raw Codec Activation Failed %s@%uhz 1 channel %dms\n", codec_name,
							  rate, interval);
			flags = 0;
			switch_core_speech_close(sh, &flags);
			switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
			switch_ivr_clear_speech_cache(session);
			return SWITCH_STATUS_GENERR;
		}
	}

	write_frame.codec = codec;

	if (timer_name) {
		if (need_create) {
			if (switch_core_timer_init(timer, timer_name, interval, (int) sh->samples, pool) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Setup timer failed!\n");
				switch_core_codec_destroy(write_frame.codec);
				flags = 0;
				switch_core_speech_close(sh, &flags);
				switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
				switch_ivr_clear_speech_cache(session);
				return SWITCH_STATUS_GENERR;
			}
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Setup timer success %u bytes per %d ms!\n", sh->samples * 2,
							  interval);
		}
		switch_core_timer_sync(timer); // Sync timer

		/* start a thread to absorb incoming audio */
		switch_core_service_session(session);

	}

	status = switch_ivr_speak_text_handle(session, sh, write_frame.codec, timer_name ? timer : NULL, text, args);
	flags = 0;

	if (!cache_obj) {
		switch_core_speech_close(sh, &flags);
		switch_core_codec_destroy(codec);
	}

	if (timer_name) {
		/* End the audio absorbing thread */
		switch_core_thread_session_end(session);
		if (!cache_obj) {
			switch_core_timer_destroy(timer);
		}
	}

	switch_core_session_reset(session, SWITCH_FALSE, SWITCH_TRUE);
	return status;
}


static switch_status_t hold_on_dtmf(switch_core_session_t *session, void *input, switch_input_type_t itype, void *buf, unsigned int buflen)
{
	char *stop_key = (char *) buf;

	switch (itype) {
	case SWITCH_INPUT_TYPE_DTMF:
		{
			switch_dtmf_t *dtmf = (switch_dtmf_t *) input;
			if (dtmf->digit == *stop_key) {
				return SWITCH_STATUS_BREAK;
			}
		}
		break;
	default:
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_ivr_soft_hold(switch_core_session_t *session, const char *unhold_key, const char *moh_a, const char *moh_b)
{
	switch_channel_t *channel, *other_channel;
	switch_core_session_t *other_session;
	const char *other_uuid, *moh = NULL;
	int moh_br = 0;
	switch_input_args_t args = { 0 };
	args.input_callback = hold_on_dtmf;
	args.buf = (void *) unhold_key;
	args.buflen = (uint32_t) strlen(unhold_key);

	switch_assert(session != NULL);
	channel = switch_core_session_get_channel(session);
	switch_assert(channel != NULL);

	if ((other_uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
		if ((other_session = switch_core_session_locate(other_uuid))) {
			other_channel = switch_core_session_get_channel(other_session);

			if (moh_b) {
				moh = moh_b;
			} else {
				moh = switch_channel_get_hold_music(other_channel);
			}

			if (!zstr(moh) && strcasecmp(moh, "silence") && !switch_channel_test_flag(other_channel, CF_BROADCAST)) {
				switch_ivr_broadcast(other_uuid, moh, SMF_ECHO_ALEG | SMF_LOOP);
				moh_br++;
			}

			if (moh_a) {
				moh = moh_a;
			} else {
				moh = switch_channel_get_hold_music(channel);
			}

			if (!zstr(moh) && strcasecmp(moh, "silence")) {
				switch_ivr_play_file(session, NULL, moh, &args);
			} else {
				switch_ivr_collect_digits_callback(session, &args, 0, 0);
			}

			if (moh_br) {
				switch_channel_stop_broadcast(other_channel);
			}

			switch_core_session_rwunlock(other_session);


			return SWITCH_STATUS_SUCCESS;
		}

	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Channel %s is not in a bridge\n", switch_channel_get_name(channel));
	return SWITCH_STATUS_FALSE;

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
