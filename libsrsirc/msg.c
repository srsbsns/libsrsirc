/* msg.c - protocol message handlers
 * libsrsirc - a lightweight serious IRC lib - (C) 2012-15, Timo Buhrmester
 * See README for contact-, COPYING for license information. */

#define LOG_MODULE MOD_IMSG

#if HAVE_CONFIG_H
# include <config.h>
#endif


#include "msg.h"


#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <logger/intlog.h>

#include "common.h"
#include "conn.h"

#include <libsrsirc/defs.h>
#include <libsrsirc/util.h>


bool
lsi_msg_reghnd(irc *ctx, const char *cmd, hnd_fn hndfn, const char *module)
{
	size_t i = 0;
	D("'%s' registering '%s'-handler", module, cmd);
	for (;i < ctx->msghnds_cnt; i++)
		if (!ctx->msghnds[i].cmd[0])
			break;

	if (i == ctx->msghnds_cnt) {
		size_t ncnt = ctx->msghnds_cnt * 2;
		struct msghnd *narr;
		if (!(narr = malloc(ncnt * sizeof *narr)))
			return false;

		memcpy(narr, ctx->msghnds, ctx->msghnds_cnt * sizeof *narr);
		for (size_t j = ctx->msghnds_cnt; j < ncnt; j++)
			narr[j].cmd[0] = '\0';

		free(ctx->msghnds);

		ctx->msghnds = narr;
		ctx->msghnds_cnt = ncnt;
	}

	ctx->msghnds[i].module = module;
	ctx->msghnds[i].hndfn = hndfn;
	lsi_com_strNcpy(ctx->msghnds[i].cmd, cmd, sizeof ctx->msghnds[i].cmd);
	return true;
}

bool
lsi_msg_reguhnd(irc *ctx, const char *cmd, uhnd_fn hndfn, bool pre)
{
	size_t hcnt = pre ? ctx->uprehnds_cnt : ctx->uposthnds_cnt;
	struct umsghnd *harr = pre ? ctx->uprehnds : ctx->uposthnds;
	size_t i = 0;
	D("user registering %s-'%s'-handler", pre?"pre":"post", cmd);
	for (;i < hcnt; i++)
		if (!harr[i].cmd[0])
			break;

	if (i == hcnt) {
		size_t ncnt = hcnt * 2;
		struct umsghnd *narr;
		if (!(narr = malloc(ncnt * sizeof *narr)))
			return false;

		memcpy(narr, harr, hcnt * sizeof *narr);
		for (size_t j = hcnt; j < ncnt; j++)
			narr[j].cmd[0] = '\0';

		free(harr);

		harr = narr;
		hcnt = ncnt;
		if (pre) {
			ctx->uprehnds = harr;
			ctx->uprehnds_cnt = hcnt;
		} else {
			ctx->uposthnds = harr;
			ctx->uposthnds_cnt = hcnt;
		}
	}

	harr[i].hndfn = hndfn;
	lsi_com_strNcpy(harr[i].cmd, cmd, sizeof harr[i].cmd);
	return true;
}

void
lsi_msg_unregall(irc *ctx, const char *module)
{
	size_t i = 0;
	for (;i < ctx->msghnds_cnt; i++)
		if (ctx->msghnds[i].cmd[0]
		    && strcmp(ctx->msghnds[i].module, module) == 0)
			ctx->msghnds[i].cmd[0] = '\0';
	return;
}

static bool
dispatch_uhnd(irc *ctx, tokarr *msg, size_t ac, bool pre)
{
	size_t hcnt = pre ? ctx->uprehnds_cnt : ctx->uposthnds_cnt;
	struct umsghnd *harr = pre ? ctx->uprehnds : ctx->uposthnds;

	for (size_t i = 0; i < hcnt; i++) {
		if (!harr[i].cmd[0])
			continue;

		if (strcmp((*msg)[1], harr[i].cmd) != 0)
			continue;

		D("dispatch a %s-'%s'", pre?"pre":"post", (*msg)[1]);
		if (!harr[i].hndfn(ctx, msg, ac, pre))
			return false;
	}

	return true;
}

uint8_t
lsi_msg_handle(irc *ctx, tokarr *msg, bool logon)
{
	uint8_t res = 0;
	size_t i = 0;
	size_t ac = 2;
	while (ac < COUNTOF(*msg) && (*msg)[ac])
		ac++;

	if (!logon && !dispatch_uhnd(ctx, msg, ac, true)) {
		res |= USER_ERR;
		goto msg_handle_fail;
	}

	for (;i < ctx->msghnds_cnt; i++) {
		if (!ctx->msghnds[i].cmd[0])
			continue;

		if (strcmp((*msg)[1], ctx->msghnds[i].cmd) != 0)
			continue;

		D("dispatch a '%s' to '%s'", (*msg)[1], ctx->msghnds[i].module);
		res |= ctx->msghnds[i].hndfn(ctx, msg, ac, logon);
		if (res & CANT_PROCEED)
			goto msg_handle_fail;
	}

	if (!logon && !dispatch_uhnd(ctx, msg, ac, false)) {
		res |= USER_ERR;
		goto msg_handle_fail;
	}

	return res;

msg_handle_fail:
	res &= ~CANT_PROCEED;

	if (res & USER_ERR) {
		E("user-registered message handler denied proceeding");
		res |= CANT_PROCEED;
	} else if (res & AUTH_ERR) {
		E("failed to authenticate");
		res |= CANT_PROCEED;
	} else if (res & IO_ERR) {
		E("i/o error");
		res |= CANT_PROCEED;
	} else if (res & ALLOC_ERR) {
		E("memory allocation failed");
		/* we do proceed for now */
	} else if (res & OUT_OF_NICKS) {
		E("out of nicks");
		res |= CANT_PROCEED;
	} else if (res & PROTO_ERR) {
		char line[1024];
		E("proto error on '%s' (ct:%d)",
		    lsi_ut_sndumpmsg(line, sizeof line, NULL, msg),
		    lsi_conn_colon_trail(ctx->con));
		/* we do proceed for now */
	} else {
		E("can't proceed for unknown reasons");
		/* we do proceed for now */
	}

	return res;
}
