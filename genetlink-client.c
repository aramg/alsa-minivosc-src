#include <stdio.h>
#include <netlink/netlink.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <linux/genetlink.h>

#define _DEBUG 1
#define errprint(...) fprintf(stderr, __VA_ARGS__)

#if _DEBUG
# define dbg errprint
#endif

/* commands */
enum {
	DOC_EXMPL_C_UNSPEC,
	DOC_EXMPL_C_ECHO,
	DOC_EXMPL_C_MAX,
};

/* attributes */
enum {
	DOC_EXMPL_A_UNSPEC,
	DOC_EXMPL_A_ECHO,
	DOC_EXMPL_A_MAX,
};

#define FAMILY_NAME "EXMPL_AG"
#define GENL_VERSION 1

// http://alsa.opensrc.org/Asoundrc
// http://stackoverflow.com/questions/3299386/
// https://github.com/tacolin/C_LinuxNetlink
// search [ genl_connect  CTRL_CMD_GETFAMILY language:C size:<10000 followers:0 repos:<50 ]
// https://github.com/sabotage-linux/libnl-tiny
// https://github.com/CogentEmbedded/ltsi-openwrt-cubox (libnl-tiny/src/unl.c)
struct unl_s {
	struct nl_sock *sock;
	char *family_name;
	int   family_id;
	int hdrlen;
};

int main(int argc, char* argv[])
{
	int rc = 0;
	void *hdr = 0;
	struct nl_msg *msg = 0;
	struct unl_s unl = {0};

	unl.sock = nl_socket_alloc();
	if (!unl.sock) {
		errprint("nl_socket_alloc\n");
		goto EARLY_OUT;
	}

	unl.hdrlen = NLMSG_ALIGN(sizeof(struct genlmsghdr));
	unl.family_name = FAMILY_NAME;

	if (genl_connect(unl.sock)) {
		errprint("genl_connect\n");
		goto EARLY_OUT;
	}

	if ((rc = genl_ctrl_resolve(unl.sock, unl.family_name)) < 0) {
		errprint("Unable to resolve family name (genl_ctrl_resolve): %s\n", nl_geterror(rc));
		goto EARLY_OUT;
	}
	unl.family_id = rc;
	dbg("Found family: %s (id=%d).. sending message\n", unl.family_name, unl.family_id);
	msg = nlmsg_alloc();
	if (!msg) {
		errprint("Unable to allocate netlink message\n");
		goto EARLY_OUT;
	}

	hdr = genlmsg_put(
		msg, NL_AUTO_PORT, NL_AUTO_SEQ,
		unl.family_id, 0, 0,
		DOC_EXMPL_C_ECHO, GENL_VERSION);
	if (!hdr) {
		errprint("Unable to write genl header\n");
		goto EARLY_OUT;
	}

	if ((rc = nla_put(msg,  DOC_EXMPL_A_ECHO, 12, "foobarchiki")) < 0) {
		errprint("Unable to add attribute (nla_put_string): %s\n", nl_geterror(rc));
		goto EARLY_OUT;
	}

	if ((rc = nl_send_auto(unl.sock, msg)) < 0) {
		errprint("Unable to send message (nl_send_auto): %s\n", nl_geterror(rc));
		goto EARLY_OUT;
	}


EARLY_OUT:
	if (msg) nlmsg_free(msg);
	if (unl.sock) nl_socket_free(unl.sock);
	return 0;
}
