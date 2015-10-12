/* irc_ext.c - some more IRC functionality (see also irc.c)
 * libsrsirc - a lightweight serious IRC lib - (C) 2012-15, Timo Buhrmester
 * See README for contact-, COPYING for license information. */

#define LOG_MODULE MOD_IRC

#if HAVE_CONFIG_H
# include <config.h>
#endif


#include "irc_msghnd.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <platform/base_string.h>

#include <logger/intlog.h>

#include "common.h"
#include "conn.h"
#include "irc_track_int.h"
#include "msg.h"

#include <libsrsirc/defs.h>
#include <libsrsirc/util.h>


static uint8_t
handle_001(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 3)
		return PROTO_ERR;

	lsi_ut_freearr(hnd->logonconv[0]);
	hnd->logonconv[0] = lsi_ut_clonearr(msg);
	lsi_com_strNcpy(hnd->mynick, (*msg)[2],sizeof hnd->mynick);
	char *tmp;
	if ((tmp = strchr(hnd->mynick, '@')))
		*tmp = '\0';
	if ((tmp = strchr(hnd->mynick, '!')))
		*tmp = '\0';

	lsi_com_strNcpy(hnd->umodes, DEF_UMODES, sizeof hnd->umodes);
	lsi_com_strNcpy(hnd->cmodes, DEF_CMODES, sizeof hnd->cmodes);
	hnd->ver[0] = '\0';
	hnd->service = false;

	return 0;
}

static uint8_t
handle_002(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	lsi_ut_freearr(hnd->logonconv[1]);
	hnd->logonconv[1] = lsi_ut_clonearr(msg);

	return 0;
}

static uint8_t
handle_003(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	lsi_ut_freearr(hnd->logonconv[2]);
	hnd->logonconv[2] = lsi_ut_clonearr(msg);

	return 0;
}

static uint8_t
handle_004(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 7)
		return PROTO_ERR;

	lsi_ut_freearr(hnd->logonconv[3]);
	hnd->logonconv[3] = lsi_ut_clonearr(msg);
	lsi_com_strNcpy(hnd->myhost, (*msg)[3],sizeof hnd->myhost);
	lsi_com_strNcpy(hnd->umodes, (*msg)[5], sizeof hnd->umodes);
	lsi_com_strNcpy(hnd->cmodes, (*msg)[6], sizeof hnd->cmodes);
	lsi_com_strNcpy(hnd->ver, (*msg)[4], sizeof hnd->ver);

	return LOGON_COMPLETE;
}

static uint8_t
handle_PING(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 3)
		return PROTO_ERR;

	if (!logon)
		return 0;

	char buf[256];
	snprintf(buf, sizeof buf, "PONG :%s\r\n", (*msg)[2]);
	return lsi_conn_write(hnd->con, buf) ? 0 : IO_ERR;
}

static uint8_t
handle_XXX(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	if (!logon)
		return 0;

	if (!hnd->cb_mut_nick)
		return OUT_OF_NICKS;

	hnd->cb_mut_nick(hnd->mynick, sizeof hnd->mynick);

	if (!hnd->mynick[0])
		return OUT_OF_NICKS;

	char buf[MAX_NICK_LEN];
	snprintf(buf, sizeof buf, "NICK %s\r\n", hnd->mynick);
	return lsi_conn_write(hnd->con, buf) ? 0 : IO_ERR;
}

static uint8_t
handle_464(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	W("(%p) wrong server password", (void *)hnd);
	return AUTH_ERR;
}

static uint8_t
handle_383(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 3)
		return PROTO_ERR;

	lsi_com_strNcpy(hnd->mynick, (*msg)[2],sizeof hnd->mynick);
	char *tmp;
	if ((tmp = strchr(hnd->mynick, '@')))
		*tmp = '\0';
	if ((tmp = strchr(hnd->mynick, '!')))
		*tmp = '\0';

	lsi_com_strNcpy(hnd->myhost, (*msg)[0] ? (*msg)[0] : hnd->con->host,
	    sizeof hnd->myhost);

	lsi_com_strNcpy(hnd->umodes, DEF_UMODES, sizeof hnd->umodes);
	lsi_com_strNcpy(hnd->cmodes, DEF_CMODES, sizeof hnd->cmodes);
	hnd->ver[0] = '\0';
	hnd->service = true;
	D("(%p) got beloved 383", (void *)hnd);

	return LOGON_COMPLETE;
}

static uint8_t
handle_484(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	hnd->restricted = true;
	I("(%p) we're 'restricted'", (void *)hnd);
	return 0;
}

static uint8_t
handle_465(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	W("(%p) we're banned", (void *)hnd);
	hnd->banned = true;
	free(hnd->banmsg);
	hnd->banmsg = lsi_b_strdup((*msg)[3] ? (*msg)[3] : "");

	return 0; /* well if we are, the server will sure disconnect us */
}

static uint8_t
handle_466(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	W("(%p) we will be banned", (void *)hnd);

	return 0; /* so what, bitch? */
}

static uint8_t
handle_ERROR(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	free(hnd->lasterr);
	hnd->lasterr = lsi_b_strdup((*msg)[2] ? (*msg)[2] : "");
	W("sever said ERROR: '%s'", (*msg)[2]);
	return 0; /* not strictly a case for CANT_PROCEED */
}

static uint8_t
handle_NICK(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 3)
		return PROTO_ERR;

	char nick[128];
	if (!(*msg)[0])
		return PROTO_ERR;

	lsi_ut_pfx2nick(nick, sizeof nick, (*msg)[0]);

	if (!lsi_ut_istrcmp(nick, hnd->mynick, hnd->casemap)) {
		lsi_com_strNcpy(hnd->mynick, (*msg)[2], sizeof hnd->mynick);
		I("my nick is now '%s'", hnd->mynick);
	}

	return 0;
}

static uint8_t
handle_005_CASEMAPPING(irc *hnd, const char *val)
{
	if (lsi_b_strcasecmp(val, "ascii") == 0)
		hnd->casemap = CMAP_ASCII;
	else if (lsi_b_strcasecmp(val, "strict-rfc1459") == 0)
		hnd->casemap = CMAP_STRICT_RFC1459;
	else {
		if (lsi_b_strcasecmp(val, "rfc1459") != 0)
			W("unknown 005 casemapping: '%s'", val);
		hnd->casemap = CMAP_RFC1459;
	}

	return 0;
}

/* XXX not robust enough */
static uint8_t
handle_005_PREFIX(irc *hnd, const char *val)
{
	char str[32];
	if (!val[0])
		return PROTO_ERR;

	lsi_com_strNcpy(str, val + 1, sizeof str);
	char *p = strchr(str, ')');
	if (!p)
		return PROTO_ERR;

	*p++ = '\0';

	size_t slen = strlen(str);
	if (slen == 0 || slen != strlen(p))
		return PROTO_ERR;

	lsi_com_strNcpy(hnd->m005modepfx[0], str, MAX_005_MDPFX);
	lsi_com_strNcpy(hnd->m005modepfx[1], p, MAX_005_MDPFX);

	return 0;
}

static uint8_t
handle_005_CHANMODES(irc *hnd, const char *val)
{
	for (int z = 0; z < 4; ++z)
		hnd->m005chanmodes[z][0] = '\0';

	int c = 0;
	char argbuf[64];
	lsi_com_strNcpy(argbuf, val, sizeof argbuf);
	char *ptr = strtok(argbuf, ",");

	while (ptr) {
		if (c < 4)
			lsi_com_strNcpy(hnd->m005chanmodes[c++], ptr,
			    MAX_005_CHMD);
		ptr = strtok(NULL, ",");
	}

	if (c != 4)
		W("005 chanmodes: expected 4 params, got %i. arg: \"%s\"",
		    c, val);

	return 0;
}

static uint8_t
handle_005_CHANTYPES(irc *hnd, const char *val)
{
	lsi_com_strNcpy(hnd->m005chantypes, val, MAX_005_CHTYP);
	return 0;
}

static uint8_t
handle_005(irc *hnd, tokarr *msg, size_t nargs, bool logon)
{
	uint8_t ret = 0;
	bool have_casemap = false;

	for (size_t z = 3; z < nargs; ++z) {
		if (lsi_b_strncasecmp((*msg)[z], "CASEMAPPING=", 12) == 0) {
			ret |= handle_005_CASEMAPPING(hnd, (*msg)[z] + 12);
			have_casemap = true;
		} else if (lsi_b_strncasecmp((*msg)[z], "PREFIX=", 7) == 0)
			ret |= handle_005_PREFIX(hnd, (*msg)[z] + 7);
		else if (lsi_b_strncasecmp((*msg)[z], "CHANMODES=", 10) == 0)
			ret |= handle_005_CHANMODES(hnd, (*msg)[z] + 10);
		else if (lsi_b_strncasecmp((*msg)[z], "CHANTYPES=", 10) == 0)
			ret |= handle_005_CHANTYPES(hnd, (*msg)[z] + 10);

		if (ret & CANT_PROCEED)
			return ret;
	}

	if (hnd->tracking && !hnd->tracking_enab && have_casemap) {
		if (!lsi_trk_init(hnd))
			E("failed to enable tracking");
		else {
			hnd->tracking_enab = true;
			N("tracking enabled");
		}
	}

	return ret;
}


bool
lsi_imh_regall(irc *hnd, bool dumb)
{
	bool fail = false;
	if (!dumb) {
		fail = fail || !lsi_msg_reghnd(hnd, "PING", handle_PING, "irc");
		fail = fail || !lsi_msg_reghnd(hnd, "432", handle_XXX, "irc");
		fail = fail || !lsi_msg_reghnd(hnd, "433", handle_XXX, "irc");
		fail = fail || !lsi_msg_reghnd(hnd, "436", handle_XXX, "irc");
		fail = fail || !lsi_msg_reghnd(hnd, "437", handle_XXX, "irc");
		fail = fail || !lsi_msg_reghnd(hnd, "464", handle_464, "irc");
	}

	fail = fail || !lsi_msg_reghnd(hnd, "NICK", handle_NICK, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "ERROR", handle_ERROR, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "001", handle_001, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "002", handle_002, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "003", handle_003, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "004", handle_004, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "383", handle_383, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "484", handle_484, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "465", handle_465, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "466", handle_466, "irc");
	fail = fail || !lsi_msg_reghnd(hnd, "005", handle_005, "irc");

	return !fail;
}

void
lsi_imh_unregall(irc *hnd)
{
	lsi_msg_unregall(hnd, "irc");
}
