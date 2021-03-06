/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
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

#ifndef E_CAL_BACKEND_EWS_UTILS_H
#define E_CAL_BACKEND_EWS_UTILS_H

#include <libecal/libecal.h>
#include <libical/icaltime.h>
#include <libical/icaltimezone.h>

#include "server/e-ews-connection.h"
#include "server/e-ews-item-change.h"

#include "e-cal-backend-ews.h"

G_BEGIN_DECLS
#define EDC_ERROR(_code) e_data_cal_create_error (_code, NULL)
#define EDC_ERROR_EX(_code, _msg) e_data_cal_create_error (_code, _msg)

#define MINUTES_IN_HOUR 60
#define SECS_IN_MINUTE 60

typedef struct {
	EEwsConnection *connection;
	ETimezoneCache *timezone_cache;
	icaltimezone *default_zone;
	gchar *user_email;
	gchar *response_type; /* Accept */
	GSList *users;
	ECalComponent *comp;
	ECalComponent *old_comp;
	icalcomponent *icalcomp;
	icalcomponent *vcalendar; /* can be NULL, parent of icalcomp, where timezones can be eventually found */
	gchar *item_id;
	gchar *change_key;
	EEwsItemChangeType change_type;
	gint index;
	time_t start;
	time_t end;
} EwsCalendarConvertData;

const gchar *e_ews_collect_organizer (icalcomponent *comp);
void e_ews_collect_attendees (icalcomponent *comp, GSList **required, GSList **optional, GSList **resource, gboolean *out_rsvp_requested);

void ewscal_set_timezone (ESoapMessage *msg, const gchar *name, EEwsCalendarTimeZoneDefinition *tzd);
void ewscal_set_meeting_timezone (ESoapMessage *msg, icaltimezone *icaltz);
void ewscal_set_reccurence (ESoapMessage *msg, icalproperty *rrule, icaltimetype *dtstart);
void ewscal_set_reccurence_exceptions (ESoapMessage *msg, icalcomponent *comp);
gchar *e_ews_extract_attachment_id_from_uri (const gchar *uri);
void ews_set_alarm (ESoapMessage *msg, ECalComponent *comp, ETimezoneCache *timezone_cache, icalcomponent *vcalendar, gboolean with_due_by);
gint ews_get_alarm (ECalComponent *comp);
void e_ews_clean_icalcomponent (icalcomponent *icalcomp);

const gchar *e_cal_backend_ews_tz_util_get_msdn_equivalent (const gchar *ical_tz_location);
const gchar *e_cal_backend_ews_tz_util_get_ical_equivalent (const gchar *msdn_tz_location);
void e_cal_backend_ews_populate_windows_zones (void);
void e_cal_backend_ews_unref_windows_zones (void);

gboolean e_cal_backend_ews_convert_calcomp_to_xml (ESoapMessage *msg, gpointer user_data, GError **error);
gboolean e_cal_backend_ews_convert_component_to_updatexml (ESoapMessage *msg, gpointer user_data, GError **error);
gboolean e_cal_backend_ews_clear_reminder_is_set (ESoapMessage *msg, gpointer user_data, GError **error);
gboolean e_cal_backend_ews_prepare_set_free_busy_status (ESoapMessage *msg,gpointer user_data, GError **error);
gboolean e_cal_backend_ews_prepare_accept_item_request (ESoapMessage *msg, gpointer user_data, GError **error);

guint e_cal_backend_ews_rid_to_index (icaltimezone *timezone, const gchar *rid, icalcomponent *comp, GError **error);

struct icaltimetype
		e_cal_backend_ews_get_datetime_with_zone	(ETimezoneCache *timezone_cache,
								 icalcomponent *vcalendar,
								 icalcomponent *comp,
								 icalproperty_kind prop_kind,
								 struct icaltimetype (* get_func) (const icalproperty *prop));

G_END_DECLS

#endif
