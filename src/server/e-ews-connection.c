/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Authors :
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <libical/icalcomponent.h>
#include <libical/icalproperty.h>
#include <libical/ical.h>
#include "e-ews-connection.h"
#include <libedataserver/e-flag.h>
#include "e-ews-message.h"
#include "e-ews-item-change.h"
#include "ews-marshal.h"

#define d(x) x

/* For the number of connections */
#define EWS_CONNECTION_MAX_REQUESTS 10

#define QUEUE_LOCK(x) (g_static_rec_mutex_lock(&(x)->priv->queue_lock))
#define QUEUE_UNLOCK(x) (g_static_rec_mutex_unlock(&(x)->priv->queue_lock))

G_DEFINE_TYPE (EEwsConnection, e_ews_connection, G_TYPE_OBJECT)

struct _EwsNode;
static GObjectClass *parent_class = NULL;
static GStaticMutex connecting = G_STATIC_MUTEX_INIT;
static GHashTable *loaded_connections_permissions = NULL;
static gboolean ews_next_request (gpointer _cnc);
static gint comp_func (gconstpointer a, gconstpointer b);

typedef void (*response_cb) (ESoapParameter *param, struct _EwsNode *enode);
static void ews_response_cb (SoupSession *session, SoupMessage *msg, gpointer data);

static void
ews_connection_authenticate	(SoupSession *sess, SoupMessage *msg,
				 SoupAuth *auth, gboolean retrying,
				 gpointer data);

/* Connection APIS */

struct _EEwsConnectionPrivate {
	SoupSession *soup_session;
	GThread *soup_thread;
	GMainLoop *soup_loop;
	GMainContext *soup_context;

	gchar *uri;
	gchar *username;
	gchar *password;
	gchar *email;

	GSList *jobs;
	GSList *active_job_queue;
	GStaticRecMutex queue_lock;
};

enum {
	AUTHENTICATE,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

typedef struct _EwsNode EwsNode;
typedef struct _EwsAsyncData EwsAsyncData;

struct _EwsAsyncData {
	GSList *items_created;
	GSList *items_updated;
	GSList *items_deleted;

	gint total_items;
	const gchar *directory;
	GSList *items;
	gchar *sync_state;
	gboolean includes_last_item;
};

struct _EwsNode {
	ESoapMessage *msg;
	EEwsConnection *cnc;
	GSimpleAsyncResult *simple;
	gboolean complete_sync;

	gint pri;		/* the command priority */
	response_cb cb;

	GCancellable *cancellable;
	gulong cancel_handler_id;
};

typedef struct {
  GAsyncResult *res;
  EFlag *eflag;
} EwsSyncData;

/* Static Functions */

GQuark
ews_connection_error_quark (void)
{
	static GQuark quark = 0;

	if (G_UNLIKELY (quark == 0)) {
		const gchar *string = "ews-connection-error-quark";
		quark = g_quark_from_static_string (string);
	}

	return quark;
}

static void
async_data_free (EwsAsyncData *async_data)
{
	g_free (async_data);
}

static void
ews_sync_reply_cb	(GObject *object,
			 GAsyncResult *res,
			 gpointer user_data)
{

  EwsSyncData *sync_data = user_data;

  sync_data->res = g_object_ref (res);
  e_flag_set (sync_data->eflag);
}

static EwsNode *
ews_node_new ()
{
	EwsNode *node;

	node = g_new0 (EwsNode, 1);
	return node;
}

static gboolean
autodiscover_parse_protocol (xmlNode *node, EwsUrls *urls)
{
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "ASUrl")) {
			urls->as_url = (gchar *) xmlNodeGetContent(node);
		} else if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "OABUrl"))
			urls->oab_url = (gchar *) xmlNodeGetContent(node);
			
		if (urls->as_url && urls->oab_url)
			return TRUE;
	}

	return FALSE;
}

static gint
comp_func (gconstpointer a, gconstpointer b)
{
	EwsNode *node1 = (EwsNode *) a;
	EwsNode *node2 = (EwsNode *) b;

	if (node1->pri > node2->pri)
		return -1;
	else if (node1->pri < node2->pri)
		return 1;
	else
		return 0;
}

static void
ews_parse_soap_fault (ESoapResponse *response, GError **error)
{
	ESoapParameter *param;
	gchar *faultstring = NULL;

	param = e_soap_response_get_first_parameter_by_name(response, "faultstring");
	if (param)
		faultstring = e_soap_parameter_get_string_value(param);


	g_set_error (error, EWS_CONNECTION_ERROR, EWS_CONNECTION_ERROR_UNKNOWN,
		     "%s", faultstring?:"No <ResponseMessages> or <FreeBusyResponseArray> or SOAP <faultstring> in response");

	g_free(faultstring);
}

static gboolean
ews_get_response_status (ESoapParameter *param, GError **error)
{
	ESoapParameter *subparam;
	gchar *value;
	gboolean ret = TRUE;

	value = e_soap_parameter_get_property (param, "ResponseClass");

	if (!g_ascii_strcasecmp (value, "Error")) {
		gchar *desc, *res;
		gint error_code = EWS_CONNECTION_ERROR_UNKNOWN;

		subparam = e_soap_parameter_get_first_child_by_name (param, "MessageText");
		desc = e_soap_parameter_get_string_value (subparam);

		subparam = e_soap_parameter_get_first_child_by_name (param, "ResponseCode");
		res = e_soap_parameter_get_string_value (subparam);

		error_code = ews_get_error_code ((const gchar *) res);

		/* FIXME: This happens because of a bug in the Exchange server,
		   which doesn't like returning <Recurrence> for any appointment
		   without a timezone, even if it's an all day event like a
		   birthday. We need to handle the error and correctly report it
		   to the user, but for now we'll just ignore it... */
		if (error_code != EWS_CONNECTION_ERROR_CORRUPTDATA &&
		/* Ick, another one. If we try to set the IsRead flag on certain
		   types of item (task requests, those stupid 'recall' requests),
		   it complains. We really need to find a better way to return
		   individual errors for each response to a multiple request; it
		   isn't necessarily the case that a single error should be reported
		   as an error for the whole transaction */
		    error_code != EWS_CONNECTION_ERROR_INVALIDPROPERTYREQUEST) {
			g_set_error	(error,
					 EWS_CONNECTION_ERROR,
					 error_code,
					 "%s", desc);
			ret = FALSE;
		}

		g_free (desc);
		g_free (res);
	}

	g_free (value);

	return ret;
}

static gboolean
ews_next_request (gpointer _cnc)
{
	EEwsConnection *cnc = _cnc;
	GSList *l;
	EwsNode *node;

	QUEUE_LOCK (cnc);

	l = cnc->priv->jobs;

	if (!l || g_slist_length (cnc->priv->active_job_queue) >= EWS_CONNECTION_MAX_REQUESTS) {
		QUEUE_UNLOCK (cnc);
		return FALSE;
	}

	node = (EwsNode *) l->data;

	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 1)) {
		soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (node->msg)->request_body));
		/* print request's body */
		printf ("\n The request headers");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (node->msg)->request_body->data, stdout);
		fputc ('\n', stdout);
	}

	/* Remove the node from the priority queue */
	cnc->priv->jobs = g_slist_remove (cnc->priv->jobs, (gconstpointer *) node);

	/* Add to active job queue */
	cnc->priv->active_job_queue = g_slist_append (cnc->priv->active_job_queue, node);

	soup_session_queue_message (cnc->priv->soup_session, SOUP_MESSAGE (node->msg), ews_response_cb, node);

	QUEUE_UNLOCK (cnc);
	return FALSE;
}

static void ews_trigger_next_request(EEwsConnection *cnc)
{
	GSource *source;

	source = g_idle_source_new ();
	g_source_set_priority (source, G_PRIORITY_DEFAULT);
	g_source_set_callback (source, ews_next_request, cnc, NULL);
	g_source_attach (source, cnc->priv->soup_context);
}

/**
 * ews_active_job_done
 * @cnc:
 * @msg:
 * Removes the node from active Queue and free's the node
 *
 * Returns:
 **/
static void
ews_active_job_done (EEwsConnection *cnc, EwsNode *ews_node)
{
	QUEUE_LOCK (cnc);

	cnc->priv->active_job_queue = g_slist_remove (cnc->priv->active_job_queue, ews_node);
	if (ews_node->cancellable)
		g_signal_handler_disconnect (ews_node->cancellable, ews_node->cancel_handler_id);

	QUEUE_UNLOCK (cnc);

	ews_trigger_next_request(cnc);
	g_object_unref (ews_node->simple);
	g_free (ews_node);
}

static void
ews_cancel_request (GCancellable *cancellable,
		   gpointer user_data)
{
	EwsNode *node = user_data;
	EEwsConnection *cnc = node->cnc;
	GSimpleAsyncResult *simple = node->simple;
	ESoapMessage *msg = node->msg;
	GSList *found;

	QUEUE_LOCK (cnc);
	found = g_slist_find (cnc->priv->active_job_queue, node);
	QUEUE_UNLOCK (cnc);

	g_simple_async_result_set_error	(simple,
			EWS_CONNECTION_ERROR,
			EWS_CONNECTION_ERROR_CANCELLED,
			_("Operation Cancelled"));
	if (found)
		soup_session_cancel_message (cnc->priv->soup_session, SOUP_MESSAGE (msg), SOUP_STATUS_CANCELLED);
	else {
		QUEUE_LOCK (cnc);
		cnc->priv->jobs = g_slist_remove (cnc->priv->jobs, (gconstpointer) node);
		QUEUE_UNLOCK (cnc);

		ews_response_cb (cnc->priv->soup_session, SOUP_MESSAGE (msg), node);
	}
}

static void
ews_connection_queue_request (EEwsConnection *cnc, ESoapMessage *msg, response_cb cb, gint pri, GCancellable *cancellable, GSimpleAsyncResult *simple, gboolean complete_sync)
{
	EwsNode *node;

	node = ews_node_new ();
	node->msg = msg;
	node->pri = pri;
	node->cb = cb;
	node->cnc = cnc;
	node->complete_sync = complete_sync;
	node->simple = simple;

	QUEUE_LOCK (cnc);
	cnc->priv->jobs = g_slist_insert_sorted (cnc->priv->jobs, (gpointer *) node, (GCompareFunc) comp_func);
 	QUEUE_UNLOCK (cnc);

	if (cancellable) {
		node->cancellable = cancellable;
		node->cancel_handler_id = g_cancellable_connect	(cancellable,
								 G_CALLBACK (ews_cancel_request),
								 (gpointer) node, NULL);
	}

	ews_trigger_next_request(cnc);
}

/* Response callbacks */

static void
ews_response_cb (SoupSession *session, SoupMessage *msg, gpointer data)
{
	EwsNode *enode = (EwsNode *) data;
	ESoapResponse *response;

	if (enode->cancellable && g_cancellable_is_cancelled (enode->cancellable))
		goto exit;

	if (msg->status_code == SOUP_STATUS_UNAUTHORIZED) {
		g_simple_async_result_set_error (enode->simple,
						 EWS_CONNECTION_ERROR,
						 EWS_CONNECTION_ERROR_AUTHENTICATION_FAILED,
						 _("Authentication failed"));
		goto exit;
	}
	response = e_soap_message_parse_response ((ESoapMessage *) msg);
	if (!response) {
		g_simple_async_result_set_error	(enode->simple,
						 EWS_CONNECTION_ERROR,
						 EWS_CONNECTION_ERROR_NORESPONSE,
						 _("No response: %s"), msg->reason_phrase);
	} else {
		ESoapParameter *param, *subparam;
		GError *error = NULL;

		/* TODO: The stdout can be replaced with Evolution's
		   Logging framework also */
		if (response && g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 1))
			e_soap_response_dump_response (response, stdout);

		param = e_soap_response_get_first_parameter_by_name (response, "ResponseMessages");
		if (!param) 
			param = e_soap_response_get_first_parameter_by_name (response, "FreeBusyResponseArray");

		if (param) {
			/* Iterate over all "*ResponseMessage" elements. */
			for (subparam = e_soap_parameter_get_first_child (param);
			     subparam;
			     subparam = e_soap_parameter_get_next_child (subparam)) {
				int l = strlen ((char *)subparam->name);
				if (l < 15 || (strcmp((char *)subparam->name + l - 15, "ResponseMessage") &&
				    strcmp((char *)subparam->name, "FreeBusyResponse")&&
						strcmp((char *)subparam->name, "DelegateUserResponseMessageType"))) {
					g_warning ("Unexpected element '%s' in place of ResponseMessage or FreeBusyResponse or DelegateUserResponseMessageType",
						   subparam->name);
					continue;
				}

				if ((strcmp((char *)subparam->name, "FreeBusyResponse") == 0 && !ews_get_response_status (e_soap_parameter_get_first_child (subparam), &error)) ||
				 (strcmp((char *)subparam->name, "FreeBusyResponse") && !ews_get_response_status (subparam, &error))) {
					g_simple_async_result_set_from_error (enode->simple, error);
					break;
				}
				if (enode->cb)
					enode->cb (subparam, enode);
			}
		} else if ((param = e_soap_response_get_first_parameter_by_name (response, "ResponseMessage"))) {
			/*Parse GetUserOofSettingsResponse and SetUserOofSettingsResponse*/
			if (!ews_get_response_status (param, &error)) {
					g_simple_async_result_set_from_error (enode->simple, error);
			} else {
				subparam = e_soap_parameter_get_next_child (param);
				if (enode->cb)
					enode->cb (subparam, enode);
			}
		} else
			ews_parse_soap_fault (response, &error);

		if (error) {
			g_simple_async_result_set_from_error (enode->simple, error);
			g_clear_error (&error);
		}

		g_object_unref (response);
	}

exit:
	if (enode->complete_sync) {
		GAsyncResult *async = G_ASYNC_RESULT (enode->simple);

		/* If we just call g_simple_async_result_complete() then it
		   will bitch about being called in the wrong context, even
		   though we *know* it's OK. So instead, just call the
		   callback directly. We *know* it's ews_sync_reply_cb(),
		   because that's the only way the complete_sync flag gets
		   set */
		ews_sync_reply_cb (NULL, async, g_async_result_get_user_data (async));
	} else {
		g_simple_async_result_complete_in_idle (enode->simple);
	}
	ews_active_job_done (enode->cnc, enode);
}

typedef gpointer (*ItemParser) (ESoapParameter *param);

static void
sync_xxx_response_cb (ESoapParameter *subparam, EwsNode *enode, ItemParser parser,
		      const gchar *last_tag, const gchar *delete_id_tag)
{
	ESoapParameter *node;
	EwsAsyncData *async_data;
	gchar *new_sync_state = NULL, *value, *last;
	GSList *items_created = NULL, *items_updated = NULL, *items_deleted = NULL;
	gboolean includes_last_item = FALSE;

	node = e_soap_parameter_get_first_child_by_name (subparam, "SyncState");
	new_sync_state = e_soap_parameter_get_string_value (node);

	node = e_soap_parameter_get_first_child_by_name (subparam, last_tag);
	last = e_soap_parameter_get_string_value (node);
	if (!strcmp (last, "true"))
		includes_last_item = TRUE;
	g_free (last);

	node = e_soap_parameter_get_first_child_by_name (subparam, "Changes");

	if (node) {
		ESoapParameter *subparam1;
		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Create");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Create")) {
			EEwsFolder *folder;

			folder = parser (subparam1);
			items_created = g_slist_append (items_created, folder);
		}

		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Update");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Update")) {
			EEwsFolder *folder;

			folder = parser (subparam1);
			items_updated = g_slist_append (items_updated, folder);
		}
		  /* Exchange 2007SP1 introduced <ReadFlagChange> which is basically identical
		   * to <Update>; no idea why they thought it was a good idea. */
		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "ReadFlagChange");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "ReadFlagChange")) {
			EEwsFolder *folder;

			folder = parser (subparam1);
			items_updated = g_slist_append (items_updated, folder);
		}

		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Delete");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Delete")) {
			ESoapParameter *folder_param;

			folder_param = e_soap_parameter_get_first_child_by_name (subparam1, delete_id_tag);
			value = e_soap_parameter_get_property (folder_param, "Id");
			items_deleted = g_slist_append (items_deleted, value);
		}
	}

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);
	async_data->items_created = items_created;
	async_data->items_updated = items_updated;
	async_data->items_deleted = items_deleted;
	async_data->sync_state = new_sync_state;
	async_data->includes_last_item = includes_last_item;
}

static void
sync_hierarchy_response_cb (ESoapParameter *subparam, EwsNode *enode)
{
	sync_xxx_response_cb (subparam, enode, (ItemParser)e_ews_folder_new_from_soap_parameter,
			      "IncludesLastFolderInRange", "FolderId");
}

static void
sync_folder_items_response_cb (ESoapParameter *subparam, EwsNode *enode)
{
	sync_xxx_response_cb (subparam, enode, (ItemParser) e_ews_item_new_from_soap_parameter,
			      "IncludesLastItemInRange", "ItemId");
}

static void
get_folder_response_cb  (ESoapParameter *subparam, EwsNode *enode)
{	ESoapParameter *node;
	EwsAsyncData *async_data;
	EEwsFolder *folder;

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	for (node = e_soap_parameter_get_first_child_by_name (subparam, "Folders");
	     node; node = e_soap_parameter_get_next_child_by_name (subparam, "Folders")) {
		folder = e_ews_folder_new_from_soap_parameter (node);
		if (!folder) continue;
		async_data->items = g_slist_append (async_data->items, folder);
	}
}

static void 
find_folder_items_response_cb (ESoapParameter *subparam, EwsNode *enode)
{
	ESoapParameter *node, *subparam1;
	EwsAsyncData *async_data;
	gchar *last, *total;
	gint total_items;
	EEwsItem *item;
	gboolean includes_last_item = FALSE;

	node = e_soap_parameter_get_first_child_by_name (subparam, "RootFolder");
	total = e_soap_parameter_get_property (node, "TotalItemsInView");
	total_items = atoi (total);
	g_free (total);
	last = e_soap_parameter_get_property (node, "IncludesLastItemInRange");
	if (!strcmp (last, "true"))
		includes_last_item = TRUE;
	g_free (last);

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	node = e_soap_parameter_get_first_child_by_name (node, "Items");
	for (subparam1 = e_soap_parameter_get_first_child (node);
	     subparam1; subparam1 = e_soap_parameter_get_next_child (subparam1)) {
		item = e_ews_item_new_from_soap_parameter (subparam1);
		if (!item) continue;
		async_data->items = g_slist_append (async_data->items, item);
	}
	async_data->total_items = total_items;
	async_data->includes_last_item = includes_last_item;
}

/* Used for CreateItems and GetItems */
static void
get_items_response_cb (ESoapParameter *subparam, EwsNode *enode)
{
	ESoapParameter *node;
	EwsAsyncData *async_data;
	EEwsItem *item;

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	for (node = e_soap_parameter_get_first_child_by_name (subparam, "Items");
	     node; node = e_soap_parameter_get_next_child_by_name (subparam, "Items")) {
		item = e_ews_item_new_from_soap_parameter (node);
		if (!item) continue;
		async_data->items = g_slist_append (async_data->items, item);
	}
}

static gchar *
get_text_from_html (gchar *html_text)
{
	gssize haystack_len = strlen (html_text);
	gchar *plain_text, *ret_text;
	gchar *start = g_strstr_len (html_text, haystack_len, "<body"),
		*end = g_strstr_len (html_text, haystack_len, "</body>"),
		*i, *j;

	plain_text = g_malloc (end - start);
	i = start;
	for (j = plain_text; i < end; i++) {
		if (*i == '<') {
			while (*i != '>')
				i++;
		} else {
			*j = *i;
			j++;
		}
	}

	*j = '\0';
	ret_text = g_strdup (plain_text);

	g_free (html_text);
	g_free (plain_text);

	return ret_text;
}

static void
get_oof_settings_response_cb (ESoapParameter *subparam, EwsNode *enode)
{
	ESoapParameter *node, *node_1;
	EwsAsyncData *async_data;
	OOFSettings *oof_settings;
	gchar *state = NULL, *ext_aud = NULL;
	gchar *start_tm = NULL, *end_tm = NULL;
	gchar *ext_msg = NULL, *int_msg = NULL;
	GTimeVal time_val;

	node = e_soap_parameter_get_first_child_by_name (subparam, "OofState");
	state = e_soap_parameter_get_string_value (node);

	node = e_soap_parameter_get_first_child_by_name (subparam, "ExternalAudience");
	ext_aud = e_soap_parameter_get_string_value (node);

	node = e_soap_parameter_get_first_child_by_name (subparam, "Duration");

	node_1 = e_soap_parameter_get_first_child_by_name (node, "StartTime");
	start_tm = e_soap_parameter_get_string_value (node_1);

	node_1 = e_soap_parameter_get_first_child_by_name (node, "EndTime");
	end_tm = e_soap_parameter_get_string_value (node_1);

	node = e_soap_parameter_get_first_child_by_name (subparam, "InternalReply");
	node_1 = e_soap_parameter_get_first_child_by_name (node, "Message");
	int_msg = e_soap_parameter_get_string_value (node_1);
	if (g_strrstr (int_msg, "</body>"))
		int_msg = get_text_from_html (int_msg);

	node = e_soap_parameter_get_first_child_by_name (subparam, "ExternalReply");
	node_1 = e_soap_parameter_get_first_child_by_name (node, "Message");
	ext_msg = e_soap_parameter_get_string_value (node_1);
	if (g_strrstr (ext_msg, "</body>"))
		ext_msg = get_text_from_html (ext_msg);

	oof_settings = g_new0 (OOFSettings, 1);

	oof_settings->state = state;
	oof_settings->ext_aud = ext_aud;

	if (g_time_val_from_iso8601 (start_tm, &time_val))
		oof_settings->start_tm = time_val.tv_sec;

	if (g_time_val_from_iso8601 (end_tm, &time_val))
		oof_settings->end_tm = time_val.tv_sec;
	
	oof_settings->int_reply = int_msg;
	oof_settings->ext_reply = ext_msg;


	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);
	async_data->items = g_slist_append (async_data->items, oof_settings);

	g_free (start_tm);
	g_free (end_tm);
}

static void
resolve_names_response_cb (ESoapParameter *subparam, EwsNode *enode)
{
	ESoapParameter *node;
	gboolean includes_last_item;
	GSList *mailboxes = NULL, *contact_items = NULL;
	EwsAsyncData *async_data;
	gchar *prop;

	subparam = e_soap_parameter_get_first_child_by_name (subparam, "ResolutionSet");
	prop = e_soap_parameter_get_property (subparam, "IncludesLastItemInRange");

	if (prop && !strcmp (prop, "true"))
		includes_last_item = TRUE;
	g_free (prop);

	for (subparam = e_soap_parameter_get_first_child_by_name (subparam, "Resolution");
		subparam != NULL;
		subparam = e_soap_parameter_get_next_child_by_name (subparam, "Resolution")) {
		EwsMailbox *mb;

		node = e_soap_parameter_get_first_child_by_name (subparam, "Mailbox");
		mb = e_ews_item_mailbox_from_soap_param (node);
		if (mb) {
			EwsResolveContact *rc;

			mailboxes = g_slist_prepend (mailboxes, mb);

			/* 'mailboxes' and 'contact_items' match 1:1, but if the contact information
			   wasn't found, then NULL is stored in the corresponding position */
			node = e_soap_parameter_get_first_child_by_name (subparam, "Contact");
			rc = e_ews_item_resolve_contact_from_soap_param (node);
			contact_items = g_slist_prepend (contact_items, rc);
		}
	}

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	/* Reuse existing variables */
	async_data->items = g_slist_reverse (mailboxes);
	async_data->includes_last_item = includes_last_item;
	async_data->items_created = g_slist_reverse (contact_items);
}

static void
expand_dl_response_cb (ESoapParameter *subparam, EwsNode *enode)
{
	gboolean includes_last_item;
	GSList *mailboxes = NULL;
	EwsAsyncData *async_data;
	gchar *prop;

	subparam = e_soap_parameter_get_first_child_by_name (subparam, "DLExpansion");
	prop = e_soap_parameter_get_property (subparam, "IncludesLastItemInRange");

	if (prop && !strcmp (prop, "true"))
		includes_last_item = TRUE;
	g_free (prop);

	for (subparam = e_soap_parameter_get_first_child_by_name (subparam, "Mailbox");
		subparam != NULL;
		subparam = e_soap_parameter_get_next_child_by_name (subparam, "Mailbox")) {
		EwsMailbox *mb;

		mb = e_ews_item_mailbox_from_soap_param (subparam);
		if (mb)
			mailboxes = g_slist_append (mailboxes, mb);
	}

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	/* Reuse existing variables */
	async_data->items = mailboxes;
	async_data->includes_last_item = includes_last_item;
}

/* TODO scan all folders if we support creating multiple folders in the request */
static void
ews_create_folder_cb (ESoapParameter *soapparam, EwsNode *enode)
{
	ESoapParameter *param, *node;
	EwsAsyncData *async_data;
	EwsFolderId *fid = NULL;
	GSList *fids = NULL;

	node = e_soap_parameter_get_first_child_by_name (soapparam, "Folders");
	node = e_soap_parameter_get_first_child_by_name (node, "Folder");
	param = e_soap_parameter_get_first_child_by_name (node, "FolderId");

	fid = g_new0 (EwsFolderId, 1);
	fid->id = e_soap_parameter_get_property (param, "Id");
	fid->change_key = e_soap_parameter_get_property (param, "ChangeKey");
	fids = g_slist_append (fids, fid);

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);
	async_data->items_created = fids;
}

static void
e_ews_connection_dispose (GObject *object)
{
	EEwsConnection *cnc = (EEwsConnection *) object;
	EEwsConnectionPrivate *priv;
	gchar *hash_key;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	priv = cnc->priv;

	/* remove the connection from the hash table */
	if (loaded_connections_permissions != NULL) {
		hash_key = g_strdup_printf ("%s@%s",
					    priv->username ? priv->username : "",
					    priv->uri ? priv->uri : "");
		g_hash_table_remove (loaded_connections_permissions, hash_key);
		if (g_hash_table_size (loaded_connections_permissions) == 0) {
			g_hash_table_destroy (loaded_connections_permissions);
			loaded_connections_permissions = NULL;
		}
		g_free (hash_key);
	}

	g_signal_handlers_disconnect_by_func	(priv->soup_session, ews_connection_authenticate, cnc);

	if (priv->soup_session) {
		g_main_loop_quit(priv->soup_loop);
		g_thread_join(priv->soup_thread);
		priv->soup_thread = NULL;

		g_main_loop_unref(priv->soup_loop);
		priv->soup_loop = NULL;
		g_main_context_unref(priv->soup_context);
		priv->soup_context = NULL;
	}

	if (priv->uri) {
		g_free (priv->uri);
		priv->uri = NULL;
	}

	if (priv->username) {
		g_free (priv->username);
		priv->username = NULL;
	}

	if (priv->password) {
		g_free (priv->password);
		priv->password = NULL;
	}

	if (priv->email) {
		g_free (priv->email);
		priv->email = NULL;
	}

	if (priv->jobs) {
		g_slist_free (priv->jobs);
		priv->jobs = NULL;
	}

	if (priv->active_job_queue) {
		g_slist_free (priv->active_job_queue);
		priv->active_job_queue = NULL;
	}

	if (parent_class->dispose)
		(* parent_class->dispose) (object);
}

static void
e_ews_connection_finalize (GObject *object)
{
	EEwsConnection *cnc = (EEwsConnection *) object;
	EEwsConnectionPrivate *priv;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	priv = cnc->priv;
	g_static_rec_mutex_free (&priv->queue_lock);

	g_free (priv);
	cnc->priv = NULL;

	if (parent_class->finalize)
		(* parent_class->finalize) (object);
}

static void
e_ews_connection_class_init (EEwsConnectionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	parent_class = g_type_class_peek_parent (klass);

	object_class->dispose = e_ews_connection_dispose;
	object_class->finalize = e_ews_connection_finalize;

	klass->authenticate = NULL;

       /**
        * EEwsConnection::authenticate
        **/
	signals[AUTHENTICATE] = g_signal_new (
	      "authenticate",
	      G_OBJECT_CLASS_TYPE (klass),
	      G_SIGNAL_RUN_FIRST,
	      G_STRUCT_OFFSET (EEwsConnectionClass, authenticate),
	      NULL, NULL,
	      ews_marshal_VOID__OBJECT_OBJECT_BOOLEAN,
	      G_TYPE_NONE, 3,
	      SOUP_TYPE_MESSAGE, SOUP_TYPE_AUTH, G_TYPE_BOOLEAN);
}


static gpointer e_ews_soup_thread (gpointer user_data)
{
	EEwsConnectionPrivate *priv = user_data;

	g_main_context_push_thread_default (priv->soup_context);
	g_main_loop_run (priv->soup_loop);
	g_main_context_pop_thread_default (priv->soup_context);

	g_object_unref (priv->soup_session);
	priv->soup_session = NULL;

	return NULL;
}

static void
e_ews_connection_init (EEwsConnection *cnc)
{
	EEwsConnectionPrivate *priv;

	/* allocate internal structure */
	priv = g_new0 (EEwsConnectionPrivate, 1);
	cnc->priv = priv;

	priv->soup_context = g_main_context_new ();
	priv->soup_loop = g_main_loop_new (priv->soup_context, FALSE);

	priv->soup_thread = g_thread_create(e_ews_soup_thread, priv, TRUE, NULL);

	/* create the SoupSession for this connection */
	priv->soup_session = soup_session_async_new_with_options (SOUP_SESSION_USE_NTLM, TRUE,
								  SOUP_SESSION_ASYNC_CONTEXT, priv->soup_context, NULL);

        if (getenv("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 2)) {
                SoupLogger *logger;
                logger = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
                soup_session_add_feature(priv->soup_session, SOUP_SESSION_FEATURE(logger));
        }


	g_static_rec_mutex_init (&priv->queue_lock);

	g_signal_connect (priv->soup_session, "authenticate", G_CALLBACK(ews_connection_authenticate), cnc);
}

static void
ews_connection_authenticate	(SoupSession *sess, SoupMessage *msg,
				 SoupAuth *auth, gboolean retrying,
				 gpointer data)
{
	EEwsConnection *cnc = data;

	if (retrying) {
		g_free (cnc->priv->password);
		cnc->priv->password = NULL;
	}

	if (cnc->priv->password) {
		soup_auth_authenticate (auth, cnc->priv->username,
					cnc->priv->password);
		return;
	}

	g_signal_emit (cnc, signals[AUTHENTICATE], 0, msg, auth, retrying);
}

void
ews_user_id_free	(EwsUserId *id)
{
	if (id)
	{
		g_free(id->sid);
		g_free(id->primary_smtp_add);
		g_free(id->display_name);
		g_free(id->distinguished_user);
		g_free(id->external_user);
		g_free(id);
	}
}

void
e_ews_connection_authenticate (EEwsConnection *cnc,
			       SoupAuth *auth, const gchar *user,
			       const gchar *passwd, GError *error)
{
	if (error) {
		g_warning ("Auth error: %s", error->message);
		g_clear_error (&error);
		return;
	}

	if (user) {
		g_free (cnc->priv->username);
		cnc->priv->username = g_strdup (user);
	}

	g_free (cnc->priv->password);
	cnc->priv->password = g_strdup(passwd);

	soup_auth_authenticate (auth, cnc->priv->username,
				cnc->priv->password);
}
/* Connection APIS */

/**
 * e_ews_connection_find
 * @uri: Exchange server uri
 * @username:
 *
 * Find an existing connection for this user/uri, if it exists.
 *
 * Returns: EEwsConnection
 **/
EEwsConnection *
e_ews_connection_find (const gchar *uri, const gchar *username)
{
	EEwsConnection *cnc;
	gchar *hash_key;

	g_static_mutex_lock (&connecting);

	/* search the connection in our hash table */
	if (loaded_connections_permissions) {
		hash_key = g_strdup_printf ("%s@%s",
				username ? username : "",
				uri);
		cnc = g_hash_table_lookup (loaded_connections_permissions, hash_key);
		g_free (hash_key);

		if (E_IS_EWS_CONNECTION (cnc)) {
			g_object_ref (cnc);
			g_static_mutex_unlock (&connecting);
			return cnc;
		}
	}

	g_static_mutex_unlock (&connecting);

	return NULL;
}

/**
 * e_ews_connection_new
 * @uri: Exchange server uri
 * @username:
 * @password:
 * @error: Currently unused, but may require in future. Can take NULL value.
 *
 * This does not authenticate to the server. It merely stores the username and password.
 * Authentication happens when a request is made to the server.
 *
 * Returns: EEwsConnection
 **/
EEwsConnection *
e_ews_connection_new (const gchar *uri, const gchar *username, const gchar *password,
		      GCallback authenticate_cb, gpointer authenticate_ctx,
		      GError **error)
{
	EEwsConnection *cnc;
	gchar *hash_key;

	g_static_mutex_lock (&connecting);

	/* search the connection in our hash table */
	if (loaded_connections_permissions != NULL) {
		hash_key = g_strdup_printf ("%s@%s",
				username ? username : "",
				uri);
		cnc = g_hash_table_lookup (loaded_connections_permissions, hash_key);
		g_free (hash_key);

		if (E_IS_EWS_CONNECTION (cnc)) {
			g_object_ref (cnc);
			g_static_mutex_unlock (&connecting);
			return cnc;
		}
	}

	/* not found, so create a new connection */
	cnc = g_object_new (E_TYPE_EWS_CONNECTION, NULL);

	cnc->priv->username = g_strdup (username);
	cnc->priv->password = g_strdup (password);
	cnc->priv->uri = g_strdup (uri);

	/* register a handler to the authenticate signal */
	if (authenticate_cb)
		g_signal_connect (cnc, "authenticate",
				  authenticate_cb, authenticate_ctx);

	/* add the connection to the loaded_connections_permissions hash table */
	hash_key = g_strdup_printf ("%s@%s",
			cnc->priv->username ? cnc->priv->username : "",
			cnc->priv->uri);
	if (loaded_connections_permissions == NULL)
		loaded_connections_permissions = g_hash_table_new_full (g_str_hash, g_str_equal,
				g_free, NULL);
	g_hash_table_insert (loaded_connections_permissions, hash_key, cnc);

	/* free memory */
	g_static_mutex_unlock (&connecting);
	return cnc;

}

static xmlDoc *
e_ews_autodiscover_ws_xml(const gchar *email)
{
	xmlDoc *doc;
	xmlNode *node;
	xmlNs *ns;

	doc = xmlNewDoc((xmlChar *) "1.0");
	node = xmlNewDocNode(doc, NULL, (xmlChar *)"Autodiscover", NULL);
	xmlDocSetRootElement(doc, node);
	ns = xmlNewNs (node,
		       (xmlChar *)"http://schemas.microsoft.com/exchange/autodiscover/outlook/requestschema/2006", NULL);

	node = xmlNewChild(node, ns, (xmlChar *)"Request", NULL);
	xmlNewChild(node, ns, (xmlChar *)"EMailAddress",
			    (xmlChar *)email);
	xmlNewChild(node, ns, (xmlChar *)"AcceptableResponseSchema",
			    (xmlChar *)"http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a");

	return doc;
}

struct _autodiscover_data {
	EEwsConnection *cnc;
	xmlOutputBuffer *buf;
	GSimpleAsyncResult *simple;
	SoupMessage *msgs[4];
	EEwsAutoDiscoverCallback cb;
	gpointer cbdata;
};

/* Called in the context e_ews_autodiscover_ws_url() was called from,
   with the final result. */
static void autodiscover_done_cb (GObject *cnc, GAsyncResult *res,
				  gpointer user_data)
{
	struct _autodiscover_data *ad = user_data;
	EwsUrls *urls = NULL;
	GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
	GError *error = NULL;

	if (!g_simple_async_result_propagate_error (simple, &error))
		urls = g_simple_async_result_get_op_res_gpointer (simple);

	xmlOutputBufferClose (ad->buf);

	ad->cb (urls, ad->cbdata, error);
	g_free (ad);
}

static void
ews_dump_raw_soup_response (SoupMessage *msg)
{
	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 1)) {
		soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (msg)->response_body));
		/* print response body */
		printf ("\n The response headers");
		printf ("\n =====================");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (msg)->response_body->data, stdout);
		fputc ('\n', stdout);
	}

}

/* Called when each soup message completes */
static void
autodiscover_response_cb (SoupSession *session, SoupMessage *msg, gpointer data)

{
	GError *error = NULL;
	struct _autodiscover_data *ad = data;
	EwsUrls *urls = NULL;
	guint status = msg->status_code;
	xmlDoc *doc;
	xmlNode *node;
	int idx;
	gboolean success = FALSE;
	GSimpleAsyncResult *simple;

	for (idx = 0; idx < 4; idx++) {
		if (ad->msgs[idx] == msg)
			break;
	}
	if (idx == 4) {
		/* We already got removed (cancelled). Do nothing */
		return;
	}

	ad->msgs[idx] = NULL;

	/* Take 'simple' from 'ad' before actual call
	   to g_simple_async_result_complete_in_idle(),
	   for cases where the 'ad' is freed in autodiscover_done_cb()
	   before this function finishes. Suggested by Dan Winship.
	*/
	simple = ad->simple;

	if (status != 200) {
		g_set_error (
			     &error, EWS_CONNECTION_ERROR,
			     status,
			     _("Code: %d - Unexpected response from server"),
			     status);
		goto failed;
	}

	ews_dump_raw_soup_response (msg);
	doc = xmlReadMemory (msg->response_body->data, msg->response_body->length,
			     "autodiscover.xml", NULL, 0);
	if (!doc) {
		g_set_error (
			     &error, EWS_CONNECTION_ERROR,
			     -1,
			     _("Failed to parse autodiscover response XML"));
		goto failed;
	}
	node = xmlDocGetRootElement(doc);
	if (strcmp((char *)node->name, "Autodiscover")) {
		g_set_error (
			     &error, EWS_CONNECTION_ERROR,
			     -1,
			     _("Failed to find <Autodiscover> element\n"));
		goto failed;
	}
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "Response"))
			break;
	}
	if (!node) {
		g_set_error (
			     &error, EWS_CONNECTION_ERROR,
			     -1,
			     _("Failed to find <Response> element\n"));
		goto failed;
	}
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "Account"))
			break;
	}
	if (!node) {
		g_set_error (
			     &error, EWS_CONNECTION_ERROR,
			     -1,
			     _("Failed to find <Account> element\n"));
		goto failed;
	}

	urls = g_new0 (EwsUrls, 1);
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "Protocol")) {
		    	success = autodiscover_parse_protocol(node, urls);
			break;
		}
	}

	if (!success) {
		g_free (urls->as_url);
		g_free (urls->oab_url);
		g_free (urls);
		g_set_error	(&error, EWS_CONNECTION_ERROR,
				-1,
				_("Failed to find <ASUrl> and <OABUrl> in autodiscover response"));
		goto failed;
	}

	/* We have a good response; cancel all the others */
	for (idx = 0; idx < 4; idx++) {
		if (ad->msgs[idx]) {
			SoupMessage *m = ad->msgs[idx];
			ad->msgs[idx] = NULL;
			soup_session_cancel_message (ad->cnc->priv->soup_session,
						     m, SOUP_STATUS_CANCELLED);
		}
	}
	
	g_simple_async_result_set_op_res_gpointer (ad->simple, urls, NULL);
	g_simple_async_result_complete_in_idle (ad->simple);
	g_object_unref (simple);
	return;

failed:
	for (idx = 0; idx < 4; idx++) {
		if (ad->msgs[idx]) {
			/* There's another request outstanding.
			   Hope that it has better luck. */
			g_clear_error (&error);
			return;
		}
	}

	/* FIXME: We're actually returning the *last* error here,
	   and in some cases (stupid firewalls causing timeouts)
	   that's going to be the least interesting one. We probably
	   want the *first* error */
	g_simple_async_result_set_from_error (ad->simple, error);
	g_simple_async_result_complete_in_idle (ad->simple);
	g_object_unref (simple);
}

static void post_restarted (SoupMessage *msg, gpointer data)
{
	xmlOutputBuffer *buf = data;

	/* In violation of RFC2616, libsoup will change a POST request to
	   a GET on receiving a 302 redirect. */
	printf("Working around libsoup bug with redirect\n");
	g_object_set (msg, SOUP_MESSAGE_METHOD, "POST", NULL);

	soup_message_set_request(msg, "text/xml", SOUP_MEMORY_COPY,
				 (gchar *)buf->buffer->content,
				 buf->buffer->use);
}

static SoupMessage *
e_ews_get_msg_for_url (const gchar *url, xmlOutputBuffer *buf)
{
	SoupMessage *msg;

	msg = soup_message_new (buf?"POST":"GET", url);
	soup_message_headers_append (msg->request_headers,
				     "User-Agent", "libews/0.1");


	if (buf) {
		soup_message_set_request (msg, "text/xml; charset=utf-8", SOUP_MEMORY_COPY,
					  (gchar *)buf->buffer->content,
					  buf->buffer->use);
		g_signal_connect (msg, "restarted",
				  G_CALLBACK (post_restarted), buf);
	}

	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 1)) {
		soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (msg)->request_body));
		/* print request's body */
		printf ("\n The request headers");
		printf ("\n ===================");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (msg)->request_body->data, stdout);
		fputc ('\n', stdout);
	}

	return msg;
}

void
e_ews_autodiscover_ws_url (EEwsAutoDiscoverCallback cb, gpointer cbdata,
			   const gchar *email, const gchar *password,
			   const gchar *ews_url, const gchar *username)
{
	struct _autodiscover_data *ad;
	xmlOutputBuffer *buf;
	GError *error = NULL;
	gchar *url1, *url2, *url3, *url4;
	gchar *domain;
	xmlDoc *doc;
	EEwsConnection *cnc;
	gboolean use_secure = TRUE;

	if (!password || !email) {
		g_set_error (&error, EWS_CONNECTION_ERROR,
			     -1, _("Both email and password must be provided"));
		goto err;
	}

	domain = strchr(email, '@');
	if (!(domain && *domain)) {
		g_set_error (&error, EWS_CONNECTION_ERROR,
			     -1, _("Wrong email id"));
		goto err;
	}
	domain++;

	doc = e_ews_autodiscover_ws_xml(email);
	buf = xmlAllocOutputBuffer(NULL);
	xmlNodeDumpOutput(buf, doc, xmlDocGetRootElement(doc), 0, 1, NULL);
	xmlOutputBufferFlush(buf);

	url1 = NULL;
	url2 = NULL;
	url3 = NULL;
	url4 = NULL;
	if (ews_url) {
		SoupURI *uri = soup_uri_new (ews_url);

		if (uri) {
			use_secure = g_strcmp0 (soup_uri_get_scheme (uri), "https") == 0;

			url1 = g_strdup_printf ("http%s://%s/autodiscover/autodiscover.xml", use_secure ? "s" : "", soup_uri_get_host (uri));
			url2 = g_strdup_printf ("http%s://autodiscover.%s/autodiscover/autodiscover.xml", use_secure ? "s" : "", soup_uri_get_host (uri));
			soup_uri_free (uri);
		}
	} 

	url3 = g_strdup_printf ("http%s://%s/autodiscover/autodiscover.xml", use_secure ? "s" : "", domain);
	url4 = g_strdup_printf ("http%s://autodiscover.%s/autodiscover/autodiscover.xml", use_secure ? "s" : "", domain);

	cnc = e_ews_connection_new (url3, (username && *username) ? username : email, password, NULL, NULL, &error);
	if (!cnc) {
		g_free (url1);
		g_free (url2);
		g_free (url3);
		g_free (url4);
		xmlOutputBufferClose (buf);
		xmlFreeDoc (doc);
	err:
		cb (NULL, cbdata, error);
		return;
	}

	/*
	 * http://msdn.microsoft.com/en-us/library/ee332364.aspx says we are
	 * supposed to try $domain and then autodiscover.$domain. But some
	 * people have broken firewalls on the former which drop packets
	 * instead of rejecting connections, and make the request take ages
	 * to time out. So run both queries in parallel and let the fastest
	 * (successful) one win.
	 */
	ad = g_new0 (struct _autodiscover_data, 1);
	ad->cb = cb;
	ad->cbdata = cbdata;
	ad->cnc = cnc;
	ad->buf = buf;
	ad->simple = g_simple_async_result_new (G_OBJECT (cnc), autodiscover_done_cb,
					    ad, e_ews_autodiscover_ws_url);
	ad->msgs[0] = url1 ? e_ews_get_msg_for_url (url1, buf) : NULL;
	ad->msgs[1] = url2 ? e_ews_get_msg_for_url (url2, buf) : NULL;
	ad->msgs[2] = url3 ? e_ews_get_msg_for_url (url3, buf) : NULL;
	ad->msgs[3] = url4 ? e_ews_get_msg_for_url (url4, buf) : NULL;

	/* These have to be submitted only after they're both set in ad->msgs[]
	   or there will be races with fast completion */
	if (ad->msgs[0])
		soup_session_queue_message (cnc->priv->soup_session, ad->msgs[0],
					    autodiscover_response_cb, ad);
	if (ad->msgs[1])
		soup_session_queue_message (cnc->priv->soup_session, ad->msgs[1],
					    autodiscover_response_cb, ad);
	if (ad->msgs[2])
		soup_session_queue_message (cnc->priv->soup_session, ad->msgs[2],
					    autodiscover_response_cb, ad);
	if (ad->msgs[3])
		soup_session_queue_message (cnc->priv->soup_session, ad->msgs[3],
					    autodiscover_response_cb, ad);

	g_object_unref (cnc); /* the GSimpleAsyncResult holds it now */

	xmlFreeDoc (doc);
	g_free (url1);
	g_free (url2);
	g_free (url3);
	g_free (url4);
}

struct _oal_req_data {
	EEwsConnection *cnc;
	GSimpleAsyncResult *simple;
	SoupMessage *msg;
	GCancellable *cancellable;
	gulong cancel_handler_id;
	gchar *oal_id;
	gchar *oal_element;
	
	/* for dowloading oal file */
	gchar *cache_filename;
	GError *error;
	EwsProgressFn progress_fn;
	gpointer progress_data;
	gsize response_size;
	gsize received_size;
};

static gchar *
get_property (xmlNodePtr node_ptr, const gchar *name)
{
	xmlChar *xml_s;
	gchar *s;
	
	xml_s = xmlGetProp (node_ptr, (const xmlChar *) name);
	s = g_strdup ((gchar *)xml_s);
	xmlFree (xml_s);

	return s;
}

static guint32
get_property_as_uint32 (xmlNodePtr node_ptr, const gchar *name)
{
	gchar *s;
	guint32 val = -1;
	
	s = get_property (node_ptr, name);
	if (s)
		sscanf (s,"%"G_GUINT32_FORMAT, &val);
	g_free (s);

	return val;
}

static gchar *
get_content (xmlNodePtr node_ptr)
{
	xmlChar *xml_s;
	gchar *s;

	xml_s = xmlNodeGetContent (node_ptr);
	s = g_strdup ((gchar *)xml_s);
	xmlFree (xml_s);

	return s;
}

static GSList *
parse_oal_full_details (xmlNode *node, const gchar *element)
{
	GSList *elements = NULL;
	
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE && !strcmp((char *)node->name, element)) {
			EwsOALDetails *det = g_new0 (EwsOALDetails, 1);

			det->seq = get_property_as_uint32 (node, "seq");
			det->ver = get_property_as_uint32 (node, "ver");
			det->size = get_property_as_uint32 (node, "size");
			det->uncompressed_size = get_property_as_uint32 (node, "uncompressedsize");
			det->sha = get_property (node, "uncompressedsize");
			det->filename = g_strstrip (get_content (node));
			
			elements = g_slist_prepend (elements, det);
			if (!strcmp (element, "Full"))
				break; 
		}
	}

	return elements;
}

static void
oal_response_cb (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
	GError *error = NULL;
	guint status = msg->status_code;
	xmlDoc *doc;
	xmlNode *node;
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;
	GSList *oals = NULL;

	if (status != 200) {
		g_set_error (&error, EWS_CONNECTION_ERROR, status,
			     _("Code: %d - Unexpected response from server"),
			     status);
		goto exit;
	}
	ews_dump_raw_soup_response (msg);
	
	doc = xmlReadMemory (msg->response_body->data, msg->response_body->length,
			     "oab.xml", NULL, 0);
	if (!doc) {
		g_set_error (&error, EWS_CONNECTION_ERROR,
			     -1, _("Failed to parse oab XML"));
		goto exit;
	}

	node = xmlDocGetRootElement(doc);
	if (strcmp((char *)node->name, "OAB")) {
		g_set_error (&error, EWS_CONNECTION_ERROR, -1,
			     _("Failed to find <OAB> element\n"));
		goto exit;
	}

	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE && !strcmp((char *)node->name, "OAL")) {
			if (!data->oal_id) {
				EwsOAL *oal = g_new0 (EwsOAL, 1);

				oal->id = get_property (node, "id");
				oal->dn = get_property (node, "dn");
				oal->name = get_property (node, "name");

				oals = g_slist_prepend (oals, oal);
			} else {
				gchar *id = get_property (node, "id");
				
				if (!strcmp (id, data->oal_id)) {
					/* parse details of full_details file */
					oals = parse_oal_full_details (node, data->oal_element);

					g_free (id);
					break;
				}

				g_free (id);
			}
		}
	}

	oals = g_slist_reverse (oals);
	g_simple_async_result_set_op_res_gpointer (data->simple, oals, NULL);

exit:
	if (data->cancellable)
		g_signal_handler_disconnect (data->cancellable, data->cancel_handler_id);

	if (error) {
		g_simple_async_result_set_from_error (data->simple, error);
		g_clear_error (&error);
	}

	g_simple_async_result_complete_in_idle (data->simple);
	g_free (data->oal_id);
	g_free (data->oal_element);
	g_free (data);
}

static void
ews_cancel_msg (GCancellable *cancellable,
		gpointer user_data)
{
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;

	soup_session_cancel_message (data->cnc->priv->soup_session, SOUP_MESSAGE (data->msg), SOUP_STATUS_CANCELLED);
}

void		
e_ews_connection_get_oal_list_start	(EEwsConnection *cnc,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	GSimpleAsyncResult *simple;
	SoupMessage *msg;
	struct _oal_req_data *data;

	msg = e_ews_get_msg_for_url (cnc->priv->uri, NULL);
 	
	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_get_oal_list_start);
	data = g_new0 (struct _oal_req_data, 1);
	data->cnc = cnc;
	data->simple = simple;
	data->cancellable = cancellable;
	data->msg = msg;
	
	if (cancellable)
		data->cancel_handler_id = g_cancellable_connect	(cancellable,
							 	 G_CALLBACK (ews_cancel_msg), (gpointer) data, NULL);
	soup_session_queue_message (cnc->priv->soup_session, msg,
				    oal_response_cb, data);
}

gboolean	
e_ews_connection_get_oal_list_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **oals,
					 GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_oal_list_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;
	
	*oals= g_simple_async_result_get_op_res_gpointer (simple);

	return TRUE;
}

void		
e_ews_connection_get_oal_detail_start	(EEwsConnection *cnc,
					 const gchar *oal_id,
					 const gchar *oal_element,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	GSimpleAsyncResult *simple;
	SoupMessage *msg;
	struct _oal_req_data *data;

	msg = e_ews_get_msg_for_url (cnc->priv->uri, NULL);
 	
	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_get_oal_detail_start);
	data = g_new0 (struct _oal_req_data, 1);
	data->cnc = cnc;
	data->simple = simple;
	data->cancellable = cancellable;
	data->msg = msg;
	data->oal_id = g_strdup (oal_id);
	data->oal_element = g_strdup (oal_element);
	
	if (cancellable)
		data->cancel_handler_id = g_cancellable_connect	(cancellable,
							 	 G_CALLBACK (ews_cancel_msg), (gpointer) data, NULL);
	soup_session_queue_message (cnc->priv->soup_session, msg,
				    oal_response_cb, data);

}

gboolean	
e_ews_connection_get_oal_detail_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **elements,
					 GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_oal_detail_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;
	
	*elements = g_simple_async_result_get_op_res_gpointer (simple);

	return TRUE;

}

/**
 * e_ews_connection_get_oal_detail 
 * @cnc: 
 * @oal_id: 
 * @oal_element: 
 * @elements: "Full" "Diff" "Template" are the possible values.
 * @cancellable: 
 * @error: 
 * 
 * 
 * Returns: 
 **/
gboolean	
e_ews_connection_get_oal_detail	(EEwsConnection *cnc,
				 const gchar *oal_id,
				 const gchar *oal_element,
				 GSList **elements,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_get_oal_detail_start	(cnc, oal_id, oal_element,
						 ews_sync_reply_cb,
						 cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_get_oal_detail_finish 
						(cnc, sync_data->res,
						 elements, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

static void
oal_download_response_cb (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
	GError *error = NULL;
	guint status = msg->status_code;
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;

	if (status != 200) {
		g_set_error (&error, EWS_CONNECTION_ERROR, status,
			     _("Code: %d - Unexpected response from server"),
			     status);
		g_unlink (data->cache_filename);
		goto exit;
	} else if (data->error != NULL) {
		g_propagate_error (&error, data->error);
		g_unlink (data->cache_filename);
		goto exit;
	}

	g_simple_async_result_set_op_res_gpointer (data->simple, NULL, NULL);

exit:
	if (error) {
		g_simple_async_result_set_from_error (data->simple, error);
		g_clear_error (&error);
	}

	g_simple_async_result_complete_in_idle (data->simple);
	g_free (data->cache_filename);
	g_free (data);
}

static void 
ews_soup_got_headers (SoupMessage *msg, gpointer user_data)
{
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;
	const char *size;

	size = soup_message_headers_get_one (msg->response_headers,
					     "Content-Length");

	if (size)
		data->response_size = strtol(size, NULL, 10);
}

static void 
ews_soup_restarted (SoupMessage *msg, gpointer user_data)
{
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;

	data->response_size = 0;
	data->received_size = 0;
}

static void 
ews_soup_got_chunk (SoupMessage *msg, SoupBuffer *chunk, gpointer user_data)
{
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;
	gint fd;

	if (msg->status_code != 200)
		return;

	data->received_size += chunk->length;

	if (data->response_size && data->progress_fn) {
		int pc = data->received_size * 100 / data->response_size;
		data->progress_fn (data->progress_data, pc);
	}

	fd = g_open (data->cache_filename, O_RDONLY | O_WRONLY | O_APPEND | O_CREAT, 0600);
	if (fd != -1) {
		if (write (fd, (const gchar*)chunk->data, chunk->length) != chunk->length) {
			g_set_error (&data->error, EWS_CONNECTION_ERROR, EWS_CONNECTION_ERROR_UNKNOWN,
					"Failed to write streaming data to file : %d ", errno);
		}
#ifdef G_OS_WIN32
		closesocket (fd);
#else
		close (fd);
#endif
	} else {
		g_set_error (&data->error, EWS_CONNECTION_ERROR, EWS_CONNECTION_ERROR_UNKNOWN,
			"Failed to open the cache file : %d ", errno);
	}
}

void		
e_ews_connection_download_oal_file_start	(EEwsConnection *cnc,
						 const gchar *cache_filename,
						 GAsyncReadyCallback cb,
						 EwsProgressFn progress_fn,
						 gpointer progress_data,
						 GCancellable *cancellable,
						 gpointer user_data)
{
	GSimpleAsyncResult *simple;
	SoupMessage *msg;
	struct _oal_req_data *data;

	msg = e_ews_get_msg_for_url (cnc->priv->uri, NULL);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
			cb,
			user_data,
			e_ews_connection_download_oal_file_start);
	data = g_new0 (struct _oal_req_data, 1);
	data->cnc = cnc;
	data->simple = simple;
	data->cancellable = cancellable;
	data->msg = SOUP_MESSAGE (msg);
	data->cache_filename = g_strdup (cache_filename);
	data->progress_fn = progress_fn;
	data->progress_data = progress_data;

	if (cancellable)
		data->cancel_handler_id = g_cancellable_connect	(cancellable,
						G_CALLBACK (ews_cancel_msg), (gpointer) data, NULL);
	
	soup_message_body_set_accumulate (SOUP_MESSAGE (msg)->response_body,
					  FALSE);
	g_signal_connect (msg, "got-headers", G_CALLBACK (ews_soup_got_headers), data);
	g_signal_connect (msg, "got-chunk", G_CALLBACK (ews_soup_got_chunk), data);
	g_signal_connect (msg, "restarted", G_CALLBACK (ews_soup_restarted), data);

	soup_session_queue_message	(cnc->priv->soup_session, SOUP_MESSAGE (msg),
					 oal_download_response_cb, data);
}

gboolean	
e_ews_connection_download_oal_file_finish	(EEwsConnection *cnc,
						 GAsyncResult *result,
						 GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (
			g_simple_async_result_is_valid (
				result, G_OBJECT (cnc), e_ews_connection_download_oal_file_start),
			FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean	
e_ews_connection_download_oal_file	(EEwsConnection *cnc,
					 const gchar *cache_filename,
					 EwsProgressFn progress_fn,
					 gpointer progress_data,
					 GCancellable *cancellable,
					 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_download_oal_file_start
						(cnc, cache_filename, 
						 ews_sync_reply_cb,
						 progress_fn, progress_data,
						 cancellable, sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_download_oal_file_finish
						(cnc, sync_data->res,
						 error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void
e_ews_connection_set_mailbox	(EEwsConnection *cnc,
				 const gchar *email)
{

	g_return_if_fail (email != NULL);

	g_free (cnc->priv->email);
	cnc->priv->email = g_strdup (email);
}

static void
ews_append_additional_props_to_msg (ESoapMessage *msg, EwsAdditionalProps *add_props)
{
	GSList *l;

	if (!add_props)
		return;
	
	e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);

	if (add_props->field_uri) {
		gchar **prop = g_strsplit (add_props->field_uri, " ", 0);
		gint i = 0;
	
		while (prop[i]) {
			e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", prop [i]);
			i++;
		}

		g_strfreev (prop);
	}

	if (add_props->extended_furis) {
		for (l = add_props->extended_furis; l != NULL; l = g_slist_next (l)) {
			EwsExtendedFieldURI *ex_furi = (EwsExtendedFieldURI *) l->data;
			
			e_soap_message_start_element (msg, "ExtendedFieldURI", NULL, NULL);
			
			if (ex_furi->distinguished_prop_set_id)
				e_soap_message_add_attribute (msg, "DistinguishedPropertySetId", ex_furi->distinguished_prop_set_id, NULL, NULL);

			if (ex_furi->prop_set_id)
				e_soap_message_add_attribute (msg, "PropertySetId", ex_furi->prop_set_id, NULL, NULL);

			if (ex_furi->prop_name)
				e_soap_message_add_attribute (msg, "PropertyName", ex_furi->prop_name, NULL, NULL);
			
			if (ex_furi->prop_id)
				e_soap_message_add_attribute (msg, "PropertyId", ex_furi->prop_id, NULL, NULL);
			
			if (ex_furi->prop_type)
				e_soap_message_add_attribute (msg, "PropertyType", ex_furi->prop_type, NULL, NULL);

			e_soap_message_end_element (msg);
		}
	}

	if (add_props->indexed_furis) {
		for (l = add_props->indexed_furis; l != NULL; l = g_slist_next (l)) {
			EwsIndexedFieldURI *in_furi = (EwsIndexedFieldURI *) l->data;

			e_soap_message_start_element (msg, "IndexedFieldURI", NULL, NULL);
			
			e_soap_message_add_attribute (msg, "FieldURI", in_furi->field_uri, NULL, NULL);
			e_soap_message_add_attribute (msg, "FieldIndex", in_furi->field_index, NULL, NULL);
			
			e_soap_message_end_element (msg);
		}
	}

	e_soap_message_end_element (msg);
}

static void
ews_write_sort_order_to_msg (ESoapMessage *msg, EwsSortOrder *sort_order)
{
	if (!sort_order)
		return;

	e_soap_message_start_element (msg, "SortOrder", NULL, NULL);
	e_soap_message_start_element (msg, "FieldOrder", NULL, NULL);
	e_soap_message_add_attribute (msg, "Order", sort_order->order, NULL, NULL);

	if (sort_order->uri_type == NORMAL_FIELD_URI)
		e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", (gchar *) sort_order->field_uri);
	else if (sort_order->uri_type == INDEXED_FIELD_URI) {
		EwsIndexedFieldURI *in_furi = (EwsIndexedFieldURI *) sort_order->field_uri;

		e_soap_message_start_element (msg, "IndexedFieldURI", NULL, NULL);
		e_soap_message_add_attribute (msg, "FieldURI", in_furi->field_uri, NULL, NULL);
		e_soap_message_add_attribute (msg, "FieldIndex", in_furi->field_index, NULL, NULL);
		e_soap_message_end_element (msg);
	} else if (sort_order->uri_type == EXTENDED_FIELD_URI) {
		EwsExtendedFieldURI *ex_furi = (EwsExtendedFieldURI *) sort_order->field_uri;
			
		e_soap_message_start_element (msg, "ExtendedFieldURI", NULL, NULL);
		
		if (ex_furi->distinguished_prop_set_id)
			e_soap_message_add_attribute (msg, "DistinguishedPropertySetId", ex_furi->distinguished_prop_set_id, NULL, NULL);
		if (ex_furi->prop_set_id)
			e_soap_message_add_attribute (msg, "PropertySetId", ex_furi->prop_set_id, NULL, NULL);
		if (ex_furi->prop_name)
			e_soap_message_add_attribute (msg, "PropertyName", ex_furi->prop_name, NULL, NULL);
		if (ex_furi->prop_id)
			e_soap_message_add_attribute (msg, "PropertyId", ex_furi->prop_id, NULL, NULL);
		if (ex_furi->prop_type)
			e_soap_message_add_attribute (msg, "PropertyType", ex_furi->prop_type, NULL, NULL);

		e_soap_message_end_element (msg);
	}

	e_soap_message_end_element (msg);
	e_soap_message_end_element (msg);
}

/**
 * e_ews_connection_sync_folder_items_start
 * @cnc: The EWS Connection
 * @pri: The priority associated with the request
 * @sync_state: To sync with the previous requests
 * @folder_id: The folder to which the items belong
 * @default_props: Can take one of the values: IdOnly,Default or AllProperties
 * @additional_props: Specify any additional properties to be fetched
 * @max_entries: Maximum number of items to be returned
 * @cb: Responses are parsed and returned to this callback
 * @cancellable: a GCancellable to monitor cancelled operations
 * @user_data: user data passed to callback
 **/
void
e_ews_connection_sync_folder_items_start	(EEwsConnection *cnc,
						 gint pri,
						 const gchar *sync_state,
						 const gchar *fid,
						 const gchar *default_props,
						 const gchar *additional_props,
						 guint max_entries,
						 GAsyncReadyCallback cb,
						 GCancellable *cancellable,
						 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "SyncFolderItems", NULL, NULL, EWS_EXCHANGE_2007_SP1);
	e_soap_message_start_element (msg, "ItemShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, default_props);

	if (additional_props && *additional_props) {
		gchar **prop = g_strsplit (additional_props, " ", 0);
		gint i = 0;

		e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);
		while (prop[i]) {
			e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", prop [i]);
			i++;
		}
		g_strfreev (prop);
		e_soap_message_end_element (msg);
	}

	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "SyncFolderId", "messages", NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL, NULL, "Id", fid);
	e_soap_message_end_element (msg);

	if (sync_state)
		e_ews_message_write_string_parameter (msg, "SyncState", "messages", sync_state);

	/* Max changes requested */
	e_ews_message_write_int_parameter (msg, "MaxChangesReturned", "messages", max_entries);

	/* Complete the footer and print the request */
	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_sync_folder_items_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, sync_folder_items_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_sync_folder_items_finish	(EEwsConnection *cnc,
						 GAsyncResult *result,
					 	 gchar **sync_state,
						 gboolean *includes_last_item,
						 GSList **items_created,
						 GSList **items_updated,
						 GSList **items_deleted,
						 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_sync_folder_items_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*sync_state = async_data->sync_state;
	*includes_last_item = async_data->includes_last_item;
	*items_created = async_data->items_created;
	*items_updated = async_data->items_updated;
	*items_deleted = async_data->items_deleted;

	return TRUE;
}

gboolean
e_ews_connection_sync_folder_items	(EEwsConnection *cnc,
					 gint pri,
					 gchar **sync_state,
					 const gchar *fid,
					 const gchar *default_props,
					 const gchar *additional_props,
					 guint max_entries,
					 gboolean *includes_last_item,
					 GSList **items_created,
					 GSList **items_updated,
					 GSList **items_deleted,
					 GCancellable *cancellable,
					 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_sync_folder_items_start	(cnc, pri, *sync_state, fid,
							 default_props, additional_props,
							 max_entries,
							 ews_sync_reply_cb, cancellable,
							 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_sync_folder_items_finish (cnc, sync_data->res,
							    sync_state,
							    includes_last_item,
							    items_created,
							    items_updated,
							    items_deleted,
							    error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

static void
ews_append_folder_ids_to_msg (ESoapMessage *msg, const gchar *email, GSList *folder_ids)
{
	GSList *l;
	
	for (l = folder_ids; l != NULL; l = g_slist_next (l)) {
		EwsFolderId *fid = (EwsFolderId *) l->data;

		if (fid->is_distinguished_id)
			e_soap_message_start_element (msg, "DistinguishedFolderId", NULL, NULL);
		else
			e_soap_message_start_element (msg, "FolderId", NULL, NULL);

		e_soap_message_add_attribute (msg, "Id", fid->id, NULL, NULL);
		if (fid->change_key)
			e_soap_message_add_attribute (msg, "ChangeKey", fid->change_key, NULL, NULL);

		if (fid->is_distinguished_id && email)
			e_ews_message_write_string_parameter (msg, "Mailbox", NULL, email);

		e_soap_message_end_element (msg);
	}
}

/**
 * e_ews_connection_find_folder_items_start
 * @cnc: The EWS Connection
 * @pri: The priority associated with the request
 * @fid: The folder id to which the items belong
 * @default_props: Can take one of the values: IdOnly,Default or AllProperties
 * @add_props: Specify any additional properties to be fetched
 * @sort_order: Specific sorting order for items
 * @query: evo query based on which items will be fetched
 * @type: type of folder
 * @convert_query_cb: a callback method to convert query to ews restiction
 * @cb: Responses are parsed and returned to this callback
 * @cancellable: a GCancellable to monitor cancelled operations
 * @user_data: user data passed to callback
 **/
void
e_ews_connection_find_folder_items_start	(EEwsConnection *cnc,
						 gint pri,
						 EwsFolderId *fid,
						 const gchar *default_props,
						 EwsAdditionalProps *add_props,
						 EwsSortOrder *sort_order,
						 const gchar *query,
						 EwsFolderType type,
						 EwsConvertQueryCallback convert_query_cb,
						 GAsyncReadyCallback cb,
						 GCancellable *cancellable,
						 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "FindItem", "Traversal", "Shallow", EWS_EXCHANGE_2007_SP1);
	e_soap_message_start_element (msg, "ItemShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, default_props);

	ews_append_additional_props_to_msg (msg, add_props);

	e_soap_message_end_element (msg);

	/*write restriction message based on query*/
	if (convert_query_cb)
		convert_query_cb (msg, query, type);

	if (sort_order)
		ews_write_sort_order_to_msg (msg, sort_order);

	e_soap_message_start_element (msg, "ParentFolderIds", "messages", NULL);

	if (fid->is_distinguished_id)
		e_ews_message_write_string_parameter_with_attribute (msg, "DistinguishedFolderId", NULL, NULL, "Id", fid->id);
	else
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL, NULL, "Id", fid->id);

	e_soap_message_end_element (msg);

	/* Complete the footer and print the request */
	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_find_folder_items_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, find_folder_items_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_find_folder_items_finish	(EEwsConnection *cnc,
						 GAsyncResult *result,
						 gboolean *includes_last_item,
						 GSList **items,
						 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_find_folder_items_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*includes_last_item = async_data->includes_last_item;
	*items = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_find_folder_items	(EEwsConnection *cnc,
					 gint pri,
					 EwsFolderId *fid,
					 const gchar *default_props,
					 EwsAdditionalProps *add_props,
					 EwsSortOrder *sort_order,
					 const gchar *query,
					 EwsFolderType type,
					 gboolean *includes_last_item,
					 GSList **items,
					 EwsConvertQueryCallback convert_query_cb,
					 GCancellable *cancellable,
					 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_find_folder_items_start	(cnc, pri, fid, default_props,
							 add_props, sort_order, query,
							 type, convert_query_cb,
							 ews_sync_reply_cb, NULL,
							 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_find_folder_items_finish (cnc, sync_data->res,
							    includes_last_item,
							    items,
							    error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}


void
e_ews_connection_sync_folder_hierarchy_start	(EEwsConnection *cnc,
						 gint pri,
						 const gchar *sync_state,
						 GAsyncReadyCallback cb,
						 GCancellable *cancellable,
						 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "SyncFolderHierarchy", NULL, NULL, EWS_EXCHANGE_2007_SP1);
	e_soap_message_start_element (msg, "FolderShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, "AllProperties");
	e_soap_message_end_element (msg);

	if (sync_state)
		e_ews_message_write_string_parameter (msg, "SyncState", "messages", sync_state);

	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_sync_folder_hierarchy_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, sync_hierarchy_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}


gboolean
e_ews_connection_sync_folder_hierarchy_finish	(EEwsConnection *cnc,
						 GAsyncResult *result,
					 	 gchar **sync_state,
						 gboolean *includes_last_folder,
						 GSList **folders_created,
						 GSList **folders_updated,
						 GSList **folders_deleted,
						 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_sync_folder_hierarchy_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*sync_state = async_data->sync_state;
	*includes_last_folder = async_data->includes_last_item;
	*folders_created = async_data->items_created;
	*folders_updated = async_data->items_updated;
	*folders_deleted = async_data->items_deleted;

	return TRUE;
}

gboolean
e_ews_connection_sync_folder_hierarchy	(EEwsConnection *cnc,
					 gint pri,
					 gchar **sync_state,
					 gboolean *includes_last_folder,
					 GSList **folders_created,
					 GSList **folders_updated,
					 GSList **folders_deleted,
					 GCancellable *cancellable,
					 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_sync_folder_hierarchy_start	(cnc, pri, *sync_state,
							 ews_sync_reply_cb, cancellable,
							 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_sync_folder_hierarchy_finish (cnc, sync_data->res,
								sync_state,
								includes_last_folder,
								folders_created,
								folders_updated,
								folders_deleted,
								error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void
e_ews_connection_get_items_start	(EEwsConnection *cnc,
					 gint pri,
					 const GSList *ids,
					 const gchar *default_props,
					 const gchar *additional_props,
					 gboolean include_mime,
					 const gchar *mime_directory,
					 GAsyncReadyCallback cb,
					 ESoapProgressFn progress_fn,
					 gpointer progress_data,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *l;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "GetItem", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	if (progress_fn && progress_data)
		e_soap_message_set_progress_fn (msg, progress_fn, progress_data);

	e_soap_message_start_element (msg, "ItemShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, default_props);

	if (include_mime)
		e_ews_message_write_string_parameter (msg, "IncludeMimeContent", NULL, "true");
	else
		e_ews_message_write_string_parameter (msg, "IncludeMimeContent", NULL, "false");
	if (mime_directory)
		e_soap_message_store_node_data (msg, "MimeContent", mime_directory, TRUE);

	if (additional_props && *additional_props) {
		gchar **prop = g_strsplit (additional_props, " ", 0);
		gint i = 0;

		e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);
		while (prop[i]) {
			/* XX FIXME: Come up with a better way of doing this */
			if (!g_ascii_strncasecmp (prop[i], "mapi:int:0x", 11)) {
				e_soap_message_start_element (msg, "ExtendedFieldURI", NULL, NULL);
				e_soap_message_add_attribute (msg, "PropertyTag", prop[i] + 9, NULL, NULL);
				e_soap_message_add_attribute (msg, "PropertyType", "Integer", NULL, NULL);
				e_soap_message_end_element (msg);
			} else {
				e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", prop [i]);
			}
			i++;
		}
		g_strfreev (prop);
		e_soap_message_end_element (msg);
	}
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "ItemIds", "messages", NULL);

	for (l = ids; l != NULL; l = g_slist_next (l))
		e_ews_message_write_string_parameter_with_attribute (msg, "ItemId", NULL, NULL, "Id", l->data);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_get_items_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_items_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_get_items_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **items,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_items_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*items = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_get_items	(EEwsConnection *cnc,
				 gint pri,
				 const GSList *ids,
				 const gchar *default_props,
				 const gchar *additional_props,
				 gboolean include_mime,
				 const gchar *mime_directory,
				 GSList **items,
				 ESoapProgressFn progress_fn,
				 gpointer progress_data,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_get_items_start	(cnc, pri,ids, default_props,
						 additional_props, include_mime,
						 mime_directory, ews_sync_reply_cb,
						 progress_fn, progress_data,
						 cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_get_items_finish (cnc,
						    sync_data->res,
						    items,
						    error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

static const char *
ews_delete_type_to_str (EwsDeleteType delete_type)
{
	switch (delete_type) {
		case EWS_HARD_DELETE:
			return "HardDelete";
		case EWS_SOFT_DELETE:
			return "SoftDelete";
		case EWS_MOVE_TO_DELETED_ITEMS:
			return "MoveToDeletedItems";
	}
	return NULL;
}

static const char *
ews_send_cancels_to_str (EwsSendMeetingCancellationsType send_cancels)
{
	switch (send_cancels) {
		case EWS_SEND_TO_NONE:
			return "SendToNone";
		case EWS_SEND_ONLY_TO_ALL:
			return "SendOnlyToAll";
		case EWS_SEND_TO_ALL_AND_SAVE_COPY:
			return "SendToAllAndSaveCopy";
	}
	return NULL;
}

static const char *
ews_affected_tasks_to_str (EwsAffectedTaskOccurrencesType affected_tasks)
{
	switch (affected_tasks) {
		case EWS_ALL_OCCURRENCES:
			return "AllOccurrences";
		case EWS_SPECIFIED_OCCURRENCE_ONLY:
			return "SpecifiedOccurrenceOnly";
	}
	return NULL;
}

void
e_ews_connection_delete_items_start	(EEwsConnection *cnc,
					 gint pri,
					 GSList *ids,
					 EwsDeleteType delete_type,
					 EwsSendMeetingCancellationsType send_cancels,
					 EwsAffectedTaskOccurrencesType affected_tasks,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	GSList *l;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "DeleteItem",
					     "DeleteType", ews_delete_type_to_str (delete_type), EWS_EXCHANGE_2007_SP1);

	if (send_cancels)
		e_soap_message_add_attribute (msg, "SendMeetingCancellations",
					      ews_send_cancels_to_str (send_cancels), NULL, NULL);

	if (affected_tasks)
		e_soap_message_add_attribute (msg, "AffectedTaskOccurrences",
					      ews_affected_tasks_to_str (affected_tasks), NULL, NULL);

	e_soap_message_start_element (msg, "ItemIds", "messages", NULL);

	for (l = ids; l != NULL; l = g_slist_next (l))
		e_ews_message_write_string_parameter_with_attribute (msg, "ItemId", NULL, NULL, "Id", l->data);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_delete_items_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, NULL, pri, cancellable, simple,
				      cb == ews_sync_reply_cb);
}

void
e_ews_connection_delete_item_start	(EEwsConnection *cnc,
					 gint pri,
					 EwsId *item_id,
					 guint index,
					 EwsDeleteType delete_type,
					 EwsSendMeetingCancellationsType send_cancels,
					 EwsAffectedTaskOccurrencesType affected_tasks,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	gchar buffer[32];

	msg = e_ews_message_new_with_header (cnc->priv->uri, "DeleteItem",
					     "DeleteType", ews_delete_type_to_str (delete_type), EWS_EXCHANGE_2007_SP1);

	if (send_cancels)
		e_soap_message_add_attribute (msg, "SendMeetingCancellations",
					      ews_send_cancels_to_str (send_cancels), NULL, NULL);

	if (affected_tasks)
		e_soap_message_add_attribute (msg, "AffectedTaskOccurrences",
					      ews_affected_tasks_to_str (affected_tasks), NULL, NULL);

	e_soap_message_start_element (msg, "ItemIds", "messages", NULL);

	if (index) {
		e_soap_message_start_element (msg, "OccurrenceItemId", NULL, NULL);
		e_soap_message_add_attribute (msg, "RecurringMasterId", item_id->id, NULL, NULL);
		if (item_id->change_key)
			e_soap_message_add_attribute (msg, "ChangeKey", item_id->change_key, NULL, NULL);
		snprintf (buffer, 32, "%u", index);
		e_soap_message_add_attribute (msg, "InstanceIndex", buffer, NULL, NULL);
		e_soap_message_end_element (msg);
	} else {
		e_soap_message_start_element (msg, "ItemId", NULL, NULL);
		e_soap_message_add_attribute (msg, "Id", item_id->id, NULL, NULL);
		if (item_id->change_key)
			e_soap_message_add_attribute (msg, "ChangeKey", item_id->change_key, NULL, NULL);
		e_soap_message_end_element (msg);
	}

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_delete_items_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, NULL, pri, cancellable, simple,
				      cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_delete_items_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_delete_items_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean
e_ews_connection_delete_items	(EEwsConnection *cnc,
				 gint pri,
				 GSList *ids,
				 EwsDeleteType delete_type,
				 EwsSendMeetingCancellationsType send_cancels,
				 EwsAffectedTaskOccurrencesType affected_tasks,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_delete_items_start (cnc, pri, ids, delete_type,
					     send_cancels, affected_tasks,
					     ews_sync_reply_cb, cancellable,
					     (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_delete_items_finish (cnc, sync_data->res,
						       error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

gboolean
e_ews_connection_delete_item	(EEwsConnection *cnc,
				 gint pri,
				 EwsId *id,
				 guint index,
				 EwsDeleteType delete_type,
				 EwsSendMeetingCancellationsType send_cancels,
				 EwsAffectedTaskOccurrencesType affected_tasks,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_delete_item_start (cnc, pri, id, index, delete_type,
					     send_cancels, affected_tasks,
					     ews_sync_reply_cb, cancellable,
					     (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_delete_items_finish (cnc, sync_data->res,
						       error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void
e_ews_connection_update_items_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *conflict_res,
					 const gchar *msg_disposition,
					 const gchar *send_invites,
					 const gchar *folder_id,
					 EEwsRequestCreationCallback create_cb,
					 gpointer create_user_data,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "UpdateItem",
					     NULL, NULL, EWS_EXCHANGE_2007_SP1);

	if (conflict_res)
		e_soap_message_add_attribute (msg, "ConflictResolution",
					      conflict_res, NULL, NULL);
	if (msg_disposition)
		e_soap_message_add_attribute (msg, "MessageDisposition",
					      msg_disposition, NULL, NULL);
	if (send_invites)
		e_soap_message_add_attribute (msg, "SendMeetingInvitationsOrCancellations",
					      send_invites, NULL, NULL);

	if (folder_id) {
		e_soap_message_start_element (msg, "SavedItemFolderId", "messages", NULL);
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId",
						     NULL, NULL, "Id", folder_id);
		e_soap_message_end_element (msg);
	}

	e_soap_message_start_element (msg, "ItemChanges", "messages", NULL);

	create_cb (msg, create_user_data);

	e_soap_message_end_element (msg); /* ItemChanges */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_update_items_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_items_response_cb, pri, cancellable, simple,
				      cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_update_items_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **ids,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_update_items_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;
	if (ids)
		*ids = async_data->items;
	else {
		while (async_data->items) {
			g_object_unref (async_data->items->data);
			async_data->items = g_slist_remove (async_data->items,
							    async_data->items->data);
		}
	}
	return TRUE;
}

gboolean
e_ews_connection_update_items	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *conflict_res,
				 const gchar *msg_disposition,
				 const gchar *send_invites,
				 const gchar *folder_id,
				 EEwsRequestCreationCallback create_cb,
				 gpointer create_user_data,
				 GSList **ids,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_update_items_start (cnc, pri, conflict_res,
					     msg_disposition, send_invites,
					     folder_id,
					     create_cb, create_user_data,
					     ews_sync_reply_cb, cancellable,
					     (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_update_items_finish (cnc, sync_data->res,
						       ids, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void
e_ews_connection_create_items_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *msg_disposition,
					 const gchar *send_invites,
					 const gchar *folder_id,
					 EEwsRequestCreationCallback create_cb,
					 gpointer create_user_data,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "CreateItem",
					     NULL, NULL, EWS_EXCHANGE_2007_SP1);

	if (msg_disposition)
		e_soap_message_add_attribute (msg, "MessageDisposition",
					      msg_disposition, NULL, NULL);
	if (send_invites)
		e_soap_message_add_attribute (msg, "SendMeetingInvitations",
					      send_invites, NULL, NULL);

	if (folder_id) {
		e_soap_message_start_element (msg, "SavedItemFolderId", "messages", NULL);
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId",
						     NULL, NULL, "Id", folder_id);
		e_soap_message_end_element (msg);
	}

	e_soap_message_start_element (msg, "Items", "messages", NULL);

	create_cb (msg, create_user_data);

	e_soap_message_end_element (msg); /* Items */

	e_ews_message_write_footer (msg); /* CreateItem */

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_create_items_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_items_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_create_items_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **ids,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_create_items_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;
	*ids = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_create_items	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *msg_disposition,
				 const gchar *send_invites,
				 const gchar *folder_id,
				 EEwsRequestCreationCallback create_cb,
				 gpointer create_user_data,
				 GSList **ids,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_create_items_start (cnc, pri, msg_disposition,
					     send_invites, folder_id,
					     create_cb, create_user_data,
					     ews_sync_reply_cb, cancellable,
					     (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_create_items_finish (cnc, sync_data->res,
						       ids, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

static const gchar *
get_search_scope_str (EwsContactsSearchScope scope)
{
	switch (scope) {
		case EWS_SEARCH_AD:
			return "ActiveDirectory";
		case EWS_SEARCH_AD_CONTACTS:
			return "ActiveDirectoryContacts";
		case EWS_SEARCH_CONTACTS:
			return "Contacts";
		case EWS_SEARCH_CONTACTS_AD:
			return "ContactsActiveDirectory";
		default:
			g_assert_not_reached ();
			return NULL;

	}
}


void
e_ews_connection_resolve_names_start 	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *resolve_name,
					 EwsContactsSearchScope scope,
					 GSList *parent_folder_ids,
					 gboolean fetch_contact_data,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "ResolveNames", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_add_attribute (msg, "SearchScope", get_search_scope_str (scope), NULL, NULL);

	if (fetch_contact_data)
		e_soap_message_add_attribute (msg, "ReturnFullContactData", "true", NULL, NULL);
	else
		e_soap_message_add_attribute (msg, "ReturnFullContactData", "false", NULL, NULL);

	if (parent_folder_ids) {
		e_soap_message_start_element (msg, "ParentFolderIds", "messages", NULL);
		ews_append_folder_ids_to_msg (msg, cnc->priv->email, parent_folder_ids);
		e_soap_message_end_element (msg);
	}

	e_ews_message_write_string_parameter (msg, "UnresolvedEntry", "messages", resolve_name);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_resolve_names_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, resolve_names_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_resolve_names_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **mailboxes,
					 GSList **contact_items,
					 gboolean *includes_last_item,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_resolve_names_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*includes_last_item = async_data->includes_last_item;

	if (contact_items)
		*contact_items = async_data->items_created;
	else {
		g_slist_foreach (async_data->items_created, (GFunc)e_ews_free_resolve_contact, NULL);
		g_slist_free (async_data->items_created);
	}

	*mailboxes = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_resolve_names	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *resolve_name,
				 EwsContactsSearchScope scope,
				 GSList *parent_folder_ids,
				 gboolean fetch_contact_data,
				 GSList **mailboxes,
				 GSList **contact_items,
				 gboolean *includes_last_item,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_resolve_names_start (cnc, pri, resolve_name,
					     scope, parent_folder_ids,
					     fetch_contact_data,
					     ews_sync_reply_cb, cancellable,
					     (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_resolve_names_finish (cnc, sync_data->res,
						       mailboxes, contact_items,
						       includes_last_item, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void		
e_ews_connection_expand_dl_start	(EEwsConnection *cnc,
					 gint pri,
					 const EwsMailbox *mb,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "ExpandDL", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "Mailbox", "messages", NULL);

	if (mb->item_id) {
		e_soap_message_start_element (msg, "ItemId", NULL, NULL);
		
		e_soap_message_add_attribute (msg, "Id", mb->item_id->id, NULL, NULL);
		e_soap_message_add_attribute (msg, "ChangeKey", mb->item_id->change_key, NULL, NULL);
		
		e_soap_message_end_element (msg); /* Mailbox */
		
	} else if (mb->email)
		e_ews_message_write_string_parameter (msg, "EmailAddress", NULL, mb->email);
	
	e_soap_message_end_element (msg); /* Mailbox */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_expand_dl_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, expand_dl_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

/* includes_last_item does not make sense as expand_dl does not support recursive 
   fetch, wierd */
gboolean	
e_ews_connection_expand_dl_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **mailboxes,
					 gboolean *includes_last_item,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_expand_dl_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*includes_last_item = async_data->includes_last_item;
	*mailboxes = async_data->items;

	return TRUE;

}

gboolean	
e_ews_connection_expand_dl	(EEwsConnection *cnc,
				 gint pri,
				 const EwsMailbox *mb,
				 GSList **mailboxes,
				 gboolean *includes_last_item,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_expand_dl_start (cnc, pri, mb,
					  ews_sync_reply_cb, cancellable,
					  (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_expand_dl_finish (cnc, sync_data->res,
						       mailboxes, includes_last_item, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void
e_ews_connection_update_folder_start	(EEwsConnection *cnc,
					 gint pri,
					 EEwsRequestCreationCallback create_cb,
					 gpointer create_user_data,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "UpdateFolder",
					     NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "FolderChanges", "messages", NULL);

	create_cb (msg, create_user_data);

	e_soap_message_end_element (msg); /* FolderChanges */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_update_folder_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, NULL, pri, cancellable, simple,
				      cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_update_folder_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_update_folder_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean
e_ews_connection_update_folder	(EEwsConnection *cnc,
				 gint pri,
				 EEwsRequestCreationCallback create_cb,
				 gpointer create_user_data,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_update_folder_start (cnc, pri,
					      create_cb, create_user_data,
					      ews_sync_reply_cb, cancellable,
					      (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_update_folder_finish (cnc, sync_data->res,
							error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void
e_ews_connection_move_folder_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *to_folder,
					 const gchar *folder,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "MoveFolder",
					     NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "ToFolderId", "messages", NULL);
	if (to_folder)
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL,
								     NULL, "Id", to_folder);
	else
		e_ews_message_write_string_parameter_with_attribute (msg, "DistinguishedFolderId", NULL,
								     NULL, "Id", "msgfolderroot");

	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "FolderIds", "messages", NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL,
							     NULL, "Id", folder);
	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_move_folder_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, NULL, pri, cancellable, simple,
				      cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_move_folder_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_move_folder_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean
e_ews_connection_move_folder	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *to_folder,
				 const gchar *folder,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_move_folder_start (cnc, pri, to_folder, folder,
					    ews_sync_reply_cb, cancellable,
					    (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_move_folder_finish (cnc, sync_data->res,
						      error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}


void
e_ews_connection_get_folder_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *folder_shape,
					 EwsAdditionalProps *add_props,
					 GSList *folder_ids,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "GetFolder",
					     NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "FolderShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, folder_shape);
	e_soap_message_end_element (msg);

	ews_append_additional_props_to_msg (msg, add_props);

	if (folder_ids) {
		e_soap_message_start_element (msg, "FolderIds", "messages", NULL);
		ews_append_folder_ids_to_msg (msg, cnc->priv->email, folder_ids);
		e_soap_message_end_element (msg);
	}

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_get_folder_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_folder_response_cb, pri, cancellable, simple,
				      cb == ews_sync_reply_cb);

}

gboolean	
e_ews_connection_get_folder_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **folders,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_folder_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*folders = async_data->items;

	return TRUE;
}

gboolean	
e_ews_connection_get_folder	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *folder_shape,
				 EwsAdditionalProps *add_props,
				 GSList *folder_ids,
				 GSList **folders,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_get_folder_start (cnc, pri, folder_shape, add_props,
					   folder_ids, ews_sync_reply_cb, cancellable,
					   (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_get_folder_finish (cnc, sync_data->res,
						     folders, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;

}

void
e_ews_connection_create_folder_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *parent_folder_id,
					 gboolean is_distinguished_id,
					 const gchar *folder_name,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "CreateFolder", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "ParentFolderId", "messages", NULL);

	/* If NULL passed for parent_folder_id, use "msgfolderroot" */
	if (is_distinguished_id || !parent_folder_id)
		e_ews_message_write_string_parameter_with_attribute (msg, "DistinguishedFolderId", NULL, NULL, "Id",
								     parent_folder_id?:"msgfolderroot");
	else
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL, NULL, "Id", parent_folder_id);

	if (is_distinguished_id && cnc->priv->email)
		e_ews_message_write_string_parameter (msg, "Mailbox", NULL, cnc->priv->email);

	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "Folders", "messages", NULL);
	e_soap_message_start_element(msg, "Folder", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "DisplayName", NULL, folder_name);

	e_soap_message_end_element (msg);
	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
				      user_data,
                                      e_ews_connection_create_folder_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, ews_create_folder_cb, pri, cancellable, simple, cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_create_folder_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 EwsFolderId **fid,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_create_folder_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*fid = (EwsFolderId *) async_data->items_created->data;
	g_slist_free (async_data->items_created);

	return TRUE;
}


gboolean
e_ews_connection_create_folder	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *parent_folder_id,
				 gboolean is_distinguished_id,
				 const gchar *folder_name,
				 EwsFolderId **folder_id,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_create_folder_start	(cnc, pri, parent_folder_id,
						 is_distinguished_id,
						 folder_name,
						 ews_sync_reply_cb,
						 cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_create_folder_finish (cnc, sync_data->res,
							folder_id,
							error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;

}

void
e_ews_connection_move_items_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *folder_id,
					 gboolean docopy,
					 GSList *ids,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	GSList *l;

	if (docopy)
		msg = e_ews_message_new_with_header (cnc->priv->uri, "CopyItem",
					     NULL, NULL, EWS_EXCHANGE_2007_SP1);
	else
		msg = e_ews_message_new_with_header (cnc->priv->uri, "MoveItem",
					     NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "ToFolderId", "messages", NULL);
	e_soap_message_start_element (msg, "FolderId", NULL, NULL);
	e_soap_message_add_attribute (msg, "Id", folder_id, NULL, NULL);
	e_soap_message_end_element (msg); /* FolderId */
	e_soap_message_end_element (msg); /* ToFolderId */

	e_soap_message_start_element (msg, "ItemIds", "messages", NULL);
	for (l = ids; l != NULL; l = g_slist_next (l))
		e_ews_message_write_string_parameter_with_attribute (msg, "ItemId", NULL, NULL, "Id", l->data);
	e_soap_message_end_element (msg); /* ItemIds */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
					    cb,
					    user_data,
					    e_ews_connection_move_items_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_items_response_cb, pri, cancellable, simple,
				      cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_move_items_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **items,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_move_items_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*items = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_move_items	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *folder_id,
				 gboolean docopy,
				 GSList *ids,
				 GSList **items,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_move_items_start (cnc, pri, folder_id, docopy, ids,
					   ews_sync_reply_cb, cancellable,
					   (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_move_items_finish (cnc, sync_data->res,
						     items, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void
/**
 * e_ews_connection_delete_folder_start
 * @cnc:
 * @pri:
 * @folder_id: folder to be deleted
 * @is_distinguished_id:
 * @delete_type: "HardDelete", "SoftDelete", "MoveToDeletedItems"
 * @cb:
 * @cancellable:
 * @user_data:
 **/
e_ews_connection_delete_folder_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *folder_id,
					 gboolean is_distinguished_id,
					 const gchar *delete_type,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "DeleteFolder", "DeleteType", delete_type, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "FolderIds", "messages", NULL);

	if (is_distinguished_id)
		e_ews_message_write_string_parameter_with_attribute (msg, "DistinguishedFolderId", NULL, NULL, "Id", folder_id);
	else
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL, NULL, "Id", folder_id);

	/*This element is required for delegate access*/
	if (is_distinguished_id && cnc->priv->email)
		e_ews_message_write_string_parameter (msg, "Mailbox", NULL, cnc->priv->email);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
				      user_data,
                                      e_ews_connection_delete_folder_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, NULL, pri, cancellable, simple, cb == ews_sync_reply_cb);
}


gboolean
e_ews_connection_delete_folder_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_delete_folder_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean
/**
 * e_ews_connection_delete_folder
 * @cnc:
 * @pri:
 * @folder_id: folder to be deleted
 * @is_distinguished_id:
 * @delete_type: "HardDelete", "SoftDelete", "MoveToDeletedItems"
 * @cancellable:
 * @error:
 **/
e_ews_connection_delete_folder	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *folder_id,
				 gboolean is_distinguished_id,
				 const gchar *delete_type,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_delete_folder_start	(cnc, pri, folder_id,
						 is_distinguished_id,
						 delete_type,
						 ews_sync_reply_cb,
						 cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_delete_folder_finish (cnc, sync_data->res,
							error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;

}

static void
create_attachments_response_cb (ESoapParameter *param,
				EwsNode *enode)
{
	/* http://msdn.microsoft.com/en-us/library/aa565877%28v=EXCHG.80%29.aspx */
	ESoapParameter *subparam, *attspara, *last_relevant = NULL, *attparam;
	EwsAsyncData *async_data;

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	attspara = e_soap_parameter_get_first_child_by_name (param, "Attachments");

	for (subparam = e_soap_parameter_get_first_child (attspara); subparam != NULL; subparam = e_soap_parameter_get_next_child (subparam)) {
		if (!g_ascii_strcasecmp (e_soap_parameter_get_name(subparam), "FileAttachment")) {
			attparam = e_soap_parameter_get_first_child (subparam);
			last_relevant = attparam;

			async_data->items = g_slist_append (async_data->items, e_soap_parameter_get_property (attparam, "Id"));
		}
	}

	if (last_relevant != NULL) {
		async_data->sync_state = e_soap_parameter_get_property (last_relevant, "RootItemChangeKey");
	}
}

static void
e_ews_connection_attach_file (ESoapMessage *msg,
				const char *uri)
{
	/* TODO - handle a situation where the file isnt accessible/other problem with it */
	/* TODO - This is a naive implementation that just uploads the whole content into memory, ie very inefficient */
	struct stat st;
	char *buffer, *filepath;
	const char *filename;
	int fd;

	/* convert uri to actual file path */
	filepath = g_filename_from_uri (uri, NULL, NULL);

	if (stat (filepath, &st) == -1) {
		g_warning ("Error while calling stat() on %s\n", filepath);
		return;
	}

	fd = open (filepath, O_RDONLY);
	if (fd == -1) {
		g_warning ("Error opening %s for reading\n", filepath);
		return;
	}

	buffer = malloc (st.st_size);
	if (read (fd, buffer, st.st_size) != st.st_size) {
		g_warning ("Error reading %u bytes from %s\n", (unsigned int)st.st_size, filepath);
		close (fd);
		return;
	}
	close (fd);

	filename = strrchr (filepath, '/');
	if (filename) filename++;
	else filename = filepath;

	e_soap_message_start_element (msg, "FileAttachment", NULL, NULL);

	e_ews_message_write_string_parameter (msg, "Name", NULL, filename);

	e_soap_message_start_element (msg, "Content", NULL, NULL);
	e_soap_message_write_base64 (msg, buffer, st.st_size);
	e_soap_message_end_element(msg); /* "Content" */

	e_soap_message_end_element(msg); /* "FileAttachment" */

	free (filepath);
	free (buffer);
}

void
e_ews_connection_create_attachments_start (EEwsConnection *cnc,
					   gint pri,
					   const EwsId *parent,
					   const GSList *files,
					   GAsyncReadyCallback cb,
					   GCancellable *cancellable,
					   gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *l;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "CreateAttachment", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "ParentItemId", "messages", NULL);
	e_soap_message_add_attribute (msg, "Id", parent->id, NULL, NULL);
	if (parent->change_key)
		e_soap_message_add_attribute (msg, "ChangeKey", parent->change_key, NULL, NULL);
	e_soap_message_end_element(msg);

	/* start interation over all items to get the attachemnts */
	e_soap_message_start_element (msg, "Attachments", "messages", NULL);

	for (l = files; l != NULL; l = g_slist_next (l))
		e_ews_connection_attach_file (msg, l->data);

	e_soap_message_end_element (msg); /* "Attachments" */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
				      cb,
				      user_data,
				      e_ews_connection_create_attachments_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, create_attachments_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

GSList *
e_ews_connection_create_attachments_finish (EEwsConnection *cnc,
					    gchar **change_key,
					    GAsyncResult *result,
					    GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	GSList *ids = NULL;

	g_return_val_if_fail (
			g_simple_async_result_is_valid (
					result, G_OBJECT (cnc), e_ews_connection_create_attachments_start),
			NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	ids = async_data->items;
	*change_key = async_data->sync_state;

	return ids;
}

GSList *
e_ews_connection_create_attachments (EEwsConnection *cnc,
				     gint pri,
				     const EwsId *parent,
				     const GSList *files,
				     gchar **change_key,
				     GCancellable *cancellable,
				     GError **error)
{
	EwsSyncData *sync_data;
	GSList *ids;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_create_attachments_start (cnc, pri,
						 parent,
						 files,
						 ews_sync_reply_cb,
						 cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	ids = e_ews_connection_create_attachments_finish (cnc, change_key, sync_data->res,
							error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return ids;
}

/* Delete attachemnts */
static void
delete_attachments_response_cb (ESoapParameter *subparam, EwsNode *enode)
{
	/* http://msdn.microsoft.com/en-us/library/aa580782%28v=EXCHG.80%29.aspx */
	ESoapParameter *attspara;
	EwsAsyncData *async_data;

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	attspara = e_soap_parameter_get_first_child_by_name (subparam, "RootItemId");

	if (!attspara) return;

	async_data->items = g_slist_append (async_data->items, e_soap_parameter_get_property (attspara, "RootItemChangeKey"));
}

void
e_ews_connection_delete_attachments_start (EEwsConnection *cnc,
					 gint pri,
					 const GSList *ids,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *l;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "DeleteAttachment", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	/* start interation over all items to get the attachemnts */
	e_soap_message_start_element (msg, "AttachmentIds", "messages", NULL);

	for (l = ids; l != NULL; l = g_slist_next (l)) {
		e_ews_message_write_string_parameter_with_attribute (msg, "AttachmentId", NULL, NULL, "Id", l->data);
	}

	e_soap_message_end_element (msg); /* "AttachmentIds" */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
				      cb,
				      user_data,
				      e_ews_connection_delete_attachments_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, delete_attachments_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

GSList *
e_ews_connection_delete_attachments_finish (EEwsConnection *cnc,
					 GAsyncResult *result,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	GSList *ids = NULL;

	g_return_val_if_fail (
			g_simple_async_result_is_valid (
					result, G_OBJECT (cnc), e_ews_connection_delete_attachments_start),
			NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	ids = async_data->items;

	return ids;
}

GSList *
e_ews_connection_delete_attachments (EEwsConnection *cnc,
				 gint pri,
				 const GSList *ids,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	GSList *parents;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_delete_attachments_start (cnc, pri,
						 ids,
						 ews_sync_reply_cb,
						 cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	parents = e_ews_connection_delete_attachments_finish (cnc, sync_data->res,
							error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return parents;
}

static void get_attachments_response_cb (ESoapParameter *subparam, EwsNode *enode);

void
e_ews_connection_get_attachments_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *uid,
					 const GSList *ids,
					 const gchar *cache,
					 gboolean include_mime,
					 GAsyncReadyCallback cb,
					 ESoapProgressFn progress_fn,
					 gpointer progress_data,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *l;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "GetAttachment", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	/* not sure why I need it, need to check */
	if (progress_fn && progress_data)
		e_soap_message_set_progress_fn (msg, progress_fn, progress_data);

	if (cache)
		e_soap_message_store_node_data (msg, "MimeContent Content", cache, TRUE);

	/* wrtie empty attachments shape, need to discover maybe usefull in some cases*/
	e_soap_message_start_element (msg, "AttachmentShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "IncludeMimeContent", NULL, "true");
	e_soap_message_end_element(msg);

	/* start interation over all items to get the attachemnts */
	e_soap_message_start_element (msg, "AttachmentIds", "messages", NULL);

	for (l = ids; l != NULL; l = g_slist_next (l))
		e_ews_message_write_string_parameter_with_attribute (msg, "AttachmentId", NULL, NULL, "Id", l->data);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (G_OBJECT (cnc),
				      cb,
				      user_data,
				      e_ews_connection_get_attachments_start);

	async_data = g_new0 (EwsAsyncData, 1);
	async_data->directory = cache;
	async_data->sync_state = (gchar *)uid;
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_attachments_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

GSList *
e_ews_connection_get_attachments_finish(EEwsConnection *cnc,
					 GAsyncResult *result,
					 GSList **items,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
			g_simple_async_result_is_valid (
					result, G_OBJECT (cnc), e_ews_connection_get_attachments_start),
			NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	*items = async_data->items;

	return async_data->items_created;
}

GSList *
e_ews_connection_get_attachments(EEwsConnection *cnc,
				 gint pri,
				 const gchar *uid,
				 GSList *ids,
				 const gchar *cache,
				 gboolean include_mime,
				 GSList **items,
				 ESoapProgressFn progress_fn,
				 gpointer progress_data,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	GSList *attachments_ids;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_get_attachments_start	(cnc,pri,uid,ids,cache,include_mime,
						 ews_sync_reply_cb,
						 progress_fn, progress_data,
						 cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	attachments_ids = e_ews_connection_get_attachments_finish(cnc,
						    sync_data->res,
						    items,
						    error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return attachments_ids;
}

static void
get_attachments_response_cb (ESoapParameter *param, EwsNode *enode)
{
	ESoapParameter *subparam, *attspara;
	EwsAsyncData *async_data;
	gchar *uri = NULL, *attach_id = NULL;
	EEwsItem *item;
	const gchar *name;

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	attspara = e_soap_parameter_get_first_child_by_name (param, "Attachments");

	for (subparam = e_soap_parameter_get_first_child (attspara); subparam != NULL; subparam = e_soap_parameter_get_next_child (subparam)) {
		name = e_soap_parameter_get_name(subparam);

		if (!g_ascii_strcasecmp (name, "ItemAttachment")) {
			item = e_ews_item_new_from_soap_parameter(subparam);
			attach_id = g_strdup (e_ews_item_get_attachment_id (item)->id);
			uri = e_ews_item_dump_mime_content(item, async_data->directory);

		}
		else if (!g_ascii_strcasecmp (name, "FileAttachment")) {
			uri = e_ews_dump_file_attachment_from_soap_parameter(subparam, async_data->directory, async_data->sync_state, &attach_id);
		}
		if (uri && attach_id) {
			async_data->items = g_slist_append (async_data->items, uri);
			async_data->items_created = g_slist_append (async_data->items_created, attach_id);
			uri = NULL;
			attach_id = NULL;
		}
	}
}

static void
get_free_busy_response_cb (ESoapParameter *param, EwsNode *enode)
{
       /*parse the response to create a free_busy data
        http://msdn.microsoft.com/en-us/library/aa564001%28v=EXCHG.140%29.aspx*/
	icalcomponent *vfb;
	icalproperty *icalprop = NULL;
	struct icalperiodtype ipt;
	ESoapParameter *viewparam, *eventarray, *event_param, *subparam;
	GTimeVal t_val;
	const gchar *name;
	gchar *value, *new_val = NULL;
	EwsAsyncData *async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	viewparam = e_soap_parameter_get_first_child_by_name (param, "FreeBusyView");
	if (!viewparam) return;
	vfb = icalcomponent_new_vfreebusy ();
	eventarray = e_soap_parameter_get_first_child_by_name (viewparam, "CalendarEventArray");
	for (event_param = e_soap_parameter_get_first_child (eventarray); event_param != NULL; event_param = e_soap_parameter_get_next_child (event_param), icalprop = NULL) {
		for (subparam = e_soap_parameter_get_first_child (event_param); subparam != NULL; subparam = e_soap_parameter_get_next_child (subparam)) {
			name = e_soap_parameter_get_name (subparam);

			if (!g_ascii_strcasecmp (name, "StartTime")) {
				value = e_soap_parameter_get_string_value (subparam);
				/*We are sending UTC timezone and expect server to return in same*/
				
				/*Remove leading and trailing whitespace*/
				g_strstrip (value);

				if (g_utf8_strlen (value, -1) == 19) {
					/*If server returns time without zone add Z to treat it in UTC*/
					new_val = g_strdup_printf ("%sZ", value);
					g_free (value);
				} else
					new_val = value;
				
				g_time_val_from_iso8601 (new_val, &t_val);
				g_free (new_val);

				ipt.start = icaltime_from_timet_with_zone (t_val.tv_sec, 0, NULL);

			} else if (!g_ascii_strcasecmp (name, "EndTime")) {
				value = e_soap_parameter_get_string_value (subparam);
				/*We are sending UTC timezone and expect server to return in same*/
				
				/*Remove leading and trailing whitespace*/
				g_strstrip (value);

				if (g_utf8_strlen (value, -1) == 19) {
					/*If server returns time without zone add Z to treat it in UTC*/
					new_val = g_strdup_printf ("%sZ", value);
					g_free (value);
				} else
					new_val = value;

				g_time_val_from_iso8601 (new_val, &t_val);
				g_free (new_val);

				ipt.end = icaltime_from_timet_with_zone (t_val.tv_sec, 0, NULL);

				icalprop = icalproperty_new_freebusy (ipt);
			} else if (!g_ascii_strcasecmp (name, "BusyType")) {
				value = e_soap_parameter_get_string_value (subparam);
				if (!strcmp (value, "Busy"))
					icalproperty_set_parameter_from_string (icalprop, "FBTYPE", "BUSY");
				else if (!strcmp (value, "Tentative"))
					icalproperty_set_parameter_from_string (icalprop, "FBTYPE", "BUSY-TENTATIVE");
				else if (!strcmp (value, "OOF"))
					icalproperty_set_parameter_from_string (icalprop, "FBTYPE", "BUSY-UNAVAILABLE");
				else if (!strcmp (value, "Free"))
					icalproperty_set_parameter_from_string (icalprop, "FBTYPE", "FREE");
				g_free (value);
			}
		}
		if (icalprop != NULL) icalcomponent_add_property(vfb, icalprop);
	}

	async_data->items = g_slist_append (async_data->items, vfb);
}

void
e_ews_connection_get_free_busy_start (EEwsConnection *cnc,
				      gint pri,
				      EEwsRequestCreationCallback free_busy_cb,
				      gpointer free_busy_user_data,
				      GAsyncReadyCallback cb,
				      GCancellable *cancellable,
				      gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "GetUserAvailabilityRequest",
					     NULL, NULL, EWS_EXCHANGE_2007_SP1);

	free_busy_cb (msg, free_busy_user_data);

	e_ews_message_write_footer (msg); /*GetUserAvailabilityRequest  */

	simple = g_simple_async_result_new (G_OBJECT (cnc),
					    cb,
					    user_data,
					    e_ews_connection_get_free_busy_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
						   simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_free_busy_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_get_free_busy_finish (EEwsConnection *cnc,
				       GAsyncResult *result,
				       GSList **free_busy,
				       GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
			      g_simple_async_result_is_valid (
							      result, G_OBJECT (cnc), e_ews_connection_get_free_busy_start),
			      FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;
	*free_busy = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_get_free_busy (EEwsConnection *cnc,
				gint pri,
				EEwsRequestCreationCallback free_busy_cb,
				gpointer free_busy_user_data,
				GSList **free_busy,
				GCancellable *cancellable,
				GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_get_free_busy_start (cnc, pri,
					      free_busy_cb, free_busy_user_data,
					      ews_sync_reply_cb, cancellable,
					      (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_get_free_busy_finish (cnc, sync_data->res,
							free_busy, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

static EwsPermissionLevel get_permission_from_string(gchar *permission)
{
	g_return_val_if_fail(permission != NULL, NONE);

	if (!g_ascii_strcasecmp (permission, "Editor"))
		return EWS_PERM_EDITOR;
	else if (!g_ascii_strcasecmp (permission, "Author"))
		return EWS_PERM_AUTHOR;
	else if (!g_ascii_strcasecmp (permission, "Reviewer"))
		return EWS_PERM_REVIEWER;
	else if (!g_ascii_strcasecmp (permission, "Custom"))
		return CUSTOM;
	else
		return NONE;

}

static void
get_delegate_response_cb (ESoapParameter *param, EwsNode *enode)
{
	ESoapParameter *subparam, *node, *child;
	EwsAsyncData *async_data;
	EwsDelegateInfo *data;
	gchar *value;

	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);

	node = e_soap_parameter_get_first_child_by_name (param, "DelegateUser");

	data = g_new(EwsDelegateInfo, 1);
	data->user_id = g_new0(EwsUserId, 1);

	subparam = e_soap_parameter_get_first_child_by_name(node, "UserId");

	/*Parse User Id*/
	
	child = e_soap_parameter_get_first_child_by_name(subparam, "SID");
	data->user_id->sid = e_soap_parameter_get_string_value(child);

	child = e_soap_parameter_get_first_child_by_name(subparam, "PrimarySmtpAddress");
	data->user_id->primary_smtp_add = e_soap_parameter_get_string_value(child);

	child = e_soap_parameter_get_first_child_by_name(subparam, "DisplayName");
	data->user_id->display_name = e_soap_parameter_get_string_value(child);

	subparam = e_soap_parameter_get_first_child_by_name(node, "DelegatePermissions");
	/*Parse Delegate Permissions*/

	child = e_soap_parameter_get_first_child_by_name(subparam, "CalendarFolderPermissionLevel");
	data->calendar = get_permission_from_string(e_soap_parameter_get_string_value(child));

	child = e_soap_parameter_get_first_child_by_name(subparam, "ContactsFolderPermissionLevel");
	data->contact = get_permission_from_string(e_soap_parameter_get_string_value(child));

	child = e_soap_parameter_get_first_child_by_name(subparam, "InboxFolderPermissionLevel");
	data->inbox = get_permission_from_string(e_soap_parameter_get_string_value(child));

	child = e_soap_parameter_get_first_child_by_name(subparam, "TasksFolderPermissionLevel");
	data->tasks = get_permission_from_string(e_soap_parameter_get_string_value(child));

	child = e_soap_parameter_get_first_child_by_name(subparam, "NotesFolderPermissionLevel");
	data->notes = get_permission_from_string(e_soap_parameter_get_string_value(child));

	child = e_soap_parameter_get_first_child_by_name(subparam, "JournalFolderPermissionLevel");
	data->journal = get_permission_from_string(e_soap_parameter_get_string_value(child));

	subparam = e_soap_parameter_get_first_child_by_name(node, "ReceiveCopiesOfMeetingMessages");

	value = e_soap_parameter_get_string_value(subparam);
	if(!g_ascii_strcasecmp(value, "true"))
		data->meetingcopies = TRUE;

	subparam = e_soap_parameter_get_first_child_by_name(node, "ViewPrivateItems");

	value = e_soap_parameter_get_string_value(subparam);
	if(!g_ascii_strcasecmp(value, "true"))
		data->view_priv_items = TRUE;
	else
		data->view_priv_items = FALSE;

	async_data->items = g_slist_append (async_data->items, data);
	async_data->items_created = g_slist_append (async_data->items_created, data);

	return;
}


/**
 * e_ews_connection_get_delegate_start
 * @cnc:
 * @pri:
 * @mail_id: mail is for which delegate is requested
 * @include permission: "true", "false"
 * @delete_type: "HardDelete", "SoftDelete", "MoveToDeletedItems"
 * @cb:
 * @cancellable:
 * @user_data:
 **/
void
e_ews_connection_get_delegate_start	(EEwsConnection *cnc,
					 gint pri,
					 const gchar *mail_id,
					 const gchar *include_permissions,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "GetDelegate", "IncludePermissions", include_permissions, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "Mailbox", "messages", NULL);

	e_ews_message_write_string_parameter (msg, "EmailAddress", NULL, mail_id);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
				      user_data,
                                      e_ews_connection_get_delegate_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_delegate_response_cb, pri, cancellable, simple, cb == ews_sync_reply_cb);
}


gboolean
e_ews_connection_get_delegate_finish	(EEwsConnection *cnc,
					 GAsyncResult *result,
					 EwsDelegateInfo **get_delegate,
					 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_delegate_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;
	*get_delegate = (EwsDelegateInfo *) async_data->items_created->data;
	g_slist_free (async_data->items_created);

	return TRUE;
}

/**
 * e_ews_connection_get_delegate
 * @cnc:
 * @pri:
 * @mail_id: mail id for which delegate requested
 * @include_permissions: "true", "false"
 * @cancellable:
 * @error:
 **/
gboolean
e_ews_connection_get_delegate	(EEwsConnection *cnc,
				 gint pri,
				 const gchar *mail_id,
				 const gchar *include_permissions,
				 EwsDelegateInfo **get_delegate,
				 GCancellable *cancellable,
				 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_get_delegate_start	(cnc, pri, mail_id,
						 include_permissions,
						 ews_sync_reply_cb,
						 cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_get_delegate_finish (cnc, sync_data->res,
							get_delegate, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;

}

/**
 * e_ews_connection__get_oof_settings_start
 * @cnc: The EWS Connection
 * @pri: The priority associated with the request
 * @cb: Responses are parsed and returned to this callback
 * @cancellable: a GCancellable to monitor cancelled operations
 * @user_data: user data passed to callback
 **/
void
e_ews_connection_get_oof_settings_start	(EEwsConnection *cnc,
					 gint pri,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "GetUserOofSettingsRequest", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	e_soap_message_start_element (msg, "Mailbox", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "Address", NULL, cnc->priv->email);
	e_soap_message_end_element (msg);

	/* Complete the footer and print the request */
	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
					    cb, user_data,
					    e_ews_connection_get_oof_settings_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, get_oof_settings_response_cb, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);
}

gboolean
e_ews_connection_get_oof_settings_finish	(EEwsConnection *cnc,
						 GAsyncResult *result,
						 OOFSettings **oof_settings,
						 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_oof_settings_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*oof_settings = (OOFSettings *) async_data->items->data;

	return TRUE;
}

gboolean
e_ews_connection_get_oof_settings	(EEwsConnection *cnc,
					 gint pri,
					 OOFSettings **oof_settings,
					 GCancellable *cancellable,
					 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_get_oof_settings_start	(cnc, pri,
						 ews_sync_reply_cb, cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_get_oof_settings_finish (cnc, sync_data->res,
							    oof_settings, error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

/**
 * e_ews_connection__set_oof_settings_start
 * @cnc: The EWS Connection
 * @pri: The priority associated with the request
 * @oof_settings: Details to set for ooof
 * @cb: Responses are parsed and returned to this callback
 * @cancellable: a GCancellable to monitor cancelled operations
 * @user_data: user data passed to callback
 **/
void
e_ews_connection_set_oof_settings_start	(EEwsConnection *cnc,
					 gint pri,
					 OOFSettings *oof_settings,
					 GAsyncReadyCallback cb,
					 GCancellable *cancellable,
					 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	gchar *start_tm = NULL, *end_tm = NULL;
	GTimeVal *time_val;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "SetUserOofSettingsRequest", NULL, NULL, EWS_EXCHANGE_2007_SP1);

	/*Set Mailbox to user Address we want to set*/
	e_soap_message_start_element (msg, "Mailbox", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "Address", NULL, cnc->priv->email);
	e_soap_message_end_element (msg);

	/*Write out of office settings to message*/
	e_soap_message_start_element (msg, "UserOofSettings", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "OofState", NULL, oof_settings->state);
	e_ews_message_write_string_parameter (msg, "ExternalAudience", NULL, oof_settings->ext_aud);

	time_val = g_new0 (GTimeVal, 1);
	time_val->tv_sec = oof_settings->start_tm;
	start_tm = g_time_val_to_iso8601 (time_val);

	time_val->tv_sec = oof_settings->end_tm;
	end_tm = g_time_val_to_iso8601 (time_val);

	e_soap_message_start_element (msg, "Duration", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "StartTime", NULL, start_tm);
	e_ews_message_write_string_parameter (msg, "EndTime", NULL, end_tm);
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "InternalReply", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "Message", NULL, oof_settings->int_reply);
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "ExternalReply", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "Message", NULL, oof_settings->ext_reply);
	e_soap_message_end_element (msg);

	e_soap_message_end_element (msg);

	/* Complete the footer and print the request */
	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
					    cb, user_data,
					    e_ews_connection_set_oof_settings_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, NULL, pri,
				      cancellable, simple, cb == ews_sync_reply_cb);

	g_free (time_val);
	g_free (start_tm);
	g_free (end_tm);
}

gboolean
e_ews_connection_set_oof_settings_finish	(EEwsConnection *cnc,
						 GAsyncResult *result,
						 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_set_oof_settings_start),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean
e_ews_connection_set_oof_settings	(EEwsConnection *cnc,
					 gint pri,
					 OOFSettings *oof_settings,
					 GCancellable *cancellable,
					 GError **error)
{
	EwsSyncData *sync_data;
	gboolean result;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->eflag = e_flag_new ();

	e_ews_connection_set_oof_settings_start	(cnc, pri, oof_settings,
						 ews_sync_reply_cb, cancellable,
						 (gpointer) sync_data);

	e_flag_wait (sync_data->eflag);

	result = e_ews_connection_set_oof_settings_finish (cnc, sync_data->res,
							    error);

	e_flag_free (sync_data->eflag);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return result;
}

void
e_ews_connection_free_oof_settings (OOFSettings *oof_settings)
{
	if (oof_settings->state) {
		g_free (oof_settings->state);
		oof_settings->state = NULL;
	}
	if (oof_settings->ext_aud) {
		g_free (oof_settings->ext_aud);
		oof_settings->ext_aud = NULL;
	}
	if (oof_settings->int_reply) {
		g_free (oof_settings->int_reply);
		oof_settings->int_reply = NULL;
	}
	if (oof_settings->ext_reply) {
		g_free (oof_settings->ext_reply);
		oof_settings->ext_reply = NULL;
	}

	if (oof_settings) {
		g_free (oof_settings);
		oof_settings = NULL;
	}
}
