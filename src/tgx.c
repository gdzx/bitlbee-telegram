/*
 * Bitblee Telegram Plugin
 *
 * The core of this plugin is a GAsyncQueue containing JSON messages to
 * process. A second thread runs tgx_poll_func to perform the following
 * actions:
 *
 * - Polling for new messages from tdlib with tgx_clt_poll.
 * - Scheduling tgx_evt_handler_func execution on the main thread.
 *
 * On the polling thread, tgx_clt_poll calls tgx_clt_recv to receive a message
 * from tdlib, parses it as JSON, and enqueues it.
 *
 * On the main thread, tgx_evt_handler_func dequeues a message, and calls the
 * event handler to update bitlbee state.
 *
 * Any thread can send a message to tdlib (for example, when bitlbee calls the
 * tgx_bee_buddy_msg callback), but only the polling thread receives the
 * response (that it parses and enqueues for the main thread).
 */

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bitlbee.h>
#include <jansson.h>

#include "tgx.h"

typedef struct im_connection im_connection_t;

typedef struct {
	int uid;

	// JSON messages received from tdlib
	GAsyncQueue *evts;

	// bitlbee handle
	im_connection_t *ic;

	// tdlib handle
	tgx_clt_t *tc;

	// Thread termination
	sig_atomic_t sig;
	pthread_t thread;
} tgx_t;

/**
 * Lifecycle: create the main struct and the polling thread.
 */

static void *tgx_poll_func(void *);
static tgx_t *tgx_new(account_t *);
static int tgx_run(tgx_t *);
static int tgx_poll(tgx_t *, int timeout);
static int tgx_wait(tgx_t *);
static int tgx_exit(tgx_t *);
static void tgx_free(tgx_t *);

/**
 * Client: send or receive a message from tdlib.
 */

static const char *tgx_clt_recv(tgx_t *, int);
static void tgx_clt_send(tgx_t *, const char *);
static int tgx_clt_poll(tgx_t *, int);

/**
 * Events: push / pop events from / to the queue, and process them to update
 * bitlbee state.
 */

static gboolean tgx_evt_handler_func(gpointer, gint, b_input_condition);
static json_t *tgx_evt_pop(tgx_t *);
static void tgx_evt_push(tgx_t *, json_t *);
static int tgx_evt_handler(tgx_t *, json_t *);
static int tgx_evt_update_auth_state(tgx_t *, json_t *);
static int tgx_evt_update_new_message(tgx_t *, json_t *);
static int tgx_evt_update_option(tgx_t *, json_t *);
static int tgx_evt_update_user(tgx_t *, json_t *);
static int tgx_evt_update_user_status(tgx_t *, json_t *);

/**
 * tdlib API requests
 */

static void tgx_api_check_auth_code(tgx_t *t, char *code);
static void tgx_api_check_db_encryption_keys(tgx_t *t);
static void tgx_api_get_chats(tgx_t *t, int64_t order, int64_t offset, int32_t limit);
static void tgx_api_search_contacts(tgx_t *, const char *query, int32_t limit);
static void tgx_api_send_message(tgx_t *, int64_t cid, const char *msg);
static void tgx_api_set_auth_phone_number(tgx_t *t, char *number);
static void tgx_api_set_parameters(tgx_t *t);

/**
 * Bitlbee callbacks
 */

static void tgx_bee_login(account_t *);
static void tgx_bee_logout(im_connection_t *);
static int tgx_bee_buddy_msg(im_connection_t *, char *who, char *message,
			     int away);

/**
 * Lifecycle
 */

/**
 * Polls events from tdlib and schedules the event handler execution.
 *
 * This function runs in the polling thread. It calls tgx_clt_poll with some
 * timeout, and then schedules the execution of tgx_evt_handler_func on Glib's
 * main event loop.
 */
static void *
tgx_poll_func(void *tgx_void)
{
	tgx_t *t = (tgx_t *)tgx_void;

	while (!t->sig) {
		if (!tgx_clt_poll(t, 2)) {
			fprintf(stderr, "polling error\n");
			return NULL;
		}
		b_timeout_add(0, tgx_evt_handler_func, t);
	}

	return NULL;
}

static tgx_t *
tgx_new(account_t *act)
{
	tgx_t *t = g_new0(tgx_t, 1);

	t->evts = g_async_queue_new();

	t->ic = imcb_new(act);
	t->ic->proto_data = t;

	td_set_log_verbosity_level(0);
	t->tc = td_json_client_create();

	t->sig = false;

	return t;
}

static int
tgx_run(tgx_t *t)
{
	if (pthread_create(&t->thread, NULL, tgx_poll_func, t)) {
		fprintf(stderr, "error creating thread\n");
		return false;
	}

	return true;
}

static int
tgx_wait(tgx_t *t)
{
	if (pthread_join(t->thread, NULL)) {
		fprintf(stderr, "error joining thread\n");
		return false;
	}

	return true;
}

static int
tgx_exit(tgx_t *t)
{
	t->sig = true;

	if (pthread_join(t->thread, NULL)) {
		fprintf(stderr, "error joining thread\n");
		return false;
	}

	return true;
}

static void
tgx_free(tgx_t *t)
{
	td_json_client_destroy(t->tc);
	g_async_queue_unref(t->evts);
	g_free(t);
}

/**
 * Client
 */

static void
tgx_clt_send(tgx_t *t, const char *buf)
{
	if (buf)
		printf("> %s\n", buf);

	td_json_client_send(t->tc, buf);
}

static const char *
tgx_clt_recv(tgx_t *t, int tt)
{
	const char *buf = td_json_client_receive(t->tc, tt);

	if (buf)
		printf("< %s\n", buf);

	return buf;
}

/**
 * Receives and enqueues a message from tdlib.
 *
 * - Waits for a message from tdlib with the specified timeout.
 * - Parses the result as JSON.
 * - Enqueues it with tgx_evt_push.
 */
static int
tgx_clt_poll(tgx_t *t, int timeout)
{
	json_t *root;
	json_error_t err;

	const char *msg = tgx_clt_recv(t, timeout);

	if (!msg) {
		return true;
	}

	root = json_loads(msg, 0, &err);
	if (!root) {
		fprintf(stderr, "error: on line %d: %s\n", err.line, err.text);
		return false;
	}

	tgx_evt_push(t, root);
	return true;
}

/**
 * Events
 */

/**
 * Processes all events from the async receive queue.
 *
 * Runs on the main thread, since most event handling functions use the bitlbee
 * API (not thread-safe).
 */
static gboolean
tgx_evt_handler_func(gpointer data, gint __attribute__((unused)) fd,
		     b_input_condition __attribute__((unused)) cond)
{
	tgx_t *t = data;
	json_t *root;

	while ((root = tgx_evt_pop(t)) != NULL) {
		if (!tgx_evt_handler(t, root)) {
			fprintf(stderr, "event handling error\n");
		}
		json_decref(root);
	}

	return false;
}

/**
 * Pops a JSON message from the async receive queue.
 */
static json_t *
tgx_evt_pop(tgx_t *t)
{
	return g_async_queue_try_pop(t->evts);
}

/**
 * Pushes a JSON message to the async receive queue.
 */
static void
tgx_evt_push(tgx_t *t, json_t *j)
{
	g_async_queue_push(t->evts, j);
}

/**
 * Forwards an event to the matching handler.
 */
static int
tgx_evt_handler(tgx_t *t, json_t *root)
{
	json_t *type_obj = json_object_get(root, "@type");
	assert(json_is_string(type_obj));

	const char *type = json_string_value(type_obj);
	if (strcmp(type, "updateAuthorizationState") == 0) {
		return tgx_evt_update_auth_state(t, root);
	} else if (strcmp(type, "updateNewMessage") == 0) {
		return tgx_evt_update_new_message(t, root);
	} else if (strcmp(type, "updateOption") == 0) {
		return tgx_evt_update_option(t, root);
	} else if (strcmp(type, "updateUser") == 0) {
		return tgx_evt_update_user(t, root);
	} else if (strcmp(type, "updateUserStatus") == 0) {
		return tgx_evt_update_user_status(t, root);
	} else {
		fprintf(stderr, "error: unhandled event %s\n", type);
	}

	return true;
}

/**
 * Authenticates Bitlbee with tdlib.
 *
 * (Reads information directly from stdin.)
 */
static int
tgx_evt_update_auth_state(tgx_t *t, json_t *root)
{
	json_t *auth_state = json_object_get(root, "authorization_state");
	assert(json_is_object(auth_state));

	json_t *type_obj = json_object_get(auth_state, "@type");
	assert(json_is_string(type_obj));

	const char *state = json_string_value(type_obj);
	if (strcmp(state, "authorizationStateWaitTdlibParameters") == 0) {
		tgx_api_set_parameters(t);
	} else if (strcmp(state, "authorizationStateWaitEncryptionKey") == 0) {
		tgx_api_check_db_encryption_keys(t);
	} else if (strcmp(state, "authorizationStateWaitPhoneNumber") == 0) {
		char *line = NULL;
		size_t len = 0;
		ssize_t slen = 0;

		printf("Enter phone number: ");
		if ((slen = getline(&line, &len, stdin)) == -1) {
			fprintf(stderr, "error: cannot retrieve password\n");
			return false;
		}
		line[slen - 1] = '\0';

		tgx_api_set_auth_phone_number(t, line);
		g_free(line);
	} else if (strcmp(state, "authorizationStateWaitCode") == 0) {
		char *line = NULL;
		size_t len = 0;
		ssize_t slen = 0;

		printf("Enter code: ");
		if ((slen = getline(&line, &len, stdin)) == -1) {
			fprintf(stderr, "error: cannot retrieve code\n");
			return false;
		}
		line[slen - 1] = '\0';

		tgx_api_check_auth_code(t, line);
		g_free(line);
	} else if (strcmp(state, "authorizationStateReady") == 0) {
		imcb_connected(t->ic);
		tgx_api_search_contacts(t, "", 10);
		tgx_api_get_chats(t, INT64_MAX, 0, 10);
	} else {
		fprintf(stderr, "error: unhandled authorization state %s\n",
			state);
		return false;
	}

	return true;
}

static int
tgx_evt_update_new_message(tgx_t *t, json_t *root)
{
	json_t *msg_obj = json_object_get(root, "message");
	assert(json_is_object(msg_obj));

	json_t *ss_obj = json_object_get(msg_obj, "sending_state");
	if (ss_obj != NULL) {
		return true;
	}

	json_t *sid_attr = json_object_get(msg_obj, "sender_user_id");
	assert(json_is_integer(sid_attr));

	json_t *cid_attr = json_object_get(msg_obj, "chat_id");
	assert(json_is_integer(cid_attr));

	json_t *content_obj = json_object_get(msg_obj, "content");
	assert(json_is_object(content_obj));

	json_t *text_obj = json_object_get(content_obj, "text");
	if (text_obj == NULL) {
		return true;
	}
	assert(json_is_object(text_obj));

	json_t *text_attr = json_object_get(text_obj, "text");
	assert(json_is_string(text_attr));

	char *handle;
	bee_user_t *bu;

	if (json_integer_value(sid_attr) == t->uid) {
		handle = g_strdup_printf("%lli", json_integer_value(cid_attr));
		bu = bee_user_by_handle(t->ic->bee, t->ic, handle);
		if (bu != NULL) {
			imcb_buddy_msg(t->ic, bu->handle,
				       json_string_value(text_attr),
				       OPT_SELFMESSAGE, 0);
		}
	} else {
		handle = g_strdup_printf("%lli", json_integer_value(sid_attr));
		bu = bee_user_by_handle(t->ic->bee, t->ic, handle);
		if (bu != NULL) {
			imcb_buddy_msg(t->ic, bu->handle,
				       json_string_value(text_attr), 0, 0);
		}
	}

	g_free(handle);
	return true;
}

static int
tgx_evt_update_option(tgx_t *t, json_t *root)
{
	json_t *name_attr = json_object_get(root, "name");
	assert(json_is_string(name_attr));
	if (strcmp("my_id", json_string_value(name_attr)) == 0) {
		json_t *value_obj = json_object_get(root, "value");
		assert(json_is_object(value_obj));

		json_t *value_attr = json_object_get(value_obj, "value");
		assert(json_is_integer(value_attr));

		t->uid = json_integer_value(value_attr);
	}

	return true;
}

static int
tgx_evt_update_user(tgx_t *t, json_t *root)
{
	json_t *user_obj = json_object_get(root, "user");
	assert(json_is_object(user_obj));

	json_t *type_obj = json_object_get(user_obj, "type");
	assert(json_is_object(type_obj));

	json_t *type_attr = json_object_get(type_obj, "@type");
	assert(json_is_string(type_attr));

	if (strcmp("userTypeRegular", json_string_value(type_attr)) != 0) {
		return true;
	}

	json_t *id_attr = json_object_get(user_obj, "id");
	assert(json_is_integer(id_attr));

	json_t *name_attr = json_object_get(user_obj, "username");
	assert(json_is_string(name_attr));

	json_t *firstname_attr = json_object_get(user_obj, "first_name");
	assert(json_is_string(firstname_attr));
	const char *firstname = json_string_value(firstname_attr);
	if (strcmp("", firstname) == 0) {
		firstname = NULL;
	}

	json_t *lastname_attr = json_object_get(user_obj, "last_name");
	assert(json_is_string(lastname_attr));
	const char *lastname = json_string_value(lastname_attr);
	if (strcmp("", lastname) == 0) {
		lastname = NULL;
	}

	char *name;
	if (strcmp("", json_string_value(name_attr)) != 0) {
		name = g_strdup(json_string_value(name_attr));
	} else {
		name = g_strjoin(" ", firstname, lastname, NULL);
	}

	char *handle = g_strdup_printf("%lli", json_integer_value(id_attr));

	bee_user_t *bu = bee_user_by_handle(t->ic->bee, t->ic, handle);
	if (bu == NULL) {
		imcb_add_buddy(t->ic, handle, NULL);
		imcb_buddy_nick_hint(t->ic, handle, name);
		imcb_buddy_status(t->ic, handle, OPT_LOGGED_IN | OPT_AWAY, NULL,
				  NULL);
	}

	g_free(handle);
	g_free(name);

	return true;
}

static int
tgx_evt_update_user_status(tgx_t *t, json_t *root)
{
	json_t *uid_attr = json_object_get(root, "user_id");
	assert(json_is_integer(uid_attr));

	json_t *status_obj = json_object_get(root, "status");
	assert(json_is_object(status_obj));

	json_t *type_attr = json_object_get(status_obj, "@type");
	assert(json_is_string(type_attr));

	bool online = false;
	if (strcmp(json_string_value(type_attr), "userStatusOnline") == 0) {
		online = true;
	}

	char *handle = g_strdup_printf("%lli", json_integer_value(uid_attr));
	bee_user_t *bu = bee_user_by_handle(t->ic->bee, t->ic, handle);
	if (bu != NULL) {
		if (online) {
			imcb_buddy_status(t->ic, bu->handle, OPT_LOGGED_IN,
					  NULL, NULL);
		} else {
			imcb_buddy_status(t->ic, bu->handle,
					  OPT_LOGGED_IN | OPT_AWAY, NULL, NULL);
		}
	}

	g_free(handle);
	return true;
}

/**
 * tdlib API requests
 */

static void
tgx_api_check_auth_code(tgx_t *t, char *code)
{
	char *req;
	json_t *root = json_object();

	json_object_set_new(root, "@type",
			    json_string("checkAuthenticationCode"));
	json_object_set_new(root, "code", json_string(code));

	req = json_dumps(root, JSON_COMPACT);
	json_decref(root);

	tgx_clt_send(t, req);
	g_free(req);
}

static void
tgx_api_check_db_encryption_keys(tgx_t *t)
{
	char *req;
	json_t *root = json_object();

	json_object_set_new(root, "@type",
			    json_string("checkDatabaseEncryptionKey"));

	req = json_dumps(root, JSON_COMPACT);
	json_decref(root);

	tgx_clt_send(t, req);
	g_free(req);
}

static void
tgx_api_get_chats(tgx_t *t, int64_t order, int64_t offset, int32_t limit)
{
	char *req;
	json_t *root = json_object();

	json_object_set_new(root, "@type", json_string("getChats"));
	json_object_set_new(root, "offset_order", json_integer(order));
	json_object_set_new(root, "offset_chat_id", json_integer(offset));
	json_object_set_new(root, "limit", json_integer(limit));

	req = json_dumps(root, JSON_COMPACT);
	json_decref(root);

	tgx_clt_send(t, req);
	g_free(req);
}

static void
tgx_api_search_contacts(tgx_t *t, const char *query, int32_t limit)
{
	char *req;
	json_t *root = json_object();

	json_object_set_new(root, "@type", json_string("searchContacts"));
	json_object_set_new(root, "query", json_string(query));
	json_object_set_new(root, "limit", json_integer(limit));

	req = json_dumps(root, JSON_COMPACT);
	json_decref(root);

	tgx_clt_send(t, req);
	g_free(req);
}

static void
tgx_api_send_message(tgx_t *t, int64_t chat_id, const char *msg)
{
	char *req;
	json_t *root = json_object();
	json_t *content = json_object();
	json_t *text = json_object();

	json_object_set_new(root, "@type", json_string("sendMessage"));
	json_object_set_new(root, "chat_id", json_integer(chat_id));
	json_object_set_new(root, "reply_to_message", json_integer(0));
	json_object_set_new(root, "input_message_content", content);

	json_object_set_new(content, "@type", json_string("inputMessageText"));
	json_object_set_new(content, "text", text);

	json_object_set_new(text, "text", json_string(msg));

	req = json_dumps(root, JSON_COMPACT);
	json_decref(root);

	tgx_clt_send(t, req);
	g_free(req);
}

static void
tgx_api_set_auth_phone_number(tgx_t *t, char *number)
{
	char *req;
	json_t *root = json_object();

	json_object_set_new(root, "@type",
			    json_string("setAuthenticationPhoneNumber"));
	json_object_set_new(root, "phone_number", json_string(number));

	req = json_dumps(root, JSON_COMPACT);
	json_decref(root);

	tgx_clt_send(t, req);
	g_free(req);
}

static void
tgx_api_set_parameters(tgx_t *t)
{
	char *req;
	json_t *root = json_object();
	json_t *parameters = json_object();

	json_object_set_new(root, "@type", json_string("setTdlibParameters"));
	json_object_set_new(root, "parameters", parameters);

	json_object_set_new(parameters, "@type",
			    json_string("tdlibParameters"));
	json_object_set_new(parameters, "api_id", json_string(TD_API_KEY));
	json_object_set_new(parameters, "api_hash", json_string(TD_API_HASH));
	json_object_set_new(parameters, "database_directory",
			    json_string("/tmp/tdata"));
	json_object_set_new(parameters, "device_model", json_string("server"));
	json_object_set_new(parameters, "system_language_code",
			    json_string("en"));
	json_object_set_new(parameters, "system_version",
			    json_string("unknown"));

	req = json_dumps(root, JSON_COMPACT);
	json_decref(root);

	tgx_clt_send(t, req);
	g_free(req);
}

/**
 * Bitlbee callbacks
 */

static void
tgx_bee_login(account_t *a)
{
	tgx_t *t = tgx_new(a);

	imcb_log(t->ic, "Connecting");

	tgx_run(t);
}

static void
tgx_bee_logout(struct im_connection *ic)
{
	tgx_t *t = ic->proto_data;

	tgx_exit(t);

	imcb_log(t->ic, "Disconnected");

	tgx_free(t);
}

static int
tgx_bee_buddy_msg(struct im_connection *ic, char *who, char *message,
		  int __attribute__((unused)) away)
{
	tgx_t *t = ic->proto_data;

	tgx_api_send_message(t, atoi(who), message);

	return 0;
}

#ifdef BITLBEE_ABI_VERSION_CODE
struct plugin_info *
init_plugin_info(void)
{
	static struct plugin_info info = {
	    BITLBEE_ABI_VERSION_CODE,
	    "bitlbee-telegram",
	    "0.1.0",
	    "Bitlbee plugin for Telegram",
	    "gdzx <gdzx@gmx.com>",
	    "https://github.com/gdzx/bitlbee-telegram"};

	return &info;
}
#endif

G_MODULE_EXPORT void
init_plugin(void)
{
	static const struct prpl pp = {.name = "telegram",
				       .login = tgx_bee_login,
				       .logout = tgx_bee_logout,
				       .buddy_msg = tgx_bee_buddy_msg,
				       .handle_cmp = g_ascii_strcasecmp};

	register_protocol(g_memdup(&pp, sizeof pp));
}
