/*
 *  Minimal virtual oscillator (minivosc) soundcard
 *
 *  Based on Loopback soundcard (aloop-kernel.c):
 *  Original code:
 *  Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 *
 *  More accurate positioning and full-duplex support:
 *  Copyright (c) Ahmet İnan <ainan at mathematik.uni-freiburg.de>
 *
 *  Major (almost complete) rewrite:
 *  Copyright (c) by Takashi Iwai <tiwai@suse.de>
 *
 *  with snippets from Ben Collins: Writing an ALSA driver
 *  http://ben-collins.blogspot.com/2010/04/writing-alsa-driver.html
 *
 *  minivosc specific parts:
 *  Copyright (c) by Smilen Dimitrov <sd at imi.aau.dk>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

/* Use our own dbg macro http://www.n1ywb.com/projects/darts/darts-usb/darts-usb.c*/
#undef dbg
#define _DEBUG 1

#if _DEBUG
//# define dbg(format, arg...) printk(KERN_DEBUG __FILE__ ": " format "\n" , ## arg)
# define dbg dbg2
# define dbg2(format, arg...) printk( ": " format "\n" , ## arg)
#else
# define dbg(format, arg...) /* */
# define dbg2(format, arg...) /* */
#endif

/* Here is our user defined breakpoint to */
/* initiate communication with remote (k)gdb */
/* don't use if not actually using kgdb */
#define BREAKPOINT() /* asm("   int $3") */


// copy from aloop-kernel.c:
#include <linux/init.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <linux/version.h>
#include <net/genetlink.h>

#include "genetlink-common.h"

MODULE_AUTHOR("sdaau, dev47apps");
MODULE_DESCRIPTION("droidcam virtual mic");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{ALSA,minivosc soundcard}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = {1, [1 ... (SNDRV_CARDS - 1)] = 0};

static struct platform_device *devices[SNDRV_CARDS];

#define byte_pos(x) ((x) / HZ)
#define frac_pos(x) ((x) * HZ)

#define PERIODS_MAX    16
#define PERIOD_BYTES 1600 /* 50ms @16KHz or 100ms @8KHZ */
#define MAX_BUFFER (PERIODS_MAX * PERIOD_BYTES)

static struct snd_pcm_hardware minivosc_pcm_hw =
{
	.info = ( SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER ),
	.formats          = SNDRV_PCM_FMTBIT_U16,
	.rates            = SNDRV_PCM_RATE_8000|SNDRV_PCM_RATE_16000,
	.rate_min         = 8000,  /* 16kBps */
	.rate_max         = 16000, /* 32kBps */
	.channels_min     = 1,
	.channels_max     = 1,
	.buffer_bytes_max = MAX_BUFFER,
	.period_bytes_min = PERIOD_BYTES,
	.period_bytes_max = PERIOD_BYTES,
	.periods_min      = 1,
	.periods_max      = PERIODS_MAX,
};


struct minivosc_device
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	const struct minivosc_pcm_ops *timer_ops;
	/*
	* we have only one substream, so all data in this struct
	*/
	/* copied from struct loopback: */
	struct mutex cable_lock;
	/* copied from struct loopback_cable: */
	/* PCM parameters */
	unsigned int pcm_period_size;
	unsigned int pcm_bps;		/* bytes per second */
	/* flags */
	unsigned int valid;
	unsigned int running;
	/* timer stuff */
	unsigned int irq_pos;		/* fractional IRQ position */
	unsigned int period_size_frac;
	unsigned long last_jiffies;
	struct timer_list timer;
	/* copied from struct loopback_pcm: */
	struct snd_pcm_substream *substream;
	unsigned int pcm_buffer_size;
	unsigned int buf_pos;	/* position in buffer */
	unsigned int silent_size;
	/* added for waveform: */
	unsigned int wvf_pos;	/* position in waveform array */
	unsigned int wvf_lift;	/* lift of waveform array */
};

#define SND_MINIVOSC_DRIVER    "snd_droidcam"


#define CABLE_PLAYBACK	(1 << SNDRV_PCM_STREAM_PLAYBACK)
#define CABLE_CAPTURE	(1 << SNDRV_PCM_STREAM_CAPTURE)
#define CABLE_BOTH	(CABLE_PLAYBACK | CABLE_CAPTURE)

static char wvfdat[]={	20, 22, 24, 25, 24, 22, 21,
			19, 17, 15, 14, 15, 17, 19,
			20, 127, 22, 19, 17, 15, 19};
static char wvfdat2[]={	20, 22, 24, 25, 24, 22, 21,
			19, 17, 15, 14, 15, 17, 19,
			20, 127, 22, 19, 17, 15, 19};

static unsigned int wvfsz=sizeof(wvfdat);//*sizeof(float) is included already

// * functions for driver/kernel module initialization
static void minivosc_unregister_all(void);
static int __init alsa_card_minivosc_init(void);
static void __exit alsa_card_minivosc_exit(void);

// * declare functions for this struct describing the driver (to be defined later):
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
static int __devinit minivosc_probe(struct platform_device *devptr);
static int __devexit minivosc_remove(struct platform_device *devptr);
#else
static int minivosc_probe(struct platform_device *devptr);
static int minivosc_remove(struct platform_device *devptr);
#endif


// * here declaration of functions that will need to be in _ops, before they are defined
static int minivosc_hw_params(struct snd_pcm_substream *ss,
                        struct snd_pcm_hw_params *hw_params);
static int minivosc_hw_free(struct snd_pcm_substream *ss);
static int minivosc_pcm_open(struct snd_pcm_substream *ss);
static int minivosc_pcm_close(struct snd_pcm_substream *ss);
static int minivosc_pcm_prepare(struct snd_pcm_substream *ss);
static int minivosc_pcm_trigger(struct snd_pcm_substream *ss,
                          int cmd);
static snd_pcm_uframes_t minivosc_pcm_pointer(struct snd_pcm_substream *ss);

static int minivosc_pcm_dev_free(struct snd_device *device);
static int minivosc_pcm_free(struct minivosc_device *chip);

// * declare timer functions - copied from aloop-kernel.c
static void minivosc_timer_start(struct minivosc_device *mydev);
static void minivosc_timer_stop(struct minivosc_device *mydev);
static void minivosc_timer_function(unsigned long data);
static void minivosc_fill_capture_buf(struct minivosc_device *mydev, unsigned int bytes);


// note snd_pcm_ops can usually be separate _playback_ops and _capture_ops
static struct snd_pcm_ops minivosc_pcm_ops =
{
	.open      = minivosc_pcm_open,
	.close     = minivosc_pcm_close,
	.ioctl     = snd_pcm_lib_ioctl,
	.hw_params = minivosc_hw_params,
	.hw_free   = minivosc_hw_free,
	.prepare   = minivosc_pcm_prepare,
	.trigger   = minivosc_pcm_trigger,
	.pointer   = minivosc_pcm_pointer,
};

// specifies what func is called @ snd_card_free
// used in snd_device_new
static struct snd_device_ops dev_ops =
{
	.dev_free = minivosc_pcm_dev_free,
};


// * we need a struct describing the driver:
static struct platform_driver minivosc_driver =
{
	.probe		= minivosc_probe,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
	.remove		= __devexit_p(minivosc_remove),
#else
	.remove		= minivosc_remove,
#endif
//~ #ifdef CONFIG_PM
	//~ .suspend	= minivosc_suspend,
	//~ .resume	= minivosc_resume,
//~ #endif
	.driver = {
		.name	= SND_MINIVOSC_DRIVER,
		.owner = THIS_MODULE
	},
};


/*
 * Netlink related code for DroidCam
 * References:
 * http://lwn.net/Articles/208755/
 * http://www.linuxfoundation.org/collaborate/workgroups/networking/generic_netlink_howto
 */

// attribute policies
static struct nla_policy dc_genl_policy[DC_GENL_ATTR_MAX] = {
	[DC_GENL_ATTR_S16LE_8K_100MS_PCM] = { .type = NLA_BINARY },
};

// family definition
static struct genl_family dc_genl_family = {
	.id = GENL_ID_GENERATE,
	.hdrsize = 0,
	.name    = DC_GENL_FAMILY_NAME,
	.version = DC_GENL_VERSION,
	.maxattr = ( DC_GENL_ATTR_MAX - 1 ),
};

static int dc_genl_s16le_8k_100ms_pcm_handler(struct sk_buff *skb, struct genl_info *info);

struct genl_ops dc_genl_ops[] = {
 {
	.cmd = DC_GENL_CMD_S16LE_8K_100MS_PCM,
	.flags = 0,
	.policy = dc_genl_policy,
	.doit = dc_genl_s16le_8k_100ms_pcm_handler,
	.dumpit = NULL,
 },
};

static int is_genl_family_registered = 0;

static int dc_netlink_init(void)
{
	int rc;
	is_genl_family_registered = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0)
#error Kernels below v3.13 not tested yet

	rc = genl_register_family(&dc_genl_family);
	if (rc != 0) {
		dbg("%s: error: genl_register_family() returned %d", __func__, rc);
		goto EARLY_OUT;
	}

	rc = genl_register_ops(&dc_genl_family, &dc_genl_ops[0]);
#else

	rc = genl_register_family_with_ops(&dc_genl_family, dc_genl_ops);
#endif
	if (rc != 0) {
		dbg("%s: error: genl_register_ops() returned %d", __func__, rc);
		goto EARLY_OUT;
	}

	is_genl_family_registered = 1;

EARLY_OUT:
	return rc;
}

static void dc_netlink_fini(void)
{
	int rc;
	if (is_genl_family_registered == 0) return;
	is_genl_family_registered = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0)
#error Kernels below v3.13 not tested yet
	rc = genl_unregister_ops(&dc_genl_family, &dc_genl_ops[0]);
	if (rc != 0) {
		dbg("%s: error: genl_UNregister_ops() returned %d", __func__, rc);
	}
#endif

	rc = genl_unregister_family(&dc_genl_family);
	if (rc != 0) {
		dbg("%s: error: genl_UNregister_family() returned %d", __func__, rc);
	}
}

static int _parseMsgFromUseSpace(struct genl_info *pInfo)
{
	//struct nlattr *pAttr1 = NULL;
	//uint32 data1;
	struct nlattr *pAttr2 = NULL;
	char* pData2;

	dbg("%s()", __func__);

	// pAttr1 = pInfo->attrs[?];
	// if (pAttr1){
	// 	data1 = *(uint32*)nla_data(pAttr1);
	// 	dbg("[KERNEL-PART] (int) data1 = %d\n", data1);
	// }

	pAttr2 = pInfo->attrs[DC_GENL_ATTR_S16LE_8K_100MS_PCM];
	if (pAttr2) {
		pData2 = (char*) nla_data(pAttr2);
		dbg("[KERNEL-PART] (char*) data2 = %.*s", nla_len(pAttr2), pData2);
	}

	return 0;
}

#if 0
static int _sendMsgToUserSpace(struct genl_info *pInfo)
{
	struct sk_buff *pSendbackSkb = NULL;
	void *pMsgHead = NULL;
	int rc;

	dbg("%s()\n", __func__);

	// NLMSG_GOODSIZE = PAGE_SIZE - headers = ~4k
	pSendbackSkb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!pSendbackSkb) {
		dbg("%s: error: genlmsg_new() failure", __func__);
		return -1;
	}

	pMsgHead = genlmsg_put(pSendbackSkb, 0, pInfo->snd_seq+1, &doc_exmpl_gnl_family, 0, DOC_EXMPL_C_ECHO);
	if (!pMsgHead) {
		dbg("%s: error: genlmsg_put() failure", __func__);
		return -1;
	}

	// rc = nla_put_u32(pSendbackSkb, TACO_ATTRIBUTE_1_INTEGER, 9999);
	// if (rc != 0){
	// 	dbg("%s: error: nla_put_u32() failure", __func__);
	// 	return -1;
	// }

	rc = nla_put_string(pSendbackSkb, DOC_EXMPL_A_ECHO, "Hello From Kernel! Wazzuup :P");
	if (rc != 0){
		dbg("%s: error: nla_put_string() failure", __func__);
		return -1;
	}

	genlmsg_end(pSendbackSkb, pMsgHead);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
	rc = genlmsg_unicast(&init_net, pSendbackSkb, pInfo->snd_pid);
#else
	rc = genlmsg_unicast(&init_net, pSendbackSkb, pInfo->snd_portid);
#endif
	if (rc != 0) {
		dbg("%s: error: genlmsg_unicast() failure", __func__);
		return -1;
	}

	return 0;
}
#endif

static int dc_genl_s16le_8k_100ms_pcm_handler(struct sk_buff *skb, struct genl_info *info)
{
	/* message handling code goes here; return 0 on success, negative values on failure */
	dbg("%s()\n", __func__);
	if (skb == NULL || info == NULL){
		dbg("%s: error: NULL at input. skb=%p, info=%p", __func__, skb, info);
		return -1;
	}

	if ( 0 != _parseMsgFromUseSpace(info) )
		return -1;

	// if ( 0 != _sendMsgToUserSpace(info) )
	// 	return -1;

	return 0;
}

// -- end netlink code
//
/*
 *
 * Probe/remove functions
 *
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
static int __devinit minivosc_probe(struct platform_device *devptr)
#else
static int minivosc_probe(struct platform_device *devptr)
#endif
{

	struct snd_card *card;
	struct minivosc_device *mydev;
	int ret;

	int nr_subdevs; // how many capture substreams we want
	struct snd_pcm *pcm;

	int dev = devptr->id; // from aloop-kernel.c

	dbg("%s: probe", __func__);


	// no need to kzalloc minivosc_device separately, if the sizeof is included here
	ret = snd_card_create(index[dev], id[dev],
	                      THIS_MODULE, sizeof(struct minivosc_device), &card);

	if (ret < 0)
		goto __nodev;

	mydev = card->private_data;
	mydev->card = card;
	// MUST have mutex_init here - else crash on mutex_lock!!
	mutex_init(&mydev->cable_lock);

	dbg2("-- mydev %p", mydev);

	sprintf(card->driver, SND_MINIVOSC_DRIVER);
	sprintf(card->shortname, "DroidCam-Mic");
	sprintf(card->longname, "DroidCam Virtual Mic");

	snd_card_set_dev(card, &devptr->dev); // present in dummy, not in aloop though

	ret = snd_device_new(card, SNDRV_DEV_LOWLEVEL, mydev, &dev_ops);

	if (ret < 0)
		goto __nodev;


	nr_subdevs = 1; // how many capture substreams we want
	// * we want 0 playback, and 1 capture substreams (4th and 5th arg) ..
	ret = snd_pcm_new(card, card->driver, 0, 0, nr_subdevs, &pcm);

	if (ret < 0)
		goto __nodev;


	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &minivosc_pcm_ops); // in both aloop-kernel.c and dummy.c, after snd_pcm_new...
	pcm->private_data = mydev; //here it should be dev/card struct (the one containing struct snd_card *card) - this DOES NOT end up in substream->private_data

	pcm->info_flags = 0;
	strcpy(pcm->name, card->shortname);

	/*
	trid to add this - but it crashes here:
	//mydev->substream->private_data = mydev;
	Well, first time real substream comes in, is in _open - so
	that has to be handled there.. That is: at this point, mydev->substream is null,
	and we first have a chance to set it ... in _open!
	*/

	ret = snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS, snd_dma_continuous_data(GFP_KERNEL), MAX_BUFFER, MAX_BUFFER);

	if (ret < 0)
		goto __nodev;

	// * will use the snd_card_register form from aloop-kernel.c/dummy.c here..
	ret = snd_card_register(card);

	if (ret == 0)   // or... (!ret)
	{
		platform_set_drvdata(devptr, card);
		return 0; // success
	}

__nodev: // as in aloop/dummy...
	dbg("__nodev reached!!");
	snd_card_free(card); // this will autocall .dev_free (= minivosc_pcm_dev_free)
	return ret;
}

// from dummy/aloop:
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
static int __devexit minivosc_remove(struct platform_device *devptr)
#else
static int minivosc_remove(struct platform_device *devptr)
#endif
{
	dbg("%s", __func__);
	snd_card_free(platform_get_drvdata(devptr));
	platform_set_drvdata(devptr, NULL);
	return 0;
}


/*
 *
 * hw alloc/free functions
 *
 */
static int minivosc_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *hw_params)
{
	dbg("%s", __func__);
	return snd_pcm_lib_malloc_pages(ss, params_buffer_bytes(hw_params));
}

static int minivosc_hw_free(struct snd_pcm_substream *ss)
{
	dbg("%s", __func__);
	return snd_pcm_lib_free_pages(ss);
}


/*
 *
 * PCM functions
 *
 */
static int minivosc_pcm_open(struct snd_pcm_substream *ss)
{
	struct minivosc_device *mydev = ss->private_data;

	//BREAKPOINT();
	dbg("%s", __func__);

	// copied from aloop-kernel.c:
	mutex_lock(&mydev->cable_lock);

	ss->runtime->hw = minivosc_pcm_hw;

	mydev->substream = ss; 	//save (system given) substream *ss, in our structure field
	ss->runtime->private_data = mydev;
	mydev->wvf_pos = 0; 	//init
	mydev->wvf_lift = 0; 	//init

	// SETUP THE TIMER HERE:
	setup_timer(&mydev->timer, minivosc_timer_function, /* user data */(unsigned long)mydev);

	mutex_unlock(&mydev->cable_lock);
	return 0;
}

static int minivosc_pcm_close(struct snd_pcm_substream *ss)
{
	struct minivosc_device *mydev = ss->private_data;

	dbg("%s", __func__);

	// copied from aloop-kernel.c:
	// * even though mutexes are retrieved from ss->private_data,
	// * which will be set to null,
	// * lock the mutex here anyway:
	mutex_lock(&mydev->cable_lock);
	// * not much else to do here, but set to null:
	ss->private_data = NULL;
	mutex_unlock(&mydev->cable_lock);

	return 0;
}


static int minivosc_pcm_prepare(struct snd_pcm_substream *ss)
{
	// copied from aloop-kernel.c

	// for one, we could get mydev from ss->private_data...
	// here we try it via ss->runtime->private_data instead.
	// turns out, this type of call via runtime->private_data
	// ends up with mydev as null pointer causing SIGSEGV
	// .. UNLESS runtime->private_data is assigned in _open?
	struct snd_pcm_runtime *runtime = ss->runtime;
	struct minivosc_device *mydev = runtime->private_data;
	unsigned int bps; // bytes per sec (eg. 16000 @ PCM16 8000Hz)
	unsigned int format_width = snd_pcm_format_width(runtime->format) / 8;

	dbg("%s()", __func__);
	dbg2("	runtime->rate=%d (format_width=%d), runtime->channels=%d", runtime->rate, format_width, runtime->channels);

	bps = runtime->rate * runtime->channels * format_width; // params requested by user app (arecord, audacity)
	if (bps <= 0)
		return -EINVAL;

	mydev->buf_pos = 0;
	mydev->pcm_buffer_size = frames_to_bytes(runtime, runtime->buffer_size);
	dbg2("	bps: %u; runtime->buffer_size: %lu; mydev->pcm_buffer_size: %u", bps, runtime->buffer_size, mydev->pcm_buffer_size);
	if (ss->stream == SNDRV_PCM_STREAM_CAPTURE) {
		/* clear capture buffer */
		mydev->silent_size = mydev->pcm_buffer_size;
		//memset(runtime->dma_area, 0, mydev->pcm_buffer_size);
		// we're in char land here, so let's mark prepare buffer with value 45 (signature)
		// this turns out to set everything permanently throughout - not just first buffer,
		// even though it runs only at start?
		memset(runtime->dma_area, 45, mydev->pcm_buffer_size);
	}

	if (!mydev->running) {
		mydev->irq_pos = 0;
	}


	mutex_lock(&mydev->cable_lock);
	if (!(mydev->valid & ~(1 << ss->stream))) {
		mydev->pcm_bps = bps;
		mydev->pcm_period_size = frames_to_bytes(runtime, runtime->period_size);
		mydev->period_size_frac = frac_pos(mydev->pcm_period_size);
	}
	mydev->valid |= 1 << ss->stream;
	mutex_unlock(&mydev->cable_lock);

	dbg2("	pcm_period_size=%u; period_size_frac=%u", mydev->pcm_period_size, mydev->period_size_frac);

	return 0;
}


static int minivosc_pcm_trigger(struct snd_pcm_substream *ss,
                          int cmd)
{
	int ret = 0;
	//copied from aloop-kernel.c

	//here we do not get mydev from
	// ss->runtime->private_data; but from:
	struct minivosc_device *mydev = ss->private_data;

	dbg("%s - trig %d (start=%d, stop=%d)", __func__, cmd, SNDRV_PCM_TRIGGER_START, SNDRV_PCM_TRIGGER_STOP);

	switch (cmd)
	{
		case SNDRV_PCM_TRIGGER_START:
			// Start the hardware capture
			// from aloop-kernel.c:
			if (!mydev->running) {
				// SET OFF THE TIMER HERE:
				minivosc_timer_start(mydev);
			}
			mydev->running |= (1 << ss->stream);
			break;
		case SNDRV_PCM_TRIGGER_STOP:
			// Stop the hardware capture
			// from aloop-kernel.c:
			mydev->running &= ~(1 << ss->stream);
			if (!mydev->running)
				// STOP THE TIMER HERE:
				minivosc_timer_stop(mydev);
			break;
		default:
			ret = -EINVAL;
	}

	return ret;
}


static snd_pcm_uframes_t minivosc_pcm_pointer(struct snd_pcm_substream *ss)
{
	//copied from aloop-kernel.c
	struct snd_pcm_runtime *runtime = ss->runtime;
	struct minivosc_device *mydev= runtime->private_data;

	// dbg2("+minivosc_pointer ");
	// minivosc_pos_update(mydev);
	// dbg2("+	bytes_to_frames(: %lu, mydev->buf_pos: %d", bytes_to_frames(runtime, mydev->buf_pos),mydev->buf_pos);
	return bytes_to_frames(runtime, mydev->buf_pos);

}


/*
 *
 * Timer functions
 *
 */
static void minivosc_timer_start(struct minivosc_device *mydev)
{
	dbg2("minivosc_timer_start()");
	mydev->last_jiffies = jiffies;

	// Fixed 50ms timer
	mydev->timer.expires = mydev->last_jiffies + (50 * HZ / 1000);
	dbg2("	last_jiffies=%lu, next_jiffies=%lu", mydev->last_jiffies, mydev->timer.expires);
	add_timer(&mydev->timer);
}

static void minivosc_timer_stop(struct minivosc_device *mydev)
{
	dbg2("minivosc_timer_stop");
	del_timer(&mydev->timer);
}

static void minivosc_timer_function(unsigned long data)
{
	unsigned int last_pos, count;
	unsigned long delta;
	unsigned long jiffies_now = jiffies;
	struct minivosc_device *mydev = (struct minivosc_device *)data;

	delta = jiffies_now - mydev->last_jiffies;
	dbg2("%s() // jiffies delta = %lu", __func__, delta);
	if (!mydev->running)
		return;

	if (delta == 0) goto timer_restart;

	mydev->last_jiffies += jiffies_now;

	last_pos = byte_pos(mydev->irq_pos);
	mydev->irq_pos += delta * mydev->pcm_bps;
	count = byte_pos(mydev->irq_pos) - last_pos;

	dbg2("*	: bytes count=%d (dma buf pos=%d, size=%d)", count, mydev->buf_pos, mydev->pcm_buffer_size);

	if (!count) goto timer_restart;

	// FILL BUFFER HERE
	minivosc_fill_capture_buf(mydev, count);

	if (mydev->irq_pos >= mydev->period_size_frac)
	{
		// dbg2("*	: mydev->irq_pos >= mydev->period_size_frac %d, calling snd_pcm_period_elapsed", mydev->period_size_frac);
		mydev->irq_pos %= mydev->period_size_frac;
		snd_pcm_period_elapsed(mydev->substream);
	}

timer_restart:
	// SET OFF THE TIMER HERE:
	minivosc_timer_start(mydev);
	return;
}

static void minivosc_fill_capture_buf(struct minivosc_device *mydev, unsigned int bytes)
{
	char *dst = mydev->substream->runtime->dma_area;
	float wrdat;
	unsigned i, j;
	unsigned int dst_off = mydev->buf_pos; // buf_pos is in bytes, not in samples !

	for (j=0; j < bytes; j++) {
		int mylift = mydev->wvf_lift*10 - 10;
		for (i=0; i<sizeof(wvfdat); i++) {
			wvfdat[i] = wvfdat2[i]+mylift;
		}

		dst[mydev->buf_pos] = wvfdat[mydev->wvf_pos];
		mydev->buf_pos++;
		mydev->wvf_pos++;

		if (mydev->wvf_pos >= wvfsz) { // we should wrap waveform here..
			mydev->wvf_pos = 0;
			// also handle lift here..
			mydev->wvf_lift++;
			if (mydev->wvf_lift >=4) mydev->wvf_lift = 0;
		}
		if (mydev->buf_pos >= mydev->pcm_buffer_size) {
			mydev->buf_pos = 0;
			//break; //we don;t really need this
		}
	} // end loop v2 */

	if (mydev->silent_size >= mydev->pcm_buffer_size)
		return;

	// NOTE: usually, the code returns by now -
	// - it doesn't even execute past this point!
	// from here on, apparently silent_size should be handled..

	if (mydev->silent_size + bytes > mydev->pcm_buffer_size)
		bytes = mydev->pcm_buffer_size - mydev->silent_size;

	wrdat = -0.2; // value to copy, instead of 0 for silence (if needed)
	for (;;) {
		unsigned int size = bytes;
		unsigned int dpos = 0;
		dbg2("_ clearrr..	%d", bytes);
		if (dst_off + size > mydev->pcm_buffer_size)
			size = mydev->pcm_buffer_size - dst_off;

		//memset(dst + dst_off, 255, size); //0, size);
		while (dpos < size)
		{
			memcpy(dst + dst_off + dpos, &wrdat, sizeof(wrdat));
			dpos += sizeof(wrdat);
			if (dpos >= size) break;
		}
		mydev->silent_size += size;
		bytes -= size;
		if (!bytes)
			break;
		dst_off = 0;
	}
}


/*
 *
 * snd_device_ops free functions
 *
 */
// these should eventually get called by snd_card_free (via .dev_free)
// however, since we do no special allocations, we need not free anything
static int minivosc_pcm_free(struct minivosc_device *chip)
{
	dbg("%s", __func__);
	return 0;
}

static int minivosc_pcm_dev_free(struct snd_device *device)
{
	dbg("%s", __func__);
	return minivosc_pcm_free(device->device_data);
}

/*
 *
 * functions for driver/kernel module initialization
 * (_init, _exit)
 * copied from aloop-kernel.c (same in dummy.c)
 *
 */
static void minivosc_unregister_all(void)
{
	int i;

	dbg("%s", __func__);

	for (i = 0; i < ARRAY_SIZE(devices); ++i)
		platform_device_unregister(devices[i]);

	platform_driver_unregister(&minivosc_driver);
}

static int __init alsa_card_minivosc_init(void)
{
	int i, err, cards;

	dbg("%s", __func__);
	err = dc_netlink_init();
	if (err != 0)
		return -EEXIST;

	err = platform_driver_register(&minivosc_driver);
	if (err < 0)
		return err;

	cards = 0;
	for (i = 0; i < SNDRV_CARDS; i++)
	{
		struct platform_device *device;

		if (!enable[i])
			continue;

		device = platform_device_register_simple(SND_MINIVOSC_DRIVER, i, NULL, 0);

		if (IS_ERR(device))
			continue;

		if (!platform_get_drvdata(device))
		{
			platform_device_unregister(device);
			continue;
		}

		devices[i] = device;
		cards++;
	}

	if (!cards)
	{
#ifdef MODULE
		printk(KERN_ERR "minivosc-alsa: No enabled, not found or device busy\n");
#endif
		minivosc_unregister_all();
		return -ENODEV;
	}

	return 0;
}

static void __exit alsa_card_minivosc_exit(void)
{
	dbg("%s", __func__);
	dc_netlink_fini();
	minivosc_unregister_all();
}

module_init(alsa_card_minivosc_init)
module_exit(alsa_card_minivosc_exit)
