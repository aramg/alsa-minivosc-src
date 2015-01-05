#include <stdio.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
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

#define FAMILY_NAME "DOC_EXMPL"

// http://alsa.opensrc.org/Asoundrc
// http://stackoverflow.com/questions/3299386/
// https://github.com/tacolin/C_LinuxNetlink
// search [ genl_connect  CTRL_CMD_GETFAMILY language:C size:<10000 followers:0 repos:<50 ]
// https://github.com/sabotage-linux/libnl-tiny
// https://github.com/CogentEmbedded/ltsi-openwrt-cubox (libnl-tiny/src/unl.c)
struct unl_s {
	struct nl_sock *sock;
	struct nl_cache *cache;
	struct genl_family *family;
	char *family_name;
	int hdrlen;
};

int main(int argc, char* argv[])
{
	int rc;
	void *hdr;
	struct unl_s unl = {0};

	unl.sock = nl_socket_alloc();
	if (!unl.sock) {
		nl_perror("nl_socket_alloc");
		goto EARLY_OUT;
	}

	unl.hdrlen = NLMSG_ALIGN(sizeof(struct genlmsghdr));
	unl.family_name = FAMILY_NAME;

	if (genl_connect(unl.sock)) {
		nl_perror("genl_connect");
		goto EARLY_OUT;
	}

	if (genl_ctrl_alloc_cache(unl.sock, &unl.cache)) {
		nl_perror("genl_connect");
		goto EARLY_OUT;
	}

	unl->family = genl_ctrl_search_by_name(unl->cache, unl.family_name);
	if (!unl->family) {
		errprint("error: genl_ctrl_search_by_name(family='%s') failure\n", unl.family_name);
		goto EARLY_OUT;
	}


	if (unl->family_name) free(unl->family_name);
	if (unl->sock) nl_socket_free(unl->sock);
	if (unl->cache) nl_cache_free(unl->cache);

EARLY_OUT:
	return 0;
}
