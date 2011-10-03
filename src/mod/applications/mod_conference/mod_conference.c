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
 * Neal Horman <neal at wanlink dot com>
 * Bret McDanel <trixter at 0xdecafbad dot com>
 * Dale Thatcher <freeswitch at dalethatcher dot com>
 * Chris Danielson <chris at maxpowersoft dot com>
 * Rupa Schomaker <rupa@rupa.com>
 * David Weekly <david@weekly.org>
 * Joao Mesquita <jmesquita@gmail.com>
 *
 * mod_conference.c -- Software Conference Bridge
 *
 */
#include <switch.h>
#define DEFAULT_AGC_LEVEL 1100
#define CONFERENCE_UUID_VARIABLE "conference_uuid"

SWITCH_MODULE_LOAD_FUNCTION(mod_conference_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_conference_shutdown);
SWITCH_MODULE_DEFINITION(mod_conference, mod_conference_load, mod_conference_shutdown, NULL);

typedef enum {
	CONF_SILENT_REQ = (1 << 0),
	CONF_SILENT_DONE = (1 << 1)
} conf_app_flag_t;

static const char global_app_name[] = "conference";
static char *global_cf_name = "conference.conf";
static char *cf_pin_url_param_name = "X-ConfPin=";
static char *api_syntax;
static int EC = 0;

/* Size to allocate for audio buffers */
#define CONF_BUFFER_SIZE 1024 * 128
#define CONF_EVENT_MAINT "conference::maintenance"
#define CONF_DEFAULT_LEADIN 20

#define CONF_DBLOCK_SIZE CONF_BUFFER_SIZE
#define CONF_DBUFFER_SIZE CONF_BUFFER_SIZE
#define CONF_DBUFFER_MAX 0
#define CONF_CHAT_PROTO "conf"

#ifndef MIN
#define MIN(a, b) ((a)<(b)?(a):(b))
#endif

/* the rate at which the infinite impulse response filter on speaker score will decay. */
#define SCORE_DECAY 0.8
/* the maximum value for the IIR score [keeps loud & longwinded people from getting overweighted] */
#define SCORE_MAX_IIR 25000
/* the minimum score for which you can be considered to be loud enough to now have the floor */
#define SCORE_IIR_SPEAKING_MAX 3000
/* the threshold below which you cede the floor to someone loud (see above value). */
#define SCORE_IIR_SPEAKING_MIN 100


#define test_eflag(conference, flag) ((conference)->eflags & flag)

typedef enum {
	FILE_STOP_CURRENT,
	FILE_STOP_ALL,
	FILE_STOP_ASYNC
} file_stop_t;

/* Global Values */
static struct {
	switch_memory_pool_t *conference_pool;
	switch_mutex_t *conference_mutex;
	switch_hash_t *conference_hash;
	switch_mutex_t *id_mutex;
	switch_mutex_t *hash_mutex;
	switch_mutex_t *setup_mutex;
	uint32_t id_pool;
	int32_t running;
	uint32_t threads;
	switch_event_node_t *node;
} globals;

/* forward declaration for conference_obj and caller_control */
struct conference_member;
typedef struct conference_member conference_member_t;

struct call_list {
	char *string;
	int iteration;
	struct call_list *next;
};
typedef struct call_list call_list_t;

struct caller_control_actions;

typedef struct caller_control_actions {
	char *binded_dtmf;
	char *data;
	char *expanded_data;
} caller_control_action_t;

typedef struct caller_control_menu_info {
	switch_ivr_menu_t *stack;
	char *name;
} caller_control_menu_info_t;

typedef enum {
	MFLAG_RUNNING = (1 << 0),
	MFLAG_CAN_SPEAK = (1 << 1),
	MFLAG_CAN_HEAR = (1 << 2),
	MFLAG_KICKED = (1 << 3),
	MFLAG_ITHREAD = (1 << 4),
	MFLAG_NOCHANNEL = (1 << 5),
	MFLAG_INTREE = (1 << 6),
	MFLAG_WASTE_BANDWIDTH = (1 << 7),
	MFLAG_FLUSH_BUFFER = (1 << 8),
	MFLAG_ENDCONF = (1 << 9),
	MFLAG_HAS_AUDIO = (1 << 10),
	MFLAG_TALKING = (1 << 11),
	MFLAG_RESTART = (1 << 12),
	MFLAG_MINTWO = (1 << 13),
	MFLAG_MUTE_DETECT = (1 << 14),
	MFLAG_DIST_DTMF = (1 << 15),
	MFLAG_MOD = (1 << 16),
	MFLAG_INDICATE_MUTE = (1 << 17),
	MFLAG_INDICATE_UNMUTE = (1 << 18),
	MFLAG_NOMOH = (1 << 19),
	MFLAG_VIDEO_BRIDGE = (1 << 20),
	MFLAG_INDICATE_MUTE_DETECT = (1 << 21)
} member_flag_t;

typedef enum {
	CFLAG_RUNNING = (1 << 0),
	CFLAG_DYNAMIC = (1 << 1),
	CFLAG_ENFORCE_MIN = (1 << 2),
	CFLAG_DESTRUCT = (1 << 3),
	CFLAG_LOCKED = (1 << 4),
	CFLAG_ANSWERED = (1 << 5),
	CFLAG_BRIDGE_TO = (1 << 6),
	CFLAG_WAIT_MOD = (1 << 7),
	CFLAG_VID_FLOOR = (1 << 8),
	CFLAG_WASTE_BANDWIDTH = (1 << 9),
	CFLAG_OUTCALL = (1 << 10),
	CFLAG_INHASH = (1 << 11),
	CFLAG_EXIT_SOUND = (1 << 12),
	CFLAG_ENTER_SOUND = (1 << 13),
	CFLAG_VIDEO_BRIDGE = (1 << 14)
} conf_flag_t;

typedef enum {
	RFLAG_CAN_SPEAK = (1 << 0),
	RFLAG_CAN_HEAR = (1 << 1)
} relation_flag_t;

typedef enum {
	NODE_TYPE_FILE,
	NODE_TYPE_SPEECH
} node_type_t;

typedef enum {
	EFLAG_ADD_MEMBER = (1 << 0),
	EFLAG_DEL_MEMBER = (1 << 1),
	EFLAG_ENERGY_LEVEL = (1 << 2),
	EFLAG_VOLUME_LEVEL = (1 << 3),
	EFLAG_GAIN_LEVEL = (1 << 4),
	EFLAG_DTMF = (1 << 5),
	EFLAG_STOP_TALKING = (1 << 6),
	EFLAG_START_TALKING = (1 << 7),
	EFLAG_MUTE_MEMBER = (1 << 8),
	EFLAG_UNMUTE_MEMBER = (1 << 9),
	EFLAG_DEAF_MEMBER = (1 << 10),
	EFLAG_UNDEAF_MEMBER = (1 << 11),
	EFLAG_KICK_MEMBER = (1 << 12),
	EFLAG_DTMF_MEMBER = (1 << 13),
	EFLAG_ENERGY_LEVEL_MEMBER = (1 << 14),
	EFLAG_VOLUME_IN_MEMBER = (1 << 15),
	EFLAG_VOLUME_OUT_MEMBER = (1 << 16),
	EFLAG_PLAY_FILE = (1 << 17),
	EFLAG_PLAY_FILE_MEMBER = (1 << 18),
	EFLAG_SPEAK_TEXT = (1 << 19),
	EFLAG_SPEAK_TEXT_MEMBER = (1 << 20),
	EFLAG_LOCK = (1 << 21),
	EFLAG_UNLOCK = (1 << 22),
	EFLAG_TRANSFER = (1 << 23),
	EFLAG_BGDIAL_RESULT = (1 << 24),
	EFLAG_FLOOR_CHANGE = (1 << 25),
	EFLAG_MUTE_DETECT = (1 << 26),
	EFLAG_RECORD = (1 << 27),
	EFLAG_HUP_MEMBER = (1 << 28)
} event_type_t;

typedef struct conference_file_node {
	switch_file_handle_t fh;
	switch_speech_handle_t *sh;
	node_type_t type;
	uint8_t done;
	uint8_t async;
	switch_memory_pool_t *pool;
	uint32_t leadin;
	struct conference_file_node *next;
	char *file;
} conference_file_node_t;

/* conference xml config sections */
typedef struct conf_xml_cfg {
	switch_xml_t profile;
	switch_xml_t controls;
} conf_xml_cfg_t;

/* Conference Object */
typedef struct conference_obj {
	char *name;
	char *timer_name;
	char *tts_engine;
	char *tts_voice;
	char *enter_sound;
	char *exit_sound;
	char *alone_sound;
	char *perpetual_sound;
	char *moh_sound;
	char *ack_sound;
	char *nack_sound;
	char *muted_sound;
	char *mute_detect_sound;
	char *unmuted_sound;
	char *locked_sound;
	char *is_locked_sound;
	char *is_unlocked_sound;
	char *kicked_sound;
	char *caller_id_name;
	char *caller_id_number;
	char *sound_prefix;
	char *special_announce;
	char *auto_record;
	uint32_t terminate_on_silence;
	uint32_t max_members;
	char *maxmember_sound;
	uint32_t announce_count;
	char *pin;
	char *mpin;
	char *pin_sound;
	char *bad_pin_sound;
	char *profile_name;
	char *domain;
	char *caller_controls;
	char *moderator_controls;
	uint32_t flags;
	member_flag_t mflags;
	switch_call_cause_t bridge_hangup_cause;
	switch_mutex_t *flag_mutex;
	uint32_t rate;
	uint32_t interval;
	switch_mutex_t *mutex;
	conference_member_t *members;
	conference_member_t *floor_holder;
	switch_mutex_t *member_mutex;
	conference_file_node_t *fnode;
	conference_file_node_t *async_fnode;
	switch_memory_pool_t *pool;
	switch_thread_rwlock_t *rwlock;
	uint32_t count;
	int32_t energy_level;
	uint8_t min;
	switch_speech_handle_t lsh;
	switch_speech_handle_t *sh;
	switch_byte_t *not_talking_buf;
	uint32_t not_talking_buf_len;
	int pin_retries;
	int comfort_noise_level;
	int is_recording;
	int record_count;
	int video_running;
	int ivr_dtmf_timeout;
	int ivr_input_timeout;
	uint32_t eflags;
	uint32_t verbose_events;
	int end_count;
	uint32_t relationship_total;
	uint32_t score;
	int mux_loop_count;
	int member_loop_count;
	int agc_level;

	uint32_t avg_score;
	uint32_t avg_itt;
	uint32_t avg_tally;
	switch_time_t run_time;
	char *uuid_str;
	uint32_t originating;
	switch_call_cause_t cancel_cause;
} conference_obj_t;

/* Relationship with another member */
typedef struct conference_relationship {
	uint32_t id;
	uint32_t flags;
	struct conference_relationship *next;
} conference_relationship_t;

/* Conference Member Object */
struct conference_member {
	uint32_t id;
	switch_core_session_t *session;
	conference_obj_t *conference;
	switch_memory_pool_t *pool;
	switch_buffer_t *audio_buffer;
	switch_buffer_t *mux_buffer;
	switch_buffer_t *resample_buffer;
	uint32_t flags;
	uint32_t score;
	uint32_t last_score;
	uint32_t score_iir;
	switch_mutex_t *flag_mutex;
	switch_mutex_t *write_mutex;
	switch_mutex_t *audio_in_mutex;
	switch_mutex_t *audio_out_mutex;
	switch_mutex_t *read_mutex;
	switch_thread_rwlock_t *rwlock;
	switch_codec_implementation_t read_impl;
	switch_codec_implementation_t orig_read_impl;
	switch_codec_t read_codec;
	switch_codec_t write_codec;
	char *rec_path;
	uint8_t *frame;
	uint8_t *last_frame;
	uint32_t frame_size;
	uint8_t *mux_frame;
	uint32_t read;
	uint32_t vol_period;
	int32_t energy_level;
	int32_t agc_volume_in_level;
	int32_t volume_in_level;
	int32_t volume_out_level;
	int32_t agc_concur;
	int32_t nt_tally;
	switch_time_t join_time;
	switch_time_t last_talking;
	uint32_t native_rate;
	switch_audio_resampler_t *read_resampler;
	int16_t *resample_out;
	uint32_t resample_out_len;
	conference_file_node_t *fnode;
	conference_relationship_t *relationships;
	switch_speech_handle_t lsh;
	switch_speech_handle_t *sh;
	uint32_t verbose_events;
	uint32_t avg_score;
	uint32_t avg_itt;
	uint32_t avg_tally;
	struct conference_member *next;
	switch_ivr_dmachine_t *dmachine;
};

/* Record Node */
typedef struct conference_record {
	conference_obj_t *conference;
	char *path;
	switch_memory_pool_t *pool;
} conference_record_t;

typedef enum {
	CONF_API_SUB_ARGS_SPLIT,
	CONF_API_SUB_MEMBER_TARGET,
	CONF_API_SUB_ARGS_AS_ONE
} conference_fntype_t;

typedef void (*void_fn_t) (void);

/* API command parser */
typedef struct api_command {
	char *pname;
	void_fn_t pfnapicmd;
	conference_fntype_t fntype;
	char *pcommand;
	char *psyntax;
} api_command_t;

/* Function Prototypes */
static int setup_media(conference_member_t *member, conference_obj_t *conference);
static uint32_t next_member_id(void);
static conference_relationship_t *member_get_relationship(conference_member_t *member, conference_member_t *other_member);
static conference_member_t *conference_member_get(conference_obj_t *conference, uint32_t id);
static conference_relationship_t *member_add_relationship(conference_member_t *member, uint32_t id);
static switch_status_t member_del_relationship(conference_member_t *member, uint32_t id);
static switch_status_t conference_add_member(conference_obj_t *conference, conference_member_t *member);
static switch_status_t conference_del_member(conference_obj_t *conference, conference_member_t *member);
static void *SWITCH_THREAD_FUNC conference_thread_run(switch_thread_t *thread, void *obj);
static void *SWITCH_THREAD_FUNC conference_video_thread_run(switch_thread_t *thread, void *obj);
static void conference_loop_output(conference_member_t *member);
static uint32_t conference_stop_file(conference_obj_t *conference, file_stop_t stop);
static switch_status_t conference_play_file(conference_obj_t *conference, char *file, uint32_t leadin, switch_channel_t *channel, uint8_t async);
static void conference_send_all_dtmf(conference_member_t *member, conference_obj_t *conference, const char *dtmf);
static switch_status_t conference_say(conference_obj_t *conference, const char *text, uint32_t leadin);
static void conference_list(conference_obj_t *conference, switch_stream_handle_t *stream, char *delim);
static conference_obj_t *conference_find(char *name);
static void member_bind_controls(conference_member_t *member, const char *controls);

SWITCH_STANDARD_API(conf_api_main);

static switch_status_t conference_outcall(conference_obj_t *conference,
										  char *conference_name,
										  switch_core_session_t *session,
										  char *bridgeto, uint32_t timeout, 
										  char *flags, 
										  char *cid_name, 
										  char *cid_num,
										  char *profile,
										  switch_call_cause_t *cause,
										  switch_call_cause_t *cancel_cause);
static switch_status_t conference_outcall_bg(conference_obj_t *conference,
											 char *conference_name,
											 switch_core_session_t *session, char *bridgeto, uint32_t timeout, const char *flags, const char *cid_name,
											 const char *cid_num, const char *call_uuid, const char *profile, switch_call_cause_t *cancel_cause);
SWITCH_STANDARD_APP(conference_function);
static void launch_conference_thread(conference_obj_t *conference);
static void launch_conference_video_thread(conference_obj_t *conference);
static void *SWITCH_THREAD_FUNC conference_loop_input(switch_thread_t *thread, void *obj);
static switch_status_t conference_local_play_file(conference_obj_t *conference, switch_core_session_t *session, char *path, uint32_t leadin, void *buf,
												  uint32_t buflen);
static switch_status_t conference_member_play_file(conference_member_t *member, char *file, uint32_t leadin);
static switch_status_t conference_member_say(conference_member_t *member, char *text, uint32_t leadin);
static uint32_t conference_member_stop_file(conference_member_t *member, file_stop_t stop);
static conference_obj_t *conference_new(char *name, conf_xml_cfg_t cfg, switch_core_session_t *session, switch_memory_pool_t *pool);
static switch_status_t chat_send(switch_event_t *message_event);
								 

static void launch_conference_record_thread(conference_obj_t *conference, char *path);
static void launch_conference_video_bridge_thread(conference_member_t *member_a, conference_member_t *member_b);

typedef switch_status_t (*conf_api_args_cmd_t) (conference_obj_t *, switch_stream_handle_t *, int, char **);
typedef switch_status_t (*conf_api_member_cmd_t) (conference_member_t *, switch_stream_handle_t *, void *);
typedef switch_status_t (*conf_api_text_cmd_t) (conference_obj_t *, switch_stream_handle_t *, const char *);

static void conference_member_itterator(conference_obj_t *conference, switch_stream_handle_t *stream, conf_api_member_cmd_t pfncallback, void *data);
static switch_status_t conf_api_sub_mute(conference_member_t *member, switch_stream_handle_t *stream, void *data);
static switch_status_t conf_api_sub_unmute(conference_member_t *member, switch_stream_handle_t *stream, void *data);
static switch_status_t conf_api_sub_deaf(conference_member_t *member, switch_stream_handle_t *stream, void *data);
static switch_status_t conf_api_sub_undeaf(conference_member_t *member, switch_stream_handle_t *stream, void *data);
static switch_status_t conference_add_event_data(conference_obj_t *conference, switch_event_t *event);
static switch_status_t conference_add_event_member_data(conference_member_t *member, switch_event_t *event);


#define lock_member(_member) switch_mutex_lock(_member->write_mutex); switch_mutex_lock(_member->read_mutex)
#define unlock_member(_member) switch_mutex_unlock(_member->read_mutex); switch_mutex_unlock(_member->write_mutex)

//#define lock_member(_member) switch_mutex_lock(_member->write_mutex)
//#define unlock_member(_member) switch_mutex_unlock(_member->write_mutex)

static switch_status_t conference_add_event_data(conference_obj_t *conference, switch_event_t *event)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Conference-Name", conference->name);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Conference-Size", "%u", conference->count);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Conference-Profile-Name", conference->profile_name);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Conference-Unique-ID", conference->uuid_str);

	return status;
}

static switch_status_t conference_add_event_member_data(conference_member_t *member, switch_event_t *event)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!member)
		return status;

	if (member->conference) {
		status = conference_add_event_data(member->conference, event);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Floor", "%s", (member == member->conference->floor_holder) ? "true" : "false" );
	}

	if (member->session) {
		switch_channel_t *channel = switch_core_session_get_channel(member->session);

		if (member->verbose_events) {
			switch_channel_event_set_data(channel, event);
		} else {
			switch_channel_event_set_basic_data(channel, event);
		}
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Video", "%s",
								switch_channel_test_flag(switch_core_session_get_channel(member->session), CF_VIDEO) ? "true" : "false" );

	}

	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Hear", "%s", switch_test_flag(member, MFLAG_CAN_HEAR) ? "true" : "false" );
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Speak", "%s", switch_test_flag(member, MFLAG_CAN_SPEAK) ? "true" : "false" );
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Talking", "%s", switch_test_flag(member, MFLAG_TALKING) ? "true" : "false" );
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Mute-Detect", "%s", switch_test_flag(member, MFLAG_MUTE_DETECT) ? "true" : "false" );
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Member-ID", "%u", member->id);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Member-Type", "%s", switch_test_flag(member, MFLAG_MOD) ? "moderator" : "member");
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Energy-Level", "%d", member->energy_level);

	return status;
}

/* Return a Distinct ID # */
static uint32_t next_member_id(void)
{
	uint32_t id;

	switch_mutex_lock(globals.id_mutex);
	id = ++globals.id_pool;
	switch_mutex_unlock(globals.id_mutex);

	return id;
}

/* if other_member has a relationship with member, produce it */
static conference_relationship_t *member_get_relationship(conference_member_t *member, conference_member_t *other_member)
{
	conference_relationship_t *rel = NULL, *global = NULL;

	if (member == NULL || other_member == NULL || member->relationships == NULL)
		return NULL;

	lock_member(member);
	lock_member(other_member);

	for (rel = member->relationships; rel; rel = rel->next) {
		if (rel->id == other_member->id) {
			break;
		}

		/* 0 matches everyone. (We will still test the others because a real match carries more clout) */
		if (rel->id == 0) {
			global = rel;
		}
	}

	unlock_member(other_member);
	unlock_member(member);

	return rel ? rel : global;
}

/* traverse the conference member list for the specified member id and return it's pointer */
static conference_member_t *conference_member_get(conference_obj_t *conference, uint32_t id)
{
	conference_member_t *member = NULL;

	switch_assert(conference != NULL);
	if (!id) {
		return NULL;
	}

	switch_mutex_lock(conference->member_mutex);
	for (member = conference->members; member; member = member->next) {

		if (switch_test_flag(member, MFLAG_NOCHANNEL)) {
			continue;
		}

		if (member->id == id) {
			break;
		}
	}

	if (member && !switch_test_flag(member, MFLAG_INTREE)) {
		member = NULL;
	}

	if (member) {
		switch_thread_rwlock_rdlock(member->rwlock);
	}

	switch_mutex_unlock(conference->member_mutex);

	return member;
}

/* stop the specified recording */
static switch_status_t conference_record_stop(conference_obj_t *conference, char *path)
{
	conference_member_t *member = NULL;
	int count = 0;

	switch_assert(conference != NULL);
	switch_mutex_lock(conference->member_mutex);
	for (member = conference->members; member; member = member->next) {
		if (switch_test_flag(member, MFLAG_NOCHANNEL) && (!path || !strcmp(path, member->rec_path))) {
			switch_clear_flag_locked(member, MFLAG_RUNNING);
			count++;
		}
	}
	switch_mutex_unlock(conference->member_mutex);
	return count;
}


/* Add a custom relationship to a member */
static conference_relationship_t *member_add_relationship(conference_member_t *member, uint32_t id)
{
	conference_relationship_t *rel = NULL;

	if (member == NULL || id == 0 || !(rel = switch_core_alloc(member->pool, sizeof(*rel))))
		return NULL;

	rel->id = id;


	lock_member(member);
	switch_mutex_lock(member->conference->member_mutex);
	member->conference->relationship_total++;
	switch_mutex_unlock(member->conference->member_mutex);
	rel->next = member->relationships;
	member->relationships = rel;
	unlock_member(member);

	return rel;
}

/* Remove a custom relationship from a member */
static switch_status_t member_del_relationship(conference_member_t *member, uint32_t id)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	conference_relationship_t *rel, *last = NULL;

	if (member == NULL || id == 0)
		return status;

	lock_member(member);
	for (rel = member->relationships; rel; rel = rel->next) {
		if (rel->id == id) {
			/* we just forget about rel here cos it was allocated by the member's pool 
			   it will be freed when the member is */
			status = SWITCH_STATUS_SUCCESS;
			if (last) {
				last->next = rel->next;
			} else {
				member->relationships = rel->next;
			}

			switch_mutex_lock(member->conference->member_mutex);
			member->conference->relationship_total--;
			switch_mutex_unlock(member->conference->member_mutex);

		}
		last = rel;
	}
	unlock_member(member);

	return status;
}

/* Gain exclusive access and add the member to the list */
static switch_status_t conference_add_member(conference_obj_t *conference, conference_member_t *member)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_event_t *event;
	char msg[512];				/* conference count announcement */
	call_list_t *call_list = NULL;
	switch_channel_t *channel;
	const char *controls = NULL;

	switch_assert(conference != NULL);
	switch_assert(member != NULL);

	switch_mutex_lock(conference->mutex);
	switch_mutex_lock(member->audio_in_mutex);
	switch_mutex_lock(member->audio_out_mutex);
	lock_member(member);
	switch_mutex_lock(conference->member_mutex);


	member->join_time = switch_epoch_time_now(NULL);
	member->conference = conference;
	member->next = conference->members;
	member->energy_level = conference->energy_level;
	member->score_iir = 0;
	member->verbose_events = conference->verbose_events;
	conference->members = member;
	switch_set_flag_locked(member, MFLAG_INTREE);
	switch_mutex_unlock(conference->member_mutex);

	if (!switch_test_flag(member, MFLAG_NOCHANNEL)) {
		conference->count++;

		if (switch_test_flag(member, MFLAG_ENDCONF)) {
			if (conference->end_count++);
		}

		if (switch_event_create(&event, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", CONF_CHAT_PROTO);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", conference->name);
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "from", "%s@%s", conference->name, conference->domain);
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "status", "Active (%d caller%s)", conference->count, conference->count == 1 ? "" : "s");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "event_count", "%d", EC++);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique-id", conference->name);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "channel-state", "CS_ROUTING");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "answer-state", conference->count == 1 ? "early" : "confirmed");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "presence-call-direction", conference->count == 1 ? "outbound" : "inbound");
			switch_event_fire(&event);
		}

		channel = switch_core_session_get_channel(member->session);
		switch_channel_set_variable_printf(channel, "conference_member_id", "%d", member->id);
		switch_channel_set_variable_printf(channel, "conference_moderator", "%s", switch_test_flag(member, MFLAG_MOD) ? "true" : "false");
		switch_channel_set_variable(channel, CONFERENCE_UUID_VARIABLE, conference->uuid_str);
		
		if (switch_test_flag(conference, CFLAG_WAIT_MOD) && switch_test_flag(member, MFLAG_MOD)) {
			switch_clear_flag(conference, CFLAG_WAIT_MOD);
		}

		if (conference->count > 1) {
			if (conference->moh_sound && !switch_test_flag(conference, CFLAG_WAIT_MOD)) {
				/* stop MoH if any */
				conference_stop_file(conference, FILE_STOP_ASYNC);
			}

			if (!switch_channel_test_app_flag_key("conf_silent", channel, CONF_SILENT_REQ) && !zstr(conference->enter_sound)) {
                                const char * enter_sound = switch_channel_get_variable(channel, "conference_enter_sound");
				if (switch_test_flag(conference, CFLAG_ENTER_SOUND)) {
					if (!zstr(enter_sound)) {
				     	conference_play_file(conference, (char *)enter_sound, CONF_DEFAULT_LEADIN,
						     	switch_core_session_get_channel(member->session), !switch_test_flag(conference, CFLAG_WAIT_MOD) ? 0 : 1);
			        } else {	
				     		conference_play_file(conference, conference->enter_sound, CONF_DEFAULT_LEADIN, switch_core_session_get_channel(member->session),
								!switch_test_flag(conference, CFLAG_WAIT_MOD) ? 0 : 1);
					}
				}
			}
		}


		call_list = (call_list_t *) switch_channel_get_private(channel, "_conference_autocall_list_");

		if (call_list) {
			char saymsg[1024];
			switch_snprintf(saymsg, sizeof(saymsg), "Auto Calling %d parties", call_list->iteration);
			conference_member_say(member, saymsg, 0);
		} else {

			if (!switch_channel_test_app_flag_key("conf_silent", channel, CONF_SILENT_REQ)) {
				/* announce the total number of members in the conference */
				if (conference->count >= conference->announce_count && conference->announce_count > 1) {
					switch_snprintf(msg, sizeof(msg), "There are %d callers", conference->count);
					conference_member_say(member, msg, CONF_DEFAULT_LEADIN);
				} else if (conference->count == 1 && !conference->perpetual_sound && !switch_test_flag(conference, CFLAG_WAIT_MOD)) {
					/* as long as its not a bridge_to conference, announce if person is alone */
					if (!switch_test_flag(conference, CFLAG_BRIDGE_TO)) {
						if (conference->alone_sound) {
							conference_stop_file(conference, FILE_STOP_ASYNC);
							conference_play_file(conference, conference->alone_sound, CONF_DEFAULT_LEADIN,
												 switch_core_session_get_channel(member->session), 1);
						} else {
							switch_snprintf(msg, sizeof(msg), "You are currently the only person in this conference.");
							conference_member_say(member, msg, CONF_DEFAULT_LEADIN);
						}
					}

				}
			}
		}

		if (conference->count == 1) {
			conference->floor_holder = member;
		}

		if (conference->min && conference->count >= conference->min) {
			switch_set_flag(conference, CFLAG_ENFORCE_MIN);
		}

		if (!switch_channel_test_app_flag_key("conf_silent", channel, CONF_SILENT_REQ) &&
			switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_member_data(member, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "add-member");
			switch_event_fire(&event);
		}

		switch_channel_clear_app_flag_key("conf_silent", channel, CONF_SILENT_REQ);
		switch_channel_set_app_flag_key("conf_silent", channel, CONF_SILENT_DONE);

		switch_ivr_dmachine_create(&member->dmachine, "mod_conference", NULL, 
				conference->ivr_dtmf_timeout, conference->ivr_input_timeout, NULL, NULL, NULL);

		controls = switch_channel_get_variable(channel, "conference_controls");

		if (zstr(controls)) {
			if (!switch_test_flag(member, MFLAG_MOD) || !conference->moderator_controls) {
				controls = conference->caller_controls;
			} else {
				controls = conference->moderator_controls;
			}
		}

		if (zstr(controls)) {
			controls = "default";
		}

		if (strcasecmp(controls, "none")) {
			member_bind_controls(member, controls);
		}
		
	}
	unlock_member(member);
	switch_mutex_unlock(member->audio_out_mutex);
	switch_mutex_unlock(member->audio_in_mutex);

	switch_mutex_unlock(conference->mutex);
	status = SWITCH_STATUS_SUCCESS;

	return status;
}

/* Gain exclusive access and remove the member from the list */
static switch_status_t conference_del_member(conference_obj_t *conference, conference_member_t *member)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	conference_member_t *imember, *last = NULL;
	switch_event_t *event;
	conference_file_node_t *member_fnode;
	switch_speech_handle_t *member_sh;
	const char *exit_sound = NULL;

	switch_assert(conference != NULL);
	switch_assert(member != NULL);

	switch_thread_rwlock_wrlock(member->rwlock);

	if (member->session && (exit_sound = switch_channel_get_variable(switch_core_session_get_channel(member->session), "conference_exit_sound"))) {
		conference_play_file(conference, (char *)exit_sound, CONF_DEFAULT_LEADIN,
							 switch_core_session_get_channel(member->session), !switch_test_flag(conference, CFLAG_WAIT_MOD) ? 0 : 1);
	}


	lock_member(member);
	member_fnode = member->fnode;
	member_sh = member->sh;
	member->fnode = NULL;
	member->sh = NULL;
	unlock_member(member);

	switch_ivr_dmachine_destroy(&member->dmachine);

	switch_mutex_lock(conference->mutex);
	switch_mutex_lock(conference->member_mutex);
	switch_mutex_lock(member->audio_in_mutex);
	switch_mutex_lock(member->audio_out_mutex);
	lock_member(member);
	switch_clear_flag(member, MFLAG_INTREE);

	for (imember = conference->members; imember; imember = imember->next) {
		if (imember == member) {
			if (last) {
				last->next = imember->next;
			} else {
				conference->members = imember->next;
			}
			break;
		}
		last = imember;
	}

	switch_thread_rwlock_unlock(member->rwlock);
	
	/* Close Unused Handles */
	if (member_fnode) {
		conference_file_node_t *fnode, *cur;
		switch_memory_pool_t *pool;

		fnode = member_fnode;
		while (fnode) {
			cur = fnode;
			fnode = fnode->next;

			if (cur->type != NODE_TYPE_SPEECH) {
				switch_core_file_close(&cur->fh);
			}

			pool = cur->pool;
			switch_core_destroy_memory_pool(&pool);
		}
	}

	if (member_sh) {
		switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_NONE;
		switch_core_speech_close(&member->lsh, &flags);
	}

	if (member == member->conference->floor_holder) {
		member->conference->floor_holder = NULL;
	}

	member->conference = NULL;

	if (!switch_test_flag(member, MFLAG_NOCHANNEL)) {
		conference->count--;

		if (switch_test_flag(member, MFLAG_ENDCONF)) {
			if (!--conference->end_count) {
				switch_set_flag_locked(conference, CFLAG_DESTRUCT);
			}
		}


		if (switch_event_create(&event, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", CONF_CHAT_PROTO);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", conference->name);
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "from", "%s@%s", conference->name, conference->domain);
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "status", "Active (%d caller%s)", conference->count, conference->count == 1 ? "" : "s");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "event_count", "%d", EC++);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique-id", conference->name);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "channel-state", "CS_ROUTING");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "answer-state", conference->count == 1 ? "early" : "confirmed");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-direction", conference->count == 1 ? "outbound" : "inbound");
			switch_event_fire(&event);
		}

		if ((conference->min && switch_test_flag(conference, CFLAG_ENFORCE_MIN) && conference->count < conference->min)
			|| (switch_test_flag(conference, CFLAG_DYNAMIC) && conference->count == 0)) {
			switch_set_flag(conference, CFLAG_DESTRUCT);
		} else {
			if (!exit_sound && conference->exit_sound && switch_test_flag(conference, CFLAG_EXIT_SOUND)) {
				conference_play_file(conference, conference->exit_sound, 0, switch_core_session_get_channel(member->session), 0);
			}
			if (conference->count == 1 && conference->alone_sound && !switch_test_flag(conference, CFLAG_WAIT_MOD)) {
				conference_stop_file(conference, FILE_STOP_ASYNC);
				conference_play_file(conference, conference->alone_sound, 0, switch_core_session_get_channel(member->session), 1);
			}
		}

		if (conference->count == 1) {
			conference->floor_holder = conference->members;
		}

		if (test_eflag(conference, EFLAG_DEL_MEMBER) &&
			switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_member_data(member, event);
			conference_add_event_data(conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "del-member");
			switch_event_fire(&event);
		}
	}
	switch_mutex_unlock(conference->member_mutex);
	unlock_member(member);
	switch_mutex_unlock(member->audio_out_mutex);
	switch_mutex_unlock(member->audio_in_mutex);
	switch_mutex_unlock(conference->mutex);
	status = SWITCH_STATUS_SUCCESS;

	return status;
}

struct vid_helper {
	conference_member_t *member_a;
	conference_member_t *member_b;
	int up;
};

/* Thread bridging video between two members, there will be two threads if video briding is used */
static void *SWITCH_THREAD_FUNC conference_video_bridge_thread_run(switch_thread_t *thread, void *obj)
{
	struct vid_helper *vh = obj;
	switch_channel_t *channel_a = switch_core_session_get_channel(vh->member_a->session);
	switch_channel_t *channel_b = switch_core_session_get_channel(vh->member_b->session);
	switch_status_t status;
	switch_frame_t *read_frame;
	
	/* Acquire locks for both sessions so the helper object and member structures don't get destroyed before we exit */
	if (switch_core_session_read_lock(vh->member_a->session) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	
	if (switch_core_session_read_lock(vh->member_b->session) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_rwunlock(vh->member_a->session);
		return NULL;
	}

	vh->up = 1;
	while (switch_test_flag(vh->member_a, MFLAG_RUNNING) && switch_test_flag(vh->member_b, MFLAG_RUNNING) &&
		   switch_channel_ready(channel_a) && switch_channel_ready(channel_b))  {
			status = switch_core_session_read_video_frame(vh->member_a->session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
			if (!SWITCH_READ_ACCEPTABLE(status)) {
				break;
			}

			if (!switch_test_flag(read_frame, SFF_CNG)) {
				if (switch_core_session_write_video_frame(vh->member_b->session, read_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
	}
	
	switch_core_session_rwunlock(vh->member_a->session);
	switch_core_session_rwunlock(vh->member_b->session);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s video thread ended.\n", switch_channel_get_name(channel_a));
	
	vh->up = 0;
	return NULL;
}


/* Main video monitor thread (1 per distinct conference room) */
static void *SWITCH_THREAD_FUNC conference_video_thread_run(switch_thread_t *thread, void *obj)
{
	conference_obj_t *conference = (conference_obj_t *) obj;
	conference_member_t *imember;
	switch_frame_t *vid_frame;
	switch_status_t status;
	int has_vid = 1;// req_iframe = 0;
	int yield = 0;
	uint32_t last_member = 0;
	switch_core_session_t *session;

	conference->video_running = 1;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Video thread started for conference %s\n", conference->name);

	while (has_vid && conference->video_running == 1 && globals.running && !switch_test_flag(conference, CFLAG_DESTRUCT)) {
		if (yield) {
			switch_yield(yield);
			yield = 0;
		}

		switch_mutex_lock(conference->member_mutex);

		if (!conference->floor_holder) {
			yield = 100000;
			goto do_continue;
		}

		if (!switch_channel_test_flag(switch_core_session_get_channel(conference->floor_holder->session), CF_VIDEO)) {
			yield = 100000;
			goto do_continue;
		}

		session = conference->floor_holder->session;
		switch_core_session_read_lock(session);
		switch_mutex_unlock(conference->member_mutex);
		status = switch_core_session_read_video_frame(session, &vid_frame, SWITCH_IO_FLAG_NONE, 0);
		switch_mutex_lock(conference->member_mutex);
		switch_core_session_rwunlock(session);

		if (!SWITCH_READ_ACCEPTABLE(status) || !conference->floor_holder || switch_test_flag(vid_frame, SFF_CNG)) {
			conference->floor_holder = NULL;
			//req_iframe = 0;
			goto do_continue;
		}

		if (conference->floor_holder->id != last_member) {
			int iframe = 0;
#if 0
			switch_core_session_message_t msg = { 0 };


			if (!req_iframe) {
				/* Tell the channel to request a fresh vid frame */
				msg.from = __FILE__;
				msg.message_id = SWITCH_MESSAGE_INDICATE_VIDEO_REFRESH_REQ;
				switch_core_session_receive_message(conference->floor_holder->session, &msg);
				req_iframe = 1;
			}
#endif

			if (vid_frame->codec->implementation->ianacode == 34) {	/* h.263 */
				//iframe = (*((int16_t *) vid_frame->data) >> 12 == 6);
				iframe = 1;
			} else if (vid_frame->codec->implementation->ianacode == 115) {	/* h.263-1998 */
				int y = *((int8_t *) vid_frame->data + 2) & 0xfe;
				iframe = (y == 0x80 || y == 0x82);
			} else if (vid_frame->codec->implementation->ianacode == 99) {	/* h.264 */
				uint8_t * hdr = vid_frame->data;
                uint8_t fragment_type = hdr[0] & 0x1f;
                uint8_t nal_type = hdr[1] & 0x1f;
                uint8_t start_bit = hdr[1] & 0x80;
                iframe = (((fragment_type == 28 || fragment_type == 29) && nal_type == 5 && start_bit == 128) || fragment_type == 5);
			} else {			/* we need more defs */
				iframe = 1;
			}

			if (!iframe) {
				goto do_continue;
			}

			//req_iframe = 0;
		}

		last_member = conference->floor_holder->id;

		switch_mutex_unlock(conference->member_mutex);
		switch_mutex_lock(conference->member_mutex);
		has_vid = 0;
		for (imember = conference->members; imember; imember = imember->next) {
			if (imember->session && switch_channel_test_flag(switch_core_session_get_channel(imember->session), CF_VIDEO)) {
				has_vid++;
				switch_core_session_write_video_frame(imember->session, vid_frame, SWITCH_IO_FLAG_NONE, 0);
			}
		}

	  do_continue:

		switch_mutex_unlock(conference->member_mutex);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Video thread ending for conference %s\n", conference->name);
	conference->video_running = 0;

	return NULL;
}

/* Main monitor thread (1 per distinct conference room) */
static void *SWITCH_THREAD_FUNC conference_thread_run(switch_thread_t *thread, void *obj)
{
	conference_obj_t *conference = (conference_obj_t *) obj;
	conference_member_t *imember, *omember;
	uint32_t samples = switch_samples_per_packet(conference->rate, conference->interval);
	uint32_t bytes = samples * 2;
	uint8_t ready = 0, total = 0;
	switch_timer_t timer = { 0 };
	switch_event_t *event;
	uint8_t *file_frame;
	uint8_t *async_file_frame;
	int16_t *bptr;
	uint32_t x = 0;
	int32_t z = 0;
	int member_score_sum = 0;
	int divisor = 0;
	
	if (!(divisor = conference->rate / 8000)) {
		divisor = 1;
	}

	file_frame = switch_core_alloc(conference->pool, SWITCH_RECOMMENDED_BUFFER_SIZE);
	async_file_frame = switch_core_alloc(conference->pool, SWITCH_RECOMMENDED_BUFFER_SIZE);

	if (switch_core_timer_init(&timer, conference->timer_name, conference->interval, samples, conference->pool) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Setup timer success interval: %u  samples: %u\n", conference->interval, samples);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Timer Setup Failed.  Conference Cannot Start\n");
		return NULL;
	}

	switch_mutex_lock(globals.hash_mutex);
	globals.threads++;
	switch_mutex_unlock(globals.hash_mutex);

	conference->is_recording = 0;
	conference->record_count = 0;

	while (globals.running && !switch_test_flag(conference, CFLAG_DESTRUCT)) {
		switch_size_t file_sample_len = samples;
		switch_size_t file_data_len = samples * 2;
		int has_file_data = 0, members_with_video = 0;
		uint32_t conf_energy = 0;
		int nomoh = 0;
		conference_member_t *video_bridge_members[2] = { 0 };

		/* Sync the conference to a single timing source */
		if (switch_core_timer_next(&timer) != SWITCH_STATUS_SUCCESS) {
			switch_set_flag(conference, CFLAG_DESTRUCT);
			break;
		}

		switch_mutex_lock(conference->mutex);
		has_file_data = ready = total = 0;


		/* Read one frame of audio from each member channel and save it for redistribution */
		for (imember = conference->members; imember; imember = imember->next) {
			uint32_t buf_read = 0;
			total++;
			imember->read = 0;

			if (imember->session) {
				if (switch_channel_test_flag(switch_core_session_get_channel(imember->session), CF_VIDEO)) {
					members_with_video++;
				}

				if (switch_test_flag(imember, MFLAG_NOMOH)) {
					nomoh++;
				}
				
				if (switch_test_flag(imember, MFLAG_VIDEO_BRIDGE)) {
					if (!video_bridge_members[0]) {
						video_bridge_members[0] = imember;
					} else {
						video_bridge_members[1] = imember;
					}
				}
			}

			switch_clear_flag_locked(imember, MFLAG_HAS_AUDIO);
			switch_mutex_lock(imember->audio_in_mutex);

			if (switch_buffer_inuse(imember->audio_buffer) >= bytes
				&& (buf_read = (uint32_t) switch_buffer_read(imember->audio_buffer, imember->frame, bytes))) {
				imember->read = buf_read;
				switch_set_flag_locked(imember, MFLAG_HAS_AUDIO);
				ready++;
			}
			switch_mutex_unlock(imember->audio_in_mutex);
		}

		if (conference->perpetual_sound && !conference->async_fnode) {
			conference_play_file(conference, conference->perpetual_sound, CONF_DEFAULT_LEADIN, NULL, 1);
		} else if (conference->moh_sound && ((nomoh == 0 && conference->count == 1) 
											 || switch_test_flag(conference, CFLAG_WAIT_MOD)) && !conference->async_fnode) {
			conference_play_file(conference, conference->moh_sound, CONF_DEFAULT_LEADIN, NULL, 1);
		}


		/* Find if no one talked for more than x number of second */
		if (conference->terminate_on_silence && conference->count > 1) {
			int is_talking = 0;

			for (imember = conference->members; imember; imember = imember->next) {
				if (switch_epoch_time_now(NULL) - imember->join_time <= conference->terminate_on_silence) {
					is_talking++;
				} else if (imember->last_talking != 0 && switch_epoch_time_now(NULL) - imember->last_talking <= conference->terminate_on_silence) {
					is_talking++;
				}
			}
			if (is_talking == 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Conference has been idle for over %d seconds, terminating\n", conference->terminate_on_silence);
				switch_set_flag(conference, CFLAG_DESTRUCT);
			}
		}

		/* Start recording if there's more than one participant. */
		if (conference->auto_record && !conference->is_recording && conference->count > 1) {
			conference->is_recording = 1;
			conference->record_count++;
			imember = conference->members;
			if (imember) {
				switch_channel_t *channel = switch_core_session_get_channel(imember->session);
				char *rfile = switch_channel_expand_variables(channel, conference->auto_record);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Auto recording file: %s\n", rfile);
				launch_conference_record_thread(conference, rfile);
				if (rfile != conference->auto_record) {
					switch_safe_free(rfile);
				}
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Auto Record Failed.  No members in conference.\n");
			}
		}


		if (members_with_video && conference->video_running != 1) {
			if (!switch_test_flag(conference, CFLAG_VIDEO_BRIDGE)) {
				launch_conference_video_thread(conference);	
			} else if (video_bridge_members[0] && video_bridge_members[1]){
				launch_conference_video_bridge_thread(video_bridge_members[0], video_bridge_members[1]);
			}
		}

		/* If a file or speech event is being played */
		if (conference->fnode) {
			/* Lead in time */
			if (conference->fnode->leadin) {
				conference->fnode->leadin--;
			} else if (!conference->fnode->done) {
				file_sample_len = samples;
				if (conference->fnode->type == NODE_TYPE_SPEECH) {
					switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_BLOCKING;

					if (switch_core_speech_read_tts(conference->fnode->sh, file_frame, &file_data_len, &flags) == SWITCH_STATUS_SUCCESS) {
						file_sample_len = file_data_len / 2;
					} else {
						file_sample_len = file_data_len = 0;
					}
				} else if (conference->fnode->type == NODE_TYPE_FILE) {
					switch_core_file_read(&conference->fnode->fh, file_frame, &file_sample_len);
				}

				if (file_sample_len <= 0) {
					if (test_eflag(conference, EFLAG_PLAY_FILE) &&
						switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
						conference_add_event_data(conference, event);
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "play-file-done");
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "File", conference->fnode->file);
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Async", "true");
						switch_event_fire(&event);
					}

					conference->fnode->done++;
				} else {
					has_file_data = 1;
				}
			}
		}

		if (conference->async_fnode) {
			/* Lead in time */
			if (conference->async_fnode->leadin) {
				conference->async_fnode->leadin--;
			} else if (!conference->async_fnode->done) {
				file_sample_len = samples;
				switch_core_file_read(&conference->async_fnode->fh, async_file_frame, &file_sample_len);

				if (file_sample_len <= 0) {
					if (test_eflag(conference, EFLAG_PLAY_FILE) &&
						switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
						conference_add_event_data(conference, event);
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "play-file-done");
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "File", conference->async_fnode->file);
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Async", "true");
						switch_event_fire(&event);
					}
					conference->async_fnode->done++;
				} else {
					if (has_file_data) {
						switch_size_t x;

						for (x = 0; x < file_sample_len; x++) {
							int32_t z;
							int16_t *muxed;

							muxed = (int16_t *) file_frame;
							bptr = (int16_t *) async_file_frame;
							z = muxed[x] + bptr[x];
							switch_normalize_to_16bit(z);
							muxed[x] = (int16_t) z;
						}
					} else {
						memcpy(file_frame, async_file_frame, file_sample_len * 2);
						has_file_data = 1;
					}
				}
			}
		}

		if (switch_test_flag(conference, CFLAG_WASTE_BANDWIDTH) && !has_file_data) {
			file_sample_len = bytes / 2;

			if (conference->comfort_noise_level) {
				switch_generate_sln_silence((int16_t *) file_frame, file_sample_len, conference->comfort_noise_level);
			} else {
				memset(file_frame, 255, bytes);
			}
			has_file_data = 1;
		}
		
		if (ready || has_file_data) {
			/* Use more bits in the main_frame to preserve the exact sum of the audio samples. */
			int main_frame[SWITCH_RECOMMENDED_BUFFER_SIZE / 2] = { 0 };
			int16_t write_frame[SWITCH_RECOMMENDED_BUFFER_SIZE / 2] = { 0 };


			/* Init the main frame with file data if there is any. */
			bptr = (int16_t *) file_frame;
			if (has_file_data && file_sample_len) {
				for (x = 0; x < bytes / 2; x++) {
					if (x <= file_sample_len) {
						main_frame[x] = (int32_t) bptr[x];
					} else {
						main_frame[x] = 255;
					}
				}
			}

			member_score_sum = 0;
			conference->mux_loop_count = 0;
			conference->member_loop_count = 0;


			/* Copy audio from every member known to be producing audio into the main frame. */
			for (omember = conference->members; omember; omember = omember->next) {
				conference->member_loop_count++;
				
				if (!(switch_test_flag(omember, MFLAG_RUNNING) && switch_test_flag(omember, MFLAG_HAS_AUDIO))) {
					continue;
				}

				if (conference->agc_level) {
					if (switch_test_flag(omember, MFLAG_TALKING) && switch_test_flag(omember, MFLAG_CAN_SPEAK)) {
						member_score_sum += omember->score;
						conference->mux_loop_count++;
					}
				}
				
				bptr = (int16_t *) omember->frame;
				for (x = 0; x < omember->read / 2; x++) {
					main_frame[x] += (int32_t) bptr[x];
				}
			}

			if (conference->agc_level && conference->member_loop_count) {
				conf_energy = 0;
			
				for (x = 0; x < bytes / 2; x++) {
					z = abs(main_frame[x]);
					switch_normalize_to_16bit(z);
					conf_energy += (int16_t) z;
				}
				
				conference->score = conf_energy / ((bytes / 2) / divisor) / conference->member_loop_count;

				conference->avg_tally += conference->score;
				conference->avg_score = conference->avg_tally / ++conference->avg_itt;
				if (!conference->avg_itt) conference->avg_tally = conference->score;
			}
			
			/* Create write frame once per member who is not deaf for each sample in the main frame
			   check if our audio is involved and if so, subtract it from the sample so we don't hear ourselves.
			   Since main frame was 32 bit int, we did not lose any detail, now that we have to convert to 16 bit we can
			   cut it off at the min and max range if need be and write the frame to the output buffer.
			 */
			for (omember = conference->members; omember; omember = omember->next) {
				switch_size_t ok = 1;

				if (!switch_test_flag(omember, MFLAG_RUNNING)) {
					continue;
				}

				if (!switch_test_flag(omember, MFLAG_CAN_HEAR) && !switch_test_flag(omember, MFLAG_WASTE_BANDWIDTH)
					&& !switch_test_flag(conference, CFLAG_WASTE_BANDWIDTH)) {
					continue;
				}

				bptr = (int16_t *) omember->frame;
				for (x = 0; x < bytes / 2; x++) {
					z = main_frame[x];
					/* bptr[x] represents my own contribution to this audio sample */
					if (switch_test_flag(omember, MFLAG_HAS_AUDIO) && x <= omember->read / 2) {
						z -= (int32_t) bptr[x];
					}

					/* when there are relationships, we have to do more work by scouring all the members to see if there are any 
					   reasons why we should not be hearing a paticular member, and if not, delete their samples as well.
					 */
					if (conference->relationship_total) {
						for (imember = conference->members; imember; imember = imember->next) {
							if (imember != omember && switch_test_flag(imember, MFLAG_HAS_AUDIO)) {
								conference_relationship_t *rel;
								switch_size_t found = 0;
								int16_t *rptr = (int16_t *) imember->frame;
								for (rel = imember->relationships; rel; rel = rel->next) {
									if ((rel->id == omember->id || rel->id == 0) && !switch_test_flag(rel, RFLAG_CAN_SPEAK)) {
										z -= (int32_t) rptr[x];
										found = 1;
										break;
									}
								}
								if (!found) {
									for (rel = omember->relationships; rel; rel = rel->next) {
										if ((rel->id == imember->id || rel->id == 0) && !switch_test_flag(rel, RFLAG_CAN_HEAR)) {
											z -= (int32_t) rptr[x];
											break;
										}
									}
								}

							}
						}
					}

					/* Now we can convert to 16 bit. */
					switch_normalize_to_16bit(z);
					write_frame[x] = (int16_t) z;
				}
				
				switch_mutex_lock(omember->audio_out_mutex);
				ok = switch_buffer_write(omember->mux_buffer, write_frame, bytes);
				switch_mutex_unlock(omember->audio_out_mutex);

				if (!ok) {
					switch_mutex_unlock(conference->mutex);
					goto end;
				}
			}
		}

		if (conference->async_fnode && conference->async_fnode->done) {
			switch_memory_pool_t *pool;
			switch_core_file_close(&conference->async_fnode->fh);
			pool = conference->async_fnode->pool;
			conference->async_fnode = NULL;
			switch_core_destroy_memory_pool(&pool);
		}

		if (conference->fnode && conference->fnode->done) {
			conference_file_node_t *fnode;
			switch_memory_pool_t *pool;

			if (conference->fnode->type != NODE_TYPE_SPEECH) {
				switch_core_file_close(&conference->fnode->fh);
			}

			fnode = conference->fnode;
			conference->fnode = conference->fnode->next;

			pool = fnode->pool;
			fnode = NULL;
			switch_core_destroy_memory_pool(&pool);
		}

		switch_mutex_unlock(conference->mutex);
	}
	/* Rinse ... Repeat */
  end:

	if (switch_test_flag(conference, CFLAG_OUTCALL)) {
		conference->cancel_cause = SWITCH_CAUSE_ORIGINATOR_CANCEL;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Ending pending outcall channels for Conference: '%s'\n", conference->name);
		while(conference->originating) {
			switch_yield(200000);
		}
	}

	if (switch_event_create(&event, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", CONF_CHAT_PROTO);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", conference->name);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "from", "%s@%s", conference->name, conference->domain);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "status", "Inactive");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "rpid", "idle");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "event_count", "%d", EC++);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique-id", conference->name);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "channel-state", "CS_HANGUP");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "answer-state", "terminated");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-direction", "inbound");
		switch_event_fire(&event);
	}

	switch_mutex_lock(conference->mutex);
	conference_stop_file(conference, FILE_STOP_ASYNC);
	conference_stop_file(conference, FILE_STOP_ALL);
	/* Close Unused Handles */
	if (conference->fnode) {
		conference_file_node_t *fnode, *cur;
		switch_memory_pool_t *pool;

		fnode = conference->fnode;
		while (fnode) {
			cur = fnode;
			fnode = fnode->next;

			if (cur->type != NODE_TYPE_SPEECH) {
				switch_core_file_close(&cur->fh);
			}

			pool = cur->pool;
			switch_core_destroy_memory_pool(&pool);
		}
		conference->fnode = NULL;
	}

	if (conference->async_fnode) {
		switch_memory_pool_t *pool;
		switch_core_file_close(&conference->async_fnode->fh);
		pool = conference->async_fnode->pool;
		conference->async_fnode = NULL;
		switch_core_destroy_memory_pool(&pool);
	}

	switch_mutex_lock(conference->member_mutex);
	for (imember = conference->members; imember; imember = imember->next) {
		switch_channel_t *channel;

		if (!switch_test_flag(imember, MFLAG_NOCHANNEL)) {
			channel = switch_core_session_get_channel(imember->session);

			if (!switch_false(switch_channel_get_variable(channel, "hangup_after_conference"))) {
				/* add this little bit to preserve the bridge cause code in case of an early media call that */
				/* never answers */
				if (switch_test_flag(conference, CFLAG_ANSWERED)) {
					switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
				} else {
					/* put actual cause code from outbound channel hangup here */
					switch_channel_hangup(channel, conference->bridge_hangup_cause);
				}
			}
		}

		switch_clear_flag_locked(imember, MFLAG_RUNNING);
	}
	switch_mutex_unlock(conference->member_mutex);
	switch_mutex_unlock(conference->mutex);

	if (conference->video_running == 1) {
		conference->video_running = -1;
		while (conference->video_running) {
			switch_cond_next();
		}
	}

	
	switch_core_timer_destroy(&timer);
	switch_mutex_lock(globals.hash_mutex);
	if (switch_test_flag(conference, CFLAG_INHASH)) {
		switch_core_hash_delete(globals.conference_hash, conference->name);
	}
	switch_mutex_unlock(globals.hash_mutex);

	/* Wait till everybody is out */
	switch_clear_flag_locked(conference, CFLAG_RUNNING);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Write Lock ON\n");
	switch_thread_rwlock_wrlock(conference->rwlock);
	switch_thread_rwlock_unlock(conference->rwlock);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Write Lock OFF\n");

	if (conference->sh) {
		switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_NONE;
		switch_core_speech_close(&conference->lsh, &flags);
		conference->sh = NULL;
	}

	if (conference->pool) {
		switch_memory_pool_t *pool = conference->pool;
		switch_core_destroy_memory_pool(&pool);
	}

	switch_mutex_lock(globals.hash_mutex);
	globals.threads--;
	switch_mutex_unlock(globals.hash_mutex);

	return NULL;
}

static void conference_loop_fn_mute_toggle(conference_member_t *member, caller_control_action_t *action)
{
	if (member == NULL)
		return;

	if (switch_test_flag(member, MFLAG_CAN_SPEAK)) {
		conf_api_sub_mute(member, NULL, NULL);
	} else {
		conf_api_sub_unmute(member, NULL, NULL);
		if (!switch_test_flag(member, MFLAG_CAN_HEAR)) {
			conf_api_sub_undeaf(member, NULL, NULL);
		}
	}
}

static void conference_loop_fn_mute_on(conference_member_t *member, caller_control_action_t *action)
{
	if (switch_test_flag(member, MFLAG_CAN_SPEAK)) {
		conf_api_sub_mute(member, NULL, NULL);
	}
}

static void conference_loop_fn_mute_off(conference_member_t *member, caller_control_action_t *action)
{
	if (!switch_test_flag(member, MFLAG_CAN_SPEAK)) {
		conf_api_sub_unmute(member, NULL, NULL);
		if (!switch_test_flag(member, MFLAG_CAN_HEAR)) {
			conf_api_sub_undeaf(member, NULL, NULL);
		}
	}
}

static void conference_loop_fn_lock_toggle(conference_member_t *member, caller_control_action_t *action)
{
	switch_event_t *event;

	if (member == NULL)
		return;

	if (!switch_test_flag(member->conference, CFLAG_LOCKED)) {
		if (member->conference->is_locked_sound) {
			conference_play_file(member->conference, member->conference->is_locked_sound, CONF_DEFAULT_LEADIN, NULL, 0);
		}

		switch_set_flag_locked(member->conference, CFLAG_LOCKED);
		if (test_eflag(member->conference, EFLAG_LOCK) &&
			switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_data(member->conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "lock");
			switch_event_fire(&event);
		}
	} else {
		if (member->conference->is_unlocked_sound) {
			conference_play_file(member->conference, member->conference->is_unlocked_sound, CONF_DEFAULT_LEADIN, NULL, 0);
		}

		switch_clear_flag_locked(member->conference, CFLAG_LOCKED);
		if (test_eflag(member->conference, EFLAG_UNLOCK) &&
			switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_data(member->conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "unlock");
			switch_event_fire(&event);
		}
	}

}

static void conference_loop_fn_deafmute_toggle(conference_member_t *member, caller_control_action_t *action)
{
	if (member == NULL)
		return;

	if (switch_test_flag(member, MFLAG_CAN_SPEAK)) {
		conf_api_sub_mute(member, NULL, NULL);
		if (switch_test_flag(member, MFLAG_CAN_HEAR)) {
			conf_api_sub_deaf(member, NULL, NULL);
		}
	} else {
		conf_api_sub_unmute(member, NULL, NULL);
		if (!switch_test_flag(member, MFLAG_CAN_HEAR)) {
			conf_api_sub_undeaf(member, NULL, NULL);
		}
	}
}

static void conference_loop_fn_energy_up(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512], str[30] = "";
	switch_event_t *event;
	char *p;

	if (member == NULL)
		return;


	member->energy_level += 200;
	if (member->energy_level > 1800) {
		member->energy_level = 1800;
	}

	if (test_eflag(member->conference, EFLAG_ENERGY_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "energy-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->energy_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Energy level %d", member->energy_level);
	//conference_member_say(member, msg, 0);

	switch_snprintf(str, sizeof(str), "%d", abs(member->energy_level) / 200);
	for (p = str; p && *p; p++) {
		switch_snprintf(msg, sizeof(msg), "digits/%c.wav", *p);
		conference_member_play_file(member, msg, 0);
	}


	

}

static void conference_loop_fn_energy_equ_conf(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512], str[30] = "", *p;
	switch_event_t *event;

	if (member == NULL)
		return;

	member->energy_level = member->conference->energy_level;

	if (test_eflag(member->conference, EFLAG_ENERGY_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "energy-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->energy_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Energy level %d", member->energy_level);
	//conference_member_say(member, msg, 0);

	switch_snprintf(str, sizeof(str), "%d", abs(member->energy_level) / 200);
	for (p = str; p && *p; p++) {
		switch_snprintf(msg, sizeof(msg), "digits/%c.wav", *p);
		conference_member_play_file(member, msg, 0);
	}
	
}

static void conference_loop_fn_energy_dn(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512], str[30] = "", *p;
	switch_event_t *event;

	if (member == NULL)
		return;

	member->energy_level -= 200;
	if (member->energy_level < 0) {
		member->energy_level = 0;
	}

	if (test_eflag(member->conference, EFLAG_ENERGY_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "energy-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->energy_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Energy level %d", member->energy_level);
	//conference_member_say(member, msg, 0);

	switch_snprintf(str, sizeof(str), "%d", abs(member->energy_level) / 200);
	for (p = str; p && *p; p++) {
		switch_snprintf(msg, sizeof(msg), "digits/%c.wav", *p);
		conference_member_play_file(member, msg, 0);
	}
	
}

static void conference_loop_fn_volume_talk_up(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512];
	switch_event_t *event;

	if (member == NULL)
		return;

	member->volume_out_level++;
	switch_normalize_volume(member->volume_out_level);

	if (test_eflag(member->conference, EFLAG_VOLUME_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "volume-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->volume_out_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Volume level %d", member->volume_out_level);
	//conference_member_say(member, msg, 0);

	if (member->volume_out_level < 0) {
		switch_snprintf(msg, sizeof(msg), "currency/negative.wav", member->volume_out_level);
		conference_member_play_file(member, msg, 0);
	}

	switch_snprintf(msg, sizeof(msg), "digits/%d.wav", abs(member->volume_out_level));
	conference_member_play_file(member, msg, 0);

}

static void conference_loop_fn_volume_talk_zero(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512];
	switch_event_t *event;

	if (member == NULL)
		return;

	member->volume_out_level = 0;

	if (test_eflag(member->conference, EFLAG_VOLUME_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "volume-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->volume_out_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Volume level %d", member->volume_out_level);
	//conference_member_say(member, msg, 0);


	if (member->volume_out_level < 0) {
		switch_snprintf(msg, sizeof(msg), "currency/negative.wav", member->volume_out_level);
		conference_member_play_file(member, msg, 0);
	}

	switch_snprintf(msg, sizeof(msg), "digits/%d.wav", abs(member->volume_out_level));
	conference_member_play_file(member, msg, 0);
}

static void conference_loop_fn_volume_talk_dn(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512];
	switch_event_t *event;

	if (member == NULL)
		return;

	member->volume_out_level--;
	switch_normalize_volume(member->volume_out_level);

	if (test_eflag(member->conference, EFLAG_VOLUME_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "volume-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->volume_out_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Volume level %d", member->volume_out_level);
	//conference_member_say(member, msg, 0);

	if (member->volume_out_level < 0) {
		switch_snprintf(msg, sizeof(msg), "currency/negative.wav", member->volume_out_level);
		conference_member_play_file(member, msg, 0);
	}

	switch_snprintf(msg, sizeof(msg), "digits/%d.wav", abs(member->volume_out_level));
	conference_member_play_file(member, msg, 0);
}

static void conference_loop_fn_volume_listen_up(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512];
	switch_event_t *event;

	if (member == NULL)
		return;

	member->volume_in_level++;
	switch_normalize_volume(member->volume_in_level);

	if (test_eflag(member->conference, EFLAG_GAIN_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "gain-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->volume_in_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Gain level %d", member->volume_in_level);
	//conference_member_say(member, msg, 0);

	if (member->volume_in_level < 0) {
		switch_snprintf(msg, sizeof(msg), "currency/negative.wav", member->volume_in_level);
		conference_member_play_file(member, msg, 0);
	}

	switch_snprintf(msg, sizeof(msg), "digits/%d.wav", abs(member->volume_in_level));
	conference_member_play_file(member, msg, 0);

}

static void conference_loop_fn_volume_listen_zero(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512];
	switch_event_t *event;

	if (member == NULL)
		return;

	member->volume_in_level = 0;

	if (test_eflag(member->conference, EFLAG_GAIN_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "gain-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->volume_in_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Gain level %d", member->volume_in_level);
	//conference_member_say(member, msg, 0);

	if (member->volume_in_level < 0) {
		switch_snprintf(msg, sizeof(msg), "currency/negative.wav", member->volume_in_level);
		conference_member_play_file(member, msg, 0);
	}

	switch_snprintf(msg, sizeof(msg), "digits/%d.wav", abs(member->volume_in_level));
	conference_member_play_file(member, msg, 0);
	
}

static void conference_loop_fn_volume_listen_dn(conference_member_t *member, caller_control_action_t *action)
{
	char msg[512];
	switch_event_t *event;

	if (member == NULL)
		return;

	member->volume_in_level--;
	switch_normalize_volume(member->volume_in_level);

	if (test_eflag(member->conference, EFLAG_GAIN_LEVEL) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "gain-level");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-Level", "%d", member->volume_in_level);
		switch_event_fire(&event);
	}

	//switch_snprintf(msg, sizeof(msg), "Gain level %d", member->volume_in_level);
	//conference_member_say(member, msg, 0);

	if (member->volume_in_level < 0) {
		switch_snprintf(msg, sizeof(msg), "currency/negative.wav", member->volume_in_level);
		conference_member_play_file(member, msg, 0);
	}

    switch_snprintf(msg, sizeof(msg), "digits/%d.wav", abs(member->volume_in_level));
    conference_member_play_file(member, msg, 0);
}

static void conference_loop_fn_event(conference_member_t *member, caller_control_action_t *action)
{
	switch_event_t *event;
	if (test_eflag(member->conference, EFLAG_DTMF) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "dtmf");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "DTMF-Key", action->binded_dtmf);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Data", action->expanded_data);
		switch_event_fire(&event);
	}
}

static void conference_loop_fn_transfer(conference_member_t *member, caller_control_action_t *action)
{
	char *exten = NULL;
	char *dialplan = "XML";
	char *context = "default";

	char *argv[3] = { 0 };
	int argc;
	char *mydata = NULL;
	switch_event_t *event;

	if (test_eflag(member->conference, EFLAG_DTMF) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "transfer");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Dialplan", action->expanded_data);
		switch_event_fire(&event);
	}
	switch_clear_flag_locked(member, MFLAG_RUNNING);

	if ((mydata = switch_core_session_strdup(member->session, action->expanded_data))) {
		if ((argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
			if (argc > 0) {
				exten = argv[0];
			}
			if (argc > 1) {
				dialplan = argv[1];
			}
			if (argc > 2) {
				context = argv[2];
			}

		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_ERROR, "Empty transfer string [%s]\n", (char *) action->expanded_data);
			goto done;
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_ERROR, "Unable to allocate memory to duplicate transfer data.\n");
		goto done;
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG, "Transfering to: %s, %s, %s\n", exten, dialplan, context);

	switch_ivr_session_transfer(member->session, exten, dialplan, context);

  done:
	return;
}

static void conference_loop_fn_exec_app(conference_member_t *member, caller_control_action_t *action)
{
	char *app = NULL;
	char *arg = "";

	char *argv[2] = { 0 };
	int argc;
	char *mydata = NULL;
	switch_event_t *event = NULL;
	switch_channel_t *channel = NULL;

	if (!action->expanded_data) return;

	if (test_eflag(member->conference, EFLAG_DTMF) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "execute_app");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Application", action->expanded_data);
		switch_event_fire(&event);
	}

	mydata = strdup(action->expanded_data);
	switch_assert(mydata);

	if ((argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
		if (argc > 0) {
			app = argv[0];
		}
		if (argc > 1) {
			arg = argv[1];
		}

	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_ERROR, "Empty execute app string [%s]\n", 
						  (char *) action->expanded_data);
		goto done;
	}

	if (!app) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_ERROR, "Unable to find application.\n");
		goto done;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG, "Execute app: %s, %s\n", app, arg);

	channel = switch_core_session_get_channel(member->session);

	switch_channel_set_app_flag(channel, CF_APP_TAGGED);
	switch_core_session_set_read_codec(member->session, NULL);
	switch_core_session_execute_application(member->session, app, arg);
	switch_core_session_set_read_codec(member->session, &member->read_codec);
	switch_channel_clear_app_flag(channel, CF_APP_TAGGED);

  done:

	switch_safe_free(mydata);

	return;
}

static void conference_loop_fn_hangup(conference_member_t *member, caller_control_action_t *action)
{
	switch_clear_flag_locked(member, MFLAG_RUNNING);
}


static int noise_gate_check(conference_member_t *member)
{
	int r = 0;


	if (member->conference->agc_level && member->agc_volume_in_level != 0) {
		int target_score = 0;

		target_score = (member->energy_level + (25 * member->agc_volume_in_level));

		if (target_score < 0) target_score = 0;

		r = member->score > target_score;
		
	} else {
		r = member->score > member->energy_level;
	}

	return r;
}

static void clear_avg(conference_member_t *member)
{

	member->avg_score = 0;
	member->avg_itt = 0;
	member->avg_tally = 0;
	member->agc_concur = 0;
}

static void check_agc_levels(conference_member_t *member)
{
	int x = 0;

	if (!member->avg_score) return;
	
	if (member->avg_score < member->conference->agc_level - 100) {
		member->agc_volume_in_level++;
		switch_normalize_volume_granular(member->agc_volume_in_level);
		x = 1;
	} else if (member->avg_score > member->conference->agc_level + 100) {
		member->agc_volume_in_level--;
		switch_normalize_volume_granular(member->agc_volume_in_level);
		x = -1;
	}

	if (x) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG7,
						  "AGC %s:%d diff:%d level:%d cur:%d avg:%d vol:%d %s\n", 
						  member->conference->name,
						  member->id, member->conference->agc_level - member->avg_score, member->conference->agc_level, 
						  member->score, member->avg_score, member->agc_volume_in_level, x > 0 ? "+++" : "---");
		
		clear_avg(member);
	}
}




/* marshall frames from the call leg to the conference thread for muxing to other call legs */
static void *SWITCH_THREAD_FUNC conference_loop_input(switch_thread_t *thread, void *obj)
{
    switch_event_t *event;
	conference_member_t *member = obj;
	switch_channel_t *channel;
	switch_status_t status;
	switch_frame_t *read_frame = NULL;
	uint32_t hangover = 40, hangunder = 5, hangover_hits = 0, hangunder_hits = 0, diff_level = 400;
	switch_core_session_t *session = member->session;
	int check_floor_change;

	switch_assert(member != NULL);

	switch_clear_flag_locked(member, MFLAG_TALKING);

	channel = switch_core_session_get_channel(session);

	switch_core_session_get_read_impl(session, &member->read_impl);

	/* As long as we have a valid read, feed that data into an input buffer where the conference thread will take it 
	   and mux it with any audio from other channels. */

	while (switch_test_flag(member, MFLAG_RUNNING) && switch_channel_ready(channel)) {
		check_floor_change = 0;

		if (switch_channel_ready(channel) && switch_channel_test_app_flag(channel, CF_APP_TAGGED)) {
			switch_yield(100000);
			continue;
		}

		/* Read a frame. */
		status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

		switch_mutex_lock(member->read_mutex);

		/* end the loop, if appropriate */
		if (!SWITCH_READ_ACCEPTABLE(status) || !switch_test_flag(member, MFLAG_RUNNING)) {
			switch_mutex_unlock(member->read_mutex);
			break;
		}

		if (switch_test_flag(read_frame, SFF_CNG)) {
			if (member->conference->agc_level) {
				member->nt_tally++;
			}

			if (hangunder_hits) {
				hangunder_hits--;
			}
			if (switch_test_flag(member, MFLAG_TALKING)) {
				if (++hangover_hits >= hangover) {
					hangover_hits = hangunder_hits = 0;
					switch_clear_flag_locked(member, MFLAG_TALKING);
					check_agc_levels(member);
					clear_avg(member);

					if (test_eflag(member->conference, EFLAG_STOP_TALKING) &&
						switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
						conference_add_event_member_data(member, event);
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "stop-talking");
						switch_event_fire(&event);
					}
				}
			}

			goto do_continue;
		}

		if (member->nt_tally > (member->read_impl.actual_samples_per_second / member->read_impl.samples_per_packet) * 3) {
			member->agc_volume_in_level = 0;
			clear_avg(member);
		}

		/* Check for input volume adjustments */
		if (!member->conference->agc_level) {
			member->conference->agc_level = 0;
			clear_avg(member);
		}
		

		/* if the member can speak, compute the audio energy level and */
		/* generate events when the level crosses the threshold        */
		if ((switch_test_flag(member, MFLAG_CAN_SPEAK) || switch_test_flag(member, MFLAG_MUTE_DETECT))) {
			uint32_t energy = 0, i = 0, samples = 0, j = 0;
			int16_t *data;
			int agc_period = (member->read_impl.actual_samples_per_second / member->read_impl.samples_per_packet) / 4;
			

			data = read_frame->data;
			member->score = 0;

			if (member->volume_in_level) {
				switch_change_sln_volume(read_frame->data, read_frame->datalen / 2, member->volume_in_level);
			}

			if (member->agc_volume_in_level) {
				switch_change_sln_volume_granular(read_frame->data, read_frame->datalen / 2, member->agc_volume_in_level);
			}
			
			if ((samples = read_frame->datalen / sizeof(*data))) {
				for (i = 0; i < samples; i++) {
					energy += abs(data[j]);
					j += member->read_impl.number_of_channels;
				}
				
				member->score = energy / samples;
			}

			if (member->vol_period) {
				member->vol_period--;
			}
			
			if (member->conference->agc_level && member->score && 
				switch_test_flag(member, MFLAG_CAN_SPEAK) &&
				noise_gate_check(member)
				) {
				int last_shift = abs(member->last_score - member->score);
				
				if (member->score && member->last_score && last_shift > 900) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG7,
									  "AGC %s:%d drop anomalous shift of %d\n", 
									  member->conference->name,
									  member->id, last_shift);

				} else {
					member->avg_tally += member->score;
					member->avg_itt++;
					if (!member->avg_itt) member->avg_itt++;
					member->avg_score = member->avg_tally / member->avg_itt;
				}

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG7,
								  "AGC %s:%d diff:%d level:%d cur:%d avg:%d vol:%d\n", 
								  member->conference->name,
								  member->id, member->conference->agc_level - member->avg_score, member->conference->agc_level, 
								  member->score, member->avg_score, member->agc_volume_in_level);
				
				if (++member->agc_concur >= agc_period) {
					if (!member->vol_period) {
						check_agc_levels(member);
					}
					member->agc_concur = 0;
				}
			} else {
				member->nt_tally++;
			}

			member->score_iir = (int) (((1.0 - SCORE_DECAY) * (float) member->score) + (SCORE_DECAY * (float) member->score_iir));

			if (member->score_iir > SCORE_MAX_IIR) {
				member->score_iir = SCORE_MAX_IIR;
			}

			if (noise_gate_check(member)) {
				uint32_t diff = member->score - member->energy_level;
				if (hangover_hits) {
					hangover_hits--;
				}

				if (member->conference->agc_level) {
					member->nt_tally = 0;
				}

				if (diff >= diff_level || ++hangunder_hits >= hangunder) { 
					check_floor_change = 1;

					hangover_hits = hangunder_hits = 0;
					member->last_talking = switch_epoch_time_now(NULL);

					if (!switch_test_flag(member, MFLAG_TALKING)) {
						switch_set_flag_locked(member, MFLAG_TALKING);

						if (test_eflag(member->conference, EFLAG_START_TALKING) && switch_test_flag(member, MFLAG_CAN_SPEAK) &&
							switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
							conference_add_event_member_data(member, event);
							switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "start-talking");
							switch_event_fire(&event);
						}

						if (switch_test_flag(member, MFLAG_MUTE_DETECT) && !switch_test_flag(member, MFLAG_CAN_SPEAK)) {

							if (!zstr(member->conference->mute_detect_sound)) {
								switch_set_flag(member, MFLAG_INDICATE_MUTE_DETECT);
							}

							if (test_eflag(member->conference, EFLAG_MUTE_DETECT) &&
								switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
								conference_add_event_member_data(member, event);
								switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "mute-detect");
								switch_event_fire(&event);
							}
						}
					}
				}
			} else {
				if (hangunder_hits) {
					hangunder_hits--;
				}

				if (member->conference->agc_level) {
					member->nt_tally++;
				}

				if (switch_test_flag(member, MFLAG_TALKING) && switch_test_flag(member, MFLAG_CAN_SPEAK)) {
					switch_event_t *event;
					if (++hangover_hits >= hangover) {
						hangover_hits = hangunder_hits = 0;
						switch_clear_flag_locked(member, MFLAG_TALKING);
						check_agc_levels(member);
						clear_avg(member);
						
						if (test_eflag(member->conference, EFLAG_STOP_TALKING) &&
							switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
							conference_add_event_member_data(member, event);
							switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "stop-talking");
							switch_event_fire(&event);
						}
					}
				}
			}


			member->last_score = member->score;
		}

		/* skip frames that are not actual media or when we are muted or silent */
		if ((switch_test_flag(member, MFLAG_TALKING) || member->energy_level == 0) && switch_test_flag(member, MFLAG_CAN_SPEAK) &&
			!switch_test_flag(member->conference, CFLAG_WAIT_MOD)) {
			switch_audio_resampler_t *read_resampler = member->read_resampler;
			void *data;
			uint32_t datalen;

			if (read_resampler) {
				int16_t *bptr = (int16_t *) read_frame->data;
				int len = (int) read_frame->datalen;

				switch_resample_process(read_resampler, bptr, len / 2);
				memcpy(member->resample_out, read_resampler->to, read_resampler->to_len * 2);
				len = read_resampler->to_len * 2;
				datalen = len;
				data = member->resample_out;
			} else {
				data = read_frame->data;
				datalen = read_frame->datalen;
			}


			if (datalen) {
				switch_size_t ok = 1;

				/* Write the audio into the input buffer */
				switch_mutex_lock(member->audio_in_mutex);
				ok = switch_buffer_write(member->audio_buffer, data, datalen);
				switch_mutex_unlock(member->audio_in_mutex);
				if (!ok) {
					switch_mutex_unlock(member->read_mutex);
					break;
				}
			}
		}

	  do_continue:

		switch_mutex_unlock(member->read_mutex);

		if (check_floor_change) {
			switch_mutex_lock(member->conference->member_mutex);
			if ((!member->conference->floor_holder ||
				 !switch_test_flag(member->conference->floor_holder, MFLAG_TALKING) ||
				 ((member->score_iir > SCORE_IIR_SPEAKING_MAX) && (member->conference->floor_holder->score_iir < SCORE_IIR_SPEAKING_MIN))) &&
				(!switch_test_flag(member->conference, CFLAG_VID_FLOOR) || switch_channel_test_flag(channel, CF_VIDEO))) {

				if (test_eflag(member->conference, EFLAG_FLOOR_CHANGE) &&
					switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
					conference_add_event_member_data(member, event);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "floor-change");
					switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Old-ID", "%d",
											member->conference->floor_holder ? member->conference->floor_holder->id : 0);
					switch_event_add_header(event, SWITCH_STACK_BOTTOM, "New-ID", "%d", member->conference->floor_holder ? member->id : 0);
					switch_event_fire(&event);
				}
				member->conference->floor_holder = member;
			}
			switch_mutex_unlock(member->conference->member_mutex);
		}

	}


	switch_resample_destroy(&member->read_resampler);
	switch_clear_flag_locked(member, MFLAG_ITHREAD);

	return NULL;
}


static void member_add_file_data(conference_member_t *member, int16_t *data, switch_size_t file_data_len)
{
	switch_size_t file_sample_len = file_data_len / 2;
	int16_t file_frame[SWITCH_RECOMMENDED_BUFFER_SIZE / 2] = { 0 };

	if (!member->fnode) {
		return;
	}

	/* if we are done, clean it up */
	if (member->fnode->done) {
		conference_file_node_t *fnode;
		switch_memory_pool_t *pool;

		if (member->fnode->type != NODE_TYPE_SPEECH) {
			switch_core_file_close(&member->fnode->fh);
		}

		fnode = member->fnode;
		member->fnode = member->fnode->next;

		pool = fnode->pool;
		fnode = NULL;
		switch_core_destroy_memory_pool(&pool);
	} else {
		/* skip this frame until leadin time has expired */
		if (member->fnode->leadin) {
			member->fnode->leadin--;
		} else {
			if (member->fnode->type == NODE_TYPE_SPEECH) {
				switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_BLOCKING;

				if (switch_core_speech_read_tts(member->fnode->sh, file_frame, &file_data_len, &flags) == SWITCH_STATUS_SUCCESS) {
					file_sample_len = file_data_len / 2;
				} else {
					file_sample_len = file_data_len = 0;
				}
			} else if (member->fnode->type == NODE_TYPE_FILE) {
				switch_core_file_read(&member->fnode->fh, file_frame, &file_sample_len);
				file_data_len = file_sample_len * 2;
			}

			if (file_sample_len <= 0) {
				switch_event_t *event;
				member->fnode->done++;

				if (test_eflag(member->conference, EFLAG_PLAY_FILE) &&
					switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
					conference_add_event_data(member->conference, event);
					conference_add_event_member_data(member, event);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "play-file-member-done");
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "File", member->fnode->file);
					switch_event_fire(&event);
				}
			} else {			/* there is file node data to mix into the frame */
				int32_t i, sample;

				/* Check for output volume adjustments */
				if (member->volume_out_level) {
					switch_change_sln_volume(file_frame, file_sample_len, member->volume_out_level);
				}

				for (i = 0; i < file_sample_len; i++) {
					sample = data[i] + file_frame[i];
					switch_normalize_to_16bit(sample);
					data[i] = sample;
				}

			}
		}
	}
}



/* launch an input thread for the call leg */
static void launch_conference_loop_input(conference_member_t *member, switch_memory_pool_t *pool)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	if (member == NULL)
		return;

	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_set_flag_locked(member, MFLAG_ITHREAD);
	switch_thread_create(&thread, thd_attr, conference_loop_input, member, pool);
}

/* marshall frames from the conference (or file or tts output) to the call leg */
/* NB. this starts the input thread after some initial setup for the call leg */
static void conference_loop_output(conference_member_t *member)
{
	switch_channel_t *channel;
	switch_frame_t write_frame = { 0 };
	uint8_t *data = NULL;
	switch_timer_t timer = { 0 };
	uint32_t interval;
	uint32_t samples;
	//uint32_t csamples;
	uint32_t tsamples;
	uint32_t flush_len;
	uint32_t low_count, bytes;
	call_list_t *call_list, *cp;
	switch_codec_implementation_t read_impl = { 0 };
	int sanity;

	switch_core_session_get_read_impl(member->session, &read_impl);


	channel = switch_core_session_get_channel(member->session);
	interval = read_impl.microseconds_per_packet / 1000;
	samples = switch_samples_per_packet(member->conference->rate, interval);
	//csamples = samples;
	tsamples = member->orig_read_impl.samples_per_packet;
	flush_len = 0;
	low_count = 0;
	bytes = samples * 2;
	call_list = NULL;
	cp = NULL;



	switch_assert(member->conference != NULL);

	flush_len = switch_samples_per_packet(member->conference->rate, member->conference->interval) * 10;

	if (switch_core_timer_init(&timer, member->conference->timer_name, interval, tsamples, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_ERROR, "Timer Setup Failed.  Conference Cannot Start\n");
		return;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG, "Setup timer %s success interval: %u  samples: %u\n",
					  member->conference->timer_name, interval, tsamples);

	
	write_frame.data = data = switch_core_session_alloc(member->session, SWITCH_RECOMMENDED_BUFFER_SIZE);
	write_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;


	write_frame.codec = &member->write_codec;

	/* Start the input thread */
	launch_conference_loop_input(member, switch_core_session_get_pool(member->session));

	if ((call_list = switch_channel_get_private(channel, "_conference_autocall_list_"))) {
		const char *cid_name = switch_channel_get_variable(channel, "conference_auto_outcall_caller_id_name");
		const char *cid_num = switch_channel_get_variable(channel, "conference_auto_outcall_caller_id_number");
		const char *toval = switch_channel_get_variable(channel, "conference_auto_outcall_timeout");
		const char *flags = switch_channel_get_variable(channel, "conference_auto_outcall_flags");
		const char *profile = switch_channel_get_variable(channel, "conference_auto_outcall_profile");
		const char *ann = switch_channel_get_variable(channel, "conference_auto_outcall_announce");
		const char *prefix = switch_channel_get_variable(channel, "conference_auto_outcall_prefix");
		int to = 60;

		if (ann && !switch_channel_test_app_flag_key("conf_silent", channel, CONF_SILENT_REQ)) {
			member->conference->special_announce = switch_core_strdup(member->conference->pool, ann);
		}

		switch_channel_set_private(channel, "_conference_autocall_list_", NULL);

		switch_set_flag(member->conference, CFLAG_OUTCALL);

		if (toval) {
			to = atoi(toval);
			if (to < 10 || to > 500) {
				to = 60;
			}
		}

		for (cp = call_list; cp; cp = cp->next) {
			int argc;
			char *argv[512] = { 0 };
			char *cpstr = strdup(cp->string);
			int x = 0;

			switch_assert(cpstr);
			argc = switch_separate_string(cpstr, ',', argv, (sizeof(argv) / sizeof(argv[0])));
			for (x = 0; x < argc; x++) {
				char *dial_str = switch_mprintf("%s%s", switch_str_nil(prefix), argv[x]);
				switch_assert(dial_str);
				conference_outcall_bg(member->conference, NULL, NULL, dial_str, to, switch_str_nil(flags), cid_name, cid_num, NULL, 
									  profile, &member->conference->cancel_cause);
				switch_safe_free(dial_str);
			}
			switch_safe_free(cpstr);
		}

		switch_channel_set_app_flag(channel, CF_APP_TAGGED);
		do {
			switch_ivr_sleep(member->session, 500, SWITCH_TRUE, NULL);
		} while(switch_channel_up(channel) && member->conference->originating);
		switch_channel_clear_app_flag(channel, CF_APP_TAGGED);

		if (!switch_channel_ready(channel)) {
			member->conference->cancel_cause = SWITCH_CAUSE_ORIGINATOR_CANCEL;
			goto end;
		}
			
		conference_member_play_file(member, "tone_stream://%(500,0,640)", 0);
	}
	
	if (!switch_test_flag(member->conference, CFLAG_ANSWERED)) {
		switch_channel_answer(channel);
	}


	sanity = 2000;
	while(!switch_test_flag(member, MFLAG_ITHREAD) && sanity > 0) {
		switch_cond_next();
		sanity--;
	}

	/* Fair WARNING, If you expect the caller to hear anything or for digit handling to be processed,      */
	/* you better not block this thread loop for more than the duration of member->conference->timer_name!  */
	while (switch_test_flag(member, MFLAG_RUNNING) && switch_test_flag(member, MFLAG_ITHREAD)
		   && switch_channel_ready(channel)) {
		char dtmf[128] = "";
		switch_event_t *event;
		int use_timer = 0;
		switch_buffer_t *use_buffer = NULL;
		uint32_t mux_used = 0;

		switch_mutex_lock(member->write_mutex);

		if (switch_core_session_dequeue_event(member->session, &event, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
			if (event->event_id == SWITCH_EVENT_MESSAGE) {
				char *from = switch_event_get_header(event, "from");
				char *to = switch_event_get_header(event, "to");
				char *body = switch_event_get_body(event);
				char *p;

				if (to && from && body) {
					if ((p = strchr(to, '+')) && strncmp(to, CONF_CHAT_PROTO, strlen(CONF_CHAT_PROTO))) {
						switch_event_del_header(event, "to");
						switch_event_add_header(event, SWITCH_STACK_BOTTOM,
												"to", "%s+%s@%s", CONF_CHAT_PROTO, member->conference->name, member->conference->domain);
					}
					chat_send(event);
				}
			}
			switch_event_destroy(&event);
		}

		if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
			/* test to see if outbound channel has answered */
			if (switch_channel_test_flag(channel, CF_ANSWERED) && !switch_test_flag(member->conference, CFLAG_ANSWERED)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG,
								  "Outbound conference channel answered, setting CFLAG_ANSWERED\n");
				switch_set_flag(member->conference, CFLAG_ANSWERED);
			}
		} else {
			if (switch_test_flag(member->conference, CFLAG_ANSWERED) && !switch_channel_test_flag(channel, CF_ANSWERED)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG, "CLFAG_ANSWERED set, answering inbound channel\n");
				switch_channel_answer(channel);
			}
		}

		/* if we have caller digits, feed them to the parser to find an action */
		if (switch_channel_has_dtmf(channel)) {
			switch_channel_dequeue_dtmf_string(channel, dtmf, sizeof(dtmf));

			if (switch_test_flag(member, MFLAG_DIST_DTMF)) {
				conference_send_all_dtmf(member, member->conference, dtmf);
			} else if (member->dmachine) {
				switch_ivr_dmachine_feed(member->dmachine, dtmf, NULL);
			}
		} else if (member->dmachine) {
			switch_ivr_dmachine_ping(member->dmachine, NULL);
		}

		use_buffer = NULL;
		mux_used = (uint32_t) switch_buffer_inuse(member->mux_buffer);
		
		use_timer = 1;
		
		if (mux_used) {
			if (mux_used < bytes) {
				if (++low_count >= 5) {
					/* partial frame sitting around this long is useless and builds delay */
					switch_set_flag_locked(member, MFLAG_FLUSH_BUFFER);
				}
			} else if (mux_used > flush_len) {
				/* getting behind, clear the buffer */
				switch_set_flag_locked(member, MFLAG_FLUSH_BUFFER);
			}
		}

		if (mux_used >= bytes) {
			/* Flush the output buffer and write all the data (presumably muxed) back to the channel */
			switch_mutex_lock(member->audio_out_mutex);
			write_frame.data = data;
			use_buffer = member->mux_buffer;
			low_count = 0;
			if ((write_frame.datalen = (uint32_t) switch_buffer_read(use_buffer, write_frame.data, bytes))) {
				if (write_frame.datalen) {
					write_frame.samples = write_frame.datalen / 2;
				   
				   if( !switch_test_flag(member, MFLAG_CAN_HEAR)) {
				      memset(write_frame.data, 255, write_frame.datalen);
				   }
				   else {
   					/* Check for output volume adjustments */
   					if (member->volume_out_level) {
   						switch_change_sln_volume(write_frame.data, write_frame.samples, member->volume_out_level);
   					}
				   }
					write_frame.timestamp = timer.samplecount;
					if (member->fnode) {
						member_add_file_data(member, write_frame.data, write_frame.datalen);
					}
					if (switch_core_session_write_frame(member->session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
						switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
						switch_mutex_unlock(member->audio_out_mutex);
						break;
					}
				}
			}

			switch_mutex_unlock(member->audio_out_mutex);
		} else if (member->fnode) {
			write_frame.datalen = bytes;
			write_frame.samples = samples;
			memset(write_frame.data, 255, write_frame.datalen);
			write_frame.timestamp = timer.samplecount;
			member_add_file_data(member, write_frame.data, write_frame.datalen);
			if (switch_core_session_write_frame(member->session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
				switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
				break;
			}
		} else if (!switch_test_flag(member->conference, CFLAG_WASTE_BANDWIDTH)) {
			if (switch_test_flag(member, MFLAG_WASTE_BANDWIDTH)) {
				if (member->conference->comfort_noise_level) {
					switch_generate_sln_silence(write_frame.data, samples, member->conference->comfort_noise_level);
				} else {
					memset(write_frame.data, 255, bytes);
				}

				write_frame.datalen = bytes;
				write_frame.samples = samples;
				write_frame.timestamp = timer.samplecount;

				if (switch_core_session_write_frame(member->session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
					switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
					break;
				}
			}
		}

		if (switch_test_flag(member, MFLAG_FLUSH_BUFFER)) {
			if (switch_buffer_inuse(member->mux_buffer)) {
				switch_mutex_lock(member->audio_out_mutex);
				switch_buffer_zero(member->mux_buffer);
				switch_mutex_unlock(member->audio_out_mutex);
			}
			switch_clear_flag_locked(member, MFLAG_FLUSH_BUFFER);
		}

		switch_mutex_unlock(member->write_mutex);


		if (switch_test_flag(member, MFLAG_INDICATE_MUTE)) {
			if (!zstr(member->conference->muted_sound)) {
				conference_member_play_file(member, member->conference->muted_sound, 0);
			} else {
				char msg[512];
				
				switch_snprintf(msg, sizeof(msg), "Muted");
				conference_member_say(member, msg, 0);
			}
			switch_clear_flag(member, MFLAG_INDICATE_MUTE);
		}

		if (switch_test_flag(member, MFLAG_INDICATE_MUTE_DETECT)) {
			if (!zstr(member->conference->mute_detect_sound)) {
				conference_member_play_file(member, member->conference->mute_detect_sound, 0);
			} else {
				char msg[512];
				
				switch_snprintf(msg, sizeof(msg), "Currently Muted");
				conference_member_say(member, msg, 0);
			}
			switch_clear_flag(member, MFLAG_INDICATE_MUTE_DETECT);
		}
		
		if (switch_test_flag(member, MFLAG_INDICATE_UNMUTE)) {
			if (!zstr(member->conference->unmuted_sound)) {
				conference_member_play_file(member, member->conference->unmuted_sound, 0);
			} else {
				char msg[512];
				
				switch_snprintf(msg, sizeof(msg), "Un-Muted");
				conference_member_say(member, msg, 0);
			}
			switch_clear_flag(member, MFLAG_INDICATE_UNMUTE);
		}

		if (switch_core_session_private_event_count(member->session)) {
			switch_channel_set_app_flag(channel, CF_APP_TAGGED);
			switch_ivr_parse_all_events(member->session);
			switch_channel_clear_app_flag(channel, CF_APP_TAGGED);
			switch_set_flag_locked(member, MFLAG_FLUSH_BUFFER);
			switch_core_session_set_read_codec(member->session, &member->read_codec);
		} else {
			switch_ivr_parse_all_messages(member->session);
		}

		if (use_timer) {
			switch_core_timer_next(&timer);
		} else {
			switch_cond_next();
		}

	} /* Rinse ... Repeat */

 end:

	switch_clear_flag_locked(member, MFLAG_RUNNING);
	switch_core_timer_destroy(&timer);

	switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG, "Channel leaving conference, cause: %s\n",
					  switch_channel_cause2str(switch_channel_get_cause(channel)));

	/* if it's an outbound channel, store the release cause in the conference struct, we might need it */
	if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
		member->conference->bridge_hangup_cause = switch_channel_get_cause(channel);
	}

	/* Wait for the input thread to end */
	while (switch_test_flag(member, MFLAG_ITHREAD)) {
		switch_cond_next();
	}
}

/* Sub-Routine called by a record entity inside a conference */
static void *SWITCH_THREAD_FUNC conference_record_thread_run(switch_thread_t *thread, void *obj)
{
	int16_t *data_buf;
	switch_file_handle_t fh = { 0 };
	conference_member_t smember = { 0 }, *member;
	conference_record_t *rec = (conference_record_t *) obj;
	conference_obj_t *conference = rec->conference;
	uint32_t samples = switch_samples_per_packet(conference->rate, conference->interval);
	uint32_t mux_used;
	char *vval;
	switch_timer_t timer = { 0 };
	uint32_t rlen;
	switch_size_t data_buf_len;
	switch_event_t *event;
	int no_data = 0;
	int lead_in = 20;
	switch_size_t len = 0;

	data_buf_len = samples * sizeof(int16_t);

	switch_zmalloc(data_buf, data_buf_len);

	if (switch_thread_rwlock_tryrdlock(conference->rwlock) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Read Lock Fail\n");
		return NULL;
	}

	switch_mutex_lock(globals.hash_mutex);
	globals.threads++;
	switch_mutex_unlock(globals.hash_mutex);

	member = &smember;

	member->flags = MFLAG_CAN_HEAR | MFLAG_NOCHANNEL | MFLAG_RUNNING;

	member->conference = conference;
	member->native_rate = conference->rate;
	member->rec_path = rec->path;
	fh.channels = 1;
	fh.samplerate = conference->rate;
	member->id = next_member_id();
	member->pool = rec->pool;

	member->frame_size = SWITCH_RECOMMENDED_BUFFER_SIZE;
	member->frame = switch_core_alloc(member->pool, member->frame_size);
	member->mux_frame = switch_core_alloc(member->pool, member->frame_size);


	switch_mutex_init(&member->write_mutex, SWITCH_MUTEX_NESTED, rec->pool);
	switch_mutex_init(&member->flag_mutex, SWITCH_MUTEX_NESTED, rec->pool);
	switch_mutex_init(&member->audio_in_mutex, SWITCH_MUTEX_NESTED, rec->pool);
	switch_mutex_init(&member->audio_out_mutex, SWITCH_MUTEX_NESTED, rec->pool);
	switch_mutex_init(&member->read_mutex, SWITCH_MUTEX_NESTED, rec->pool);
	switch_thread_rwlock_create(&member->rwlock, rec->pool);

	/* Setup an audio buffer for the incoming audio */
	if (switch_buffer_create_dynamic(&member->audio_buffer, CONF_DBLOCK_SIZE, CONF_DBUFFER_SIZE, 0) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error Creating Audio Buffer!\n");
		goto end;
	}

	/* Setup an audio buffer for the outgoing audio */
	if (switch_buffer_create_dynamic(&member->mux_buffer, CONF_DBLOCK_SIZE, CONF_DBUFFER_SIZE, 0) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error Creating Audio Buffer!\n");
		goto end;
	}

	if (conference_add_member(conference, member) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Joining Conference\n");
		goto end;
	}

	fh.pre_buffer_datalen = SWITCH_DEFAULT_FILE_BUFFER_LEN;

	if (switch_core_file_open(&fh,
							  rec->path, (uint8_t) 1, conference->rate, SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT,
							  rec->pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Opening File [%s]\n", rec->path);
		goto end;
	}


	if (switch_core_timer_init(&timer, conference->timer_name, conference->interval, samples, rec->pool) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Setup timer success interval: %u  samples: %u\n", conference->interval, samples);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Timer Setup Failed.  Conference Cannot Start\n");
		goto end;
	}

	if ((vval = switch_mprintf("Conference %s", conference->name))) {
		switch_core_file_set_string(&fh, SWITCH_AUDIO_COL_STR_TITLE, vval);
		switch_safe_free(vval);
	}

	switch_core_file_set_string(&fh, SWITCH_AUDIO_COL_STR_ARTIST, "FreeSWITCH mod_conference Software Conference Module");

	if (test_eflag(conference, EFLAG_RECORD) &&
			switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_data(conference, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "start-recording");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Path", rec->path);
		switch_event_fire(&event);
	}

	while (switch_test_flag(member, MFLAG_RUNNING) && switch_test_flag(conference, CFLAG_RUNNING) && conference->count) {

		len = 0;

		if (lead_in) {
			lead_in--;
			goto loop;
		}
		
		mux_used = (uint32_t) switch_buffer_inuse(member->mux_buffer);

		if (switch_test_flag(member, MFLAG_FLUSH_BUFFER)) {
			if (mux_used) {
				switch_mutex_lock(member->audio_out_mutex);
				switch_buffer_zero(member->mux_buffer);
				switch_mutex_unlock(member->audio_out_mutex);
				mux_used = 0;
			}
			switch_clear_flag_locked(member, MFLAG_FLUSH_BUFFER);
		}

	again:

		if (switch_test_flag((&fh), SWITCH_FILE_PAUSE)) {
			switch_set_flag_locked(member, MFLAG_FLUSH_BUFFER);
			goto loop;
		}

		if (mux_used >= data_buf_len) {
			/* Flush the output buffer and write all the data (presumably muxed) to the file */
			switch_mutex_lock(member->audio_out_mutex);
			//low_count = 0;

			if ((rlen = (uint32_t) switch_buffer_read(member->mux_buffer, data_buf, data_buf_len))) {
				len = (switch_size_t) rlen / sizeof(int16_t);
				no_data = 0;
			}
			switch_mutex_unlock(member->audio_out_mutex);
		}

		if (len == 0) {
			mux_used = (uint32_t) switch_buffer_inuse(member->mux_buffer);
				
			if (mux_used >= data_buf_len) {
				goto again;
			}

			if (++no_data < 2) {
				goto loop;
			}

			memset(data_buf, 255, (switch_size_t) data_buf_len);
			len = (switch_size_t) samples;
		}

		if (!len || switch_core_file_write(&fh, data_buf, &len) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Write Failed\n");
			switch_clear_flag_locked(member, MFLAG_RUNNING);
		}
		
	loop:

		switch_core_timer_next(&timer);
	}							/* Rinse ... Repeat */

  end:

	while(!no_data) {
		switch_mutex_lock(member->audio_out_mutex);
		if ((rlen = (uint32_t) switch_buffer_read(member->mux_buffer, data_buf, data_buf_len))) {
			len = (switch_size_t) rlen / sizeof(int16_t);
			switch_core_file_write(&fh, data_buf, &len);
		} else {
			no_data = 1;
		}
		switch_mutex_unlock(member->audio_out_mutex);
	}


	switch_safe_free(data_buf);
	switch_core_timer_destroy(&timer);
	conference_del_member(conference, member);
	switch_buffer_destroy(&member->audio_buffer);
	switch_buffer_destroy(&member->mux_buffer);
	switch_clear_flag_locked(member, MFLAG_RUNNING);
	if (switch_test_flag((&fh), SWITCH_FILE_OPEN)) {
		switch_core_file_close(&fh);
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Recording of %s Stopped\n", rec->path);
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_data(conference, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "stop-recording");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Path", rec->path);
		switch_event_fire(&event);
	}

	if (rec->pool) {
		switch_memory_pool_t *pool = rec->pool;
		rec = NULL;
		switch_core_destroy_memory_pool(&pool);
	}

	switch_mutex_lock(globals.hash_mutex);
	globals.threads--;
	switch_mutex_unlock(globals.hash_mutex);

	switch_thread_rwlock_unlock(conference->rwlock);
	return NULL;
}

/* Make files stop playing in a conference either the current one or all of them */
static uint32_t conference_stop_file(conference_obj_t *conference, file_stop_t stop)
{
	uint32_t count = 0;
	conference_file_node_t *nptr;

	switch_assert(conference != NULL);

	switch_mutex_lock(conference->mutex);

	if (stop == FILE_STOP_ALL) {
		for (nptr = conference->fnode; nptr; nptr = nptr->next) {
			nptr->done++;
			count++;
		}
		if (conference->async_fnode) {
			conference->async_fnode->done++;
			count++;
		}
	} else if (stop == FILE_STOP_ASYNC) {
		if (conference->async_fnode) {
			conference->async_fnode->done++;
			count++;
		}
	} else {
		if (conference->fnode) {
			conference->fnode->done++;
			count++;
		}
	}

	switch_mutex_unlock(conference->mutex);

	return count;
}

/* stop playing a file for the member of the conference */
static uint32_t conference_member_stop_file(conference_member_t *member, file_stop_t stop)
{
	conference_file_node_t *nptr;
	uint32_t count = 0;

	if (member == NULL)
		return count;

	lock_member(member);

	if (stop == FILE_STOP_ALL) {
		for (nptr = member->fnode; nptr; nptr = nptr->next) {
			nptr->done++;
			count++;
		}
	} else {
		if (member->fnode) {
			member->fnode->done++;
			count++;
		}
	}

	unlock_member(member);

	return count;
}

static void conference_send_all_dtmf(conference_member_t *member, conference_obj_t *conference, const char *dtmf)
{
	conference_member_t *imember;

	switch_mutex_lock(conference->mutex);
	switch_mutex_lock(conference->member_mutex);

	for (imember = conference->members; imember; imember = imember->next) {
		/* don't send to self */
		if (imember->id == member->id) {
			continue;
		}
		if (imember->session) {
			const char *p;
			for (p = dtmf; p && *p; p++) {
				switch_dtmf_t digit = { *p, SWITCH_DEFAULT_DTMF_DURATION };
				lock_member(imember);
				switch_core_session_kill_channel(imember->session, SWITCH_SIG_BREAK);
				switch_core_session_send_dtmf(imember->session, &digit);
				unlock_member(imember);
			}
		}
	}

	switch_mutex_unlock(conference->member_mutex);
	switch_mutex_unlock(conference->mutex);
}

/* Play a file in the conference room */
static switch_status_t conference_play_file(conference_obj_t *conference, char *file, uint32_t leadin, switch_channel_t *channel, uint8_t async)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	conference_file_node_t *fnode, *nptr = NULL;
	switch_memory_pool_t *pool;
	uint32_t count;
	char *dfile = NULL, *expanded = NULL;
	int say = 0;

	switch_assert(conference != NULL);

	if (zstr(file)) {
		return SWITCH_STATUS_NOTFOUND;
	}

	switch_mutex_lock(conference->mutex);
	switch_mutex_lock(conference->member_mutex);
	count = conference->count;
	switch_mutex_unlock(conference->member_mutex);
	switch_mutex_unlock(conference->mutex);

	if (!count)
		return SWITCH_STATUS_FALSE;

	if (channel) {
		if ((expanded = switch_channel_expand_variables(channel, file)) != file) {
			file = expanded;
		} else {
			expanded = NULL;
		}
	}

	if (!strncasecmp(file, "say:", 4)) {
		say = 1;
	}

	if (!async && say) {
		status = conference_say(conference, file + 4, leadin);
		goto done;
	}

	if (!switch_is_file_path(file)) {
		if (!say && conference->sound_prefix) {
			if (!(dfile = switch_mprintf("%s%s%s", conference->sound_prefix, SWITCH_PATH_SEPARATOR, file))) {
				goto done;
			}
			file = dfile;
		} else if (!async) {
			status = conference_say(conference, file, leadin);
			goto done;
		} else {
			goto done;
		}
	}

	/* Setup a memory pool to use. */
	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Pool Failure\n");
		status = SWITCH_STATUS_MEMERR;
		goto done;
	}

	/* Create a node object */
	if (!(fnode = switch_core_alloc(pool, sizeof(*fnode)))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Alloc Failure\n");
		switch_core_destroy_memory_pool(&pool);
		status = SWITCH_STATUS_MEMERR;
		goto done;
	}

	fnode->type = NODE_TYPE_FILE;
	fnode->leadin = leadin;

	/* Open the file */
	fnode->fh.pre_buffer_datalen = SWITCH_DEFAULT_FILE_BUFFER_LEN;
	if (switch_core_file_open(&fnode->fh, file, (uint8_t) 1, conference->rate, SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, pool) !=
		SWITCH_STATUS_SUCCESS) {
		switch_core_destroy_memory_pool(&pool);
		status = SWITCH_STATUS_NOTFOUND;
		goto done;
	}

	fnode->pool = pool;
	fnode->async = async;
	fnode->file = switch_core_strdup(fnode->pool, file);

	/* Queue the node */
	switch_mutex_lock(conference->mutex);

	if (async) {
		if (conference->async_fnode) {
			nptr = conference->async_fnode;
		}
		conference->async_fnode = fnode;

		if (nptr) {
			switch_memory_pool_t *tmppool;
			switch_core_file_close(&nptr->fh);
			tmppool = nptr->pool;
			switch_core_destroy_memory_pool(&tmppool);
		}

	} else {
		for (nptr = conference->fnode; nptr && nptr->next; nptr = nptr->next);

		if (nptr) {
			nptr->next = fnode;
		} else {
			conference->fnode = fnode;
		}
	}

	switch_mutex_unlock(conference->mutex);

  done:

	switch_safe_free(expanded);
	switch_safe_free(dfile);

	return status;
}

/* Play a file in the conference room to a member */
static switch_status_t conference_member_play_file(conference_member_t *member, char *file, uint32_t leadin)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	char *dfile = NULL, *expanded = NULL;
	conference_file_node_t *fnode, *nptr = NULL;
	switch_memory_pool_t *pool;

	if (member == NULL || file == NULL)
		return status;

	if ((expanded = switch_channel_expand_variables(switch_core_session_get_channel(member->session), file)) != file) {
		file = expanded;
	} else {
		expanded = NULL;
	}
	if (!strncasecmp(file, "say:", 4)) {
		if (!zstr(file + 4)) {
			status = conference_member_say(member, file + 4, leadin);
		}
		goto done;
	}
	if (!switch_is_file_path(file)) {
		if (member->conference->sound_prefix) {
			if (!(dfile = switch_mprintf("%s%s%s", member->conference->sound_prefix, SWITCH_PATH_SEPARATOR, file))) {
				goto done;
			}
			file = dfile;
		} else if (!zstr(file)) {
			status = conference_member_say(member, file, leadin);
			goto done;
		}
	}
	/* Setup a memory pool to use. */
	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_CRIT, "Pool Failure\n");
		status = SWITCH_STATUS_MEMERR;
		goto done;
	}
	/* Create a node object */
	if (!(fnode = switch_core_alloc(pool, sizeof(*fnode)))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_CRIT, "Alloc Failure\n");
		switch_core_destroy_memory_pool(&pool);
		status = SWITCH_STATUS_MEMERR;
		goto done;
	}
	fnode->type = NODE_TYPE_FILE;
	fnode->leadin = leadin;
	/* Open the file */
	fnode->fh.pre_buffer_datalen = SWITCH_DEFAULT_FILE_BUFFER_LEN;
	if (switch_core_file_open(&fnode->fh,
							  file, (uint8_t) 1, member->conference->rate, SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT,
							  pool) != SWITCH_STATUS_SUCCESS) {
		switch_core_destroy_memory_pool(&pool);
		status = SWITCH_STATUS_NOTFOUND;
		goto done;
	}
	fnode->pool = pool;
	fnode->file = switch_core_strdup(fnode->pool, file);
	/* Queue the node */
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG, "Queueing file '%s' for play\n", file);
	lock_member(member);
	for (nptr = member->fnode; nptr && nptr->next; nptr = nptr->next);
	if (nptr) {
		nptr->next = fnode;
	} else {
		member->fnode = fnode;
	}
	unlock_member(member);
	status = SWITCH_STATUS_SUCCESS;

  done:

	switch_safe_free(expanded);
	switch_safe_free(dfile);

	return status;
}

/* Say some thing with TTS in the conference room */
static switch_status_t conference_member_say(conference_member_t *member, char *text, uint32_t leadin)
{
	conference_obj_t *conference = (member != NULL ? member->conference : NULL);
	conference_file_node_t *fnode, *nptr;
	switch_memory_pool_t *pool;
	switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_NONE;
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (member == NULL || zstr(text))
		return SWITCH_STATUS_FALSE;

	switch_assert(conference != NULL);

	if (!(conference->tts_engine && conference->tts_voice)) {
		return SWITCH_STATUS_SUCCESS;
	}

	/* Setup a memory pool to use. */
	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_CRIT, "Pool Failure\n");
		return SWITCH_STATUS_MEMERR;
	}

	/* Create a node object */
	if (!(fnode = switch_core_alloc(pool, sizeof(*fnode)))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_CRIT, "Alloc Failure\n");
		switch_core_destroy_memory_pool(&pool);
		return SWITCH_STATUS_MEMERR;
	}

	fnode->type = NODE_TYPE_SPEECH;
	fnode->leadin = leadin;
	fnode->pool = pool;

	if (!member->sh) {
		memset(&member->lsh, 0, sizeof(member->lsh));
		if (switch_core_speech_open(&member->lsh, conference->tts_engine, conference->tts_voice,
									conference->rate, conference->interval, &flags, switch_core_session_get_pool(member->session)) !=
			SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_ERROR, "Invalid TTS module [%s]!\n", conference->tts_engine);
			return SWITCH_STATUS_FALSE;
		}
		member->sh = &member->lsh;
	}

	/* Queue the node */
	lock_member(member);
	for (nptr = member->fnode; nptr && nptr->next; nptr = nptr->next);

	if (nptr) {
		nptr->next = fnode;
	} else {
		member->fnode = fnode;
	}

	fnode->sh = member->sh;
	/* Begin Generation */
	switch_sleep(200000);

	if (*text == '#') {
		char *tmp = (char *) text + 1;
		char *vp = tmp, voice[128] = "";
		if ((tmp = strchr(tmp, '#'))) {
			text = tmp + 1;
			switch_copy_string(voice, vp, (tmp - vp) + 1);
			switch_core_speech_text_param_tts(fnode->sh, "voice", voice);
		}
	} else {
		switch_core_speech_text_param_tts(fnode->sh, "voice", conference->tts_voice);
	}

	switch_core_speech_feed_tts(fnode->sh, text, &flags);
	unlock_member(member);

	status = SWITCH_STATUS_SUCCESS;

	return status;
}

/* Say some thing with TTS in the conference room */
static switch_status_t conference_say(conference_obj_t *conference, const char *text, uint32_t leadin)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	conference_file_node_t *fnode, *nptr;
	switch_memory_pool_t *pool;
	switch_speech_flag_t flags = SWITCH_SPEECH_FLAG_NONE;
	uint32_t count;

	switch_assert(conference != NULL);

	if (zstr(text)) {
		return SWITCH_STATUS_GENERR;
	}

	switch_mutex_lock(conference->mutex);
	switch_mutex_lock(conference->member_mutex);
	count = conference->count;
	if (!(conference->tts_engine && conference->tts_voice)) {
		count = 0;
	}
	switch_mutex_unlock(conference->member_mutex);
	switch_mutex_unlock(conference->mutex);

	if (!count) {
		return SWITCH_STATUS_FALSE;
	}

	/* Setup a memory pool to use. */
	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Pool Failure\n");
		return SWITCH_STATUS_MEMERR;
	}

	/* Create a node object */
	if (!(fnode = switch_core_alloc(pool, sizeof(*fnode)))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Alloc Failure\n");
		switch_core_destroy_memory_pool(&pool);
		return SWITCH_STATUS_MEMERR;
	}

	fnode->type = NODE_TYPE_SPEECH;
	fnode->leadin = leadin;

	if (!conference->sh) {
		memset(&conference->lsh, 0, sizeof(conference->lsh));
		if (switch_core_speech_open(&conference->lsh, conference->tts_engine, conference->tts_voice,
									conference->rate, conference->interval, &flags, NULL) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid TTS module [%s]!\n", conference->tts_engine);
			return SWITCH_STATUS_FALSE;
		}
		conference->sh = &conference->lsh;
	}

	fnode->pool = pool;

	/* Queue the node */
	switch_mutex_lock(conference->mutex);
	for (nptr = conference->fnode; nptr && nptr->next; nptr = nptr->next);

	if (nptr) {
		nptr->next = fnode;
	} else {
		conference->fnode = fnode;
	}

	fnode->sh = conference->sh;
	if (*text == '#') {
		char *tmp = (char *) text + 1;
		char *vp = tmp, voice[128] = "";
		if ((tmp = strchr(tmp, '#'))) {
			text = tmp + 1;
			switch_copy_string(voice, vp, (tmp - vp) + 1);
			switch_core_speech_text_param_tts(fnode->sh, "voice", voice);
		}
	} else {
		switch_core_speech_text_param_tts(fnode->sh, "voice", conference->tts_voice);
	}

	/* Begin Generation */
	switch_sleep(200000);
	switch_core_speech_feed_tts(fnode->sh, (char *) text, &flags);
	switch_mutex_unlock(conference->mutex);
	status = SWITCH_STATUS_SUCCESS;

	return status;
}

/* execute a callback for every member of the conference */
static void conference_member_itterator(conference_obj_t *conference, switch_stream_handle_t *stream, conf_api_member_cmd_t pfncallback, void *data)
{
	conference_member_t *member = NULL;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);
	switch_assert(pfncallback != NULL);

	switch_mutex_lock(conference->member_mutex);
	for (member = conference->members; member; member = member->next) {
		if (member->session && !switch_test_flag(member, MFLAG_NOCHANNEL)) {
			pfncallback(member, stream, data);
		}
	}
	switch_mutex_unlock(conference->member_mutex);
}


static switch_status_t list_conferences(const char *line, const char *cursor, switch_console_callback_match_t **matches)
{
	switch_console_callback_match_t *my_matches = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_hash_index_t *hi;
	void *val;
	const void *vvar;

	switch_mutex_lock(globals.hash_mutex);
	for (hi = switch_hash_first(NULL, globals.conference_hash); hi; hi = switch_hash_next(hi)) {
		switch_hash_this(hi, &vvar, NULL, &val);
		switch_console_push_match(&my_matches, (const char *) vvar);		
	}
	switch_mutex_unlock(globals.hash_mutex);
	
	if (my_matches) {
		*matches = my_matches;
		status = SWITCH_STATUS_SUCCESS;
	}

	return status;
}

static void conference_list_pretty(conference_obj_t *conference, switch_stream_handle_t *stream)
{
	conference_member_t *member = NULL;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	switch_mutex_lock(conference->member_mutex);

	for (member = conference->members; member; member = member->next) {
		switch_channel_t *channel;
		switch_caller_profile_t *profile;

		if (switch_test_flag(member, MFLAG_NOCHANNEL)) {
			continue;
		}
		channel = switch_core_session_get_channel(member->session);
		profile = switch_channel_get_caller_profile(channel);

		stream->write_function(stream, "%u) %s (%s)\n", member->id, profile->caller_id_name, profile->caller_id_number);
	}

	switch_mutex_unlock(conference->member_mutex);
}

static void conference_list(conference_obj_t *conference, switch_stream_handle_t *stream, char *delim)
{
	conference_member_t *member = NULL;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);
	switch_assert(delim != NULL);

	switch_mutex_lock(conference->member_mutex);

	for (member = conference->members; member; member = member->next) {
		switch_channel_t *channel;
		switch_caller_profile_t *profile;
		char *uuid;
		char *name;
		uint32_t count = 0;

		if (switch_test_flag(member, MFLAG_NOCHANNEL)) {
			continue;
		}

		uuid = switch_core_session_get_uuid(member->session);
		channel = switch_core_session_get_channel(member->session);
		profile = switch_channel_get_caller_profile(channel);
		name = switch_channel_get_name(channel);

		stream->write_function(stream, "%u%s%s%s%s%s%s%s%s%s",
							   member->id, delim, name, delim, uuid, delim, profile->caller_id_name, delim, profile->caller_id_number, delim);

		if (switch_test_flag(member, MFLAG_CAN_HEAR)) {
			stream->write_function(stream, "hear");
			count++;
		}

		if (switch_test_flag(member, MFLAG_CAN_SPEAK)) {
			stream->write_function(stream, "%s%s", count ? "|" : "", "speak");
			count++;
		}

		if (switch_test_flag(member, MFLAG_TALKING)) {
			stream->write_function(stream, "%s%s", count ? "|" : "", "talking");
			count++;
		}

		if (switch_channel_test_flag(switch_core_session_get_channel(member->session), CF_VIDEO)) {
			stream->write_function(stream, "%s%s", count ? "|" : "", "video");
			count++;
		}

		if (member == member->conference->floor_holder) {
			stream->write_function(stream, "%s%s", count ? "|" : "", "floor");
			count++;
		}

		stream->write_function(stream, "%s%d%s%d%s%d%s%d\n", delim,
							   member->volume_in_level, 
							   delim,
							   member->agc_volume_in_level,
							   delim, member->volume_out_level, delim, member->energy_level);
	}

	switch_mutex_unlock(conference->member_mutex);
}

static void conference_list_count_only(conference_obj_t *conference, switch_stream_handle_t *stream)
{
	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	stream->write_function(stream, "%d", conference->count);
}

static switch_status_t conf_api_sub_mute(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL)
		return SWITCH_STATUS_GENERR;

	switch_clear_flag_locked(member, MFLAG_CAN_SPEAK);
	switch_clear_flag_locked(member, MFLAG_TALKING);

	switch_set_flag(member, MFLAG_INDICATE_MUTE);

	if (stream != NULL) {
		stream->write_function(stream, "OK mute %u\n", member->id);
	}

	if (test_eflag(member->conference, EFLAG_MUTE_MEMBER) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "mute-member");
		switch_event_fire(&event);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_agc(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	int level;
	int on = 0;

	if (argc == 2) {
		stream->write_function(stream, "+OK CURRENT AGC LEVEL IS %d\n", conference->agc_level);
		return SWITCH_STATUS_SUCCESS;
	}


	if (!(on = !strcasecmp(argv[2], "on"))) {
		stream->write_function(stream, "+OK AGC DISABLED\n");
		conference->agc_level = 0;
		return SWITCH_STATUS_SUCCESS;
	}
	
	if (argc > 3) {
		level = atoi(argv[3]);
	} else {
		level = DEFAULT_AGC_LEVEL;
	}

	if (level > conference->energy_level) {
		conference->avg_score = 0;
		conference->avg_itt = 0;
		conference->avg_tally = 0;
		conference->agc_level = level;

		if (stream) {
			stream->write_function(stream, "OK AGC ENABLED %d\n", conference->agc_level);
		}
		
	} else {
		if (stream) {
			stream->write_function(stream, "-ERR invalid level\n");
		}
	}




	return SWITCH_STATUS_SUCCESS;
		
}

static switch_status_t conf_api_sub_unmute(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL)
		return SWITCH_STATUS_GENERR;

	switch_set_flag_locked(member, MFLAG_CAN_SPEAK);
	switch_set_flag(member, MFLAG_INDICATE_UNMUTE);

	if (stream != NULL) {
		stream->write_function(stream, "OK unmute %u\n", member->id);
	}

	if (test_eflag(member->conference, EFLAG_UNMUTE_MEMBER) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "unmute-member");
		switch_event_fire(&event);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_deaf(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL)
		return SWITCH_STATUS_GENERR;

	switch_clear_flag_locked(member, MFLAG_CAN_HEAR);
	if (stream != NULL) {
		stream->write_function(stream, "OK deaf %u\n", member->id);
	}
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "deaf-member");
		switch_event_fire(&event);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_undeaf(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL)
		return SWITCH_STATUS_GENERR;

	switch_set_flag_locked(member, MFLAG_CAN_HEAR);
	if (stream != NULL) {
		stream->write_function(stream, "OK undeaf %u\n", member->id);
	}
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "undeaf-member");
		switch_event_fire(&event);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_hup(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL) {
		return SWITCH_STATUS_GENERR;
	}
	
	switch_clear_flag(member, MFLAG_RUNNING);

	if (member->conference && test_eflag(member->conference, EFLAG_HUP_MEMBER)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_member_data(member, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "hup-member");
			switch_event_fire(&event);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_kick(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL) {
		return SWITCH_STATUS_GENERR;
	}
	
	switch_clear_flag(member, MFLAG_RUNNING);
	switch_set_flag_locked(member, MFLAG_KICKED);
	switch_core_session_kill_channel(member->session, SWITCH_SIG_BREAK);

	if (stream != NULL) {
		stream->write_function(stream, "OK kicked %u\n", member->id);
	}

	if (member->conference && test_eflag(member->conference, EFLAG_KICK_MEMBER)) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_member_data(member, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "kick-member");
			switch_event_fire(&event);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}


static switch_status_t conf_api_sub_dtmf(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;
	char *dtmf = (char *) data;

	if (member == NULL) {
		stream->write_function(stream, "Invalid member!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (zstr(dtmf)) {
		stream->write_function(stream, "Invalid input!\n");
		return SWITCH_STATUS_GENERR;
	}

	lock_member(member);
	switch_core_session_kill_channel(member->session, SWITCH_SIG_BREAK);
	switch_core_session_send_dtmf_string(member->session, (char *) data);
	unlock_member(member);

	if (stream != NULL) {
		stream->write_function(stream, "OK sent %s to %u\n", (char *) data, member->id);
	}

	if (test_eflag(member->conference, EFLAG_DTMF_MEMBER) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "dtmf-member");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Digits", dtmf);
		switch_event_fire(&event);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_energy(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL)
		return SWITCH_STATUS_GENERR;

	if (data) {
		lock_member(member);
		member->energy_level = atoi((char *) data);
		unlock_member(member);
	}
	if (stream != NULL) {
		stream->write_function(stream, "Energy %u = %d\n", member->id, member->energy_level);
	}
	if (test_eflag(member->conference, EFLAG_ENERGY_LEVEL_MEMBER) &&
		data && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "energy-level-member");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Energy-Level", "%d", member->energy_level);
		switch_event_fire(&event);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_volume_in(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL)
		return SWITCH_STATUS_GENERR;

	if (data) {
		lock_member(member);
		member->volume_in_level = atoi((char *) data);
		switch_normalize_volume(member->volume_in_level);
		unlock_member(member);
	}
	if (stream != NULL) {
		stream->write_function(stream, "Volume IN %u = %d\n", member->id, member->volume_in_level);
	}
	if (test_eflag(member->conference, EFLAG_VOLUME_IN_MEMBER) &&
		data && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "volume-in-member");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Volume-Level", "%d", member->volume_in_level);
		switch_event_fire(&event);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_volume_out(conference_member_t *member, switch_stream_handle_t *stream, void *data)
{
	switch_event_t *event;

	if (member == NULL)
		return SWITCH_STATUS_GENERR;

	if (data) {
		lock_member(member);
		member->volume_out_level = atoi((char *) data);
		switch_normalize_volume(member->volume_out_level);
		unlock_member(member);
	}
	if (stream != NULL) {
		stream->write_function(stream, "Volume OUT %u = %d\n", member->id, member->volume_out_level);
	}
	if (test_eflag(member->conference, EFLAG_VOLUME_OUT_MEMBER) && data &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "volume-out-member");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Volume-Level", "%d", member->volume_out_level);
		switch_event_fire(&event);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_list(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	int ret_status = SWITCH_STATUS_GENERR;
	int count = 0;
	switch_hash_index_t *hi;
	void *val;
	char *d = ";";
	int pretty = 0;
	int summary = 0;
	int countonly = 0;
	int argofs = (argc >= 2 && strcasecmp(argv[1], "list") == 0);	/* detect being called from chat vs. api */

	if (argv[1 + argofs]) {
		if (argv[2 + argofs] && !strcasecmp(argv[1 + argofs], "delim")) {
			d = argv[2 + argofs];

			if (*d == '"') {
				if (++d) {
					char *p;
					if ((p = strchr(d, '"'))) {
						*p = '\0';
					}
				} else {
					d = ";";
				}
			}
		} else if (strcasecmp(argv[1 + argofs], "pretty") == 0) {
			pretty = 1;
		} else if (strcasecmp(argv[1 + argofs], "summary") == 0) {
			summary = 1;
		} else if (strcasecmp(argv[1 + argofs], "count") == 0) {
			countonly = 1;
		}
	}

	if (conference == NULL) {
		switch_mutex_lock(globals.hash_mutex);
		for (hi = switch_hash_first(NULL, globals.conference_hash); hi; hi = switch_hash_next(hi)) {
			switch_hash_this(hi, NULL, NULL, &val);
			conference = (conference_obj_t *) val;

			stream->write_function(stream, "Conference %s (%u member%s rate: %u%s)\n",
								   conference->name,
								   conference->count,
								   conference->count == 1 ? "" : "s", conference->rate, switch_test_flag(conference, CFLAG_LOCKED) ? " locked" : "");
			count++;
			if (!summary) {
				if (pretty) {
					conference_list_pretty(conference, stream);
				} else {
					conference_list(conference, stream, d);
				}
			}
		}
		switch_mutex_unlock(globals.hash_mutex);
	} else {
		count++;
		if (countonly) {
			conference_list_count_only(conference, stream);
		} else if (pretty) {
			conference_list_pretty(conference, stream);
		} else {
			conference_list(conference, stream, d);
		}
	}

	if (!count) {
		stream->write_function(stream, "No active conferences.\n");
	}

	ret_status = SWITCH_STATUS_SUCCESS;

	return ret_status;
}


static switch_xml_t add_x_tag(switch_xml_t x_member, const char *name, const char *value, int off)
{
	switch_size_t dlen = strlen(value) * 3 + 1;
	char *data;
	switch_xml_t x_tag;

	x_tag = switch_xml_add_child_d(x_member, name, off);
	switch_assert(x_tag);

	switch_zmalloc(data, dlen);

	switch_url_encode(value, data, dlen);
	switch_xml_set_txt_d(x_tag, data);
	free(data);

	return x_tag;
}

static void conference_xlist(conference_obj_t *conference, switch_xml_t x_conference, int off)
{
	conference_member_t *member = NULL;
	switch_xml_t x_member = NULL, x_members = NULL, x_flags;
	int moff = 0;
	char i[30] = "";
	char *ival = i;
	switch_assert(conference != NULL);
	switch_assert(x_conference != NULL);

	switch_xml_set_attr_d(x_conference, "name", conference->name);
	switch_snprintf(i, sizeof(i), "%d", conference->count);
	switch_xml_set_attr_d(x_conference, "member-count", ival);
	switch_snprintf(i, sizeof(i), "%u", conference->rate);
	switch_xml_set_attr_d(x_conference, "rate", ival);
	switch_xml_set_attr_d(x_conference, "uuid", conference->uuid_str);
		
	if (switch_test_flag(conference, CFLAG_LOCKED)) {
		switch_xml_set_attr_d(x_conference, "locked", "true");
	}

	if (switch_test_flag(conference, CFLAG_DESTRUCT)) {
		switch_xml_set_attr_d(x_conference, "destruct", "true");
	}

	if (switch_test_flag(conference, CFLAG_WAIT_MOD)) {
		switch_xml_set_attr_d(x_conference, "wait_mod", "true");
	}

	if (switch_test_flag(conference, CFLAG_RUNNING)) {
		switch_xml_set_attr_d(x_conference, "running", "true");
	}

	if (switch_test_flag(conference, CFLAG_ANSWERED)) {
		switch_xml_set_attr_d(x_conference, "answered", "true");
	}

	if (switch_test_flag(conference, CFLAG_ENFORCE_MIN)) {
		switch_xml_set_attr_d(x_conference, "enforce_min", "true");
	}

	if (switch_test_flag(conference, CFLAG_BRIDGE_TO)) {
		switch_xml_set_attr_d(x_conference, "bridge_to", "true");
	}

	if (switch_test_flag(conference, CFLAG_DYNAMIC)) {
		switch_xml_set_attr_d(x_conference, "dynamic", "true");
	}

	if (switch_test_flag(conference, CFLAG_EXIT_SOUND)) {
		switch_xml_set_attr_d(x_conference, "exit_sound", "true");
	}
	
	if (switch_test_flag(conference, CFLAG_ENTER_SOUND)) {
		switch_xml_set_attr_d(x_conference, "enter_sound", "true");
	}
	
	if (conference->record_count > 0) {
		switch_xml_set_attr_d(x_conference, "recording", "true");
	}

	switch_snprintf(i, sizeof(i), "%d", switch_epoch_time_now(NULL) - conference->run_time);
	switch_xml_set_attr_d(x_conference, "run_time", ival);

	if (conference->agc_level) {
		char tmp[30] = "";
		switch_snprintf(tmp, sizeof(tmp), "%d", conference->agc_level);
		switch_xml_set_attr_d_buf(x_conference, "agc", tmp);
	}

	x_members = switch_xml_add_child_d(x_conference, "members", 0);
	switch_assert(x_members);

	switch_mutex_lock(conference->member_mutex);

	for (member = conference->members; member; member = member->next) {
		switch_channel_t *channel;
		switch_caller_profile_t *profile;
		char *uuid;
		//char *name;
		uint32_t count = 0;
		switch_xml_t x_tag;
		int toff = 0;
		char tmp[50] = "";

		if (switch_test_flag(member, MFLAG_NOCHANNEL)) {
			continue;
		}

		uuid = switch_core_session_get_uuid(member->session);
		channel = switch_core_session_get_channel(member->session);
		profile = switch_channel_get_caller_profile(channel);
		//name = switch_channel_get_name(channel);


		x_member = switch_xml_add_child_d(x_members, "member", moff++);
		switch_assert(x_member);

		switch_snprintf(i, sizeof(i), "%d", member->id);

		add_x_tag(x_member, "id", i, toff++);
		add_x_tag(x_member, "uuid", uuid, toff++);
		add_x_tag(x_member, "caller_id_name", profile->caller_id_name, toff++);
		add_x_tag(x_member, "caller_id_number", profile->caller_id_number, toff++);


		switch_snprintf(i, sizeof(i), "%d", switch_epoch_time_now(NULL) - member->join_time);
		add_x_tag(x_member, "join_time", i, toff++);
		
		switch_snprintf(i, sizeof(i), "%d", switch_epoch_time_now(NULL) - member->last_talking);
		add_x_tag(x_member, "last_talking", member->last_talking ? i : "N/A", toff++);

		switch_snprintf(i, sizeof(i), "%d", member->energy_level);
		add_x_tag(x_member, "energy", i, toff++);

		switch_snprintf(i, sizeof(i), "%d", member->volume_in_level);
		add_x_tag(x_member, "volume_in", i, toff++);

		switch_snprintf(i, sizeof(i), "%d", member->volume_out_level);
		add_x_tag(x_member, "volume_out", i, toff++);
		
		x_flags = switch_xml_add_child_d(x_member, "flags", count++);
		switch_assert(x_flags);

		x_tag = switch_xml_add_child_d(x_flags, "can_hear", count++);
		switch_xml_set_txt_d(x_tag, switch_test_flag(member, MFLAG_CAN_HEAR) ? "true" : "false");

		x_tag = switch_xml_add_child_d(x_flags, "can_speak", count++);
		switch_xml_set_txt_d(x_tag, switch_test_flag(member, MFLAG_CAN_SPEAK) ? "true" : "false");

		x_tag = switch_xml_add_child_d(x_flags, "mute_detect", count++);
		switch_xml_set_txt_d(x_tag, switch_test_flag(member, MFLAG_MUTE_DETECT) ? "true" : "false");

		x_tag = switch_xml_add_child_d(x_flags, "talking", count++);
		switch_xml_set_txt_d(x_tag, switch_test_flag(member, MFLAG_TALKING) ? "true" : "false");

		x_tag = switch_xml_add_child_d(x_flags, "has_video", count++);
		switch_xml_set_txt_d(x_tag, switch_channel_test_flag(switch_core_session_get_channel(member->session), CF_VIDEO) ? "true" : "false");

		x_tag = switch_xml_add_child_d(x_flags, "has_floor", count++);
		switch_xml_set_txt_d(x_tag, (member == member->conference->floor_holder) ? "true" : "false");

		x_tag = switch_xml_add_child_d(x_flags, "is_moderator", count++);
		switch_xml_set_txt_d(x_tag, switch_test_flag(member, MFLAG_MOD) ? "true" : "false");

		x_tag = switch_xml_add_child_d(x_flags, "end_conference", count++);
		switch_xml_set_txt_d(x_tag, switch_test_flag(member, MFLAG_ENDCONF) ? "true" : "false");

		switch_snprintf(tmp, sizeof(tmp), "%d", member->volume_out_level);
		x_tag = add_x_tag(x_member, "output-volume", tmp, toff++);

		switch_snprintf(tmp, sizeof(tmp), "%d", member->agc_volume_in_level ? member->agc_volume_in_level : member->volume_in_level);
		x_tag = add_x_tag(x_member, "input-volume", tmp, toff++);

		switch_snprintf(tmp, sizeof(tmp), "%d", member->agc_volume_in_level);
		x_tag = add_x_tag(x_member, "auto-adjusted-input-volume", tmp, toff++);

	}

	switch_mutex_unlock(conference->member_mutex);
}
static switch_status_t conf_api_sub_xml_list(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	int count = 0;
	switch_hash_index_t *hi;
	void *val;
	switch_xml_t x_conference, x_conferences;
	int off = 0;
	char *ebuf;

	x_conferences = switch_xml_new("conferences");
	switch_assert(x_conferences);

	if (conference == NULL) {
		switch_mutex_lock(globals.hash_mutex);
		for (hi = switch_hash_first(NULL, globals.conference_hash); hi; hi = switch_hash_next(hi)) {
			switch_hash_this(hi, NULL, NULL, &val);
			conference = (conference_obj_t *) val;

			x_conference = switch_xml_add_child_d(x_conferences, "conference", off++);
			switch_assert(conference);

			count++;
			conference_xlist(conference, x_conference, off);

		}
		switch_mutex_unlock(globals.hash_mutex);
	} else {
		x_conference = switch_xml_add_child_d(x_conferences, "conference", off++);
		switch_assert(conference);
		count++;
		conference_xlist(conference, x_conference, off);
	}


	ebuf = switch_xml_toxml(x_conferences, SWITCH_TRUE);

	stream->write_function(stream, "%s", ebuf);

	switch_xml_free(x_conferences);
	free(ebuf);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_play(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	int ret_status = SWITCH_STATUS_GENERR;
	switch_event_t *event;
	uint8_t async = 0;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	if ((argc == 4 && !strcasecmp(argv[3], "async")) || (argc == 5 && !strcasecmp(argv[4], "async"))) {
		argc--;
		async++;
	}

	if (argc == 3) {
		if (conference_play_file(conference, argv[2], 0, NULL, async) == SWITCH_STATUS_SUCCESS) {
			stream->write_function(stream, "(play) Playing file %s\n", argv[2]);
			if (test_eflag(conference, EFLAG_PLAY_FILE) &&
				switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
				conference_add_event_data(conference, event);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "play-file");
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "File", argv[2]);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Async", async ? "true" : "false");
				switch_event_fire(&event);
			}
		} else {
			stream->write_function(stream, "(play) File: %s not found.\n", argv[2] ? argv[2] : "(unspecified)");
		}
		ret_status = SWITCH_STATUS_SUCCESS;
	} else if (argc == 4) {
		uint32_t id = atoi(argv[3]);
		conference_member_t *member;

		if ((member = conference_member_get(conference, id))) {
			if (conference_member_play_file(member, argv[2], 0) == SWITCH_STATUS_SUCCESS) {
				stream->write_function(stream, "(play) Playing file %s to member %u\n", argv[2], id);
				if (test_eflag(conference, EFLAG_PLAY_FILE_MEMBER) &&
					switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
					conference_add_event_member_data(member, event);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "play-file-member");
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "File", argv[2]);
					switch_event_fire(&event);
				}
			} else {
				stream->write_function(stream, "(play) File: %s not found.\n", argv[2] ? argv[2] : "(unspecified)");
			}
			switch_thread_rwlock_unlock(member->rwlock);
			ret_status = SWITCH_STATUS_SUCCESS;
		} else {
			stream->write_function(stream, "Member: %u not found.\n", id);
		}
	}

	return ret_status;
}

static switch_status_t conf_api_sub_say(conference_obj_t *conference, switch_stream_handle_t *stream, const char *text)
{
	switch_event_t *event;

	if (zstr(text)) {
		stream->write_function(stream, "(say) Error! No text.\n");
		return SWITCH_STATUS_GENERR;
	}

	if (conference_say(conference, text, 0) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "(say) Error!\n");
		return SWITCH_STATUS_GENERR;
	}

	stream->write_function(stream, "(say) OK\n");
	if (test_eflag(conference, EFLAG_SPEAK_TEXT) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_data(conference, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "speak-text");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Text", text);
		switch_event_fire(&event);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_saymember(conference_obj_t *conference, switch_stream_handle_t *stream, const char *text)
{
	int ret_status = SWITCH_STATUS_GENERR;
	char *expanded = NULL;
	char *start_text = NULL;
	char *workspace = NULL;
	uint32_t id = 0;
	conference_member_t *member = NULL;
	switch_event_t *event;

	if (zstr(text)) {
		stream->write_function(stream, "(saymember) No Text!\n");
		goto done;
	}

	if (!(workspace = strdup(text))) {
		stream->write_function(stream, "(saymember) Memory Error!\n");
		goto done;
	}

	if ((start_text = strchr(workspace, ' '))) {
		*start_text++ = '\0';
		text = start_text;
	}

	id = atoi(workspace);

	if (!id || zstr(text)) {
		stream->write_function(stream, "(saymember) No Text!\n");
		goto done;
	}

	if (!(member = conference_member_get(conference, id))) {
		stream->write_function(stream, "(saymember) Unknown Member %u!\n", id);
		goto done;
	}

	if ((expanded = switch_channel_expand_variables(switch_core_session_get_channel(member->session), (char *) text)) != text) {
		text = expanded;
	} else {
		expanded = NULL;
	}

	if (!text || conference_member_say(member, (char *) text, 0) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "(saymember) Error!\n");
		goto done;
	}

	stream->write_function(stream, "(saymember) OK\n");
	if (test_eflag(member->conference, EFLAG_SPEAK_TEXT_MEMBER) &&
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_member_data(member, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "speak-text-member");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Text", text);
		switch_event_fire(&event);
	}
	ret_status = SWITCH_STATUS_SUCCESS;

  done:

	if (member) {
		switch_thread_rwlock_unlock(member->rwlock);
	}

	switch_safe_free(workspace);
	switch_safe_free(expanded);
	return ret_status;
}

static switch_status_t conf_api_sub_stop(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	uint8_t current = 0, all = 0, async = 0;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	if (argc > 2) {
		current = strcasecmp(argv[2], "current") ? 0 : 1;
		all = strcasecmp(argv[2], "all") ? 0 : 1;
		async = strcasecmp(argv[2], "async") ? 0 : 1;
	} else {
		all = 1;
	}

	if (!(current || all || async))
		return SWITCH_STATUS_GENERR;

	if (argc == 4) {
		uint32_t id = atoi(argv[3]);
		conference_member_t *member;

		if ((member = conference_member_get(conference, id))) {
			uint32_t stopped = conference_member_stop_file(member, async ? FILE_STOP_ASYNC : current ? FILE_STOP_CURRENT : FILE_STOP_ALL);
			stream->write_function(stream, "Stopped %u files.\n", stopped);
			switch_thread_rwlock_unlock(member->rwlock);
		} else {
			stream->write_function(stream, "Member: %u not found.\n", id);
		}
	} else {
		uint32_t stopped = conference_stop_file(conference, async ? FILE_STOP_ASYNC : current ? FILE_STOP_CURRENT : FILE_STOP_ALL);
		stream->write_function(stream, "Stopped %u files.\n", stopped);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_relate(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	uint8_t nospeak = 0, nohear = 0, clear = 0;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	if (argc <= 4)
		return SWITCH_STATUS_GENERR;

	nospeak = strstr(argv[4], "nospeak") ? 1 : 0;
	nohear = strstr(argv[4], "nohear") ? 1 : 0;

	if (!strcasecmp(argv[4], "clear")) {
		clear = 1;
	}

	if (!(clear || nospeak || nohear)) {
		return SWITCH_STATUS_GENERR;
	}

	if (clear) {
		conference_member_t *member = NULL;
		uint32_t id = atoi(argv[2]);
		uint32_t oid = atoi(argv[3]);

		if ((member = conference_member_get(conference, id))) {
			member_del_relationship(member, oid);
			stream->write_function(stream, "relationship %u->%u cleared.\n", id, oid);
			switch_thread_rwlock_unlock(member->rwlock);
		} else {
			stream->write_function(stream, "relationship %u->%u not found.\n", id, oid);
		}
		return SWITCH_STATUS_SUCCESS;
	}

	if (nospeak || nohear) {
		conference_member_t *member = NULL, *other_member = NULL;
		uint32_t id = atoi(argv[2]);
		uint32_t oid = atoi(argv[3]);

		if ((member = conference_member_get(conference, id))) {
			other_member = conference_member_get(conference, oid);
		}

		if (member && other_member) {
			conference_relationship_t *rel = NULL;

			if ((rel = member_get_relationship(member, other_member))) {
				rel->flags = 0;
			} else {
				rel = member_add_relationship(member, oid);
			}

			if (rel) {
				switch_set_flag(rel, RFLAG_CAN_SPEAK | RFLAG_CAN_HEAR);
				if (nospeak) {
					switch_clear_flag(rel, RFLAG_CAN_SPEAK);
					switch_clear_flag_locked(member, MFLAG_TALKING);
				}
				if (nohear) {
					switch_clear_flag(rel, RFLAG_CAN_HEAR);
				}
				stream->write_function(stream, "ok %u->%u set\n", id, oid);
			} else {
				stream->write_function(stream, "error!\n");
			}
		} else {
			stream->write_function(stream, "relationship %u->%u not found.\n", id, oid);
		}

		if (member) {
			switch_thread_rwlock_unlock(member->rwlock);
		}

		if (other_member) {
			switch_thread_rwlock_unlock(other_member->rwlock);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_lock(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_event_t *event;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	if (conference->is_locked_sound) {
		conference_play_file(conference, conference->is_locked_sound, CONF_DEFAULT_LEADIN, NULL, 0);
	}

	switch_set_flag_locked(conference, CFLAG_LOCKED);
	stream->write_function(stream, "OK %s locked\n", argv[0]);
	if (test_eflag(conference, EFLAG_LOCK) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_data(conference, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "lock");
		switch_event_fire(&event);
	}

	return 0;
}

static switch_status_t conf_api_sub_unlock(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_event_t *event;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	if (conference->is_unlocked_sound) {
		conference_play_file(conference, conference->is_unlocked_sound, CONF_DEFAULT_LEADIN, NULL, 0);
	}

	switch_clear_flag_locked(conference, CFLAG_LOCKED);
	stream->write_function(stream, "OK %s unlocked\n", argv[0]);
	if (test_eflag(conference, EFLAG_UNLOCK) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
		conference_add_event_data(conference, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "unlock");
		switch_event_fire(&event);
	}

	return 0;
}

static switch_status_t conf_api_sub_exit_sound(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_event_t *event;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);
	
	if (argc <= 2) {
		stream->write_function(stream, "Not enough args\n");
		return SWITCH_STATUS_GENERR;
	}
	
	if ( !strcasecmp(argv[2], "on") ) {
		switch_set_flag_locked(conference, CFLAG_EXIT_SOUND);
		stream->write_function(stream, "OK %s exit sounds on (%s)\n", argv[0], conference->exit_sound);
		if (test_eflag(conference, EFLAG_LOCK) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_data(conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "exit-sounds-on");
			switch_event_fire(&event);
		}
	} else if ( !strcasecmp(argv[2], "off") || !strcasecmp(argv[2], "none") ) {
		switch_clear_flag_locked(conference, CFLAG_EXIT_SOUND);
		stream->write_function(stream, "OK %s exit sounds off (%s)\n", argv[0], conference->exit_sound);
		if (test_eflag(conference, EFLAG_LOCK) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_data(conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "exit-sounds-off");
			switch_event_fire(&event);
		}		
	} else if ( !strcasecmp(argv[2], "file") ) {
		if (! argv[3]) {
			stream->write_function(stream, "No filename specified\n");
		} else {
			/* TODO: if possible, verify file exists before setting it */
			stream->write_function(stream,"Old exit sound: [%s]\n", conference->exit_sound);
			conference->exit_sound = switch_core_strdup(conference->pool, argv[3]);
			stream->write_function(stream, "OK %s exit sound file set to %s\n", argv[0], conference->exit_sound);
			if (test_eflag(conference, EFLAG_LOCK) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
				conference_add_event_data(conference, event);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "exit-sound-file-changed");
				switch_event_fire(&event);
			}			
		}
	} else {
		stream->write_function(stream, "Bad args\n");
		return SWITCH_STATUS_GENERR;		
	}
	
	return 0;
}


static switch_status_t conf_api_sub_enter_sound(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_event_t *event;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);
	
	if (argc <= 2) {
		stream->write_function(stream, "Not enough args\n");
		return SWITCH_STATUS_GENERR;
	}
	
	if ( !strcasecmp(argv[2], "on") ) {
		switch_set_flag_locked(conference, CFLAG_ENTER_SOUND);
		stream->write_function(stream, "OK %s enter sounds on (%s)\n", argv[0], conference->enter_sound);
		if (test_eflag(conference, EFLAG_LOCK) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_data(conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "enter-sounds-on");
			switch_event_fire(&event);
		}
	} else if ( !strcasecmp(argv[2], "off") || !strcasecmp(argv[2], "none") ) {
		switch_clear_flag_locked(conference, CFLAG_ENTER_SOUND);
		stream->write_function(stream, "OK %s enter sounds off (%s)\n", argv[0], conference->enter_sound);
		if (test_eflag(conference, EFLAG_LOCK) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_data(conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "enter-sounds-off");
			switch_event_fire(&event);
		}		
	} else if ( !strcasecmp(argv[2], "file") ) {
		if (! argv[3]) {
			stream->write_function(stream, "No filename specified\n");
		} else {
			/* TODO: verify file exists before setting it */
			conference->enter_sound = switch_core_strdup(conference->pool, argv[3]);
			stream->write_function(stream, "OK %s enter sound file set to %s\n", argv[0], conference->enter_sound);
			if (test_eflag(conference, EFLAG_LOCK) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
				conference_add_event_data(conference, event);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "enter-sound-file-changed");
				switch_event_fire(&event);
			}			
		}
	} else {
		stream->write_function(stream, "Bad args\n");
		return SWITCH_STATUS_GENERR;		
	}
	
	return 0;
}


static switch_status_t conf_api_sub_dial(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_call_cause_t cause;

	switch_assert(stream != NULL);

	if (argc <= 2) {
		stream->write_function(stream, "Bad Args\n");
		return SWITCH_STATUS_GENERR;
	}

	if (conference) {
		conference_outcall(conference, NULL, NULL, argv[2], 60, NULL, argv[4], argv[3], NULL, &cause, NULL);
	} else {
		conference_outcall(NULL, argv[0], NULL, argv[2], 60, NULL, argv[4], argv[3], NULL, &cause, NULL);
	}
	stream->write_function(stream, "Call Requested: result: [%s]\n", switch_channel_cause2str(cause));

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_bgdial(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_uuid_t uuid;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];

	switch_assert(stream != NULL);

	if (argc <= 2) {
		stream->write_function(stream, "Bad Args\n");
		return SWITCH_STATUS_GENERR;
	}

	switch_uuid_get(&uuid);
	switch_uuid_format(uuid_str, &uuid);

	if (conference) {
		conference_outcall_bg(conference, NULL, NULL, argv[2], 60, NULL, argv[4], argv[3], uuid_str, NULL, NULL);
	} else {
		conference_outcall_bg(NULL, argv[0], NULL, argv[2], 60, NULL, argv[4], argv[3], uuid_str, NULL, NULL);
	}

	stream->write_function(stream, "OK Job-UUID: %s\n", uuid_str);

	return SWITCH_STATUS_SUCCESS;
}



static switch_status_t conf_api_sub_transfer(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_status_t ret_status = SWITCH_STATUS_SUCCESS;
	char *conf_name = NULL, *profile_name;
	switch_event_t *params = NULL;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);
	
	if (argc > 3 && !zstr(argv[2])) {
		int x;

		conf_name = strdup(argv[2]);

		if ((profile_name = strchr(conf_name, '@'))) {
			*profile_name++ = '\0';
		} else {
			profile_name = "default";
		}

		for (x = 3; x < argc; x++) {
			conference_member_t *member = NULL;
			uint32_t id = atoi(argv[x]);
			switch_channel_t *channel;
			switch_event_t *event;
			char *xdest = NULL;
			
			if (!id || !(member = conference_member_get(conference, id))) {
				stream->write_function(stream, "No Member %u in conference %s.\n", id, conference->name);
				continue;
			}

			channel = switch_core_session_get_channel(member->session);
			xdest = switch_core_session_sprintf(member->session, "conference:%s@%s", conf_name, profile_name);
			switch_ivr_session_transfer(member->session, xdest, "inline", NULL);
			
			switch_channel_set_variable(channel, "last_transfered_conference", conf_name);

			stream->write_function(stream, "OK Member '%d' sent to conference %s.\n", member->id, argv[2]);
			
			/* tell them what happened */
			if (test_eflag(conference, EFLAG_TRANSFER) &&
				switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
				conference_add_event_member_data(member, event);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Old-Conference-Name", conference->name);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "New-Conference-Name", argv[3]);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "transfer");
				switch_event_fire(&event);
			}

			switch_thread_rwlock_unlock(member->rwlock);
		}
	} else {
		ret_status = SWITCH_STATUS_GENERR;
	}

	if (params) {
		switch_event_destroy(&params);
	}

	switch_safe_free(conf_name);

	return ret_status;
}


static switch_status_t conf_api_sub_record(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	if (argc <= 2)
		return SWITCH_STATUS_GENERR;

	stream->write_function(stream, "Record file %s\n", argv[2]);
	conference->record_count++;
	launch_conference_record_thread(conference, argv[2]);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_norecord(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	int all;
	switch_event_t *event;

	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	if (argc <= 2)
		return SWITCH_STATUS_GENERR;

	all = (strcasecmp(argv[2], "all") == 0);
	stream->write_function(stream, "Stop recording file %s\n", argv[2]);
	if (!conference_record_stop(conference, all ? NULL : argv[2]) && !all) {
		stream->write_function(stream, "non-existant recording '%s'\n", argv[2]);
	} else {
		if (all) {
			conference->record_count = 0;
		} else {
			conference->record_count--;
		}
		if (test_eflag(conference, EFLAG_RECORD) &&
				switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_data(conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "stop-recording");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Path", all ? "all" : argv[2]);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Other-Recordings", conference->record_count ? "true" : "false");
			switch_event_fire(&event);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t conf_api_sub_pin(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv)
{
	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	if ((argc == 3) && (!strcmp(argv[1], "pin"))) {
		conference->pin = switch_core_strdup(conference->pool, argv[2]);
		stream->write_function(stream, "Pin for conference %s set: %s\n", argv[0], conference->pin);
		return SWITCH_STATUS_SUCCESS;
	} else if (argc == 2 && (!strcmp(argv[1], "nopin"))) {
		conference->pin = NULL;
		stream->write_function(stream, "Pin for conference %s deleted\n", argv[0]);
		return SWITCH_STATUS_SUCCESS;
	} else {
		stream->write_function(stream, "Invalid parameters:\n");
		return SWITCH_STATUS_GENERR;
	}
}

typedef enum {
	CONF_API_COMMAND_LIST = 0,
	CONF_API_COMMAND_ENERGY,
	CONF_API_COMMAND_VOLUME_IN,
	CONF_API_COMMAND_VOLUME_OUT,
	CONF_API_COMMAND_PLAY,
	CONF_API_COMMAND_SAY,
	CONF_API_COMMAND_SAYMEMBER,
	CONF_API_COMMAND_STOP,
	CONF_API_COMMAND_DTMF,
	CONF_API_COMMAND_KICK,
	CONF_API_COMMAND_MUTE,
	CONF_API_COMMAND_UNMUTE,
	CONF_API_COMMAND_DEAF,
	CONF_API_COMMAND_UNDEAF,
	CONF_API_COMMAND_RELATE,
	CONF_API_COMMAND_LOCK,
	CONF_API_COMMAND_UNLOCK,
	CONF_API_COMMAND_DIAL,
	CONF_API_COMMAND_BGDIAL,
	CONF_API_COMMAND_TRANSFER,
	CONF_API_COMMAND_RECORD,
	CONF_API_COMMAND_NORECORD,
	CONF_API_COMMAND_EXIT_SOUND,
	CONF_API_COMMAND_ENTER_SOUND,
} api_command_type_t;

/* API Interface Function sub-commands */
/* Entries in this list should be kept in sync with the enum above */
static api_command_t conf_api_sub_commands[] = {
	{"list", (void_fn_t) & conf_api_sub_list, CONF_API_SUB_ARGS_SPLIT, "list", "[delim <string>]"},
	{"xml_list", (void_fn_t) & conf_api_sub_xml_list, CONF_API_SUB_ARGS_SPLIT, "xml_list", ""},
	{"energy", (void_fn_t) & conf_api_sub_energy, CONF_API_SUB_MEMBER_TARGET, "energy", "<member_id|all|last> [<newval>]"},
	{"volume_in", (void_fn_t) & conf_api_sub_volume_in, CONF_API_SUB_MEMBER_TARGET, "volume_in", "<member_id|all|last> [<newval>]"},
	{"volume_out", (void_fn_t) & conf_api_sub_volume_out, CONF_API_SUB_MEMBER_TARGET, "volume_out", "<member_id|all|last> [<newval>]"},
	{"play", (void_fn_t) & conf_api_sub_play, CONF_API_SUB_ARGS_SPLIT, "play", "<file_path> [async|<member_id>]"},
	{"say", (void_fn_t) & conf_api_sub_say, CONF_API_SUB_ARGS_AS_ONE, "say", "<text>"},
	{"saymember", (void_fn_t) & conf_api_sub_saymember, CONF_API_SUB_ARGS_AS_ONE, "saymember", "<member_id> <text>"},
	{"stop", (void_fn_t) & conf_api_sub_stop, CONF_API_SUB_ARGS_SPLIT, "stop", "<[current|all|async|last]> [<member_id>]"},
	{"dtmf", (void_fn_t) & conf_api_sub_dtmf, CONF_API_SUB_MEMBER_TARGET, "dtmf", "<[member_id|all|last]> <digits>"},
	{"kick", (void_fn_t) & conf_api_sub_kick, CONF_API_SUB_MEMBER_TARGET, "kick", "<[member_id|all|last]>"},
	{"hup", (void_fn_t) & conf_api_sub_hup, CONF_API_SUB_MEMBER_TARGET, "hup", "<[member_id|all|last]>"},
	{"mute", (void_fn_t) & conf_api_sub_mute, CONF_API_SUB_MEMBER_TARGET, "mute", "<[member_id|all]|last>"},
	{"unmute", (void_fn_t) & conf_api_sub_unmute, CONF_API_SUB_MEMBER_TARGET, "unmute", "<[member_id|all]|last>"},
	{"deaf", (void_fn_t) & conf_api_sub_deaf, CONF_API_SUB_MEMBER_TARGET, "deaf", "<[member_id|all]|last>"},
	{"undeaf", (void_fn_t) & conf_api_sub_undeaf, CONF_API_SUB_MEMBER_TARGET, "undeaf", "<[member_id|all]|last>"},
	{"relate", (void_fn_t) & conf_api_sub_relate, CONF_API_SUB_ARGS_SPLIT, "relate", "<member_id> <other_member_id> [nospeak|nohear|clear]"},
	{"lock", (void_fn_t) & conf_api_sub_lock, CONF_API_SUB_ARGS_SPLIT, "lock", ""},
	{"unlock", (void_fn_t) & conf_api_sub_unlock, CONF_API_SUB_ARGS_SPLIT, "unlock", ""},
	{"agc", (void_fn_t) & conf_api_sub_agc, CONF_API_SUB_ARGS_SPLIT, "agc", ""},
	{"dial", (void_fn_t) & conf_api_sub_dial, CONF_API_SUB_ARGS_SPLIT, "dial", "<endpoint_module_name>/<destination> <callerid number> <callerid name>"},
	{"bgdial", (void_fn_t) & conf_api_sub_bgdial, CONF_API_SUB_ARGS_SPLIT, "bgdial", "<endpoint_module_name>/<destination> <callerid number> <callerid name>"},
	{"transfer", (void_fn_t) & conf_api_sub_transfer, CONF_API_SUB_ARGS_SPLIT, "transfer", "<conference_name> <member id> [...<member id>]"},
	{"record", (void_fn_t) & conf_api_sub_record, CONF_API_SUB_ARGS_SPLIT, "record", "<filename>"},
	{"norecord", (void_fn_t) & conf_api_sub_norecord, CONF_API_SUB_ARGS_SPLIT, "norecord", "<[filename|all]>"},
	{"exit_sound", (void_fn_t) & conf_api_sub_exit_sound, CONF_API_SUB_ARGS_SPLIT, "exit_sound", "on|off|none|file <filename>"},
	{"enter_sound", (void_fn_t) & conf_api_sub_enter_sound, CONF_API_SUB_ARGS_SPLIT, "enter_sound", "on|off|none|file <filename>"},
	{"pin", (void_fn_t) & conf_api_sub_pin, CONF_API_SUB_ARGS_SPLIT, "pin", "<pin#>"},
	{"nopin", (void_fn_t) & conf_api_sub_pin, CONF_API_SUB_ARGS_SPLIT, "nopin", ""},
};

#define CONFFUNCAPISIZE (sizeof(conf_api_sub_commands)/sizeof(conf_api_sub_commands[0]))

switch_status_t conf_api_dispatch(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv, const char *cmdline, int argn)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	uint32_t i, found = 0;
	switch_assert(conference != NULL);
	switch_assert(stream != NULL);

	/* loop through the command table to find a match */
	for (i = 0; i < CONFFUNCAPISIZE && !found; i++) {
		if (strcasecmp(argv[argn], conf_api_sub_commands[i].pname) == 0) {
			found = 1;
			switch (conf_api_sub_commands[i].fntype) {

				/* commands that we've broken the command line into arguments for */
			case CONF_API_SUB_ARGS_SPLIT:
				{
					conf_api_args_cmd_t pfn = (conf_api_args_cmd_t) conf_api_sub_commands[i].pfnapicmd;

					if (pfn(conference, stream, argc, argv) != SWITCH_STATUS_SUCCESS) {
						/* command returned error, so show syntax usage */
						stream->write_function(stream, "%s %s", conf_api_sub_commands[i].pcommand, conf_api_sub_commands[i].psyntax);
					}
				}
				break;

				/* member specific command that can be itteratted */
			case CONF_API_SUB_MEMBER_TARGET:
				{
					uint32_t id = 0;
					uint8_t all = 0;
					uint8_t last = 0;

					if (argv[argn + 1]) {
						if (!(id = atoi(argv[argn + 1]))) {
							all = strcasecmp(argv[argn + 1], "all") ? 0 : 1;
							last = strcasecmp(argv[argn + 1], "last") ? 0 : 1;
						}
					}

					if (all) {
						conference_member_itterator(conference, stream, (conf_api_member_cmd_t) conf_api_sub_commands[i].pfnapicmd, argv[argn + 2]);
					} else if (last) {
						conference_member_t *member = NULL;
						conference_member_t *last_member = NULL;

						switch_mutex_lock(conference->member_mutex);

						/* find last (oldest) member */
						member = conference->members;
						while (member != NULL) {
							if (last_member == NULL || member->id > last_member->id) {
								last_member = member;
							}
							member = member->next;
						}

						/* exec functio on last (oldest) member */
						if (last_member != NULL && last_member->session && !switch_test_flag(last_member, MFLAG_NOCHANNEL)) {
							conf_api_member_cmd_t pfn = (conf_api_member_cmd_t) conf_api_sub_commands[i].pfnapicmd;
							pfn(last_member, stream, argv[argn + 2]);
						}

						switch_mutex_unlock(conference->member_mutex);
					} else if (id) {
						conf_api_member_cmd_t pfn = (conf_api_member_cmd_t) conf_api_sub_commands[i].pfnapicmd;
						conference_member_t *member = conference_member_get(conference, id);

						if (member != NULL) {
							pfn(member, stream, argv[argn + 2]);
							switch_thread_rwlock_unlock(member->rwlock);
						} else {
							stream->write_function(stream, "Non-Existant ID %u\n", id);
						}
					} else {
						stream->write_function(stream, "%s %s", conf_api_sub_commands[i].pcommand, conf_api_sub_commands[i].psyntax);
					}
				}
				break;

				/* commands that deals with all text after command */
			case CONF_API_SUB_ARGS_AS_ONE:
				{
					conf_api_text_cmd_t pfn = (conf_api_text_cmd_t) conf_api_sub_commands[i].pfnapicmd;
					char *start_text;
					const char *modified_cmdline = cmdline;
					const char *cmd = conf_api_sub_commands[i].pname;

					if (!zstr(modified_cmdline) && (start_text = strstr(modified_cmdline, cmd))) {
						modified_cmdline = start_text + strlen(cmd);
						while (modified_cmdline && (*modified_cmdline == ' ' || *modified_cmdline == '\t')) {
							modified_cmdline++;
						}
					}

					/* call the command handler */
					if (pfn(conference, stream, modified_cmdline) != SWITCH_STATUS_SUCCESS) {
						/* command returned error, so show syntax usage */
						stream->write_function(stream, "%s %s", conf_api_sub_commands[i].pcommand, conf_api_sub_commands[i].psyntax);
					}
				}
				break;
			}
		}
	}

	if (!found) {
		stream->write_function(stream, "Conference command '%s' not found.\n", argv[argn]);
	} else {
		status = SWITCH_STATUS_SUCCESS;
	}

	return status;
}

/* API Interface Function */
SWITCH_STANDARD_API(conf_api_main)
{
	char *lbuf = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	char *http = NULL, *type = NULL;
	int argc;
	char *argv[25] = { 0 };

	if (!cmd) {
		cmd = "help";
	}

	if (stream->param_event) {
		http = switch_event_get_header(stream->param_event, "http-host");
		type = switch_event_get_header(stream->param_event, "content-type");
	}

	if (http) {
		/* Output must be to a web browser */
		if (type && !strcasecmp(type, "text/html")) {
			stream->write_function(stream, "<pre>\n");
		}
	}

	if (!(lbuf = strdup(cmd))) {
		return status;
	}

	argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0])));

	/* try to find a command to execute */
	if (argc && argv[0]) {
		conference_obj_t *conference = NULL;

		if ((conference = conference_find(argv[0]))) {
			if (switch_thread_rwlock_tryrdlock(conference->rwlock) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Read Lock Fail\n");
				goto done;
			}
			if (argc >= 2) {
				conf_api_dispatch(conference, stream, argc, argv, cmd, 1);
			} else {
				stream->write_function(stream, "Conference command, not specified.\nTry 'help'\n");
			}
			switch_thread_rwlock_unlock(conference->rwlock);

		} else if (argv[0]) {
			/* special case the list command, because it doesn't require a conference argument */
			if (strcasecmp(argv[0], "list") == 0) {
				conf_api_sub_list(NULL, stream, argc, argv);
			} else if (strcasecmp(argv[0], "xml_list") == 0) {
				conf_api_sub_xml_list(NULL, stream, argc, argv);
			} else if (strcasecmp(argv[0], "help") == 0 || strcasecmp(argv[0], "commands") == 0) {
				stream->write_function(stream, "%s\n", api_syntax);
			} else if (argv[1] && strcasecmp(argv[1], "dial") == 0) {
				if (conf_api_sub_dial(NULL, stream, argc, argv) != SWITCH_STATUS_SUCCESS) {
					/* command returned error, so show syntax usage */
					stream->write_function(stream, "%s %s", conf_api_sub_commands[CONF_API_COMMAND_DIAL].pcommand, 
										   conf_api_sub_commands[CONF_API_COMMAND_DIAL].psyntax);
				}
			} else if (argv[1] && strcasecmp(argv[1], "bgdial") == 0) {
				if (conf_api_sub_bgdial(NULL, stream, argc, argv) != SWITCH_STATUS_SUCCESS) {
					/* command returned error, so show syntax usage */
					stream->write_function(stream, "%s %s", conf_api_sub_commands[CONF_API_COMMAND_BGDIAL].pcommand,
										   conf_api_sub_commands[CONF_API_COMMAND_BGDIAL].psyntax);
				}
			} else {
				stream->write_function(stream, "Conference %s not found\n", argv[0]);
			}
		}

	} else {
		int i;

		for (i = 0; i < CONFFUNCAPISIZE; i++) {
			stream->write_function(stream, "<conf name> %s %s\n", conf_api_sub_commands[i].pcommand, conf_api_sub_commands[i].psyntax);
		}
	}

  done:
	switch_safe_free(lbuf);

	return status;
}

/* generate an outbound call from the conference */
static switch_status_t conference_outcall(conference_obj_t *conference,
										  char *conference_name,
										  switch_core_session_t *session,
										  char *bridgeto, uint32_t timeout, 
										  char *flags, char *cid_name, 
										  char *cid_num,
										  char *profile,
										  switch_call_cause_t *cause,
										  switch_call_cause_t *cancel_cause)
{
	switch_core_session_t *peer_session = NULL;
	switch_channel_t *peer_channel;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_channel_t *caller_channel = NULL;
	char appdata[512];
	int rdlock = 0;
	switch_bool_t have_flags = SWITCH_FALSE;

	*cause = SWITCH_CAUSE_NORMAL_CLEARING;

	if (conference == NULL) {
		char *dialstr = switch_mprintf("{ignore_early_media=true}%s", bridgeto);
		status = switch_ivr_originate(NULL, &peer_session, cause, dialstr, 60, NULL, cid_name, cid_num, NULL, NULL, SOF_NO_LIMITS, NULL);
		switch_safe_free(dialstr);

		if (status != SWITCH_STATUS_SUCCESS) {
			return status;
		}

		peer_channel = switch_core_session_get_channel(peer_session);
		rdlock = 1;
		goto callup;
	}

	conference_name = conference->name;

	if (switch_thread_rwlock_tryrdlock(conference->rwlock) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Read Lock Fail\n");
		return SWITCH_STATUS_FALSE;
	}

	if (session != NULL) {
		caller_channel = switch_core_session_get_channel(session);
	}

	if (zstr(cid_name)) {
		cid_name = conference->caller_id_name;
	}

	if (zstr(cid_num)) {
		cid_num = conference->caller_id_number;
	}

	/* establish an outbound call leg */

	switch_mutex_lock(conference->mutex);
	conference->originating++;
	switch_mutex_unlock(conference->mutex);
	status = switch_ivr_originate(session, &peer_session, cause, bridgeto, timeout, NULL, cid_name, cid_num, NULL, NULL, SOF_NO_LIMITS, cancel_cause);
	switch_mutex_lock(conference->mutex);
	conference->originating--;
	switch_mutex_unlock(conference->mutex);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot create outgoing channel, cause: %s\n",
						  switch_channel_cause2str(*cause));
		if (caller_channel) {
			switch_channel_hangup(caller_channel, *cause);
		}
		goto done;
	}

	rdlock = 1;
	peer_channel = switch_core_session_get_channel(peer_session);

	/* make sure the conference still exists */
	if (!switch_test_flag(conference, CFLAG_RUNNING)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Conference is gone now, nevermind..\n");
		if (caller_channel) {
			switch_channel_hangup(caller_channel, SWITCH_CAUSE_NO_ROUTE_DESTINATION);
		}
		switch_channel_hangup(peer_channel, SWITCH_CAUSE_NO_ROUTE_DESTINATION);
		goto done;
	}

	if (caller_channel && switch_channel_test_flag(peer_channel, CF_ANSWERED)) {
		switch_channel_answer(caller_channel);
	}

  callup:

	/* if the outbound call leg is ready */
	if (switch_channel_test_flag(peer_channel, CF_ANSWERED) || switch_channel_test_flag(peer_channel, CF_EARLY_MEDIA)) {
		switch_caller_extension_t *extension = NULL;

		/* build an extension name object */
		if ((extension = switch_caller_extension_new(peer_session, conference_name, conference_name)) == 0) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Memory Error!\n");
			status = SWITCH_STATUS_MEMERR;
			goto done;
		}

		if (flags && strcasecmp(flags, "none")) {
			have_flags = SWITCH_TRUE;
		}
		/* add them to the conference */

		switch_snprintf(appdata, sizeof(appdata), "%s%s%s%s%s%s", conference_name,
				profile?"@":"", profile?profile:"",
				have_flags?"+flags{":"", have_flags?flags:"", have_flags?"}":"");
		switch_caller_extension_add_application(peer_session, extension, (char *) global_app_name, appdata);

		switch_channel_set_caller_extension(peer_channel, extension);
		switch_channel_set_state(peer_channel, CS_EXECUTE);

	} else {
		switch_channel_hangup(peer_channel, SWITCH_CAUSE_NO_ANSWER);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

  done:
	if (conference) {
		switch_thread_rwlock_unlock(conference->rwlock);
	}
	if (rdlock && peer_session) {
		switch_core_session_rwunlock(peer_session);
	}

	return status;
}

struct bg_call {
	conference_obj_t *conference;
	switch_core_session_t *session;
	char *bridgeto;
	uint32_t timeout;
	char *flags;
	char *cid_name;
	char *cid_num;
	char *conference_name;
	char *uuid;
	char *profile;
	switch_call_cause_t *cancel_cause;
	switch_memory_pool_t *pool;
};

static void *SWITCH_THREAD_FUNC conference_outcall_run(switch_thread_t *thread, void *obj)
{
	struct bg_call *call = (struct bg_call *) obj;

	if (call) {
		switch_call_cause_t cause;
		switch_event_t *event;

		conference_outcall(call->conference, call->conference_name,
						   call->session, call->bridgeto, call->timeout, call->flags, call->cid_name, call->cid_num, call->profile, &cause, call->cancel_cause);

		if (call->conference && test_eflag(call->conference, EFLAG_BGDIAL_RESULT) &&
			switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, CONF_EVENT_MAINT) == SWITCH_STATUS_SUCCESS) {
			conference_add_event_data(call->conference, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "bgdial-result");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Result", switch_channel_cause2str(cause));
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Job-UUID", call->uuid);
			switch_event_fire(&event);
		}
		switch_safe_free(call->bridgeto);
		switch_safe_free(call->flags);
		switch_safe_free(call->cid_name);
		switch_safe_free(call->cid_num);
		switch_safe_free(call->conference_name);
		switch_safe_free(call->uuid);
		switch_safe_free(call->profile);
		if (call->pool) {
			switch_core_destroy_memory_pool(&call->pool);
		}
		switch_safe_free(call);
	}

	return NULL;
}

static switch_status_t conference_outcall_bg(conference_obj_t *conference,
											 char *conference_name,
											 switch_core_session_t *session, char *bridgeto, uint32_t timeout, const char *flags, const char *cid_name,
											 const char *cid_num, const char *call_uuid, const char *profile, switch_call_cause_t *cancel_cause)
{
	struct bg_call *call = NULL;
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
	switch_memory_pool_t *pool = NULL;

	if (!(call = malloc(sizeof(*call))))
		return SWITCH_STATUS_MEMERR;

	memset(call, 0, sizeof(*call));
	call->conference = conference;
	call->session = session;
	call->timeout = timeout;
	call->cancel_cause = cancel_cause;

	if (conference) {
		pool = conference->pool;
	} else {
		switch_core_new_memory_pool(&pool);
		call->pool = pool;
	}

	if (bridgeto) {
		call->bridgeto = strdup(bridgeto);
	}
	if (flags) {
		call->flags = strdup(flags);
	}
	if (cid_name) {
		call->cid_name = strdup(cid_name);
	}
	if (cid_num) {
		call->cid_num = strdup(cid_num);
	}

	if (conference_name) {
		call->conference_name = strdup(conference_name);
	}

	if (call_uuid) {
		call->uuid = strdup(call_uuid);
	}

        if (profile) {
                call->profile = strdup(profile);
        }

	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&thread, thd_attr, conference_outcall_run, call, pool);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Launching BG Thread for outcall\n");

	return SWITCH_STATUS_SUCCESS;
}

/* Play a file */
static switch_status_t conference_local_play_file(conference_obj_t *conference, switch_core_session_t *session, char *path, uint32_t leadin, void *buf,
												  uint32_t buflen)
{
	uint32_t x = 0;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_channel_t *channel;
	char *expanded = NULL;
	switch_input_args_t args = { 0 }, *ap = NULL;

	if (buf) {
		args.buf = buf;
		args.buflen = buflen;
		ap = &args;
	}

	/* generate some space infront of the file to be played */
	for (x = 0; x < leadin; x++) {
		switch_frame_t *read_frame;
		status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}
	}

	/* if all is well, really play the file */
	if (status == SWITCH_STATUS_SUCCESS) {
		char *dpath = NULL;

		channel = switch_core_session_get_channel(session);
		if ((expanded = switch_channel_expand_variables(channel, path)) != path) {
			path = expanded;
		} else {
			expanded = NULL;
		}

		if (!strncasecmp(path, "say:", 4)) {
			if (!(conference->tts_engine && conference->tts_voice)) {
				status = SWITCH_STATUS_FALSE;
			} else {
				status = switch_ivr_speak_text(session, conference->tts_engine, conference->tts_voice, path + 4, ap);
			}
			goto done;
		}

		if (!switch_is_file_path(path) && conference->sound_prefix) {
			if (!(dpath = switch_mprintf("%s%s%s", conference->sound_prefix, SWITCH_PATH_SEPARATOR, path))) {
				status = SWITCH_STATUS_MEMERR;
				goto done;
			}
			path = dpath;
		}

		status = switch_ivr_play_file(session, NULL, path, ap);
		switch_safe_free(dpath);
	}

  done:
	switch_safe_free(expanded);

	return status;
}

static void set_mflags(const char *flags, member_flag_t *f)
{
	if (flags) {
		char *dup = strdup(flags);
		char *p;
		char *argv[10] = { 0 };
		int i, argc = 0;

		for (p = dup; p && *p; p++) {
			if (*p == ',') {
				*p = '|';
			}
		}

		argc = switch_separate_string(dup, '|', argv, (sizeof(argv) / sizeof(argv[0])));

		for (i = 0; i < argc && argv[i]; i++) {
			if (!strcasecmp(argv[i], "mute")) {
				*f &= ~MFLAG_CAN_SPEAK;
				*f &= ~MFLAG_TALKING;
			} else if (!strcasecmp(argv[i], "deaf")) {
				*f &= ~MFLAG_CAN_HEAR;
			} else if (!strcasecmp(argv[i], "waste")) {
				*f |= MFLAG_WASTE_BANDWIDTH;
			} else if (!strcasecmp(argv[i], "mute-detect")) {
				*f |= MFLAG_MUTE_DETECT;
			} else if (!strcasecmp(argv[i], "dist-dtmf")) {
				*f |= MFLAG_DIST_DTMF;
			} else if (!strcasecmp(argv[i], "moderator")) {
				*f |= MFLAG_MOD;
			} else if (!strcasecmp(argv[i], "nomoh")) {
				*f |= MFLAG_NOMOH;
			} else if (!strcasecmp(argv[i], "endconf")) {
				*f |= MFLAG_ENDCONF;
			} else if (!strcasecmp(argv[i], "mintwo")) {
				*f |= MFLAG_MINTWO;
			} else if (!strcasecmp(argv[i], "video-bridge")) {
				*f |= MFLAG_VIDEO_BRIDGE;
			}
		}

		free(dup);
	}
}



static void set_cflags(const char *flags, uint32_t *f)
{
	if (flags) {
		char *dup = strdup(flags);
		char *p;
		char *argv[10] = { 0 };
		int i, argc = 0;

		for (p = dup; p && *p; p++) {
			if (*p == ',') {
				*p = '|';
			}
		}

		argc = switch_separate_string(dup, '|', argv, (sizeof(argv) / sizeof(argv[0])));

		for (i = 0; i < argc && argv[i]; i++) {
			if (!strcasecmp(argv[i], "wait-mod")) {
				*f |= CFLAG_WAIT_MOD;
			} else if (!strcasecmp(argv[i], "video-floor-only")) {
				*f |= CFLAG_VID_FLOOR;
			} else if (!strcasecmp(argv[i], "waste-bandwidth")) {
				*f |= CFLAG_WASTE_BANDWIDTH;
			} else if (!strcasecmp(argv[i], "video-bridge")) {
				*f |= CFLAG_VIDEO_BRIDGE;
			}
		}		

		free(dup);
	}
}


static void clear_eflags(char *events, uint32_t *f)
{
	char buf[512] = "";
	char *next = NULL;
	char *event = buf;

	if (events) {
		switch_copy_string(buf, events, sizeof(buf));

		while (event) {
			next = strchr(event, ',');
			if (next) {
				*next++ = '\0';
			}

			if (!strcmp(event, "add-member")) {
				*f &= ~EFLAG_ADD_MEMBER;
			} else if (!strcmp(event, "del-member")) {
				*f &= ~EFLAG_DEL_MEMBER;
			} else if (!strcmp(event, "energy-level")) {
				*f &= ~EFLAG_ENERGY_LEVEL;
			} else if (!strcmp(event, "volume-level")) {
				*f &= ~EFLAG_VOLUME_LEVEL;
			} else if (!strcmp(event, "gain-level")) {
				*f &= ~EFLAG_GAIN_LEVEL;
			} else if (!strcmp(event, "dtmf")) {
				*f &= ~EFLAG_DTMF;
			} else if (!strcmp(event, "stop-talking")) {
				*f &= ~EFLAG_STOP_TALKING;
			} else if (!strcmp(event, "start-talking")) {
				*f &= ~EFLAG_START_TALKING;
			} else if (!strcmp(event, "mute-detect")) {
				*f &= ~EFLAG_MUTE_DETECT;
			} else if (!strcmp(event, "mute-member")) {
				*f &= ~EFLAG_MUTE_MEMBER;
			} else if (!strcmp(event, "unmute-member")) {
				*f &= ~EFLAG_UNMUTE_MEMBER;
			} else if (!strcmp(event, "kick-member")) {
				*f &= ~EFLAG_KICK_MEMBER;
			} else if (!strcmp(event, "dtmf-member")) {
				*f &= ~EFLAG_DTMF_MEMBER;
			} else if (!strcmp(event, "energy-level-member")) {
				*f &= ~EFLAG_ENERGY_LEVEL_MEMBER;
			} else if (!strcmp(event, "volume-in-member")) {
				*f &= ~EFLAG_VOLUME_IN_MEMBER;
			} else if (!strcmp(event, "volume-out-member")) {
				*f &= ~EFLAG_VOLUME_OUT_MEMBER;
			} else if (!strcmp(event, "play-file")) {
				*f &= ~EFLAG_PLAY_FILE;
			} else if (!strcmp(event, "play-file-member")) {
				*f &= ~EFLAG_PLAY_FILE_MEMBER;
			} else if (!strcmp(event, "speak-text")) {
				*f &= ~EFLAG_SPEAK_TEXT;
			} else if (!strcmp(event, "speak-text-member")) {
				*f &= ~EFLAG_SPEAK_TEXT_MEMBER;
			} else if (!strcmp(event, "lock")) {
				*f &= ~EFLAG_LOCK;
			} else if (!strcmp(event, "unlock")) {
				*f &= ~EFLAG_UNLOCK;
			} else if (!strcmp(event, "transfer")) {
				*f &= ~EFLAG_TRANSFER;
			} else if (!strcmp(event, "bgdial-result")) {
				*f &= ~EFLAG_BGDIAL_RESULT;
			} else if (!strcmp(event, "floor-change")) {
				*f &= ~EFLAG_FLOOR_CHANGE;
			} else if (!strcmp(event, "record")) {
				*f &= ~EFLAG_RECORD;
			}

			event = next;
		}
	}
}

SWITCH_STANDARD_APP(conference_auto_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	call_list_t *call_list, *np;

	call_list = switch_channel_get_private(channel, "_conference_autocall_list_");

	if (zstr(data)) {
		call_list = NULL;
	} else {
		np = switch_core_session_alloc(session, sizeof(*np));
		switch_assert(np != NULL);

		np->string = switch_core_session_strdup(session, data);
		if (call_list) {
			np->next = call_list;
			np->iteration = call_list->iteration + 1;
		} else {
			np->iteration = 1;
		}
		call_list = np;
	}
	switch_channel_set_private(channel, "_conference_autocall_list_", call_list);
}


static int setup_media(conference_member_t *member, conference_obj_t *conference)
{
	switch_codec_implementation_t read_impl = { 0 };
	switch_core_session_get_read_impl(member->session, &read_impl);

	switch_core_session_reset(member->session, SWITCH_TRUE, SWITCH_FALSE);

	if (switch_core_codec_ready(&member->read_codec)) {
		switch_core_codec_destroy(&member->read_codec);
	}

	if (member->read_resampler) {
		switch_resample_destroy(&member->read_resampler);
	}


	switch_core_session_get_read_impl(member->session, &member->orig_read_impl);
	member->native_rate = read_impl.samples_per_second;

	/* Setup a Signed Linear codec for reading audio. */
	if (switch_core_codec_init(&member->read_codec,
							   "L16",
							   NULL, read_impl.actual_samples_per_second, read_impl.microseconds_per_packet / 1000,
							   1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, member->pool) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG,
						  "Raw Codec Activation Success L16@%uhz 1 channel %dms\n",
						  read_impl.actual_samples_per_second, read_impl.microseconds_per_packet / 1000);

	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG, "Raw Codec Activation Failed L16@%uhz 1 channel %dms\n",
						  read_impl.actual_samples_per_second, read_impl.microseconds_per_packet / 1000);

		goto done;
	}

	if (!member->frame_size) {
		member->frame_size = SWITCH_RECOMMENDED_BUFFER_SIZE;
		member->frame = switch_core_alloc(member->pool, member->frame_size);
		member->mux_frame = switch_core_alloc(member->pool, member->frame_size);
	}

	if (read_impl.actual_samples_per_second != conference->rate) {
		if (switch_resample_create(&member->read_resampler,
								   read_impl.actual_samples_per_second,
								   conference->rate, member->frame_size, SWITCH_RESAMPLE_QUALITY, 1) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_CRIT, "Unable to create resampler!\n");
			goto done;
		}


		member->resample_out = switch_core_alloc(member->pool, member->frame_size);
		member->resample_out_len = member->frame_size;

		/* Setup an audio buffer for the resampled audio */
		if (switch_buffer_create_dynamic(&member->resample_buffer, CONF_DBLOCK_SIZE, CONF_DBUFFER_SIZE, CONF_DBUFFER_MAX)
			!= SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_CRIT, "Memory Error Creating Audio Buffer!\n");
			goto done;
		}
	}


	/* Setup a Signed Linear codec for writing audio. */
	if (switch_core_codec_init(&member->write_codec,
							   "L16",
							   NULL,
							   conference->rate,
							   read_impl.microseconds_per_packet / 1000,
							   1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, member->pool) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG,
						  "Raw Codec Activation Success L16@%uhz 1 channel %dms\n", conference->rate, read_impl.microseconds_per_packet / 1000);
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_DEBUG, "Raw Codec Activation Failed L16@%uhz 1 channel %dms\n",
						  conference->rate, read_impl.microseconds_per_packet / 1000);
		goto codec_done2;
	}

	/* Setup an audio buffer for the incoming audio */
	if (switch_buffer_create_dynamic(&member->audio_buffer, CONF_DBLOCK_SIZE, CONF_DBUFFER_SIZE, CONF_DBUFFER_MAX) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_CRIT, "Memory Error Creating Audio Buffer!\n");
		goto codec_done1;
	}

	/* Setup an audio buffer for the outgoing audio */
	if (switch_buffer_create_dynamic(&member->mux_buffer, CONF_DBLOCK_SIZE, CONF_DBUFFER_SIZE, CONF_DBUFFER_MAX) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(member->session), SWITCH_LOG_CRIT, "Memory Error Creating Audio Buffer!\n");
		goto codec_done1;
	}

	return 0;

  codec_done1:
	switch_core_codec_destroy(&member->read_codec);
  codec_done2:
	switch_core_codec_destroy(&member->write_codec);
  done:

	return -1;


}

#define validate_pin(buf, pin, mpin) \
	pin_valid = (pin && strcmp(buf, pin) == 0); \
	if (!pin_valid && mpin && strcmp(buf, mpin) == 0) { \
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Moderator PIN found!\n"); \
		pin_valid = 1; \
		mpin_matched = 1; \
	} 
/* Application interface function that is called from the dialplan to join the channel to a conference */
SWITCH_STANDARD_APP(conference_function)
{
	switch_codec_t *read_codec = NULL;
	//uint32_t flags = 0;
	conference_member_t member = { 0 };
	conference_obj_t *conference = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	char *mydata = NULL;
	char *conf_name = NULL;
	char *bridge_prefix = "bridge:";
	char *flags_prefix = "+flags{";
	char *bridgeto = NULL;
	char *profile_name = NULL;
	switch_xml_t cxml = NULL, cfg = NULL, profiles = NULL;
	const char *flags_str;
	member_flag_t mflags = 0;
	switch_core_session_message_t msg = { 0 };
	uint8_t rl = 0, isbr = 0;
	char *dpin = "";
	char *mdpin = "";
	conf_xml_cfg_t xml_cfg = { 0 };
	switch_event_t *params = NULL;
	int locked = 0;
	int mpin_matched = 0;
	uint32_t *mid;

	if (!switch_channel_test_app_flag_key("conf_silent", channel, CONF_SILENT_DONE) &&
		(switch_channel_test_flag(channel, CF_RECOVERED) || switch_true(switch_channel_get_variable(channel, "conference_silent_entry")))) {
		switch_channel_set_app_flag_key("conf_silent", channel, CONF_SILENT_REQ);
	}

	if (switch_channel_answer(channel) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Channel answer failed.\n");
        return;
	}

	/* Save the original read codec. */
	if (!(read_codec = switch_core_session_get_read_codec(session))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Channel has no media!\n");
		return;
	}


	if (zstr(data)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Invalid arguments\n");
		return;
	}

	mydata = switch_core_session_strdup(session, data);

	if (!mydata) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Pool Failure\n");
		return;
	}

	if ((flags_str = strstr(mydata, flags_prefix))) {
		char *p;
		*((char *) flags_str) = '\0';
		flags_str += strlen(flags_prefix);
		if ((p = strchr(flags_str, '}'))) {
			*p = '\0';
		}
	} else {
		flags_str = switch_channel_get_variable(channel, "conference_member_flags");
	}

	/* is this a bridging conference ? */
	if (!strncasecmp(mydata, bridge_prefix, strlen(bridge_prefix))) {
		isbr = 1;
		mydata += strlen(bridge_prefix);
		if ((bridgeto = strchr(mydata, ':'))) {
			*bridgeto++ = '\0';
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Config Error!\n");
			goto done;
		}
	}

	conf_name = mydata;

	/* eat all leading spaces on conference name, which can cause problems */
	while (*conf_name == ' ') {
		conf_name++;
	}

	/* is there a conference pin ? */
	if ((dpin = strchr(conf_name, '+'))) {
		*dpin++ = '\0';
	}

	/* is there profile specification ? */
	if ((profile_name = strchr(conf_name, '@'))) {
		*profile_name++ = '\0';
	} else {
		profile_name = "default";
	}

#if 0
	if (0) {
		member.dtmf_parser = conference->dtmf_parser;
	} else {

	}
#endif

	if (switch_channel_test_flag(channel, CF_RECOVERED)) {
		const char *check = switch_channel_get_variable(channel, "last_transfered_conference");
		
		if (!zstr(check)) {
			conf_name = (char *) check;
		}
	}

	switch_event_create(&params, SWITCH_EVENT_COMMAND);
	switch_assert(params);
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "conf_name", conf_name);
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "profile_name", profile_name);

	/* Open the config from the xml registry */
	if (!(cxml = switch_xml_open_cfg(global_cf_name, &cfg, params))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Open of %s failed\n", global_cf_name);
		goto done;
	}

	if ((profiles = switch_xml_child(cfg, "profiles"))) {
		xml_cfg.profile = switch_xml_find_child(profiles, "profile", "name", profile_name);
	}

	/* if this is a bridging call, and it's not a duplicate, build a */
	/* conference object, and skip pin handling, and locked checking */

	if (!locked) {
		switch_mutex_lock(globals.setup_mutex);
		locked = 1;
	}

	if (isbr) {
		char *uuid = switch_core_session_get_uuid(session);

		if (!strcmp(conf_name, "_uuid_")) {
			conf_name = uuid;
		}

		if ((conference = conference_find(conf_name))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Conference %s already exists!\n", conf_name);
			goto done;
		}

		/* Create the conference object. */
		conference = conference_new(conf_name, xml_cfg, session, NULL);

		if (!conference) {
			goto done;
		}

		if (locked) {
			switch_mutex_unlock(globals.setup_mutex);
			locked = 0;
		}

		switch_channel_set_variable(channel, "conference_name", conference->name);

		/* Set the minimum number of members (once you go above it you cannot go below it) */
		conference->min = 2;

		/* Indicate the conference is dynamic */
		switch_set_flag_locked(conference, CFLAG_DYNAMIC);

		/* Indicate the conference has a bridgeto party */
		switch_set_flag_locked(conference, CFLAG_BRIDGE_TO);

		/* Start the conference thread for this conference */
		launch_conference_thread(conference);

	} else {
		int enforce_security =  switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_INBOUND;
		const char *pvar = switch_channel_get_variable(channel, "conference_enforce_security");

		if (pvar) {
			enforce_security = switch_true(pvar);
		}

		if ((conference = conference_find(conf_name))) {
			if (locked) {
				switch_mutex_unlock(globals.setup_mutex);
				locked = 0;
			}
		}

		/* if the conference exists, get the pointer to it */
		if (!conference) {
			const char *max_members_str;

			/* couldn't find the conference, create one */
			conference = conference_new(conf_name, xml_cfg, session, NULL);

			if (!conference) {
				goto done;
			}

			if (locked) {
				switch_mutex_unlock(globals.setup_mutex);
				locked = 0;
			}

			switch_channel_set_variable(channel, "conference_name", conference->name);

			/* Set MOH from variable if not set */
			if (zstr(conference->moh_sound)) {
				conference->moh_sound = switch_core_strdup(conference->pool, switch_channel_get_variable(channel, "conference_moh_sound"));
			}
			/* Set perpetual-sound from variable if not set */
			if (zstr(conference->perpetual_sound)) {
				conference->perpetual_sound = switch_core_strdup(conference->pool, switch_channel_get_variable(channel, "conference_perpetual_sound"));
			}

			/* Set the minimum number of members (once you go above it you cannot go below it) */
			conference->min = 1;

			/* check for variable used to specify override for max_members */
			if (!zstr(max_members_str = switch_channel_get_variable(channel, "conference_max_members"))) {
				uint32_t max_members_val;
				errno = 0;		/* sanity first */
				max_members_val = strtol(max_members_str, NULL, 0);	/* base 0 lets 0x... for hex 0... for octal and base 10 otherwise through */
				if (errno == ERANGE || errno == EINVAL || max_members_val < 0 || max_members_val == 1) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
									  "conference_max_members variable %s is invalid, not setting a limit\n", max_members_str);
				} else {
					conference->max_members = max_members_val;
				}
			}

			/* Indicate the conference is dynamic */
			switch_set_flag_locked(conference, CFLAG_DYNAMIC);

			/* Start the conference thread for this conference */
			launch_conference_thread(conference);
		} else {				/* setup user variable */
			switch_channel_set_variable(channel, "conference_name", conference->name);
		}

		/* acquire a read lock on the thread so it can't leave without us */
		if (switch_thread_rwlock_tryrdlock(conference->rwlock) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Read Lock Fail\n");
			goto done;
		}
		rl++;

		if (zstr(dpin) && conference->pin) {
			dpin = conference->pin;
		}
		if (zstr(mdpin) && conference->mpin) {
			mdpin = conference->mpin;
		}


		/* if this is not an outbound call, deal with conference pins */
		if (enforce_security && (!zstr(dpin) || !zstr(mdpin))) {
			char pin_buf[80] = "";
			int pin_retries = conference->pin_retries;
			int pin_valid = 0;
			switch_status_t status = SWITCH_STATUS_SUCCESS;
			char *supplied_pin_value;

			/* Answer the channel */
			switch_channel_answer(channel);

			/* look for PIN in channel variable first.  If not present or invalid revert to prompting user */
			supplied_pin_value = switch_core_strdup(conference->pool, switch_channel_get_variable(channel, "supplied_pin"));
			if (!zstr(supplied_pin_value)) {
				char *supplied_pin_value_start;
				int i = 0;
				if ((supplied_pin_value_start = (char *) switch_stristr(cf_pin_url_param_name, supplied_pin_value))) {
					/* pin supplied as a URL parameter, move pointer to start of actual pin value */
					supplied_pin_value = supplied_pin_value_start + strlen(cf_pin_url_param_name);
				}
				while (*supplied_pin_value != 0 && *supplied_pin_value != ';') {
					pin_buf[i++] = *supplied_pin_value++;
				}

				validate_pin(pin_buf, dpin, mdpin);
				memset(pin_buf, 0, sizeof(pin_buf));
			}

			if (!conference->pin_sound) {
				conference->pin_sound = switch_core_strdup(conference->pool, "conference/conf-pin.wav");
			}

			if (!conference->bad_pin_sound) {
				conference->bad_pin_sound = switch_core_strdup(conference->pool, "conference/conf-bad-pin.wav");
			}

			while (!pin_valid && pin_retries && status == SWITCH_STATUS_SUCCESS) {
				int maxpin = strlen(dpin) > strlen(mdpin) ? strlen(dpin) : strlen(mdpin);
				switch_status_t pstatus = SWITCH_STATUS_FALSE;

				/* be friendly */
				if (conference->pin_sound) {
					pstatus = conference_local_play_file(conference, session, conference->pin_sound, 20, pin_buf, sizeof(pin_buf));
				} else if (conference->tts_engine && conference->tts_voice) {
					pstatus =
						switch_ivr_speak_text(session, conference->tts_engine, conference->tts_voice, "please enter the conference pin number", NULL);
				} else {
					pstatus = switch_ivr_speak_text(session, "flite", "slt", "please enter the conference pin number", NULL);
				}

				if (pstatus != SWITCH_STATUS_SUCCESS && pstatus != SWITCH_STATUS_BREAK) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot ask the user for a pin, ending call\n");
					switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
				}

				/* wait for them if neccessary */
				if (strlen(pin_buf) < maxpin) {
					char *buf = pin_buf + strlen(pin_buf);
					char term = '\0';

					status = switch_ivr_collect_digits_count(session,
															 buf,
															 sizeof(pin_buf) - strlen(pin_buf), maxpin - strlen(pin_buf), "#", &term, 10000, 0, 0);
					if (status == SWITCH_STATUS_TIMEOUT) {
						status = SWITCH_STATUS_SUCCESS;
					}
				}

				if (status == SWITCH_STATUS_SUCCESS) {
					validate_pin(pin_buf, dpin, mdpin);
				}
				if (!pin_valid) {
					/* zero the collected pin */
					memset(pin_buf, 0, sizeof(pin_buf));

					/* more friendliness */
					if (conference->bad_pin_sound) {
						conference_local_play_file(conference, session, conference->bad_pin_sound, 20, NULL, 0);
					}
					switch_channel_flush_dtmf(channel);
				}
				pin_retries--;
			}

			if (!pin_valid) {
				goto done;
			}
		}

		if (conference->special_announce && !switch_channel_test_app_flag_key("conf_silent", channel, CONF_SILENT_REQ)) {
			conference_local_play_file(conference, session, conference->special_announce, CONF_DEFAULT_LEADIN, NULL, 0);
		}

		/* don't allow more callers if the conference is locked, unless we invited them */
		if (switch_test_flag(conference, CFLAG_LOCKED) && enforce_security) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Conference %s is locked.\n", conf_name);
			if (conference->locked_sound) {
				/* Answer the channel */
				switch_channel_answer(channel);
				conference_local_play_file(conference, session, conference->locked_sound, 20, NULL, 0);
			}
			goto done;
		}

		/* dont allow more callers than the max_members allows for -- I explicitly didnt allow outbound calls
		 * someone else can add that (see above) if they feel that outbound calls should be able to violate the
		 * max_members limit
		 */
		if ((conference->max_members > 0) && (conference->count >= conference->max_members)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Conference %s is full.\n", conf_name);
			if (conference->maxmember_sound) {
				/* Answer the channel */
				switch_channel_answer(channel);
				conference_local_play_file(conference, session, conference->maxmember_sound, 20, NULL, 0);
			}
			goto done;
		}

	}

	/* Release the config registry handle */
	if (cxml) {
		switch_xml_free(cxml);
		cxml = NULL;
	}

	/* if we're using "bridge:" make an outbound call and bridge it in */
	if (!zstr(bridgeto) && strcasecmp(bridgeto, "none")) {
		switch_call_cause_t cause;
		if (conference_outcall(conference, NULL, session, bridgeto, 60, NULL, NULL, NULL, NULL, &cause, NULL) != SWITCH_STATUS_SUCCESS) {
			goto done;
		}
	} else {
		/* if we're not using "bridge:" set the conference answered flag */
		/* and this isn't an outbound channel, answer the call */
		if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_INBOUND)
			switch_set_flag(conference, CFLAG_ANSWERED);
	}

	member.session = session;
	member.pool = switch_core_session_get_pool(session);

	if (setup_media(&member, conference)) {
		//flags = 0;
		goto done;
	}


	if (!(mid = switch_channel_get_private(channel, "__confmid"))) {
		mid = switch_core_session_alloc(session, sizeof(*mid));
		*mid = next_member_id();
		switch_channel_set_private(channel, "__confmid", mid);
	}

	switch_channel_set_variable_printf(channel, "conference_member_id", "%u", *mid);

	/* Prepare MUTEXS */
	member.id = *mid;
	switch_mutex_init(&member.flag_mutex, SWITCH_MUTEX_NESTED, member.pool);
	switch_mutex_init(&member.write_mutex, SWITCH_MUTEX_NESTED, member.pool);
	switch_mutex_init(&member.read_mutex, SWITCH_MUTEX_NESTED, member.pool);
	switch_mutex_init(&member.audio_in_mutex, SWITCH_MUTEX_NESTED, member.pool);
	switch_mutex_init(&member.audio_out_mutex, SWITCH_MUTEX_NESTED, member.pool);
	switch_thread_rwlock_create(&member.rwlock, member.pool);

	/* Install our Signed Linear codec so we get the audio in that format */
	switch_core_session_set_read_codec(member.session, &member.read_codec);


	mflags = conference->mflags;
	set_mflags(flags_str, &mflags);
	mflags |= MFLAG_RUNNING;
	if (mpin_matched) {
		mflags |= MFLAG_MOD;
	}
	switch_set_flag_locked((&member), mflags);

	if (mflags & MFLAG_MINTWO) {
		conference->min = 2;
	}

	/* Add the caller to the conference */
	if (conference_add_member(conference, &member) != SWITCH_STATUS_SUCCESS) {
		switch_core_codec_destroy(&member.read_codec);
		goto done;
	}

	msg.from = __FILE__;

	/* Tell the channel we are going to be in a bridge */
	msg.message_id = SWITCH_MESSAGE_INDICATE_BRIDGE;
	switch_core_session_receive_message(session, &msg);

	/* Run the conference loop */
	conference_loop_output(&member);
	switch_channel_set_private(channel, "_conference_autocall_list_", NULL);

	/* Tell the channel we are no longer going to be in a bridge */
	msg.message_id = SWITCH_MESSAGE_INDICATE_UNBRIDGE;
	switch_core_session_receive_message(session, &msg);

	/* Remove the caller from the conference */
	conference_del_member(member.conference, &member);

	/* Put the original codec back */
	switch_core_session_set_read_codec(member.session, NULL);

	/* Clean Up. */

  done:

	if (locked) {
		switch_mutex_unlock(globals.setup_mutex);
		locked = 0;
	}

	if (member.read_resampler) {
		switch_resample_destroy(&member.read_resampler);
	}

	switch_event_destroy(&params);
	switch_buffer_destroy(&member.resample_buffer);
	switch_buffer_destroy(&member.audio_buffer);
	switch_buffer_destroy(&member.mux_buffer);

	if (conference) {
		switch_mutex_lock(conference->mutex);
		if (switch_test_flag(conference, CFLAG_DYNAMIC) && conference->count == 0) {
			switch_set_flag_locked(conference, CFLAG_DESTRUCT);
		}
		switch_mutex_unlock(conference->mutex);
	}

	/* Release the config registry handle */
	if (cxml) {
		switch_xml_free(cxml);
	}

	if (conference && switch_test_flag(&member, MFLAG_KICKED) && conference->kicked_sound) {
		char *toplay = NULL;
		char *dfile = NULL;
		char *expanded = NULL;

		if (!strncasecmp(conference->kicked_sound, "say:", 4)) {
			if (conference->tts_engine && conference->tts_voice) {
				switch_ivr_speak_text(session, conference->tts_engine, conference->tts_voice, conference->kicked_sound + 4, NULL);
			}
		} else {
			if ((expanded = switch_channel_expand_variables(switch_core_session_get_channel(session), conference->kicked_sound)) != conference->kicked_sound) {
				toplay = expanded;
			} else {
				expanded = NULL;
				toplay = conference->kicked_sound; 
			}

			if (!switch_is_file_path(toplay) && conference->sound_prefix) {
				dfile = switch_mprintf("%s%s%s", conference->sound_prefix, SWITCH_PATH_SEPARATOR, toplay);
				switch_assert(dfile);
				toplay = dfile;
			}

			switch_ivr_play_file(session, NULL, toplay, NULL);
			switch_safe_free(dfile);
			switch_safe_free(expanded);
		}
	}

	switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);

	/* release the readlock */
	if (rl) {
		switch_thread_rwlock_unlock(conference->rwlock);
	}

	switch_channel_set_variable(channel, "last_transfered_conference", NULL);
}

/* Create a thread for the conference and launch it */
static void launch_conference_thread(conference_obj_t *conference)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	switch_set_flag_locked(conference, CFLAG_RUNNING);
	switch_threadattr_create(&thd_attr, conference->pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_mutex_lock(globals.hash_mutex);
	switch_mutex_unlock(globals.hash_mutex);
	switch_thread_create(&thread, thd_attr, conference_thread_run, conference, conference->pool);
}

static switch_thread_t *launch_thread_detached(switch_thread_start_t func, switch_memory_pool_t *pool, void *data)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&thread, thd_attr, func, data, pool);
	
	return thread;
}

/* Create a video thread for the conference and launch it */
static void launch_conference_video_thread(conference_obj_t *conference)
{
	launch_thread_detached(conference_video_thread_run, conference->pool, conference);
	conference->video_running = 1;
}

/* Create a video thread for the conference and launch it */
static void launch_conference_video_bridge_thread(conference_member_t *member_a, conference_member_t *member_b)
{
	switch_memory_pool_t *pool = member_a->conference->pool;
	struct vid_helper *vh = switch_core_alloc(pool, 2 * sizeof *vh);
	
	vh[0].member_a = member_a;
	vh[0].member_b = member_b;
	
	vh[1].member_a = member_b;
	vh[1].member_b = member_a;
	
	launch_thread_detached(conference_video_bridge_thread_run, pool, &vh[0]);
	launch_thread_detached(conference_video_bridge_thread_run, pool, &vh[1]);

	member_a->conference->video_running = 1;
}

static void launch_conference_record_thread(conference_obj_t *conference, char *path)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
	switch_memory_pool_t *pool;
	conference_record_t *rec;

	/* Setup a memory pool to use. */
	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Pool Failure\n");
	}

	/* Create a node object */
	if (!(rec = switch_core_alloc(pool, sizeof(*rec)))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Alloc Failure\n");
		switch_core_destroy_memory_pool(&pool);
		return;
	}

	rec->conference = conference;
	rec->path = switch_core_strdup(pool, path);
	rec->pool = pool;

	switch_threadattr_create(&thd_attr, rec->pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&thread, thd_attr, conference_record_thread_run, rec, rec->pool);
}

static switch_status_t chat_send(switch_event_t *message_event)
{
	char name[512] = "", *p, *lbuf = NULL;
	conference_obj_t *conference = NULL;
	switch_stream_handle_t stream = { 0 };
	const char *proto;
	const char *from; 
	const char *to;
	//const char *subject;
	const char *body;
	//const char *type;
	const char *hint;

	proto = switch_event_get_header(message_event, "proto");
	from = switch_event_get_header(message_event, "from");
	to = switch_event_get_header(message_event, "to");
	//subject = switch_event_get_header(message_event, "subject");
	body = switch_event_get_body(message_event);
	//type = switch_event_get_header(message_event, "type");
	hint = switch_event_get_header(message_event, "hint");

	if ((p = strchr(to, '+'))) {
		to = ++p;
	}

	if (!body) {
		return SWITCH_STATUS_SUCCESS;
	}

	if ((p = strchr(to, '@'))) {
		switch_copy_string(name, to, ++p - to);
	} else {
		switch_copy_string(name, to, sizeof(name));
	}

	if (!(conference = conference_find(name))) {
		switch_core_chat_send_args(proto, CONF_CHAT_PROTO, to, hint && strchr(hint, '/') ? hint : from, "", "Conference not active.", NULL, NULL);
		return SWITCH_STATUS_FALSE;
	}

	SWITCH_STANDARD_STREAM(stream);

	if (body != NULL && (lbuf = strdup(body))) {
		/* special case list */
		if (switch_stristr("list", lbuf)) {
			conference_list_pretty(conference, &stream);
			/* provide help */
		} else {
			return SWITCH_STATUS_SUCCESS;
		}
	}

	switch_safe_free(lbuf);

	switch_core_chat_send_args(proto, CONF_CHAT_PROTO, to, hint && strchr(hint, '/') ? hint : from, "", stream.data, NULL, NULL);
	switch_safe_free(stream.data);

	return SWITCH_STATUS_SUCCESS;
}

static conference_obj_t *conference_find(char *name)
{
	conference_obj_t *conference;

	switch_mutex_lock(globals.hash_mutex);
	if ((conference = switch_core_hash_find(globals.conference_hash, name))) {
		if (switch_test_flag(conference, CFLAG_DESTRUCT)) {
			switch_core_hash_delete(globals.conference_hash, conference->name);
			switch_clear_flag(conference, CFLAG_INHASH);
			conference = NULL;
		}
	}
	switch_mutex_unlock(globals.hash_mutex);

	return conference;
}

/* create a new conferene with a specific profile */
static conference_obj_t *conference_new(char *name, conf_xml_cfg_t cfg, switch_core_session_t *session, switch_memory_pool_t *pool)
{
	conference_obj_t *conference;
	switch_xml_t xml_kvp;
	char *timer_name = NULL;
	char *domain = NULL;
	char *tts_engine = NULL;
	char *tts_voice = NULL;
	char *enter_sound = NULL;
	char *sound_prefix = NULL;
	char *exit_sound = NULL;
	char *alone_sound = NULL;
	char *ack_sound = NULL;
	char *nack_sound = NULL;
	char *muted_sound = NULL;
	char *mute_detect_sound = NULL;
	char *unmuted_sound = NULL;
	char *locked_sound = NULL;
	char *is_locked_sound = NULL;
	char *is_unlocked_sound = NULL;
	char *kicked_sound = NULL;
	char *pin = NULL;
	char *mpin = NULL;
	char *pin_sound = NULL;
	char *bad_pin_sound = NULL;
	char *energy_level = NULL;
	char *auto_gain_level = NULL;
	char *caller_id_name = NULL;
	char *caller_id_number = NULL;
	char *caller_controls = NULL;
	char *moderator_controls = NULL;
	char *member_flags = NULL;
	char *conference_flags = NULL;
	char *perpetual_sound = NULL;
	char *moh_sound = NULL;
	uint32_t max_members = 0;
	uint32_t announce_count = 0;
	char *maxmember_sound = NULL;
	uint32_t rate = 8000, interval = 20;
	int comfort_noise_level = 0;
	int pin_retries = 3;
	int ivr_dtmf_timeout = 500;
	int ivr_input_timeout = 0;
	char *suppress_events = NULL;
	char *verbose_events = NULL;
	char *auto_record = NULL;
	char *terminate_on_silence = NULL;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH+1];
	switch_uuid_t uuid;
	switch_codec_implementation_t read_impl = { 0 };
	switch_channel_t *channel = NULL;
	const char *force_rate = NULL, *force_interval = NULL;
	uint32_t force_rate_i = 0, force_interval_i = 0;

	/* Validate the conference name */
	if (zstr(name)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid Record! no name.\n");
		return NULL;
	}

	if (session) {
		uint32_t tmp;

		switch_core_session_get_read_impl(session, &read_impl);
		channel = switch_core_session_get_channel(session);
		
		if ((force_rate = switch_channel_get_variable(channel, "conference_force_rate"))) {
			if (!strcasecmp(force_rate, "auto")) {
				force_rate_i = read_impl.actual_samples_per_second;
			} else {
				tmp = atoi(force_rate);

				if (tmp == 8000 || tmp == 12000 || tmp == 16000 || tmp == 24000 || tmp == 32000 || tmp == 48000) {
					force_rate_i = rate = tmp;
				}
			}
		}

		if ((force_interval = switch_channel_get_variable(channel, "conference_force_interval"))) {
			if (!strcasecmp(force_interval, "auto")) {
				force_interval_i = read_impl.microseconds_per_packet / 1000;
			} else {
				tmp = atoi(force_interval);
				
				if (SWITCH_ACCEPTABLE_INTERVAL(tmp)) {
					force_interval_i = interval = tmp;
				}
			}
		}
	}

	switch_mutex_lock(globals.hash_mutex);

	/* parse the profile tree for param values */
	if (cfg.profile)
		for (xml_kvp = switch_xml_child(cfg.profile, "param"); xml_kvp; xml_kvp = xml_kvp->next) {
			char *var = (char *) switch_xml_attr_soft(xml_kvp, "name");
			char *val = (char *) switch_xml_attr_soft(xml_kvp, "value");
			char buf[128] = "";
			char *p;

			if ((p = strchr(var, '_'))) {
				switch_copy_string(buf, var, sizeof(buf));
				for (p = buf; *p; p++) {
					if (*p == '_') {
						*p = '-';
					}
				}
				var = buf;
			}

			if (!force_rate_i && !strcasecmp(var, "rate") && !zstr(val)) {
				uint32_t tmp = atoi(val);
				if (session && tmp == 0) {
					if (!strcasecmp(val, "auto")) {
						rate = read_impl.actual_samples_per_second;
					}
				} else {
					if (tmp == 8000 || tmp == 12000 || tmp == 16000 || tmp == 24000 || tmp == 32000 || tmp == 48000) {
						rate = tmp;
					}
				}
			} else if (!strcasecmp(var, "domain") && !zstr(val)) {
				domain = val;
			} else if (!force_interval_i && !strcasecmp(var, "interval") && !zstr(val)) {
				uint32_t tmp = atoi(val);

				if (session && tmp == 0) {
					if (!strcasecmp(val, "auto")) {
						interval = read_impl.microseconds_per_packet / 1000;
					}
				} else {
					if (SWITCH_ACCEPTABLE_INTERVAL(tmp)) {
						interval = tmp;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
										  "Interval must be multipe of 10 and less than %d, Using default of 20\n", SWITCH_MAX_INTERVAL);
					}
				}
			} else if (!strcasecmp(var, "timer-name") && !zstr(val)) {
				timer_name = val;
			} else if (!strcasecmp(var, "tts-engine") && !zstr(val)) {
				tts_engine = val;
			} else if (!strcasecmp(var, "tts-voice") && !zstr(val)) {
				tts_voice = val;
			} else if (!strcasecmp(var, "enter-sound") && !zstr(val)) {
				enter_sound = val;
			} else if (!strcasecmp(var, "exit-sound") && !zstr(val)) {
				exit_sound = val;
			} else if (!strcasecmp(var, "alone-sound") && !zstr(val)) {
				alone_sound = val;
			} else if (!strcasecmp(var, "perpetual-sound") && !zstr(val)) {
				perpetual_sound = val;
			} else if (!strcasecmp(var, "moh-sound") && !zstr(val)) {
				moh_sound = val;
			} else if (!strcasecmp(var, "ack-sound") && !zstr(val)) {
				ack_sound = val;
			} else if (!strcasecmp(var, "nack-sound") && !zstr(val)) {
				nack_sound = val;
			} else if (!strcasecmp(var, "muted-sound") && !zstr(val)) {
				muted_sound = val;
			} else if (!strcasecmp(var, "mute-detect-sound") && !zstr(val)) {
				mute_detect_sound = val;
			} else if (!strcasecmp(var, "unmuted-sound") && !zstr(val)) {
				unmuted_sound = val;
			} else if (!strcasecmp(var, "locked-sound") && !zstr(val)) {
				locked_sound = val;
			} else if (!strcasecmp(var, "is-locked-sound") && !zstr(val)) {
				is_locked_sound = val;
			} else if (!strcasecmp(var, "is-unlocked-sound") && !zstr(val)) {
				is_unlocked_sound = val;
			} else if (!strcasecmp(var, "member-flags") && !zstr(val)) {
				member_flags = val;
			} else if (!strcasecmp(var, "conference-flags") && !zstr(val)) {
				conference_flags = val;
			} else if (!strcasecmp(var, "kicked-sound") && !zstr(val)) {
				kicked_sound = val;
			} else if (!strcasecmp(var, "pin") && !zstr(val)) {
				pin = val;
			} else if (!strcasecmp(var, "moderator-pin") && !zstr(val)) {
				mpin = val;
			} else if (!strcasecmp(var, "pin-retries") && !zstr(val)) {
				int tmp = atoi(val);
				if (tmp >= 0) {
					pin_retries = tmp;
				}
			} else if (!strcasecmp(var, "pin-sound") && !zstr(val)) {
				pin_sound = val;
			} else if (!strcasecmp(var, "bad-pin-sound") && !zstr(val)) {
				bad_pin_sound = val;
			} else if (!strcasecmp(var, "energy-level") && !zstr(val)) {
				energy_level = val;
			} else if (!strcasecmp(var, "auto-gain-level") && !zstr(val)) {
				auto_gain_level = val;
			} else if (!strcasecmp(var, "caller-id-name") && !zstr(val)) {
				caller_id_name = val;
			} else if (!strcasecmp(var, "caller-id-number") && !zstr(val)) {
				caller_id_number = val;
			} else if (!strcasecmp(var, "caller-controls") && !zstr(val)) {
				caller_controls = val;
                       } else if (!strcasecmp(var, "ivr-dtmf-timeout") && !zstr(val)) {
                               ivr_dtmf_timeout = atoi(val);
                               if (ivr_dtmf_timeout < 500) {
                                       switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "not very smart value for ivr-dtmf-timeout found (%d), defaulting to 500ms\n", ivr_dtmf_timeout);
                                       ivr_dtmf_timeout = 500;
                               }
                       } else if (!strcasecmp(var, "ivr-input-timeout") && !zstr(val)) {
                               ivr_input_timeout = atoi(val);
                               if (ivr_input_timeout != 0 && ivr_input_timeout < 500) {
                                       switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "not very smart value for ivr-input-timeout found (%d), defaulting to 500ms\n", ivr_input_timeout);
                                       ivr_input_timeout = 5000;
                               }
			} else if (!strcasecmp(var, "moderator-controls") && !zstr(val)) {
				moderator_controls = val;
			} else if (!strcasecmp(var, "comfort-noise") && !zstr(val)) {
				int tmp;
				tmp = atoi(val);
				if (tmp > 1 && tmp < 10000) {
					comfort_noise_level = tmp;
				} else if (switch_true(val)) {
					comfort_noise_level = 1400;
				}
			} else if (!strcasecmp(var, "sound-prefix") && !zstr(val)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "override sound-prefix with: %s\n", val);
				sound_prefix = val;
			} else if (!strcasecmp(var, "max-members") && !zstr(val)) {
				errno = 0;		/* sanity first */
				max_members = strtol(val, NULL, 0);	/* base 0 lets 0x... for hex 0... for octal and base 10 otherwise through */
				if (errno == ERANGE || errno == EINVAL || max_members < 0 || max_members == 1) {
					/* a negative wont work well, and its foolish to have a conference limited to 1 person unless the outbound 
					 * stuff is added, see comments above
					 */
					max_members = 0;	/* set to 0 to disable max counts */
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "max-members %s is invalid, not setting a limit\n", val);
				}
			} else if (!strcasecmp(var, "max-members-sound") && !zstr(val)) {
				maxmember_sound = val;
			} else if (!strcasecmp(var, "announce-count") && !zstr(val)) {
				errno = 0;		/* safety first */
				announce_count = strtol(val, NULL, 0);
				if (errno == ERANGE || errno == EINVAL) {
					announce_count = 0;
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "announce-count is invalid, not anouncing member counts\n");
				}
			} else if (!strcasecmp(var, "suppress-events") && !zstr(val)) {
				suppress_events = val;
			} else if (!strcasecmp(var, "verbose-events") && !zstr(val)) {
				verbose_events = val;
			} else if (!strcasecmp(var, "auto-record") && !zstr(val)) {
				auto_record = val;
			} else if (!strcasecmp(var, "terminate-on-silence") && !zstr(val)) {
				terminate_on_silence = val;
			}
		}

	/* Set defaults and various paramaters */

	/* Timer module to use */
	if (zstr(timer_name)) {
		timer_name = "soft";
	}

	/* Caller ID Name */
	if (zstr(caller_id_name)) {
		caller_id_name = (char *) global_app_name;
	}

	/* Caller ID Number */
	if (zstr(caller_id_number)) {
		caller_id_number = "0000000000";
	}

	if (!pool) {
		/* Setup a memory pool to use. */
		if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Pool Failure\n");
			conference = NULL;
			goto end;
		}
	}

	/* Create the conference object. */
	if (!(conference = switch_core_alloc(pool, sizeof(*conference)))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error!\n");
		conference = NULL;
		goto end;
	}

	/* initialize the conference object with settings from the specified profile */
	conference->pool = pool;
	conference->profile_name = switch_core_strdup(conference->pool, cfg.profile ? switch_xml_attr_soft(cfg.profile, "name") : "none");
	if (timer_name) {
		conference->timer_name = switch_core_strdup(conference->pool, timer_name);
	}
	if (tts_engine) {
		conference->tts_engine = switch_core_strdup(conference->pool, tts_engine);
	}
	if (tts_voice) {
		conference->tts_voice = switch_core_strdup(conference->pool, tts_voice);
	}

	conference->comfort_noise_level = comfort_noise_level;
	conference->pin_retries = pin_retries;
	conference->caller_id_name = switch_core_strdup(conference->pool, caller_id_name);
	conference->caller_id_number = switch_core_strdup(conference->pool, caller_id_number);
	conference->caller_controls = switch_core_strdup(conference->pool, caller_controls);
	conference->moderator_controls = switch_core_strdup(conference->pool, moderator_controls);
	conference->run_time = switch_epoch_time_now(NULL);
	

	if (!zstr(perpetual_sound)) {
		conference->perpetual_sound = switch_core_strdup(conference->pool, perpetual_sound);
	}

	conference->mflags = MFLAG_CAN_SPEAK | MFLAG_CAN_HEAR;

	if (!zstr(moh_sound) && switch_is_moh(moh_sound)) {
		conference->moh_sound = switch_core_strdup(conference->pool, moh_sound);
	}

	if (member_flags) {
		set_mflags(member_flags, &conference->mflags);
	}

	if (conference_flags) {
		set_cflags(conference_flags, &conference->flags);
	}

	if (!zstr(sound_prefix)) {
		conference->sound_prefix = switch_core_strdup(conference->pool, sound_prefix);
	} else {
		const char *val;
		if ((val = switch_channel_get_variable(channel, "sound_prefix")) && !zstr(val)) {
			/* if no sound_prefix was set, use the channel sound_prefix */
			conference->sound_prefix = switch_core_strdup(conference->pool, val);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "using channel sound prefix: %s\n", conference->sound_prefix);
		}
	}

	if (!zstr(enter_sound)) {
		conference->enter_sound = switch_core_strdup(conference->pool, enter_sound);
	}

	if (!zstr(exit_sound)) {
		conference->exit_sound = switch_core_strdup(conference->pool, exit_sound);
	}

	if (!zstr(ack_sound)) {
		conference->ack_sound = switch_core_strdup(conference->pool, ack_sound);
	}

	if (!zstr(nack_sound)) {
		conference->nack_sound = switch_core_strdup(conference->pool, nack_sound);
	}

	if (!zstr(muted_sound)) {
		conference->muted_sound = switch_core_strdup(conference->pool, muted_sound);
	}

	if (zstr(mute_detect_sound)) {
		if (!zstr(muted_sound)) {
			conference->mute_detect_sound = switch_core_strdup(conference->pool, muted_sound);
		}
	} else {
		conference->mute_detect_sound = switch_core_strdup(conference->pool, mute_detect_sound);
	}

	if (!zstr(unmuted_sound)) {
		conference->unmuted_sound = switch_core_strdup(conference->pool, unmuted_sound);
	}

	if (!zstr(kicked_sound)) {
		conference->kicked_sound = switch_core_strdup(conference->pool, kicked_sound);
	}

	if (!zstr(pin_sound)) {
		conference->pin_sound = switch_core_strdup(conference->pool, pin_sound);
	}

	if (!zstr(bad_pin_sound)) {
		conference->bad_pin_sound = switch_core_strdup(conference->pool, bad_pin_sound);
	}

	if (!zstr(pin)) {
		conference->pin = switch_core_strdup(conference->pool, pin);
	}

	if (!zstr(mpin)) {
		conference->mpin = switch_core_strdup(conference->pool, mpin);
	}

	if (!zstr(alone_sound)) {
		conference->alone_sound = switch_core_strdup(conference->pool, alone_sound);
	}

	if (!zstr(locked_sound)) {
		conference->locked_sound = switch_core_strdup(conference->pool, locked_sound);
	}

	if (!zstr(is_locked_sound)) {
		conference->is_locked_sound = switch_core_strdup(conference->pool, is_locked_sound);
	}

	if (!zstr(is_unlocked_sound)) {
		conference->is_unlocked_sound = switch_core_strdup(conference->pool, is_unlocked_sound);
	}

	if (!zstr(energy_level)) {
		conference->energy_level = atoi(energy_level);
		if (conference->energy_level < 0) {
			conference->energy_level = 0;
		}
	}

	if (!zstr(auto_gain_level)) {
		int level = 0;

		if (switch_true(auto_gain_level)) {
			level = DEFAULT_AGC_LEVEL;
		} else {
			level = atoi(auto_gain_level);
		}

		if (level > 0 && level > conference->energy_level) {
			conference->agc_level = level;
		}
	}

	if (!zstr(maxmember_sound)) {
		conference->maxmember_sound = switch_core_strdup(conference->pool, maxmember_sound);
	}
	/* its going to be 0 by default, set to a value otherwise so this should be safe */
	conference->max_members = max_members;
	conference->announce_count = announce_count;

	conference->name = switch_core_strdup(conference->pool, name);
	if (domain) {
		conference->domain = switch_core_strdup(conference->pool, domain);
	} else {
		conference->domain = "cluecon.com";
	}

	conference->rate = rate;
	conference->interval = interval;
	conference->ivr_dtmf_timeout = ivr_dtmf_timeout;
	conference->ivr_input_timeout = ivr_input_timeout;

	conference->eflags = 0xFFFFFFFF;
	if (!zstr(suppress_events)) {
		clear_eflags(suppress_events, &conference->eflags);
	}

	if (!zstr(auto_record)) {
		conference->auto_record = switch_core_strdup(conference->pool, auto_record);
	}
        if (!zstr(terminate_on_silence)) {
                conference->terminate_on_silence = atoi(terminate_on_silence);
        }

	if (!zstr(verbose_events) && switch_true(verbose_events)) {
		conference->verbose_events = 1;
	}

	/* Create the conference unique identifier */
	switch_uuid_get(&uuid);
	switch_uuid_format(uuid_str, &uuid);
	conference->uuid_str = switch_core_strdup(conference->pool, uuid_str);

	/* Set enter sound and exit sound flags so that default is on */
	switch_set_flag(conference, CFLAG_ENTER_SOUND);
	switch_set_flag(conference, CFLAG_EXIT_SOUND);
	
	/* Activate the conference mutex for exclusivity */
	switch_mutex_init(&conference->mutex, SWITCH_MUTEX_NESTED, conference->pool);
	switch_mutex_init(&conference->flag_mutex, SWITCH_MUTEX_NESTED, conference->pool);
	switch_thread_rwlock_create(&conference->rwlock, conference->pool);
	switch_mutex_init(&conference->member_mutex, SWITCH_MUTEX_NESTED, conference->pool);

	switch_mutex_lock(globals.hash_mutex);
	switch_set_flag(conference, CFLAG_INHASH);
	switch_core_hash_insert(globals.conference_hash, conference->name, conference);
	switch_mutex_unlock(globals.hash_mutex);

  end:

	switch_mutex_unlock(globals.hash_mutex);

	return conference;
}

static void pres_event_handler(switch_event_t *event)
{
	char *to = switch_event_get_header(event, "to");
	char *dup_to = NULL, *conf_name, *e;
	conference_obj_t *conference;

	if (!to || strncasecmp(to, "conf+", 5)) {
		return;
	}

	if (!(dup_to = strdup(to))) {
		return;
	}

	conf_name = dup_to + 5;

	if ((e = strchr(conf_name, '@'))) {
		*e = '\0';
	}

	if ((conference = conference_find(conf_name))) {
		if (switch_event_create(&event, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", CONF_CHAT_PROTO);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", conference->name);
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "from", "%s@%s", conference->name, conference->domain);
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "status", "Active (%d caller%s)", conference->count, conference->count == 1 ? "" : "s");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "event_count", "%d", EC++);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique-id", conf_name);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "channel-state", "CS_ROUTING");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "answer-state", conference->count == 1 ? "early" : "confirmed");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-direction", conference->count == 1 ? "outbound" : "inbound");
			switch_event_fire(&event);
		}
	} else if (switch_event_create(&event, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", CONF_CHAT_PROTO);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", conf_name);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "from", to);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "status", "Idle");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "rpid", "idle");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "event_count", "%d", EC++);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique-id", conf_name);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "channel-state", "CS_HANGUP");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "answer-state", "terminated");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-direction", "inbound");
		switch_event_fire(&event);
	}

	switch_safe_free(dup_to);
}

static void send_presence(switch_event_types_t id)
{
	switch_xml_t cxml, cfg, advertise, room;
	switch_event_t *params = NULL;

	switch_event_create(&params, SWITCH_EVENT_COMMAND);
	switch_assert(params);
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "presence", "true");


	/* Open the config from the xml registry */
	if (!(cxml = switch_xml_open_cfg(global_cf_name, &cfg, params))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", global_cf_name);
		goto done;
	}

	if ((advertise = switch_xml_child(cfg, "advertise"))) {
		for (room = switch_xml_child(advertise, "room"); room; room = room->next) {
			char *name = (char *) switch_xml_attr_soft(room, "name");
			char *status = (char *) switch_xml_attr_soft(room, "status");
			switch_event_t *event;

			if (name && switch_event_create(&event, id) == SWITCH_STATUS_SUCCESS) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", CONF_CHAT_PROTO);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", name);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "from", name);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "status", status ? status : "Available");
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "rpid", "idle");
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
				switch_event_fire(&event);
			}
		}
	}

  done:
	switch_event_destroy(&params);

	/* Release the config registry handle */
	if (cxml) {
		switch_xml_free(cxml);
		cxml = NULL;
	}
}

typedef void (*conf_key_callback_t) (conference_member_t *, struct caller_control_actions *);

typedef struct {
	conference_member_t *member;
	caller_control_action_t action;
	conf_key_callback_t handler;
} key_binding_t;


static switch_status_t dmachine_dispatcher(switch_ivr_dmachine_match_t *match)
{
	key_binding_t *binding = match->user_data;
	switch_channel_t *channel;

	if (!binding) return SWITCH_STATUS_FALSE;

	channel = switch_core_session_get_channel(binding->member->session);
	switch_channel_set_variable(channel, "conference_last_matching_digits", match->match_digits);

	if (binding->action.data) {
		binding->action.expanded_data = switch_channel_expand_variables(channel, binding->action.data);
	}

	binding->handler(binding->member, &binding->action);

	if (binding->action.expanded_data != binding->action.data) {
		free(binding->action.expanded_data);
		binding->action.expanded_data = NULL;
	}

	switch_set_flag_locked(binding->member, MFLAG_FLUSH_BUFFER);

	return SWITCH_STATUS_SUCCESS;
}

static void do_binding(conference_member_t *member, conf_key_callback_t handler, const char *digits, const char *data)
{
	key_binding_t *binding;

	binding = switch_core_alloc(member->pool, sizeof(*binding));
	binding->member = member;

	binding->action.binded_dtmf = switch_core_strdup(member->pool, digits);

	if (data) {
		binding->action.data = switch_core_strdup(member->pool, data);
	}

	binding->handler = handler;
	switch_ivr_dmachine_bind(member->dmachine, "conf", digits, 0, dmachine_dispatcher, binding);
	
}

struct _mapping {
	const char *name;
	conf_key_callback_t handler;
};

static struct _mapping control_mappings[] = {
    {"mute", conference_loop_fn_mute_toggle},
    {"mute on", conference_loop_fn_mute_on},
    {"mute off", conference_loop_fn_mute_off},
    {"deaf mute", conference_loop_fn_deafmute_toggle},
    {"energy up", conference_loop_fn_energy_up},
    {"energy equ", conference_loop_fn_energy_equ_conf},
    {"energy dn", conference_loop_fn_energy_dn},
    {"vol talk up", conference_loop_fn_volume_talk_up},
    {"vol talk zero", conference_loop_fn_volume_talk_zero},
    {"vol talk dn", conference_loop_fn_volume_talk_dn},
    {"vol listen up", conference_loop_fn_volume_listen_up},
    {"vol listen zero", conference_loop_fn_volume_listen_zero},
    {"vol listen dn", conference_loop_fn_volume_listen_dn},
    {"hangup", conference_loop_fn_hangup},
    {"event", conference_loop_fn_event},
    {"lock", conference_loop_fn_lock_toggle},
    {"transfer", conference_loop_fn_transfer},
    {"execute_application", conference_loop_fn_exec_app}
};
#define MAPPING_LEN (sizeof(control_mappings)/sizeof(control_mappings[0]))

static void member_bind_controls(conference_member_t *member, const char *controls)
{
	switch_xml_t cxml, cfg, xgroups, xcontrol;
	switch_event_t *params;
	int i;

	switch_event_create(&params, SWITCH_EVENT_REQUEST_PARAMS);
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "Conf-Name", member->conference->name);
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "Action", "request-controls");
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "Controls", controls);

	if (!(cxml = switch_xml_open_cfg(global_cf_name, &cfg, params))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", global_cf_name);
		goto end;
	}

	if (!(xgroups = switch_xml_child(cfg, "caller-controls"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't find caller-controls in %s\n", global_cf_name);
		goto end;
	}

	if (!(xgroups = switch_xml_find_child(xgroups, "group", "name", controls))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't find caller-controls in %s\n", global_cf_name);
		goto end;
	}


	for (xcontrol = switch_xml_child(xgroups, "control"); xcontrol; xcontrol = xcontrol->next) {
        const char *key = switch_xml_attr(xcontrol, "action");
        const char *digits = switch_xml_attr(xcontrol, "digits");
        const char *data = switch_xml_attr_soft(xcontrol, "data");

		if (zstr(key) || zstr(digits)) continue;

		for(i = 0; i < MAPPING_LEN; i++) {
			if (!strcasecmp(key, control_mappings[i].name)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s binding '%s' to '%s'\n", 
								  switch_core_session_get_name(member->session), digits, key);

				do_binding(member, control_mappings[i].handler, digits, data);
			}
		}
	}

 end:

	/* Release the config registry handle */
	if (cxml) {
		switch_xml_free(cxml);
		cxml = NULL;
	}
	
	if (params) switch_event_destroy(&params);
	
}




/* Called by FreeSWITCH when the module loads */
SWITCH_MODULE_LOAD_FUNCTION(mod_conference_load)
{
	uint32_t i;
	size_t nl, ol = 0;
	char *p = NULL, *tmp = NULL;
	switch_chat_interface_t *chat_interface;
	switch_api_interface_t *api_interface;
	switch_application_interface_t *app_interface;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	char cmd_str[256];

	memset(&globals, 0, sizeof(globals));

	/* Connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_console_add_complete_func("::conference::list_conferences", list_conferences);
	

	/* build api interface help ".syntax" field string */
	p = strdup("");
	for (i = 0; i < CONFFUNCAPISIZE; i++) {
		nl = strlen(conf_api_sub_commands[i].pcommand) + strlen(conf_api_sub_commands[i].psyntax) + 5;

		switch_snprintf(cmd_str, sizeof(cmd_str), "add conference ::conference::list_conferences %s", conf_api_sub_commands[i].pcommand);

		switch_console_set_complete(cmd_str);

		if (p != NULL) {
			ol = strlen(p);
		}
		tmp = realloc(p, ol + nl);
		if (tmp != NULL) {
			p = tmp;
			strcat(p, "\t\t");
			strcat(p, conf_api_sub_commands[i].pcommand);
			if (!zstr(conf_api_sub_commands[i].psyntax)) {
				strcat(p, " ");
				strcat(p, conf_api_sub_commands[i].psyntax);
			}
			if (i < CONFFUNCAPISIZE - 1) {
				strcat(p, "\n");
			}
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't realloc\n");
			return SWITCH_STATUS_TERM;
		}

	}
	api_syntax = p;

	/* create/register custom event message type */
	if (switch_event_reserve_subclass(CONF_EVENT_MAINT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", CONF_EVENT_MAINT);
		return SWITCH_STATUS_TERM;
	}

	/* Setup the pool */
	globals.conference_pool = pool;

	/* Setup a hash to store conferences by name */
	switch_core_hash_init(&globals.conference_hash, globals.conference_pool);
	switch_mutex_init(&globals.conference_mutex, SWITCH_MUTEX_NESTED, globals.conference_pool);
	switch_mutex_init(&globals.id_mutex, SWITCH_MUTEX_NESTED, globals.conference_pool);
	switch_mutex_init(&globals.hash_mutex, SWITCH_MUTEX_NESTED, globals.conference_pool);
	switch_mutex_init(&globals.setup_mutex, SWITCH_MUTEX_NESTED, globals.conference_pool);

	/* Subscribe to presence request events */
	if (switch_event_bind_removable(modname, SWITCH_EVENT_PRESENCE_PROBE, SWITCH_EVENT_SUBCLASS_ANY, pres_event_handler, NULL, &globals.node) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't subscribe to presence request events!\n");
		return SWITCH_STATUS_GENERR;
	}

	SWITCH_ADD_API(api_interface, "conference", "Conference module commands", conf_api_main, p);
	SWITCH_ADD_APP(app_interface, global_app_name, global_app_name, NULL, conference_function, NULL, SAF_NONE);
	SWITCH_ADD_APP(app_interface, "conference_set_auto_outcall", "conference_set_auto_outcall", NULL, conference_auto_function, NULL, SAF_NONE);
	SWITCH_ADD_CHAT(chat_interface, CONF_CHAT_PROTO, chat_send);
	

	send_presence(SWITCH_EVENT_PRESENCE_IN);

	globals.running = 1;
	/* indicate that the module should continue to be loaded */
	return status;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_conference_shutdown)
{
	if (globals.running) {

		/* signal all threads to shutdown */
		globals.running = 0;

		switch_console_del_complete_func("::conference::list_conferences");

		/* wait for all threads */
		while (globals.threads) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Waiting for %d threads\n", globals.threads);
			switch_yield(100000);
		}

		switch_event_unbind(&globals.node);
		switch_event_free_subclass(CONF_EVENT_MAINT);

		/* free api interface help ".syntax" field string */
		switch_safe_free(api_syntax);
	}
	switch_core_hash_destroy(&globals.conference_hash);

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
