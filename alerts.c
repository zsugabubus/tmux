/* $OpenBSD$ */

/*
 * Copyright (c) 2015 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>

#include "tmux.h"

static int	alerts_fired;

static void	alerts_timer(int, short, void *);
static int	alerts_enabled(struct window *, int);
static void	alerts_callback(int, short, void *);
static void	alerts_reset(struct window *);

static int	alerts_action_applies(const struct winlink *,
		    const struct client *, const char *);
static int	alerts_check_all(struct window *);
static int	alerts_check_bell(struct window *);
static int	alerts_check_activity(struct window *);
static int	alerts_check_silence(struct window *);
static int	alerts_check_window(struct window *, int, int, int,
		    const char *, const char *);
static void	alerts_set_message(struct winlink *, const char *,
		    const char *);

static TAILQ_HEAD(, window) alerts_list = TAILQ_HEAD_INITIALIZER(alerts_list);

static void
alerts_timer(__unused int fd, __unused short events, void *arg)
{
	struct window	*w = arg;

	log_debug("@%u alerts timer expired", w->id);
	alerts_queue(w, WINDOW_SILENCE);
}

static void
alerts_callback(__unused int fd, __unused short events, __unused void *arg)
{
	struct window	*w, *w1;
	int		 alerts;

	TAILQ_FOREACH_SAFE(w, &alerts_list, alerts_entry, w1) {
		alerts = alerts_check_all(w);
		log_debug("@%u alerts check, alerts %#x", w->id, alerts);

		w->alerts_queued = 0;
		TAILQ_REMOVE(&alerts_list, w, alerts_entry);

		w->flags &= ~WINDOW_ALERTFLAGS;
		window_remove_ref(w, __func__);
	}
	alerts_fired = 0;
}

static int
alerts_action_applies(const struct winlink *wl, const struct client *c,
    const char *name)
{
	int	action;

	/*
	 * {bell,activity,silence}-action determines when to alert: none means
	 * nothing happens, current means only do something for the current
	 * window and other means only for windows other than the current.
	 */

	action = options_get_number(wl->session->options, name);
	if (wl->session == c->session) {
		if (action == ALERT_ANY || action == ALERT_SERVER_ANY)
			return (1);
		if (action == ALERT_CURRENT)
			return (wl == wl->session->curw);
		if (action == ALERT_OTHER || action == ALERT_SERVER_OTHER)
			return (wl != wl->session->curw);
	} else {
		if (action == ALERT_SERVER_ANY || action == ALERT_SERVER_OTHER)
			return (1);
	}
	return (0);
}

static int
alerts_check_all(struct window *w)
{
	int	alerts;

	alerts	= alerts_check_bell(w);
	alerts |= alerts_check_activity(w);
	alerts |= alerts_check_silence(w);
	return (alerts);
}

void
alerts_check_session(struct session *s)
{
	struct winlink	*wl;

	RB_FOREACH(wl, winlinks, &s->windows)
		alerts_check_all(wl->window);
}

static int
alerts_enabled(struct window *w, int flags)
{
	if (flags & WINDOW_BELL) {
		if (options_get_number(w->options, "monitor-bell"))
			return (1);
	}
	if (flags & WINDOW_ACTIVITY) {
		if (options_get_number(w->options, "monitor-activity"))
			return (1);
	}
	if (flags & WINDOW_SILENCE) {
		if (options_get_number(w->options, "monitor-silence") != 0)
			return (1);
	}
	return (0);
}

void
alerts_reset_all(void)
{
	struct window	*w;

	RB_FOREACH(w, windows, &windows)
		alerts_reset(w);
}

static void
alerts_reset(struct window *w)
{
	struct timeval	tv;

	if (!event_initialized(&w->alerts_timer))
		evtimer_set(&w->alerts_timer, alerts_timer, w);

	w->flags &= ~WINDOW_SILENCE;
	event_del(&w->alerts_timer);

	timerclear(&tv);
	tv.tv_sec = options_get_number(w->options, "monitor-silence");

	log_debug("@%u alerts timer reset %u", w->id, (u_int)tv.tv_sec);
	if (tv.tv_sec != 0)
		event_add(&w->alerts_timer, &tv);
}

void
alerts_queue(struct window *w, int flags)
{
	alerts_reset(w);

	if ((w->flags & flags) != flags) {
		w->flags |= flags;
		log_debug("@%u alerts flags added %#x", w->id, flags);
	}

	if (alerts_enabled(w, flags)) {
		if (!w->alerts_queued) {
			w->alerts_queued = 1;
			TAILQ_INSERT_TAIL(&alerts_list, w, alerts_entry);
			window_add_ref(w, __func__);
		}

		if (!alerts_fired) {
			log_debug("alerts check queued (by @%u)", w->id);
			event_once(-1, EV_TIMEOUT, alerts_callback, NULL, NULL);
			alerts_fired = 1;
		}
	}
}

static int
alerts_check_bell(struct window *w)
{
	return alerts_check_window(w, WINDOW_BELL, WINLINK_BELL, 1,
	    "Bell", "bell");
}

static int
alerts_check_activity(struct window *w)
{
	return alerts_check_window(w, WINDOW_ACTIVITY, WINLINK_ACTIVITY, 0,
	    "Activity", "activity");
}

static int
alerts_check_silence(struct window *w)
{
	return alerts_check_window(w, WINDOW_SILENCE, WINLINK_SILENCE, 0,
	    "Silence", "silence");
}

static int
alerts_check_window(struct window *w, int wflag, int wlflag, int delivery_all,
    const char *type, const char *name)
{
	struct winlink	*wl;
	struct session	*s;
	char		monitor_name[50];

	sprintf(monitor_name, "monitor-%s", name);

	if (~w->flags & wflag)
		return (0);
	if (options_get_number(w->options, monitor_name) == 0)
		return (0);

	TAILQ_FOREACH(wl, &w->winlinks, wentry)
		wl->session->flags &= ~SESSION_ALERTED;

	TAILQ_FOREACH(wl, &w->winlinks, wentry) {
		if (!delivery_all && (wl->flags & wlflag))
			continue;
		s = wl->session;
		if (s->curw != wl || s->attached == 0) {
			wl->flags |= wflag;
			server_status_session(s);
		}
		alerts_set_message(wl, type, name);
	}

	return (wflag);
}

static void
alerts_set_message(struct winlink *wl, const char *type, const char *name)
{
	struct client	*c;
	int		 visual;
	char		 action_name[16];
	char		 alert_name[16];
	char		 visual_name[16];

	/*
	 * We have found an alert (bell, activity or silence), so we need to
	 * pass it on to the user. For each client, decide whether a bell,
	 * message or both is needed.
	 *
	 * If visual-{bell,activity,silence} is on, then a message is
	 * substituted for a bell; if it is off, a bell is sent as normal; both
	 * mean both a bell and message is sent.
	 */

	sprintf(action_name, "%s-action", name);
	sprintf(alert_name, "alert-%s", name);
	sprintf(visual_name, "visual-%s", name);

	visual = options_get_number(wl->session->options, visual_name);
	TAILQ_FOREACH(c, &clients, entry)
		if (!alerts_action_applies(wl, c, action_name))
			return;

	notify_winlink(alert_name, wl);

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session == NULL || (c->flags & CLIENT_CONTROL))
			continue;

		if (visual == VISUAL_OFF || visual == VISUAL_BOTH)
			tty_putcode(&c->tty, TTYC_BEL);
		if (visual == VISUAL_OFF)
			continue;
		if (c->session->curw == wl) {
			status_message_set(c, -1, 1, 0, 0,
			    "%s in current window", type);
		} else {
			status_message_set(c, -1, 1, 0, 0,
			    "%s in %s:%s", type, wl->session->name, wl->window->name);
		}
	}
}
