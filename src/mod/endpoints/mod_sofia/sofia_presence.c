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
 * Ken Rice, Asteria Solutions Group, Inc <ken@asteriasgi.com>
 * Paul D. Tinsley <pdt at jackhammer.org>
 * Bret McDanel <trixter AT 0xdecafbad.com>
 *
 *
 * sofia_presence.c -- SOFIA SIP Endpoint (presence code)
 *
 */
#include "mod_sofia.h"
#include "switch_stun.h"

#define SUB_OVERLAP 300
struct state_helper {
	switch_hash_t *hash;
	sofia_profile_t *profile;
	switch_memory_pool_t *pool;
};


static int sofia_presence_mwi_callback(void *pArg, int argc, char **argv, char **columnNames);
static int sofia_presence_mwi_callback2(void *pArg, int argc, char **argv, char **columnNames);
static int sofia_presence_sub_reg_callback(void *pArg, int argc, char **argv, char **columnNames);
static int sofia_presence_resub_callback(void *pArg, int argc, char **argv, char **columnNames);
static int sofia_presence_sub_callback(void *pArg, int argc, char **argv, char **columnNames);
static int broadsoft_sla_gather_state_callback(void *pArg, int argc, char **argv, char **columnNames);
static int broadsoft_sla_notify_callback(void *pArg, int argc, char **argv, char **columnNames);
static void sync_sla(sofia_profile_t *profile, const char *to_user, const char *to_host, switch_bool_t clear, switch_bool_t unseize);

struct dialog_helper {
	char status[512];
	char rpid[512];
	char presence_id[1024];
};

struct resub_helper {
	sofia_profile_t *profile;
	switch_event_t *event;
	int rowcount;
	int noreg;
};

struct presence_helper {
	sofia_profile_t *profile;
	switch_event_t *event;
	switch_stream_handle_t stream;
	char last_uuid[512];
};

switch_status_t sofia_presence_chat_send(const char *proto, const char *from, const char *to, const char *subject,
										 const char *body, const char *type, const char *hint)
{
	char *prof = NULL, *user = NULL, *host = NULL;
	sofia_profile_t *profile = NULL;
	char *ffrom = NULL;
	nua_handle_t *msg_nh;
	char *contact = NULL;
	char *dup = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;
	const char *ct = "text/html";
	sofia_destination_t *dst = NULL;
	char *to_uri = NULL;
	switch_console_callback_match_t *list = NULL;
	switch_console_callback_match_node_t *m;
	char *remote_ip = NULL;
	char *user_via = NULL;
	char *contact_str = NULL;
	char *dup_dest = NULL;
	char *p = NULL;
	char *remote_host = NULL;

	if (!to) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing To: header.\n");
		goto end;
	}

	if (!zstr(type)) {
		ct = type;
	}

	dup = strdup(to);
	switch_assert(dup);
	prof = dup;

	/* Do we have a user of the form profile/user[@host]? */
	if ((user = strchr(prof, '/'))) {
		*user++ = '\0';
	} else {
		user = prof;
		prof = NULL;
	}
	
	if (!strncasecmp(user, "sip:", 4)) {
		to_uri = user;
	}

	if ((host = strchr(user, '@'))) {
		if (!to_uri) {
			*host++ = '\0';	
		} else {
			host++;
		}
		if (!prof)
			prof = host;
	}

	if (!prof || !(profile = sofia_glue_find_profile(prof))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "Chat proto [%s]\nfrom [%s]\nto [%s]\n%s\nInvalid Profile %s\n", proto, from, to,
						  body ? body : "[no body]", prof ? prof : "NULL");
		goto end;
	}

	if (zstr(host)) {
		host = profile->domain_name;
		if (zstr(host)) {
			host = prof;
		}
	}

	if (to_uri) {
		switch_console_push_match(&list, to_uri);
	}  else if (!(list = sofia_reg_find_reg_url_multi(profile, user, host))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't find registered user %s@%s\n", user, host);
		goto end;
	}
	
	if (!strcasecmp(proto, SOFIA_CHAT_PROTO)) {
		from = hint;
	} else {
		char *fp, *p = NULL;
		
		fp = strdup(from);

		if (!fp) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error!\n");
			goto end;
		}

		if ((p = strchr(fp, '@'))) {
			*p++ = '\0';
		}

		if (zstr(p)) {
			p = profile->domain_name;
			if (zstr(p)) {
				p = host;
			}
		}

		ffrom = switch_mprintf("\"%s\" <sip:%s+%s@%s>", fp, proto, fp, p);

		from = ffrom;
		switch_safe_free(fp);
	}

	for (m = list->head; m; m = m->next) {

		if (!(dst = sofia_glue_get_destination(m->val))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error!\n");
			break;
		}
	
		/* sofia_glue is running sofia_overcome_sip_uri_weakness we do not, not sure if it matters */

		if (dst->route_uri) {
			dup_dest = strdup(dst->route_uri);
		} else  {
			dup_dest = strdup(dst->to);
		}


		remote_host = strdup(dup_dest);
		if (!zstr(remote_host)) {
			switch_split_user_domain(remote_host, NULL, &remote_ip);
		}

		if (!zstr(remote_ip) && sofia_glue_check_nat(profile, remote_ip)) {
			char *ptr = NULL;
			//const char *transport_str = NULL;
			if ((ptr = sofia_glue_find_parameter(dst->contact, "transport="))) {
				sofia_transport_t transport = sofia_glue_str2transport(ptr);
				//transport_str = sofia_glue_transport2str(transport);
				switch (transport) {
				case SOFIA_TRANSPORT_TCP:
					contact_str = profile->tcp_public_contact;
					break;
				case SOFIA_TRANSPORT_TCP_TLS:
					contact_str = profile->tls_public_contact;
					break;
				default:
					contact_str = profile->public_url;
					break;
				}
				user_via = sofia_glue_create_external_via(NULL, profile, transport);
			} else {
				user_via = sofia_glue_create_external_via(NULL, profile, SOFIA_TRANSPORT_UDP);
				contact_str = profile->public_url;
			}
		} else {
			contact_str = profile->url;
		}

		status = SWITCH_STATUS_SUCCESS;

		if (dup_dest && (p = strstr(dup_dest, ";fs_"))) {
			*p = '\0';
		}
		
		/* if this cries, add contact here too, change the 1 to 0 and omit the safe_free */
		
		msg_nh = nua_handle(profile->nua, NULL,
							TAG_IF(dst->route_uri, NUTAG_PROXY(dst->route_uri)),
							TAG_IF(dst->route, SIPTAG_ROUTE_STR(dst->route)),
							SIPTAG_FROM_STR(from),
							TAG_IF(contact, NUTAG_URL(contact)),
							SIPTAG_TO_STR(dup_dest),
							SIPTAG_CONTACT_STR(contact_str),
							TAG_END());

		nua_handle_bind(msg_nh, &mod_sofia_globals.destroy_private);
		
		nua_message(msg_nh,
					TAG_IF(user_via, SIPTAG_VIA_STR(user_via)),
					SIPTAG_CONTENT_TYPE_STR(ct),
					SIPTAG_PAYLOAD_STR(body),
					TAG_END());

		sofia_glue_free_destination(dst);
		switch_safe_free(dup_dest);
		switch_safe_free(remote_host);
	}		
	
	switch_console_free_matches(&list);

  end:
	
	switch_safe_free(contact);
	switch_safe_free(ffrom);
	switch_safe_free(dup);

	if (profile) {
		switch_thread_rwlock_unlock(profile->rwlock);
	}

	return status;
}

void sofia_presence_cancel(void)
{
	char *sql;
	sofia_profile_t *profile;
	struct presence_helper helper = { 0 };
	switch_console_callback_match_t *matches;

	if (!mod_sofia_globals.profile_hash) {
		return;
	}
	
	if (list_profiles_full(NULL, NULL, &matches, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
		switch_console_callback_match_node_t *m;
		
		sql = switch_mprintf("select proto,sip_user,sip_host,sub_to_user,sub_to_host,event,contact,call_id,full_from,"
							 "full_via,expires,user_agent,accept,profile_name,network_ip"
							 ",-1,'unavailable','unavailable' from sip_subscriptions where version > -1 and "
							 "expires > -1 and event='presence' and hostname='%q'",
							 mod_sofia_globals.hostname);
	

		for (m = matches->head; m; m = m->next) {
			if ((profile = sofia_glue_find_profile(m->val))) {
				if (profile->pres_type == PRES_TYPE_FULL) {
					helper.profile = profile;
					helper.event = NULL;
					if (sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_sub_callback, &helper) != SWITCH_TRUE) {
						sofia_glue_release_profile(profile);
						continue;
					}
				}
				sofia_glue_release_profile(profile);
			}
		}

		switch_safe_free(sql);
		switch_console_free_matches(&matches);

	}
}

void sofia_presence_establish_presence(sofia_profile_t *profile)
{
	struct resub_helper h = { 0 };
	h.profile = profile;

	if (sofia_glue_execute_sql_callback(profile, profile->ireg_mutex,
										"select sip_user,sip_host,'Registered','unknown','' from sip_registrations",
										sofia_presence_resub_callback, &h) != SWITCH_TRUE) {
		return;
	}

	if (sofia_glue_execute_sql_callback(profile, profile->ireg_mutex,
										"select sub_to_user,sub_to_host,'Online','unknown',proto from sip_subscriptions "
										"where expires > -1 and version > -1 and proto='ext' or proto='user' or proto='conf'",
										sofia_presence_resub_callback, &h) != SWITCH_TRUE) {
		return;
	}
}

char *sofia_presence_translate_rpid(char *in, char *ext)
{
	char *r = in;

	if (in && (switch_stristr("null", in))) {
		in = NULL;
	}

	if (!in) {
		in = ext;
	}

	if (!in) {
		return NULL;
	}

	if (!strcasecmp(in, "dnd") || !strcasecmp(in, "idle")) {
		r = "busy";
	}

	return r;
}

struct mwi_helper {
	sofia_profile_t *profile;
	int total;
};

static void actual_sofia_presence_mwi_event_handler(switch_event_t *event)
{
	char *account, *dup_account, *yn, *host, *user;
	char *sql;
	sofia_profile_t *profile = NULL;
	switch_stream_handle_t stream = { 0 };
	switch_event_header_t *hp;
	struct mwi_helper h = { 0 };
	const char *pname = NULL;
	const char *call_id;
	const char *sub_call_id;
	int for_everyone = 0;

	switch_assert(event != NULL);

	if (!(account = switch_event_get_header(event, "mwi-message-account"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing required Header 'MWI-Message-Account'\n");
		return;
	}

	if (!(yn = switch_event_get_header(event, "mwi-messages-waiting"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing required Header 'MWI-Messages-Waiting'\n");
		return;
	}

	call_id = switch_event_get_header(event, "call-id");
	sub_call_id = switch_event_get_header(event, "sub-call-id");

	if (!call_id && !sub_call_id) {
		for_everyone = 1;
	}


	dup_account = strdup(account);
	switch_assert(dup_account != NULL);
	switch_split_user_domain(dup_account, &user, &host);


	if ((pname = switch_event_get_header(event, "sofia-profile"))) {
		profile = sofia_glue_find_profile(pname);
	}

	if (!profile) {
		if (!host || !(profile = sofia_glue_find_profile(host))) {
			char *sql;
			char buf[512] = "";
			switch_console_callback_match_t *matches;

			sql = switch_mprintf("select profile_name from sip_registrations where sip_host='%s' or mwi_host='%s'", host, host);

			if (list_profiles_full(NULL, NULL, &matches, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
				switch_console_callback_match_node_t *m;

				for (m = matches->head; m; m = m->next) {
					if ((profile = sofia_glue_find_profile(m->val))) {

						sofia_glue_execute_sql2str(profile, profile->ireg_mutex, sql, buf, sizeof(buf));
						if (!zstr(buf)) {
							break;
						}
						sofia_glue_release_profile(profile);
					}
				}

				switch_console_free_matches(&matches);
			}

			

			if (!(profile = sofia_glue_find_profile(buf))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot find profile %s\n", switch_str_nil(host));
				switch_safe_free(dup_account);
				return;
			}
		}
	}

	if (profile->domain_name && strcasecmp(profile->domain_name, host)) {
		host = profile->domain_name;
	}

	h.profile = profile;
	h.total = 0;

	SWITCH_STANDARD_STREAM(stream);

	for (hp = event->headers; hp; hp = hp->next) {
		if (!strncasecmp(hp->name, "mwi-", 4)) {
			char *tmp = NULL;
			char *value = hp->value;
			if (!strcasecmp(hp->name, "mwi-message-account") && strncasecmp(hp->value, "sip:", 4)) {
				tmp = switch_mprintf("sip:%s", hp->value);
				value = tmp;
			}
			stream.write_function(&stream, "%s: %s\r\n", hp->name + 4, value);
			switch_safe_free(tmp);
		}
	}

	stream.write_function(&stream, "\r\n");

	sql = NULL;

	if (for_everyone) {
		sql = switch_mprintf("select proto,sip_user,sip_host,sub_to_user,sub_to_host,event,contact,call_id,full_from,"
							 "full_via,expires,user_agent,accept,profile_name,network_ip"
							 ",'%q','%q' from sip_subscriptions where version > -1 and expires > -1 and event='message-summary' "
							 "and sub_to_user='%q' and (sub_to_host='%q' or presence_hosts like '%%%q%%')", stream.data, host, user, host, host);
	} else if (sub_call_id) {
		sql = switch_mprintf("select proto,sip_user,sip_host,sub_to_user,sub_to_host,event,contact,call_id,full_from,"
							 "full_via,expires,user_agent,accept,profile_name,network_ip"
							 ",'%q','%q' from sip_subscriptions where version > -1 and expires > -1 and event='message-summary' "
							 "and sub_to_user='%q' and (sub_to_host='%q' or presence_hosts like '%%%q%%' and call_id='%q')",
							 stream.data, host, user, host, host, sub_call_id);
	}


	if (sql) {
		sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_mwi_callback, &h);
		free(sql);
		sql = NULL;

	}

	if (for_everyone) {
		sql = switch_mprintf("select sip_user,sip_host,contact,profile_name,network_ip,'%q' "
							 "from sip_registrations where mwi_user='%q' and mwi_host='%q'", stream.data, user, host);
	} else if (call_id) {
		sql = switch_mprintf("select sip_user,sip_host,contact,profile_name,network_ip,'%q' "
							 "from sip_registrations where mwi_user='%q' and mwi_host='%q' and call_id='%q'", stream.data, user, host, call_id);
	}

	if (sql) {
		switch_assert(sql != NULL);
		sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_mwi_callback2, &h);
		free(sql);
		sql = NULL;
	}

	switch_safe_free(stream.data);
	switch_safe_free(dup_account);

	if (profile) {
		sofia_glue_release_profile(profile);
	}
}

static int sofia_presence_dialog_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	struct dialog_helper *helper = (struct dialog_helper *) pArg;

	if (argc == 3) {
		switch_set_string(helper->status, argv[0]);
		switch_set_string(helper->rpid, argv[1]);
		switch_set_string(helper->presence_id, argv[2]);
	}

	return -1;
}

static void actual_sofia_presence_event_handler(switch_event_t *event)
{
	sofia_profile_t *profile = NULL;
	char *from = switch_event_get_header(event, "from");
	char *proto = switch_event_get_header(event, "proto");
	char *rpid = switch_event_get_header(event, "rpid");
	char *status = switch_event_get_header(event, "status");
	char *event_type = switch_event_get_header(event, "event_type");
	char *alt_event_type = switch_event_get_header(event, "alt_event_type");
	//char *event_subtype = switch_event_get_header(event, "event_subtype");
	char *sql = NULL;
	char *euser = NULL, *user = NULL, *host = NULL;
	char *call_info = switch_event_get_header(event, "presence-call-info");
	char *call_info_state = switch_event_get_header(event, "presence-call-info-state");
	struct resub_helper h = { 0 };
	switch_console_callback_match_t *matches;

	if (!mod_sofia_globals.running) {
		return;
	}

	if (rpid && !strcasecmp(rpid, "n/a")) {
		rpid = NULL;
	}

	if (status && !strcasecmp(status, "n/a")) {
		status = NULL;
	}

	if (status && switch_stristr("CS_HANGUP", status)) {
		status = "Available";
	}

	if (rpid) {
		rpid = sofia_presence_translate_rpid(rpid, status);
	}

	if (event->event_id == SWITCH_EVENT_ROSTER) {
		struct presence_helper helper = { 0 };
		
		if (!mod_sofia_globals.profile_hash)
			return;

		if (from) {
			sql = switch_mprintf("select sip_subscriptions.proto,sip_subscriptions.sip_user,sip_subscriptions.sip_host,"
								 "sip_subscriptions.sub_to_user,sip_subscriptions.sub_to_host,sip_subscriptions.event,"
								 "sip_subscriptions.contact,sip_subscriptions.call_id,sip_subscriptions.full_from,"
								 "sip_subscriptions.full_via,sip_subscriptions.expires,sip_subscriptions.user_agent,"
								 "sip_subscriptions.accept,sip_subscriptions.profile_name,sip_subscriptions.network_ip"
								 ",1,'%q','%q',sip_presence.status,sip_presence.rpid,sip_presence.open_closed "
								 "from sip_subscriptions left join sip_presence on "
								 "(sip_subscriptions.sub_to_user=sip_presence.sip_user and sip_subscriptions.sub_to_host=sip_presence.sip_host and "
								 "sip_subscriptions.profile_name=sip_presence.profile_name) "
								 "where sip_subscriptions.version > -1 and "
								 "sip_subscriptions.expires > -1 and sip_subscriptions.event='presence' and sip_subscriptions.full_from like '%%%q%%'",
								 switch_str_nil(status), switch_str_nil(rpid), from);
		} else {
			sql = switch_mprintf("select sip_subscriptions.proto,sip_subscriptions.sip_user,sip_subscriptions.sip_host,"
								 "sip_subscriptions.sub_to_user,sip_subscriptions.sub_to_host,sip_subscriptions.event,"
								 "sip_subscriptions.contact,sip_subscriptions.call_id,sip_subscriptions.full_from,"
								 "sip_subscriptions.full_via,sip_subscriptions.expires,sip_subscriptions.user_agent,"
								 "sip_subscriptions.accept,sip_subscriptions.profile_name,sip_subscriptions.network_ip"
								 ",1,'%q','%q',sip_presence.status,sip_presence.rpid,sip_presence.open_closed "
								 "from sip_subscriptions left join sip_presence on "
								 "(sip_subscriptions.sub_to_user=sip_presence.sip_user and sip_subscriptions.sub_to_host=sip_presence.sip_host and "
								 "sip_subscriptions.profile_name=sip_presence.profile_name) "
								 "where sip_subscriptions.version > -1 and "
								 "sip_subscriptions.expires > -1 and sip_subscriptions.event='presence'", switch_str_nil(status),
								 switch_str_nil(rpid));
		}

		switch_assert(sql != NULL);
		
		if (list_profiles_full(NULL, NULL, &matches, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
			switch_console_callback_match_node_t *m;

			for (m = matches->head; m; m = m->next) {
				if ((profile = sofia_glue_find_profile(m->val))) {
					if (profile->pres_type != PRES_TYPE_FULL) {
						if (mod_sofia_globals.debug_presence > 0) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "%s is passive, skipping\n", (char *) profile->name);
						}
						sofia_glue_release_profile(profile);
						continue;
					}
					helper.profile = profile;
					helper.event = NULL;
					sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_sub_callback, &helper);
					sofia_glue_release_profile(profile);
				}
			}
			switch_console_free_matches(&matches);
		}
		
		free(sql);
		return;
	}

	if (zstr(event_type)) {
		event_type = "presence";
	}

	if (zstr(alt_event_type)) {
		if (!strcasecmp(event_type, "presence")) {
			alt_event_type = "dialog";
		} else {
			alt_event_type = "presence";
		}
	}

	if (from && (user = strdup(from))) {
		if ((host = strchr(user, '@'))) {
			char *p;
			*host++ = '\0';
			if ((p = strchr(host, '/'))) {
				*p = '\0';
			}
		} else {
			switch_safe_free(user);
			return;
		}
		if ((euser = strchr(user, '+'))) {
			euser++;
		} else {
			euser = user;
		}
	} else {
		return;
	}

	switch (event->event_id) {
	case SWITCH_EVENT_PRESENCE_PROBE:
		if (proto) {
			char *to = switch_event_get_header(event, "to");
			char *probe_user = NULL, *probe_euser, *probe_host, *p;
			struct dialog_helper dh = { { 0 } };

			if (strcasecmp(proto, SOFIA_CHAT_PROTO) != 0) {
				goto done;
			}


			if (!to || !(probe_user = strdup(to))) {
				goto done;
			}

			if ((probe_host = strchr(probe_user, '@'))) {
				*probe_host++ = '\0';
			}
			probe_euser = probe_user;
			if ((p = strchr(probe_euser, '+'))) {
				probe_euser = (p + 1);
			}

			if (probe_euser && probe_host && (profile = sofia_glue_find_profile(probe_host))) {
				sql = switch_mprintf("select status,rpid,presence_id from sip_dialogs "
									 "where ((sip_from_user='%q' and sip_from_host='%q') or presence_id='%q@%q')", 
									 probe_euser, probe_host, probe_euser, probe_host);
				sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_dialog_callback, &dh);

				h.profile = profile;

				switch_safe_free(sql);

				sql = switch_mprintf("select sip_registrations.sip_user, "
									 "sip_registrations.sip_host, "
									 "sip_registrations.status, "
									 "sip_registrations.rpid, "
									 "'', "
									 "sip_dialogs.uuid, "
									 "sip_dialogs.state, "
									 "sip_dialogs.direction, "
									 "sip_dialogs.sip_to_user, "
									 "sip_dialogs.sip_to_host, "

									 "sip_presence.status,"
									 "sip_presence.rpid,"
									 "sip_dialogs.presence_id, "
									 "sip_presence.open_closed,"
									 "'%q','%q' "
									 "from sip_registrations "

									 "left join sip_dialogs on "
									 "sip_dialogs.presence_id = sip_registrations.sip_user || '@' || sip_registrations.sip_host "
									 "or (sip_dialogs.sip_from_user = sip_registrations.sip_user "
									 "and sip_dialogs.sip_from_host = sip_registrations.sip_host) "
 
									 "left join sip_presence on "
									 "(sip_registrations.sip_user=sip_presence.sip_user and sip_registrations.orig_server_host=sip_presence.sip_host and "
									 "sip_registrations.profile_name=sip_presence.profile_name) "
									 "where sip_dialogs.presence_id='%q@%q' or (sip_registrations.sip_user='%q' and "
									 "(sip_registrations.orig_server_host='%q' or sip_registrations.sip_host='%q' "
									 "or sip_registrations.presence_hosts like '%%%q%%'))",
									 dh.status, dh.rpid, 
									 probe_euser, probe_host,  probe_euser, probe_host, probe_host, probe_host);
				switch_assert(sql);
				
				if (mod_sofia_globals.debug_presence > 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s START_PRESENCE_PROBE_SQL\n", profile->name);
				}

				if (mod_sofia_globals.debug_presence > 1) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s DUMP PRESENCE_PROBE_SQL:\n%s\n", profile->name, sql);
				}

				sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_resub_callback, &h);


				if (!h.rowcount) {
					h.noreg++;
					switch_safe_free(sql);
					/* find ones with presence_id defined that are not registred */
					sql = switch_mprintf("select sip_from_user, sip_from_host, 'Registered', '', '', "
										 "uuid, state, direction, "
										 "sip_to_user, sip_to_host,"
										 "'%q','%q',presence_id, '','','' "
										 
										 "from sip_dialogs "
										 
										 "where (presence_id='%q@%q' or "
										 "(sip_from_user='%q' and (sip_from_host='%q' or sip_to_host='%q')))",
										 
										 dh.status, dh.rpid, probe_euser, probe_host,  probe_euser, probe_host, probe_host);

					if (mod_sofia_globals.debug_presence > 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s START_PRESENCE_PROBE_SQL\n", profile->name);
					}

					if (mod_sofia_globals.debug_presence > 1) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s DUMP PRESENCE_PROBE_SQL:\n%s\n", profile->name, sql);
					}

					sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_resub_callback, &h);

					if (mod_sofia_globals.debug_presence > 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s END_PRESENCE_PROBE_SQL\n\n", profile->name);
					}
				}

				if (!h.rowcount) {
					switch_event_t *sevent;
					if (switch_event_create(&sevent, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
						switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "proto", SOFIA_CHAT_PROTO);
						switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "login", profile->name);
						switch_event_add_header(sevent, SWITCH_STACK_BOTTOM, "from", "%s@%s", probe_euser, probe_host);
						switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "status", "Unregistered");
						switch_event_fire(&sevent);
					}					
				}


				sofia_glue_release_profile(profile);
				switch_safe_free(sql);
			}
						 

			switch_safe_free(probe_user);
		}
		goto done;
	default:
		break;
	}



	if (!mod_sofia_globals.profile_hash) {
		goto done;
	}

	if (list_profiles_full(NULL, NULL, &matches, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
		switch_console_callback_match_node_t *m;

		for (m = matches->head; m; m = m->next) {
			struct dialog_helper dh = { { 0 } };

			if ((profile = sofia_glue_find_profile(m->val))) {
				if (profile->pres_type != PRES_TYPE_FULL) {
					if (mod_sofia_globals.debug_presence > 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "%s is passive, skipping\n", (char *) profile->name);
					}
					sofia_glue_release_profile(profile);
					continue;
				}

				if (call_info) {
					const char *uuid = switch_event_get_header(event, "unique-id");

					
#if 0
					if (mod_sofia_globals.debug_sla > 1) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SLA EVENT:\n");
						DUMP_EVENT(event);
					}
#endif

					if (uuid) {
						sql = switch_mprintf("update sip_dialogs set call_info='%q',call_info_state='%q' where hostname='%q' and uuid='%q'",
											 call_info, call_info_state, mod_sofia_globals.hostname, uuid);
					} else {
						sql = switch_mprintf("update sip_dialogs set call_info='%q', call_info_state='%q' where hostname='%q' and "
											 "((sip_dialogs.sip_from_user='%q' and sip_dialogs.sip_from_host='%q') or presence_id='%q@%q') and call_info='%q'",
											 
											 call_info, call_info_state, mod_sofia_globals.hostname, euser, host, euser, host, call_info);
						
					}
					
					if (mod_sofia_globals.debug_sla > 1) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "STATE SQL %s\n", sql);
					}
					sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
					


					if (mod_sofia_globals.debug_sla > 1) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PROCESS PRESENCE EVENT\n");
					}
					
					sync_sla(profile, euser, host, SWITCH_TRUE, SWITCH_TRUE);
				}
				
				if (!strcmp(proto, "dp")) {
					sql = switch_mprintf("update sip_presence set rpid='%q',status='%q' where sip_user='%q' and sip_host='%q'",
										 rpid, status, euser, host);
					sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
				}
				
				sql = switch_mprintf("select status,rpid,presence_id from sip_dialogs where ((sip_from_user='%q' and sip_from_host='%q') or presence_id='%q@%q')", 
									 euser, host, euser, host);
				sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_dialog_callback, &dh);
				switch_safe_free(sql);
				
				if ((sql = switch_mprintf("select distinct sip_subscriptions.proto,sip_subscriptions.sip_user,sip_subscriptions.sip_host,"
										  "sip_subscriptions.sub_to_user,sip_subscriptions.sub_to_host,sip_subscriptions.event,"
										  "sip_subscriptions.contact,sip_subscriptions.call_id,sip_subscriptions.full_from,"
										  "sip_subscriptions.full_via,sip_subscriptions.expires,sip_subscriptions.user_agent,"
										  "sip_subscriptions.accept,sip_subscriptions.profile_name"
										  ",'%q','%q','%q',sip_presence.status,sip_presence.rpid,sip_presence.open_closed,'%q','%q',"
										  "sip_subscriptions.version, '%q' "
										  "from sip_subscriptions "
										  "left join sip_presence on "
										  "(sip_subscriptions.sub_to_user=sip_presence.sip_user and sip_subscriptions.sub_to_host=sip_presence.sip_host and "
										  "sip_subscriptions.profile_name=sip_presence.profile_name) "
										  
										  "where sip_subscriptions.version > -1 and sip_subscriptions.expires > -1 and "
										  "(event='%q' or event='%q') and sub_to_user='%q' "
										  "and (sub_to_host='%q' or presence_hosts like '%%%q%%') "
										  "and (sip_subscriptions.profile_name = '%q' or sip_subscriptions.presence_hosts != sip_subscriptions.sub_to_host) ",
										  
										  switch_str_nil(status), switch_str_nil(rpid), host,
										  dh.status,dh.rpid,dh.presence_id,
										  event_type, alt_event_type, euser, host, host, profile->name))) {
					
					struct presence_helper helper = { 0 };			

					helper.profile = profile;
					helper.event = event;
					SWITCH_STANDARD_STREAM(helper.stream);
					switch_assert(helper.stream.data);
					
					if (mod_sofia_globals.debug_presence > 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s START_PRESENCE_SQL (%s)\n",
										  event->event_id == SWITCH_EVENT_PRESENCE_IN ? "IN" : "OUT", profile->name);
					}

					if (mod_sofia_globals.debug_presence) {
						char *buf;
						switch_event_serialize(event, &buf, SWITCH_FALSE);
						switch_assert(buf);
						if (mod_sofia_globals.debug_presence > 1) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "DUMP PRESENCE SQL:\n%s\nEVENT DUMP:\n%s\n", sql, buf);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "EVENT DUMP:\n%s\n", buf);
						}
						free(buf);
					}

					sofia_glue_execute_sql_callback(profile, NULL, sql, sofia_presence_sub_callback, &helper);
					switch_safe_free(sql);
					
					sql = switch_mprintf("update sip_subscriptions set version=version+1 where event='dialog' and sub_to_user='%q' "
										 "and (sub_to_host='%q' or presence_hosts like '%%%q%%') "
										 "and (profile_name = '%q' or presence_hosts != sub_to_host)",
										 euser, host, host, profile->name);
					
					sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
					
					
					if (mod_sofia_globals.debug_presence > 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s END_PRESENCE_SQL (%s)\n",
										  event->event_id == SWITCH_EVENT_PRESENCE_IN ? "IN" : "OUT", profile->name);
					}

					if (!zstr((char *) helper.stream.data)) {
						char *this_sql = (char *) helper.stream.data;
						char *next = NULL;
						char *last = NULL;
						
						do {
							if ((next = strchr(this_sql, ';'))) {
								*next++ = '\0';
								while (*next == '\n' || *next == ' ' || *next == '\r') {
									*next++ = '\0';
								}
							}
							
							if (!zstr(this_sql) && (!last || strcmp(last, this_sql))) {
								sofia_glue_execute_sql(profile, &this_sql, SWITCH_FALSE);
								last = this_sql;
							}
							this_sql = next;
						} while (this_sql);
					}
					switch_safe_free(helper.stream.data);
					helper.stream.data = NULL;
				}
				sofia_glue_release_profile(profile);
			}
		}
		switch_console_free_matches(&matches);
	}

  done:
	switch_safe_free(sql);
	switch_safe_free(user);
}

static int EVENT_THREAD_RUNNING = 0;
static int EVENT_THREAD_STARTED = 0;

void *SWITCH_THREAD_FUNC sofia_presence_event_thread_run(switch_thread_t *thread, void *obj)
{
	void *pop;
	int done = 0;

	switch_mutex_lock(mod_sofia_globals.mutex);
	if (!EVENT_THREAD_RUNNING) {
		EVENT_THREAD_RUNNING++;
		mod_sofia_globals.threads++;
	} else {
		done = 1;
	}
	switch_mutex_unlock(mod_sofia_globals.mutex);

	if (done) {
		return NULL;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Event Thread Started\n");

	while (mod_sofia_globals.running == 1) {
		int count = 0;

		if (switch_queue_trypop(mod_sofia_globals.presence_queue, &pop) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *event = (switch_event_t *) pop;

			if (!pop) {
				break;
			}
			actual_sofia_presence_event_handler(event);
			switch_event_destroy(&event);
			count++;
		}

		if (switch_queue_trypop(mod_sofia_globals.mwi_queue, &pop) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *event = (switch_event_t *) pop;

			if (!pop) {
				break;
			}

			actual_sofia_presence_mwi_event_handler(event);
			switch_event_destroy(&event);
			count++;
		}

		if (!count) {
			switch_yield(100000);
		}
	}

	while (switch_queue_trypop(mod_sofia_globals.presence_queue, &pop) == SWITCH_STATUS_SUCCESS && pop) {
		switch_event_t *event = (switch_event_t *) pop;
		switch_event_destroy(&event);
	}

	while (switch_queue_trypop(mod_sofia_globals.mwi_queue, &pop) == SWITCH_STATUS_SUCCESS && pop) {
		switch_event_t *event = (switch_event_t *) pop;
		switch_event_destroy(&event);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Event Thread Ended\n");

	switch_mutex_lock(mod_sofia_globals.mutex);
	mod_sofia_globals.threads--;
	EVENT_THREAD_RUNNING = EVENT_THREAD_STARTED = 0;
	switch_mutex_unlock(mod_sofia_globals.mutex);

	return NULL;
}

void sofia_presence_event_thread_start(void)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
	int done = 0;

	switch_mutex_lock(mod_sofia_globals.mutex);
	if (!EVENT_THREAD_STARTED) {
		EVENT_THREAD_STARTED++;
	} else {
		done = 1;
	}
	switch_mutex_unlock(mod_sofia_globals.mutex);

	if (done) {
		return;
	}

	switch_threadattr_create(&thd_attr, mod_sofia_globals.pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_threadattr_priority_increase(thd_attr);
	switch_thread_create(&thread, thd_attr, sofia_presence_event_thread_run, NULL, mod_sofia_globals.pool);
}


void sofia_presence_event_handler(switch_event_t *event)
{
	switch_event_t *cloned_event;

	switch_event_dup(&cloned_event, event);
	switch_assert(cloned_event);
	switch_queue_push(mod_sofia_globals.presence_queue, cloned_event);

	if (!EVENT_THREAD_STARTED) {
		sofia_presence_event_thread_start();
	}
}

void sofia_presence_mwi_event_handler(switch_event_t *event)
{
	switch_event_t *cloned_event;

	switch_event_dup(&cloned_event, event);
	switch_assert(cloned_event);
	switch_queue_push(mod_sofia_globals.mwi_queue, cloned_event);

	if (!EVENT_THREAD_STARTED) {
		sofia_presence_event_thread_start();
	}
}


static int sofia_presence_sub_reg_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	sofia_profile_t *profile = (sofia_profile_t *) pArg;
	char *user = argv[1];
	char *host = argv[2];
	switch_event_t *event;
	char *event_name = argv[5];
	char *expires = argv[10];

	if (!strcasecmp(event_name, "message-summary")) {
		if (switch_event_create(&event, SWITCH_EVENT_MESSAGE_QUERY) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Message-Account", "sip:%s@%s", user, host);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "VM-Sofia-Profile", profile->name);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "VM-sub-call-id", argv[7]);
			switch_event_fire(&event);
		}
		return 0;
	}

	if (switch_event_create(&event, SWITCH_EVENT_PRESENCE_PROBE) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", SOFIA_CHAT_PROTO);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", profile->url);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "from", "%s@%s", user, host);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_subtype", "probe");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto-specific-event-name", event_name);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "expires", expires);
		switch_event_fire(&event);
	}

	return 0;
}

static int sofia_presence_resub_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	struct resub_helper *h = (struct resub_helper *) pArg;
	sofia_profile_t *profile = h->profile;
	char *user = argv[0];
	char *host = argv[1];
	char *status = argv[2];
	char *rpid = argv[3];
	char *proto = argv[4];

	char *presence_id = NULL;
	char *to_user = NULL;
	char *uuid = NULL;
	char *state = NULL;
	char *direction = NULL;
	switch_event_t *event;
	char to_buf[128] = "";
	switch_event_header_t *hp;
	char *free_me = NULL;

	if (argc > 5) {
		uuid = argv[5];
		state = switch_str_nil(argv[6]);
		direction = switch_str_nil(argv[7]);
		if (argc > 8) {
			switch_set_string(to_buf, argv[8]);
			switch_url_decode(to_buf);
			to_user = to_buf;
		}
		if (argc > 10 && !zstr(argv[10]) && !zstr(argv[11])) {
			status = argv[10];
			rpid = argv[11];
		}

		if (argc > 12 && !zstr(argv[12]) && strchr(argv[12], '@')) {
			char *p;

			presence_id = argv[12];
			free_me = strdup(presence_id);
			if ((p = strchr(free_me, '@'))) *p = '\0';
			user = free_me;
		}
	}

	if (zstr(proto)) {
		proto = NULL;
	}

	if (mod_sofia_globals.debug_presence > 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "%s PRESENCE_PROBE %s@%s\n", profile->name, user, host);
	}

	if (switch_event_create(&event, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", proto ? proto : SOFIA_CHAT_PROTO);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", profile->url);

		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "from", "%s@%s", user, host);

		if (h->noreg) {
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Force-Direction", "inbound");
		}

		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "status", status);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "rpid", rpid);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", "presence");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "event_count", "%d", 0);

		if (!zstr(to_user)) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "to-user", to_user);
		}

		if (zstr(state)) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "channel-state", "CS_HANGUP");
			//switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "answer-state", "resubscribe");
		} else {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "channel-state", "CS_ROUTING");
			if (uuid) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique-id", uuid);
			}
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "answer-state", state);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "astate", state);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "presence-call-direction", direction);
		}

		if (h->event) {
			for (hp = h->event->headers; hp; hp = hp->next) {
				if (!strncasecmp(hp->name, "fwd-", 4)) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, hp->name + 4, hp->value);
				}
			}
		}


		switch_event_fire(&event);
	}

	switch_safe_free(free_me);

	h->rowcount++;
	return 0;
}

static char *translate_rpid(char *in)
{
	char *r = in;

	if (in && (strstr(in, "null") || strstr(in, "NULL"))) {
		in = NULL;
	}

	if (zstr(in)) {
		return NULL;
	}

	if (!strcasecmp(in, "unknown")) {
		r = NULL;
		goto end;
	}

	if (!strcasecmp(in, "busy")) {
		r = in;
		goto end;
	}

	if (!strcasecmp(in, "unavailable")) {
		r = "away";
		goto end;
	}

	if (!strcasecmp(in, "idle")) {
		r = "busy";
	}

  end:
	return r;
}


static char *gen_pidf(char *user_agent, char *id, char *url, char *open, char *rpid, char *prpid, char *status, const char **ct)
{
	char *ret = NULL;

	if (switch_stristr("polycom", user_agent)) {
		*ct = "application/xpidf+xml";

		/* If unknown/none prpid is provided, just show the user as online. */
		if (!prpid || !strcasecmp(prpid, "unknown")) {
			prpid = "online";
		}

		/* of course!, lets make a big deal over dashes. Now the stupidity is complete. */
		if (!strcmp(prpid, "on-the-phone")) {
			prpid = "onthephone";
		}

		ret = switch_mprintf("<?xml version=\"1.0\"?>\n"
							 "<!DOCTYPE presence PUBLIC \"-//IETF//DTD RFCxxxx XPIDF 1.0//EN\" \"xpidf.dtd\">\n"
							 "<presence>\n"
							 " <status>\n"
							 "  <note>%s</note>\n"
							 " </status>\n"
							 " <presentity uri=\"%s;method=SUBSCRIBE\" />\n"
							 " <atom id=\"%s\">\n"
							 "  <address uri=\"%s;user=ip\" priority=\"0.800000\">\n"
							 "   <status status=\"%s\" />\n"
							 "   <msnsubstatus substatus=\"%s\" />\n" 
							 "  </address>\n" 
							 " </atom>\n" 
							 "</presence>\n", status, id, id, url, open, prpid);
	} else {
		char *xml_rpid = NULL;
		
		*ct = "application/pidf+xml";

		if (!strcasecmp(open, "closed")) {
			status = "Unregistered";
			prpid = NULL;
		}

		if (!strncasecmp(status, "Registered", 10)) {
			status = "Available";
		}
		
		if (!strcasecmp(status, "Available")) {
			prpid = NULL;
		}


		if (!strcasecmp(status, "Unregistered")) {
			prpid = NULL;
			open = "closed";
		}

		if (zstr(rpid)) {
			prpid = NULL;
		}


		if (zstr(status) && !zstr(prpid)) {
			status = "Available";
			prpid = NULL;
		}
		
		if (prpid) {
			xml_rpid = switch_mprintf("  <rpid:activities>\r\n"
									  "   <rpid:%s/>\n"
									  "  </rpid:activities>\n", prpid);
		}

		ret = switch_mprintf("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?> \n"
							 "<presence xmlns='urn:ietf:params:xml:ns:pidf' \n"
							 "xmlns:dm='urn:ietf:params:xml:ns:pidf:data-model' \n"
							 "xmlns:rpid='urn:ietf:params:xml:ns:pidf:rpid' \n"
							 "xmlns:c='urn:ietf:params:xml:ns:pidf:cipid' entity='%s'>\n"
							 " <tuple id='t6a5ed77e'>\n"
							 "  <status>\r\n"
							 "   <basic>%s</basic>\n"
							 "  </status>\n"
							 " </tuple>\n"
							 " <dm:person id='p06360c4a'>\n"
							 "%s"
							 "  <dm:note>%s</dm:note>\n"
							 " </dm:person>\n" 
							 "</presence>", id, open, switch_str_nil(xml_rpid), status);
		

		switch_safe_free(xml_rpid);
	}


	return ret;
}



static int sofia_presence_sub_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	struct presence_helper *helper = (struct presence_helper *) pArg;
	char *pl = NULL;
	char *clean_id = NULL, *id = NULL;
	char *proto = argv[0];
	char *user = argv[1];
	char *host = argv[2];
	char *sub_to_user = argv[3];
	char *event = argv[5];
	char *call_id = argv[7];
	char *expires = argv[10];
	char *user_agent = argv[11];
	char *profile_name = argv[13];
	uint32_t in = 0;
	char *status = argv[14];
	char *rpid = argv[15];
	char *sub_to_host = argv[16];
	char *open_closed = NULL;
	char *dialog_status = NULL;
	char *dialog_rpid = NULL;

	const char *ct = "no/idea";
	nua_handle_t *nh;
	char *to = NULL;
	char *open;
	char *prpid;
	time_t exptime = switch_epoch_time_now(NULL) + 3600;
	int is_dialog = 0;
	sofia_profile_t *ext_profile = NULL, *profile = helper->profile;
	char sstr[128] = "";
	int kill_handle = 0;
	char expires_str[10] = "";
	char status_line[256] = "";
	char *version = "0";
	char *presence_id = NULL;
	char *free_me = NULL;

	//int i;
	
	//for(i = 0; i < argc; i++) {
	//printf("arg %d[%s] = [%s]\n", i, columnNames[i], argv[i]);
	//}
	
	if (argc > 18) {
		if (!zstr(argv[17])) {
			status = argv[17];
		}
		if (!zstr(argv[18])) {
			rpid = argv[18];
		}
		open_closed = argv[19];
	}

	if (argc > 20) {
		dialog_status = argv[20];
		dialog_rpid = argv[21];
		version = argv[22];
		presence_id = argv[23];
	}

	if (!zstr(presence_id) && strchr(presence_id, '@')) {
		char *p;
		 
		free_me = strdup(presence_id);

		if ((p = strchr(free_me, '@'))) {
			*p = '\0';
		}

		user = free_me;
	}
	

	in = helper->event && helper->event->event_id == SWITCH_EVENT_PRESENCE_IN;

	if (zstr(rpid)) {
		rpid = "unknown";
	}

	if (zstr(status)) {
		if (!strcasecmp(rpid, "busy")) {
			status = "Busy";
		} else if (!strcasecmp(rpid, "unavailable")) {
			status = "Idle";
		} else if (!strcasecmp(rpid, "away")) {
			status = "Idle";
		} else {
			status = "Available";
		}
	}

	if (profile_name && strcasecmp(profile_name, helper->profile->name)) {
		if ((ext_profile = sofia_glue_find_profile(profile_name))) {
			profile = ext_profile;
		}
	}

	if (!(nh = nua_handle_by_call_id(profile->nua, call_id))) {

		if (mod_sofia_globals.debug_presence > 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to find handle for call id %s\n", call_id);
		}

		goto end;
	}

	if (expires) {
		long tmp = atol(expires);
		if (tmp > 0) {
			exptime = tmp - switch_epoch_time_now(NULL);	// - SUB_OVERLAP;
		} else {
			exptime = tmp;
		}
	}

	if (!rpid) {
		rpid = "unknown";
	}

	if (!strcasecmp(proto, SOFIA_CHAT_PROTO)) {
		clean_id = switch_mprintf("sip:%s@%s", sub_to_user, sub_to_host);
	} else {
		clean_id = switch_mprintf("sip:%s+%s@%s", proto, sub_to_user, sub_to_host);
	}

	if (mod_sofia_globals.debug_presence > 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "SEND PRESENCE\nTo:      \t%s@%s\nFrom:    \t%s@%s\nCall-ID:  \t%s\nProfile:\t%s [%s]\n\n",
						  user, host, sub_to_user, sub_to_host, call_id, profile_name, helper->profile->name);
	}

	if (!strcasecmp(sub_to_host, host)) {
		/* same host */
		id = switch_mprintf("sip:%s+%s@%s", proto, sub_to_user, sub_to_host);
	} else if (strcasecmp(proto, SOFIA_CHAT_PROTO)) {
		/*encapsulate */
		id = switch_mprintf("sip:%s+%s+%s@%s", proto, sub_to_user, sub_to_host, host);
	} else {
		id = switch_mprintf("sip:%s@%s", sub_to_user, sub_to_host);
	}

	to = switch_mprintf("sip:%s@%s", user, host);

	is_dialog = !strcmp(event, "dialog");

	if (helper->event) {
		switch_stream_handle_t stream = { 0 };
		const char *direction = switch_str_nil(switch_event_get_header(helper->event, "presence-call-direction"));
		const char *force_direction = switch_str_nil(switch_event_get_header(helper->event, "force-direction"));
		const char *uuid = switch_str_nil(switch_event_get_header(helper->event, "unique-id"));
		const char *event_status = switch_str_nil(switch_event_get_header(helper->event, "status"));
		const char *astate = switch_str_nil(switch_event_get_header(helper->event, "astate"));
		const char *answer_state = switch_str_nil(switch_event_get_header(helper->event, "answer-state"));
		const char *dft_state;
		const char *from_id;
		const char *to_user = switch_str_nil(switch_event_get_header(helper->event, "variable_sip_to_user"));
		const char *from_user = switch_str_nil(switch_event_get_header(helper->event, "variable_sip_from_user"));
		char *clean_to_user = NULL;
		char *clean_from_user = NULL;
		int force_status = 0;

		if (!strcasecmp(direction, "inbound")) {
			from_id = switch_str_nil(switch_event_get_header(helper->event, "Caller-Destination-Number"));
		} else {
			from_id = switch_str_nil(switch_event_get_header(helper->event, "Other-Leg-Caller-ID-Number"));
		}
#if 0
		char *buf;
		switch_event_serialize(helper->event, &buf, SWITCH_FALSE);
		switch_assert(buf);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "CHANNEL_DATA:\n%s\n", buf);
		free(buf);
#endif

		if (is_dialog) {
			SWITCH_STANDARD_STREAM(stream);
		}
		
		if (!zstr(force_direction)) {
			direction = force_direction;
		}

		if (!strcasecmp(direction, "outbound")) {
			direction = "recipient";
			dft_state = "early";
		} else {
			direction = "initiator";
			dft_state = "confirmed";
		}

		if (is_dialog) {
			if (zstr(version)) {
				version = "0";
			}

			stream.write_function(&stream,
								  "<?xml version=\"1.0\"?>\n"
								  "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" "
								  "version=\"%s\" state=\"partial\" entity=\"%s\">\n", version, clean_id);
								  
								  
		}

		//if (strcasecmp(event_status, "Registered")) {
		if (!zstr(uuid)) {
			if (!zstr(answer_state)) {
				astate = answer_state;
			}
			
			if (zstr(astate)) {
				if (is_dialog) {
					astate = dft_state;
				} else {
					astate = "terminated";
				}
			}

			if (!strcasecmp(event_status, "hold")) {
				astate = "early";
			}

			if (!strcasecmp(astate, "answered")) {
				astate = "confirmed";
			}

			if (!strcasecmp(astate, "hangup")) {
				astate = "terminated";
			}

			if (is_dialog) {
				if (!strcasecmp(astate, "ringing")) {
					if (!strcasecmp(direction, "recipient")) {
						astate = "early";
					} else {
						astate = "confirmed";
					}
				}

				stream.write_function(&stream, "<dialog id=\"%s\" direction=\"%s\">\n", uuid, direction);
				stream.write_function(&stream, "<state>%s</state>\n", astate);
			} else {
				if (!strcasecmp(astate, "ringing")) {
					astate = "early";
				}
			}

			if (!strcasecmp(astate, "early") || !strcasecmp(astate, "confirmed")) {

				clean_to_user = switch_mprintf("%s", sub_to_user ? sub_to_user : to_user);
				clean_from_user = switch_mprintf("%s", from_id ? from_id : from_user);

				if (is_dialog) {
					if (!zstr(clean_to_user) && !zstr(clean_from_user)) {
						stream.write_function(&stream, "<local>\n<identity display=\"%s\">sip:%s@%s</identity>\n", clean_to_user, clean_to_user, host);
						stream.write_function(&stream, "<target uri=\"sip:%s@%s\">\n", clean_to_user, host);
						stream.write_function(&stream, "<param pname=\"+sip.rendering\" pvalue=\"%s\"/>\n",
											  !strcasecmp(event_status, "hold") ? "no" : "yes");
						stream.write_function(&stream, "</target>\n</local>\n");
						if (switch_true(switch_event_get_header(helper->event, "Presence-Privacy"))) {
							stream.write_function(&stream, "<remote>\n<identity display=\"Anonymous\">sip:anonymous@anonymous.invalid</identity>\n");
						} else {
							stream.write_function(&stream, "<remote>\n<identity display=\"%s\">sip:%s@%s</identity>\n", clean_from_user, clean_from_user,
												  host);
						}
						stream.write_function(&stream, "<target uri=\"sip:**%s@%s\"/>\n", clean_to_user, host);
						stream.write_function(&stream, "</remote>\n");
					} else if (!strcasecmp(proto, "park")) {
						stream.write_function(&stream, "<local>\n<identity display=\"parking\">sip:parking@%s;fifo=%s</identity>\n",
											  host, !zstr(clean_to_user) ? clean_to_user : "unknown");
						stream.write_function(&stream, "<target uri=\"sip:parking@%s\">\n", host);
						stream.write_function(&stream, "<param pname=\"+sip.rendering\" pvalue=\"no\"/>\n</target>\n</local>\n");
						stream.write_function(&stream, "<remote>\n<identity display=\"parking\">sip:%s</identity>\n", uuid);
						stream.write_function(&stream, "<target uri=\"sip:park+%s\"/>\n", uuid);
						stream.write_function(&stream, "</remote>\n");
					} else if (!strcasecmp(proto, "conf")) {
						stream.write_function(&stream, "<local>\n<identity display=\"conference\">sip:conference@%s;conference=%s</identity>\n",
											  host, !zstr(clean_to_user) ? clean_to_user : "unknown");
						stream.write_function(&stream, "<target uri=\"sip:conference@%s\">\n", host);
						stream.write_function(&stream, "<param pname=\"+sip.rendering\" pvalue=\"yes\"/>\n</target>\n</local>\n");
						stream.write_function(&stream, "<remote>\n<identity display=\"conference\">sip:%s@%s</identity>\n", uuid, host);
						stream.write_function(&stream, "<target uri=\"sip:conf+%s@%s\"/>\n", uuid, host);
						stream.write_function(&stream, "</remote>\n");
					}
				}

				switch_safe_free(clean_to_user);
				switch_safe_free(clean_from_user);
			}
			if (is_dialog) {
				stream.write_function(&stream, "</dialog>\n");
			}
		}

		if (is_dialog) {
			stream.write_function(&stream, "</dialog-info>\n");
			pl = stream.data;
			ct = "application/dialog-info+xml";
		}

		if (!zstr(astate) && !zstr(uuid) && helper && helper->stream.data && strcmp(helper->last_uuid, uuid)) {
			helper->stream.write_function(&helper->stream, "update sip_dialogs set state='%s' where uuid='%s';", astate, uuid);

			switch_copy_string(helper->last_uuid, uuid, sizeof(helper->last_uuid));
		}


		if (!is_dialog && (zstr(astate) || strcasecmp(astate, "hangup"))) {
			switch_set_string(status_line, status);

			if (in) {
				const char *direction = switch_event_get_header(helper->event, "Caller-Direction");
				const char *op, *what = "Ring";
				
				if (direction && !strcasecmp(direction, "outbound")) {
					op = switch_event_get_header(helper->event, "Other-Leg-Caller-ID-Number");
				} else {
					op = switch_event_get_header(helper->event, "Caller-Callee-ID-Number");
				}

				if (zstr(op)) {
					op = switch_event_get_header(helper->event, "Caller-Destination-Number");
				}

				if (direction) {
					what = strcasecmp(direction, "outbound") ? "Call" : "Ring";
				}

				if (!strcmp(astate, "early")) {
					if (zstr(op)) {
						switch_snprintf(status_line, sizeof(status_line), "%sing", what);
					} else {
						switch_snprintf(status_line, sizeof(status_line), "%s %s", what, op);
					}

					rpid = "on-the-phone";
					force_status = 1;

				} else if (!strcmp(astate, "confirmed")) {
					if (zstr(op)) {
						switch_snprintf(status_line, sizeof(status_line), "On The Phone");
					} else {
						switch_snprintf(status_line, sizeof(status_line), "Talk %s", op);
					}

					rpid = "on-the-phone";
					force_status = 1;
				}

				open = "open";
			} else {
				open = "closed";
			}

			if (open_closed) {
				open = open_closed;
			}
			
			prpid = translate_rpid(rpid);

			if (!zstr(dialog_status) && !force_status) {
				status = dialog_status;
				switch_set_string(status_line, status);
			}
			
			if (!zstr(dialog_rpid)) {
				prpid = rpid = dialog_rpid;
			}
			
			
			pl = gen_pidf(user_agent, clean_id, profile->url, open, rpid, prpid, status_line, &ct);
		}

	} else {
		if (in) {
			open = "open";
		} else {
			open = "closed";
		}

		if (open_closed) {
			open = open_closed;
		}

		prpid = translate_rpid(rpid);

		if (!zstr(dialog_status)) {
			status = dialog_status;
		}
		
		if (!zstr(dialog_rpid)) {
			prpid = rpid = dialog_rpid;
		}

		
		pl = gen_pidf(user_agent, clean_id, profile->url, open, rpid, prpid, status, &ct);

	}


	if (helper->event){ 
		const char *uuid = switch_event_get_header_nil(helper->event, "unique-id");

		if (!zstr(uuid) && strchr(uuid, '-')) {
		    char *sql = switch_mprintf("update sip_dialogs set rpid='%q',status='%q' where uuid='%q'", rpid, status_line, uuid);
			sofia_glue_execute_sql(profile, &sql, SWITCH_TRUE);
		}
	}


	nua_handle_bind(nh, &mod_sofia_globals.keep_private);

	if (helper->event && helper->event->event_id == SWITCH_EVENT_PRESENCE_OUT) {
		switch_set_string(sstr, "terminated;reason=noresource");
		switch_set_string(expires_str, "0");
		kill_handle = 1;
	} else if (exptime > 0) {
		switch_snprintf(sstr, sizeof(sstr), "active;expires=%u", (unsigned) exptime);
	} else {
		unsigned delta = (unsigned) (exptime * -1);
		switch_snprintf(sstr, sizeof(sstr), "active;expires=%u", delta);
		switch_snprintf(expires_str, sizeof(expires_str), "%u", delta);
		if (nh && nh->nh_ds && nh->nh_ds->ds_usage) {
			nua_dialog_usage_set_refresh_range(nh->nh_ds->ds_usage, delta, delta);
		}
	}

	if (mod_sofia_globals.debug_presence > 0 && pl) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send payload:\n%s\n", pl);
	}

	nua_notify(nh,
			   TAG_IF(*expires_str, SIPTAG_EXPIRES_STR(expires_str)),
			   SIPTAG_SUBSCRIPTION_STATE_STR(sstr), SIPTAG_EVENT_STR(event), SIPTAG_CONTENT_TYPE_STR(ct), SIPTAG_PAYLOAD_STR(pl), TAG_END());

	nua_handle_unref(nh);

  end:

	switch_safe_free(free_me);

	if (ext_profile) {
		sofia_glue_release_profile(ext_profile);
	}

	switch_safe_free(id);
	switch_safe_free(clean_id);
	switch_safe_free(pl);
	switch_safe_free(to);

	if (nh && kill_handle) {
		nua_handle_destroy(nh);
	}

	return 0;
}

static int sofia_presence_mwi_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	char *sub_to_user = argv[3];
	char *sub_to_host = argv[15];
	char *event = argv[5];
	char *call_id = argv[7];
	char *expires = argv[10];
	char *profile_name = argv[13];
	char *body = argv[15];
	char *id = NULL;
	nua_handle_t *nh;
	int expire_sec = atoi(expires);
	struct mwi_helper *h = (struct mwi_helper *) pArg;
	sofia_profile_t *ext_profile = NULL, *profile = h->profile;

	if (profile_name && strcasecmp(profile_name, h->profile->name)) {
		if ((ext_profile = sofia_glue_find_profile(profile_name))) {
			profile = ext_profile;
		}
	}

	if (!(nh = nua_handle_by_call_id(h->profile->nua, call_id))) {
		if (profile->debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Cannot find handle for %s\n", call_id);
		}
		goto end;
	}

	id = switch_mprintf("sip:%s@%s", sub_to_user, sub_to_host);
	expire_sec = (int) (expire_sec - switch_epoch_time_now(NULL));
	if (expire_sec < 0) {
		expire_sec = 3600;
	}

	nua_handle_bind(nh, &mod_sofia_globals.keep_private);
	nua_notify(nh, SIPTAG_SUBSCRIPTION_STATE_STR("active"),
			   SIPTAG_EVENT_STR(event), SIPTAG_CONTENT_TYPE_STR("application/simple-message-summary"), SIPTAG_PAYLOAD_STR(body), TAG_END());

	nua_handle_unref(nh);

	switch_safe_free(id);

	h->total++;

  end:

	if (ext_profile) {
		sofia_glue_release_profile(ext_profile);
	}

	return 0;
}

static int sofia_presence_mwi_callback2(void *pArg, int argc, char **argv, char **columnNames)
{
	const char *user = argv[0];
	const char *host = argv[1];
	const char *event = "message-summary";
	const char *contenttype = "application/simple-message-summary";
	const char *body = argv[5];
	const char *o_contact = argv[2];
	const char *network_ip = argv[4];

	char *profile_name = argv[3];
	struct mwi_helper *h = (struct mwi_helper *) pArg;
	sofia_profile_t *ext_profile = NULL, *profile = h->profile;

	if (profile_name && strcasecmp(profile_name, h->profile->name)) {
		if ((ext_profile = sofia_glue_find_profile(profile_name))) {
			profile = ext_profile;
		}
	}

	sofia_glue_send_notify(profile, user, host, event, contenttype, body, o_contact, network_ip);

	if (ext_profile) {
		sofia_glue_release_profile(ext_profile);
	}

	return 0;
}

static int broadsoft_sla_notify_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	struct state_helper *sh = (struct state_helper *) pArg;
	char key[256] = "";
	char *data = NULL, *tmp;
	char *call_id = argv[0];
	char *expires = argv[1];
	char *user = argv[2];
	char *host = argv[3];
	char *event = argv[4];
	int i;
	char sstr[128] = "", expires_str[128] = "";
	time_t exptime = 3600;
	nua_handle_t *nh;

	if (mod_sofia_globals.debug_sla > 1) {
		for (i = 0; i < argc; i++) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SLA3: %d [%s]=[%s]\n", i, columnNames[i], argv[i]);
		}
	}

	switch_snprintf(key, sizeof(key), "%s%s", user, host);
	data = switch_core_hash_find(sh->hash, key);

	if (expires) {
		long tmp = atol(expires);
		exptime = tmp - switch_epoch_time_now(NULL);
	}

	if (exptime > 0) {
		switch_snprintf(sstr, sizeof(sstr), "active;expires=%u", (unsigned) exptime);
	} else {
		switch_snprintf(sstr, sizeof(sstr), "terminated;reason=noresource");
	}

	switch_snprintf(expires_str, sizeof(expires_str), "%u", (unsigned) exptime);

	data = switch_core_hash_find(sh->hash, key);

	if (data) {
		tmp = switch_core_sprintf(sh->pool, "%s,<sip:%s>;appearance-index=*;appearance-state=idle", data, host);
	} else {
		tmp = switch_core_sprintf(sh->pool, "<sip:%s>;appearance-index=*;appearance-state=idle", host);
	}

	if (!strcasecmp(event, "line-seize") && (nh = nua_handle_by_call_id(sh->profile->nua, call_id))) {
		char *hack;

		if ((hack = (char *) switch_stristr("=seized", tmp))) {
			switch_snprintf(hack, 7, "=idle  ");
		}
		nua_notify(nh,
				   SIPTAG_EXPIRES_STR("0"),
				   SIPTAG_SUBSCRIPTION_STATE_STR("terminated;reason=noresource"), SIPTAG_EVENT_STR("line-seize"), SIPTAG_CALL_INFO_STR(tmp), TAG_END());
		nua_handle_unref(nh);
		return 0;
	}

	if (!strcasecmp(event, "call-info") && (nh = nua_handle_by_call_id(sh->profile->nua, call_id))) {
		nua_notify(nh,
				   TAG_IF(*expires_str, SIPTAG_EXPIRES_STR(expires_str)),
				   SIPTAG_SUBSCRIPTION_STATE_STR(sstr), SIPTAG_EVENT_STR("call-info"), SIPTAG_CALL_INFO_STR(tmp), TAG_END());
		nua_handle_unref(nh);
	}

	return 0;
}

static int broadsoft_sla_gather_state_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	struct state_helper *sh = (struct state_helper *) pArg;
	char key[256] = "";
	switch_core_session_t *session;
	const char *callee_name = NULL, *callee_number = NULL;
	char *data = NULL, *tmp;
	char *user = argv[0];
	char *host = argv[1];
	char *info = argv[2];
	char *state = argv[3];
	char *uuid = argv[4];
	int i;

	if (mod_sofia_globals.debug_sla > 1) {
		for (i = 0; i < argc; i++) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SLA2: %d [%s]=[%s]\n", i, columnNames[i], argv[i]);
		}
	}

	if (zstr(info)) {
		return 0;
	}

	if (zstr(state)) {
		state = "idle";
	}

	switch_snprintf(key, sizeof(key), "%s%s", user, host);

	data = switch_core_hash_find(sh->hash, key);

	if (uuid && (session = switch_core_session_locate(uuid))) {
		switch_channel_t *channel = switch_core_session_get_channel(session);

		if (zstr((callee_name = switch_channel_get_variable(channel, "effective_callee_id_name"))) &&
			zstr((callee_name = switch_channel_get_variable(channel, "sip_callee_id_name")))) {
			callee_name = switch_channel_get_variable(channel, "callee_id_name");
		}
		
		if (zstr((callee_number = switch_channel_get_variable(channel, "effective_callee_id_number"))) &&
			zstr((callee_number = switch_channel_get_variable(channel, "sip_callee_id_number")))) {
			callee_number = switch_channel_get_variable(channel, "destination_number");
		}
		
		if (zstr(callee_name) && !zstr(callee_number)) {
			callee_name = callee_number;
		}

		if (!zstr(callee_number)) {
			callee_number = switch_sanitize_number(switch_core_session_strdup(session, callee_number));
		}

		if (!zstr(callee_name)) {
			callee_name = switch_sanitize_number(switch_core_session_strdup(session, callee_name));
		}
		switch_core_session_rwunlock(session);
	}

	if (!zstr(callee_number)) {
		if (zstr(callee_name)) {
			callee_name = "unknown";
		}
		
		if (data) {
			tmp = switch_core_sprintf(sh->pool,
									  "%s,<sip:%s>;%s;appearance-state=%s;appearance-uri=\"\\\"%s\\\" <sip:%s@%s>\"",
									  data, host, info, state, callee_name, callee_number, host);
		} else {
			tmp = switch_core_sprintf(sh->pool,
									  "<sip:%s>;%s;appearance-state=%s;appearance-uri=\"\\\"%s\\\" <sip:%s@%s>\"",
									  host, info, state, callee_name, callee_number, host);
		}
	} else {
		if (data) {
			tmp = switch_core_sprintf(sh->pool, "%s,<sip:%s>;%s;appearance-state=%s", data, host, info, state);
		} else {
			tmp = switch_core_sprintf(sh->pool, "<sip:%s>;%s;appearance-state=%s", host, info, state);
		}
	}

	switch_core_hash_insert(sh->hash, key, tmp);

	return 0;
}

static void sync_sla(sofia_profile_t *profile, const char *to_user, const char *to_host, switch_bool_t clear, switch_bool_t unseize)
{
	struct state_helper *sh;
	switch_memory_pool_t *pool;
	char *sql;

	switch_core_new_memory_pool(&pool);
	sh = switch_core_alloc(pool, sizeof(*sh));
	sh->pool = pool;
	switch_core_hash_init(&sh->hash, sh->pool);

	sql = switch_mprintf("select sip_from_user,sip_from_host,call_info,call_info_state,uuid from sip_dialogs "
						 "where call_info_state is not null and hostname='%q' and ((sip_from_user='%q' and sip_from_host='%q') or presence_id='%q@%q')", 
						 mod_sofia_globals.hostname, to_user, to_host, to_user, to_host);


	if (mod_sofia_globals.debug_sla > 1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PRES SQL %s\n", sql);
	}
	sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, broadsoft_sla_gather_state_callback, sh);
	switch_safe_free(sql);


	if (unseize) {
		sql = switch_mprintf("select call_id,expires,sub_to_user,sub_to_host,event "
							 "from sip_subscriptions "
							 "where version > -1 and expires > -1 and hostname='%q' "
							 "and sub_to_user='%q' and sub_to_host='%q' "
							 "and (event='call-info' or event='line-seize')", mod_sofia_globals.hostname, to_user, to_host);
	} else {
		sql = switch_mprintf("select call_id,expires,sub_to_user,sub_to_host,event "
							 "from sip_subscriptions "
							 "where version > -1 and expires > -1 and hostname='%q' "
							 "and sub_to_user='%q' and sub_to_host='%q' " "and (event='call-info')", mod_sofia_globals.hostname, to_user, to_host);
	}


	if (mod_sofia_globals.debug_sla > 1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PRES SQL %s\n", sql);
	}
	sh->profile = profile;
	sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, broadsoft_sla_notify_callback, sh);
	switch_safe_free(sql);
	sh = NULL;
	switch_core_destroy_memory_pool(&pool);



	if (clear) {
		sql = switch_mprintf("delete from sip_dialogs where ((sip_from_user='%q' and sip_from_host='%q') or presence_id='%q@%q') "
							 "and call_info_state='seized'", to_user, to_host, to_user, to_host);


		if (mod_sofia_globals.debug_sla > 1) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "CLEAR SQL %s\n", sql);
		}
		sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
	}

}

void sofia_presence_handle_sip_i_subscribe(int status,
										   char const *phrase,
										   nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
								sofia_dispatch_event_t *de,
										   tagi_t tags[])
{

	long exp_delta;
	char exp_delta_str[30] = "";
	sip_to_t const *to;
	const char *from_user = NULL, *from_host = NULL;
	const char *to_user = NULL, *to_host = NULL;
	char *my_to_user = NULL;
	char *sql, *event = NULL;
	char *proto = "sip";
	char *d_user = NULL;
	char *contact_str = "";
	const char *call_id = NULL;
	char *to_str = NULL;
	char *full_from = NULL;
	char *full_via = NULL;
	char *full_agent = NULL;
	char *sstr;
	switch_event_t *sevent;
	int sub_state;
	int sent_reply = 0;
	sip_contact_t const *contact;
	const char *ipv6;
	const char *contact_user;
	sofia_nat_parse_t np = { { 0 } };

	if (!sip) {
		return;
	}

	to = sip->sip_to;
	contact = sip->sip_contact;

	if (!(contact_str = sofia_glue_gen_contact_str(profile, sip, de, &np))) {
		nua_respond(nh, 481, "INVALID SUBSCRIPTION", TAG_END());
		return;
	}

	//contact_host = sip->sip_contact->m_url->url_host;
	contact_user = sip->sip_contact->m_url->url_user;

	tl_gets(tags, NUTAG_SUBSTATE_REF(sub_state), TAG_END());

	event = sip_header_as_string(nh->nh_home, (void *) sip->sip_event);


	/* the following could be refactored back to the calling event handler in sofia.c XXX MTK */
	if (sofia_test_pflag(profile, PFLAG_MANAGE_SHARED_APPEARANCE)) {
		if (sip->sip_request->rq_url->url_user && !strncmp(sip->sip_request->rq_url->url_user, "sla-agent", sizeof("sla-agent"))) {
			/* only fire this on <200 to try to avoid resubscribes. probably better ways to do this? */
			if (status < 200) {
				sofia_sla_handle_sip_i_subscribe(nua, contact_str, profile, nh, sip, de, tags);
			}
			switch_safe_free(contact_str);
			return;
		}
	}

	if (to) {
		to_str = switch_mprintf("sip:%s@%s", to->a_url->url_user, to->a_url->url_host);
	}

	if (to) {
		to_user = to->a_url->url_user;
		to_host = to->a_url->url_host;
	}

	if (profile->sub_domain) {
		to_host = profile->sub_domain;
	}

	if (sip->sip_from) {
		from_user = sip->sip_from->a_url->url_user;
		from_host = sip->sip_from->a_url->url_host;
	} else {
		from_user = "n/a";
		from_host = "n/a";
	}

	if ((exp_delta = sip->sip_expires ? sip->sip_expires->ex_delta : 3600)) {
		if (profile->force_subscription_expires) {
			exp_delta = profile->force_subscription_expires;
		}
	}

	if (!exp_delta) {
		sub_state = nua_substate_terminated;
	}

	switch_snprintf(exp_delta_str, sizeof(exp_delta_str), "%ld", exp_delta);

	if (to_user && strchr(to_user, '+')) {
		char *h;
		if ((proto = (d_user = strdup(to_user)))) {
			if ((my_to_user = strchr(d_user, '+'))) {
				*my_to_user++ = '\0';
				to_user = my_to_user;
				if ((h = strchr(to_user, '+')) || (h = strchr(to_user, '@'))) {
					*h++ = '\0';
					to_host = h;
				}
			}
		}

		if (!(proto && to_user && to_host)) {
			nua_respond(nh, SIP_404_NOT_FOUND, NUTAG_WITH_THIS_MSG(de->data->e_msg), TAG_END());
			goto end;
		}
	}

	call_id = sip->sip_call_id->i_id;
	full_from = sip_header_as_string(nh->nh_home, (void *) sip->sip_from);
	full_via = sip_header_as_string(nh->nh_home, (void *) sip->sip_via);

	if (sip->sip_expires && sip->sip_expires->ex_delta > 31536000) {
		sip->sip_expires->ex_delta = 31536000;
	}

	
	if ((sub_state == nua_substate_active) && (switch_stristr("dialog", (const char *) event))) {
		
		sstr = switch_mprintf("active;expires=%ld", exp_delta);
		
		sql = switch_mprintf("update sip_subscriptions "
							 "set expires=%ld "
							 "where call_id='%q' and event='dialog' and hostname='%q' ",
							 (long) switch_epoch_time_now(NULL) + (exp_delta * 2),
							 call_id,
							 mod_sofia_globals.hostname);
		

		if (mod_sofia_globals.debug_presence > 0 || mod_sofia_globals.debug_sla > 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "re-subscribe with dialog detected, sql: %s\n", sql);
		}
		
		sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
	} else {
		if (sofia_test_pflag(profile, PFLAG_MULTIREG)) {
			sql = switch_mprintf("delete from sip_subscriptions where call_id='%q' "
								 "or (proto='%q' and sip_user='%q' and sip_host='%q' "
								 "and sub_to_user='%q' and sub_to_host='%q' and event='%q' and hostname='%q' "
								 "and contact='%q')",
								 call_id, proto, from_user, from_host, to_user, to_host, event, mod_sofia_globals.hostname, contact_str);
			
		} else {
			sql = switch_mprintf("delete from sip_subscriptions where "
								 "proto='%q' and sip_user='%q' and sip_host='%q' and sub_to_user='%q' and sub_to_host='%q' and event='%q' and hostname='%q'",
								 proto, from_user, from_host, to_user, to_host, event, mod_sofia_globals.hostname);
		}

		switch_mutex_lock(profile->ireg_mutex);
		switch_assert(sql != NULL);
		sofia_glue_actually_execute_sql(profile, sql, NULL);
		switch_safe_free(sql);

		if (sub_state == nua_substate_terminated) {
			sstr = switch_mprintf("terminated");
		} else {
			sip_accept_t *ap = sip->sip_accept;
			char accept[256] = "";
			full_agent = sip_header_as_string(nh->nh_home, (void *) sip->sip_user_agent);
			while (ap) {
				switch_snprintf(accept + strlen(accept), sizeof(accept) - strlen(accept), "%s%s ", ap->ac_type, ap->ac_next ? "," : "");
				ap = ap->ac_next;
			}

			sql = switch_mprintf("insert into sip_subscriptions "
								 "(proto,sip_user,sip_host,sub_to_user,sub_to_host,presence_hosts,event,contact,call_id,full_from,"
								 "full_via,expires,user_agent,accept,profile_name,hostname,network_port,network_ip) "
								 "values ('%q','%q','%q','%q','%q','%q','%q','%q','%q','%q','%q',%ld,'%q','%q','%q','%q','%d','%q')",
								 proto, from_user, from_host, to_user, to_host, profile->presence_hosts ? profile->presence_hosts : to_host,
								 event, contact_str, call_id, full_from, full_via,
								 //sofia_test_pflag(profile, PFLAG_MULTIREG) ? switch_epoch_time_now(NULL) + exp_delta : exp_delta * -1,
								 (long) switch_epoch_time_now(NULL) + (exp_delta * 2),
								 full_agent, accept, profile->name, mod_sofia_globals.hostname, np.network_port, np.network_ip);

			switch_assert(sql != NULL);

			if (mod_sofia_globals.debug_presence > 0 || mod_sofia_globals.debug_sla > 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "%s SUBSCRIBE %s@%s %s@%s\n%s\n",
								  profile->name, from_user, from_host, to_user, to_host, sql);
			}


			sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
			sstr = switch_mprintf("active;expires=%ld", exp_delta);
	}

	switch_mutex_unlock(profile->ireg_mutex);
	}

	if (status < 200) {
		char *sticky = NULL;
		char *contactstr = profile->url, *cs = NULL;
		char *p = NULL, *new_contactstr = NULL;

		if (np.is_nat) {
			char params[128] = "";
			if (contact->m_url->url_params) {
				switch_snprintf(params, sizeof(params), ";%s", contact->m_url->url_params);
			}
			ipv6 = strchr(np.network_ip, ':');
			sticky = switch_mprintf("sip:%s@%s%s%s:%d%s", contact_user, ipv6 ? "[" : "", np.network_ip, ipv6 ? "]" : "", np.network_port, params);
		}

		if (np.is_auto_nat) {
			contactstr = profile->public_url;
		} else {
			contactstr = profile->url;
		}


		if (switch_stristr("port=tcp", contact->m_url->url_params)) {
			if (np.is_auto_nat) {
				cs = profile->tcp_public_contact;
			} else {
				cs = profile->tcp_contact;
			}
		} else if (switch_stristr("port=tls", contact->m_url->url_params)) {
			if (np.is_auto_nat) {
				cs = profile->tls_public_contact;
			} else {
				cs = profile->tls_contact;
			}
		}

		if (cs) {
			contactstr = cs;
		}

			
		if (nh && nh->nh_ds && nh->nh_ds->ds_usage) {
			/* nua_dialog_usage_set_refresh_range(nh->nh_ds->ds_usage, exp_delta + SUB_OVERLAP, exp_delta + SUB_OVERLAP); */
			nua_dialog_usage_set_refresh_range(nh->nh_ds->ds_usage, exp_delta * 2, exp_delta * 2);
		}

		if (contactstr && (p = strchr(contactstr, '@'))) {
			if (strrchr(p, '>')) {
				new_contactstr = switch_mprintf("<sip:%s%s", to_user, p);
			} else {
				new_contactstr = switch_mprintf("<sip:%s%s>", to_user, p);
			}
		}

		nua_respond(nh, SIP_202_ACCEPTED,
					TAG_IF(new_contactstr, SIPTAG_CONTACT_STR(new_contactstr)),
					NUTAG_WITH_THIS_MSG(de->data->e_msg),
					SIPTAG_SUBSCRIPTION_STATE_STR(sstr), SIPTAG_EXPIRES_STR(exp_delta_str), TAG_IF(sticky, NUTAG_PROXY(sticky)), TAG_END());

		switch_safe_free(new_contactstr);
		switch_safe_free(sticky);
	}

	if (sub_state == nua_substate_terminated) {
		char *full_call_info = NULL;
		char *p = NULL;

		if (sip->sip_call_info) {
			full_call_info = sip_header_as_string(nh->nh_home, (void *) sip->sip_call_info);
			if ((p = strchr(full_call_info, ';'))) {
				p++;
			}

			nua_notify(nh,
					   SIPTAG_EXPIRES_STR("0"),
					   SIPTAG_SUBSCRIPTION_STATE_STR(sstr), TAG_IF(full_call_info, SIPTAG_CALL_INFO_STR(full_call_info)), TAG_END());


			if (!strcasecmp(event, "line-seize")) {
				if (mod_sofia_globals.debug_sla > 1) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "CANCEL LINE SEIZE\n");
				}

				sql = switch_mprintf("delete from sip_dialogs where ((sip_from_user='%q' and sip_from_host='%q') or presence_id='%q@%q') "
									 "and call_info_state='seized'",
									 to_user, to_host, to_user, to_host);


				if (mod_sofia_globals.debug_sla > 1) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "CLEAR SQL %s\n", sql);
				}
				sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);

				sync_sla(profile, to_user, to_host, SWITCH_FALSE, SWITCH_FALSE);
			}

			su_free(nh->nh_home, full_call_info);

		}
			
	} else {
		if (!strcasecmp(event, "line-seize")) {
			char *full_call_info = NULL;
			char *p;

			if (sip->sip_call_info) {
				full_call_info = sip_header_as_string(nh->nh_home, (void *) sip->sip_call_info);
				if ((p = strchr(full_call_info, ';'))) {
					p++;
				}

				nua_notify(nh,
						   SIPTAG_EXPIRES_STR(exp_delta_str),
						   SIPTAG_SUBSCRIPTION_STATE_STR(sstr),
						   SIPTAG_EVENT_STR("line-seize"), TAG_IF(full_call_info, SIPTAG_CALL_INFO_STR(full_call_info)), TAG_END());



				sql = switch_mprintf("delete from sip_dialogs where ((sip_from_user='%q' and sip_from_host='%q') or presence_id='%q@%q') "
									 "and call_info_state='seized'",
									 to_user, to_host, to_user, to_host);


				if (mod_sofia_globals.debug_sla > 1) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "CLEAR SQL %s\n", sql);
				}
				sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);

				sql = switch_mprintf("insert into sip_dialogs (sip_from_user,sip_from_host,call_info,call_info_state,hostname,expires) "
									 "values ('%q','%q','%q','seized','%q',%ld)",
									 to_user, to_host, switch_str_nil(p), mod_sofia_globals.hostname, switch_epoch_time_now(NULL) + exp_delta);

				if (mod_sofia_globals.debug_sla > 1) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SEIZE SQL %s\n", sql);
				}
				sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
				sync_sla(profile, to_user, to_host, SWITCH_FALSE, SWITCH_FALSE);

				su_free(nh->nh_home, full_call_info);
			}
		} else if (!strcasecmp(event, "call-info")) {
			sync_sla(profile, to_user, to_host, SWITCH_FALSE, SWITCH_FALSE);
		}
	}

	sent_reply++;

	switch_safe_free(sstr);

	if (!strcasecmp(event, "message-summary")) {
		if ((sql = switch_mprintf("select proto,sip_user,'%q',sub_to_user,sub_to_host,event,contact,call_id,full_from,"
								  "full_via,expires,user_agent,accept,profile_name,network_ip"
								  " from sip_subscriptions where version > -1 and "
								  "expires > -1 and event='message-summary' and sip_user='%q' "
								  "and (sip_host='%q' or presence_hosts like '%%%q%%')", to_host, to_user, to_host, to_host))) {
			sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_presence_sub_reg_callback, profile);

			switch_safe_free(sql);
		}
	}

 end:

	if (to_user && (strstr(to_user, "ext+") || strstr(to_user, "user+"))) {
		char protocol[80];
		char *p;

		switch_copy_string(protocol, to_user, sizeof(protocol));
		if ((p = strchr(protocol, '+'))) {
			*p = '\0';
		}

		if (switch_event_create(&sevent, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "proto", protocol);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "login", profile->name);
			switch_event_add_header(sevent, SWITCH_STACK_BOTTOM, "from", "%s@%s", to_user, to_host);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "rpid", "active");
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "status", "Click To Call");
			switch_event_fire(&sevent);
		}

	} else if (to_user && (strcasecmp(proto, SOFIA_CHAT_PROTO) != 0)) {
		if (switch_event_create(&sevent, SWITCH_EVENT_PRESENCE_PROBE) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "proto", proto);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "long", profile->name);
			switch_event_add_header(sevent, SWITCH_STACK_BOTTOM, "from", "%s@%s", from_user, from_host);
			switch_event_add_header(sevent, SWITCH_STACK_BOTTOM, "to", "%s%s%s@%s", proto, "+", to_user, to_host);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "proto-specific-event-name", event);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "expires", exp_delta_str);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "event_type", "presence");
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "expires", exp_delta_str);
			switch_event_fire(&sevent);
	
		}
	} else {
		if (switch_event_create(&sevent, SWITCH_EVENT_PRESENCE_PROBE) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "proto", SOFIA_CHAT_PROTO);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "login", profile->name);
			switch_event_add_header(sevent, SWITCH_STACK_BOTTOM, "from", "%s@%s", from_user, from_host);
			switch_event_add_header(sevent, SWITCH_STACK_BOTTOM, "to", "%s@%s", to_user, to_host);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "proto-specific-event-name", event);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "expires", exp_delta_str);
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "event_type", "presence");
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "alt_event_type", "dialog");
			switch_event_add_header_string(sevent, SWITCH_STACK_BOTTOM, "expires", exp_delta_str);
			switch_event_fire(&sevent);
		}
	}



	if (event) {
		su_free(nh->nh_home, event);
	}

	if (full_from) {
		su_free(nh->nh_home, full_from);
	}
	if (full_via) {
		su_free(nh->nh_home, full_via);
	}
	if (full_agent) {
		su_free(nh->nh_home, full_agent);
	}

	switch_safe_free(d_user);
	switch_safe_free(to_str);
	switch_safe_free(contact_str);

	if (!sent_reply) {
		nua_respond(nh, 481, "INVALID SUBSCRIPTION", TAG_END());
	}
		
}


sofia_gateway_subscription_t *sofia_find_gateway_subscription(sofia_gateway_t *gateway_ptr, const char *event)
{
	sofia_gateway_subscription_t *gw_sub_ptr;
	for (gw_sub_ptr = gateway_ptr->subscriptions; gw_sub_ptr; gw_sub_ptr = gw_sub_ptr->next) {
		if (!strcasecmp(gw_sub_ptr->event, event)) {
			/* this is the gateway subscription we are interested in */
			return gw_sub_ptr;
		}
	}
	return NULL;
}

void sofia_presence_handle_sip_r_subscribe(int status,
										   char const *phrase,
										   nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
								sofia_dispatch_event_t *de,
										   tagi_t tags[])
{
	sip_event_t const *o = NULL;
	sofia_gateway_subscription_t *gw_sub_ptr;

	if (!sip) {
		return;
	}

	tl_gets(tags, SIPTAG_EVENT_REF(o), TAG_END());
	/* o->o_type: message-summary (for example) */
	if (!o) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Event information not given\n");
		return;
	}

	/* the following could possibly be refactored back towards the calling event handler in sofia.c XXX MTK */
	if (sofia_test_pflag(profile, PFLAG_MANAGE_SHARED_APPEARANCE)) {
		if (!strcasecmp(o->o_type, "dialog") && msg_params_find(o->o_params, "sla")) {
			sofia_sla_handle_sip_r_subscribe(status, phrase, nua, profile, nh, sofia_private, sip, de, tags);
			return;
		}
	}

	if (!sofia_private || !sofia_private->gateway) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Gateway information missing\n");
		return;
	}

	/* Find the subscription if one exists */
	if (!(gw_sub_ptr = sofia_find_gateway_subscription(sofia_private->gateway, o->o_type))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not find gateway subscription.  Gateway: %s.  Subscription Event: %s\n",
						  sofia_private->gateway->name, o->o_type);
		return;
	}

	/* Update the subscription status for the subscription */
	switch (status) {
	case 200:
		/* TODO: in the spec it is possible for the other side to change the original expiry time,
		 * this needs to be researched (eg, what sip header this information will be in) and implemented.
		 * Although, since it seems the sofia stack is pretty much handling the subscription expiration
		 * anyway, then maybe its not even worth bothering.
		 */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "got 200 OK response, updated state to SUB_STATE_SUBSCRIBE.\n");
		gw_sub_ptr->state = SUB_STATE_SUBSCRIBE;
		break;
	case 100:
		break;
	default:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "status (%d) != 200, updated state to SUB_STATE_FAILED.\n", status);
		gw_sub_ptr->state = SUB_STATE_FAILED;

		if (sofia_private) {
			nua_handle_destroy(sofia_private->gateway->sub_nh);
			sofia_private->gateway->sub_nh = NULL;
			nua_handle_bind(sofia_private->gateway->sub_nh, NULL);
			sofia_private_free(sofia_private);
		} else {
			nua_handle_destroy(nh);
		}

		break;
	}
}

struct cpc {
	sofia_profile_t *profile;
	sofia_dispatch_event_t *de;
};

static int sofia_counterpath_crutch(void *pArg, int argc, char **argv, char **columnNames)
{
	nua_handle_t *nh;
	struct cpc *crutch = (struct cpc *) pArg;
	char *call_id = argv[0];
	char *pl = argv[1];
	char *event_type = argv[2];
	long exp_delta = atol(argv[3]);

	if ((nh = nua_handle_by_call_id(crutch->profile->nua, call_id))) {
		char sstr[128] = "", expstr[128] = "";
		switch_snprintf(expstr, sizeof(expstr), "%d", exp_delta);
		switch_snprintf(sstr, sizeof(sstr), "active;expires=%u", exp_delta);
		nua_notify(nh,
				   NUTAG_WITH_THIS_MSG(crutch->de->data->e_msg),
				   SIPTAG_EXPIRES_STR(expstr),
				   SIPTAG_SUBSCRIPTION_STATE_STR(sstr), SIPTAG_EVENT_STR(event_type), 
				   SIPTAG_CONTENT_TYPE_STR("application/pidf+xml"), SIPTAG_PAYLOAD_STR(pl), TAG_END());
		nua_handle_unref(nh);
	}

	return 0;
}


uint32_t sofia_presence_contact_count(sofia_profile_t *profile, const char *contact_str)
{
	char buf[32] = "";
	char *sql;
	
	sql = switch_mprintf("select count(*) from sip_subscriptions where profile_name='%q' and contact='%q'", profile->name, contact_str);
	
	sofia_glue_execute_sql2str(profile, profile->ireg_mutex, sql, buf, sizeof(buf));
	switch_safe_free(sql);
	return atoi(buf);				
}

void sofia_presence_handle_sip_i_publish(nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
								sofia_dispatch_event_t *de,
										 tagi_t tags[])
{
	
	sip_from_t const *from;
	char *from_user = NULL;
	char *from_host = NULL;
	char *rpid = "";
	sip_payload_t *payload;
	char *event_type = NULL;
	char etag[9] = "";
	char expstr[30] = "";
	long exp = 0, exp_delta = 3600;
	char *pd_dup = NULL;
	int count = 1, sub_count = 0;
	char *contact_str;
	int open = 1;

	if (!sip) {
		return;
	}

	from = sip->sip_from;
	payload = sip->sip_payload;

	/* the following could instead be refactored back to the calling event handler in sofia.c XXX MTK */
	if (sofia_test_pflag(profile, PFLAG_MANAGE_SHARED_APPEARANCE)) {
		/* also it probably is unsafe to dereference so many things in a row without testing XXX MTK */
		if (sip->sip_request->rq_url->url_user && !strncmp(sip->sip_request->rq_url->url_user, "sla-agent", sizeof("sla-agent"))) {
			sofia_sla_handle_sip_i_publish(nua, profile, nh, sip, de, tags);
			return;
		}
	}

	contact_str = sofia_glue_gen_contact_str(profile, sip, de, NULL);

	if (from) {
		from_user = (char *) from->a_url->url_user;
		from_host = (char *) from->a_url->url_host;
	}
		
	exp_delta = (sip->sip_expires ? sip->sip_expires->ex_delta : 3600);
	if (profile->force_publish_expires) {
		exp_delta = profile->force_publish_expires;
	}

	if (exp_delta < 0) {
		exp = exp_delta;
	} else {
		exp = (long) switch_epoch_time_now(NULL) + exp_delta;
	}

	if (payload) {
		switch_xml_t xml, note, person, tuple, status, basic, act;
		switch_event_t *event;
		char *sql;
		char *full_agent = NULL;
			
		pd_dup = strdup(payload->pl_data);

		if ((xml = switch_xml_parse_str(pd_dup, strlen(pd_dup)))) {
			char *open_closed = "", *note_txt = "";

			if (sip->sip_user_agent) {
				full_agent = sip_header_as_string(nh->nh_home, (void *) sip->sip_user_agent);
			}

			if ((tuple = switch_xml_child(xml, "tuple")) && (status = switch_xml_child(tuple, "status"))
				&& (basic = switch_xml_child(status, "basic"))) {
				open_closed = basic->txt;
			}

			if ((person = switch_xml_child(xml, "dm:person")) && (note = switch_xml_child(person, "dm:note"))) {
				note_txt = note->txt;
			}

			if (person && (act = switch_xml_child(person, "rpid:activities")) && act->child && act->child->name) {
				if ((rpid = strchr(act->child->name, ':'))) {
					rpid++;
				} else {
					rpid = act->child->name;
				}
			}

			if (!(open = !strcasecmp(open_closed, "open"))) {
				sql = switch_mprintf("update sip_subscriptions set version = -1 where contact='%q'", contact_str);
			} else {
				sql = switch_mprintf("update sip_subscriptions set version = 0 where contact='%q'", contact_str);
			}
			
			sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);


			if (sofia_test_pflag(profile, PFLAG_MULTIREG) && !open) {
				count = sofia_reg_reg_count(profile, from_user, from_host);
			}
				
			sub_count = sofia_presence_contact_count(profile, contact_str);

			/* if (count > 1) let's not and say we did or all the clients who subscribe to their own presence will think they selves is offline */

			event_type = sip_header_as_string(nh->nh_home, (void *) sip->sip_event);

			if (count < 2) {
				if ((sql = switch_mprintf("delete from sip_presence where sip_user='%q' and sip_host='%q' "
										  " and profile_name='%q' and hostname='%q'", 
										  from_user, from_host, profile->name, mod_sofia_globals.hostname))) {
					sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
				}
					
				if (sub_count > 0 && (sql = switch_mprintf("insert into sip_presence (sip_user, sip_host, status, rpid, expires, user_agent,"
														   " profile_name, hostname, open_closed) "
														   "values ('%q','%q','%q','%q',%ld,'%q','%q','%q','%q')",
														   from_user, from_host, note_txt, rpid, exp, full_agent, profile->name, 
														   mod_sofia_globals.hostname, open_closed))) {
					
					sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
				}
				
			} else if (contact_str) {
				struct cpc crutch;

				crutch.profile = profile;
				crutch.de = de;
				sql = switch_mprintf("select call_id,'%q','%q','%ld' from sip_subscriptions where sub_to_user='%q' and sub_to_host='%q' "
									 "and contact = '%q' ", payload->pl_data ? payload->pl_data : "", event_type, exp_delta,
									 from_user, from_host, contact_str);
				sofia_glue_execute_sql_callback(profile, profile->ireg_mutex, sql, sofia_counterpath_crutch, &crutch);
				switch_safe_free(sql);
			}

			if (sub_count > 0) {
				if (switch_event_create(&event, SWITCH_EVENT_PRESENCE_IN) == SWITCH_STATUS_SUCCESS) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", SOFIA_CHAT_PROTO);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "rpid", rpid);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", profile->url);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "user-agent", full_agent);
					switch_event_add_header(event, SWITCH_STACK_BOTTOM, "from", "%s@%s", from_user, from_host);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "status", note_txt);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_type", event_type);
					switch_event_fire(&event);
				}
			}

			if (event_type) {
				su_free(nh->nh_home, event_type);
			}

			if (full_agent) {
				su_free(nh->nh_home, full_agent);
			}

			switch_xml_free(xml);
		}
	} else {
		char *sql = switch_mprintf("update sip_presence set expires=%ld where sip_user='%q' and sip_host='%q' and profile_name='%q' and hostname='%q'",
								   exp, from_user, from_host, profile->name, mod_sofia_globals.hostname);
		sofia_glue_execute_sql_now(profile, &sql, SWITCH_TRUE);
	}

	switch_safe_free(pd_dup);

	switch_snprintf(expstr, sizeof(expstr), "%d", exp_delta);
	switch_stun_random_string(etag, 8, NULL);

	if (sub_count > 0) {
		nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS_MSG(de->data->e_msg), SIPTAG_ETAG_STR(etag), SIPTAG_EXPIRES_STR(expstr), TAG_END());
	} else {
		nua_respond(nh, SIP_404_NOT_FOUND, NUTAG_WITH_THIS_MSG(de->data->e_msg), TAG_END());
	}

	switch_safe_free(contact_str);
}

void sofia_presence_set_hash_key(char *hash_key, int32_t len, sip_t const *sip)
{
	url_t *to = sip->sip_to->a_url;
	url_t *from = sip->sip_from->a_url;
	switch_snprintf(hash_key, len, "%s%s%s", from->url_user, from->url_host, to->url_user);
}

void sofia_presence_handle_sip_i_message(int status,
										 char const *phrase,
										 nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
								sofia_dispatch_event_t *de,
										 tagi_t tags[])
{
	if (sip) {
		sip_from_t const *from = sip->sip_from;
		const char *from_user = NULL;
		const char *from_host = NULL;
		sip_to_t const *to = sip->sip_to;
		const char *to_user = NULL;
		const char *to_host = NULL;
		sip_payload_t *payload = sip->sip_payload;
		char *msg = NULL;

		if (sip->sip_content_type && sip->sip_content_type->c_subtype) {
			if (strstr(sip->sip_content_type->c_subtype, "composing")) {
				return;
			}
		}

		if (from) {
			from_user = from->a_url->url_user;
			from_host = from->a_url->url_host;
		}

		if (to) {
			to_user = to->a_url->url_user;
			to_host = to->a_url->url_host;
		}

		if (!to_user) {
			return;
		}

		if (payload) {
			msg = payload->pl_data;
		}

		if (nh) {
			char hash_key[512];
			private_object_t *tech_pvt;
			switch_event_t *event;
			char *to_addr;
			char *from_addr;
			char *p;
			char *full_from;
			char proto[512] = SOFIA_CHAT_PROTO;

			full_from = sip_header_as_string(nh->nh_home, (void *) sip->sip_from);

			if ((p = strchr(to_user, '+'))) {
				switch_copy_string(proto, to_user, sizeof(proto));
				p = strchr(proto, '+');
				*p++ = '\0';

				if ((to_addr = strdup(p))) {
					if ((p = strchr(to_addr, '+'))) {
						*p = '@';
					}
				}
			} else {
				to_addr = switch_mprintf("%s@%s", to_user, to_host);
			}

			from_addr = switch_mprintf("%s@%s", from_user, from_host);

			if (sofia_test_pflag(profile, PFLAG_IN_DIALOG_CHAT)) {
				sofia_presence_set_hash_key(hash_key, sizeof(hash_key), sip);
			}

			if (sofia_test_pflag(profile, PFLAG_IN_DIALOG_CHAT) && (tech_pvt = (private_object_t *) switch_core_hash_find(profile->chat_hash, hash_key))) {
				if (switch_event_create(&event, SWITCH_EVENT_MESSAGE) == SWITCH_STATUS_SUCCESS) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", SOFIA_CHAT_PROTO);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "login", profile->url);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "from", from_addr);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "hint", full_from);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "to", to_addr);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "subject", "SIMPLE MESSAGE");
					if (msg) {
						switch_event_add_body(event, "%s", msg);
					}

					if (switch_core_session_queue_event(tech_pvt->session, &event) != SWITCH_STATUS_SUCCESS) {
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "delivery-failure", "true");
						switch_event_fire(&event);
					}
				}
			} else {
				switch_core_chat_send(proto, SOFIA_CHAT_PROTO, from_addr, to_addr, "", msg, NULL, full_from);
			}
			switch_safe_free(to_addr);
			switch_safe_free(from_addr);
			if (full_from) {
				su_free(nh->nh_home, full_from);
			}
		}
	}
}

void sofia_presence_set_chat_hash(private_object_t *tech_pvt, sip_t const *sip)
{
	char hash_key[256] = "";
	char buf[512];
	su_home_t *home = NULL;

	if (!tech_pvt || tech_pvt->hash_key || !sip || !sip->sip_from || !sip->sip_from->a_url ||
		!sip->sip_from->a_url->url_user || !sip->sip_from->a_url->url_host) {
		return;
	}

	if (sofia_reg_find_reg_url(tech_pvt->profile, sip->sip_from->a_url->url_user, sip->sip_from->a_url->url_host, buf, sizeof(buf))) {
		home = su_home_new(sizeof(*home));
		switch_assert(home != NULL);
		tech_pvt->chat_from = sip_header_as_string(home, (const sip_header_t *) sip->sip_to);
		tech_pvt->chat_to = switch_core_session_strdup(tech_pvt->session, buf);
		sofia_presence_set_hash_key(hash_key, sizeof(hash_key), sip);
		su_home_unref(home);
		home = NULL;
	} else {
		return;
	}

	switch_mutex_lock(tech_pvt->profile->flag_mutex);
	tech_pvt->hash_key = switch_core_session_strdup(tech_pvt->session, hash_key);
	switch_core_hash_insert(tech_pvt->profile->chat_hash, tech_pvt->hash_key, tech_pvt);
	switch_mutex_unlock(tech_pvt->profile->flag_mutex);
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
