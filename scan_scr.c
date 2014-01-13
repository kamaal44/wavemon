/*
 * wavemon - a wireless network monitoring aplication
 *
 * Copyright (c) 2001-2002 Jan Morgenstern <jan@jm-music.de>
 *
 * wavemon is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any later
 * version.
 *
 * wavemon is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along
 * with wavemon; see the file COPYING.  If not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "iw_if.h"

#define START_LINE	2	/* where to begin the screen */

/* GLOBALS */
static struct scan_result sr;
static pthread_t scan_thread;
static WINDOW *w_aplst;

/**
 * Sanitize and format single scan entry as a string.
 * @cur: entry to format
 * @buf: buffer to put results into
 * @buflen: length of @buf
 */
static void fmt_scan_entry(struct scan_entry *cur, char buf[], size_t buflen)
{
	size_t len = 0;

	if (!(cur->qual.updated & (IW_QUAL_QUAL_INVALID|IW_QUAL_LEVEL_INVALID)))
		len += snprintf(buf + len, buflen - len, "%3.0f%%, %.0f dBm",
				1E2 * cur->qual.qual / sr.range.max_qual.qual,
				cur->dbm.signal);
	else if (!(cur->qual.updated & IW_QUAL_QUAL_INVALID))
		len += snprintf(buf + len, buflen - len, "%2d/%d",
				cur->qual.qual, sr.range.max_qual.qual);
	else if (!(cur->qual.updated & IW_QUAL_LEVEL_INVALID))
		len += snprintf(buf + len, buflen - len, "%.0f dBm",
				cur->dbm.signal);
	else
		len += snprintf(buf + len, buflen - len, "? dBm");

	if (cur->freq < 1e3)
		len += snprintf(buf + len, buflen - len, ", Chan %2.0f",
				cur->freq);
	else if (cur->chan >= 0)
		len += snprintf(buf + len, buflen - len, ", %s %3d, %g MHz",
				cur->freq < 5e9 ? "ch" : "CH",
				cur->chan, cur->freq / 1e6);
	else
		len += snprintf(buf + len, buflen - len, ", %g GHz",
				cur->freq / 1e9);

	/* Access Points are marked by CP_SCAN_CRYPT/CP_SCAN_UNENC already */
	if (cur->mode != IW_MODE_MASTER)
		len += snprintf(buf + len, buflen - len, " %s",
				iw_opmode(cur->mode));
	if (cur->flags)
		len += snprintf(buf + len, buflen - len, ", %s",
				 format_enc_capab(cur->flags, "/"));
}

static void display_aplist(WINDOW *w_aplst)
{
	char s[IW_ESSID_MAX_SIZE << 3];
	const char *sort_type[] = {
		[SO_CHAN]	= "Chan",
		[SO_SIGNAL]	= "Sig",
		[SO_ESSID]	= "Essid",
		[SO_OPEN]	= "Open",
		[SO_CHAN_SIG]	= "Ch/Sg",
		[SO_OPEN_SIG]	= "Op/Sg"
	};
	int i, col, line = START_LINE;
	struct scan_entry *cur;

	/* Scanning can take several seconds - do not refresh if locked. */
	if (pthread_mutex_trylock(&sr.mutex))
		return;

	if (sr.head || *sr.msg)
		for (i = 1; i <= MAXYLEN; i++)
			mvwclrtoborder(w_aplst, i, 1);

	if (!sr.head)
		waddstr_center(w_aplst, WAV_HEIGHT/2 - 1, sr.msg);

	sort_scan_list(&sr.head);

	/* Truncate overly long access point lists to match screen height. */
	for (cur = sr.head; cur && line < MAXYLEN; line++, cur = cur->next) {
		col = CP_SCAN_NON_AP;

		if (cur->mode == IW_MODE_MASTER)
			col = cur->has_key ? CP_SCAN_CRYPT : CP_SCAN_UNENC;

		wmove(w_aplst, line, 1);
		if (!*cur->essid) {
			sprintf(s, "%-*s ", sr.max_essid_len, "<hidden ESSID>");
			wattron(w_aplst, COLOR_PAIR(col));
			waddstr(w_aplst, s);
		} else if (str_is_ascii(cur->essid)) {
			sprintf(s, "%-*s ", sr.max_essid_len, cur->essid);
			waddstr_b(w_aplst, s);
			wattron(w_aplst, COLOR_PAIR(col));
		} else {
			sprintf(s, "%-*s ", sr.max_essid_len, "<cryptic ESSID>");
			wattron(w_aplst, COLOR_PAIR(col));
			waddstr(w_aplst, s);
		}
		waddstr(w_aplst, ether_addr(&cur->ap_addr));

		wattroff(w_aplst, COLOR_PAIR(col));

		fmt_scan_entry(cur, s, sizeof(s));
		waddstr(w_aplst, " ");
		waddstr(w_aplst, s);
	}

	if (sr.num.entries < MAX_CH_STATS)
		goto done;

	wmove(w_aplst, MAXYLEN, 1);
	wadd_attr_str(w_aplst, A_REVERSE, "total:");
	sprintf(s, " %d ", sr.num.entries);
	waddstr(w_aplst, s);

	sprintf(s, "%s %ssc", sort_type[conf.scan_sort_order], conf.scan_sort_asc ? "a" : "de");
	wadd_attr_str(w_aplst, A_REVERSE, s);

	if (sr.num.entries + START_LINE > line) {
		sprintf(s, ", %d not shown", sr.num.entries + START_LINE - line);
		waddstr(w_aplst, s);
	}
	if (sr.num.open) {
		sprintf(s, ", %d open", sr.num.open);
		waddstr(w_aplst, s);
	}

	if (sr.num.two_gig && sr.num.five_gig) {
		waddch(w_aplst, ' ');
		wadd_attr_str(w_aplst, A_REVERSE, "5/2GHz:");
		sprintf(s, " %d/%d", sr.num.five_gig, sr.num.two_gig);
		waddstr(w_aplst, s);
	}

	if (sr.channel_stats) {
		waddch(w_aplst, ' ');
		if (conf.scan_sort_order == SO_CHAN && !conf.scan_sort_asc)
			sprintf(s, "bottom-%d:", (int)sr.num.ch_stats);
		else
			sprintf(s, "top-%d:", (int)sr.num.ch_stats);
		wadd_attr_str(w_aplst, A_REVERSE, s);

		for (i = 0; i < sr.num.ch_stats; i++) {
			waddstr(w_aplst, i ? ", " : " ");
			sprintf(s, "ch#%d", sr.channel_stats[i].val);
			wadd_attr_str(w_aplst, A_BOLD, s);
			sprintf(s, " (%d)", sr.channel_stats[i].count);
			waddstr(w_aplst, s);
		}
	}
done:
	pthread_mutex_unlock(&sr.mutex);
	wrefresh(w_aplst);
}

void scr_aplst_init(void)
{
	w_aplst = newwin_title(0, WAV_HEIGHT, "Scan window", false);

	/* Gathering scan data can take seconds. Inform user. */
	mvwaddstr(w_aplst, START_LINE, 1, "Waiting for scan data ...");
	wrefresh(w_aplst);

	scan_result_init(&sr);
	pthread_create(&scan_thread, NULL, do_scan, &sr);
}

int scr_aplst_loop(WINDOW *w_menu)
{
	int key;

	display_aplist(w_aplst);

	key = wgetch(w_menu);
	switch (key) {
	case 'a':	/* ascending */
		conf.scan_sort_asc = true;
		return -1;
	case 'c':	/* channel */
		conf.scan_sort_order = SO_CHAN;
		return -1;
	case 'C':	/* channel and signal */
		conf.scan_sort_order = SO_CHAN_SIG;
		return -1;
	case 'd':	/* descending */
		conf.scan_sort_asc = false;
		return -1;
	case 'e':	/* ESSID */
		conf.scan_sort_order = SO_ESSID;
		return -1;
	case 'o':	/* open (descending is default) */
		conf.scan_sort_order = SO_OPEN;
		conf.scan_sort_asc = false;
		return -1;
	case 'O':	/* open and signal (descending) */
		conf.scan_sort_order = SO_OPEN_SIG;
		conf.scan_sort_asc = false;
		return -1;
	case 's':	/* signal */
		conf.scan_sort_order = SO_SIGNAL;
		return -1;
	}
	return key;
}

void scr_aplst_fini(void)
{
	pthread_cancel(scan_thread);
	scan_result_fini(&sr);
	delwin(w_aplst);
}
