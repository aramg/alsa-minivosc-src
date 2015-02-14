#ifndef GENL_TEST_COMMON_H
#define GENL_TEST_COMMON_H

// attributes
enum {
	DC_GENL_ATTR_UNSPEC,
	DC_GENL_ATTR_S16LE_16K_100MS_PCM,
	DC_GENL_ATTR_MAX,
};

// commands
enum {
	DC_GENL_CMD_UNSPEC,
	DC_GENL_CMD_S16LE_16K_100MS_PCM,
	DC_GENL_CMD_MAX,
};

#define DC_GENL_FAMILY_NAME "DROIDCAM_SND"
#define DC_GENL_VERSION 1

#define DC_PCM_CHUNK_DATA_LEN   3200 /* 16kHz 16-bit 100ms */
#define DC_PCM_CHINK_MSG_SIZE   4096 /* rounded up to nearest ^2 */
#define DC_PCM_CHUNK_BYTES      sizeof(struct dc_pcm_chunk_s)

struct dc_pcm_chunk_s {
	unsigned sequence;
	char data[DC_PCM_CHUNK_DATA_LEN];
};

#endif
