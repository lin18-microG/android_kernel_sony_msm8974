/*
 * Linux cfg80211 Vendor Extension Code
 *
 * Copyright (C) 1999-2014, Broadcom Corporation
 *
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 *
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 *
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 * $Id: wl_cfgvendor.c 473890 2014-04-30 01:55:06Z $
*/

/*
 * New vendor interface additon to nl80211/cfg80211 to allow vendors
 * to implement proprietary features over the cfg80211 stack.
*/

#include <typedefs.h>
#include <linuxver.h>
#include <osl.h>
#include <linux/kernel.h>

#include <bcmutils.h>
#include <bcmwifi_channels.h>
#include <bcmendian.h>
#include <proto/ethernet.h>
#include <proto/802.11.h>
#include <linux/if_arp.h>
#include <asm/uaccess.h>


#include <dngl_stats.h>
#include <dhd.h>
#include <dhdioctl.h>
#include <wlioctl.h>
#include <dhd_cfg80211.h>
#ifdef PNO_SUPPORT
#include <dhd_pno.h>
#endif /* PNO_SUPPORT */
#ifdef RTT_SUPPORT
#include <dhd_rtt.h>
#endif /* RTT_SUPPORT */
#include <dhd_debug.h>
#include <proto/ethernet.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/sched.h>
#include <linux/etherdevice.h>
#include <linux/wireless.h>
#include <linux/ieee80211.h>
#include <linux/wait.h>
#include <linux/vmalloc.h>
#include <net/cfg80211.h>
#include <net/rtnetlink.h>

#include <wlioctl.h>
#include <wldev_common.h>
#include <wl_cfg80211.h>
#include <wl_cfgp2p.h>
#include <wl_android.h>
#include <wl_cfgvendor.h>
#ifdef PROP_TXSTATUS
#include <dhd_wlfc.h>
#endif

#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 13, 0)) || defined(WL_VENDOR_EXT_SUPPORT)

/*
 * This API is to be used for asynchronous vendor events. This
 * shouldn't be used in response to a vendor command from its
 * do_it handler context (instead wl_cfgvendor_send_cmd_reply should
 * be used).
 */
int wl_cfgvendor_send_async_event(struct wiphy *wiphy,
	struct net_device *dev, int event_id, const void  *data, int len)
{
	u16 kflags;
	struct sk_buff *skb;

	kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;

	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_event_alloc(wiphy, len, event_id, kflags);
	if (!skb) {
		WL_ERR(("skb alloc failed"));
		return -ENOMEM;
	}

	/* Push the data to the skb */
	nla_put_nohdr(skb, len, data);

	cfg80211_vendor_event(skb, kflags);

	return 0;
}

static int wl_cfgvendor_send_cmd_reply(struct wiphy *wiphy,
	struct net_device *dev, const void  *data, int len)
{
	struct sk_buff *skb;

	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, len);
	if (unlikely(!skb)) {
		WL_ERR(("skb alloc failed"));
		return -ENOMEM;
	}

	/* Push the data to the skb */
	nla_put_nohdr(skb, len, data);

	return cfg80211_vendor_cmd_reply(skb);
}

static int wl_cfgvendor_get_feature_set(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int reply;

	reply = dhd_dev_get_feature_set(bcmcfg_to_prmry_ndev(cfg));

	err =  wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        &reply, sizeof(int));

	if (unlikely(err))
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));

	return err;
}

static int wl_cfgvendor_get_feature_set_matrix(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	struct sk_buff *skb;
	int *reply;
	int num, mem_needed, i;

	reply = dhd_dev_get_feature_set_matrix(bcmcfg_to_prmry_ndev(cfg), &num);

	if (!reply) {
		WL_ERR(("Could not get feature list matrix\n"));
		err = -EINVAL;
		return err;
	}

	mem_needed = VENDOR_REPLY_OVERHEAD + (ATTRIBUTE_U32_LEN * num) +
	             ATTRIBUTE_U32_LEN;

	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, mem_needed);
	if (unlikely(!skb)) {
		WL_ERR(("skb alloc failed"));
		err = -ENOMEM;
		goto exit;
	}

	nla_put_u32(skb, ANDR_WIFI_ATTRIBUTE_NUM_FEATURE_SET, num);
	for (i = 0; i < num; i++) {
		nla_put_u32(skb, ANDR_WIFI_ATTRIBUTE_FEATURE_SET, reply[i]);
	}

	err =  cfg80211_vendor_cmd_reply(skb);

	if (unlikely(err))
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));
exit:
	kfree(reply);
	return err;
}

static int wl_cfgvendor_set_rand_mac_oui(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int type;
	uint8 random_mac_oui[DOT11_OUI_LEN];

	type = nla_type(data);

	if (type == ANDR_WIFI_ATTRIBUTE_RANDOM_MAC_OUI) {
		memcpy(random_mac_oui, nla_data(data), DOT11_OUI_LEN);

		err = dhd_dev_cfg_rand_mac_oui(bcmcfg_to_prmry_ndev(cfg), random_mac_oui);

		if (unlikely(err))
			WL_ERR(("Bad OUI, could not set:%d \n", err));

	} else {
		err = -1;
	}

	return err;
}

static int wl_cfgvendor_set_nodfs_flag(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int type;
	u32 nodfs;

	type = nla_type(data);
	if (type == ANDR_WIFI_ATTRIBUTE_NODFS_SET) {
		nodfs = nla_get_u32(data);
		err = dhd_dev_set_nodfs(bcmcfg_to_prmry_ndev(cfg), nodfs);
	} else {
		err = -1;
	}
	return err;
}

#ifdef GSCAN_SUPPORT
int wl_cfgvendor_send_hotlist_event(struct wiphy *wiphy,
	struct net_device *dev, void  *data, int len, wl_vendor_event_t event)
{
	u16 kflags;
	const void *ptr;
	struct sk_buff *skb;
	int malloc_len, total, iter_cnt_to_send, cnt;
	gscan_results_cache_t *cache = (gscan_results_cache_t *)data;

	total = len/sizeof(wifi_gscan_result_t);
	while (total > 0) {
		malloc_len = (total * sizeof(wifi_gscan_result_t)) + VENDOR_DATA_OVERHEAD;
		if (malloc_len > NLMSG_DEFAULT_SIZE) {
			malloc_len = NLMSG_DEFAULT_SIZE;
		}
		iter_cnt_to_send =
		   (malloc_len - VENDOR_DATA_OVERHEAD)/sizeof(wifi_gscan_result_t);
		total = total - iter_cnt_to_send;

		kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;

		/* Alloc the SKB for vendor_event */
		skb = cfg80211_vendor_event_alloc(wiphy, malloc_len, event, kflags);
		if (!skb) {
			WL_ERR(("skb alloc failed"));
			return -ENOMEM;
		}

		while (cache && iter_cnt_to_send) {
			ptr = (const void *) &cache->results[cache->tot_consumed];

			if (iter_cnt_to_send < (cache->tot_count - cache->tot_consumed))
				cnt = iter_cnt_to_send;
			else
				cnt = (cache->tot_count - cache->tot_consumed);

			iter_cnt_to_send -= cnt;
			cache->tot_consumed += cnt;
			/* Push the data to the skb */
			nla_append(skb, cnt * sizeof(wifi_gscan_result_t), ptr);
			if (cache->tot_consumed == cache->tot_count)
				cache = cache->next;

		}

		cfg80211_vendor_event(skb, kflags);
	}

	return 0;
}

static int wl_cfgvendor_gscan_get_capabilities(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pno_gscan_capabilities_t *reply = NULL;
	uint32 reply_len = 0;


	reply = dhd_dev_pno_get_gscan(bcmcfg_to_prmry_ndev(cfg),
	   DHD_PNO_GET_CAPABILITIES, NULL, &reply_len);
	if (!reply) {
		WL_ERR(("Could not get capabilities\n"));
		err = -EINVAL;
		return err;
	}

	err =  wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        reply, reply_len);

	if (unlikely(err))
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));

	kfree(reply);
	return err;
}

static int wl_cfgvendor_gscan_get_channel_list(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0, type, band;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	uint16 *reply = NULL;
	uint32 reply_len = 0, num_channels, mem_needed;
	struct sk_buff *skb;

	type = nla_type(data);

	if (type == GSCAN_ATTRIBUTE_BAND) {
		band = nla_get_u32(data);
	} else {
		return -1;
	}

	reply = dhd_dev_pno_get_gscan(bcmcfg_to_prmry_ndev(cfg),
	   DHD_PNO_GET_CHANNEL_LIST, &band, &reply_len);

	if (!reply) {
		WL_ERR(("Could not get channel list\n"));
		err = -EINVAL;
		return err;
	}
	num_channels =  reply_len/ sizeof(uint32);
	mem_needed = reply_len + VENDOR_REPLY_OVERHEAD + (ATTRIBUTE_U32_LEN * 2);

	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, mem_needed);
	if (unlikely(!skb)) {
		WL_ERR(("skb alloc failed"));
		err = -ENOMEM;
		goto exit;
	}

	nla_put_u32(skb, GSCAN_ATTRIBUTE_NUM_CHANNELS, num_channels);
	nla_put(skb, GSCAN_ATTRIBUTE_CHANNEL_LIST, reply_len, reply);

	err =  cfg80211_vendor_cmd_reply(skb);

	if (unlikely(err))
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));
exit:
	kfree(reply);
	return err;
}

static int wl_cfgvendor_gscan_get_batch_results(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	gscan_results_cache_t *results, *iter;
	uint32 reply_len, complete = 1;
	int32 mem_needed, num_results_iter;
	wifi_gscan_result_t *ptr;
	uint16 num_scan_ids, num_results;
	struct sk_buff *skb;
	struct nlattr *scan_hdr, *complete_flag;

	err = dhd_dev_wait_batch_results_complete(bcmcfg_to_prmry_ndev(cfg));
	if (err != BCME_OK)
		return -EBUSY;

	err = dhd_dev_pno_lock_access_batch_results(bcmcfg_to_prmry_ndev(cfg));
	if (err != BCME_OK) {
		WL_ERR(("Can't obtain lock to access batch results %d\n", err));
		return -EBUSY;
	}
	results = dhd_dev_pno_get_gscan(bcmcfg_to_prmry_ndev(cfg),
	             DHD_PNO_GET_BATCH_RESULTS, NULL, &reply_len);

	if (!results) {
		WL_ERR(("No results to send %d\n", err));
		err =  wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
		        results, 0);

		if (unlikely(err))
			WL_ERR(("Vendor Command reply failed ret:%d \n", err));
		dhd_dev_pno_unlock_access_batch_results(bcmcfg_to_prmry_ndev(cfg));
		return err;
	}
	num_scan_ids = reply_len & 0xFFFF;
	num_results = (reply_len & 0xFFFF0000) >> 16;
	mem_needed = (num_results * sizeof(wifi_gscan_result_t)) +
	             (num_scan_ids * GSCAN_BATCH_RESULT_HDR_LEN) +
	             VENDOR_REPLY_OVERHEAD + SCAN_RESULTS_COMPLETE_FLAG_LEN;

	if (mem_needed > (int32)NLMSG_DEFAULT_SIZE) {
		mem_needed = (int32)NLMSG_DEFAULT_SIZE;
	}

	WL_TRACE(("complete %d mem_needed %d max_mem %d\n", complete, mem_needed,
		(int)NLMSG_DEFAULT_SIZE));
	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, mem_needed);
	if (unlikely(!skb)) {
		WL_ERR(("skb alloc failed"));
		dhd_dev_pno_unlock_access_batch_results(bcmcfg_to_prmry_ndev(cfg));
		return -ENOMEM;
	}
	iter = results;
	complete_flag = nla_reserve(skb, GSCAN_ATTRIBUTE_SCAN_RESULTS_COMPLETE,
	                    sizeof(complete));
	mem_needed = mem_needed - (SCAN_RESULTS_COMPLETE_FLAG_LEN + VENDOR_REPLY_OVERHEAD);

	while (iter) {
		num_results_iter =
		    (mem_needed - (int32)GSCAN_BATCH_RESULT_HDR_LEN)/(int32)sizeof(wifi_gscan_result_t);
		if (num_results_iter <= 0 ||
		    ((iter->tot_count - iter->tot_consumed) > num_results_iter)) {
			break;
		}
		scan_hdr = nla_nest_start(skb, GSCAN_ATTRIBUTE_SCAN_RESULTS);
		/* no more room? we are done then (for now) */
		if (scan_hdr == NULL) {
			complete = 0;
			break;
		}
		nla_put_u32(skb, GSCAN_ATTRIBUTE_SCAN_ID, iter->scan_id);
		nla_put_u8(skb, GSCAN_ATTRIBUTE_SCAN_FLAGS, iter->flag);
		nla_put_u32(skb, GSCAN_ATTRIBUTE_CH_BUCKET_BITMASK, iter->scan_ch_bucket);

		num_results_iter = iter->tot_count - iter->tot_consumed;

		nla_put_u32(skb, GSCAN_ATTRIBUTE_NUM_OF_RESULTS, num_results_iter);
		if (num_results_iter) {
			ptr = &iter->results[iter->tot_consumed];
			iter->tot_consumed += num_results_iter;
			nla_put(skb, GSCAN_ATTRIBUTE_SCAN_RESULTS,
			 num_results_iter * sizeof(wifi_gscan_result_t), ptr);
		}
		nla_nest_end(skb, scan_hdr);
		mem_needed -= GSCAN_BATCH_RESULT_HDR_LEN +
		    (num_results_iter * sizeof(wifi_gscan_result_t));
		iter = iter->next;
	}
	/* Returns TRUE if all result consumed */
	complete = dhd_dev_gscan_batch_cache_cleanup(bcmcfg_to_prmry_ndev(cfg));
	memcpy(nla_data(complete_flag), &complete, sizeof(complete));
	dhd_dev_pno_unlock_access_batch_results(bcmcfg_to_prmry_ndev(cfg));
	return cfg80211_vendor_cmd_reply(skb);
}

static int wl_cfgvendor_initiate_gscan(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int type, tmp = len;
	int run = 0xFF;
	int flush = 0;
	const struct nlattr *iter;

	nla_for_each_attr(iter, data, len, tmp) {
		type = nla_type(iter);
		if (type == GSCAN_ATTRIBUTE_ENABLE_FEATURE)
			run = nla_get_u32(iter);
		else if (type == GSCAN_ATTRIBUTE_FLUSH_FEATURE)
			flush = nla_get_u32(iter);
	}

	if (run != 0xFF) {
		err = dhd_dev_pno_run_gscan(bcmcfg_to_prmry_ndev(cfg), run, flush);

		if (unlikely(err))
			WL_ERR(("Could not run gscan:%d \n", err));
		return err;
	} else {
		return -1;
	}


}

static int wl_cfgvendor_enable_full_scan_result(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int type;
	bool real_time = FALSE;

	type = nla_type(data);

	if (type == GSCAN_ATTRIBUTE_ENABLE_FULL_SCAN_RESULTS) {
		real_time = nla_get_u32(data);

		err = dhd_dev_pno_enable_full_scan_result(bcmcfg_to_prmry_ndev(cfg), real_time);

		if (unlikely(err))
			WL_ERR(("Could not run gscan:%d \n", err));

	} else {
		err = -1;
	}

	return err;
}

static int
wl_cfgvendor_set_scan_cfg_bucket(const struct nlattr *prev,
				 gscan_scan_params_t *scan_param, int num)
{
	struct dhd_pno_gscan_channel_bucket  *ch_bucket;
	int k = 0;
	int type, err = 0, rem;
	const struct nlattr *cur, *next;

	nla_for_each_nested(cur, prev, rem) {
		type = nla_type(cur);
		ch_bucket = scan_param->channel_bucket;
		switch (type) {
		case GSCAN_ATTRIBUTE_BUCKET_ID:
			break;
		case GSCAN_ATTRIBUTE_BUCKET_PERIOD:
			if (nla_len(cur) != sizeof(uint32)) {
				err = -EINVAL;
				goto exit;
			}

			ch_bucket[num].bucket_freq_multiple =
				nla_get_u32(cur) / MSEC_PER_SEC;
			break;
		case GSCAN_ATTRIBUTE_BUCKET_NUM_CHANNELS:
			if (nla_len(cur) != sizeof(uint32)) {
				err = -EINVAL;
				goto exit;
			}
			ch_bucket[num].num_channels = nla_get_u32(cur);
			if (ch_bucket[num].num_channels >
				GSCAN_MAX_CHANNELS_IN_BUCKET) {
				WL_ERR(("channel range:%d,bucket:%d\n",
					ch_bucket[num].num_channels,
					num));
				err = -EINVAL;
				goto exit;
			}
			break;
		case GSCAN_ATTRIBUTE_BUCKET_CHANNELS:
			nla_for_each_nested(next, cur, rem) {
				if (k >= GSCAN_MAX_CHANNELS_IN_BUCKET)
					break;
				if (nla_len(next) != sizeof(uint32)) {
					err = -EINVAL;
					goto exit;
				}
				ch_bucket[num].chan_list[k] = nla_get_u32(next);
				k++;
			}
			break;
		case GSCAN_ATTRIBUTE_BUCKETS_BAND:
			if (nla_len(cur) != sizeof(uint32)) {
				err = -EINVAL;
				goto exit;
			}
			ch_bucket[num].band = (uint16)nla_get_u32(cur);
			break;
		case GSCAN_ATTRIBUTE_REPORT_EVENTS:
			if (nla_len(cur) != sizeof(uint32)) {
				err = -EINVAL;
				goto exit;
			}
			ch_bucket[num].report_flag = (uint8)nla_get_u32(cur);
			break;
		case GSCAN_ATTRIBUTE_BUCKET_STEP_COUNT:
			if (nla_len(cur) != sizeof(uint32)) {
				err = -EINVAL;
				goto exit;
			}
			ch_bucket[num].repeat = (uint16)nla_get_u32(cur);
			break;
		case GSCAN_ATTRIBUTE_BUCKET_MAX_PERIOD:
			if (nla_len(cur) != sizeof(uint32)) {
				err = -EINVAL;
				goto exit;
			}
			ch_bucket[num].bucket_max_multiple =
				nla_get_u32(cur) / MSEC_PER_SEC;
			break;
		default:
			WL_ERR(("unknown attr type:%d\n", type));
			err = -EINVAL;
			goto exit;
		}
	}

exit:
	return err;
}

static int
wl_cfgvendor_set_scan_cfg(struct wiphy *wiphy, struct wireless_dev *wdev,
			  const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	gscan_scan_params_t *scan_param;
	int j = 0;
	int type, tmp;
	const struct nlattr *iter;

	scan_param = kzalloc(sizeof(gscan_scan_params_t), GFP_KERNEL);
	if (!scan_param) {
		WL_ERR(("Could not set GSCAN scan cfg, mem alloc failure\n"));
		err = -EINVAL;
		return err;
	}

	scan_param->scan_fr = PNO_SCAN_MIN_FW_SEC;
	nla_for_each_attr(iter, data, len, tmp) {
		type = nla_type(iter);

		if (j >= GSCAN_MAX_CH_BUCKETS)
			break;

		switch (type) {
		case GSCAN_ATTRIBUTE_BASE_PERIOD:
			if (nla_len(iter) != sizeof(uint32)) {
				err = -EINVAL;
				goto exit;
			}
			scan_param->scan_fr = nla_get_u32(iter) / MSEC_PER_SEC;
			break;
		case GSCAN_ATTRIBUTE_NUM_BUCKETS:
			if (nla_len(iter) != sizeof(uint32)) {
				err = -EINVAL;
				goto exit;
			}
			scan_param->nchannel_buckets = nla_get_u32(iter);
			if (scan_param->nchannel_buckets >=
			    GSCAN_MAX_CH_BUCKETS) {
				WL_ERR(("ncha_buck out of range %d\n",
					scan_param->nchannel_buckets));
				err = -EINVAL;
				goto exit;
			}
			break;
		case GSCAN_ATTRIBUTE_CH_BUCKET_1:
		case GSCAN_ATTRIBUTE_CH_BUCKET_2:
		case GSCAN_ATTRIBUTE_CH_BUCKET_3:
		case GSCAN_ATTRIBUTE_CH_BUCKET_4:
		case GSCAN_ATTRIBUTE_CH_BUCKET_5:
		case GSCAN_ATTRIBUTE_CH_BUCKET_6:
		case GSCAN_ATTRIBUTE_CH_BUCKET_7:
			err = wl_cfgvendor_set_scan_cfg_bucket(iter,
							       scan_param, j);
			if (err < 0) {
				WL_ERR(("set_scan_cfg_buck error:%d\n", err));
				goto exit;
			}

			j++;
			break;
		default:
			WL_ERR(("Unknown attr type %d\n", type));
			err = -EINVAL;
			goto exit;
		}
	}

	err = dhd_dev_pno_set_cfg_gscan(bcmcfg_to_prmry_ndev(cfg),
					DHD_PNO_SCAN_CFG_ID, scan_param, FALSE);

	if (err < 0) {
		WL_ERR(("Could not set GSCAN scan cfg error %d\n", err));
		err = -EINVAL;
	}

exit:
	kfree(scan_param);
	return err;
}

static int wl_cfgvendor_hotlist_cfg(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	gscan_hotlist_scan_params_t *hotlist_params;
	int tmp, tmp1, tmp2, type, j = 0, dummy;
	const struct nlattr *outer, *inner = NULL, *iter;
	uint8 flush = 0;
	struct bssid_t *pbssid;

	if (len < sizeof(*hotlist_params) || len >= WLC_IOCTL_MAXLEN) {
		WL_ERR(("buffer length :%d wrong - bail out.\n", len));
		return -EINVAL;
	}

	hotlist_params = kzalloc(sizeof(*hotlist_params)
		+ (sizeof(struct bssid_t) * (PFN_SWC_MAX_NUM_APS - 1)),
		GFP_KERNEL);

	if (!hotlist_params) {
		WL_ERR(("Cannot Malloc mem.\n"));
		return -ENOMEM;
	}

	hotlist_params->lost_ap_window = GSCAN_LOST_AP_WINDOW_DEFAULT;

	nla_for_each_attr(iter, data, len, tmp2) {
		type = nla_type(iter);
		switch (type) {
		case GSCAN_ATTRIBUTE_HOTLIST_BSSID_COUNT:
			if (nla_len(iter) != sizeof(uint32)) {
				WL_DBG(("type:%d length:%d not matching.\n",
					type, nla_len(inner)));
				err = -EINVAL;
				goto exit;
			}
			hotlist_params->nbssid = (uint16)nla_get_u32(iter);
			if ((hotlist_params->nbssid == 0) ||
			    (hotlist_params->nbssid > PFN_SWC_MAX_NUM_APS)) {
				WL_ERR(("nbssid:%d exceed limit.\n",
					hotlist_params->nbssid));
				err = -EINVAL;
				goto exit;
			}
			break;
		case GSCAN_ATTRIBUTE_HOTLIST_BSSIDS:
			if (hotlist_params->nbssid == 0) {
				WL_ERR(("nbssid not retrieved.\n"));
				err = -EINVAL;
				goto exit;
			}
			pbssid = hotlist_params->bssid;
			nla_for_each_nested(outer, iter, tmp) {
				nla_for_each_nested(inner, outer, tmp1) {
					if (j >= hotlist_params->nbssid)
						break;
					type = nla_type(inner);

					switch (type) {
					case GSCAN_ATTRIBUTE_BSSID:
						if (nla_len(inner) != sizeof(pbssid[j].macaddr)) {
							WL_DBG(("type:%d length:%d not matching.\n",
								type, nla_len(inner)));
							err = -EINVAL;
							goto exit;
						}
						memcpy(
							&pbssid[j].macaddr,
							nla_data(inner),
							sizeof(pbssid[j].macaddr));
						break;
					case GSCAN_ATTRIBUTE_RSSI_LOW:
						if (nla_len(inner) != sizeof(uint8)) {
							WL_DBG(("type:%d length:%d not matching.\n",
								type, nla_len(inner)));
							err = -EINVAL;
							goto exit;
						}
						pbssid[j].rssi_reporting_threshold =
								(int8)nla_get_u8(inner);
						break;
					case GSCAN_ATTRIBUTE_RSSI_HIGH:
						if (nla_len(inner) != sizeof(uint8)) {
							WL_DBG(("type:%d length:%d not matching.\n",
								type, nla_len(inner)));
							err = -EINVAL;
							goto exit;
						}
						dummy = (int8)nla_get_u8(inner);
						break;
					}
				}
				j++;
			}
			if (j != hotlist_params->nbssid) {
				WL_ERR(("bssid_cnt:%d != nbssid:%d.\n", j,
					hotlist_params->nbssid));
				err = -EINVAL;
				goto exit;
			}
			break;
		case GSCAN_ATTRIBUTE_HOTLIST_FLUSH:
			if (nla_len(iter) != sizeof(uint8)) {
				WL_DBG(("type:%d length:%d not matching.\n",
					type, nla_len(inner)));
				err = -EINVAL;
				goto exit;
			}
			flush = nla_get_u8(iter);
			break;
		case GSCAN_ATTRIBUTE_LOST_AP_SAMPLE_SIZE:
			if (nla_len(iter) != sizeof(uint32)) {
				WL_DBG(("type:%d length:%d not matching.\n",
					type, nla_len(inner)));
				err = -EINVAL;
				goto exit;
			}
			hotlist_params->lost_ap_window = (uint16)nla_get_u32(iter);
			break;
		default:
			WL_DBG(("Unknown type %d\n", type));
			err = -EINVAL;
			goto exit;
		}

	}

	if (dhd_dev_pno_set_cfg_gscan(bcmcfg_to_prmry_ndev(cfg),
	      DHD_PNO_GEOFENCE_SCAN_CFG_ID, hotlist_params, flush) < 0) {
		WL_ERR(("Could not set GSCAN HOTLIST cfg\n"));
		err = -EINVAL;
		goto exit;
	}
exit:
	kfree(hotlist_params);
	return err;
}

/* Configures the ePNO SSID list and params sent
 * by HAL. The SSIDs are appended to the existing
 * SSID list. Thus to program a fresh list the FLUSH
 * attribute has to be set (before the SSID list
 * attributes) Can also be used to just FLUSH all
 * previously set config - SSID list and params.
 */
static int wl_cfgvendor_epno_cfg(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pno_ssid_t *ssid_elem;
	int tmp, tmp1, tmp2, type, num = 0;
	const struct nlattr *outer, *inner, *iter;
	uint32 cnt_ssid = 0;
	wl_pfn_ssid_params_t params;
	bool flush = FALSE;

	memset(&params, 0, sizeof(wl_pfn_ssid_params_t));
	nla_for_each_attr(iter, data, len, tmp2) {
		type = nla_type(iter);
		switch (type) {
			case GSCAN_ATTRIBUTE_EPNO_SSID_LIST:
				nla_for_each_nested(outer, iter, tmp) {
					ssid_elem = (dhd_pno_ssid_t *)
					          dhd_dev_pno_get_gscan(bcmcfg_to_prmry_ndev(cfg),
					                 DHD_PNO_GET_NEW_EPNO_SSID_ELEM,
					                 NULL, &num);
					if (!ssid_elem) {
						WL_ERR(("Failed to get SSID LIST buffer\n"));
						err = -ENOMEM;
						goto exit;
					}
					cnt_ssid++;
					nla_for_each_nested(inner, outer, tmp1) {
						type = nla_type(inner);
						switch (type) {
							case GSCAN_ATTRIBUTE_EPNO_SSID:
								memcpy(ssid_elem->SSID,
								  nla_data(inner),
								  DOT11_MAX_SSID_LEN);
								break;
							case GSCAN_ATTRIBUTE_EPNO_SSID_LEN:
								ssid_elem->SSID_len = nla_get_u32(inner);
								if (ssid_elem->SSID_len > DOT11_MAX_SSID_LEN) {
									WL_ERR(("SSID too long %d\n",
									      ssid_elem->SSID_len));
									err = -EINVAL;
									goto exit;
								}
								break;
							case GSCAN_ATTRIBUTE_EPNO_FLAGS:
								ssid_elem->flags =
									nla_get_u32(inner);
								ssid_elem->hidden =
									((ssid_elem->flags &
									DHD_EPNO_HIDDEN_SSID) != 0);
								break;
							case GSCAN_ATTRIBUTE_EPNO_AUTH:
								ssid_elem->wpa_auth =
									nla_get_u32(inner);
								break;
						}
					}
					/* Missing/incorrect attribute? */
					if (!ssid_elem->SSID_len) {
						WL_ERR(("Broadcast SSID is illegal for ePNO\n"));
						err = -EINVAL;
						goto exit;
					}
					dhd_pno_translate_epno_fw_flags(&ssid_elem->flags);
					dhd_pno_set_epno_auth_flag(&ssid_elem->wpa_auth);
				}
				break;
			case GSCAN_ATTRIBUTE_EPNO_SSID_NUM:
				num = nla_get_u8(iter);
				break;
			case GSCAN_ATTRIBUTE_EPNO_FLUSH:
				flush = (bool)nla_get_u32(iter);
				/* Flush attribute is expected before any ssid attribute */
				if (cnt_ssid && flush) {
					WL_ERR(("Bad attributes\n"));
					err = -EINVAL;
					goto exit;
				}
				/* Need to flush driver and FW cfg */
				dhd_dev_pno_set_cfg_gscan(bcmcfg_to_prmry_ndev(cfg),
				   DHD_PNO_EPNO_CFG_ID, NULL, flush);
				dhd_dev_flush_fw_epno(bcmcfg_to_prmry_ndev(cfg));
				break;
			case GSCAN_ATTRIBUTE_EPNO_5G_RSSI_THR:
				params.min5G_rssi = nla_get_s8(iter);
				break;
			case GSCAN_ATTRIBUTE_EPNO_2G_RSSI_THR:
				params.min2G_rssi = nla_get_s8(iter);
				break;
			case GSCAN_ATTRIBUTE_EPNO_INIT_SCORE_MAX:
				params.init_score_max = nla_get_s16(iter);
				break;
			case GSCAN_ATTRIBUTE_EPNO_CUR_CONN_BONUS:
				params.cur_bssid_bonus = nla_get_s16(iter);
				break;
			case GSCAN_ATTRIBUTE_EPNO_SAME_NETWORK_BONUS:
				params.same_ssid_bonus = nla_get_s16(iter);
				break;
			case GSCAN_ATTRIBUTE_EPNO_SECURE_BONUS:
				params.secure_bonus = nla_get_s16(iter);
				break;
			case GSCAN_ATTRIBUTE_EPNO_5G_BONUS:
				params.band_5g_bonus = nla_get_s16(iter);
				break;
			default:
				WL_ERR(("No such attribute %d\n", type));
				err = -EINVAL;
				goto exit;
			}

	}
	if (cnt_ssid != num) {
		WL_ERR(("%s: num_ssid %d does not match ssids sent %d\n", __FUNCTION__,
		     num, cnt_ssid));
		err = -EINVAL;
	}
exit:
	/* Flush all configs if error condition */
	if (err < 0) {
		dhd_dev_pno_set_cfg_gscan(bcmcfg_to_prmry_ndev(cfg),
		   DHD_PNO_EPNO_CFG_ID, NULL, TRUE);
		dhd_dev_flush_fw_epno(bcmcfg_to_prmry_ndev(cfg));
	} else if (type != GSCAN_ATTRIBUTE_EPNO_FLUSH) {
		/* If the last attribute was FLUSH, nothing else to do */
		dhd_dev_pno_set_cfg_gscan(bcmcfg_to_prmry_ndev(cfg),
		   DHD_PNO_EPNO_PARAMS_ID, &params, FALSE);
		err = dhd_dev_set_epno(bcmcfg_to_prmry_ndev(cfg));
	}
	return err;
}

#ifdef DHD_ANQPO_SUPPORT
static int wl_cfgvendor_gscan_anqpo_config(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = BCME_ERROR, rem, type, malloc_size, i = 0;
	uint32 hs_list_size = 0;
	int j, k, num_oi, oi_len;
	wifi_passpoint_network *hs_list = NULL, *src_hs;
	wl_anqpo_pfn_hs_list_t *anqpo_hs_list;
	wl_anqpo_pfn_hs_t *dst_hs;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int tmp, tmp1;
	const struct nlattr *outer, *inner, *iter;
	static char iovar_buf[WLC_IOCTL_MAXLEN];
	char *rcid;

	nla_for_each_attr(iter, data, len, rem) {
	type = nla_type(iter);
	switch (type) {
	case GSCAN_ATTRIBUTE_ANQPO_HS_LIST:
		if (hs_list) {
			err = -EINVAL;
			goto exit;
		}
		if (hs_list_size > GSCAN_ANQPO_MAX_HS_LIST_SIZE) {
			err = -EINVAL;
			goto exit;
		}
		if (hs_list_size > 0) {
			hs_list = kzalloc(hs_list_size *
				sizeof(wifi_passpoint_network), GFP_KERNEL);
			if (!hs_list) {
				WL_ERR(("failed to allocate hs_list\n"));
				return -ENOMEM;
			}
		}
		nla_for_each_nested(outer, iter, tmp) {
			if (i == hs_list_size)
				break;
			nla_for_each_nested(inner, outer, tmp1) {
			type = nla_type(inner);

			switch (type) {
			case GSCAN_ATTRIBUTE_ANQPO_HS_NETWORK_ID:
				if (nla_len(inner) != sizeof(hs_list[i].id)) {
					err = -EINVAL;
					goto exit;
				}
				hs_list[i].id = nla_get_u32(inner);
				WL_DBG(("%s: net id: %d\n",
					__func__, hs_list[i].id));
				break;
			case GSCAN_ATTRIBUTE_ANQPO_HS_NAI_REALM:
				if (nla_len(inner) !=
					sizeof(hs_list[i].realm)) {
					err = -EINVAL;
					goto exit;
				}
				memcpy(hs_list[i].realm, nla_data(inner),
				       sizeof(hs_list[i].realm));
				WL_DBG(("%s: realm: %s\n",
					__func__, hs_list[i].realm));
				break;
			case GSCAN_ATTRIBUTE_ANQPO_HS_ROAM_CONSORTIUM_ID:
				if (nla_len(inner) != sizeof(hs_list[i].
					roamingConsortiumIds)) {
					err = -EINVAL;
					goto exit;
				}
				memcpy(hs_list[i].roamingConsortiumIds,
				       nla_data(inner),
				       sizeof(hs_list[i].roamingConsortiumIds));
				break;
			case GSCAN_ATTRIBUTE_ANQPO_HS_PLMN:
				if (nla_len(inner) != sizeof(hs_list[i].plmn)) {
					err = -EINVAL;
					goto exit;
				}
				memcpy(hs_list[i].plmn,
				       nla_data(inner),
				       sizeof(hs_list[i].plmn));
				WL_DBG(("%s: plmn: %c %c %c\n",
					__func__, hs_list[i].plmn[0],
					hs_list[i].plmn[1],
					hs_list[i].plmn[2]));
				break;
			}
			}
			i++;
		}
		break;
	case GSCAN_ATTRIBUTE_ANQPO_HS_LIST_SIZE:
		if (nla_len(iter) != sizeof(hs_list_size)) {
			err = -EINVAL;
			goto exit;
		}
		hs_list_size = nla_get_u32(iter);
		if ((hs_list_size == 0) ||
		    (hs_list_size > GSCAN_ANQPO_MAX_HS_LIST_SIZE)) {
			WL_ERR(("%s: ANQPO: %d\n", __func__, hs_list_size));
			err = -EINVAL;
			goto exit;
		}
		WL_DBG(("%s: ANQPO: %d\n", __func__, hs_list_size));
		break;
	default:
		WL_ERR(("Unknown type: %d\n", type));
		err = -EINVAL;
		goto exit;
	}

	}

	malloc_size = OFFSETOF(wl_anqpo_pfn_hs_list_t, hs) +
		(hs_list_size * (sizeof(wl_anqpo_pfn_hs_t)));
	anqpo_hs_list = (wl_anqpo_pfn_hs_list_t *)kmalloc(malloc_size, GFP_KERNEL);
	if (anqpo_hs_list == NULL) {
		WL_ERR(("failed to allocate anqpo_hs_list\n"));
		err = -ENOMEM;
		goto exit;
	}
	anqpo_hs_list->count = hs_list_size;
	anqpo_hs_list->is_clear = (hs_list_size == 0) ? TRUE : FALSE;

	if ((hs_list_size > 0) && (NULL != hs_list)) {
		src_hs = hs_list;
		dst_hs = &anqpo_hs_list->hs[0];
		for (i = 0; i < hs_list_size; i++, src_hs++, dst_hs++) {
			num_oi = 0;
			dst_hs->id = src_hs->id;
			dst_hs->realm.length = strlen(src_hs->realm)+1;
			memcpy(dst_hs->realm.data, src_hs->realm, dst_hs->realm.length);
			memcpy(dst_hs->plmn.mcc, src_hs->plmn, ANQPO_MCC_LENGTH);
			memcpy(dst_hs->plmn.mnc, src_hs->plmn, ANQPO_MCC_LENGTH);
			for (j = 0; j < ANQPO_MAX_PFN_HS; j++) {
				oi_len = 0;
				if (0 != src_hs->roamingConsortiumIds[j]) {
					num_oi++;
					rcid = (char *)&src_hs->roamingConsortiumIds[j];
					for (k = 0; k < ANQPO_MAX_OI_LENGTH; k++)
						if (0 != rcid[k]) oi_len++;

					dst_hs->rc.oi[j].length = oi_len;
					memcpy(dst_hs->rc.oi[j].data, rcid, oi_len);
				}
			}
			dst_hs->rc.numOi = num_oi;
		}
	}

	err = wldev_iovar_setbuf(bcmcfg_to_prmry_ndev(cfg), "anqpo_pfn_hs_list",
			anqpo_hs_list, malloc_size, iovar_buf, WLC_IOCTL_MAXLEN, NULL);

	kfree(anqpo_hs_list);
exit:
	kfree(hs_list);
	return err;
}
#endif /* DHD_ANQPO_SUPPORT */
static int wl_cfgvendor_set_batch_scan_cfg(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0, tmp, type;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	gscan_batch_params_t batch_param;
	const struct nlattr *iter;

	batch_param.mscan = batch_param.bestn = 0;
	batch_param.buffer_threshold = GSCAN_BATCH_NO_THR_SET;

	nla_for_each_attr(iter, data, len, tmp) {
		type = nla_type(iter);

		switch (type) {
			case GSCAN_ATTRIBUTE_NUM_AP_PER_SCAN:
				batch_param.bestn = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_NUM_SCANS_TO_CACHE:
				batch_param.mscan = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_REPORT_THRESHOLD:
				batch_param.buffer_threshold = nla_get_u32(iter);
				break;
		}
	}

	if (dhd_dev_pno_set_cfg_gscan(bcmcfg_to_prmry_ndev(cfg),
	       DHD_PNO_BATCH_SCAN_CFG_ID, &batch_param, FALSE) < 0) {
		WL_ERR(("Could not set batch cfg\n"));
		err = -EINVAL;
		return err;
	}

	return err;
}

static int wl_cfgvendor_significant_change_cfg(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	gscan_swc_params_t *significant_params;
	int tmp, tmp1, tmp2, type, j = 0;
	const struct nlattr *outer, *inner, *iter;
	bool flush = FALSE;
	wl_pfn_significant_bssid_t *pbssid;
	uint16 num_bssid = 0;
	uint16 max_buf_size = sizeof(gscan_swc_params_t) +
		sizeof(wl_pfn_significant_bssid_t) * (PFN_SWC_MAX_NUM_APS - 1);

	significant_params = kzalloc(max_buf_size, GFP_KERNEL);

	if (!significant_params) {
		WL_ERR(("Cannot Malloc mem size:%d\n", len));
		return BCME_NOMEM;
	}


	nla_for_each_attr(iter, data, len, tmp2) {
		type = nla_type(iter);

		switch (type) {
			case GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH:
			flush = (bool) nla_get_u8(iter);
			break;
			case GSCAN_ATTRIBUTE_RSSI_SAMPLE_SIZE:
				significant_params->rssi_window = nla_get_u16(iter);
				break;
			case GSCAN_ATTRIBUTE_LOST_AP_SAMPLE_SIZE:
				significant_params->lost_ap_window = nla_get_u16(iter);
				break;
			case GSCAN_ATTRIBUTE_MIN_BREACHING:
				significant_params->swc_threshold = nla_get_u16(iter);
				break;
			case GSCAN_ATTRIBUTE_NUM_BSSID:
				num_bssid = nla_get_u16(iter);
				if (num_bssid > PFN_SWC_MAX_NUM_APS) {
					WL_ERR(("ovar max SWC bssids:%d\n",
						num_bssid));
					err = BCME_BADARG;
					goto exit;
				}
				break;
			case GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_BSSIDS:
				if (num_bssid == 0) {
					WL_ERR(("num_bssid : 0\n"));
					err = BCME_BADARG;
					goto exit;
				}
				pbssid = significant_params->bssid_elem_list;
				nla_for_each_nested(outer, iter, tmp) {
					if (j >= num_bssid) {
						j++;
						break;
					}
					nla_for_each_nested(inner, outer, tmp1) {
							switch (nla_type(inner)) {
								case GSCAN_ATTRIBUTE_BSSID:
								memcpy(&(pbssid[j].macaddr),
								     nla_data(inner),
								     ETHER_ADDR_LEN);
								break;
								case GSCAN_ATTRIBUTE_RSSI_HIGH:
								pbssid[j].rssi_high_threshold =
								       (int8) nla_get_u8(inner);
								break;
								case GSCAN_ATTRIBUTE_RSSI_LOW:
								pbssid[j].rssi_low_threshold =
								      (int8) nla_get_u8(inner);
								break;
							}
						}
					j++;
				}
				break;
		}
	}
	if (j != num_bssid) {
		WL_ERR(("swc bssids count:%d not matched to num_bssid:%d\n",
			j, num_bssid));
		err = BCME_BADARG;
		goto exit;
	}
	significant_params->nbssid = j;

	if (dhd_dev_pno_set_cfg_gscan(bcmcfg_to_prmry_ndev(cfg),
	    DHD_PNO_SIGNIFICANT_SCAN_CFG_ID, significant_params, flush) < 0) {
		WL_ERR(("Could not set GSCAN significant cfg\n"));
		err = -EINVAL;
		goto exit;
	}
exit:
	kfree(significant_params);
	return err;
}

static int wl_cfgvendor_enable_lazy_roam(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = -EINVAL;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int type;
	uint32 lazy_roam_enable_flag;

	type = nla_type(data);

	if (type == GSCAN_ATTRIBUTE_LAZY_ROAM_ENABLE) {
		lazy_roam_enable_flag = nla_get_u32(data);

		err = dhd_dev_lazy_roam_enable(bcmcfg_to_prmry_ndev(cfg),
		           lazy_roam_enable_flag);

		if (unlikely(err))
			WL_ERR(("Could not enable lazy roam:%d \n", err));

	}
	return err;
}

static int wl_cfgvendor_set_lazy_roam_cfg(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0, tmp, type;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	wlc_roam_exp_params_t roam_param;
	const struct nlattr *iter;

	memset(&roam_param, 0, sizeof(roam_param));

	nla_for_each_attr(iter, data, len, tmp) {
		type = nla_type(iter);

		switch (type) {
			case GSCAN_ATTRIBUTE_A_BAND_BOOST_THRESHOLD:
				roam_param.a_band_boost_threshold = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_A_BAND_PENALTY_THRESHOLD:
				roam_param.a_band_penalty_threshold = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_A_BAND_BOOST_FACTOR:
				roam_param.a_band_boost_factor = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_A_BAND_PENALTY_FACTOR:
				roam_param.a_band_penalty_factor = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_A_BAND_MAX_BOOST:
				roam_param.a_band_max_boost = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_LAZY_ROAM_HYSTERESIS:
				roam_param.cur_bssid_boost = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_ALERT_ROAM_RSSI_TRIGGER:
				roam_param.alert_roam_trigger_threshold = nla_get_u32(iter);
				break;
		}
	}

	if (dhd_dev_set_lazy_roam_cfg(bcmcfg_to_prmry_ndev(cfg), &roam_param) < 0) {
		WL_ERR(("Could not set batch cfg\n"));
		err = -EINVAL;
	}
	return err;
}

/* small helper function */
static wl_bssid_pref_cfg_t *create_bssid_pref_cfg(uint32 num)
{
	uint32 mem_needed;
	wl_bssid_pref_cfg_t *bssid_pref;

	mem_needed = sizeof(wl_bssid_pref_cfg_t);
	if (num)
		mem_needed += (num - 1) * sizeof(wl_bssid_pref_list_t);
	bssid_pref = (wl_bssid_pref_cfg_t *) kmalloc(mem_needed, GFP_KERNEL);
	return bssid_pref;
}

static int wl_cfgvendor_set_bssid_pref(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	wl_bssid_pref_cfg_t *bssid_pref = NULL;
	wl_bssid_pref_list_t *bssids;
	int tmp, tmp1, tmp2, type;
	const struct nlattr *outer, *inner, *iter;
	uint32 flush = 0, i = 0, num = 0;

	/* Assumption: NUM attribute must come first */
	nla_for_each_attr(iter, data, len, tmp2) {
		type = nla_type(iter);
		switch (type) {
			case GSCAN_ATTRIBUTE_NUM_BSSID:
				num = nla_get_u32(iter);
				if (num > MAX_BSSID_PREF_LIST_NUM) {
					WL_ERR(("Too many Preferred BSSIDs!\n"));
					err = -EINVAL;
					goto exit;
				}
				break;
			case GSCAN_ATTRIBUTE_BSSID_PREF_FLUSH:
				flush = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_BSSID_PREF_LIST:
				if (!num)
					return -EINVAL;
				if ((bssid_pref = create_bssid_pref_cfg(num)) == NULL) {
					WL_ERR(("%s: Can't malloc memory\n", __FUNCTION__));
					err = -ENOMEM;
					goto exit;
				}
				bssid_pref->count = num;
				bssids = bssid_pref->bssids;
				nla_for_each_nested(outer, iter, tmp) {
					if (i >= num) {
						WL_ERR(("CFGs don't seem right!\n"));
						err = -EINVAL;
						goto exit;
					}
					nla_for_each_nested(inner, outer, tmp1) {
						type = nla_type(inner);
						switch (type) {
							case GSCAN_ATTRIBUTE_BSSID_PREF:
								memcpy(&(bssids[i].bssid),
								  nla_data(inner), ETHER_ADDR_LEN);
								/* not used for now */
								bssids[i].flags = 0;
								break;
							case GSCAN_ATTRIBUTE_RSSI_MODIFIER:
								bssids[i].rssi_factor =
								       (int8) nla_get_u32(inner);
								break;
						}
					}
					i++;
				}
				break;
			default:
				WL_ERR(("%s: No such attribute %d\n", __FUNCTION__, type));
				break;
			}
	}

	if (!bssid_pref) {
		/* What if only flush is desired? */
		if (flush) {
			if ((bssid_pref = create_bssid_pref_cfg(0)) == NULL) {
				WL_ERR(("%s: Can't malloc memory\n", __FUNCTION__));
				err = -ENOMEM;
				goto exit;
			}
			bssid_pref->count = 0;
		} else {
			err = -EINVAL;
			goto exit;
		}
	}
	err = dhd_dev_set_lazy_roam_bssid_pref(bcmcfg_to_prmry_ndev(cfg),
	          bssid_pref, flush);
exit:
	kfree(bssid_pref);
	return err;
}


static int wl_cfgvendor_set_bssid_blacklist(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	maclist_t *blacklist = NULL;
	int err = 0;
	int type, tmp;
	const struct nlattr *iter;
	uint32 mem_needed = 0, flush = 0, i = 0, num = 0;

	/* Assumption: NUM attribute must come first */
	nla_for_each_attr(iter, data, len, tmp) {
		type = nla_type(iter);
		switch (type) {
			case GSCAN_ATTRIBUTE_NUM_BSSID:
				num = nla_get_u32(iter);
				if (num > MAX_BSSID_BLACKLIST_NUM) {
					WL_ERR(("Too many Blacklist BSSIDs!\n"));
					err = -EINVAL;
					goto exit;
				}
				break;
			case GSCAN_ATTRIBUTE_BSSID_BLACKLIST_FLUSH:
				flush = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_BLACKLIST_BSSID:
				if (num) {
					if (!blacklist) {
						mem_needed = sizeof(maclist_t) +
						     sizeof(struct ether_addr) * (num - 1);
						blacklist = (maclist_t *)
						      kmalloc(mem_needed, GFP_KERNEL);
						if (!blacklist) {
							WL_ERR(("%s: Can't malloc %d bytes\n",
							     __FUNCTION__, mem_needed));
							err = -ENOMEM;
							goto exit;
						}
						blacklist->count = num;
					}
					if (i >= num) {
						WL_ERR(("CFGs don't seem right!\n"));
						err = -EINVAL;
						goto exit;
					}
					memcpy(&(blacklist->ea[i]),
					  nla_data(iter), ETHER_ADDR_LEN);
					i++;
				}
				break;
			default:
				WL_ERR(("%s: No such attribute %d\n", __FUNCTION__, type));
				break;
			}
	}
	err = dhd_dev_set_blacklist_bssid(bcmcfg_to_prmry_ndev(cfg),
	          blacklist, mem_needed, flush);
exit:
	kfree(blacklist);
	return err;
}

static int wl_cfgvendor_set_ssid_whitelist(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	wl_ssid_whitelist_t *ssid_whitelist = NULL;
	wlc_ssid_t *ssid_elem;
	int tmp, tmp2, mem_needed = 0, type;
	const struct nlattr *inner, *iter;
	uint32 flush = 0, i = 0, num = 0;

	/* Assumption: NUM attribute must come first */
	nla_for_each_attr(iter, data, len, tmp2) {
		type = nla_type(iter);
		switch (type) {
			case GSCAN_ATTRIBUTE_NUM_WL_SSID:
				num = nla_get_u32(iter);
				if (num > MAX_SSID_WHITELIST_NUM) {
					WL_ERR(("Too many WL SSIDs!\n"));
					err = -EINVAL;
					goto exit;
				}
				mem_needed = sizeof(wl_ssid_whitelist_t);
				if (num)
					mem_needed += (num - 1) * sizeof(ssid_info_t);
				ssid_whitelist = (wl_ssid_whitelist_t *)
				        kzalloc(mem_needed, GFP_KERNEL);
				if (ssid_whitelist == NULL) {
					WL_ERR(("%s: Can't malloc %d bytes\n",
					      __FUNCTION__, mem_needed));
					err = -ENOMEM;
					goto exit;
				}
				ssid_whitelist->ssid_count = num;
				break;
			case GSCAN_ATTRIBUTE_WL_SSID_FLUSH:
				flush = nla_get_u32(iter);
				break;
			case GSCAN_ATTRIBUTE_WHITELIST_SSID_ELEM:
				if (!num || !ssid_whitelist) {
					WL_ERR(("num ssid is not set!\n"));
					return -EINVAL;
				}
				if (i >= num) {
					WL_ERR(("CFGs don't seem right!\n"));
					err = -EINVAL;
					goto exit;
				}
				ssid_elem = &ssid_whitelist->ssids[i];
				nla_for_each_nested(inner, iter, tmp) {
					type = nla_type(inner);
					switch (type) {
						case GSCAN_ATTRIBUTE_WHITELIST_SSID:
							memcpy(ssid_elem->SSID,
							  nla_data(inner),
							  DOT11_MAX_SSID_LEN);
							break;
						case GSCAN_ATTRIBUTE_WL_SSID_LEN:
							ssid_elem->SSID_len = (uint8)
							        nla_get_u32(inner);
							break;
					}
				}
				i++;
				break;
			default:
				WL_ERR(("%s: No such attribute %d\n", __FUNCTION__, type));
				break;
		}
	}

	err = dhd_dev_set_whitelist_ssid(bcmcfg_to_prmry_ndev(cfg),
	          ssid_whitelist, mem_needed, flush);
exit:
	kfree(ssid_whitelist);
	return err;
}
#endif /* GSCAN_SUPPORT */

static int wl_cfgvendor_set_rssi_monitor(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0, tmp, type, start = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int8 max_rssi = 0, min_rssi = 0;
	const struct nlattr *iter;

	nla_for_each_attr(iter, data, len, tmp) {
		type = nla_type(iter);
		switch (type) {
			case RSSI_MONITOR_ATTRIBUTE_MAX_RSSI:
				max_rssi = (int8) nla_get_u32(iter);
				break;
			case RSSI_MONITOR_ATTRIBUTE_MIN_RSSI:
				min_rssi = (int8) nla_get_u32(iter);
				break;
			case RSSI_MONITOR_ATTRIBUTE_START:
				start = nla_get_u32(iter);
		}
	}

	if (dhd_dev_set_rssi_monitor_cfg(bcmcfg_to_prmry_ndev(cfg),
	       start, max_rssi, min_rssi) < 0) {
		WL_ERR(("Could not set rssi monitor cfg\n"));
		err = -EINVAL;
	}
	return err;
}

#ifdef RTT_SUPPORT
void
wl_cfgvendor_rtt_evt(void *ctx, void *rtt_data)
{
	struct wireless_dev *wdev = (struct wireless_dev *)ctx;
	struct wiphy *wiphy;
	struct sk_buff *skb;
 	uint32 complete = 0;
	gfp_t kflags;
	rtt_result_t *rtt_result;
	rtt_results_header_t *rtt_header;
	struct list_head *rtt_cache_list;
	struct nlattr *rtt_nl_hdr;
	wiphy = wdev->wiphy;

	WL_DBG(("In\n"));
	/* Push the data to the skb */
	if (!rtt_data) {
		WL_ERR(("rtt_data is NULL\n"));
		return;
	}
	rtt_cache_list = (struct list_head *)rtt_data;
	kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;
	if (list_empty(rtt_cache_list)) {
		skb = cfg80211_vendor_event_alloc(wiphy, 100, GOOGLE_RTT_COMPLETE_EVENT, kflags);
		if (!skb) {
			WL_ERR(("skb alloc failed"));
			return;
		}
		complete = 1;
		nla_put_u32(skb, RTT_ATTRIBUTE_RESULTS_COMPLETE, complete);
		cfg80211_vendor_event(skb, kflags);
		return;
	}
	list_for_each_entry(rtt_header, rtt_cache_list, list) {
		/* Alloc the SKB for vendor_event */
		skb = cfg80211_vendor_event_alloc(wiphy, rtt_header->result_tot_len + 100,
			GOOGLE_RTT_COMPLETE_EVENT, kflags);
		if (!skb) {
			WL_ERR(("skb alloc failed"));
			return;
		}
		if (list_is_last(&rtt_header->list, rtt_cache_list)) {
			complete = 1;
		}
		nla_put_u32(skb, RTT_ATTRIBUTE_RESULTS_COMPLETE, complete);
		rtt_nl_hdr = nla_nest_start(skb, RTT_ATTRIBUTE_RESULTS_PER_TARGET);
		if (!rtt_nl_hdr) {
			WL_ERR(("rtt_nl_hdr is NULL\n"));
			break;
		}
		nla_put(skb, RTT_ATTRIBUTE_TARGET_MAC, ETHER_ADDR_LEN, &rtt_header->peer_mac);
		nla_put_u32(skb, RTT_ATTRIBUTE_RESULT_CNT, rtt_header->result_cnt);
		list_for_each_entry(rtt_result, &rtt_header->result_list, list) {
			nla_put(skb, RTT_ATTRIBUTE_RESULT,
				rtt_result->report_len, &rtt_result->report);
		}
		nla_nest_end(skb, rtt_nl_hdr);
		cfg80211_vendor_event(skb, kflags);
	}
}

static int
wl_cfgvendor_rtt_set_config(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len) {
	int err = 0, rem, rem1, rem2, type;
	int target_cnt;
	rtt_config_params_t rtt_param;
	rtt_target_info_t* rtt_target = NULL;
	const struct nlattr *iter, *iter1, *iter2;
	int8 eabuf[ETHER_ADDR_STR_LEN];
	int8 chanbuf[CHANSPEC_STR_LEN];
	int32 feature_set = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	rtt_capabilities_t capability;
	feature_set = dhd_dev_get_feature_set(bcmcfg_to_prmry_ndev(cfg));

	WL_DBG(("In\n"));
	err = dhd_dev_rtt_register_noti_callback(wdev->netdev, wdev, wl_cfgvendor_rtt_evt);
	if (err < 0) {
		WL_ERR(("failed to register rtt_noti_callback\n"));
		goto exit;
	}
	err = dhd_dev_rtt_capability(bcmcfg_to_prmry_ndev(cfg), &capability);
	if (err < 0) {
		WL_ERR(("failed to get the capability\n"));
		goto exit;
	}

	memset(&rtt_param, 0, sizeof(rtt_param));
	if (len <= 0) {
		err = BCME_ERROR;
		goto exit;
	}
	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
		case RTT_ATTRIBUTE_TARGET_CNT:
			target_cnt = nla_get_u8(iter);
			if ((target_cnt <= 0) ||
			    (target_cnt > RTT_MAX_TARGET_CNT)) {
				WL_ERR(("target_cnt is not valid: %d",
					target_cnt));
				err = BCME_RANGE;
				goto exit;
			}
			rtt_param.rtt_target_cnt = target_cnt;
			rtt_param.target_info = kzalloc(TARGET_INFO_SIZE(target_cnt), GFP_KERNEL);
			if (rtt_param.target_info == NULL) {
				WL_ERR(("failed to allocate target info for (%d)\n", target_cnt));
				err = BCME_NOMEM;
				goto exit;
			}
			break;
		case RTT_ATTRIBUTE_TARGET_INFO:
			/* Added this variable for safe check to avoid crash
			 * incase the caller did not respect the order
			 */
			if (!rtt_param.target_info) {
				err = BCME_NOMEM;
				goto exit;
			}
			rtt_target = rtt_param.target_info;
			nla_for_each_nested(iter1, iter, rem1) {
				nla_for_each_nested(iter2, iter1, rem2) {
					type = nla_type(iter2);
					switch (type) {
					case RTT_ATTRIBUTE_TARGET_MAC:
						memcpy(&rtt_target->addr, nla_data(iter2),
							ETHER_ADDR_LEN);
						break;
					case RTT_ATTRIBUTE_TARGET_TYPE:
						rtt_target->type = nla_get_u8(iter2);
						if (rtt_target->type == RTT_INVALID ||
							(rtt_target->type == RTT_ONE_WAY &&
							!capability.rtt_one_sided_supported)) {
							WL_ERR(("doesn't support RTT type"
								" : %d\n",
								rtt_target->type));
							err = -EINVAL;
							goto exit;
						}
						break;
					case RTT_ATTRIBUTE_TARGET_PEER:
						rtt_target->peer = nla_get_u8(iter2);
						break;
					case RTT_ATTRIBUTE_TARGET_CHAN:
						memcpy(&rtt_target->channel, nla_data(iter2),
							sizeof(rtt_target->channel));
						break;
					case RTT_ATTRIBUTE_TARGET_PERIOD:
						rtt_target->burst_period = nla_get_u32(iter2);
						if (rtt_target->burst_period < 32) {
							rtt_target->burst_period *= 100; /* 100 ms unit */
						} else {
							WL_ERR(("%d value must in (0-31)\n", rtt_target->burst_period));
							err = EINVAL;
							goto exit;
						}
						break;
					case RTT_ATTRIBUTE_TARGET_NUM_BURST:
						rtt_target->num_burst = nla_get_u32(iter2);
						if (rtt_target->num_burst > 16) {
							WL_ERR(("%d value must in (0-15)\n",
								rtt_target->num_burst));
							err = -EINVAL;
							goto exit;
						}
						rtt_target->num_burst = BIT(rtt_target->num_burst);
						break;
					case RTT_ATTRIBUTE_TARGET_NUM_FTM_BURST:
						rtt_target->num_frames_per_burst =
						nla_get_u32(iter2);
						break;
					case RTT_ATTRIBUTE_TARGET_NUM_RETRY_FTM:
						rtt_target->num_retries_per_ftm =
						nla_get_u32(iter2);
						break;
					case RTT_ATTRIBUTE_TARGET_NUM_RETRY_FTMR:
						rtt_target->num_retries_per_ftmr =
						nla_get_u32(iter2);
						if (rtt_target->num_retries_per_ftmr > 3) {
							WL_ERR(("%d value must in (0-3)\n",
								rtt_target->num_retries_per_ftmr));
							err = -EINVAL;
							goto exit;
						}
						break;
					case RTT_ATTRIBUTE_TARGET_LCI:
						rtt_target->LCI_request = nla_get_u8(iter2);
						break;
					case RTT_ATTRIBUTE_TARGET_LCR:
						rtt_target->LCI_request = nla_get_u8(iter2);
						break;
					case RTT_ATTRIBUTE_TARGET_BURST_DURATION:
						if ((nla_get_u32(iter2) > 1 &&
							nla_get_u32(iter2) < 12)) {
							rtt_target->burst_duration =
							dhd_rtt_idx_to_burst_duration(nla_get_u32(iter2));
						} else if (nla_get_u32(iter2) == 15) {
							/* use default value */
							rtt_target->burst_duration = 128; //ms
						} else {
							WL_ERR(("%d value must in (2-11) or 15\n",
								nla_get_u32(iter2)));
							err = -EINVAL;
							goto exit;
						}
						break;
					case RTT_ATTRIBUTE_TARGET_BW:
						rtt_target->bw = nla_get_u8(iter2);
						break;
					case RTT_ATTRIBUTE_TARGET_PREAMBLE:
						rtt_target->preamble = nla_get_u8(iter2);
						break;
					}
				}
				/* convert to chanspec value */
				rtt_target->chanspec =
					dhd_rtt_convert_to_chspec(rtt_target->channel);
				if (rtt_target->chanspec == 0) {
					WL_ERR(("Channel is not valid \n"));
					err = -EINVAL;
					goto exit;
				}
				WL_INFORM(("Target addr %s, Channel : %s for RTT \n",
					bcm_ether_ntoa((const struct ether_addr *)&rtt_target->addr,
					eabuf),
					wf_chspec_ntoa(rtt_target->chanspec, chanbuf)));
				rtt_target++;
			}
			break;
		}
	}
	WL_DBG(("leave :target_cnt : %d\n", rtt_param.rtt_target_cnt));
	if (dhd_dev_rtt_set_cfg(bcmcfg_to_prmry_ndev(cfg), &rtt_param) < 0) {
		WL_ERR(("Could not set RTT configuration\n"));
		err = -EINVAL;
	}
exit:
	/* free the target info list */
	kfree(rtt_param.target_info);
	rtt_param.target_info = NULL;
	return err;
}

static int
wl_cfgvendor_rtt_cancel_config(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	int err = 0, rem, type, target_cnt = 0;
	int target_idx = 0;
	const struct nlattr *iter;
	struct ether_addr *mac_list = NULL;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);

	if (len <= 0) {
		err = -EINVAL;
		goto exit;
	}
	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
		case RTT_ATTRIBUTE_TARGET_CNT:
			if (mac_list != NULL) {
				WL_ERR(("mac_list is not NULL\n"));
				err = -EINVAL;
				goto exit;
			}
			target_cnt = nla_get_u8(iter);
			if ((target_cnt > 0) &&
			    (target_cnt < RTT_MAX_TARGET_CNT)) {
				mac_list = (struct ether_addr *)kzalloc(target_cnt * ETHER_ADDR_LEN,
					GFP_KERNEL);
				if (mac_list == NULL) {
					WL_ERR(("failed to allocate mem for mac list\n"));
					err = -EINVAL;
					goto exit;
				}
			} else {
				/* cancel the current whole RTT process */
				goto cancel;
			}
			break;
		case RTT_ATTRIBUTE_TARGET_MAC:
			if (!mac_list) {
				err = -EINVAL;
				goto exit;
			}

			if (target_idx >= target_cnt) {
				err = -EINVAL;
				goto exit;
			}

			if (nla_len(iter) != ETHER_ADDR_LEN) {
				err = -EINVAL;
				goto exit;
			}

			memcpy(&mac_list[target_idx], nla_data(iter),
			       ETHER_ADDR_LEN);
			target_idx++;
			break;

		default:
			err = -EINVAL;
			goto exit;
		}
	}
cancel:
	if (dhd_dev_rtt_cancel_cfg(bcmcfg_to_prmry_ndev(cfg), mac_list, target_cnt) < 0) {
		WL_ERR(("Could not cancel RTT configuration\n"));
		err = -EINVAL;
	}

exit:
	if (mac_list) {
		kfree(mac_list);
	}
	return err;
}

static int
wl_cfgvendor_rtt_get_capability(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	rtt_capabilities_t capability;

	err = dhd_dev_rtt_capability(bcmcfg_to_prmry_ndev(cfg), &capability);
	if (unlikely(err)) {
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));
		goto exit;
	}
	err =  wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        &capability, sizeof(capability));

	if (unlikely(err)) {
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));
	}
exit:
	return err;
}

static int
get_responder_info(struct bcm_cfg80211 *cfg,
	struct wifi_rtt_responder *responder_info)
{
	int err = 0;
	rtt_capabilities_t capability;

	err = dhd_dev_rtt_capability(bcmcfg_to_prmry_ndev(cfg), &capability);
	if (unlikely(err)) {
		WL_ERR(("Could not get responder capability:%d \n", err));
		return err;
	}
	if (capability.preamble_support & RTT_PREAMBLE_VHT) {
		responder_info->preamble |= RTT_PREAMBLE_VHT;
	}
	if (capability.preamble_support & RTT_PREAMBLE_HT) {
		responder_info->preamble |= RTT_PREAMBLE_HT;
	}
	err = dhd_dev_rtt_avail_channel(bcmcfg_to_prmry_ndev(cfg), &(responder_info->channel));
	if (unlikely(err)) {
		WL_ERR(("Could not get available channel:%d \n", err));
		return err;
	}
	return err;
}

static int
wl_cfgvendor_rtt_get_responder_info(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	wifi_rtt_responder_t responder_info;

	WL_DBG(("Recv -get_avail_ch command \n"));

	memset(&responder_info, 0, sizeof(responder_info));
	err = get_responder_info(cfg, &responder_info);
	if (unlikely(err)) {
		WL_ERR(("Failed to get responder info:%d \n", err));
		return err;
	}

	err =  wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        &responder_info, sizeof(responder_info));

	if (unlikely(err)) {
		WL_ERR(("Vendor cmd reply for -get_avail_ch failed ret:%d \n", err));
	}
	return err;
}

static int
wl_cfgvendor_rtt_set_responder(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	wifi_rtt_responder_t responder_info;

	WL_DBG(("Recv rtt -enable_resp cmd.\n"));

	memset(&responder_info, 0, sizeof(responder_info));
	err = dhd_dev_rtt_enable_responder(bcmcfg_to_prmry_ndev(cfg), &responder_info.channel);
	if (unlikely(err)) {
		WL_ERR(("Could not enable responder ret:%d \n", err));
		/* Continue further to get the current responder info.
		 * Responder info comprises of current channel and preamble supported.
		 * Setting responder failed but we can still send current reponder info.
		 * Userspace need this information.
		 */
	}

	err = get_responder_info(cfg, &responder_info);
	if (unlikely(err)) {
		WL_ERR(("Failed to get responder info:%d \n", err));
		return err;
	}

	err =  wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        &responder_info, sizeof(responder_info));

	if (unlikely(err)) {
		WL_ERR(("Vendor cmd reply for -enable_resp failed ret:%d \n", err));
	}
	return err;
}


static int
wl_cfgvendor_rtt_cancel_responder(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);

	WL_DBG(("Recv rtt -cancel_resp cmd \n"));

	err = dhd_dev_rtt_cancel_responder(bcmcfg_to_prmry_ndev(cfg));
	if (unlikely(err)) {
		WL_ERR(("Vendor cmd -cancel_resp failed ret:%d \n", err));
	}
	return err;
}
#endif /* RTT_SUPPORT */

#ifdef LINKSTAT_SUPPORT
#define NUM_RATE 32
#define NUM_PEER 1
#define NUM_CHAN 11
static int wl_cfgvendor_lstats_get_info(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	static char iovar_buf[WLC_IOCTL_MAXLEN];
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int err = 0;
	wifi_iface_stat *iface;
	wifi_radio_stat *radio;
	wl_wme_cnt_t *wl_wme_cnt;
	wl_cnt_t *wl_cnt;
	char *output;

	WL_INFORM(("%s: Enter \n", __func__));

	bzero(cfg->ioctl_buf, WLC_IOCTL_MAXLEN);
	bzero(iovar_buf, WLC_IOCTL_MAXLEN);

	output = cfg->ioctl_buf;
	radio = (wifi_radio_stat *)output;

	err = wldev_iovar_getbuf(bcmcfg_to_prmry_ndev(cfg), "radiostat", NULL, 0,
		iovar_buf, WLC_IOCTL_MAXLEN, NULL);
	if (unlikely(err)) {
		WL_ERR(("error (%d) - size = %zu\n", err, sizeof(wifi_radio_stat)));
		return err;
	}
	memcpy(output, iovar_buf, sizeof(wifi_radio_stat));

	radio->num_channels = NUM_CHAN;
	output += sizeof(wifi_radio_stat);
	output += (NUM_CHAN*sizeof(wifi_channel_stat));

	err = wldev_iovar_getbuf(bcmcfg_to_prmry_ndev(cfg), "wme_counters", NULL, 0,
		iovar_buf, WLC_IOCTL_MAXLEN, NULL);
	if (unlikely(err)) {
		WL_ERR(("error (%d)\n", err));
		return err;
	}
	wl_wme_cnt = (wl_wme_cnt_t *)iovar_buf;
	iface = (wifi_iface_stat *)output;

	iface->ac[WIFI_AC_VO].ac = WIFI_AC_VO;
	iface->ac[WIFI_AC_VO].tx_mpdu = wl_wme_cnt->tx[AC_VO].packets;
	iface->ac[WIFI_AC_VO].rx_mpdu = wl_wme_cnt->rx[AC_VO].packets;
	iface->ac[WIFI_AC_VO].mpdu_lost = wl_wme_cnt->tx_failed[WIFI_AC_VO].packets;

	iface->ac[WIFI_AC_VI].ac = WIFI_AC_VI;
	iface->ac[WIFI_AC_VI].tx_mpdu = wl_wme_cnt->tx[AC_VI].packets;
	iface->ac[WIFI_AC_VI].rx_mpdu = wl_wme_cnt->rx[AC_VI].packets;
	iface->ac[WIFI_AC_VI].mpdu_lost = wl_wme_cnt->tx_failed[WIFI_AC_VI].packets;

	iface->ac[WIFI_AC_BE].ac = WIFI_AC_BE;
	iface->ac[WIFI_AC_BE].tx_mpdu = wl_wme_cnt->tx[AC_BE].packets;
	iface->ac[WIFI_AC_BE].rx_mpdu = wl_wme_cnt->rx[AC_BE].packets;
	iface->ac[WIFI_AC_BE].mpdu_lost = wl_wme_cnt->tx_failed[WIFI_AC_BE].packets;

	iface->ac[WIFI_AC_BK].ac = WIFI_AC_BK;
	iface->ac[WIFI_AC_BK].tx_mpdu = wl_wme_cnt->tx[AC_BK].packets;
	iface->ac[WIFI_AC_BK].rx_mpdu = wl_wme_cnt->rx[AC_BK].packets;
	iface->ac[WIFI_AC_BK].mpdu_lost = wl_wme_cnt->tx_failed[WIFI_AC_BK].packets;
	bzero(iovar_buf, WLC_IOCTL_MAXLEN);

	err = wldev_iovar_getbuf(bcmcfg_to_prmry_ndev(cfg), "counters", NULL, 0,
		iovar_buf, WLC_IOCTL_MAXLEN, NULL);
	if (unlikely(err)) {
		WL_ERR(("error (%d) - size = %zu\n", err, sizeof(wl_cnt_t)));
		return err;
	}
	wl_cnt = (wl_cnt_t *)iovar_buf;
	iface->ac[WIFI_AC_BE].retries = wl_cnt->txretry;
	iface->beacon_rx = wl_cnt->rxbeaconmbss;

	err = wldev_get_rssi(bcmcfg_to_prmry_ndev(cfg), &iface->rssi_mgmt);
	if (unlikely(err)) {
		WL_ERR(("get_rssi error (%d)\n", err));
		return err;
	}

	iface->num_peers = NUM_PEER;
	iface->peer_info->num_rate = NUM_RATE;

	bzero(iovar_buf, WLC_IOCTL_MAXLEN);
	output = (char *)iface + sizeof(wifi_iface_stat) + NUM_PEER*sizeof(wifi_peer_info);

	err = wldev_iovar_getbuf(bcmcfg_to_prmry_ndev(cfg), "ratestat", NULL, 0,
		iovar_buf, WLC_IOCTL_MAXLEN, NULL);
	if (unlikely(err)) {
		WL_ERR(("error (%d) - size = %zu\n", err, NUM_RATE*sizeof(wifi_rate_stat)));
		return err;
	}
	memcpy(output, iovar_buf, NUM_RATE*sizeof(wifi_rate_stat));

	err =  wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
		cfg->ioctl_buf, sizeof(wifi_radio_stat)+NUM_CHAN*sizeof(wifi_channel_stat)+
		sizeof(wifi_iface_stat)+NUM_PEER*sizeof(wifi_peer_info)+
		NUM_RATE*sizeof(wifi_rate_stat));
	if (unlikely(err))
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));

	return err;
}
#endif /* LINKSTAT_SUPPORT */

static int wl_cfgvendor_set_country(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = BCME_ERROR, rem, type;
	char country_code[WLC_CNTRY_BUF_SZ] = {0};
	const struct nlattr *iter;

	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case ANDR_WIFI_ATTRIBUTE_COUNTRY:
				memcpy(country_code, nla_data(iter),
					MIN(nla_len(iter), WLC_CNTRY_BUF_SZ));
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				return err;
		}
	}

	err = wldev_set_country(wdev->netdev, country_code, true, true);
	if (err < 0) {
		WL_ERR(("Set country failed ret:%d\n", err));
	}

	return err;
}

static int wl_cfgvendor_dbg_start_logging(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret = BCME_OK, rem, type;
	char ring_name[DBGRING_NAME_MAX] = {0};
	int log_level = 0, flags = 0, time_intval = 0, threshold = 0;
	const struct nlattr *iter;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;
	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case DEBUG_ATTRIBUTE_RING_NAME:
				strncpy(ring_name, nla_data(iter),
					MIN(sizeof(ring_name) -1, nla_len(iter)));
				break;
			case DEBUG_ATTRIBUTE_LOG_LEVEL:
				log_level = nla_get_u32(iter);
				break;
			case DEBUG_ATTRIBUTE_RING_FLAGS:
				flags = nla_get_u32(iter);
				break;
			case DEBUG_ATTRIBUTE_LOG_TIME_INTVAL:
				time_intval = nla_get_u32(iter);
				break;
			case DEBUG_ATTRIBUTE_LOG_MIN_DATA_SIZE:
				threshold = nla_get_u32(iter);
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_BADADDR;
				goto exit;
		}
	}

	ret = dhd_os_start_logging(dhd_pub, ring_name, log_level, flags, time_intval, threshold);
	if (ret < 0) {
		WL_ERR(("start_logging is failed ret: %d\n", ret));
	}
exit:
	return ret;
}

static int wl_cfgvendor_dbg_reset_logging(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret = BCME_OK;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;

	ret = dhd_os_reset_logging(dhd_pub);
	if (ret < 0) {
		WL_ERR(("reset logging is failed ret: %d\n", ret));
	}

	return ret;
}

static int wl_cfgvendor_dbg_trigger_mem_dump(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret = BCME_OK;
	uint32 alloc_len;
	struct sk_buff *skb;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);

	ret = dhd_os_socram_dump(bcmcfg_to_prmry_ndev(cfg), &alloc_len);
	if (ret) {
		WL_ERR(("failed to call dhd_os_socram_dump : %d\n", ret));
		goto exit;
	}
	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, 100);
	if (!skb) {
		WL_ERR(("skb allocation is failed\n"));
		ret = BCME_NOMEM;
		goto exit;
	}
	nla_put_u32(skb, DEBUG_ATTRIBUTE_FW_DUMP_LEN, alloc_len);

	ret = cfg80211_vendor_cmd_reply(skb);

	if (ret) {
		WL_ERR(("Vendor Command reply failed ret:%d \n", ret));
	}

exit:
	return ret;
}

static int wl_cfgvendor_dbg_get_mem_dump(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	int ret = BCME_OK, rem, type;
	int buf_len = 0;
	void __user *user_buf = NULL;
	const struct nlattr *iter;
	char *mem_buf = NULL;
	struct sk_buff *skb;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);

	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case DEBUG_ATTRIBUTE_FW_DUMP_LEN:
				/* Check if the iter is valid and
				 * buffer length is not already initialized.
				 */
				if ((nla_len(iter) == sizeof(uint32)) &&
					!buf_len) {
					buf_len = nla_get_u32(iter);
					if (buf_len <= 0) {
						ret = BCME_ERROR;
						goto exit;
					}
				} else {
					ret = BCME_ERROR;
					goto exit;
				}
				break;
			case DEBUG_ATTRIBUTE_FW_DUMP_DATA:
				if (nla_len(iter) != sizeof(uint64)) {
					WL_ERR(("Invalid len\n"));
					ret = BCME_ERROR;
					goto exit;
				}
				user_buf =
				(void __user *)(unsigned long)nla_get_u64(iter);
				if (!user_buf) {
					ret = BCME_ERROR;
					goto exit;
				}
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_ERROR;
				goto exit;
		}
	}
	if (buf_len > 0 && user_buf) {
		mem_buf = vmalloc(buf_len);
		if (!mem_buf) {
			WL_ERR(("failed to allocate mem_buf with size : %d\n", buf_len));
			ret = BCME_NOMEM;
			goto exit;
		}
		ret = dhd_os_get_socram_dump(bcmcfg_to_prmry_ndev(cfg), &mem_buf, &buf_len);
		if (ret) {
			WL_ERR(("failed to get_socram_dump : %d\n", ret));
			goto free_mem;
		}
		ret = copy_to_user(user_buf, mem_buf, buf_len);
		if (ret) {
			WL_ERR(("failed to copy memdump into user buffer : %d\n", ret));
			goto free_mem;
		}
		/* Alloc the SKB for vendor_event */
		skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, 100);
		if (!skb) {
			WL_ERR(("skb allocation is failed\n"));
			ret = BCME_NOMEM;
			goto free_mem;
		}
		/* Indicate the memdump is succesfully copied */
		nla_put(skb, DEBUG_ATTRIBUTE_FW_DUMP_DATA, sizeof(ret), &ret);

		ret = cfg80211_vendor_cmd_reply(skb);

		if (ret) {
			WL_ERR(("Vendor Command reply failed ret:%d \n", ret));
		}
	}

free_mem:
	vfree(mem_buf);
exit:
	return ret;
}

static int wl_cfgvendor_dbg_get_version(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	int ret = BCME_OK, rem, type;
	int buf_len = 1024;
	bool dhd_ver = FALSE;
	char *buf_ptr;
	const struct nlattr *iter;
	gfp_t kflags;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;
	buf_ptr = kzalloc(buf_len, kflags);
	if (!buf_ptr) {
		WL_ERR(("failed to allocate the buffer for version n"));
		ret = BCME_NOMEM;
		goto exit;
	}
	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case DEBUG_ATTRIBUTE_GET_DRIVER:
				dhd_ver = TRUE;
				break;
			case DEBUG_ATTRIBUTE_GET_FW:
				dhd_ver = FALSE;
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_ERROR;
				goto exit;
		}
	}
	ret = dhd_os_get_version(bcmcfg_to_prmry_ndev(cfg), dhd_ver, &buf_ptr, buf_len);
	if (ret < 0) {
		WL_ERR(("failed to get the version %d\n", ret));
		goto exit;
	}
	ret = wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        buf_ptr, strlen(buf_ptr));
exit:
	kfree(buf_ptr);
	return ret;
}

static int wl_cfgvendor_dbg_get_ring_status(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret = BCME_OK;
	int ring_id, i;
	int ring_cnt;
	struct sk_buff *skb;
	dhd_dbg_ring_status_t dbg_ring_status[DEBUG_RING_ID_MAX];
	dhd_dbg_ring_status_t ring_status;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;
	memset(dbg_ring_status, 0, DBG_RING_STATUS_SIZE * DEBUG_RING_ID_MAX);
	ring_cnt = 0;
	for (ring_id = DEBUG_RING_ID_INVALID + 1; ring_id < DEBUG_RING_ID_MAX; ring_id++) {
		ret = dhd_os_get_ring_status(dhd_pub, ring_id, &ring_status);
		if (ret == BCME_NOTFOUND) {
			WL_DBG(("The ring (%d) is not found \n", ring_id));
		} else if (ret == BCME_OK) {
			dbg_ring_status[ring_cnt++] = ring_status;
		}
	}
	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy,
		(DBG_RING_STATUS_SIZE * ring_cnt) + 100);
	if (!skb) {
		WL_ERR(("skb allocation is failed\n"));
		ret = BCME_NOMEM;
		goto exit;
	}

	nla_put_u32(skb, DEBUG_ATTRIBUTE_RING_NUM, ring_cnt);
	for (i = 0; i < ring_cnt; i++) {
		nla_put(skb, DEBUG_ATTRIBUTE_RING_STATUS, DBG_RING_STATUS_SIZE,
				&dbg_ring_status[i]);
	}
	ret = cfg80211_vendor_cmd_reply(skb);

	if (ret) {
		WL_ERR(("Vendor Command reply failed ret:%d \n", ret));
	}
exit:
	return ret;
}

static int wl_cfgvendor_dbg_get_ring_data(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret = BCME_OK, rem, type;
	char ring_name[DBGRING_NAME_MAX] = {0};
	const struct nlattr *iter;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;

	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case DEBUG_ATTRIBUTE_RING_NAME:
				strncpy(ring_name, nla_data(iter),
					MIN(sizeof(ring_name) -1, nla_len(iter)));
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				return ret;
		}
	}

	ret = dhd_os_trigger_get_ring_data(dhd_pub, ring_name);
	if (ret < 0) {
		WL_ERR(("trigger_get_data failed ret:%d\n", ret));
	}

	return ret;
}

static int wl_cfgvendor_dbg_get_feature(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret = BCME_OK;
	u32 supported_features;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;

	ret = dhd_os_dbg_get_feature(dhd_pub, &supported_features);
	if (ret < 0) {
		WL_ERR(("dbg_get_feature failed ret:%d\n", ret));
		goto exit;
	}
	ret = wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        &supported_features, sizeof(supported_features));
exit:
	return ret;
}

#ifdef DBG_PKT_MON
static int wl_cfgvendor_dbg_start_pkt_fate_monitoring(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;
	int ret;

	ret = dhd_os_dbg_attach_pkt_monitor(dhd_pub);
	if (unlikely(ret)) {
		WL_ERR(("failed to start pkt fate monitoring, ret=%d", ret));
	}

	return ret;
}

typedef int (*dbg_mon_get_pkts_t) (dhd_pub_t *dhdp, void __user *user_buf,
	uint16 req_count, uint16 *resp_count);

static int __wl_cfgvendor_dbg_get_pkt_fates(struct wiphy *wiphy,
	const void *data, int len, dbg_mon_get_pkts_t dbg_mon_get_pkts)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;
	struct sk_buff *skb = NULL;
    const struct nlattr *iter;
	void __user *user_buf;
	uint16 req_count, resp_count;
    int ret, tmp, type, mem_needed;

    nla_for_each_attr(iter, data, len, tmp) {
        type = nla_type(iter);
        switch (type) {
            case DEBUG_ATTRIBUTE_PKT_FATE_NUM:
                req_count = nla_get_u32(iter);
                break;
            case DEBUG_ATTRIBUTE_PKT_FATE_DATA:
				user_buf = (void __user *)(unsigned long) nla_get_u64(iter);
                break;
            default:
                WL_ERR(("%s: no such attribute %d\n", __FUNCTION__, type));
                ret = -EINVAL;
                goto exit;
        }
    }

	if (!req_count || !user_buf) {
		WL_ERR(("%s: invalid request, user_buf=%p, req_count=%u\n",
			__FUNCTION__, user_buf, req_count));
		ret = -EINVAL;
		goto exit;
	}

	ret = dbg_mon_get_pkts(dhd_pub, user_buf, req_count, &resp_count);
	if (unlikely(ret)) {
		WL_ERR(("failed to get packets, ret:%d \n", ret));
		goto exit;
	}

	mem_needed = VENDOR_REPLY_OVERHEAD + ATTRIBUTE_U32_LEN;
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, mem_needed);
	if (unlikely(!skb)) {
		WL_ERR(("skb alloc failed"));
		ret = -ENOMEM;
		goto exit;
	}

	nla_put_u32(skb, DEBUG_ATTRIBUTE_PKT_FATE_NUM, resp_count);

	ret = cfg80211_vendor_cmd_reply(skb);
	if (unlikely(ret)) {
		WL_ERR(("vendor Command reply failed ret:%d \n", ret));
	}

exit:
	return ret;
}

static int wl_cfgvendor_dbg_get_tx_pkt_fates(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret;

	ret = __wl_cfgvendor_dbg_get_pkt_fates(wiphy, data, len,
			dhd_os_dbg_monitor_get_tx_pkts);
	if (unlikely(ret)) {
		WL_ERR(("failed to get tx packets, ret:%d \n", ret));
	}

	return ret;
}

static int wl_cfgvendor_dbg_get_rx_pkt_fates(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret;

	ret = __wl_cfgvendor_dbg_get_pkt_fates(wiphy, data, len,
			dhd_os_dbg_monitor_get_rx_pkts);
	if (unlikely(ret)) {
		WL_ERR(("failed to get rx packets, ret:%d \n", ret));
	}

	return ret;
}
#endif /* DBG_PKT_MON */

static void wl_cfgvendor_dbg_ring_send_evt(void *ctx,
	const int ring_id, const void *data, const uint32 len,
	const dhd_dbg_ring_status_t ring_status)
{
	struct net_device *ndev = ctx;
	struct wiphy *wiphy;
	gfp_t kflags;
	struct sk_buff *skb;
	if (!ndev) {
		WL_ERR(("ndev is NULL\n"));
		return;
	}
	kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;
	wiphy = ndev->ieee80211_ptr->wiphy;
	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_event_alloc(wiphy, len + 100,
			GOOGLE_DEBUG_RING_EVENT, kflags);
	if (!skb) {
		WL_ERR(("skb alloc failed"));
		return;
	}
	nla_put(skb, DEBUG_ATTRIBUTE_RING_STATUS, sizeof(ring_status), &ring_status);
	nla_put(skb, DEBUG_ATTRIBUTE_RING_DATA, len, data);
	cfg80211_vendor_event(skb, kflags);
}


static void wl_cfgvendor_dbg_send_urgent_evt(void *ctx, const void *data,
	const uint32 len, const uint32 fw_len)
{
	struct net_device *ndev = ctx;
	struct wiphy *wiphy;
	gfp_t kflags;
	struct sk_buff *skb;
	if (!ndev) {
		WL_ERR(("ndev is NULL\n"));
		return;
	}
	kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;
	wiphy = ndev->ieee80211_ptr->wiphy;
	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_event_alloc(wiphy, len + 100,
			GOOGLE_FW_DUMP_EVENT, kflags);
	if (!skb) {
		WL_ERR(("skb alloc failed"));
		return;
	}
	nla_put_u32(skb, DEBUG_ATTRIBUTE_FW_DUMP_LEN, fw_len);
	nla_put(skb, DEBUG_ATTRIBUTE_RING_DATA, len, data);
	cfg80211_vendor_event(skb, kflags);
}


#if defined(KEEP_ALIVE)
static int wl_cfgvendor_start_mkeep_alive(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	/* max size of IP packet for keep alive */
	const int MKEEP_ALIVE_IP_PKT_MAX = 256;

	int ret = BCME_OK, rem, type;
	u8 mkeep_alive_id = 0;
	u8 *ip_pkt = NULL;
	u16 ip_pkt_len = 0;
	u8 src_mac[ETHER_ADDR_LEN];
	u8 dst_mac[ETHER_ADDR_LEN];
	u32 period_msec = 0;
	const struct nlattr *iter;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;
	gfp_t kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;

	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case MKEEP_ALIVE_ATTRIBUTE_ID:
				mkeep_alive_id = nla_get_u8(iter);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_IP_PKT_LEN:
				ip_pkt_len = nla_get_u16(iter);
				if ( ip_pkt_len > MKEEP_ALIVE_IP_PKT_MAX) {
					ret = BCME_BADARG;
					goto exit;
				}
				break;
			case MKEEP_ALIVE_ATTRIBUTE_IP_PKT:
				ip_pkt = (u8 *)kzalloc(ip_pkt_len, kflags);
				if (ip_pkt == NULL) {
					ret = BCME_NOMEM;
					WL_ERR(("Failed to allocate mem for ip packet\n"));
					goto exit;
				}
				memcpy(ip_pkt, (u8*)nla_data(iter), ip_pkt_len);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_SRC_MAC_ADDR:
				memcpy(src_mac, nla_data(iter), ETHER_ADDR_LEN);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_DST_MAC_ADDR:
				memcpy(dst_mac, nla_data(iter), ETHER_ADDR_LEN);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_PERIOD_MSEC:
				period_msec = nla_get_u32(iter);
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_BADARG;
				goto exit;
		}
	}

	ret = dhd_dev_start_mkeep_alive(dhd_pub, mkeep_alive_id, ip_pkt, ip_pkt_len, src_mac,
				        dst_mac, period_msec);
	if (ret < 0) {
		WL_ERR(("start_mkeep_alive is failed ret: %d\n", ret));
	}

exit:
	if (ip_pkt) {
		kfree(ip_pkt);
	}

	return ret;
}

static int wl_cfgvendor_stop_mkeep_alive(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	int ret = BCME_OK, rem, type;
	u8 mkeep_alive_id = 0;
	const struct nlattr *iter;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;

	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case MKEEP_ALIVE_ATTRIBUTE_ID:
				mkeep_alive_id = nla_get_u8(iter);
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_BADARG;
				break;
		}
	}

	ret = dhd_dev_stop_mkeep_alive(dhd_pub, mkeep_alive_id);
	if (ret < 0) {
		WL_ERR(("stop_mkeep_alive is failed ret: %d\n", ret));
	}

	return ret;
}
#endif /* defined(KEEP_ALIVE) */

#if defined(PKT_FILTER_SUPPORT) && defined(APF)
static int
wl_cfgvendor_apf_get_capabilities(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct net_device *ndev = wdev_to_ndev(wdev);
	struct sk_buff *skb;
	int ret, ver, max_len, mem_needed;

	/* APF version */
	ver = 0;
	ret = dhd_dev_apf_get_version(ndev, &ver);
	if (unlikely(ret)) {
		WL_ERR(("APF get version failed, ret=%d\n", ret));
		return ret;
	}

	/* APF memory size limit */
	max_len = 0;
	ret = dhd_dev_apf_get_max_len(ndev, &max_len);
	if (unlikely(ret)) {
		WL_ERR(("APF get maximum length failed, ret=%d\n", ret));
		return ret;
	}

	mem_needed = VENDOR_REPLY_OVERHEAD + (ATTRIBUTE_U32_LEN * 2);

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, mem_needed);
	if (unlikely(!skb)) {
		WL_ERR(("%s: can't allocate %d bytes\n", __FUNCTION__, mem_needed));
		return -ENOMEM;
	}

	nla_put_u32(skb, APF_ATTRIBUTE_VERSION, ver);
	nla_put_u32(skb, APF_ATTRIBUTE_MAX_LEN, max_len);

	ret = cfg80211_vendor_cmd_reply(skb);
	if (unlikely(ret)) {
		WL_ERR(("vendor command reply failed, ret=%d\n", ret));
	}

	return ret;
}

static int
wl_cfgvendor_apf_set_filter(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	struct net_device *ndev = wdev_to_ndev(wdev);
	const struct nlattr *iter;
	u8 *program = NULL;
	u32 program_len = 0;
	int ret, tmp, type;
	gfp_t kflags;

	if (len <= 0) {
		WL_ERR((" Invalid len : %d\n", len));
		ret = -EINVAL;
		goto exit;
	}
	nla_for_each_attr(iter, data, len, tmp) {
		type = nla_type(iter);
		switch (type) {
			case APF_ATTRIBUTE_PROGRAM_LEN:
				/* check if the iter is valid and program
				 * length is not already initialized.
				 */
				if (nla_len(iter) == sizeof(uint32) &&
				   !program_len) {
					program_len = nla_get_u32(iter);
				} else {
					ret = -EINVAL;
					goto exit;
				}

				if (program_len >
					WL_APF_PROGRAM_MAX_SIZE) {
					WL_ERR(("program len is more "));
					WL_ERR(("than expected len\n"));
					ret = -EINVAL;
					goto exit;
				}
				if (unlikely(!program_len)) {
					WL_ERR(("zero program length\n"));
					ret = -EINVAL;
					goto exit;
				}
				break;
			case APF_ATTRIBUTE_PROGRAM:
				if (unlikely(!program_len)) {
					WL_ERR(("program len is not set\n"));
					ret = -EINVAL;
					goto exit;
				}
				if (nla_len(iter) != program_len) {
					WL_ERR(("program_len is not same\n"));
					ret = -EINVAL;
					goto exit;
				}

				kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;
				program = kzalloc(program_len, kflags);
				if (unlikely(!program)) {
					WL_ERR(("%s: can't allocate %d bytes\n",
					      __FUNCTION__, program_len));
					ret = -ENOMEM;
					goto exit;
				}
				memcpy(program, (u8*)nla_data(iter), program_len);
				break;
			default:
				WL_ERR(("%s: no such attribute %d\n", __FUNCTION__, type));
				ret = -EINVAL;
				goto exit;
		}
	}

	ret = dhd_dev_apf_add_filter(ndev, program, program_len);

exit:
	if (program) {
		kfree(program);
	}
	return ret;
}
#endif /* PKT_FILTER_SUPPORT && APF */

#ifdef NDO_CONFIG_SUPPORT
static int wl_cfgvendor_configure_nd_offload(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	const struct nlattr *iter;
	int ret = BCME_OK, rem, type;
	u8 enable;

	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case ANDR_WIFI_ATTRIBUTE_ND_OFFLOAD_VALUE:
				enable = nla_get_u8(iter);
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_BADARG;
				goto exit;
		}
	}

	ret = dhd_dev_ndo_cfg(bcmcfg_to_prmry_ndev(cfg), enable);
	if (ret < 0) {
		WL_ERR(("dhd_dev_ndo_cfg() failed: %d\n", ret));
	}

exit:
	return ret;
}
#endif /* NDO_CONFIG_SUPPORT */


static const struct wiphy_vendor_command wl_vendor_cmds [] = {
#ifdef GSCAN_SUPPORT
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_GET_CAPABILITIES
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_gscan_get_capabilities
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_SET_CONFIG
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_scan_cfg
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_SET_SCAN_CONFIG
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_batch_scan_cfg
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_ENABLE_GSCAN
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_initiate_gscan
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_ENABLE_FULL_SCAN_RESULTS
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_enable_full_scan_result
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_SET_HOTLIST
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_hotlist_cfg
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_SET_SIGNIFICANT_CHANGE_CONFIG
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_significant_change_cfg
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_GET_SCAN_RESULTS
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_gscan_get_batch_results
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_GET_CHANNEL_LIST
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_gscan_get_channel_list
	},
#endif /* GSCAN_SUPPORT */
#ifdef RTT_SUPPORT
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = RTT_SUBCMD_SET_CONFIG
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_rtt_set_config
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = RTT_SUBCMD_CANCEL_CONFIG
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_rtt_cancel_config
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = RTT_SUBCMD_GETCAPABILITY
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_rtt_get_capability
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = RTT_SUBCMD_GETAVAILCHANNEL
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_rtt_get_responder_info
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = RTT_SUBCMD_SET_RESPONDER
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_rtt_set_responder
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = RTT_SUBCMD_CANCEL_RESPONDER
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_rtt_cancel_responder
	},
#endif /* RTT_SUPPORT */
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = ANDR_WIFI_SUBCMD_GET_FEATURE_SET
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_get_feature_set
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = ANDR_WIFI_SUBCMD_GET_FEATURE_SET_MATRIX
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_get_feature_set_matrix
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = ANDR_WIFI_RANDOM_MAC_OUI
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_rand_mac_oui
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = ANDR_WIFI_NODFS_CHANNELS
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_nodfs_flag

	},
#ifdef LINKSTAT_SUPPORT
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = LSTATS_SUBCMD_GET_INFO
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_lstats_get_info
	},
#endif /* LINKSTAT_SUPPORT */
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = ANDR_WIFI_SET_COUNTRY
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_country
	},
#ifdef GSCAN_SUPPORT
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_SET_EPNO_SSID
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_epno_cfg

	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_SUBCMD_SET_SSID_WHITELIST
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_ssid_whitelist

	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_SUBCMD_SET_LAZY_ROAM_PARAMS
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_lazy_roam_cfg

	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_SUBCMD_ENABLE_LAZY_ROAM
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_enable_lazy_roam

	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_SUBCMD_SET_BSSID_PREF
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_bssid_pref

	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_SUBCMD_SET_BSSID_BLACKLIST
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_bssid_blacklist
	},
#ifdef DHD_ANQPO_SUPPORT
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_ANQPO_CONFIG
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_gscan_anqpo_config
	},
#endif /* DHD_ANQPO_SUPPORT */
#endif /* GSCAN_SUPPORT */
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_START_LOGGING
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_start_logging
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_RESET_LOGGING
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_reset_logging
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_TRIGGER_MEM_DUMP
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_trigger_mem_dump
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_MEM_DUMP
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_mem_dump
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_VER
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_version
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_RING_STATUS
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_ring_status
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_RING_DATA
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_ring_data
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_FEATURE
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_feature
	},
#ifdef DBG_PKT_MON
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_START_PKT_FATE_MONITORING
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_start_pkt_fate_monitoring
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_TX_PKT_FATES
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_tx_pkt_fates
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_RX_PKT_FATES
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_rx_pkt_fates
	},
#endif /* DBG_PKT_MON */
#ifdef KEEP_ALIVE
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_OFFLOAD_SUBCMD_START_MKEEP_ALIVE
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_start_mkeep_alive
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_OFFLOAD_SUBCMD_STOP_MKEEP_ALIVE
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_stop_mkeep_alive
	},
#endif /* KEEP_ALIVE */
#if defined(PKT_FILTER_SUPPORT) && defined(APF)
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = APF_SUBCMD_GET_CAPABILITIES
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_apf_get_capabilities
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = APF_SUBCMD_SET_FILTER
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_apf_set_filter
	},
#endif /* PKT_FILTER_SUPPORT && APF */
#ifdef NDO_CONFIG_SUPPORT
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_SUBCMD_CONFIG_ND_OFFLOAD,
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_configure_nd_offload
	},
#endif /* NDO_CONFIG_SUPPORT */
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_SUBCMD_SET_RSSI_MONITOR
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_rssi_monitor
	}
};

static const struct  nl80211_vendor_cmd_info wl_vendor_events [] = {
		{ OUI_BRCM, BRCM_VENDOR_EVENT_UNSPEC },
		{ OUI_BRCM, BRCM_VENDOR_EVENT_PRIV_STR },
		{ OUI_GOOGLE, GOOGLE_GSCAN_SIGNIFICANT_EVENT },
		{ OUI_GOOGLE, GOOGLE_GSCAN_GEOFENCE_FOUND_EVENT },
		{ OUI_GOOGLE, GOOGLE_GSCAN_BATCH_SCAN_EVENT },
		{ OUI_GOOGLE, GOOGLE_SCAN_FULL_RESULTS_EVENT },
		{ OUI_GOOGLE, GOOGLE_RTT_COMPLETE_EVENT },
		{ OUI_GOOGLE, GOOGLE_SCAN_COMPLETE_EVENT },
		{ OUI_GOOGLE, GOOGLE_GSCAN_GEOFENCE_LOST_EVENT },
		{ OUI_GOOGLE, GOOGLE_SCAN_EPNO_EVENT },
		{ OUI_GOOGLE, GOOGLE_DEBUG_RING_EVENT },
		{ OUI_GOOGLE, GOOGLE_FW_DUMP_EVENT },
#ifdef DHD_ANQPO_SUPPORT
		{ OUI_GOOGLE, GOOGLE_PNO_HOTSPOT_FOUND_EVENT },
#endif /* DHD_ANQPO_SUPPORT */
		{ OUI_GOOGLE, GOOGLE_RSSI_MONITOR_EVENT },
		{ OUI_GOOGLE, GOOGLE_MKEEP_ALIVE_EVENT }
};

int wl_cfgvendor_attach(struct wiphy *wiphy, dhd_pub_t *dhd)
{

	WL_INFORM(("Vendor: Register BRCM cfg80211 vendor cmd(0x%x) interface \n",
		NL80211_CMD_VENDOR));

	wiphy->vendor_commands	= wl_vendor_cmds;
	wiphy->n_vendor_commands = ARRAY_SIZE(wl_vendor_cmds);
	wiphy->vendor_events	= wl_vendor_events;
	wiphy->n_vendor_events	= ARRAY_SIZE(wl_vendor_events);
	dhd_os_dbg_register_callback(FW_VERBOSE_RING_ID, wl_cfgvendor_dbg_ring_send_evt);
	dhd_os_dbg_register_callback(FW_EVENT_RING_ID, wl_cfgvendor_dbg_ring_send_evt);
	dhd_os_dbg_register_callback(DHD_EVENT_RING_ID, wl_cfgvendor_dbg_ring_send_evt);
	dhd_os_dbg_register_urgent_notifier(dhd, wl_cfgvendor_dbg_send_urgent_evt);
	return 0;
}

int wl_cfgvendor_detach(struct wiphy *wiphy)
{
	WL_INFORM(("Vendor: Unregister BRCM cfg80211 vendor interface \n"));

	wiphy->vendor_commands  = NULL;
	wiphy->vendor_events    = NULL;
	wiphy->n_vendor_commands = 0;
	wiphy->n_vendor_events  = 0;

	return 0;
}
#endif /* (LINUX_VERSION_CODE > KERNEL_VERSION(3, 13, 0)) || defined(WL_VENDOR_EXT_SUPPORT) */
