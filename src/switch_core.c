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
 * Paul D. Tinsley <pdt at jackhammer.org>
 * Marcel Barbulescu <marcelbarbulescu@gmail.com>
 *
 *
 * switch_core.c -- Main Core Library
 *
 */



#include <switch.h>
#include <switch_ssl.h>
#include <switch_stun.h>
#include <switch_nat.h>
#include <switch_version.h>
#include "private/switch_core_pvt.h"
#ifndef WIN32
#include <switch_private.h>
#ifdef HAVE_SETRLIMIT
#include <sys/resource.h>
#endif
#endif
#include <errno.h>


SWITCH_DECLARE_DATA switch_directories SWITCH_GLOBAL_dirs = { 0 };

/* The main runtime obj we keep this hidden for ourselves */
struct switch_runtime runtime = { 0 };
static void switch_load_core_config(const char *file);

static void send_heartbeat(void)
{
	switch_event_t *event;
	switch_core_time_duration_t duration;

	switch_core_measure_time(switch_core_uptime(), &duration);

	if (switch_event_create(&event, SWITCH_EVENT_HEARTBEAT) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Event-Info", "System Ready");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Up-Time",
								"%u year%s, "
								"%u day%s, "
								"%u hour%s, "
								"%u minute%s, "
								"%u second%s, "
								"%u millisecond%s, "
								"%u microsecond%s",
								duration.yr, duration.yr == 1 ? "" : "s",
								duration.day, duration.day == 1 ? "" : "s",
								duration.hr, duration.hr == 1 ? "" : "s",
								duration.min, duration.min == 1 ? "" : "s",
								duration.sec, duration.sec == 1 ? "" : "s",
								duration.ms, duration.ms == 1 ? "" : "s", duration.mms, duration.mms == 1 ? "" : "s");

		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Session-Count", "%u", switch_core_session_count());
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Max-Sessions", "%u", switch_core_session_limit(0));
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Session-Per-Sec", "%u", runtime.sps);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Session-Since-Startup", "%" SWITCH_SIZE_T_FMT, switch_core_session_id() - 1);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Idle-CPU", "%f", switch_core_idle_cpu());
		switch_event_fire(&event);
	}
}

static char main_ip4[256] = "";
static char main_ip6[256] = "";

static void check_ip(void)
{
	char guess_ip4[256] = "";
	char guess_ip6[256] = "";
	char old_ip4[256] = "";
	char old_ip6[256] = "";
	int ok4 = 1, ok6 = 1;
	int mask = 0;

	gethostname(runtime.hostname, sizeof(runtime.hostname));
	switch_core_set_variable("hostname", runtime.hostname);

	switch_find_local_ip(guess_ip4, sizeof(guess_ip4), &mask, AF_INET);
	switch_find_local_ip(guess_ip6, sizeof(guess_ip6), NULL, AF_INET6);

	if (!*main_ip4) {
		switch_set_string(main_ip4, guess_ip4);
	} else {
		if (!(ok4 = !strcmp(main_ip4, guess_ip4))) {
			struct in_addr in;

			in.s_addr = mask;
			switch_set_string(old_ip4, main_ip4);
			switch_set_string(main_ip4, guess_ip4);
			switch_core_set_variable("local_ip_v4", guess_ip4);
			switch_core_set_variable("local_mask_v4", inet_ntoa(in));
		}
	}

	if (!*main_ip6) {
		switch_set_string(main_ip6, guess_ip6);
	} else {
		if (!(ok6 = !strcmp(main_ip6, guess_ip6))) {
			switch_set_string(old_ip6, main_ip6);
			switch_set_string(main_ip6, guess_ip6);
			switch_core_set_variable("local_ip_v6", guess_ip6);
		}
	}

	if (!ok4 || !ok6) {
		switch_event_t *event;

		if (switch_event_create(&event, SWITCH_EVENT_TRAP) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "condition", "network-address-change");
			if (!ok4) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "network-address-previous-v4", old_ip4);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "network-address-change-v4", main_ip4);
			}
			if (!ok6) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "network-address-previous-v6", old_ip6);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "network-address-change-v6", main_ip6);
			}
			switch_event_fire(&event);
		}
	}
}

SWITCH_STANDARD_SCHED_FUNC(heartbeat_callback)
{
	send_heartbeat();

	/* reschedule this task */
	task->runtime = switch_epoch_time_now(NULL) + 20;
}


SWITCH_STANDARD_SCHED_FUNC(check_ip_callback)
{
	check_ip();

	/* reschedule this task */
	task->runtime = switch_epoch_time_now(NULL) + 60;
}


SWITCH_DECLARE(switch_status_t) switch_core_set_console(const char *console)
{
	if ((runtime.console = fopen(console, "a")) == 0) {
		fprintf(stderr, "Cannot open output file %s.\n", console);
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(FILE *) switch_core_get_console(void)
{
	return runtime.console;
}

SWITCH_DECLARE(FILE *) switch_core_data_channel(switch_text_channel_t channel)
{
	FILE *handle = stdout;

	switch (channel) {
	case SWITCH_CHANNEL_ID_LOG:
	case SWITCH_CHANNEL_ID_LOG_CLEAN:
		handle = runtime.console;
		break;
	default:
		handle = runtime.console;
		break;
	}

	return handle;
}


SWITCH_DECLARE(int) switch_core_curl_count(int *val)
{
	if (!val) {
		switch_mutex_lock(runtime.global_mutex);
		return runtime.curl_count;
	}

	runtime.curl_count = *val;
	switch_mutex_unlock(runtime.global_mutex);
	return 0;

}


SWITCH_DECLARE(int) switch_core_ssl_count(int *val)
{
	if (!val) {
		switch_mutex_lock(runtime.global_mutex);
		return runtime.ssl_count;
	}

	runtime.ssl_count = *val;
	switch_mutex_unlock(runtime.global_mutex);
	return 0;

}

SWITCH_DECLARE(void) switch_core_remove_state_handler(const switch_state_handler_table_t *state_handler)
{
	int index, tmp_index = 0;
	const switch_state_handler_table_t *tmp[SWITCH_MAX_STATE_HANDLERS + 1] = { 0 };

	switch_mutex_lock(runtime.global_mutex);

	for (index = 0; index < runtime.state_handler_index; index++) {
		const switch_state_handler_table_t *cur = runtime.state_handlers[index];
		runtime.state_handlers[index] = NULL;
		if (cur == state_handler) {
			continue;
		}
		tmp[tmp_index++] = cur;
	}

	runtime.state_handler_index = 0;

	for (index = 0; index < tmp_index; index++) {
		runtime.state_handlers[runtime.state_handler_index++] = tmp[index];
	}
	switch_mutex_unlock(runtime.global_mutex);
}


SWITCH_DECLARE(int) switch_core_add_state_handler(const switch_state_handler_table_t *state_handler)
{
	int index;

	switch_mutex_lock(runtime.global_mutex);
	index = runtime.state_handler_index++;

	if (runtime.state_handler_index >= SWITCH_MAX_STATE_HANDLERS) {
		index = -1;
	} else {
		runtime.state_handlers[index] = state_handler;
	}

	switch_mutex_unlock(runtime.global_mutex);
	return index;
}

SWITCH_DECLARE(const switch_state_handler_table_t *) switch_core_get_state_handler(int index)
{

	if (index >= SWITCH_MAX_STATE_HANDLERS || index > runtime.state_handler_index) {
		return NULL;
	}

	return runtime.state_handlers[index];
}

SWITCH_DECLARE(void) switch_core_dump_variables(switch_stream_handle_t *stream)
{
	switch_event_header_t *hi;

	switch_mutex_lock(runtime.global_mutex);
	for (hi = runtime.global_vars->headers; hi; hi = hi->next) {
		stream->write_function(stream, "%s=%s\n", hi->name, hi->value);
	}
	switch_mutex_unlock(runtime.global_mutex);
}

SWITCH_DECLARE(const char *) switch_core_get_hostname(void)
{
	return runtime.hostname;
}

SWITCH_DECLARE(const char *) switch_core_get_switchname(void)
{
    if (!zstr(runtime.switchname)) return runtime.switchname;
	return runtime.hostname;
}


SWITCH_DECLARE(char *) switch_core_get_variable(const char *varname)
{
	char *val;
	switch_thread_rwlock_rdlock(runtime.global_var_rwlock);
	val = (char *) switch_event_get_header(runtime.global_vars, varname);
	switch_thread_rwlock_unlock(runtime.global_var_rwlock);
	return val;
}

SWITCH_DECLARE(char *) switch_core_get_variable_dup(const char *varname)
{
	char *val = NULL, *v;

	switch_thread_rwlock_rdlock(runtime.global_var_rwlock);
	if ((v = (char *) switch_event_get_header(runtime.global_vars, varname))) {
		val = strdup(v);
	}
	switch_thread_rwlock_unlock(runtime.global_var_rwlock);

	return val;
}

SWITCH_DECLARE(char *) switch_core_get_variable_pdup(const char *varname, switch_memory_pool_t *pool)
{
	char *val = NULL, *v;

	switch_thread_rwlock_rdlock(runtime.global_var_rwlock);
	if ((v = (char *) switch_event_get_header(runtime.global_vars, varname))) {
		val = switch_core_strdup(pool, v);
	}
	switch_thread_rwlock_unlock(runtime.global_var_rwlock);

	return val;
}

static void switch_core_unset_variables(void)
{
	switch_thread_rwlock_wrlock(runtime.global_var_rwlock);
	switch_event_destroy(&runtime.global_vars);
	switch_event_create_plain(&runtime.global_vars, SWITCH_EVENT_CHANNEL_DATA);
	switch_thread_rwlock_unlock(runtime.global_var_rwlock);
}

SWITCH_DECLARE(void) switch_core_set_variable(const char *varname, const char *value)
{
	char *val;

	if (varname) {
		switch_thread_rwlock_wrlock(runtime.global_var_rwlock);
		val = (char *) switch_event_get_header(runtime.global_vars, varname);
		if (val) {
			switch_event_del_header(runtime.global_vars, varname);
		}
		if (value) {
			char *v = strdup(value);
			switch_string_var_check(v, SWITCH_TRUE);
			switch_event_add_header_string(runtime.global_vars, SWITCH_STACK_BOTTOM | SWITCH_STACK_NODUP, varname, v);
		} else {
			switch_event_del_header(runtime.global_vars, varname);
		}
		switch_thread_rwlock_unlock(runtime.global_var_rwlock);
	}
}

SWITCH_DECLARE(switch_bool_t) switch_core_set_var_conditional(const char *varname, const char *value, const char *val2)
{
	char *val;

	if (varname) {
		switch_thread_rwlock_wrlock(runtime.global_var_rwlock);
		val = (char *) switch_event_get_header(runtime.global_vars, varname);

		if (val) {
			if (!val2 || strcmp(val, val2) != 0) {
				switch_thread_rwlock_unlock(runtime.global_var_rwlock);
				return SWITCH_FALSE;
			}
			switch_event_del_header(runtime.global_vars, varname);
		} else if (!zstr(val2)) {
			switch_thread_rwlock_unlock(runtime.global_var_rwlock);
			return SWITCH_FALSE;
		}

		if (value) {
			char *v = strdup(value);
			switch_string_var_check(v, SWITCH_TRUE);
			switch_event_add_header_string(runtime.global_vars, SWITCH_STACK_BOTTOM | SWITCH_STACK_NODUP, varname, v);
		} else {
			switch_event_del_header(runtime.global_vars, varname);
		}
		switch_thread_rwlock_unlock(runtime.global_var_rwlock);
	}
	return SWITCH_TRUE;
}

SWITCH_DECLARE(char *) switch_core_get_uuid(void)
{
	return runtime.uuid_str;
}


static void *SWITCH_THREAD_FUNC switch_core_service_thread(switch_thread_t *thread, void *obj)
{
	switch_core_session_t *session = obj;
	switch_channel_t *channel;
	switch_frame_t *read_frame;

//  switch_assert(thread != NULL);
//  switch_assert(session != NULL);

	if (switch_core_session_read_lock(session) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}

	switch_mutex_lock(session->frame_read_mutex);

	channel = switch_core_session_get_channel(session);

	switch_channel_set_flag(channel, CF_SERVICE);
	while (switch_channel_test_flag(channel, CF_SERVICE)) {
		switch (switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0)) {
		case SWITCH_STATUS_SUCCESS:
		case SWITCH_STATUS_TIMEOUT:
		case SWITCH_STATUS_BREAK:
			break;
		default:
			switch_channel_clear_flag(channel, CF_SERVICE);
			continue;
		}
	}

	switch_mutex_unlock(session->frame_read_mutex);

	switch_core_session_rwunlock(session);

	return NULL;
}

/* Either add a timeout here or make damn sure the thread cannot get hung somehow (my preference) */
SWITCH_DECLARE(void) switch_core_thread_session_end(switch_core_session_t *session)
{
	switch_channel_t *channel;
	switch_assert(session);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	switch_channel_clear_flag(channel, CF_SERVICE);
}

SWITCH_DECLARE(void) switch_core_service_session(switch_core_session_t *session)
{
	switch_channel_t *channel;
	switch_assert(session);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	switch_core_session_launch_thread(session, (void *(*)(switch_thread_t *,void *))switch_core_service_thread, session);
}

/* This function abstracts the thread creation for modules by allowing you to pass a function ptr and
   a void object and trust that that the function will be run in a thread with arg  This lets
   you request and activate a thread without giving up any knowledge about what is in the thread
   neither the core nor the calling module know anything about each other.

   This thread is expected to never exit until the application exits so the func is responsible
   to make sure that is the case.

   The typical use for this is so switch_loadable_module.c can start up a thread for each module
   passing the table of module methods as a session obj into the core without actually allowing
   the core to have any clue and keeping switch_loadable_module.c from needing any thread code.

*/

SWITCH_DECLARE(switch_thread_t *) switch_core_launch_thread(switch_thread_start_t func, void *obj, switch_memory_pool_t *pool)
{
	switch_thread_t *thread = NULL;
	switch_threadattr_t *thd_attr = NULL;
	switch_core_thread_session_t *ts;
	int mypool;

	mypool = pool ? 0 : 1;

	if (!pool && switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not allocate memory pool\n");
		return NULL;
	}

	switch_threadattr_create(&thd_attr, pool);

	if ((ts = switch_core_alloc(pool, sizeof(*ts))) == 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not allocate memory\n");
	} else {
		if (mypool) {
			ts->pool = pool;
		}
		ts->objs[0] = obj;
		ts->objs[1] = thread;
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		switch_threadattr_priority_increase(thd_attr);
		switch_thread_create(&thread, thd_attr, func, ts, pool);
	}

	return thread;
}

SWITCH_DECLARE(void) switch_core_set_globals(void)
{
#define BUFSIZE 1024
#ifdef WIN32
	char lpPathBuffer[BUFSIZE];
	DWORD dwBufSize = BUFSIZE;
	char base_dir[1024];
	char *lastbacklash;

	GetModuleFileName(NULL, base_dir, BUFSIZE);
	lastbacklash = strrchr(base_dir, '\\');
	base_dir[(lastbacklash - base_dir)] = '\0';
	/* set base_dir as cwd, to be able to use relative paths in scripting languages (e.g. mod_lua) when FS is running as a service or while debugging FS using visual studio */
	SetCurrentDirectory(base_dir);

#else
	char base_dir[1024] = SWITCH_PREFIX_DIR;
#endif

	if (!SWITCH_GLOBAL_dirs.base_dir && (SWITCH_GLOBAL_dirs.base_dir = (char *) malloc(BUFSIZE))) {
		switch_snprintf(SWITCH_GLOBAL_dirs.base_dir, BUFSIZE, "%s", base_dir);
	}

	if (!SWITCH_GLOBAL_dirs.mod_dir && (SWITCH_GLOBAL_dirs.mod_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_MOD_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.mod_dir, BUFSIZE, "%s", SWITCH_MOD_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.mod_dir, BUFSIZE, "%s%smod", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.conf_dir && (SWITCH_GLOBAL_dirs.conf_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_CONF_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.conf_dir, BUFSIZE, "%s", SWITCH_CONF_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.conf_dir, BUFSIZE, "%s%sconf", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.log_dir && (SWITCH_GLOBAL_dirs.log_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_LOG_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.log_dir, BUFSIZE, "%s", SWITCH_LOG_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.log_dir, BUFSIZE, "%s%slog", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.run_dir && (SWITCH_GLOBAL_dirs.run_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_RUN_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.run_dir, BUFSIZE, "%s", SWITCH_RUN_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.run_dir, BUFSIZE, "%s%srun", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.recordings_dir && (SWITCH_GLOBAL_dirs.recordings_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_RECORDINGS_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.recordings_dir, BUFSIZE, "%s", SWITCH_RECORDINGS_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.recordings_dir, BUFSIZE, "%s%srecordings", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.sounds_dir && (SWITCH_GLOBAL_dirs.sounds_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_SOUNDS_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.sounds_dir, BUFSIZE, "%s", SWITCH_SOUNDS_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.sounds_dir, BUFSIZE, "%s%ssounds", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.storage_dir && (SWITCH_GLOBAL_dirs.storage_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_STORAGE_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.storage_dir, BUFSIZE, "%s", SWITCH_STORAGE_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.storage_dir, BUFSIZE, "%s%sstorage", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.db_dir && (SWITCH_GLOBAL_dirs.db_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_DB_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.db_dir, BUFSIZE, "%s", SWITCH_DB_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.db_dir, BUFSIZE, "%s%sdb", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.script_dir && (SWITCH_GLOBAL_dirs.script_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_SCRIPT_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.script_dir, BUFSIZE, "%s", SWITCH_SCRIPT_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.script_dir, BUFSIZE, "%s%sscripts", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.htdocs_dir && (SWITCH_GLOBAL_dirs.htdocs_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_HTDOCS_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.htdocs_dir, BUFSIZE, "%s", SWITCH_HTDOCS_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.htdocs_dir, BUFSIZE, "%s%shtdocs", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.grammar_dir && (SWITCH_GLOBAL_dirs.grammar_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_GRAMMAR_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.grammar_dir, BUFSIZE, "%s", SWITCH_GRAMMAR_DIR);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.grammar_dir, BUFSIZE, "%s%sgrammar", base_dir, SWITCH_PATH_SEPARATOR);
#endif
	}

	if (!SWITCH_GLOBAL_dirs.temp_dir && (SWITCH_GLOBAL_dirs.temp_dir = (char *) malloc(BUFSIZE))) {
#ifdef SWITCH_TEMP_DIR
		switch_snprintf(SWITCH_GLOBAL_dirs.temp_dir, BUFSIZE, "%s", SWITCH_TEMP_DIR);
#else
#ifdef WIN32
		GetTempPath(dwBufSize, lpPathBuffer);
		switch_snprintf(SWITCH_GLOBAL_dirs.temp_dir, BUFSIZE, "%s", lpPathBuffer);
#else
		switch_snprintf(SWITCH_GLOBAL_dirs.temp_dir, BUFSIZE, "%s", "/tmp/");
#endif
#endif
	}

	switch_assert(SWITCH_GLOBAL_dirs.base_dir);
	switch_assert(SWITCH_GLOBAL_dirs.mod_dir);
	switch_assert(SWITCH_GLOBAL_dirs.conf_dir);
	switch_assert(SWITCH_GLOBAL_dirs.log_dir);
	switch_assert(SWITCH_GLOBAL_dirs.run_dir);
	switch_assert(SWITCH_GLOBAL_dirs.db_dir);
	switch_assert(SWITCH_GLOBAL_dirs.script_dir);
	switch_assert(SWITCH_GLOBAL_dirs.htdocs_dir);
	switch_assert(SWITCH_GLOBAL_dirs.grammar_dir);
	switch_assert(SWITCH_GLOBAL_dirs.recordings_dir);
	switch_assert(SWITCH_GLOBAL_dirs.sounds_dir);
	switch_assert(SWITCH_GLOBAL_dirs.temp_dir);
}

static int32_t set_priority(void)
{
#ifdef WIN32
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#else
#ifdef USE_SCHED_SETSCHEDULER
	/*
	 * Try to use a round-robin scheduler
	 * with a fallback if that does not work
	 */
	struct sched_param sched = { 0 };
	sched.sched_priority = 1;
	if (sched_setscheduler(0, SCHED_RR, &sched)) {
		sched.sched_priority = 0;
		if (sched_setscheduler(0, SCHED_OTHER, &sched)) {
			return -1;
		}
	}
#endif

#ifdef HAVE_SETPRIORITY
	/*
	 * setpriority() works on FreeBSD (6.2), nice() doesn't
	 */
	if (setpriority(PRIO_PROCESS, getpid(), -10) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not set nice level\n");
		return -1;
	}
#else
	if (nice(-10) != -10) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not set nice level\n");
		return -1;
	}
#endif
#endif
	return 0;
}


SWITCH_DECLARE(int32_t) set_normal_priority(void)
{
	return set_priority();
}

SWITCH_DECLARE(int32_t) set_high_priority(void)
{
#ifdef WIN32
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#else
	int pri;

#ifdef USE_SETRLIMIT
	struct rlimit lim = { RLIM_INFINITY, RLIM_INFINITY };
#endif
	
	if ((pri = set_priority())) {
		return pri;
	}

#ifdef USE_SETRLIMIT
	/*
	 * The amount of memory which can be mlocked is limited for non-root users.
	 * FS will segfault (= hitting the limit) soon after mlockall has been called
	 * and we've switched to a different user.
	 * So let's try to remove the mlock limit here...
	 */
	if (setrlimit(RLIMIT_MEMLOCK, &lim) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed to disable memlock limit, application may crash if run as non-root user!\n");
	}
#endif

#ifdef USE_MLOCKALL
	/*
	 * Pin memory pages to RAM to prevent being swapped to disk
	 */
	mlockall(MCL_CURRENT | MCL_FUTURE);
#endif

#endif
	return 0;
}

SWITCH_DECLARE(int32_t) change_user_group(const char *user, const char *group)
{
#ifndef WIN32
	uid_t runas_uid = 0;
	gid_t runas_gid = 0;
	struct passwd *runas_pw = NULL;

	if (user) {
		/*
		 * Lookup user information in the system's db
		 */
		runas_pw = getpwnam(user);
		if (!runas_pw) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Unknown user \"%s\"\n", user);
			return -1;
		}
		runas_uid = runas_pw->pw_uid;
	}

	if (group) {
		struct group *gr = NULL;

		/*
		 * Lookup group information in the system's db
		 */
		gr = getgrnam(group);
		if (!gr) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Unknown group \"%s\"\n", group);
			return -1;
		}
		runas_gid = gr->gr_gid;
	}

	if (runas_uid && getuid() == runas_uid && (!runas_gid || runas_gid == getgid())) {
		/* already running as the right user and group, nothing to do! */
		return 0;
	}

	if (runas_uid) {
#ifdef HAVE_SETGROUPS
		/*
		 * Drop all group memberships prior to changing anything
		 * or else we're going to inherit the parent's list of groups
		 * (which is not what we want...)
		 */
		if (setgroups(0, NULL) < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed to drop group access list\n");
			return -1;
		}
#endif
		if (runas_gid) {
			/*
			 * A group has been passed, switch to it
			 * (without loading the user's other groups)
			 */
			if (setgid(runas_gid) < 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed to change gid!\n");
				return -1;
			}
		} else {
			/*
			 * No group has been passed, use the user's primary group in this case
			 */
			if (setgid(runas_pw->pw_gid) < 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed to change gid!\n");
				return -1;
			}
#ifdef HAVE_INITGROUPS
			/*
			 * Set all the other groups the user is a member of
			 * (This can be really useful for fine-grained access control)
			 */
			if (initgroups(runas_pw->pw_name, runas_pw->pw_gid) < 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed to set group access list for user\n");
				return -1;
			}
#endif
		}

		/*
		 * Finally drop all privileges by switching to the new userid
		 */
		if (setuid(runas_uid) < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed to change uid!\n");
			return -1;
		}
	}
#endif
	return 0;
}

SWITCH_DECLARE(void) switch_core_runtime_loop(int bg)
{
#ifdef WIN32
	HANDLE shutdown_event;
	char path[256] = "";
#endif
	if (bg) {
		bg = 0;
#ifdef WIN32
		switch_snprintf(path, sizeof(path), "Global\\Freeswitch.%d", getpid());
		shutdown_event = CreateEvent(NULL, FALSE, FALSE, path);
		if (shutdown_event) {
			WaitForSingleObject(shutdown_event, INFINITE);
		}
#else
		runtime.running = 1;
		while (runtime.running) {
			switch_yield(1000000);
		}
#endif
	} else {
		/* wait for console input */
		switch_console_loop();
	}
}

SWITCH_DECLARE(const char *) switch_core_mime_ext2type(const char *ext)
{
	if (!ext) {
		return NULL;
	}
	return (const char *) switch_core_hash_find(runtime.mime_types, ext);
}


SWITCH_DECLARE(switch_hash_index_t *) switch_core_mime_index(void)
{
	return switch_hash_first(NULL, runtime.mime_types);
}

SWITCH_DECLARE(switch_status_t) switch_core_mime_add_type(const char *type, const char *ext)
{
	const char *check;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_assert(type);
	switch_assert(ext);

	check = (const char *) switch_core_hash_find(runtime.mime_types, ext);

	if (!check) {
		char *ptype = switch_core_permanent_strdup(type);
		char *ext_list = strdup(ext);
		int argc = 0;
		char *argv[20] = { 0 };
		int x;

		switch_assert(ext_list);

		if ((argc = switch_separate_string(ext_list, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {

			for (x = 0; x < argc; x++) {
				if (argv[x] && ptype) {
					switch_core_hash_insert(runtime.mime_types, argv[x], ptype);
				}
			}

			status = SWITCH_STATUS_SUCCESS;
		}

		free(ext_list);
	}

	return status;
}

static void load_mime_types(void)
{
	char *cf = "mime.types";
	int fd = -1;
	char line_buf[1024] = "";
	char *mime_path = NULL;

	mime_path = switch_mprintf("%s/%s", SWITCH_GLOBAL_dirs.conf_dir, cf);
	switch_assert(mime_path);

	fd = open(mime_path, O_RDONLY | O_BINARY);
	if (fd <= 0) {
		goto end;
	}

	while ((switch_fd_read_line(fd, line_buf, sizeof(line_buf)))) {
		char *p;
		char *type = line_buf;

		if (*line_buf == '#') {
			continue;
		}

		if ((p = strchr(line_buf, '\r')) || (p = strchr(line_buf, '\n'))) {
			*p = '\0';
		}

		if ((p = strchr(type, '\t')) || (p = strchr(type, ' '))) {
			*p++ = '\0';

			while (*p == ' ' || *p == '\t') {
				p++;
			}

			switch_core_mime_add_type(type, p);
		}

	}

	if (fd > -1) {
		close(fd);
		fd = -1;
	}

  end:

	switch_safe_free(mime_path);

}

SWITCH_DECLARE(void) switch_core_setrlimits(void)
{
#ifdef HAVE_SETRLIMIT
	struct rlimit rlp;

	/* 
	   Setting the stack size on FreeBSD results in an instant crash.

	   If anyone knows how to fix this,
	   feel free to submit a patch to http://jira.freeswitch.org 
	 */

#ifndef __FreeBSD__
	memset(&rlp, 0, sizeof(rlp));
	rlp.rlim_cur = SWITCH_THREAD_STACKSIZE;
	rlp.rlim_max = SWITCH_SYSTEM_THREAD_STACKSIZE;
	setrlimit(RLIMIT_STACK, &rlp);
#endif

	memset(&rlp, 0, sizeof(rlp));
	rlp.rlim_cur = 999999;
	rlp.rlim_max = 999999;
	setrlimit(RLIMIT_NOFILE, &rlp);

	memset(&rlp, 0, sizeof(rlp));
	rlp.rlim_cur = RLIM_INFINITY;
	rlp.rlim_max = RLIM_INFINITY;

	setrlimit(RLIMIT_CPU, &rlp);
	setrlimit(RLIMIT_DATA, &rlp);
	setrlimit(RLIMIT_FSIZE, &rlp);
#ifdef RLIMIT_NPROC
	setrlimit(RLIMIT_NPROC, &rlp);
#endif
#ifdef RLIMIT_RTPRIO
	setrlimit(RLIMIT_RTPRIO, &rlp);
#endif

#if !defined(__OpenBSD__) && !defined(__NetBSD__)
	setrlimit(RLIMIT_AS, &rlp);
#endif
#endif
	return;
}

typedef struct {
	switch_memory_pool_t *pool;
	switch_hash_t *hash;
} switch_ip_list_t;

static switch_ip_list_t IP_LIST = { 0 };

SWITCH_DECLARE(switch_bool_t) switch_check_network_list_ip_token(const char *ip_str, const char *list_name, const char **token)
{
	switch_network_list_t *list;
	ip_t  ip, mask, net;
	uint32_t bits;
	char *ipv6 = strchr(ip_str,':');
	switch_bool_t ok = SWITCH_FALSE;

	switch_mutex_lock(runtime.global_mutex);
	if (ipv6) {
		switch_inet_pton(AF_INET6, ip_str, &ip);
	} else {
		switch_inet_pton(AF_INET, ip_str, &ip);
		ip.v4 = htonl(ip.v4);
	}

	if ((list = switch_core_hash_find(IP_LIST.hash, list_name))) {
		if (ipv6) {
			ok = switch_network_list_validate_ip6_token(list, ip, token);
		} else {
			ok = switch_network_list_validate_ip_token(list, ip.v4, token);
		}
	} else if (strchr(list_name, '/')) {
		if (strchr(list_name, ',')) {
			char *list_name_dup = strdup(list_name);
			char *argv[32];
			int argc;

			switch_assert(list_name_dup);

			if ((argc = switch_separate_string(list_name_dup, ',', argv, (sizeof(argv) / sizeof(argv[0]))))) {
				int i;
				for (i = 0; i < argc; i++) {
					switch_parse_cidr(argv[i], &net, &mask, &bits);
					if (ipv6) {
						if ((ok = switch_testv6_subnet(ip, net, mask))){
							break;
						}
					} else {
						if ((ok = switch_test_subnet(ip.v4, net.v4, mask.v4))) {
							break;
						}
					}
				}
			}
			free(list_name_dup);
		} else {
			switch_parse_cidr(list_name, &net, &mask, &bits);
			ok = switch_test_subnet(ip.v4, net.v4, mask.v4);
		}
	}
	switch_mutex_unlock(runtime.global_mutex);

	return ok;
}


SWITCH_DECLARE(void) switch_load_network_lists(switch_bool_t reload)
{
	switch_xml_t xml = NULL, x_lists = NULL, x_list = NULL, x_node = NULL, cfg = NULL;
	switch_network_list_t *rfc_list, *list;
	char guess_ip[16] = "";
	int mask = 0;
	char guess_mask[16] = "";
	char *tmp_name;
	struct in_addr in;

	switch_find_local_ip(guess_ip, sizeof(guess_ip), &mask, AF_INET);
	in.s_addr = mask;
	switch_set_string(guess_mask, inet_ntoa(in));

	switch_mutex_lock(runtime.global_mutex);

	if (IP_LIST.hash) {
		switch_core_hash_destroy(&IP_LIST.hash);
	}

	if (IP_LIST.pool) {
		switch_core_destroy_memory_pool(&IP_LIST.pool);
	}

	memset(&IP_LIST, 0, sizeof(IP_LIST));
	switch_core_new_memory_pool(&IP_LIST.pool);
	switch_core_hash_init(&IP_LIST.hash, IP_LIST.pool);


	tmp_name = "rfc1918.auto";
	switch_network_list_create(&rfc_list, tmp_name, SWITCH_FALSE, IP_LIST.pool);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Created ip list %s default (deny)\n", tmp_name);
	switch_network_list_add_cidr(rfc_list, "10.0.0.0/8", SWITCH_TRUE);
	switch_network_list_add_cidr(rfc_list, "172.16.0.0/12", SWITCH_TRUE);
	switch_network_list_add_cidr(rfc_list, "192.168.0.0/16", SWITCH_TRUE);
	switch_core_hash_insert(IP_LIST.hash, tmp_name, rfc_list);

	tmp_name = "wan.auto";
	switch_network_list_create(&rfc_list, tmp_name, SWITCH_TRUE, IP_LIST.pool);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Created ip list %s default (allow)\n", tmp_name);
	switch_network_list_add_cidr(rfc_list, "10.0.0.0/8", SWITCH_FALSE);
	switch_network_list_add_cidr(rfc_list, "172.16.0.0/12", SWITCH_FALSE);
	switch_network_list_add_cidr(rfc_list, "192.168.0.0/16", SWITCH_FALSE);
	switch_core_hash_insert(IP_LIST.hash, tmp_name, rfc_list);

	tmp_name = "nat.auto";
	switch_network_list_create(&rfc_list, tmp_name, SWITCH_FALSE, IP_LIST.pool);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Created ip list %s default (deny)\n", tmp_name);
	if (switch_network_list_add_host_mask(rfc_list, guess_ip, guess_mask, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Adding %s/%s (deny) to list %s\n", guess_ip, guess_mask, tmp_name);
	}
	switch_network_list_add_cidr(rfc_list, "10.0.0.0/8", SWITCH_TRUE);
	switch_network_list_add_cidr(rfc_list, "172.16.0.0/12", SWITCH_TRUE);
	switch_network_list_add_cidr(rfc_list, "192.168.0.0/16", SWITCH_TRUE);
	switch_core_hash_insert(IP_LIST.hash, tmp_name, rfc_list);

	tmp_name = "loopback.auto";
	switch_network_list_create(&rfc_list, tmp_name, SWITCH_FALSE, IP_LIST.pool);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Created ip list %s default (deny)\n", tmp_name);
	switch_network_list_add_cidr(rfc_list, "127.0.0.0/8", SWITCH_TRUE);
	switch_core_hash_insert(IP_LIST.hash, tmp_name, rfc_list);

	tmp_name = "localnet.auto";
	switch_network_list_create(&list, tmp_name, SWITCH_FALSE, IP_LIST.pool);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Created ip list %s default (deny)\n", tmp_name);

	if (switch_network_list_add_host_mask(list, guess_ip, guess_mask, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Adding %s/%s (allow) to list %s\n", guess_ip, guess_mask, tmp_name);
	}
	switch_core_hash_insert(IP_LIST.hash, tmp_name, list);


	if ((xml = switch_xml_open_cfg("acl.conf", &cfg, NULL))) {
		if ((x_lists = switch_xml_child(cfg, "network-lists"))) {
			for (x_list = switch_xml_child(x_lists, "list"); x_list; x_list = x_list->next) {
				const char *name = switch_xml_attr(x_list, "name");
				const char *dft = switch_xml_attr(x_list, "default");
				switch_bool_t default_type = SWITCH_TRUE;

				if (zstr(name)) {
					continue;
				}

				if (dft) {
					default_type = switch_true(dft);
				}

				if (switch_network_list_create(&list, name, default_type, IP_LIST.pool) != SWITCH_STATUS_SUCCESS) {
					abort();
				}

				if (reload) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Created ip list %s default (%s)\n", name, default_type ? "allow" : "deny");
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Created ip list %s default (%s)\n", name, default_type ? "allow" : "deny");
				}


				for (x_node = switch_xml_child(x_list, "node"); x_node; x_node = x_node->next) {
					const char *cidr = NULL, *host = NULL, *mask = NULL, *domain = NULL;
					switch_bool_t ok = default_type;
					const char *type = switch_xml_attr(x_node, "type");

					if (type) {
						ok = switch_true(type);
					}

					cidr = switch_xml_attr(x_node, "cidr");
					host = switch_xml_attr(x_node, "host");
					mask = switch_xml_attr(x_node, "mask");
					domain = switch_xml_attr(x_node, "domain");

					if (domain) {
						switch_event_t *my_params = NULL;
						switch_xml_t x_domain, xml_root;
						switch_xml_t gt, gts, ut, uts;

						switch_event_create(&my_params, SWITCH_EVENT_GENERAL);
						switch_assert(my_params);
						switch_event_add_header_string(my_params, SWITCH_STACK_BOTTOM, "domain", domain);
						switch_event_add_header_string(my_params, SWITCH_STACK_BOTTOM, "purpose", "network-list");

						if (switch_xml_locate_domain(domain, my_params, &xml_root, &x_domain) != SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot locate domain %s\n", domain);
							switch_event_destroy(&my_params);
							continue;
						}

						switch_event_destroy(&my_params);

						for (ut = switch_xml_child(x_domain, "user"); ut; ut = ut->next) {
							const char *user_cidr = switch_xml_attr(ut, "cidr");
							const char *id = switch_xml_attr(ut, "id");

							if (id && user_cidr) {
								char *token = switch_mprintf("%s@%s", id, domain);
								switch_assert(token);
								switch_network_list_add_cidr_token(list, user_cidr, ok, token);
								free(token);
							}
						}

						for (gts = switch_xml_child(x_domain, "groups"); gts; gts = gts->next) {
							for (gt = switch_xml_child(gts, "group"); gt; gt = gt->next) {
								for (uts = switch_xml_child(gt, "users"); uts; uts = uts->next) {
									for (ut = switch_xml_child(uts, "user"); ut; ut = ut->next) {
										const char *user_cidr = switch_xml_attr(ut, "cidr");
										const char *id = switch_xml_attr(ut, "id");

										if (id && user_cidr) {
											char *token = switch_mprintf("%s@%s", id, domain);
											switch_assert(token);
											switch_network_list_add_cidr_token(list, user_cidr, ok, token);
											free(token);
										}
									}
								}
							}
						}

						switch_xml_free(xml_root);
					} else if (cidr) {
						if (switch_network_list_add_cidr(list, cidr, ok) == SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Adding %s (%s) to list %s\n", cidr, ok ? "allow" : "deny", name);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
											  "Error Adding %s (%s) to list %s\n", cidr, ok ? "allow" : "deny", name);
						}
					} else if (host && mask) {
						if (switch_network_list_add_host_mask(list, host, mask, ok) == SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
											  "Adding %s/%s (%s) to list %s\n", host, mask, ok ? "allow" : "deny", name);
						}
					}

					switch_core_hash_insert(IP_LIST.hash, name, list);
				}
			}
		}

		switch_xml_free(xml);
	}

	switch_mutex_unlock(runtime.global_mutex);
}

SWITCH_DECLARE(uint32_t) switch_core_max_dtmf_duration(uint32_t duration)
{
	if (duration) {
		if (duration > SWITCH_MAX_DTMF_DURATION) {
			duration = SWITCH_MAX_DTMF_DURATION;
		}
		if (duration < SWITCH_MIN_DTMF_DURATION) {
			duration = SWITCH_MIN_DTMF_DURATION;
		}
		runtime.max_dtmf_duration = duration;
	}
	return runtime.max_dtmf_duration;
}

SWITCH_DECLARE(uint32_t) switch_core_default_dtmf_duration(uint32_t duration)
{
	if (duration) {
		if (duration < SWITCH_MIN_DTMF_DURATION) {
			duration = SWITCH_MIN_DTMF_DURATION;
		}
		if (duration > SWITCH_MAX_DTMF_DURATION) {
			duration = SWITCH_MAX_DTMF_DURATION;
		}
		runtime.default_dtmf_duration = duration;
	}
	return runtime.default_dtmf_duration;
}

SWITCH_DECLARE(uint32_t) switch_core_min_dtmf_duration(uint32_t duration)
{
	if (duration) {
		if (duration < SWITCH_MIN_DTMF_DURATION) {
			duration = SWITCH_MIN_DTMF_DURATION;
		}
		if (duration > SWITCH_MAX_DTMF_DURATION) {
			duration = SWITCH_MAX_DTMF_DURATION;
		}
	}
	return runtime.min_dtmf_duration;
}

static void switch_core_set_serial(void)
{
	char buf[13] = "";
	char path[256];

	int fd = -1, write_fd = -1;
	switch_ssize_t bytes = 0;

	switch_snprintf(path, sizeof(path), "%s%sfreeswitch.serial", SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR);


	if ((fd = open(path, O_RDONLY, 0)) < 0) {
		char *ip = switch_core_get_variable_dup("local_ip_v4");
		uint32_t ipi = 0;
		switch_byte_t *byte;
		int i = 0;

		if (ip) {
			switch_inet_pton(AF_INET, ip, &ipi);
			free(ip);
			ip = NULL;
		}


		byte = (switch_byte_t *) & ipi;

		for (i = 0; i < 8; i += 2) {
			switch_snprintf(buf + i, sizeof(buf) - i, "%0.2x", *byte);
			byte++;
		}

		switch_stun_random_string(buf + 8, 4, "0123456789abcdef");

		if ((write_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) >= 0) {
			bytes = write(write_fd, buf, sizeof(buf));
			bytes++;
			close(write_fd);
			write_fd = -1;
		}
	} else {
		bytes = read(fd, buf, sizeof(buf));
		close(fd);
		fd = -1;
	}

	switch_core_set_variable("switch_serial", buf);
}


SWITCH_DECLARE(switch_status_t) switch_core_init(switch_core_flag_t flags, switch_bool_t console, const char **err)
{
	switch_uuid_t uuid;
	char guess_ip[256];
	int mask = 0;
	struct in_addr in;


	if (runtime.runlevel > 0) {
		/* one per customer */
		return SWITCH_STATUS_SUCCESS;
	}
	
	memset(&runtime, 0, sizeof(runtime));
	gethostname(runtime.hostname, sizeof(runtime.hostname));

	runtime.max_db_handles = 50;
	runtime.db_handle_timeout = 5000000;;
	
	runtime.runlevel++;
	runtime.sql_buffer_len = 1024 * 32;
	runtime.max_sql_buffer_len = 1024 * 1024;
	runtime.dummy_cng_frame.data = runtime.dummy_data;
	runtime.dummy_cng_frame.datalen = sizeof(runtime.dummy_data);
	runtime.dummy_cng_frame.buflen = sizeof(runtime.dummy_data);
	switch_set_flag((&runtime.dummy_cng_frame), SFF_CNG);
	switch_set_flag((&runtime), SCF_AUTO_SCHEMAS);
	switch_set_flag((&runtime), SCF_CLEAR_SQL);

	switch_set_flag((&runtime), SCF_NO_NEW_SESSIONS);
	runtime.hard_log_level = SWITCH_LOG_DEBUG;
	runtime.mailer_app = "sendmail";
	runtime.mailer_app_args = "-t";
	runtime.max_dtmf_duration = SWITCH_MAX_DTMF_DURATION;
	runtime.default_dtmf_duration = SWITCH_DEFAULT_DTMF_DURATION;
	runtime.min_dtmf_duration = SWITCH_MIN_DTMF_DURATION;
	runtime.odbc_dbtype = DBTYPE_DEFAULT;
	runtime.dbname = NULL;

	/* INIT APR and Create the pool context */
	if (apr_initialize() != SWITCH_STATUS_SUCCESS) {
		*err = "FATAL ERROR! Could not initialize APR\n";
		return SWITCH_STATUS_MEMERR;
	}

	if (!(runtime.memory_pool = switch_core_memory_init())) {
		*err = "FATAL ERROR! Could not allocate memory pool\n";
		return SWITCH_STATUS_MEMERR;
	}
	switch_assert(runtime.memory_pool != NULL);

	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.base_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.mod_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.conf_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.log_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.run_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.db_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.script_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.htdocs_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.grammar_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.recordings_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.sounds_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.temp_dir, SWITCH_DEFAULT_DIR_PERMS, runtime.memory_pool);


	switch_mutex_init(&runtime.uuid_mutex, SWITCH_MUTEX_NESTED, runtime.memory_pool);

	switch_mutex_init(&runtime.throttle_mutex, SWITCH_MUTEX_NESTED, runtime.memory_pool);

	switch_mutex_init(&runtime.session_hash_mutex, SWITCH_MUTEX_NESTED, runtime.memory_pool);
	switch_mutex_init(&runtime.global_mutex, SWITCH_MUTEX_NESTED, runtime.memory_pool);

	switch_thread_rwlock_create(&runtime.global_var_rwlock, runtime.memory_pool);
	switch_core_set_globals();
	switch_core_session_init(runtime.memory_pool);
	switch_event_create_plain(&runtime.global_vars, SWITCH_EVENT_CHANNEL_DATA);
	switch_core_hash_init(&runtime.mime_types, runtime.memory_pool);
	switch_core_hash_init_case(&runtime.ptimes, runtime.memory_pool, SWITCH_FALSE);
	load_mime_types();
	runtime.flags |= flags;
	runtime.sps_total = 30;

	*err = NULL;

	if (console) {
		runtime.console = stdout;
	}

	switch_core_set_variable("hostname", runtime.hostname);
	switch_find_local_ip(guess_ip, sizeof(guess_ip), &mask, AF_INET);
	switch_core_set_variable("local_ip_v4", guess_ip);
	in.s_addr = mask;
	switch_core_set_variable("local_mask_v4", inet_ntoa(in));


	switch_find_local_ip(guess_ip, sizeof(guess_ip), NULL, AF_INET6);
	switch_core_set_variable("local_ip_v6", guess_ip);
	switch_core_set_variable("base_dir", SWITCH_GLOBAL_dirs.base_dir);
	switch_core_set_variable("recordings_dir", SWITCH_GLOBAL_dirs.recordings_dir);
	switch_core_set_variable("sound_prefix", SWITCH_GLOBAL_dirs.sounds_dir);
	switch_core_set_variable("sounds_dir", SWITCH_GLOBAL_dirs.sounds_dir);
	switch_core_set_serial();

	switch_console_init(runtime.memory_pool);
	switch_event_init(runtime.memory_pool);

	if (switch_xml_init(runtime.memory_pool, err) != SWITCH_STATUS_SUCCESS) {
		apr_terminate();
		return SWITCH_STATUS_MEMERR;
	}

	if (switch_test_flag((&runtime), SCF_USE_AUTO_NAT)) {
		switch_nat_init(runtime.memory_pool, switch_test_flag((&runtime), SCF_USE_NAT_MAPPING));
	}

	switch_log_init(runtime.memory_pool, runtime.colorize_console);

	if (flags & SCF_MINIMAL) return SWITCH_STATUS_SUCCESS;
													   
	runtime.tipping_point = 0;
	runtime.timer_affinity = -1;
	runtime.microseconds_per_tick = 20000;

	switch_load_core_config("switch.conf");

	switch_core_state_machine_init(runtime.memory_pool);

	if (switch_core_sqldb_start(runtime.memory_pool, switch_test_flag((&runtime), SCF_USE_SQL) ? SWITCH_TRUE : SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		abort();
	}

	switch_scheduler_task_thread_start();

	switch_nat_late_init();

	switch_rtp_init(runtime.memory_pool);

	runtime.running = 1;
	runtime.initiated = switch_time_now();
	
	switch_scheduler_add_task(switch_epoch_time_now(NULL), heartbeat_callback, "heartbeat", "core", 0, NULL, SSHF_NONE | SSHF_NO_DEL);

	switch_scheduler_add_task(switch_epoch_time_now(NULL), check_ip_callback, "check_ip", "core", 0, NULL, SSHF_NONE | SSHF_NO_DEL | SSHF_OWN_THREAD);

	switch_uuid_get(&uuid);
	switch_uuid_format(runtime.uuid_str, &uuid);
	switch_ssl_init_ssl_locks();

	return SWITCH_STATUS_SUCCESS;
}


#ifdef TRAP_BUS
static void handle_SIGBUS(int sig)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG1, "Sig BUS!\n");
	return;
}
#endif

static void handle_SIGHUP(int sig)
{
	if (sig) {
		switch_event_t *event;

		if (switch_event_create(&event, SWITCH_EVENT_TRAP) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Trapped-Signal", "HUP");
			switch_event_fire(&event);
		}
	}
	return;
}


SWITCH_DECLARE(uint32_t) switch_default_ptime(const char *name, uint32_t number)
{
	uint32_t *p;

	if ((p = switch_core_hash_find(runtime.ptimes, name))) {
		return *p;
	}

	return 20;
}

static uint32_t d_30 = 30;

static void switch_load_core_config(const char *file)
{
	switch_xml_t xml = NULL, cfg = NULL;

	switch_core_hash_insert(runtime.ptimes, "ilbc", &d_30);
	switch_core_hash_insert(runtime.ptimes, "G723", &d_30);

	if ((xml = switch_xml_open_cfg(file, &cfg, NULL))) {
		switch_xml_t settings, param;

		if ((settings = switch_xml_child(cfg, "default-ptimes"))) {
			for (param = switch_xml_child(settings, "codec"); param; param = param->next) {
				const char *var = switch_xml_attr_soft(param, "name");
				const char *val = switch_xml_attr_soft(param, "ptime");
				
				if (!zstr(var) && !zstr(val)) {
					uint32_t *p;
					uint32_t v = (unsigned long) atol(val);

					if (!strcasecmp(var, "G723") || !strcasecmp(var, "iLBC")) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Error adding %s, defaults cannot be changed\n", var);
						continue;
					}
					
					if (v < 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Error adding %s, invalid ptime\n", var);
						continue;
					}

					p = switch_core_alloc(runtime.memory_pool, sizeof(*p));
					*p = v;
					switch_core_hash_insert(runtime.ptimes, var, p);
				}

			}
		}

		if ((settings = switch_xml_child(cfg, "settings"))) {
			for (param = switch_xml_child(settings, "param"); param; param = param->next) {
				const char *var = switch_xml_attr_soft(param, "name");
				const char *val = switch_xml_attr_soft(param, "value");

				if (!strcasecmp(var, "loglevel")) {
					int level;
					if (*val > 47 && *val < 58) {
						level = atoi(val);
					} else {
						level = switch_log_str2level(val);
					}

					if (level != SWITCH_LOG_INVALID) {
						switch_core_session_ctl(SCSC_LOGLEVEL, &level);
					}
#ifdef HAVE_SETRLIMIT
				} else if (!strcasecmp(var, "dump-cores")) {
					struct rlimit rlp;
					memset(&rlp, 0, sizeof(rlp));
					rlp.rlim_cur = RLIM_INFINITY;
					rlp.rlim_max = RLIM_INFINITY;
					setrlimit(RLIMIT_CORE, &rlp);
#endif
				} else if (!strcasecmp(var, "debug-level")) {
					int tmp = atoi(val);
					if (tmp > -1 && tmp < 11) {
						switch_core_session_ctl(SCSC_DEBUG_LEVEL, &tmp);
					}
				} else if (!strcasecmp(var, "max-db-handles")) {
					long tmp = atol(val);

					if (tmp > 4 && tmp < 5001) {
						runtime.max_db_handles = (uint32_t) tmp;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "max-db-handles must be between 5 and 5000\n");
					}
				} else if (!strcasecmp(var, "db-handle-timeout")) {
					long tmp = atol(val);
					
					if (tmp > 0 && tmp < 5001) {
						runtime.db_handle_timeout = (uint32_t) tmp * 1000000;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "db-handle-timeout must be between 1 and 5000\n");
					}
					
				} else if (!strcasecmp(var, "multiple-registrations")) {
					runtime.multiple_registrations = switch_true(val);
				} else if (!strcasecmp(var, "sql-buffer-len")) {
					int tmp = atoi(val);

					if (end_of(val) == 'k') {
						tmp *= 1024;
					} else if (end_of(val) == 'm') {
						tmp *= (1024 * 1024);
					}
					
					if (tmp >= 32000 && tmp < 10500000) {
						runtime.sql_buffer_len = tmp;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "sql-buffer-len: Value is not within rage 32k to 10m\n");
					}
				} else if (!strcasecmp(var, "max-sql-buffer-len")) {
					int tmp = atoi(val);

					if (end_of(val) == 'k') {
						tmp *= 1024;
					} else if (end_of(val) == 'm') {
						tmp *= (1024 * 1024);
					}

					if (tmp < runtime.sql_buffer_len) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Value is not larger than sql-buffer-len\n");
					} else if (tmp >= 32000 && tmp < 10500000) {
						runtime.sql_buffer_len = tmp;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "max-sql-buffer-len: Value is not within rage 32k to 10m\n");
					}

				} else if (!strcasecmp(var, "auto-create-schemas")) {
					if (switch_true(val)) {
						switch_set_flag((&runtime), SCF_AUTO_SCHEMAS);
					} else {
						switch_clear_flag((&runtime), SCF_AUTO_SCHEMAS);
					}
				} else if (!strcasecmp(var, "auto-clear-sql")) {
					if (switch_true(val)) {
						switch_set_flag((&runtime), SCF_CLEAR_SQL);
					} else {
						switch_clear_flag((&runtime), SCF_CLEAR_SQL);
					}
				} else if (!strcasecmp(var, "enable-early-hangup") && switch_true(val)) {
					switch_set_flag((&runtime), SCF_EARLY_HANGUP);
				} else if (!strcasecmp(var, "colorize-console") && switch_true(val)) {
					runtime.colorize_console = SWITCH_TRUE;
				} else if (!strcasecmp(var, "mailer-app") && !zstr(val)) {
					runtime.mailer_app = switch_core_strdup(runtime.memory_pool, val);
				} else if (!strcasecmp(var, "mailer-app-args") && val) {
					runtime.mailer_app_args = switch_core_strdup(runtime.memory_pool, val);
				} else if (!strcasecmp(var, "sessions-per-second") && !zstr(val)) {
					switch_core_sessions_per_second(atoi(val));
				} else if (!strcasecmp(var, "max-dtmf-duration") && !zstr(val)) {
					int tmp = atoi(val);
					if (tmp > 0) {
						switch_core_max_dtmf_duration((uint32_t) tmp);
					}
				} else if (!strcasecmp(var, "min-dtmf-duration") && !zstr(val)) {
					int tmp = atoi(val);
					if (tmp > 0) {
						switch_core_min_dtmf_duration((uint32_t) tmp);
					}
				} else if (!strcasecmp(var, "default-dtmf-duration") && !zstr(val)) {
					int tmp = atoi(val);
					if (tmp > 0) {
						switch_core_default_dtmf_duration((uint32_t) tmp);
					}
				} else if (!strcasecmp(var, "enable-monotonic-timing")) {
					switch_time_set_monotonic(switch_true(val));
				} else if (!strcasecmp(var, "enable-softtimer-timerfd")) {
					switch_time_set_timerfd(switch_true(val));
				} else if (!strcasecmp(var, "enable-clock-nanosleep")) {
					switch_time_set_nanosleep(switch_true(val));
				} else if (!strcasecmp(var, "enable-cond-yield")) {
					switch_time_set_cond_yield(switch_true(val));
				} else if (!strcasecmp(var, "enable-timer-matrix")) {
					switch_time_set_matrix(switch_true(val));
				} else if (!strcasecmp(var, "max-sessions") && !zstr(val)) {
					switch_core_session_limit(atoi(val));
				} else if (!strcasecmp(var, "verbose-channel-events") && !zstr(val)) {
					int v = switch_true(val);
					if (v) {
						switch_set_flag((&runtime), SCF_VERBOSE_EVENTS);
					} else {
						switch_clear_flag((&runtime), SCF_VERBOSE_EVENTS);
					}
				} else if (!strcasecmp(var, "min-idle-cpu") && !zstr(val)) {
					switch_core_min_idle_cpu(atof(val));
				} else if (!strcasecmp(var, "tipping-point") && !zstr(val)) {
					runtime.tipping_point = atoi(val);
				} else if (!strcasecmp(var, "1ms-timer") && switch_true(val)) {
					runtime.microseconds_per_tick = 1000;
				} else if (!strcasecmp(var, "timer-affinity") && !zstr(val)) {
					if (!strcasecmp(val, "disabled")) {
						runtime.timer_affinity = -1;
					} else {
						runtime.timer_affinity = atoi(val);
					}
				} else if (!strcasecmp(var, "rtp-start-port") && !zstr(val)) {
					switch_rtp_set_start_port((switch_port_t) atoi(val));
				} else if (!strcasecmp(var, "rtp-end-port") && !zstr(val)) {
					switch_rtp_set_end_port((switch_port_t) atoi(val));
				} else if (!strcasecmp(var, "core-db-name") && !zstr(val)) {
					runtime.dbname = switch_core_strdup(runtime.memory_pool, val);
				} else if (!strcasecmp(var, "core-db-dsn") && !zstr(val)) {
					if (switch_odbc_available()) {
						runtime.odbc_dsn = switch_core_strdup(runtime.memory_pool, val);
						if ((runtime.odbc_user = strchr(runtime.odbc_dsn, ':'))) {
							*runtime.odbc_user++ = '\0';
							if ((runtime.odbc_pass = strchr(runtime.odbc_user, ':'))) {
								*runtime.odbc_pass++ = '\0';
							}
						}
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ODBC IS NOT AVAILABLE!\n");
					}
				} else if (!strcasecmp(var, "core-dbtype") && !zstr(val)) {
					if (!strcasecmp(val, "MSSQL")) {
						runtime.odbc_dbtype = DBTYPE_MSSQL;
					} else {
						runtime.odbc_dbtype = DBTYPE_DEFAULT;
					}
#ifdef ENABLE_ZRTP
				} else if (!strcasecmp(var, "rtp-enable-zrtp")) {
					switch_core_set_variable("zrtp_enabled", val);
#endif
                } else if (!strcasecmp(var, "switchname") && !zstr(val)) {
					runtime.switchname = switch_core_strdup(runtime.memory_pool, val);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Set switchname to %s\n", runtime.switchname);
				}
			}
		}

		if ((settings = switch_xml_child(cfg, "variables"))) {
			for (param = switch_xml_child(settings, "variable"); param; param = param->next) {
				const char *var = switch_xml_attr_soft(param, "name");
				const char *val = switch_xml_attr_soft(param, "value");
				if (var && val) {
					switch_core_set_variable(var, val);
				}
			}
		}

		switch_xml_free(xml);
	}


}

SWITCH_DECLARE(const char *) switch_core_banner(void)
{


	return ("\n"
			"   _____              ______        _____ _____ ____ _   _  \n"
			"  |  ___| __ ___  ___/ ___\\ \\      / /_ _|_   _/ ___| | | | \n"
			"  | |_ | '__/ _ \\/ _ \\___ \\\\ \\ /\\ / / | |  | || |   | |_| | \n"
			"  |  _|| | |  __/  __/___) |\\ V  V /  | |  | || |___|  _  | \n"
			"  |_|  |_|  \\___|\\___|____/  \\_/\\_/  |___| |_| \\____|_| |_| \n"
			"\n"
			"************************************************************\n"
			"* Anthony Minessale II, Michael Jerris, Brian West, Others *\n"
			"* FreeSWITCH (http://www.freeswitch.org)                   *\n"
			"* Paypal Donations Appreciated: paypal@freeswitch.org      *\n"
			"* Brought to you by ClueCon http://www.cluecon.com/        *\n" "************************************************************\n" "\n");
}


SWITCH_DECLARE(switch_status_t) switch_core_init_and_modload(switch_core_flag_t flags, switch_bool_t console, const char **err)
{
	switch_event_t *event;

	if (switch_core_init(flags, console, err) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_GENERR;
	}
	
	if (runtime.runlevel > 1) {
		/* one per customer */
		return SWITCH_STATUS_SUCCESS;
	}

	runtime.runlevel++;

	switch_core_set_signal_handlers();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Bringing up environment.\n");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Loading Modules.\n");
	if (switch_loadable_module_init(SWITCH_TRUE) != SWITCH_STATUS_SUCCESS) {
		*err = "Cannot load modules";
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Error: %s\n", *err);
		return SWITCH_STATUS_GENERR;
	}

	switch_load_network_lists(SWITCH_FALSE);

	switch_load_core_config("post_load_switch.conf");

	switch_core_set_signal_handlers();

	if (switch_event_create(&event, SWITCH_EVENT_STARTUP) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Event-Info", "System Ready");
		switch_event_fire(&event);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "%s", switch_core_banner());


	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE,
					  "\nFreeSWITCH Version %s Started.\nMax Sessions[%u]\nSession Rate[%d]\nSQL [%s]\n", SWITCH_VERSION_FULL,
					  switch_core_session_limit(0),
					  switch_core_sessions_per_second(0), switch_test_flag((&runtime), SCF_USE_SQL) ? "Enabled" : "Disabled");

	switch_clear_flag((&runtime), SCF_NO_NEW_SESSIONS);

	return SWITCH_STATUS_SUCCESS;

}

SWITCH_DECLARE(void) switch_core_measure_time(switch_time_t total_ms, switch_core_time_duration_t *duration)
{
	switch_time_t temp = total_ms / 1000;
	memset(duration, 0, sizeof(*duration));
	duration->mms = (uint32_t) (total_ms % 1000);
	duration->ms = (uint32_t) (temp % 1000);
	temp = temp / 1000;
	duration->sec = (uint32_t) (temp % 60);
	temp = temp / 60;
	duration->min = (uint32_t) (temp % 60);
	temp = temp / 60;
	duration->hr = (uint32_t) (temp % 24);
	temp = temp / 24;
	duration->day = (uint32_t) (temp % 365);
	duration->yr = (uint32_t) (temp / 365);
}

SWITCH_DECLARE(switch_time_t) switch_core_uptime(void)
{
	return switch_micro_time_now() - runtime.initiated;
}


#ifdef _MSC_VER
static void win_shutdown(void)
{

	HANDLE shutdown_event;
	char path[512];
	/* for windows we need the event to signal for shutting down a background FreeSWITCH */
	snprintf(path, sizeof(path), "Global\\Freeswitch.%d", getpid());

	/* open the event so we can signal it */
	shutdown_event = OpenEvent(EVENT_MODIFY_STATE, FALSE, path);

	if (shutdown_event) {
		/* signal the event to shutdown */
		SetEvent(shutdown_event);
		/* cleanup */
		CloseHandle(shutdown_event);
	}
}
#endif

SWITCH_DECLARE(void) switch_core_set_signal_handlers(void)
{
	/* set signal handlers */
	signal(SIGINT, SIG_IGN);

#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif
#ifdef SIGQUIT
	signal(SIGQUIT, SIG_IGN);
#endif
#ifdef SIGPOLL
	signal(SIGPOLL, SIG_IGN);
#endif
#ifdef SIGIO
	signal(SIGIO, SIG_IGN);
#endif
#ifdef TRAP_BUS
	signal(SIGBUS, handle_SIGBUS);
#endif
#ifdef SIGUSR1
	signal(SIGUSR1, handle_SIGHUP);
#endif
	signal(SIGHUP, handle_SIGHUP);
}

SWITCH_DECLARE(uint32_t) switch_core_debug_level(void)
{
	return runtime.debug_level;
}


SWITCH_DECLARE(int32_t) switch_core_session_ctl(switch_session_ctl_t cmd, void *val)
{
	int *intval = (int *) val;
	int oldintval = 0, newintval = 0;
	
	if (intval) {
		oldintval = *intval;
	}

	if (switch_test_flag((&runtime), SCF_SHUTTING_DOWN)) {
		return -1;
	}

	switch (cmd) {
	case SCSC_VERBOSE_EVENTS:
		if (intval) {
			if (oldintval > -1) {
				if (oldintval) {
					switch_set_flag((&runtime), SCF_VERBOSE_EVENTS);
				} else {
					switch_clear_flag((&runtime), SCF_VERBOSE_EVENTS);
				}
			}
			newintval = switch_test_flag((&runtime), SCF_VERBOSE_EVENTS);
		}
		break;
	case SCSC_CALIBRATE_CLOCK:
		switch_time_calibrate_clock();
		break;
	case SCSC_FLUSH_DB_HANDLES:
		switch_cache_db_flush_handles();
		break;
	case SCSC_SEND_SIGHUP:
		handle_SIGHUP(1);
		break;
	case SCSC_SYNC_CLOCK:
		switch_time_sync();
		newintval = 0;
		break;
	case SCSC_PAUSE_INBOUND:
		if (oldintval) {
			switch_set_flag((&runtime), SCF_NO_NEW_SESSIONS);
		} else {
			switch_clear_flag((&runtime), SCF_NO_NEW_SESSIONS);
		}
		break;
	case SCSC_HUPALL:
		switch_core_session_hupall(SWITCH_CAUSE_MANAGER_REQUEST);
		break;
	case SCSC_CANCEL_SHUTDOWN:
		switch_clear_flag((&runtime), SCF_SHUTDOWN_REQUESTED);
		break;
	case SCSC_SAVE_HISTORY:
		switch_console_save_history();
		break;
	case SCSC_CRASH:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Declinatio Mortuus Obfirmo!\n");
		switch_console_save_history();
		abort();
		break;
	case SCSC_SHUTDOWN_NOW:
		switch_console_save_history();
		exit(0);
		break;
	case SCSC_SHUTDOWN_ELEGANT:
	case SCSC_SHUTDOWN_ASAP:
		{
			int x = 19;
			uint32_t count;

			switch_set_flag((&runtime), SCF_SHUTDOWN_REQUESTED);
			if (cmd == SCSC_SHUTDOWN_ASAP) {
				switch_set_flag((&runtime), SCF_NO_NEW_SESSIONS);
			}

			while (runtime.running && switch_test_flag((&runtime), SCF_SHUTDOWN_REQUESTED) && (count = switch_core_session_count())) {
				switch_yield(500000);
				if (++x == 20) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
									  "Shutdown in progress, %u session(s) remain.\nShutting down %s\n",
									  count, cmd == SCSC_SHUTDOWN_ASAP ? "ASAP" : "once there are no active calls.");
					x = 0;
				}
			}

			if (switch_test_flag((&runtime), SCF_SHUTDOWN_REQUESTED)) {
				switch_set_flag((&runtime), SCF_NO_NEW_SESSIONS);
#ifdef _MSC_VER
				win_shutdown();
#endif

				if (oldintval) {
					switch_set_flag((&runtime), SCF_RESTART);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Restarting\n");
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Shutting down\n");
#ifdef _MSC_VER
					fclose(stdin);
#endif
				}
				runtime.running = 0;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Shutdown Cancelled\n");
				switch_clear_flag((&runtime), SCF_NO_NEW_SESSIONS);
			}
		}
		break;
	case SCSC_PAUSE_CHECK:
		newintval = !!switch_test_flag((&runtime), SCF_NO_NEW_SESSIONS);
		break;
	case SCSC_READY_CHECK:
		newintval = switch_core_ready();
		break;
	case SCSC_SHUTDOWN_CHECK:
		newintval = !!switch_test_flag((&runtime), SCF_SHUTDOWN_REQUESTED);
		break;
	case SCSC_SHUTDOWN:

#ifdef _MSC_VER
		win_shutdown();
#endif

		if (oldintval) {
			switch_set_flag((&runtime), SCF_RESTART);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Restarting\n");
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Shutting down\n");
#ifdef _MSC_VER
			fclose(stdin);
#endif
		}
		runtime.running = 0;
		break;
	case SCSC_CHECK_RUNNING:
		newintval = runtime.running;
		break;
	case SCSC_LOGLEVEL:
		if (oldintval > -1) {
			runtime.hard_log_level = oldintval;
		}

		if (runtime.hard_log_level > SWITCH_LOG_DEBUG) {
			runtime.hard_log_level = SWITCH_LOG_DEBUG;
		}
		newintval = runtime.hard_log_level;
		break;
	case SCSC_DEBUG_LEVEL:
		if (oldintval > -1) {
			if (oldintval > 10)
				newintval = 10;
			runtime.debug_level = oldintval;
		}
		newintval = runtime.debug_level;
		break;
	case SCSC_MIN_IDLE_CPU:
		{
			double *dval = (double *) val;
			if (dval) {
				*dval = switch_core_min_idle_cpu(*dval);
			}
			intval = NULL;
		}
		break;
	case SCSC_MAX_SESSIONS:
		newintval = switch_core_session_limit(oldintval);
		break;
	case SCSC_LAST_SPS:
		newintval = runtime.sps_last;
		break;
	case SCSC_MAX_DTMF_DURATION:
		newintval = switch_core_max_dtmf_duration(oldintval);
		break;
	case SCSC_MIN_DTMF_DURATION:
		newintval = switch_core_min_dtmf_duration(oldintval);
		break;
	case SCSC_DEFAULT_DTMF_DURATION:
		newintval = switch_core_default_dtmf_duration(oldintval);
		break;
	case SCSC_SPS:
		switch_mutex_lock(runtime.throttle_mutex);
		if (oldintval > 0) {
			runtime.sps_total = oldintval;
		}
		newintval = runtime.sps_total;
		switch_mutex_unlock(runtime.throttle_mutex);
		break;

	case SCSC_RECLAIM:
		switch_core_memory_reclaim_all();
		newintval = 0;
		break;
	}

	if (intval) {
		*intval = newintval;
	}


	return 0;
}

SWITCH_DECLARE(switch_core_flag_t) switch_core_flags(void)
{
	return runtime.flags;
}

SWITCH_DECLARE(switch_bool_t) switch_core_ready(void)
{
	return (switch_test_flag((&runtime), SCF_SHUTTING_DOWN) || switch_test_flag((&runtime), SCF_NO_NEW_SESSIONS)) ? SWITCH_FALSE : SWITCH_TRUE;
}

SWITCH_DECLARE(switch_status_t) switch_core_destroy(void)
{
	switch_event_t *event;

	if (switch_event_create(&event, SWITCH_EVENT_SHUTDOWN) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Event-Info", "System Shutting Down");
		switch_event_fire(&event);
	}

	switch_set_flag((&runtime), SCF_NO_NEW_SESSIONS);
	switch_set_flag((&runtime), SCF_SHUTTING_DOWN);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "End existing sessions\n");
	switch_core_session_hupall(SWITCH_CAUSE_SYSTEM_SHUTDOWN);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Clean up modules.\n");

	switch_loadable_module_shutdown();

	switch_ssl_destroy_ssl_locks();

	if (switch_test_flag((&runtime), SCF_USE_SQL)) {
		switch_core_sqldb_stop();
	}
	switch_scheduler_task_thread_stop();

	switch_rtp_shutdown();

	if (switch_test_flag((&runtime), SCF_USE_AUTO_NAT)) {
		switch_nat_shutdown();
	}
	switch_xml_destroy();

	switch_console_shutdown();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Closing Event Engine.\n");
	switch_event_shutdown();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Finalizing Shutdown.\n");
	switch_log_shutdown();

	switch_core_unset_variables();
	switch_core_memory_stop();

	if (runtime.console && runtime.console != stdout && runtime.console != stderr) {
		fclose(runtime.console);
		runtime.console = NULL;
	}

	switch_safe_free(SWITCH_GLOBAL_dirs.base_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.mod_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.conf_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.log_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.db_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.script_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.htdocs_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.grammar_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.storage_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.recordings_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.sounds_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.run_dir);
	switch_safe_free(SWITCH_GLOBAL_dirs.temp_dir);

	switch_event_destroy(&runtime.global_vars);
	switch_core_hash_destroy(&runtime.ptimes);
	switch_core_hash_destroy(&runtime.mime_types);

	if (IP_LIST.hash) {
		switch_core_hash_destroy(&IP_LIST.hash);
	}

	if (IP_LIST.pool) {
		switch_core_destroy_memory_pool(&IP_LIST.pool);
	}

	if (runtime.memory_pool) {
		apr_pool_destroy(runtime.memory_pool);
		apr_terminate();
	}

	return switch_test_flag((&runtime), SCF_RESTART) ? SWITCH_STATUS_RESTART : SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_core_chat_send(const char *name, const char *proto, const char *from, const char *to,
													  const char *subject, const char *body, const char *type, const char *hint)
{
	switch_chat_interface_t *ci;
	switch_status_t status;

	if (!name || !(ci = switch_loadable_module_get_chat_interface(name)) || !ci->chat_send) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid Chat Interface [%s]!\n", name);
		return SWITCH_STATUS_FALSE;
	}

	status = ci->chat_send(proto, from, to, subject, body, type, hint);

	UNPROTECT_INTERFACE(ci);

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_core_management_exec(char *relative_oid, switch_management_action_t action, char *data, switch_size_t datalen)
{
	const switch_management_interface_t *ptr;
	switch_status_t status = SWITCH_STATUS_FALSE;

	if ((ptr = switch_loadable_module_get_management_interface(relative_oid))) {
		status = ptr->management_function(relative_oid, action, data, datalen);
	}

	return status;
}

SWITCH_DECLARE(void) switch_core_memory_reclaim_all(void)
{
	switch_core_memory_reclaim_logger();
	switch_core_memory_reclaim_events();
	switch_core_memory_reclaim();
}


struct system_thread_handle {
	const char *cmd;
	switch_thread_cond_t *cond;
	switch_mutex_t *mutex;
	switch_memory_pool_t *pool;
	int ret;
};

static void *SWITCH_THREAD_FUNC system_thread(switch_thread_t *thread, void *obj)
{
	struct system_thread_handle *sth = (struct system_thread_handle *) obj;

#if 0							// if we are a luser we can never turn this back down, didn't we already set the stack size?
#if defined(HAVE_SETRLIMIT) && !defined(__FreeBSD__)
	struct rlimit rlim;

	rlim.rlim_cur = SWITCH_SYSTEM_THREAD_STACKSIZE;
	rlim.rlim_max = SWITCH_SYSTEM_THREAD_STACKSIZE;
	if (setrlimit(RLIMIT_STACK, &rlim) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Setting stack size failed! (%s)\n", strerror(errno));
	}
#endif
#endif

	sth->ret = system(sth->cmd);

#if 0
#if defined(HAVE_SETRLIMIT) && !defined(__FreeBSD__)
	rlim.rlim_cur = SWITCH_THREAD_STACKSIZE;
	rlim.rlim_max = SWITCH_SYSTEM_THREAD_STACKSIZE;
	if (setrlimit(RLIMIT_STACK, &rlim) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Setting stack size failed! (%s)\n", strerror(errno));
	}
#endif
#endif

	switch_mutex_lock(sth->mutex);
	switch_thread_cond_signal(sth->cond);
	switch_mutex_unlock(sth->mutex);

	switch_core_destroy_memory_pool(&sth->pool);

	return NULL;
}

SWITCH_DECLARE(int) switch_system(const char *cmd, switch_bool_t wait)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr;
	int ret = 0;
	struct system_thread_handle *sth;
	switch_memory_pool_t *pool;

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Pool Failure\n");
		return 1;
	}

	if (!(sth = switch_core_alloc(pool, sizeof(struct system_thread_handle)))) {
		switch_core_destroy_memory_pool(&pool);
		return 1;
	}

	sth->pool = pool;
	sth->cmd = switch_core_strdup(pool, cmd);

	switch_thread_cond_create(&sth->cond, sth->pool);
	switch_mutex_init(&sth->mutex, SWITCH_MUTEX_NESTED, sth->pool);
	switch_mutex_lock(sth->mutex);

	switch_threadattr_create(&thd_attr, sth->pool);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_SYSTEM_THREAD_STACKSIZE);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_thread_create(&thread, thd_attr, system_thread, sth, sth->pool);

	if (wait) {
		switch_thread_cond_wait(sth->cond, sth->mutex);
		ret = sth->ret;
	}
	switch_mutex_unlock(sth->mutex);

	return ret;
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
