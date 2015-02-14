#include <netlink/netlink.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <linux/genetlink.h>
#include <stdio.h>
#include <unistd.h>

#include "genetlink-common.h"

#define _DEBUG 1
#define errprint(...) fprintf(stderr, __VA_ARGS__)

#if _DEBUG
# define dbg errprint
#endif

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
	struct nl_msg *msg = 0;
	struct unl_s unl = {0};
	struct dc_pcm_chunk_s pcm_chunk;
	FILE * fp;

	if (argc != 2) {
		errprint("Usage: %s <audio.pcm>\n", argv[0]);
		goto EARLY_OUT;
	}

	fp = fopen(argv[1], "r");
	if (!fp) {
		errprint("Error opening %s\n", argv[1]);
		goto EARLY_OUT;
	}

	unl.sock = nl_socket_alloc();
	if (!unl.sock) {
		errprint("nl_socket_alloc\n");
		goto EARLY_OUT;
	}

	unl.hdrlen = NLMSG_ALIGN(sizeof(struct genlmsghdr));
	unl.family_name = DC_GENL_FAMILY_NAME;

	if (genl_connect(unl.sock)) {
		errprint("genl_connect\n");
		goto EARLY_OUT;
	}

	if ((rc = genl_ctrl_resolve(unl.sock, unl.family_name)) < 0) {
		errprint("Unable to resolve family name (genl_ctrl_resolve): %s\n", nl_geterror(rc));
		goto EARLY_OUT;
	}
	unl.family_id = rc;
	dbg("Found family: %s (id=%d).. sending pcm chunks..\n", unl.family_name, unl.family_id);

	nlmsg_set_default_size(DC_PCM_CHINK_MSG_SIZE);
	while (!feof(fp)) {
		msg = nlmsg_alloc();
		if (!msg) {
			errprint("Unable to allocate netlink message\n");
			goto EARLY_OUT;
		}

		if (!genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, unl.family_id, 0, 0, DC_GENL_CMD_S16LE_16K_100MS_PCM, DC_GENL_VERSION)) {
			errprint("Unable to write genl header\n");
			goto EARLY_OUT;
		}

		pcm_chunk.sequence ++;
		memset(pcm_chunk.data, 0, DC_PCM_CHUNK_DATA_LEN);
		fread(pcm_chunk.data, 1, DC_PCM_CHUNK_DATA_LEN, fp);
		errprint("Writing sequence %d [ %x %X %x ... %x %x %x]\n", pcm_chunk.sequence, \
			pcm_chunk.data[0] & 0xff,\
			pcm_chunk.data[1] & 0xff,\
			pcm_chunk.data[2] & 0xff,\
			pcm_chunk.data[DC_PCM_CHUNK_DATA_LEN -3]&0xff,\
			pcm_chunk.data[DC_PCM_CHUNK_DATA_LEN -2]&0xff,\
			pcm_chunk.data[DC_PCM_CHUNK_DATA_LEN -1]&0xff);
		if ((rc = nla_put(msg,  DC_GENL_ATTR_S16LE_16K_100MS_PCM, DC_PCM_CHUNK_BYTES, &pcm_chunk)) < 0) {
			errprint("Unable to add attribute (nla_put): %s\n", nl_geterror(rc));
			goto EARLY_OUT;
		}

		if ((rc = nl_send_auto(unl.sock, msg)) < 0) {
			errprint("Unable to send message (nl_send_auto): %s\n", nl_geterror(rc));
			goto EARLY_OUT;
		}

		nlmsg_free(msg);
		msg = NULL;
		usleep(100000); // 100 ms?
	}

EARLY_OUT:
	if (msg) nlmsg_free(msg);
	if (unl.sock) nl_socket_free(unl.sock);
	return 0;
}
