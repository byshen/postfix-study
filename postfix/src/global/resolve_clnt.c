/*++
/* NAME
/*	resolve_clnt 3
/* SUMMARY
/*	address resolve service client (internal forms)
/* SYNOPSIS
/*	#include <resolve_clnt.h>
/*
/*	typedef struct {
/* .in +4
/*		VSTRING *transport;
/*		VSTRING *nexthop
/*		VSTRING *recipient;
/*		int	flags;
/* .in -4
/*	} RESOLVE_REPLY;
/*
/*	void	resolve_clnt_init(reply)
/*	RESOLVE_REPLY *reply;
/*
/*	void	resolve_clnt_query(address, reply)
/*	const char *address
/*	RESOLVE_REPLY *reply;
/*
/*	void	resolve_clnt_free(reply)
/*	RESOLVE_REPLY *reply;
/* DESCRIPTION
/*	This module implements a mail address resolver client.
/*
/*	resolve_clnt_init() initializes a reply data structure for use
/*	by resolve_clnt_query(). The structure is destroyed by passing
/*	it to resolve_clnt_free().
/*
/*	resolve_clnt_query() sends an internal-form recipient address
/*	(user@domain) to the resolver daemon and returns the resulting
/*	transport name, next_hop host name, and internal-form recipient
/*	address. In case of communication failure the program keeps trying
/*	until the mail system goes down.
/*
/*	In the resolver reply, the flags member is the bit-wise OR of
/*	zero or more of the following:
/* .IP RESOLVE_FLAG_FINAL
/*	The recipient address resolves to a mail transport that performs
/*	final delivery. The destination is local or corresponds to a hosted
/*	domain that is handled by the local machine. This flag is currently
/*	not used.
/* .IP RESOLVE_FLAG_ROUTED
/*	After address resolution the recipient localpart contains further
/*	routing information, so the resolved next-hop destination is not
/*	the final destination.
/* .IP RESOLVE_FLAG_ERROR
/*	The address resolved to something that has invalid syntax.
/* .IP RESOLVE_FLAG_FAIL
/*	The request could not be completed.
/* .PP
/*	In addition, the address domain class is returned by setting
/*	one of the following flags (this is preliminary code awaiting
/*	more permanent implementation of address domain class handling):
/* .IP RESOLVE_CLASS_LOCAL
/*	The address domain matches $mydestination or $inet_interfaces.
/* .IP RESOLVE_CLASS_ALIAS
/*	The address domain matches $virtual_alias_domains (simulated
/*	virtual domains, where each address is redirected to a real
/*	local or remote address).
/* .IP RESOLVE_CLASS_VIRTUAL
/*	The address domain matches $virtual_mailbox_domains (true
/*	virtual domains where each address can have its own mailbox).
/* .IP RESOLVE_CLASS_RELAY
/*	The address domain matches $relay_domains, i.e. this is an
/*	authorized mail relay destination.
/* .IP RESOLVE_CLASS_DEFAULT
/*	The address matches none of the above. Access to this domain
/*	should be limited to authorized senders only.
/* .PP
/*	For convenience, the constant RESOLVE_CLASS_FINAL includes all
/*	cases where the local machine is the final destination.
/* DIAGNOSTICS
/*	Warnings: communication failure. Fatal error: mail system is down.
/* SEE ALSO
/*	mail_proto(3h) low-level mail component glue.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* Utility library. */

#include <msg.h>
#include <vstream.h>
#include <vstring.h>
#include <vstring_vstream.h>
#include <events.h>
#include <iostuff.h>

/* Global library. */

#include "mail_proto.h"
#include "mail_params.h"
#include "clnt_stream.h"
#include "resolve_clnt.h"

/* Application-specific. */

 /*
  * XXX this is shared with the rewrite client to save a file descriptor.
  */
extern CLNT_STREAM *rewrite_clnt_stream;

static VSTRING *last_addr;
static RESOLVE_REPLY last_reply;

/* resolve_clnt_init - initialize reply */

void    resolve_clnt_init(RESOLVE_REPLY *reply)
{
    reply->transport = vstring_alloc(100);
    reply->nexthop = vstring_alloc(100);
    reply->recipient = vstring_alloc(100);
    reply->flags = 0;
}

/* resolve_clnt_query - resolve address to (transport, next hop, recipient) */

void    resolve_clnt_query(const char *addr, RESOLVE_REPLY *reply)
{
    char   *myname = "resolve_clnt_query";
    VSTREAM *stream;

    /*
     * One-entry cache.
     */
    if (last_addr == 0) {
	last_addr = vstring_alloc(100);
	resolve_clnt_init(&last_reply);
    }

    /*
     * Sanity check. The result must not clobber the input because we may
     * have to retransmit the request.
     */
#define STR vstring_str

    if (addr == STR(reply->recipient))
	msg_panic("%s: result clobbers input", myname);

    /*
     * Peek at the cache.
     */
    if (*addr && strcmp(addr, STR(last_addr)) == 0) {
	vstring_strcpy(reply->transport, STR(last_reply.transport));
	vstring_strcpy(reply->nexthop, STR(last_reply.nexthop));
	vstring_strcpy(reply->recipient, STR(last_reply.recipient));
	reply->flags = last_reply.flags;
	if (msg_verbose)
	    msg_info("%s: cached: `%s' -> t=`%s' h=`%s' r=`%s'",
		     myname, addr, STR(reply->transport),
		     STR(reply->nexthop), STR(reply->recipient));
	return;
    }

    /*
     * Keep trying until we get a complete response. The resolve service is
     * CPU bound; making the client asynchronous would just complicate the
     * code.
     */
    if (rewrite_clnt_stream == 0)
	rewrite_clnt_stream = clnt_stream_create(MAIL_CLASS_PRIVATE,
						 var_rewrite_service,
						 var_ipc_idle_limit,
						 var_ipc_ttl_limit);

    for (;;) {
	stream = clnt_stream_access(rewrite_clnt_stream);
	if (attr_print(stream, ATTR_FLAG_NONE,
		       ATTR_TYPE_STR, MAIL_ATTR_REQ, RESOLVE_ADDR,
		       ATTR_TYPE_STR, MAIL_ATTR_ADDR, addr,
		       ATTR_TYPE_END)
	    || vstream_fflush(stream)) {
	    if (msg_verbose || (errno != EPIPE && errno != ENOENT))
		msg_warn("%s: bad write: %m", myname);
	} else if (attr_scan(stream, ATTR_FLAG_STRICT,
		       ATTR_TYPE_STR, MAIL_ATTR_TRANSPORT, reply->transport,
			   ATTR_TYPE_STR, MAIL_ATTR_NEXTHOP, reply->nexthop,

			   ATTR_TYPE_STR, MAIL_ATTR_RECIP, reply->recipient,
			     ATTR_TYPE_NUM, MAIL_ATTR_FLAGS, &reply->flags,
			     ATTR_TYPE_END) != 4) {
	    if (msg_verbose || (errno != EPIPE && errno != ENOENT))
		msg_warn("%s: bad read: %m", myname);
	} else {
	    if (msg_verbose)
		msg_info("%s: `%s' -> t=`%s' h=`%s' r=`%s'",
			 myname, addr, STR(reply->transport),
			 STR(reply->nexthop), STR(reply->recipient));
	    if (STR(reply->transport)[0] == 0)
		msg_warn("%s: null transport result for: <%s>", myname, addr);
	    else if (STR(reply->recipient)[0] == 0 && *addr != 0)
		msg_warn("%s: null recipient result for: <%s>", myname, addr);
	    else
		break;
	}
	sleep(10);				/* XXX make configurable */
	clnt_stream_recover(rewrite_clnt_stream);
    }

    /*
     * Update the cache.
     */
    vstring_strcpy(last_addr, addr);
    vstring_strcpy(last_reply.transport, STR(reply->transport));
    vstring_strcpy(last_reply.nexthop, STR(reply->nexthop));
    vstring_strcpy(last_reply.recipient, STR(reply->recipient));
    last_reply.flags = reply->flags;
}

/* resolve_clnt_free - destroy reply */

void    resolve_clnt_free(RESOLVE_REPLY *reply)
{
    reply->transport = vstring_free(reply->transport);
    reply->nexthop = vstring_free(reply->nexthop);
    reply->recipient = vstring_free(reply->recipient);
}

#ifdef TEST

#include <stdlib.h>
#include <msg_vstream.h>
#include <vstring_vstream.h>
#include <mail_conf.h>

static NORETURN usage(char *myname)
{
    msg_fatal("usage: %s [-v] [address...]", myname);
}

static void resolve(char *addr, RESOLVE_REPLY *reply)
{
    struct RESOLVE_FLAG_TABLE {
	int     flag;
	const char *name;
    };
    struct RESOLVE_FLAG_TABLE resolve_flag_table[] = {
	RESOLVE_FLAG_FINAL, "FLAG_FINAL",
	RESOLVE_FLAG_ROUTED, "FLAG_ROUTED",
	RESOLVE_FLAG_ERROR, "FLAG_ERROR",
	RESOLVE_FLAG_FAIL, "FLAG_FAIL",
	RESOLVE_CLASS_LOCAL, "CLASS_LOCAL",
	RESOLVE_CLASS_ALIAS, "CLASS_ALIAS",
	RESOLVE_CLASS_VIRTUAL, "CLASS_VIRTUAL",
	RESOLVE_CLASS_RELAY, "CLASS_RELAY",
	RESOLVE_CLASS_DEFAULT, "CLASS_DEFAULT",
	0,
    };
    struct RESOLVE_FLAG_TABLE *fp;

    resolve_clnt_query(addr, reply);
    if (reply->flags & RESOLVE_FLAG_FAIL) {
	vstream_printf("request failed\n");
    } else {
	vstream_printf("%-10s %s\n", "address", addr);
	vstream_printf("%-10s %s\n", "transport", STR(reply->transport));
	vstream_printf("%-10s %s\n", "nexthop", *STR(reply->nexthop) ?
		       STR(reply->nexthop) : "[none]");
	vstream_printf("%-10s %s\n", "recipient", STR(reply->recipient));
	vstream_printf("%-10s ", "flags");
	for (fp = resolve_flag_table; fp->name; fp++) {
	    if (reply->flags & fp->flag) {
		vstream_printf("%s ", fp->name);
		reply->flags &= ~fp->flag;
	    }
	}
	if (reply->flags != 0)
	    vstream_printf("Unknown flag 0x%x", reply->flags);
	vstream_printf("\n");
	vstream_fflush(VSTREAM_OUT);
    }
}

int     main(int argc, char **argv)
{
    RESOLVE_REPLY reply;
    int     ch;

    msg_vstream_init(argv[0], VSTREAM_ERR);

    mail_conf_read();
    msg_info("using config files in %s", var_config_dir);
    if (chdir(var_queue_dir) < 0)
	msg_fatal("chdir %s: %m", var_queue_dir);

    while ((ch = GETOPT(argc, argv, "v")) > 0) {
	switch (ch) {
	case 'v':
	    msg_verbose++;
	    break;
	default:
	    usage(argv[0]);
	}
    }
    resolve_clnt_init(&reply);

    if (argc > optind) {
	while (argv[optind]) {
	    resolve(argv[optind], &reply);
	    optind++;
	}
    } else {
	VSTRING *buffer = vstring_alloc(1);

	while (vstring_fgets_nonl(buffer, VSTREAM_IN)) {
	    resolve(STR(buffer), &reply);
	}
	vstring_free(buffer);
    }
    resolve_clnt_free(&reply);
    exit(0);
}

#endif
