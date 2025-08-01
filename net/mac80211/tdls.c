// SPDX-License-Identifier: GPL-2.0-only
/*
 * mac80211 TDLS handling code
 *
 * Copyright 2006-2010	Johannes Berg <johannes@sipsolutions.net>
 * Copyright 2014, Intel Corporation
 * Copyright 2014  Intel Mobile Communications GmbH
 * Copyright 2015 - 2016 Intel Deutschland GmbH
 * Copyright (C) 2019, 2021-2024 Intel Corporation
 */

#include <linux/ieee80211.h>
#include <linux/log2.h>
#include <net/cfg80211.h>
#include <linux/rtnetlink.h>
#include "ieee80211_i.h"
#include "driver-ops.h"
#include "rate.h"
#include "wme.h"

/* give usermode some time for retries in setting up the TDLS session */
#define TDLS_PEER_SETUP_TIMEOUT	(15 * HZ)

void ieee80211_tdls_peer_del_work(struct wiphy *wiphy, struct wiphy_work *wk)
{
	struct ieee80211_sub_if_data *sdata;
	struct ieee80211_local *local;

	sdata = container_of(wk, struct ieee80211_sub_if_data,
			     u.mgd.tdls_peer_del_work.work);
	local = sdata->local;

	lockdep_assert_wiphy(local->hw.wiphy);

	if (!is_zero_ether_addr(sdata->u.mgd.tdls_peer)) {
		tdls_dbg(sdata, "TDLS del peer %pM\n", sdata->u.mgd.tdls_peer);
		sta_info_destroy_addr(sdata, sdata->u.mgd.tdls_peer);
		eth_zero_addr(sdata->u.mgd.tdls_peer);
	}
}

static void ieee80211_tdls_add_ext_capab(struct ieee80211_link_data *link,
					 struct sk_buff *skb)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	bool chan_switch = local->hw.wiphy->features &
			   NL80211_FEATURE_TDLS_CHANNEL_SWITCH;
	bool wider_band = ieee80211_hw_check(&local->hw, TDLS_WIDER_BW) &&
			  !ifmgd->tdls_wider_bw_prohibited;
	bool buffer_sta = ieee80211_hw_check(&local->hw,
					     SUPPORTS_TDLS_BUFFER_STA);
	struct ieee80211_supported_band *sband = ieee80211_get_link_sband(link);
	bool vht = sband && sband->vht_cap.vht_supported;
	u8 *pos = skb_put(skb, 10);

	*pos++ = WLAN_EID_EXT_CAPABILITY;
	*pos++ = 8; /* len */
	*pos++ = 0x0;
	*pos++ = 0x0;
	*pos++ = 0x0;
	*pos++ = (chan_switch ? WLAN_EXT_CAPA4_TDLS_CHAN_SWITCH : 0) |
		 (buffer_sta ? WLAN_EXT_CAPA4_TDLS_BUFFER_STA : 0);
	*pos++ = WLAN_EXT_CAPA5_TDLS_ENABLED;
	*pos++ = 0;
	*pos++ = 0;
	*pos++ = (vht && wider_band) ? WLAN_EXT_CAPA8_TDLS_WIDE_BW_ENABLED : 0;
}

static u8
ieee80211_tdls_add_subband(struct ieee80211_sub_if_data *sdata,
			   struct sk_buff *skb, u16 start, u16 end,
			   u16 spacing)
{
	u8 subband_cnt = 0, ch_cnt = 0;
	struct ieee80211_channel *ch;
	struct cfg80211_chan_def chandef;
	int i, subband_start;
	struct wiphy *wiphy = sdata->local->hw.wiphy;

	for (i = start; i <= end; i += spacing) {
		if (!ch_cnt)
			subband_start = i;

		ch = ieee80211_get_channel(sdata->local->hw.wiphy, i);
		if (ch) {
			/* we will be active on the channel */
			cfg80211_chandef_create(&chandef, ch,
						NL80211_CHAN_NO_HT);
			if (cfg80211_reg_can_beacon_relax(wiphy, &chandef,
							  sdata->wdev.iftype)) {
				ch_cnt++;
				/*
				 * check if the next channel is also part of
				 * this allowed range
				 */
				continue;
			}
		}

		/*
		 * we've reached the end of a range, with allowed channels
		 * found
		 */
		if (ch_cnt) {
			u8 *pos = skb_put(skb, 2);
			*pos++ = ieee80211_frequency_to_channel(subband_start);
			*pos++ = ch_cnt;

			subband_cnt++;
			ch_cnt = 0;
		}
	}

	/* all channels in the requested range are allowed - add them here */
	if (ch_cnt) {
		u8 *pos = skb_put(skb, 2);
		*pos++ = ieee80211_frequency_to_channel(subband_start);
		*pos++ = ch_cnt;

		subband_cnt++;
	}

	return subband_cnt;
}

static void
ieee80211_tdls_add_supp_channels(struct ieee80211_sub_if_data *sdata,
				 struct sk_buff *skb)
{
	/*
	 * Add possible channels for TDLS. These are channels that are allowed
	 * to be active.
	 */
	u8 subband_cnt;
	u8 *pos = skb_put(skb, 2);

	*pos++ = WLAN_EID_SUPPORTED_CHANNELS;

	/*
	 * 5GHz and 2GHz channels numbers can overlap. Ignore this for now, as
	 * this doesn't happen in real world scenarios.
	 */

	/* 2GHz, with 5MHz spacing */
	subband_cnt = ieee80211_tdls_add_subband(sdata, skb, 2412, 2472, 5);

	/* 5GHz, with 20MHz spacing */
	subband_cnt += ieee80211_tdls_add_subband(sdata, skb, 5000, 5825, 20);

	/* length */
	*pos = 2 * subband_cnt;
}

static void ieee80211_tdls_add_oper_classes(struct ieee80211_link_data *link,
					    struct sk_buff *skb)
{
	u8 *pos;
	u8 op_class;

	if (!ieee80211_chandef_to_operating_class(&link->conf->chanreq.oper,
						  &op_class))
		return;

	pos = skb_put(skb, 4);
	*pos++ = WLAN_EID_SUPPORTED_REGULATORY_CLASSES;
	*pos++ = 2; /* len */

	*pos++ = op_class;
	*pos++ = op_class; /* give current operating class as alternate too */
}

static void ieee80211_tdls_add_bss_coex_ie(struct sk_buff *skb)
{
	u8 *pos = skb_put(skb, 3);

	*pos++ = WLAN_EID_BSS_COEX_2040;
	*pos++ = 1; /* len */

	*pos++ = WLAN_BSS_COEX_INFORMATION_REQUEST;
}

static u16 ieee80211_get_tdls_sta_capab(struct ieee80211_link_data *link,
					u16 status_code)
{
	struct ieee80211_supported_band *sband;

	/* The capability will be 0 when sending a failure code */
	if (status_code != 0)
		return 0;

	sband = ieee80211_get_link_sband(link);

	if (sband && sband->band == NL80211_BAND_2GHZ) {
		return WLAN_CAPABILITY_SHORT_SLOT_TIME |
		       WLAN_CAPABILITY_SHORT_PREAMBLE;
	}

	return 0;
}

static void ieee80211_tdls_add_link_ie(struct ieee80211_link_data *link,
				       struct sk_buff *skb, const u8 *peer,
				       bool initiator)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_tdls_lnkie *lnkid;
	const u8 *init_addr, *rsp_addr;

	if (initiator) {
		init_addr = sdata->vif.addr;
		rsp_addr = peer;
	} else {
		init_addr = peer;
		rsp_addr = sdata->vif.addr;
	}

	lnkid = skb_put(skb, sizeof(struct ieee80211_tdls_lnkie));

	lnkid->ie_type = WLAN_EID_LINK_ID;
	lnkid->ie_len = sizeof(struct ieee80211_tdls_lnkie) - 2;

	memcpy(lnkid->bssid, link->u.mgd.bssid, ETH_ALEN);
	memcpy(lnkid->init_sta, init_addr, ETH_ALEN);
	memcpy(lnkid->resp_sta, rsp_addr, ETH_ALEN);
}

static void
ieee80211_tdls_add_aid(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb)
{
	u8 *pos = skb_put(skb, 4);

	*pos++ = WLAN_EID_AID;
	*pos++ = 2; /* len */
	put_unaligned_le16(sdata->vif.cfg.aid, pos);
}

/* translate numbering in the WMM parameter IE to the mac80211 notation */
static enum ieee80211_ac_numbers ieee80211_ac_from_wmm(int ac)
{
	switch (ac) {
	default:
		WARN_ON_ONCE(1);
		fallthrough;
	case 0:
		return IEEE80211_AC_BE;
	case 1:
		return IEEE80211_AC_BK;
	case 2:
		return IEEE80211_AC_VI;
	case 3:
		return IEEE80211_AC_VO;
	}
}

static u8 ieee80211_wmm_aci_aifsn(int aifsn, bool acm, int aci)
{
	u8 ret;

	ret = aifsn & 0x0f;
	if (acm)
		ret |= 0x10;
	ret |= (aci << 5) & 0x60;
	return ret;
}

static u8 ieee80211_wmm_ecw(u16 cw_min, u16 cw_max)
{
	return ((ilog2(cw_min + 1) << 0x0) & 0x0f) |
	       ((ilog2(cw_max + 1) << 0x4) & 0xf0);
}

static void ieee80211_tdls_add_wmm_param_ie(struct ieee80211_sub_if_data *sdata,
					    struct sk_buff *skb)
{
	struct ieee80211_wmm_param_ie *wmm;
	struct ieee80211_tx_queue_params *txq;
	int i;

	wmm = skb_put_zero(skb, sizeof(*wmm));

	wmm->element_id = WLAN_EID_VENDOR_SPECIFIC;
	wmm->len = sizeof(*wmm) - 2;

	wmm->oui[0] = 0x00; /* Microsoft OUI 00:50:F2 */
	wmm->oui[1] = 0x50;
	wmm->oui[2] = 0xf2;
	wmm->oui_type = 2; /* WME */
	wmm->oui_subtype = 1; /* WME param */
	wmm->version = 1; /* WME ver */
	wmm->qos_info = 0; /* U-APSD not in use */

	/*
	 * Use the EDCA parameters defined for the BSS, or default if the AP
	 * doesn't support it, as mandated by 802.11-2012 section 10.22.4
	 */
	for (i = 0; i < IEEE80211_NUM_ACS; i++) {
		txq = &sdata->deflink.tx_conf[ieee80211_ac_from_wmm(i)];
		wmm->ac[i].aci_aifsn = ieee80211_wmm_aci_aifsn(txq->aifs,
							       txq->acm, i);
		wmm->ac[i].cw = ieee80211_wmm_ecw(txq->cw_min, txq->cw_max);
		wmm->ac[i].txop_limit = cpu_to_le16(txq->txop);
	}
}

static void
ieee80211_tdls_chandef_vht_upgrade(struct ieee80211_sub_if_data *sdata,
				   struct sta_info *sta)
{
	/* IEEE802.11ac-2013 Table E-4 */
	static const u16 centers_80mhz[] = { 5210, 5290, 5530, 5610, 5690, 5775 };
	struct cfg80211_chan_def uc = sta->tdls_chandef;
	enum nl80211_chan_width max_width =
		ieee80211_sta_cap_chan_bw(&sta->deflink);
	int i;

	/* only support upgrading non-narrow channels up to 80Mhz */
	if (max_width == NL80211_CHAN_WIDTH_5 ||
	    max_width == NL80211_CHAN_WIDTH_10)
		return;

	if (max_width > NL80211_CHAN_WIDTH_80)
		max_width = NL80211_CHAN_WIDTH_80;

	if (uc.width >= max_width)
		return;
	/*
	 * Channel usage constrains in the IEEE802.11ac-2013 specification only
	 * allow expanding a 20MHz channel to 80MHz in a single way. In
	 * addition, there are no 40MHz allowed channels that are not part of
	 * the allowed 80MHz range in the 5GHz spectrum (the relevant one here).
	 */
	for (i = 0; i < ARRAY_SIZE(centers_80mhz); i++)
		if (abs(uc.chan->center_freq - centers_80mhz[i]) <= 30) {
			uc.center_freq1 = centers_80mhz[i];
			uc.center_freq2 = 0;
			uc.width = NL80211_CHAN_WIDTH_80;
			break;
		}

	if (!uc.center_freq1)
		return;

	/* proceed to downgrade the chandef until usable or the same as AP BW */
	while (uc.width > max_width ||
	       (uc.width > sta->tdls_chandef.width &&
		!cfg80211_reg_can_beacon_relax(sdata->local->hw.wiphy, &uc,
					       sdata->wdev.iftype)))
		ieee80211_chandef_downgrade(&uc, NULL);

	if (!cfg80211_chandef_identical(&uc, &sta->tdls_chandef)) {
		tdls_dbg(sdata, "TDLS ch width upgraded %d -> %d\n",
			 sta->tdls_chandef.width, uc.width);

		/*
		 * the station is not yet authorized when BW upgrade is done,
		 * locking is not required
		 */
		sta->tdls_chandef = uc;
	}
}

static void
ieee80211_tdls_add_setup_start_ies(struct ieee80211_link_data *link,
				   struct sk_buff *skb, const u8 *peer,
				   u8 action_code, bool initiator,
				   const u8 *extra_ies, size_t extra_ies_len)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_supported_band *sband;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_sta_ht_cap ht_cap;
	struct ieee80211_sta_vht_cap vht_cap;
	const struct ieee80211_sta_he_cap *he_cap;
	const struct ieee80211_sta_eht_cap *eht_cap;
	struct sta_info *sta = NULL;
	size_t offset = 0, noffset;
	u8 *pos;

	sband = ieee80211_get_link_sband(link);
	if (WARN_ON_ONCE(!sband))
		return;

	ieee80211_put_srates_elem(skb, sband, 0, 0, WLAN_EID_SUPP_RATES);
	ieee80211_put_srates_elem(skb, sband, 0, 0, WLAN_EID_EXT_SUPP_RATES);
	ieee80211_tdls_add_supp_channels(sdata, skb);

	/* add any custom IEs that go before Extended Capabilities */
	if (extra_ies_len) {
		static const u8 before_ext_cap[] = {
			WLAN_EID_SUPP_RATES,
			WLAN_EID_COUNTRY,
			WLAN_EID_EXT_SUPP_RATES,
			WLAN_EID_SUPPORTED_CHANNELS,
			WLAN_EID_RSN,
		};
		noffset = ieee80211_ie_split(extra_ies, extra_ies_len,
					     before_ext_cap,
					     ARRAY_SIZE(before_ext_cap),
					     offset);
		skb_put_data(skb, extra_ies + offset, noffset - offset);
		offset = noffset;
	}

	ieee80211_tdls_add_ext_capab(link, skb);

	/* add the QoS element if we support it */
	if (local->hw.queues >= IEEE80211_NUM_ACS &&
	    action_code != WLAN_PUB_ACTION_TDLS_DISCOVER_RES)
		ieee80211_add_wmm_info_ie(skb_put(skb, 9), 0); /* no U-APSD */

	/* add any custom IEs that go before HT capabilities */
	if (extra_ies_len) {
		static const u8 before_ht_cap[] = {
			WLAN_EID_SUPP_RATES,
			WLAN_EID_COUNTRY,
			WLAN_EID_EXT_SUPP_RATES,
			WLAN_EID_SUPPORTED_CHANNELS,
			WLAN_EID_RSN,
			WLAN_EID_EXT_CAPABILITY,
			WLAN_EID_QOS_CAPA,
			WLAN_EID_FAST_BSS_TRANSITION,
			WLAN_EID_TIMEOUT_INTERVAL,
			WLAN_EID_SUPPORTED_REGULATORY_CLASSES,
		};
		noffset = ieee80211_ie_split(extra_ies, extra_ies_len,
					     before_ht_cap,
					     ARRAY_SIZE(before_ht_cap),
					     offset);
		skb_put_data(skb, extra_ies + offset, noffset - offset);
		offset = noffset;
	}

	/* we should have the peer STA if we're already responding */
	if (action_code == WLAN_TDLS_SETUP_RESPONSE) {
		sta = sta_info_get(sdata, peer);
		if (WARN_ON_ONCE(!sta))
			return;

		sta->tdls_chandef = link->conf->chanreq.oper;
	}

	ieee80211_tdls_add_oper_classes(link, skb);

	/*
	 * with TDLS we can switch channels, and HT-caps are not necessarily
	 * the same on all bands. The specification limits the setup to a
	 * single HT-cap, so use the current band for now.
	 */
	memcpy(&ht_cap, &sband->ht_cap, sizeof(ht_cap));

	if ((action_code == WLAN_TDLS_SETUP_REQUEST ||
	     action_code == WLAN_PUB_ACTION_TDLS_DISCOVER_RES) &&
	    ht_cap.ht_supported) {
		ieee80211_apply_htcap_overrides(sdata, &ht_cap);

		/* disable SMPS in TDLS initiator */
		ht_cap.cap |= WLAN_HT_CAP_SM_PS_DISABLED
				<< IEEE80211_HT_CAP_SM_PS_SHIFT;

		pos = skb_put(skb, sizeof(struct ieee80211_ht_cap) + 2);
		ieee80211_ie_build_ht_cap(pos, &ht_cap, ht_cap.cap);
	} else if (action_code == WLAN_TDLS_SETUP_RESPONSE &&
		   ht_cap.ht_supported && sta->sta.deflink.ht_cap.ht_supported) {
		/* the peer caps are already intersected with our own */
		memcpy(&ht_cap, &sta->sta.deflink.ht_cap, sizeof(ht_cap));

		pos = skb_put(skb, sizeof(struct ieee80211_ht_cap) + 2);
		ieee80211_ie_build_ht_cap(pos, &ht_cap, ht_cap.cap);
	}

	if (ht_cap.ht_supported &&
	    (ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40))
		ieee80211_tdls_add_bss_coex_ie(skb);

	ieee80211_tdls_add_link_ie(link, skb, peer, initiator);

	/* add any custom IEs that go before VHT capabilities */
	if (extra_ies_len) {
		static const u8 before_vht_cap[] = {
			WLAN_EID_SUPP_RATES,
			WLAN_EID_COUNTRY,
			WLAN_EID_EXT_SUPP_RATES,
			WLAN_EID_SUPPORTED_CHANNELS,
			WLAN_EID_RSN,
			WLAN_EID_EXT_CAPABILITY,
			WLAN_EID_QOS_CAPA,
			WLAN_EID_FAST_BSS_TRANSITION,
			WLAN_EID_TIMEOUT_INTERVAL,
			WLAN_EID_SUPPORTED_REGULATORY_CLASSES,
			WLAN_EID_MULTI_BAND,
		};
		noffset = ieee80211_ie_split(extra_ies, extra_ies_len,
					     before_vht_cap,
					     ARRAY_SIZE(before_vht_cap),
					     offset);
		skb_put_data(skb, extra_ies + offset, noffset - offset);
		offset = noffset;
	}

	/* add AID if VHT, HE or EHT capabilities supported */
	memcpy(&vht_cap, &sband->vht_cap, sizeof(vht_cap));
	he_cap = ieee80211_get_he_iftype_cap_vif(sband, &sdata->vif);
	eht_cap = ieee80211_get_eht_iftype_cap_vif(sband, &sdata->vif);
	if ((vht_cap.vht_supported || he_cap || eht_cap) &&
	    (action_code == WLAN_TDLS_SETUP_REQUEST ||
	     action_code == WLAN_TDLS_SETUP_RESPONSE))
		ieee80211_tdls_add_aid(sdata, skb);

	/* build the VHT-cap similarly to the HT-cap */
	if ((action_code == WLAN_TDLS_SETUP_REQUEST ||
	     action_code == WLAN_PUB_ACTION_TDLS_DISCOVER_RES) &&
	    vht_cap.vht_supported) {
		ieee80211_apply_vhtcap_overrides(sdata, &vht_cap);

		pos = skb_put(skb, sizeof(struct ieee80211_vht_cap) + 2);
		ieee80211_ie_build_vht_cap(pos, &vht_cap, vht_cap.cap);
	} else if (action_code == WLAN_TDLS_SETUP_RESPONSE &&
		   vht_cap.vht_supported && sta->sta.deflink.vht_cap.vht_supported) {
		/* the peer caps are already intersected with our own */
		memcpy(&vht_cap, &sta->sta.deflink.vht_cap, sizeof(vht_cap));

		pos = skb_put(skb, sizeof(struct ieee80211_vht_cap) + 2);
		ieee80211_ie_build_vht_cap(pos, &vht_cap, vht_cap.cap);

		/*
		 * if both peers support WIDER_BW, we can expand the chandef to
		 * a wider compatible one, up to 80MHz
		 */
		if (test_sta_flag(sta, WLAN_STA_TDLS_WIDER_BW))
			ieee80211_tdls_chandef_vht_upgrade(sdata, sta);
	}

	/* add any custom IEs that go before HE capabilities */
	if (extra_ies_len) {
		static const u8 before_he_cap[] = {
			WLAN_EID_EXTENSION,
			WLAN_EID_EXT_FILS_REQ_PARAMS,
			WLAN_EID_AP_CSN,
		};
		noffset = ieee80211_ie_split(extra_ies, extra_ies_len,
					     before_he_cap,
					     ARRAY_SIZE(before_he_cap),
					     offset);
		skb_put_data(skb, extra_ies + offset, noffset - offset);
		offset = noffset;
	}

	/* build the HE-cap from sband */
	if (action_code == WLAN_TDLS_SETUP_REQUEST ||
	    action_code == WLAN_TDLS_SETUP_RESPONSE ||
	    action_code == WLAN_PUB_ACTION_TDLS_DISCOVER_RES) {
		ieee80211_put_he_cap(skb, sdata, sband, NULL);

		/* Build HE 6Ghz capa IE from sband */
		if (sband->band == NL80211_BAND_6GHZ)
			ieee80211_put_he_6ghz_cap(skb, sdata, link->smps_mode);
	}

	/* add any custom IEs that go before EHT capabilities */
	if (extra_ies_len) {
		static const u8 before_he_cap[] = {
			WLAN_EID_EXTENSION,
			WLAN_EID_EXT_FILS_REQ_PARAMS,
			WLAN_EID_AP_CSN,
		};

		noffset = ieee80211_ie_split(extra_ies, extra_ies_len,
					     before_he_cap,
					     ARRAY_SIZE(before_he_cap),
					     offset);
		skb_put_data(skb, extra_ies + offset, noffset - offset);
		offset = noffset;
	}

	/* build the EHT-cap from sband */
	if (action_code == WLAN_TDLS_SETUP_REQUEST ||
	    action_code == WLAN_TDLS_SETUP_RESPONSE ||
	    action_code == WLAN_PUB_ACTION_TDLS_DISCOVER_RES)
		ieee80211_put_eht_cap(skb, sdata, sband, NULL);

	/* add any remaining IEs */
	if (extra_ies_len) {
		noffset = extra_ies_len;
		skb_put_data(skb, extra_ies + offset, noffset - offset);
	}

}

static void
ieee80211_tdls_add_setup_cfm_ies(struct ieee80211_link_data *link,
				 struct sk_buff *skb, const u8 *peer,
				 bool initiator, const u8 *extra_ies,
				 size_t extra_ies_len)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;
	size_t offset = 0, noffset;
	struct sta_info *sta, *ap_sta;
	struct ieee80211_supported_band *sband;
	u8 *pos;

	sband = ieee80211_get_link_sband(link);
	if (WARN_ON_ONCE(!sband))
		return;

	sta = sta_info_get(sdata, peer);
	ap_sta = sta_info_get(sdata, sdata->vif.cfg.ap_addr);

	if (WARN_ON_ONCE(!sta || !ap_sta))
		return;

	sta->tdls_chandef = link->conf->chanreq.oper;

	/* add any custom IEs that go before the QoS IE */
	if (extra_ies_len) {
		static const u8 before_qos[] = {
			WLAN_EID_RSN,
		};
		noffset = ieee80211_ie_split(extra_ies, extra_ies_len,
					     before_qos,
					     ARRAY_SIZE(before_qos),
					     offset);
		skb_put_data(skb, extra_ies + offset, noffset - offset);
		offset = noffset;
	}

	/* add the QoS param IE if both the peer and we support it */
	if (local->hw.queues >= IEEE80211_NUM_ACS && sta->sta.wme)
		ieee80211_tdls_add_wmm_param_ie(sdata, skb);

	/* add any custom IEs that go before HT operation */
	if (extra_ies_len) {
		static const u8 before_ht_op[] = {
			WLAN_EID_RSN,
			WLAN_EID_QOS_CAPA,
			WLAN_EID_FAST_BSS_TRANSITION,
			WLAN_EID_TIMEOUT_INTERVAL,
		};
		noffset = ieee80211_ie_split(extra_ies, extra_ies_len,
					     before_ht_op,
					     ARRAY_SIZE(before_ht_op),
					     offset);
		skb_put_data(skb, extra_ies + offset, noffset - offset);
		offset = noffset;
	}

	/*
	 * if HT support is only added in TDLS, we need an HT-operation IE.
	 * add the IE as required by IEEE802.11-2012 9.23.3.2.
	 */
	if (!ap_sta->sta.deflink.ht_cap.ht_supported && sta->sta.deflink.ht_cap.ht_supported) {
		u16 prot = IEEE80211_HT_OP_MODE_PROTECTION_NONHT_MIXED |
			   IEEE80211_HT_OP_MODE_NON_GF_STA_PRSNT |
			   IEEE80211_HT_OP_MODE_NON_HT_STA_PRSNT;

		pos = skb_put(skb, 2 + sizeof(struct ieee80211_ht_operation));
		ieee80211_ie_build_ht_oper(pos, &sta->sta.deflink.ht_cap,
					   &link->conf->chanreq.oper, prot,
					   true);
	}

	ieee80211_tdls_add_link_ie(link, skb, peer, initiator);

	/* only include VHT-operation if not on the 2.4GHz band */
	if (sband->band != NL80211_BAND_2GHZ &&
	    sta->sta.deflink.vht_cap.vht_supported) {
		/*
		 * if both peers support WIDER_BW, we can expand the chandef to
		 * a wider compatible one, up to 80MHz
		 */
		if (test_sta_flag(sta, WLAN_STA_TDLS_WIDER_BW))
			ieee80211_tdls_chandef_vht_upgrade(sdata, sta);

		pos = skb_put(skb, 2 + sizeof(struct ieee80211_vht_operation));
		ieee80211_ie_build_vht_oper(pos, &sta->sta.deflink.vht_cap,
					    &sta->tdls_chandef);
	}

	/* add any remaining IEs */
	if (extra_ies_len) {
		noffset = extra_ies_len;
		skb_put_data(skb, extra_ies + offset, noffset - offset);
	}
}

static void
ieee80211_tdls_add_chan_switch_req_ies(struct ieee80211_link_data *link,
				       struct sk_buff *skb, const u8 *peer,
				       bool initiator, const u8 *extra_ies,
				       size_t extra_ies_len, u8 oper_class,
				       struct cfg80211_chan_def *chandef)
{
	struct ieee80211_tdls_data *tf;
	size_t offset = 0, noffset;

	if (WARN_ON_ONCE(!chandef))
		return;

	tf = (void *)skb->data;
	tf->u.chan_switch_req.target_channel =
		ieee80211_frequency_to_channel(chandef->chan->center_freq);
	tf->u.chan_switch_req.oper_class = oper_class;

	if (extra_ies_len) {
		static const u8 before_lnkie[] = {
			WLAN_EID_SECONDARY_CHANNEL_OFFSET,
		};
		noffset = ieee80211_ie_split(extra_ies, extra_ies_len,
					     before_lnkie,
					     ARRAY_SIZE(before_lnkie),
					     offset);
		skb_put_data(skb, extra_ies + offset, noffset - offset);
		offset = noffset;
	}

	ieee80211_tdls_add_link_ie(link, skb, peer, initiator);

	/* add any remaining IEs */
	if (extra_ies_len) {
		noffset = extra_ies_len;
		skb_put_data(skb, extra_ies + offset, noffset - offset);
	}
}

static void
ieee80211_tdls_add_chan_switch_resp_ies(struct ieee80211_link_data *link,
					struct sk_buff *skb, const u8 *peer,
					u16 status_code, bool initiator,
					const u8 *extra_ies,
					size_t extra_ies_len)
{
	if (status_code == 0)
		ieee80211_tdls_add_link_ie(link, skb, peer, initiator);

	if (extra_ies_len)
		skb_put_data(skb, extra_ies, extra_ies_len);
}

static void ieee80211_tdls_add_ies(struct ieee80211_link_data *link,
				   struct sk_buff *skb, const u8 *peer,
				   u8 action_code, u16 status_code,
				   bool initiator, const u8 *extra_ies,
				   size_t extra_ies_len, u8 oper_class,
				   struct cfg80211_chan_def *chandef)
{
	switch (action_code) {
	case WLAN_TDLS_SETUP_REQUEST:
	case WLAN_TDLS_SETUP_RESPONSE:
	case WLAN_PUB_ACTION_TDLS_DISCOVER_RES:
		if (status_code == 0)
			ieee80211_tdls_add_setup_start_ies(link,
							   skb, peer,
							   action_code,
							   initiator,
							   extra_ies,
							   extra_ies_len);
		break;
	case WLAN_TDLS_SETUP_CONFIRM:
		if (status_code == 0)
			ieee80211_tdls_add_setup_cfm_ies(link, skb, peer,
							 initiator, extra_ies,
							 extra_ies_len);
		break;
	case WLAN_TDLS_TEARDOWN:
	case WLAN_TDLS_DISCOVERY_REQUEST:
		if (extra_ies_len)
			skb_put_data(skb, extra_ies, extra_ies_len);
		if (status_code == 0 || action_code == WLAN_TDLS_TEARDOWN)
			ieee80211_tdls_add_link_ie(link, skb,
						   peer, initiator);
		break;
	case WLAN_TDLS_CHANNEL_SWITCH_REQUEST:
		ieee80211_tdls_add_chan_switch_req_ies(link, skb, peer,
						       initiator, extra_ies,
						       extra_ies_len,
						       oper_class, chandef);
		break;
	case WLAN_TDLS_CHANNEL_SWITCH_RESPONSE:
		ieee80211_tdls_add_chan_switch_resp_ies(link, skb, peer,
							status_code,
							initiator, extra_ies,
							extra_ies_len);
		break;
	}

}

static int
ieee80211_prep_tdls_encap_data(struct wiphy *wiphy, struct net_device *dev,
			       struct ieee80211_link_data *link,
			       const u8 *peer, u8 action_code, u8 dialog_token,
			       u16 status_code, struct sk_buff *skb)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_tdls_data *tf;

	tf = skb_put(skb, offsetof(struct ieee80211_tdls_data, u));

	memcpy(tf->da, peer, ETH_ALEN);
	memcpy(tf->sa, sdata->vif.addr, ETH_ALEN);
	tf->ether_type = cpu_to_be16(ETH_P_TDLS);
	tf->payload_type = WLAN_TDLS_SNAP_RFTYPE;

	/* network header is after the ethernet header */
	skb_set_network_header(skb, ETH_HLEN);

	switch (action_code) {
	case WLAN_TDLS_SETUP_REQUEST:
		tf->category = WLAN_CATEGORY_TDLS;
		tf->action_code = WLAN_TDLS_SETUP_REQUEST;

		skb_put(skb, sizeof(tf->u.setup_req));
		tf->u.setup_req.dialog_token = dialog_token;
		tf->u.setup_req.capability =
			cpu_to_le16(ieee80211_get_tdls_sta_capab(link,
								 status_code));
		break;
	case WLAN_TDLS_SETUP_RESPONSE:
		tf->category = WLAN_CATEGORY_TDLS;
		tf->action_code = WLAN_TDLS_SETUP_RESPONSE;

		skb_put(skb, sizeof(tf->u.setup_resp));
		tf->u.setup_resp.status_code = cpu_to_le16(status_code);
		tf->u.setup_resp.dialog_token = dialog_token;
		tf->u.setup_resp.capability =
			cpu_to_le16(ieee80211_get_tdls_sta_capab(link,
								 status_code));
		break;
	case WLAN_TDLS_SETUP_CONFIRM:
		tf->category = WLAN_CATEGORY_TDLS;
		tf->action_code = WLAN_TDLS_SETUP_CONFIRM;

		skb_put(skb, sizeof(tf->u.setup_cfm));
		tf->u.setup_cfm.status_code = cpu_to_le16(status_code);
		tf->u.setup_cfm.dialog_token = dialog_token;
		break;
	case WLAN_TDLS_TEARDOWN:
		tf->category = WLAN_CATEGORY_TDLS;
		tf->action_code = WLAN_TDLS_TEARDOWN;

		skb_put(skb, sizeof(tf->u.teardown));
		tf->u.teardown.reason_code = cpu_to_le16(status_code);
		break;
	case WLAN_TDLS_DISCOVERY_REQUEST:
		tf->category = WLAN_CATEGORY_TDLS;
		tf->action_code = WLAN_TDLS_DISCOVERY_REQUEST;

		skb_put(skb, sizeof(tf->u.discover_req));
		tf->u.discover_req.dialog_token = dialog_token;
		break;
	case WLAN_TDLS_CHANNEL_SWITCH_REQUEST:
		tf->category = WLAN_CATEGORY_TDLS;
		tf->action_code = WLAN_TDLS_CHANNEL_SWITCH_REQUEST;

		skb_put(skb, sizeof(tf->u.chan_switch_req));
		break;
	case WLAN_TDLS_CHANNEL_SWITCH_RESPONSE:
		tf->category = WLAN_CATEGORY_TDLS;
		tf->action_code = WLAN_TDLS_CHANNEL_SWITCH_RESPONSE;

		skb_put(skb, sizeof(tf->u.chan_switch_resp));
		tf->u.chan_switch_resp.status_code = cpu_to_le16(status_code);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int
ieee80211_prep_tdls_direct(struct wiphy *wiphy, struct net_device *dev,
			   const u8 *peer, struct ieee80211_link_data *link,
			   u8 action_code, u8 dialog_token,
			   u16 status_code, struct sk_buff *skb)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_mgmt *mgmt;

	mgmt = skb_put_zero(skb, 24);
	memcpy(mgmt->da, peer, ETH_ALEN);
	memcpy(mgmt->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(mgmt->bssid, link->u.mgd.bssid, ETH_ALEN);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);

	switch (action_code) {
	case WLAN_PUB_ACTION_TDLS_DISCOVER_RES:
		skb_put(skb, 1 + sizeof(mgmt->u.action.u.tdls_discover_resp));
		mgmt->u.action.category = WLAN_CATEGORY_PUBLIC;
		mgmt->u.action.u.tdls_discover_resp.action_code =
			WLAN_PUB_ACTION_TDLS_DISCOVER_RES;
		mgmt->u.action.u.tdls_discover_resp.dialog_token =
			dialog_token;
		mgmt->u.action.u.tdls_discover_resp.capability =
			cpu_to_le16(ieee80211_get_tdls_sta_capab(link,
								 status_code));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static struct sk_buff *
ieee80211_tdls_build_mgmt_packet_data(struct ieee80211_sub_if_data *sdata,
				      const u8 *peer, int link_id,
				      u8 action_code, u8 dialog_token,
				      u16 status_code, bool initiator,
				      const u8 *extra_ies, size_t extra_ies_len,
				      u8 oper_class,
				      struct cfg80211_chan_def *chandef)
{
	struct ieee80211_local *local = sdata->local;
	struct sk_buff *skb;
	int ret;
	struct ieee80211_link_data *link;

	link_id = link_id >= 0 ? link_id : 0;
	rcu_read_lock();
	link = rcu_dereference(sdata->link[link_id]);
	if (WARN_ON(!link))
		goto unlock;

	skb = netdev_alloc_skb(sdata->dev,
			       local->hw.extra_tx_headroom +
			       max(sizeof(struct ieee80211_mgmt),
				   sizeof(struct ieee80211_tdls_data)) +
			       50 + /* supported rates */
			       10 + /* ext capab */
			       26 + /* max(WMM-info, WMM-param) */
			       2 + max(sizeof(struct ieee80211_ht_cap),
				       sizeof(struct ieee80211_ht_operation)) +
			       2 + max(sizeof(struct ieee80211_vht_cap),
				       sizeof(struct ieee80211_vht_operation)) +
			       2 + 1 + sizeof(struct ieee80211_he_cap_elem) +
				       sizeof(struct ieee80211_he_mcs_nss_supp) +
				       IEEE80211_HE_PPE_THRES_MAX_LEN +
			       2 + 1 + sizeof(struct ieee80211_he_6ghz_capa) +
			       2 + 1 + sizeof(struct ieee80211_eht_cap_elem) +
				       sizeof(struct ieee80211_eht_mcs_nss_supp) +
				       IEEE80211_EHT_PPE_THRES_MAX_LEN +
			       50 + /* supported channels */
			       3 + /* 40/20 BSS coex */
			       4 + /* AID */
			       4 + /* oper classes */
			       extra_ies_len +
			       sizeof(struct ieee80211_tdls_lnkie));
	if (!skb)
		goto unlock;

	skb_reserve(skb, local->hw.extra_tx_headroom);

	switch (action_code) {
	case WLAN_TDLS_SETUP_REQUEST:
	case WLAN_TDLS_SETUP_RESPONSE:
	case WLAN_TDLS_SETUP_CONFIRM:
	case WLAN_TDLS_TEARDOWN:
	case WLAN_TDLS_DISCOVERY_REQUEST:
	case WLAN_TDLS_CHANNEL_SWITCH_REQUEST:
	case WLAN_TDLS_CHANNEL_SWITCH_RESPONSE:
		ret = ieee80211_prep_tdls_encap_data(local->hw.wiphy,
						     sdata->dev, link, peer,
						     action_code, dialog_token,
						     status_code, skb);
		break;
	case WLAN_PUB_ACTION_TDLS_DISCOVER_RES:
		ret = ieee80211_prep_tdls_direct(local->hw.wiphy, sdata->dev,
						 peer, link, action_code,
						 dialog_token, status_code,
						 skb);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	if (ret < 0)
		goto fail;

	ieee80211_tdls_add_ies(link, skb, peer, action_code, status_code,
			       initiator, extra_ies, extra_ies_len, oper_class,
			       chandef);
	rcu_read_unlock();
	return skb;

fail:
	dev_kfree_skb(skb);
unlock:
	rcu_read_unlock();
	return NULL;
}

static int
ieee80211_tdls_prep_mgmt_packet(struct wiphy *wiphy, struct net_device *dev,
				const u8 *peer, int link_id,
				u8 action_code, u8 dialog_token,
				u16 status_code, u32 peer_capability,
				bool initiator, const u8 *extra_ies,
				size_t extra_ies_len, u8 oper_class,
				struct cfg80211_chan_def *chandef)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct sk_buff *skb = NULL;
	struct sta_info *sta;
	u32 flags = 0;
	int ret = 0;

	rcu_read_lock();
	sta = sta_info_get(sdata, peer);

	/* infer the initiator if we can, to support old userspace */
	switch (action_code) {
	case WLAN_TDLS_SETUP_REQUEST:
		if (sta) {
			set_sta_flag(sta, WLAN_STA_TDLS_INITIATOR);
			sta->sta.tdls_initiator = false;
		}
		fallthrough;
	case WLAN_TDLS_SETUP_CONFIRM:
	case WLAN_TDLS_DISCOVERY_REQUEST:
		initiator = true;
		break;
	case WLAN_TDLS_SETUP_RESPONSE:
		/*
		 * In some testing scenarios, we send a request and response.
		 * Make the last packet sent take effect for the initiator
		 * value.
		 */
		if (sta) {
			clear_sta_flag(sta, WLAN_STA_TDLS_INITIATOR);
			sta->sta.tdls_initiator = true;
		}
		fallthrough;
	case WLAN_PUB_ACTION_TDLS_DISCOVER_RES:
		initiator = false;
		break;
	case WLAN_TDLS_TEARDOWN:
	case WLAN_TDLS_CHANNEL_SWITCH_REQUEST:
	case WLAN_TDLS_CHANNEL_SWITCH_RESPONSE:
		/* any value is ok */
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	if (sta && test_sta_flag(sta, WLAN_STA_TDLS_INITIATOR))
		initiator = true;

	rcu_read_unlock();
	if (ret < 0)
		goto fail;

	skb = ieee80211_tdls_build_mgmt_packet_data(sdata, peer,
						    link_id, action_code,
						    dialog_token, status_code,
						    initiator, extra_ies,
						    extra_ies_len, oper_class,
						    chandef);
	if (!skb) {
		ret = -EINVAL;
		goto fail;
	}

	if (action_code == WLAN_PUB_ACTION_TDLS_DISCOVER_RES) {
		ieee80211_tx_skb_tid(sdata, skb, 7, link_id);
		return 0;
	}

	/*
	 * According to 802.11z: Setup req/resp are sent in AC_BK, otherwise
	 * we should default to AC_VI.
	 */
	switch (action_code) {
	case WLAN_TDLS_SETUP_REQUEST:
	case WLAN_TDLS_SETUP_RESPONSE:
		skb->priority = 256 + 2;
		break;
	default:
		skb->priority = 256 + 5;
		break;
	}

	/*
	 * Set the WLAN_TDLS_TEARDOWN flag to indicate a teardown in progress.
	 * Later, if no ACK is returned from peer, we will re-send the teardown
	 * packet through the AP.
	 */
	if ((action_code == WLAN_TDLS_TEARDOWN) &&
	    ieee80211_hw_check(&sdata->local->hw, REPORTS_TX_ACK_STATUS)) {
		bool try_resend; /* Should we keep skb for possible resend */

		/* If not sending directly to peer - no point in keeping skb */
		rcu_read_lock();
		sta = sta_info_get(sdata, peer);
		try_resend = sta && test_sta_flag(sta, WLAN_STA_TDLS_PEER_AUTH);
		rcu_read_unlock();

		spin_lock_bh(&sdata->u.mgd.teardown_lock);
		if (try_resend && !sdata->u.mgd.teardown_skb) {
			/* Mark it as requiring TX status callback  */
			flags |= IEEE80211_TX_CTL_REQ_TX_STATUS |
				 IEEE80211_TX_INTFL_MLME_CONN_TX;

			/*
			 * skb is copied since mac80211 will later set
			 * properties that might not be the same as the AP,
			 * such as encryption, QoS, addresses, etc.
			 *
			 * No problem if skb_copy() fails, so no need to check.
			 */
			sdata->u.mgd.teardown_skb = skb_copy(skb, GFP_ATOMIC);
			sdata->u.mgd.orig_teardown_skb = skb;
		}
		spin_unlock_bh(&sdata->u.mgd.teardown_lock);
	}

	/* disable bottom halves when entering the Tx path */
	local_bh_disable();
	__ieee80211_subif_start_xmit(skb, dev, flags,
				     IEEE80211_TX_CTRL_MLO_LINK_UNSPEC, NULL);
	local_bh_enable();

	return ret;

fail:
	dev_kfree_skb(skb);
	return ret;
}

static int
ieee80211_tdls_mgmt_setup(struct wiphy *wiphy, struct net_device *dev,
			  const u8 *peer, int link_id,
			  u8 action_code, u8 dialog_token,
			  u16 status_code, u32 peer_capability, bool initiator,
			  const u8 *extra_ies, size_t extra_ies_len)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_local *local = sdata->local;
	enum ieee80211_smps_mode smps_mode =
		sdata->deflink.u.mgd.driver_smps_mode;
	int ret;

	/* don't support setup with forced SMPS mode that's not off */
	if (smps_mode != IEEE80211_SMPS_AUTOMATIC &&
	    smps_mode != IEEE80211_SMPS_OFF) {
		tdls_dbg(sdata, "Aborting TDLS setup due to SMPS mode %d\n",
			 smps_mode);
		return -EOPNOTSUPP;
	}

	lockdep_assert_wiphy(local->hw.wiphy);

	/* we don't support concurrent TDLS peer setups */
	if (!is_zero_ether_addr(sdata->u.mgd.tdls_peer) &&
	    !ether_addr_equal(sdata->u.mgd.tdls_peer, peer)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	/*
	 * make sure we have a STA representing the peer so we drop or buffer
	 * non-TDLS-setup frames to the peer. We can't send other packets
	 * during setup through the AP path.
	 * Allow error packets to be sent - sometimes we don't even add a STA
	 * before failing the setup.
	 */
	if (status_code == 0) {
		rcu_read_lock();
		if (!sta_info_get(sdata, peer)) {
			rcu_read_unlock();
			ret = -ENOLINK;
			goto out_unlock;
		}
		rcu_read_unlock();
	}

	ieee80211_flush_queues(local, sdata, false);
	memcpy(sdata->u.mgd.tdls_peer, peer, ETH_ALEN);

	/* we cannot take the mutex while preparing the setup packet */
	ret = ieee80211_tdls_prep_mgmt_packet(wiphy, dev, peer,
					      link_id, action_code,
					      dialog_token, status_code,
					      peer_capability, initiator,
					      extra_ies, extra_ies_len, 0,
					      NULL);
	if (ret < 0) {
		eth_zero_addr(sdata->u.mgd.tdls_peer);
		return ret;
	}

	wiphy_delayed_work_queue(sdata->local->hw.wiphy,
				 &sdata->u.mgd.tdls_peer_del_work,
				 TDLS_PEER_SETUP_TIMEOUT);
	return 0;

out_unlock:
	return ret;
}

static int
ieee80211_tdls_mgmt_teardown(struct wiphy *wiphy, struct net_device *dev,
			     const u8 *peer, int link_id,
			     u8 action_code, u8 dialog_token,
			     u16 status_code, u32 peer_capability,
			     bool initiator, const u8 *extra_ies,
			     size_t extra_ies_len)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;
	int ret;

	/*
	 * No packets can be transmitted to the peer via the AP during setup -
	 * the STA is set as a TDLS peer, but is not authorized.
	 * During teardown, we prevent direct transmissions by stopping the
	 * queues and flushing all direct packets.
	 */
	ieee80211_stop_vif_queues(local, sdata,
				  IEEE80211_QUEUE_STOP_REASON_TDLS_TEARDOWN);
	ieee80211_flush_queues(local, sdata, false);

	ret = ieee80211_tdls_prep_mgmt_packet(wiphy, dev, peer,
					      link_id, action_code,
					      dialog_token, status_code,
					      peer_capability, initiator,
					      extra_ies, extra_ies_len, 0,
					      NULL);
	if (ret < 0)
		sdata_err(sdata, "Failed sending TDLS teardown packet %d\n",
			  ret);

	/*
	 * Remove the STA AUTH flag to force further traffic through the AP. If
	 * the STA was unreachable, it was already removed.
	 */
	rcu_read_lock();
	sta = sta_info_get(sdata, peer);
	if (sta)
		clear_sta_flag(sta, WLAN_STA_TDLS_PEER_AUTH);
	rcu_read_unlock();

	ieee80211_wake_vif_queues(local, sdata,
				  IEEE80211_QUEUE_STOP_REASON_TDLS_TEARDOWN);

	return 0;
}

int ieee80211_tdls_mgmt(struct wiphy *wiphy, struct net_device *dev,
			const u8 *peer, int link_id,
			u8 action_code, u8 dialog_token, u16 status_code,
			u32 peer_capability, bool initiator,
			const u8 *extra_ies, size_t extra_ies_len)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	int ret;

	if (!(wiphy->flags & WIPHY_FLAG_SUPPORTS_TDLS))
		return -EOPNOTSUPP;

	/* make sure we are in managed mode, and associated */
	if (sdata->vif.type != NL80211_IFTYPE_STATION ||
	    !sdata->u.mgd.associated)
		return -EINVAL;

	switch (action_code) {
	case WLAN_TDLS_SETUP_REQUEST:
	case WLAN_TDLS_SETUP_RESPONSE:
		ret = ieee80211_tdls_mgmt_setup(wiphy, dev, peer,
						link_id, action_code,
						dialog_token, status_code,
						peer_capability, initiator,
						extra_ies, extra_ies_len);
		break;
	case WLAN_TDLS_TEARDOWN:
		ret = ieee80211_tdls_mgmt_teardown(wiphy, dev, peer, link_id,
						   action_code, dialog_token,
						   status_code,
						   peer_capability, initiator,
						   extra_ies, extra_ies_len);
		break;
	case WLAN_TDLS_DISCOVERY_REQUEST:
		/*
		 * Protect the discovery so we can hear the TDLS discovery
		 * response frame. It is transmitted directly and not buffered
		 * by the AP.
		 */
		drv_mgd_protect_tdls_discover(sdata->local, sdata, link_id);
		fallthrough;
	case WLAN_TDLS_SETUP_CONFIRM:
	case WLAN_PUB_ACTION_TDLS_DISCOVER_RES:
		/* no special handling */
		ret = ieee80211_tdls_prep_mgmt_packet(wiphy, dev, peer,
						      link_id, action_code,
						      dialog_token,
						      status_code,
						      peer_capability,
						      initiator, extra_ies,
						      extra_ies_len, 0, NULL);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	tdls_dbg(sdata, "TDLS mgmt action %d peer %pM link_id %d status %d\n",
		 action_code, peer, link_id, ret);
	return ret;
}

static void iee80211_tdls_recalc_chanctx(struct ieee80211_sub_if_data *sdata,
					 struct sta_info *sta)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_chanctx *ctx;
	enum nl80211_chan_width width;
	struct ieee80211_supported_band *sband;

	lockdep_assert_wiphy(local->hw.wiphy);

	conf = rcu_dereference_protected(sdata->vif.bss_conf.chanctx_conf,
					 lockdep_is_held(&local->hw.wiphy->mtx));
	if (conf) {
		width = conf->def.width;
		sband = local->hw.wiphy->bands[conf->def.chan->band];
		ctx = container_of(conf, struct ieee80211_chanctx, conf);
		ieee80211_recalc_chanctx_chantype(local, ctx);

		/* if width changed and a peer is given, update its BW */
		if (width != conf->def.width && sta &&
		    test_sta_flag(sta, WLAN_STA_TDLS_WIDER_BW)) {
			enum ieee80211_sta_rx_bandwidth bw;

			bw = ieee80211_chan_width_to_rx_bw(conf->def.width);
			bw = min(bw, ieee80211_sta_cap_rx_bw(&sta->deflink));
			if (bw != sta->sta.deflink.bandwidth) {
				sta->sta.deflink.bandwidth = bw;
				rate_control_rate_update(local, sband,
							 &sta->deflink,
							 IEEE80211_RC_BW_CHANGED);
				/*
				 * if a TDLS peer BW was updated, we need to
				 * recalc the chandef width again, to get the
				 * correct chanctx min_def
				 */
				ieee80211_recalc_chanctx_chantype(local, ctx);
			}
		}

	}
}

static int iee80211_tdls_have_ht_peers(struct ieee80211_sub_if_data *sdata)
{
	struct sta_info *sta;
	bool result = false;

	rcu_read_lock();
	list_for_each_entry_rcu(sta, &sdata->local->sta_list, list) {
		if (!sta->sta.tdls || sta->sdata != sdata || !sta->uploaded ||
		    !test_sta_flag(sta, WLAN_STA_AUTHORIZED) ||
		    !test_sta_flag(sta, WLAN_STA_TDLS_PEER_AUTH) ||
		    !sta->sta.deflink.ht_cap.ht_supported)
			continue;
		result = true;
		break;
	}
	rcu_read_unlock();

	return result;
}

static void
iee80211_tdls_recalc_ht_protection(struct ieee80211_sub_if_data *sdata,
				   struct sta_info *sta)
{
	bool tdls_ht;
	u16 protection = IEEE80211_HT_OP_MODE_PROTECTION_NONHT_MIXED |
			 IEEE80211_HT_OP_MODE_NON_GF_STA_PRSNT |
			 IEEE80211_HT_OP_MODE_NON_HT_STA_PRSNT;
	u16 opmode;

	/* Nothing to do if the BSS connection uses (at least) HT */
	if (sdata->deflink.u.mgd.conn.mode >= IEEE80211_CONN_MODE_HT)
		return;

	tdls_ht = (sta && sta->sta.deflink.ht_cap.ht_supported) ||
		  iee80211_tdls_have_ht_peers(sdata);

	opmode = sdata->vif.bss_conf.ht_operation_mode;

	if (tdls_ht)
		opmode |= protection;
	else
		opmode &= ~protection;

	if (opmode == sdata->vif.bss_conf.ht_operation_mode)
		return;

	sdata->vif.bss_conf.ht_operation_mode = opmode;
	ieee80211_link_info_change_notify(sdata, &sdata->deflink,
					  BSS_CHANGED_HT);
}

int ieee80211_tdls_oper(struct wiphy *wiphy, struct net_device *dev,
			const u8 *peer, enum nl80211_tdls_operation oper)
{
	struct sta_info *sta;
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_local *local = sdata->local;
	int ret;

	lockdep_assert_wiphy(local->hw.wiphy);

	if (!(wiphy->flags & WIPHY_FLAG_SUPPORTS_TDLS))
		return -EOPNOTSUPP;

	if (sdata->vif.type != NL80211_IFTYPE_STATION || !sdata->vif.cfg.assoc)
		return -EINVAL;

	switch (oper) {
	case NL80211_TDLS_ENABLE_LINK:
	case NL80211_TDLS_DISABLE_LINK:
		break;
	case NL80211_TDLS_TEARDOWN:
	case NL80211_TDLS_SETUP:
	case NL80211_TDLS_DISCOVERY_REQ:
		/* We don't support in-driver setup/teardown/discovery */
		return -EOPNOTSUPP;
	}

	/* protect possible bss_conf changes and avoid concurrency in
	 * ieee80211_bss_info_change_notify()
	 */
	tdls_dbg(sdata, "TDLS oper %d peer %pM\n", oper, peer);

	switch (oper) {
	case NL80211_TDLS_ENABLE_LINK:
		if (sdata->vif.bss_conf.csa_active) {
			tdls_dbg(sdata, "TDLS: disallow link during CSA\n");
			return -EBUSY;
		}

		sta = sta_info_get(sdata, peer);
		if (!sta)
			return -ENOLINK;

		iee80211_tdls_recalc_chanctx(sdata, sta);
		iee80211_tdls_recalc_ht_protection(sdata, sta);

		set_sta_flag(sta, WLAN_STA_TDLS_PEER_AUTH);

		WARN_ON_ONCE(is_zero_ether_addr(sdata->u.mgd.tdls_peer) ||
			     !ether_addr_equal(sdata->u.mgd.tdls_peer, peer));
		break;
	case NL80211_TDLS_DISABLE_LINK:
		/*
		 * The teardown message in ieee80211_tdls_mgmt_teardown() was
		 * created while the queues were stopped, so it might still be
		 * pending. Before flushing the queues we need to be sure the
		 * message is handled by the tasklet handling pending messages,
		 * otherwise we might start destroying the station before
		 * sending the teardown packet.
		 * Note that this only forces the tasklet to flush pendings -
		 * not to stop the tasklet from rescheduling itself.
		 */
		tasklet_kill(&local->tx_pending_tasklet);
		/* flush a potentially queued teardown packet */
		ieee80211_flush_queues(local, sdata, false);

		ret = sta_info_destroy_addr(sdata, peer);

		iee80211_tdls_recalc_ht_protection(sdata, NULL);

		iee80211_tdls_recalc_chanctx(sdata, NULL);
		if (ret)
			return ret;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (ether_addr_equal(sdata->u.mgd.tdls_peer, peer)) {
		wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
					  &sdata->u.mgd.tdls_peer_del_work);
		eth_zero_addr(sdata->u.mgd.tdls_peer);
	}

	wiphy_work_queue(sdata->local->hw.wiphy,
			 &sdata->deflink.u.mgd.request_smps_work);

	return 0;
}

void ieee80211_tdls_oper_request(struct ieee80211_vif *vif, const u8 *peer,
				 enum nl80211_tdls_operation oper,
				 u16 reason_code, gfp_t gfp)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);

	if (vif->type != NL80211_IFTYPE_STATION || !vif->cfg.assoc) {
		sdata_err(sdata, "Discarding TDLS oper %d - not STA or disconnected\n",
			  oper);
		return;
	}

	cfg80211_tdls_oper_request(sdata->dev, peer, oper, reason_code, gfp);
}
EXPORT_SYMBOL(ieee80211_tdls_oper_request);

static void
iee80211_tdls_add_ch_switch_timing(u8 *buf, u16 switch_time, u16 switch_timeout)
{
	struct ieee80211_ch_switch_timing *ch_sw;

	*buf++ = WLAN_EID_CHAN_SWITCH_TIMING;
	*buf++ = sizeof(struct ieee80211_ch_switch_timing);

	ch_sw = (void *)buf;
	ch_sw->switch_time = cpu_to_le16(switch_time);
	ch_sw->switch_timeout = cpu_to_le16(switch_timeout);
}

/* find switch timing IE in SKB ready for Tx */
static const u8 *ieee80211_tdls_find_sw_timing_ie(struct sk_buff *skb)
{
	struct ieee80211_tdls_data *tf;
	const u8 *ie_start;

	/*
	 * Get the offset for the new location of the switch timing IE.
	 * The SKB network header will now point to the "payload_type"
	 * element of the TDLS data frame struct.
	 */
	tf = container_of(skb->data + skb_network_offset(skb),
			  struct ieee80211_tdls_data, payload_type);
	ie_start = tf->u.chan_switch_req.variable;
	return cfg80211_find_ie(WLAN_EID_CHAN_SWITCH_TIMING, ie_start,
				skb->len - (ie_start - skb->data));
}

static struct sk_buff *
ieee80211_tdls_ch_sw_tmpl_get(struct sta_info *sta, u8 oper_class,
			      struct cfg80211_chan_def *chandef,
			      u32 *ch_sw_tm_ie_offset)
{
	struct ieee80211_sub_if_data *sdata = sta->sdata;
	u8 extra_ies[2 + sizeof(struct ieee80211_sec_chan_offs_ie) +
		     2 + sizeof(struct ieee80211_ch_switch_timing)];
	int extra_ies_len = 2 + sizeof(struct ieee80211_ch_switch_timing);
	u8 *pos = extra_ies;
	struct sk_buff *skb;
	int link_id = sta->sta.valid_links ? ffs(sta->sta.valid_links) - 1 : 0;

	/*
	 * if chandef points to a wide channel add a Secondary-Channel
	 * Offset information element
	 */
	if (chandef->width == NL80211_CHAN_WIDTH_40) {
		struct ieee80211_sec_chan_offs_ie *sec_chan_ie;
		bool ht40plus;

		*pos++ = WLAN_EID_SECONDARY_CHANNEL_OFFSET;
		*pos++ = sizeof(*sec_chan_ie);
		sec_chan_ie = (void *)pos;

		ht40plus = cfg80211_get_chandef_type(chandef) ==
							NL80211_CHAN_HT40PLUS;
		sec_chan_ie->sec_chan_offs = ht40plus ?
					     IEEE80211_HT_PARAM_CHA_SEC_ABOVE :
					     IEEE80211_HT_PARAM_CHA_SEC_BELOW;
		pos += sizeof(*sec_chan_ie);

		extra_ies_len += 2 + sizeof(struct ieee80211_sec_chan_offs_ie);
	}

	/* just set the values to 0, this is a template */
	iee80211_tdls_add_ch_switch_timing(pos, 0, 0);

	skb = ieee80211_tdls_build_mgmt_packet_data(sdata, sta->sta.addr,
					      link_id,
					      WLAN_TDLS_CHANNEL_SWITCH_REQUEST,
					      0, 0, !sta->sta.tdls_initiator,
					      extra_ies, extra_ies_len,
					      oper_class, chandef);
	if (!skb)
		return NULL;

	skb = ieee80211_build_data_template(sdata, skb, 0);
	if (IS_ERR(skb)) {
		tdls_dbg(sdata, "Failed building TDLS channel switch frame\n");
		return NULL;
	}

	if (ch_sw_tm_ie_offset) {
		const u8 *tm_ie = ieee80211_tdls_find_sw_timing_ie(skb);

		if (!tm_ie) {
			tdls_dbg(sdata, "No switch timing IE in TDLS switch\n");
			dev_kfree_skb_any(skb);
			return NULL;
		}

		*ch_sw_tm_ie_offset = tm_ie - skb->data;
	}

	tdls_dbg(sdata,
		 "TDLS channel switch request template for %pM ch %d width %d\n",
		 sta->sta.addr, chandef->chan->center_freq, chandef->width);
	return skb;
}

int
ieee80211_tdls_channel_switch(struct wiphy *wiphy, struct net_device *dev,
			      const u8 *addr, u8 oper_class,
			      struct cfg80211_chan_def *chandef)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;
	struct sk_buff *skb = NULL;
	u32 ch_sw_tm_ie;
	int ret;

	lockdep_assert_wiphy(local->hw.wiphy);

	if (chandef->chan->freq_offset)
		/* this may work, but is untested */
		return -EOPNOTSUPP;

	sta = sta_info_get(sdata, addr);
	if (!sta) {
		tdls_dbg(sdata,
			 "Invalid TDLS peer %pM for channel switch request\n",
			 addr);
		ret = -ENOENT;
		goto out;
	}

	if (!test_sta_flag(sta, WLAN_STA_TDLS_CHAN_SWITCH)) {
		tdls_dbg(sdata, "TDLS channel switch unsupported by %pM\n",
			 addr);
		ret = -EOPNOTSUPP;
		goto out;
	}

	skb = ieee80211_tdls_ch_sw_tmpl_get(sta, oper_class, chandef,
					    &ch_sw_tm_ie);
	if (!skb) {
		ret = -ENOENT;
		goto out;
	}

	ret = drv_tdls_channel_switch(local, sdata, &sta->sta, oper_class,
				      chandef, skb, ch_sw_tm_ie);
	if (!ret)
		set_sta_flag(sta, WLAN_STA_TDLS_OFF_CHANNEL);

out:
	dev_kfree_skb_any(skb);
	return ret;
}

void
ieee80211_tdls_cancel_channel_switch(struct wiphy *wiphy,
				     struct net_device *dev,
				     const u8 *addr)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;

	lockdep_assert_wiphy(local->hw.wiphy);

	sta = sta_info_get(sdata, addr);
	if (!sta) {
		tdls_dbg(sdata,
			 "Invalid TDLS peer %pM for channel switch cancel\n",
			 addr);
		return;
	}

	if (!test_sta_flag(sta, WLAN_STA_TDLS_OFF_CHANNEL)) {
		tdls_dbg(sdata, "TDLS channel switch not initiated by %pM\n",
			 addr);
		return;
	}

	drv_tdls_cancel_channel_switch(local, sdata, &sta->sta);
	clear_sta_flag(sta, WLAN_STA_TDLS_OFF_CHANNEL);
}

static struct sk_buff *
ieee80211_tdls_ch_sw_resp_tmpl_get(struct sta_info *sta,
				   u32 *ch_sw_tm_ie_offset)
{
	struct ieee80211_sub_if_data *sdata = sta->sdata;
	struct sk_buff *skb;
	u8 extra_ies[2 + sizeof(struct ieee80211_ch_switch_timing)];
	int link_id = sta->sta.valid_links ? ffs(sta->sta.valid_links) - 1 : 0;

	/* initial timing are always zero in the template */
	iee80211_tdls_add_ch_switch_timing(extra_ies, 0, 0);

	skb = ieee80211_tdls_build_mgmt_packet_data(sdata, sta->sta.addr,
					link_id,
					WLAN_TDLS_CHANNEL_SWITCH_RESPONSE,
					0, 0, !sta->sta.tdls_initiator,
					extra_ies, sizeof(extra_ies), 0, NULL);
	if (!skb)
		return NULL;

	skb = ieee80211_build_data_template(sdata, skb, 0);
	if (IS_ERR(skb)) {
		tdls_dbg(sdata,
			 "Failed building TDLS channel switch resp frame\n");
		return NULL;
	}

	if (ch_sw_tm_ie_offset) {
		const u8 *tm_ie = ieee80211_tdls_find_sw_timing_ie(skb);

		if (!tm_ie) {
			tdls_dbg(sdata,
				 "No switch timing IE in TDLS switch resp\n");
			dev_kfree_skb_any(skb);
			return NULL;
		}

		*ch_sw_tm_ie_offset = tm_ie - skb->data;
	}

	tdls_dbg(sdata, "TDLS get channel switch response template for %pM\n",
		 sta->sta.addr);
	return skb;
}

static int
ieee80211_process_tdls_channel_switch_resp(struct ieee80211_sub_if_data *sdata,
					   struct sk_buff *skb)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee802_11_elems *elems = NULL;
	struct sta_info *sta;
	struct ieee80211_tdls_data *tf = (void *)skb->data;
	bool local_initiator;
	struct ieee80211_rx_status *rx_status = IEEE80211_SKB_RXCB(skb);
	int baselen = offsetof(typeof(*tf), u.chan_switch_resp.variable);
	struct ieee80211_tdls_ch_sw_params params = {};
	int ret;

	lockdep_assert_wiphy(local->hw.wiphy);

	params.action_code = WLAN_TDLS_CHANNEL_SWITCH_RESPONSE;
	params.timestamp = rx_status->device_timestamp;

	if (skb->len < baselen) {
		tdls_dbg(sdata, "TDLS channel switch resp too short: %d\n",
			 skb->len);
		return -EINVAL;
	}

	sta = sta_info_get(sdata, tf->sa);
	if (!sta || !test_sta_flag(sta, WLAN_STA_TDLS_PEER_AUTH)) {
		tdls_dbg(sdata, "TDLS chan switch from non-peer sta %pM\n",
			 tf->sa);
		ret = -EINVAL;
		goto out;
	}

	params.sta = &sta->sta;
	params.status = le16_to_cpu(tf->u.chan_switch_resp.status_code);
	if (params.status != 0) {
		ret = 0;
		goto call_drv;
	}

	elems = ieee802_11_parse_elems(tf->u.chan_switch_resp.variable,
				       skb->len - baselen, false, NULL);
	if (!elems) {
		ret = -ENOMEM;
		goto out;
	}

	if (elems->parse_error) {
		tdls_dbg(sdata, "Invalid IEs in TDLS channel switch resp\n");
		ret = -EINVAL;
		goto out;
	}

	if (!elems->ch_sw_timing || !elems->lnk_id) {
		tdls_dbg(sdata, "TDLS channel switch resp - missing IEs\n");
		ret = -EINVAL;
		goto out;
	}

	/* validate the initiator is set correctly */
	local_initiator =
		!memcmp(elems->lnk_id->init_sta, sdata->vif.addr, ETH_ALEN);
	if (local_initiator == sta->sta.tdls_initiator) {
		tdls_dbg(sdata, "TDLS chan switch invalid lnk-id initiator\n");
		ret = -EINVAL;
		goto out;
	}

	params.switch_time = le16_to_cpu(elems->ch_sw_timing->switch_time);
	params.switch_timeout = le16_to_cpu(elems->ch_sw_timing->switch_timeout);

	params.tmpl_skb =
		ieee80211_tdls_ch_sw_resp_tmpl_get(sta, &params.ch_sw_tm_ie);
	if (!params.tmpl_skb) {
		ret = -ENOENT;
		goto out;
	}

	ret = 0;
call_drv:
	drv_tdls_recv_channel_switch(sdata->local, sdata, &params);

	tdls_dbg(sdata,
		 "TDLS channel switch response received from %pM status %d\n",
		 tf->sa, params.status);

out:
	dev_kfree_skb_any(params.tmpl_skb);
	kfree(elems);
	return ret;
}

static int
ieee80211_process_tdls_channel_switch_req(struct ieee80211_sub_if_data *sdata,
					  struct sk_buff *skb)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee802_11_elems *elems;
	struct cfg80211_chan_def chandef;
	struct ieee80211_channel *chan;
	enum nl80211_channel_type chan_type;
	int freq;
	u8 target_channel, oper_class;
	bool local_initiator;
	struct sta_info *sta;
	enum nl80211_band band;
	struct ieee80211_tdls_data *tf = (void *)skb->data;
	struct ieee80211_rx_status *rx_status = IEEE80211_SKB_RXCB(skb);
	int baselen = offsetof(typeof(*tf), u.chan_switch_req.variable);
	struct ieee80211_tdls_ch_sw_params params = {};
	int ret = 0;

	lockdep_assert_wiphy(local->hw.wiphy);

	params.action_code = WLAN_TDLS_CHANNEL_SWITCH_REQUEST;
	params.timestamp = rx_status->device_timestamp;

	if (skb->len < baselen) {
		tdls_dbg(sdata, "TDLS channel switch req too short: %d\n",
			 skb->len);
		return -EINVAL;
	}

	target_channel = tf->u.chan_switch_req.target_channel;
	oper_class = tf->u.chan_switch_req.oper_class;

	/*
	 * We can't easily infer the channel band. The operating class is
	 * ambiguous - there are multiple tables (US/Europe/JP/Global). The
	 * solution here is to treat channels with number >14 as 5GHz ones,
	 * and specifically check for the (oper_class, channel) combinations
	 * where this doesn't hold. These are thankfully unique according to
	 * IEEE802.11-2012.
	 * We consider only the 2GHz and 5GHz bands and 20MHz+ channels as
	 * valid here.
	 */
	if ((oper_class == 112 || oper_class == 2 || oper_class == 3 ||
	     oper_class == 4 || oper_class == 5 || oper_class == 6) &&
	     target_channel < 14)
		band = NL80211_BAND_5GHZ;
	else
		band = target_channel < 14 ? NL80211_BAND_2GHZ :
					     NL80211_BAND_5GHZ;

	freq = ieee80211_channel_to_frequency(target_channel, band);
	if (freq == 0) {
		tdls_dbg(sdata, "Invalid channel in TDLS chan switch: %d\n",
			 target_channel);
		return -EINVAL;
	}

	chan = ieee80211_get_channel(sdata->local->hw.wiphy, freq);
	if (!chan) {
		tdls_dbg(sdata,
			 "Unsupported channel for TDLS chan switch: %d\n",
			 target_channel);
		return -EINVAL;
	}

	elems = ieee802_11_parse_elems(tf->u.chan_switch_req.variable,
				       skb->len - baselen, false, NULL);
	if (!elems)
		return -ENOMEM;

	if (elems->parse_error) {
		tdls_dbg(sdata, "Invalid IEs in TDLS channel switch req\n");
		ret = -EINVAL;
		goto free;
	}

	if (!elems->ch_sw_timing || !elems->lnk_id) {
		tdls_dbg(sdata, "TDLS channel switch req - missing IEs\n");
		ret = -EINVAL;
		goto free;
	}

	if (!elems->sec_chan_offs) {
		chan_type = NL80211_CHAN_HT20;
	} else {
		switch (elems->sec_chan_offs->sec_chan_offs) {
		case IEEE80211_HT_PARAM_CHA_SEC_ABOVE:
			chan_type = NL80211_CHAN_HT40PLUS;
			break;
		case IEEE80211_HT_PARAM_CHA_SEC_BELOW:
			chan_type = NL80211_CHAN_HT40MINUS;
			break;
		default:
			chan_type = NL80211_CHAN_HT20;
			break;
		}
	}

	cfg80211_chandef_create(&chandef, chan, chan_type);

	/* we will be active on the TDLS link */
	if (!cfg80211_reg_can_beacon_relax(sdata->local->hw.wiphy, &chandef,
					   sdata->wdev.iftype)) {
		tdls_dbg(sdata, "TDLS chan switch to forbidden channel\n");
		ret = -EINVAL;
		goto free;
	}

	sta = sta_info_get(sdata, tf->sa);
	if (!sta || !test_sta_flag(sta, WLAN_STA_TDLS_PEER_AUTH)) {
		tdls_dbg(sdata, "TDLS chan switch from non-peer sta %pM\n",
			 tf->sa);
		ret = -EINVAL;
		goto out;
	}

	params.sta = &sta->sta;

	/* validate the initiator is set correctly */
	local_initiator =
		!memcmp(elems->lnk_id->init_sta, sdata->vif.addr, ETH_ALEN);
	if (local_initiator == sta->sta.tdls_initiator) {
		tdls_dbg(sdata, "TDLS chan switch invalid lnk-id initiator\n");
		ret = -EINVAL;
		goto out;
	}

	/* peer should have known better */
	if (!sta->sta.deflink.ht_cap.ht_supported && elems->sec_chan_offs &&
	    elems->sec_chan_offs->sec_chan_offs) {
		tdls_dbg(sdata, "TDLS chan switch - wide chan unsupported\n");
		ret = -EOPNOTSUPP;
		goto out;
	}

	params.chandef = &chandef;
	params.switch_time = le16_to_cpu(elems->ch_sw_timing->switch_time);
	params.switch_timeout = le16_to_cpu(elems->ch_sw_timing->switch_timeout);

	params.tmpl_skb =
		ieee80211_tdls_ch_sw_resp_tmpl_get(sta,
						   &params.ch_sw_tm_ie);
	if (!params.tmpl_skb) {
		ret = -ENOENT;
		goto out;
	}

	drv_tdls_recv_channel_switch(sdata->local, sdata, &params);

	tdls_dbg(sdata,
		 "TDLS ch switch request received from %pM ch %d width %d\n",
		 tf->sa, params.chandef->chan->center_freq,
		 params.chandef->width);
out:
	dev_kfree_skb_any(params.tmpl_skb);
free:
	kfree(elems);
	return ret;
}

void
ieee80211_process_tdls_channel_switch(struct ieee80211_sub_if_data *sdata,
				      struct sk_buff *skb)
{
	struct ieee80211_tdls_data *tf = (void *)skb->data;
	struct wiphy *wiphy = sdata->local->hw.wiphy;

	lockdep_assert_wiphy(wiphy);

	/* make sure the driver supports it */
	if (!(wiphy->features & NL80211_FEATURE_TDLS_CHANNEL_SWITCH))
		return;

	/* we want to access the entire packet */
	if (skb_linearize(skb))
		return;
	/*
	 * The packet/size was already validated by mac80211 Rx path, only look
	 * at the action type.
	 */
	switch (tf->action_code) {
	case WLAN_TDLS_CHANNEL_SWITCH_REQUEST:
		ieee80211_process_tdls_channel_switch_req(sdata, skb);
		break;
	case WLAN_TDLS_CHANNEL_SWITCH_RESPONSE:
		ieee80211_process_tdls_channel_switch_resp(sdata, skb);
		break;
	default:
		WARN_ON_ONCE(1);
		return;
	}
}

void ieee80211_teardown_tdls_peers(struct ieee80211_link_data *link)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct sta_info *sta;
	u16 reason = WLAN_REASON_TDLS_TEARDOWN_UNSPECIFIED;

	rcu_read_lock();
	list_for_each_entry_rcu(sta, &sdata->local->sta_list, list) {
		if (!sta->sta.tdls || sta->sdata != sdata || !sta->uploaded ||
		    !test_sta_flag(sta, WLAN_STA_AUTHORIZED))
			continue;

		if (sta->deflink.link_id != link->link_id)
			continue;

		ieee80211_tdls_oper_request(&sdata->vif, sta->sta.addr,
					    NL80211_TDLS_TEARDOWN, reason,
					    GFP_ATOMIC);
	}
	rcu_read_unlock();
}

void ieee80211_tdls_handle_disconnect(struct ieee80211_sub_if_data *sdata,
				      const u8 *peer, u16 reason)
{
	struct ieee80211_sta *sta;

	rcu_read_lock();
	sta = ieee80211_find_sta(&sdata->vif, peer);
	if (!sta || !sta->tdls) {
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();

	tdls_dbg(sdata, "disconnected from TDLS peer %pM (Reason: %u=%s)\n",
		 peer, reason,
		 ieee80211_get_reason_code_string(reason));

	ieee80211_tdls_oper_request(&sdata->vif, peer,
				    NL80211_TDLS_TEARDOWN,
				    WLAN_REASON_TDLS_TEARDOWN_UNREACHABLE,
				    GFP_ATOMIC);
}
