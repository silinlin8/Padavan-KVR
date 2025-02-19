/*
 ***************************************************************************
 * Ralink Tech Inc.
 * 4F, No. 2 Technology 5th Rd.
 * Science-based Industrial Park
 * Hsin-chu, Taiwan, R.O.C.
 *
 * (c) Copyright 2002-2006, Ralink Technology, Inc.
 *
 * All rights reserved. Ralink's source code is an unpublished work and the
 * use of a copyright notice does not imply otherwise. This source code
 * contains confidential trade secret material of Ralink Tech. Any attemp
 * or participation in deciphering, decoding, reverse engineering or in any
 * way altering the source code is stricitly prohibited, unless the prior
 * written consent of Ralink Technology, Inc. is obtained.
 ***************************************************************************

    Module Name:
    ap_apcli.c

    Abstract:
    Support AP-Client function.

    Note:
    1. Call RT28xx_ApCli_Init() in init function and
       call RT28xx_ApCli_Remove() in close function

    2. MAC of ApCli-interface is initialized in RT28xx_ApCli_Init()

    3. ApCli index (0) of different rx packet is got in

    4. ApCli index (0) of different tx packet is assigned in

    5. ApCli index (0) of different interface is got in APHardTransmit() by using

    6. ApCli index (0) of IOCTL command is put in pAd->OS_Cookie->ioctl_if

    8. The number of ApCli only can be 1

	9. apcli convert engine subroutines, we should just take care data packet.
    Revision History:
    Who             When            What
    --------------  ----------      ----------------------------------------------
    Shiang, Fonchi  02-13-2007      created
*/

#ifdef APCLI_SUPPORT

#include "rt_config.h"
#ifdef ROAMING_ENHANCE_SUPPORT
#include <net/arp.h>
#endif /* ROAMING_ENHANCE_SUPPORT */

#ifdef MAC_REPEATER_SUPPORT
VOID ReptWaitLinkDown(REPEATER_CLIENT_ENTRY *pReptEntry);
#endif /*MAC_REPEATER_SUPPORT*/



BOOLEAN ApCliWaitProbRsp(PRTMP_ADAPTER pAd, USHORT ifIndex)
{
        if (ifIndex >= MAX_APCLI_NUM)
                return FALSE;

        return (pAd->ApCfg.ApCliTab[ifIndex].SyncCurrState == APCLI_JOIN_WAIT_PROBE_RSP) ?
                TRUE : FALSE;
}

VOID ApCliSimulateRecvBeacon(RTMP_ADAPTER *pAd)
{
	INT loop;
        ULONG Now32, BPtoJiffies;
        PAPCLI_STRUCT pApCliEntry = NULL;
	LONG timeDiff;

	NdisGetSystemUpTime(&Now32);
        for (loop = 0; loop < MAX_APCLI_NUM; loop++)
        {
        	pApCliEntry = &pAd->ApCfg.ApCliTab[loop];
                if ((pApCliEntry->Valid == TRUE) && (VALID_UCAST_ENTRY_WCID(pAd, pApCliEntry->MacTabWCID)))
                {
                	/*
                          When we are connected and do the scan progress, it's very possible we cannot receive
                          the beacon of the AP. So, here we simulate that we received the beacon.
                         */
                        if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BSS_SCAN_IN_PROGRESS) &&
                            (RTMP_TIME_AFTER(pAd->Mlme.Now32, pApCliEntry->ApCliRcvBeaconTime + (60 * OS_HZ))))
                        {
                        	BPtoJiffies = (((pApCliEntry->ApCliBeaconPeriod * 1024 / 1000) * OS_HZ) / 1000);
                                timeDiff = (pAd->Mlme.Now32 - pApCliEntry->ApCliRcvBeaconTime) / BPtoJiffies;
                                if (timeDiff > 0)
                                	pApCliEntry->ApCliRcvBeaconTime += (timeDiff * BPtoJiffies);

                                if (RTMP_TIME_AFTER(pApCliEntry->ApCliRcvBeaconTime, pAd->Mlme.Now32))
                                {
                                	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("MMCHK - APCli BeaconRxTime adjust wrong(BeaconRx=0x%lx, Now=0x%lx)\n",
                                                                   pApCliEntry->ApCliRcvBeaconTime, pAd->Mlme.Now32));
                                }
                        }

                        /* update channel quality for Roaming and UI LinkQuality display */
                        MlmeCalculateChannelQuality(pAd, &pAd->MacTab.Content[pApCliEntry->MacTabWCID], Now32);
		}
	}
}


/*
* Synchronized function
*/

static VOID ApCliCompleteInit(APCLI_STRUCT *pApCliEntry)
{
	RTMP_OS_INIT_COMPLETION(&pApCliEntry->ifdown_complete);
	RTMP_OS_INIT_COMPLETION(&pApCliEntry->linkdown_complete);	
}

static VOID ApCliLinkDownComplete(APCLI_STRUCT *pApCliEntry)
{
	RTMP_OS_COMPLETE(&pApCliEntry->linkdown_complete);
}

static VOID ApCliWaitLinkDown(APCLI_STRUCT *pApCliEntry)
{
	if(pApCliEntry->Valid && !RTMP_OS_WAIT_FOR_COMPLETION_TIMEOUT(&pApCliEntry->linkdown_complete,APCLI_WAIT_TIMEOUT))
	{
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
		("(%s) ApCli [%d] can't done.\n", __FUNCTION__, pApCliEntry->wdev.func_idx));
	}
}

static VOID ApCliWaitStateDisconnect(APCLI_STRUCT *pApCliEntry)
{
	/* 
	 * Before doing WifiSysClose,we have to make sure the ctrl
	* state machine has switched to APCLI_CTRL_DISCONNECTED 
	 */
	int wait_cnt=0;
	int wait_times=50;
	int delay_time=100;
	while(pApCliEntry->CtrlCurrState != APCLI_CTRL_DISCONNECTED) {																
	  if (wait_cnt >= wait_times) {					 
					 MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
					 	("Need to debug apcli ctrl state machine(Ctrl State=%lu)\n\r",pApCliEntry->CtrlCurrState));
					 break;
	  }
	  os_msec_delay(delay_time);
	  wait_cnt++;
	}
}


static VOID ApCliIfDownComplete(APCLI_STRUCT *pApCliEntry)
{
	RTMP_OS_COMPLETE(&pApCliEntry->ifdown_complete);
}

static VOID ApCliWaitIfDown(APCLI_STRUCT *pApCliEntry)
{
	if(pApCliEntry->Valid  && 
		!RTMP_OS_WAIT_FOR_COMPLETION_TIMEOUT(&pApCliEntry->ifdown_complete,APCLI_WAIT_TIMEOUT))
	{					
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, 
			("%s: wait ApCli [%d] interface down faild!!\n", __FUNCTION__,pApCliEntry->wdev.func_idx));
	}
}



/*
========================================================================
Routine Description:
    Close ApCli network interface.

Arguments:
    ad_p            points to our adapter

Return Value:
    None

Note:
========================================================================
*/
VOID RT28xx_ApCli_Close(RTMP_ADAPTER *ad_p)
{
	UINT index;


	for(index = 0; index < MAX_APCLI_NUM; index++)
	{
		if (ad_p->ApCfg.ApCliTab[index].wdev.if_dev)
			RtmpOSNetDevClose(ad_p->ApCfg.ApCliTab[index].wdev.if_dev);
	}

}


/* --------------------------------- Private -------------------------------- */
INT ApCliIfLookUp(RTMP_ADAPTER *pAd, UCHAR *pAddr)
{
	SHORT if_idx;

	for(if_idx = 0; if_idx < MAX_APCLI_NUM; if_idx++)
	{
		if(MAC_ADDR_EQUAL(pAd->ApCfg.ApCliTab[if_idx].wdev.if_addr, pAddr))
		{
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("%s():ApCliIfIndex=%d\n",
						__FUNCTION__, if_idx));
			return if_idx;
		}
	}

	return -1;
}


BOOLEAN isValidApCliIf(SHORT if_idx)
{
	return (((if_idx >= 0) && (if_idx < MAX_APCLI_NUM)) ? TRUE : FALSE);
}


/*! \brief init the management mac frame header
 *  \param p_hdr mac header
 *  \param subtype subtype of the frame
 *  \param p_ds destination address, don't care if it is a broadcast address
 *  \return none
 *  \pre the station has the following information in the pAd->UserCfg
 *   - bssid
 *   - station address
 *  \post
 *  \note this function initializes the following field
 */
VOID ApCliMgtMacHeaderInit(
    IN RTMP_ADAPTER *pAd,
    INOUT HEADER_802_11 *pHdr80211,
    IN UCHAR SubType,
    IN UCHAR ToDs,
    IN UCHAR *pDA,
    IN UCHAR *pBssid,
    IN USHORT ifIndex)
{
    NdisZeroMemory(pHdr80211, sizeof(HEADER_802_11));
    pHdr80211->FC.Type = FC_TYPE_MGMT;
    pHdr80211->FC.SubType = SubType;
    pHdr80211->FC.ToDs = ToDs;
    COPY_MAC_ADDR(pHdr80211->Addr1, pDA);
    COPY_MAC_ADDR(pHdr80211->Addr2, pAd->ApCfg.ApCliTab[ifIndex].wdev.if_addr);
    COPY_MAC_ADDR(pHdr80211->Addr3, pBssid);
}


#ifdef DOT11_N_SUPPORT
/*
	========================================================================

	Routine Description:
		Verify the support rate for HT phy type

	Arguments:
		pAd 				Pointer to our adapter

	Return Value:
		FALSE if pAd->CommonCfg.SupportedHtPhy doesn't accept the pHtCapability.  (AP Mode)

	IRQL = PASSIVE_LEVEL

	========================================================================
*/
BOOLEAN ApCliCheckHt(
	IN RTMP_ADAPTER *pAd,
	IN USHORT IfIndex,
	INOUT HT_CAPABILITY_IE *pHtCapability,
	INOUT ADD_HT_INFO_IE *pAddHtInfo)
{
	APCLI_STRUCT *pApCliEntry = NULL;
	HT_CAPABILITY_IE *aux_ht_cap;
	RT_HT_CAPABILITY *rt_ht_cap = &pAd->CommonCfg.DesiredHtPhy;
	struct wifi_dev *wdev;
	UCHAR cfg_ht_bw;
	UCHAR op_ext_cha;

	if (IfIndex >= MAX_APCLI_NUM)
		return FALSE;

	pApCliEntry = &pAd->ApCfg.ApCliTab[IfIndex];
	wdev = &pApCliEntry->wdev;
	cfg_ht_bw = wlan_config_get_ht_bw(wdev);
	op_ext_cha = wlan_operate_get_ext_cha(wdev);

	aux_ht_cap = &pApCliEntry->MlmeAux.HtCapability;
	aux_ht_cap->MCSSet[0] = 0xff;
	aux_ht_cap->MCSSet[4] = 0x1;
	 switch (wlan_config_get_rx_stream(wdev))
	{
		case 1:
			aux_ht_cap->MCSSet[0] = 0xff;
			aux_ht_cap->MCSSet[1] = 0x00;
			aux_ht_cap->MCSSet[2] = 0x00;
			aux_ht_cap->MCSSet[3] = 0x00;
			break;
		case 2:
			aux_ht_cap->MCSSet[0] = 0xff;
			aux_ht_cap->MCSSet[1] = 0xff;
			aux_ht_cap->MCSSet[2] = 0x00;
			aux_ht_cap->MCSSet[3] = 0x00;
			break;
		case 3:
			aux_ht_cap->MCSSet[0] = 0xff;
			aux_ht_cap->MCSSet[1] = 0xff;
			aux_ht_cap->MCSSet[2] = 0xff;
			aux_ht_cap->MCSSet[3] = 0x00;
			break;
		case 4:
			aux_ht_cap->MCSSet[0] = 0xff;
			aux_ht_cap->MCSSet[1] = 0xff;
			aux_ht_cap->MCSSet[2] = 0xff;
			aux_ht_cap->MCSSet[3] = 0xff;
			break;
	}

	aux_ht_cap->MCSSet[0] &= pHtCapability->MCSSet[0];
	aux_ht_cap->MCSSet[1] &= pHtCapability->MCSSet[1];
	aux_ht_cap->MCSSet[2] &= pHtCapability->MCSSet[2];
	aux_ht_cap->MCSSet[3] &= pHtCapability->MCSSet[3];

	/* Record the RxMcs of AP */
	NdisMoveMemory(pApCliEntry->RxMcsSet, pHtCapability->MCSSet, 16);

	/* choose smaller setting */
#ifdef CONFIG_MULTI_CHANNEL
	aux_ht_cap->HtCapInfo.ChannelWidth = pAddHtInfo->AddHtInfo.RecomWidth;
#else /* CONFIG_MULTI_CHANNEL */
	aux_ht_cap->HtCapInfo.ChannelWidth = pAddHtInfo->AddHtInfo.RecomWidth & cfg_ht_bw;
#endif /* !CONFIG_MULTI_CHANNEL */

	//it should go back to bw 20 if extension channel is different with root ap
	if (op_ext_cha != pAddHtInfo->AddHtInfo.ExtChanOffset) {
		if (pApCliEntry->wdev.channel > 14) {
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("ApCliCheckHt :  channel=%u,  my extcha=%u, root ap extcha=%u, inconsistent!!\n",
				pApCliEntry->wdev.channel, op_ext_cha, pAddHtInfo->AddHtInfo.ExtChanOffset));
		}
		aux_ht_cap->HtCapInfo.ChannelWidth = BW_20;
		wlan_operate_set_ht_bw(wdev,HT_BW_20);
		wlan_operate_set_ext_cha(wdev,EXTCHA_NONE);
	}else{
		wlan_operate_set_ext_cha(wdev,pAddHtInfo->AddHtInfo.ExtChanOffset);
	}

	aux_ht_cap->HtCapInfo.GF =  pHtCapability->HtCapInfo.GF & rt_ht_cap->GF;

#ifdef CONFIG_MULTI_CHANNEL //APCLI's bw , Central , channel
	if (RTMP_CFG80211_VIF_P2P_CLI_ON(pAd))
	{
		UCHAR ht_bw = aux_ht_cap->HtCapInfo.ChannelWidth;
		wlan_operate_set_ht_bw(&pApCliEntry->wdev,ht_bw);
		if (ht_bw== HT_BW_20)
		{
			pApCliEntry->wdev.channel = pAddHtInfo->ControlChan;
			pApCliEntry->wdev.CentralChannel = pApCliEntry->wdev.channel;
			wlan_operate_set_ext_cha(wdev,EXTCHA_NONE);
		}
		else if (ht_bw == HT_BW_40)
		{
			pApCliEntry->wdev.channel = pAddHtInfo->ControlChan;

			if (pAddHtInfo->AddHtInfo.ExtChanOffset == EXTCHA_ABOVE )
			{
				wlan_operate_set_ext_cha(wdev,EXTCHA_ABOVE);
				pApCliEntry->wdev.CentralChannel = pApCliEntry->wdev.channel + 2;
			}
			else if (pAddHtInfo->AddHtInfo.ExtChanOffset == EXTCHA_BELOW)
			{
				wlan_operate_set_ext_cha(wdev,EXTCHA_BELOW);
				pApCliEntry->wdev.CentralChannel = pApCliEntry->wdev.channel - 2;
			}
			else /* EXTCHA_NONE , should not be here!*/
			{
				wlan_operate_set_ext_cha(wdev,EXTCHA_NONE);
				pApCliEntry->wdev.CentralChannel = pApCliEntry->wdev.channel;
			}
		}
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("ApCliCheckHt :  channel=%u,  CentralChannel=%u, bw=%u\n", pApCliEntry->wdev.channel, pApCliEntry->wdev.CentralChannel, bw));

	}
#endif /* CONFIG_MULTI_CHANNEL */


	/* Send Assoc Req with my HT capability. */
	aux_ht_cap->HtCapInfo.AMsduSize =  rt_ht_cap->AmsduSize;
	aux_ht_cap->HtCapInfo.MimoPs = pHtCapability->HtCapInfo.MimoPs;
	aux_ht_cap->HtCapInfo.ShortGIfor20 =  (rt_ht_cap->ShortGIfor20) & (pHtCapability->HtCapInfo.ShortGIfor20);
	aux_ht_cap->HtCapInfo.ShortGIfor40 =  (rt_ht_cap->ShortGIfor40) & (pHtCapability->HtCapInfo.ShortGIfor40);
	aux_ht_cap->HtCapInfo.TxSTBC =  (rt_ht_cap->TxSTBC)&(pHtCapability->HtCapInfo.RxSTBC);
	aux_ht_cap->HtCapInfo.RxSTBC =  (rt_ht_cap->RxSTBC)&(pHtCapability->HtCapInfo.TxSTBC);

	/* Fix throughput issue for some vendor AP with AES mode */
	if (pAddHtInfo->AddHtInfo.RecomWidth & cfg_ht_bw)
		aux_ht_cap->HtCapInfo.CCKmodein40 = pHtCapability->HtCapInfo.CCKmodein40;
	else 
		aux_ht_cap->HtCapInfo.CCKmodein40 = 0;
	
	aux_ht_cap->HtCapParm.MaxRAmpduFactor = rt_ht_cap->MaxRAmpduFactor;
	aux_ht_cap->HtCapParm.MpduDensity = pHtCapability->HtCapParm.MpduDensity;
	aux_ht_cap->ExtHtCapInfo.PlusHTC = pHtCapability->ExtHtCapInfo.PlusHTC;
	if (pAd->CommonCfg.bRdg)
	{
		aux_ht_cap->ExtHtCapInfo.RDGSupport = pHtCapability->ExtHtCapInfo.RDGSupport;
	}

	if (wlan_config_get_ht_ldpc(wdev) && (pAd->chipCap.phy_caps & fPHY_CAP_LDPC)) {
		aux_ht_cap->HtCapInfo.ht_rx_ldpc = pHtCapability->HtCapInfo.ht_rx_ldpc;
	} else {
		aux_ht_cap->HtCapInfo.ht_rx_ldpc = 0;
	}

	/*COPY_AP_HTSETTINGS_FROM_BEACON(pAd, pHtCapability); */
	return TRUE;
}
#endif /* DOT11_N_SUPPORT */

#ifdef APCLI_CERT_SUPPORT
void ApCliCertEDCAAdjust(RTMP_ADAPTER *pAd, struct wifi_dev *wdev,PEDCA_PARM pEdcaParm)
{
	UCHAR       Cwmin[WMM_NUM_OF_AC] = {3,3,3,3};
	UCHAR       Cwmax[WMM_NUM_OF_AC] = {4,4,4,4};
	if ((memcmp(pEdcaParm->Cwmin,Cwmin,4) == 0) &&
		(memcmp(pEdcaParm->Cwmax,Cwmax,4) == 0)) {
		/* ignore 5.2.32*/
		return;
	}
	/* 
	 *	fix 5.2.29 step 7 fail
	 */
	if ((pAd->bApCliCertTest == TRUE) && 
			(wdev->func_type == OMAC_TYPE_APCLI)) {
		if ((pEdcaParm->Cwmin[2] == 3) &&
				(pEdcaParm->Cwmax[2] == 4)) {
			pEdcaParm->Cwmin[2]++;
			pEdcaParm->Cwmax[2]++;
			if (pEdcaParm->Txop[2] == 94) {
				pEdcaParm->Txop[2] = pEdcaParm->Txop[2] - 9;
			}
		}
	}
	return;
}
#endif
/*
    ==========================================================================

	Routine	Description:
		Connected to the BSSID

	Arguments:
		pAd				- Pointer to our adapter
		ApCliIdx		- Which ApCli interface
	Return Value:
		FALSE: fail to alloc Mac entry.

	Note:

	==========================================================================
*/
BOOLEAN ApCliLinkUp(RTMP_ADAPTER *pAd, UCHAR ifIndex)
{
	BOOLEAN result = FALSE;
	PAPCLI_STRUCT pApCliEntry = NULL;
	PMAC_TABLE_ENTRY pMacEntry = NULL;
	STA_TR_ENTRY *tr_entry;
	struct wifi_dev *wdev;
#if defined(MAC_REPEATER_SUPPORT) || defined(MT_MAC)
	UCHAR CliIdx = 0xFF;
#ifdef MAC_REPEATER_SUPPORT
	INVAILD_TRIGGER_MAC_ENTRY *pSkipEntry = NULL;
    REPEATER_CLIENT_ENTRY *pReptEntry = NULL;
#endif /* MAC_REPEATER_SUPPORT */
#endif /* defined(MAC_REPEATER_SUPPORT) || defined(MT_MAC) */
#ifdef APCLI_AUTO_CONNECT_SUPPORT
	USHORT apcli_ifIndex;
#endif
#ifdef MWDS
	UCHAR ApCliIdx = 0; /* default use apcli0 */
#endif /* MWDS */

	do
	{
#ifdef MWDS
		if (ifIndex < MAX_APCLI_NUM)
			ApCliIdx = ifIndex;
#ifdef MAC_REPEATER_SUPPORT
		else if(ifIndex >= REPT_MLME_START_IDX)
        {
            pReptEntry = &pAd->ApCfg.pRepeaterCliPool[(ifIndex - REPT_MLME_START_IDX)];
            ApCliIdx = pReptEntry->wdev->func_idx;
            pReptEntry = NULL;
        }
#endif /* MAC_REPEATER_SUPPORT */
#endif /* MWDS */

		if ((ifIndex < MAX_APCLI_NUM)
#ifdef MAC_REPEATER_SUPPORT
			|| (ifIndex >= REPT_MLME_START_IDX)
#endif /* MAC_REPEATER_SUPPORT */
		)
		{
#ifdef MAC_REPEATER_SUPPORT
			if ((pAd->ApCfg.bMACRepeaterEn)
#ifdef MWDS
				&& ((pAd->ApCfg.ApCliTab[ApCliIdx].wdev.bSupportMWDS == FALSE) ||
					(pAd->ApCfg.ApCliTab[ApCliIdx].MlmeAux.bSupportMWDS == FALSE))
#endif /* MWDS */			

			)
			{
				if (ifIndex < MAX_APCLI_NUM)
				{
#ifdef LINUX
					struct net_device *pNetDev;
					struct net *net= &init_net;

/* old kernerl older than 2.6.21 didn't have for_each_netdev()*/
#ifndef for_each_netdev
					for(pNetDev=dev_base; pNetDev!=NULL; pNetDev=pNetDev->next)
#else
					for_each_netdev(net, pNetDev)
#endif
					{
						if (pNetDev->priv_flags == IFF_EBRIDGE)
						{
							COPY_MAC_ADDR(pAd->ApCfg.BridgeAddress, pNetDev->dev_addr);
							MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, (" Bridge Addr = %02X:%02X:%02X:%02X:%02X:%02X. !!!\n",
										PRINT_MAC(pAd->ApCfg.BridgeAddress)));
						}
						pSkipEntry = RepeaterInvaildMacLookup(pAd, pNetDev->dev_addr);

                        if (pSkipEntry == NULL)
                            InsertIgnoreAsRepeaterEntryTable(pAd, pNetDev->dev_addr);
                    }
					if (!MAC_ADDR_EQUAL(pAd->ApCfg.BridgeAddress, ZERO_MAC_ADDR))
					{
						pSkipEntry = RepeaterInvaildMacLookup(pAd, pAd->ApCfg.BridgeAddress);

						if (pSkipEntry)
						{
							UCHAR MacAddr[MAC_ADDR_LEN];
							UCHAR entry_idx;

							COPY_MAC_ADDR(MacAddr, pSkipEntry->MacAddr);
							entry_idx = pSkipEntry->entry_idx;
							RepeaterRemoveIngoreEntry(pAd, entry_idx, MacAddr);
						}
					}
#endif
				}

				if (ifIndex >= REPT_MLME_START_IDX)
				{
					CliIdx = ifIndex - REPT_MLME_START_IDX;
                    pReptEntry = &pAd->ApCfg.pRepeaterCliPool[CliIdx];
					ifIndex = pReptEntry->wdev->func_idx;

					pMacEntry = MacTableLookup(pAd, pReptEntry->OriginalAddress);
					if (pMacEntry && IS_ENTRY_CLIENT(pMacEntry))
						pReptEntry->bEthCli = FALSE;
					else
						pReptEntry->bEthCli = TRUE;

					pMacEntry = NULL;
				}
			}
#endif /* MAC_REPEATER_SUPPORT */

		}
		else
		{
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
                    ("!!! ERROR : APCLI LINK UP - IF(apcli%d)!!!\n", ifIndex));
			result = FALSE;
			break;
		}

#ifdef MAC_REPEATER_SUPPORT
		if ((CliIdx != 0xFF)
#ifdef MWDS
		&& ((pAd->ApCfg.ApCliTab[ApCliIdx].wdev.bSupportMWDS == FALSE) ||
			(pAd->ApCfg.ApCliTab[ApCliIdx].MlmeAux.bSupportMWDS == FALSE))
#endif /* MWDS */

		)
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
                    ("(%s) ifIndex = %d, CliIdx = %d !!!\n",
                                __FUNCTION__, ifIndex, CliIdx));
		else
#endif /* MAC_REPEATER_SUPPORT */
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
				 ("(%s) ifIndex = %d!!!\n",
				  __FUNCTION__, ifIndex));

#ifdef APCLI_AUTO_CONNECT_SUPPORT
		apcli_ifIndex = (USHORT)(ifIndex);
#endif
		pApCliEntry = &pAd->ApCfg.ApCliTab[ifIndex];

		if ((pApCliEntry->Valid)
#ifdef MAC_REPEATER_SUPPORT
			 && (CliIdx == 0xFF)
#endif /* MAC_REPEATER_SUPPORT */
			)
		{
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
                    ("!!! ERROR : This link had existed - IF(apcli%d)!!!\n",
                                ifIndex));
			result = FALSE;
			break;
		}

		wdev = &pApCliEntry->wdev;
#if defined(CONFIG_WIFI_PKT_FWD) || defined(CONFIG_WIFI_PKT_FWD_MODULE)
#ifdef MAC_REPEATER_SUPPORT
		if (CliIdx == 0xFF)
#endif /* MAC_REPEATER_SUPPORT */
		{
			if (wf_fwd_get_rep_hook)
				wf_fwd_get_rep_hook(pAd->CommonCfg.EtherTrafficBand);

			if (wf_fwd_check_device_hook)
				wf_fwd_check_device_hook(wdev->if_dev, INT_APCLI, CliIdx, wdev->channel, 1);			

			if (wf_fwd_entry_insert_hook)
				wf_fwd_entry_insert_hook (wdev->if_dev, pAd->net_dev, pAd);

			if (wf_fwd_insert_repeater_mapping_hook)
#ifdef MAC_REPEATER_SUPPORT
				wf_fwd_insert_repeater_mapping_hook (pAd, &pAd->ApCfg.ReptCliEntryLock, &pAd->ApCfg.ReptCliHash[0], &pAd->ApCfg.ReptMapHash[0], &pAd->ApCfg.ApCliTab[ifIndex].wdev.if_addr);
#else
				wf_fwd_insert_repeater_mapping_hook (pAd, NULL, NULL, NULL, &pAd->ApCfg.ApCliTab[ifIndex].wdev.if_addr);
#endif /* MAC_REPEATER_SUPPORT */
		}

#endif /* CONFIG_WIFI_PKT_FWD */
		/* Insert the Remote AP to our MacTable. */
		/*pMacEntry = MacTableInsertApCliEntry(pAd, (PUCHAR)(pAd->ApCfg.ApCliTab[0].MlmeAux.Bssid)); */
#ifdef RADIO_LINK_SELECTION
#ifdef MAC_REPEATER_SUPPORT
	if (CliIdx == 0xFF)
#endif /* MAC_REPEATER_SUPPORT */
	{
		if (pAd->ApCfg.RadioLinkSelection)
			Rls_InfCliLinkUp(pAd, wdev);
	}
#endif /* RADIO_LINK_SELECTION */

#ifdef FAST_EAPOL_WAR
#ifdef MAC_REPEATER_SUPPORT
		if ((pAd->ApCfg.bMACRepeaterEn) &&
			/*(pAd->chipCap.hif_type == HIF_MT) &&*/
			(CliIdx != 0xFF))
		{
			pReptEntry = &pAd->ApCfg.pRepeaterCliPool[CliIdx];
			if (pReptEntry->pre_entry_alloc == TRUE) {
				pMacEntry = &pAd->MacTab.Content[pReptEntry->MacTabWCID];
			}
		}
		else 
#endif /* MAC_REPEATER_SUPPORT */
		{
			if (pApCliEntry->pre_entry_alloc == TRUE) {
				pMacEntry = &pAd->MacTab.Content[pApCliEntry->MacTabWCID];
			}
		}
#else /* FAST_EAPOL_WAR */
#ifdef MAC_REPEATER_SUPPORT
#if defined(RTMP_MAC) || defined(RLT_MAC)
		if ((pAd->ApCfg.bMACRepeaterEn) &&
            (pAd->chipCap.hif_type == HIF_RLT ||
            pAd->chipCap.hif_type == HIF_RTMP)
        )
        {
			pMacEntry = RTMPInsertRepeaterMacEntry(
							pAd,
							(PUCHAR)(pApCliEntry->MlmeAux.Bssid),
							wdev,
							(ifIndex + MIN_NET_DEVICE_FOR_APCLI),
							CliIdx,
							TRUE);
        }
        else 
#endif /* RTMP_MAC || RLT_MAC  */
	if ((pAd->ApCfg.bMACRepeaterEn) &&
                /*(pAd->chipCap.hif_type == HIF_MT) &&*/
        (CliIdx != 0xFF)
#ifdef MWDS
		&& (pApCliEntry != NULL)
		&& ((pApCliEntry->wdev.bSupportMWDS == FALSE) ||
			(pApCliEntry->MlmeAux.bSupportMWDS == FALSE))
#endif /* MWDS */
		)
        {
                pMacEntry = MacTableInsertEntry(
                                        pAd,
                                        (PUCHAR)(pApCliEntry->MlmeAux.Bssid),
                                        wdev,
                                        ENTRY_REPEATER,
                                        OPMODE_AP,
                                        TRUE);
        }
		else
#endif /* MAC_REPEATER_SUPPORT */
			pMacEntry = MacTableInsertEntry(pAd, (PUCHAR)(pApCliEntry->MlmeAux.Bssid),
										wdev, ENTRY_APCLI,
										OPMODE_AP, TRUE);
#endif /* !FAST_EAPOL_WAR */


		if (pMacEntry)
		{
			struct _SECURITY_CONFIG *pProfile_SecConfig = &wdev->SecConfig;
			struct _SECURITY_CONFIG *pEntry_SecConfig = &pMacEntry->SecConfig;

#ifdef DOT11W_PMF_SUPPORT
			/*fill the pMacEntry's PMF parameters*/
			{
				RSN_CAPABILITIES RsnCap;
				NdisMoveMemory(&RsnCap, &pApCliEntry->MlmeAux.RsnCap, sizeof(RSN_CAPABILITIES));
				RsnCap.word = cpu2le16(RsnCap.word);

				/*mismatch case*/
				if (((pProfile_SecConfig->PmfCfg.MFPR) && (RsnCap.field.MFPC == FALSE))
					|| ((pProfile_SecConfig->PmfCfg.MFPC == FALSE) && (RsnCap.field.MFPR)))

				{
					pEntry_SecConfig->PmfCfg.UsePMFConnect = FALSE;
					pEntry_SecConfig->PmfCfg.UseSHA256Connect = FALSE;
				}
				
				if ((pProfile_SecConfig->PmfCfg.MFPC) && (RsnCap.field.MFPC))
				{
					pEntry_SecConfig->PmfCfg.UsePMFConnect = TRUE;
					if ((pApCliEntry->MlmeAux.IsSupportSHA256KeyDerivation) || (RsnCap.field.MFPR))
						pEntry_SecConfig->PmfCfg.UseSHA256Connect = TRUE;

					pEntry_SecConfig->PmfCfg.MFPC = RsnCap.field.MFPC;
					pEntry_SecConfig->PmfCfg.MFPR = RsnCap.field.MFPR;
				}
			}
#endif /* DOT11W_PMF_SUPPORT */

			if (IS_CIPHER_WEP(pEntry_SecConfig->PairwiseCipher))
			{
				os_move_mem(pEntry_SecConfig->WepKey, pProfile_SecConfig->WepKey,  sizeof(SEC_KEY_INFO)*SEC_KEY_NUM);
				pProfile_SecConfig->GroupKeyId = pProfile_SecConfig->PairwiseKeyId;
				pEntry_SecConfig->PairwiseKeyId = pProfile_SecConfig->PairwiseKeyId;
			}
			else
			{
				CHAR rsne_idx = 0;
#ifdef WH_EZ_SETUP
#ifdef EZ_MOD_SUPPORT
				if (IS_EZ_SETUP_ENABLED(wdev) && (wdev->ez_driver_params.ez_api_mode != CONNECTION_OFFLOAD)
					&& !wdev->ez_driver_params.ez_wps_reconnect ){
					NdisCopyMemory(pEntry_SecConfig->PMK, wdev->SecConfig.PMK,PMK_LEN);
				} else if (IS_EZ_SETUP_ENABLED(wdev) && (wdev->ez_driver_params.ez_api_mode != CONNECTION_OFFLOAD)
					&& wdev->ez_driver_params.ez_wps_reconnect)
				{
					/* Calculate PMK */
					SetWPAPSKKey(pAd, pApCliEntry->WscControl.WpaPsk, 
						pApCliEntry->WscControl.WpaPskLen, 
						pApCliEntry->MlmeAux.Ssid, 
						pApCliEntry->MlmeAux.SsidLen, pEntry_SecConfig->PMK);				
				
				} else
#else
				if (wdev->enable_easy_setup && (wdev->ez_security.ez_api_mode != CONNECTION_OFFLOAD)
					&& !wdev->ez_security.ez_wps_reconnect ){
					NdisCopyMemory(pEntry_SecConfig->PMK, wdev->ez_security.this_band_info.pmk,PMK_LEN);
				} else if (wdev->enable_easy_setup && (wdev->ez_security.ez_api_mode != CONNECTION_OFFLOAD)
					&& wdev->ez_security.ez_wps_reconnect)
				{
					/* Calculate PMK */
					SetWPAPSKKey(pAd, pApCliEntry->WscControl.WpaPsk, 
						pApCliEntry->WscControl.WpaPskLen, 
						pApCliEntry->MlmeAux.Ssid, 
						pApCliEntry->MlmeAux.SsidLen, pEntry_SecConfig->PMK);				
				
				} else
#endif				
#endif
				{
				/* Calculate PMK */
				SetWPAPSKKey(pAd, pProfile_SecConfig->PSK, strlen(pProfile_SecConfig->PSK), pApCliEntry->MlmeAux.Ssid, pApCliEntry->MlmeAux.SsidLen, pEntry_SecConfig->PMK);
				}

#ifdef MAC_REPEATER_SUPPORT
				if ((pAd->ApCfg.bMACRepeaterEn)
					/*&&(pAd->chipCap.hif_type == HIF_MT)*/
					&& (CliIdx != 0xFF)
#ifdef MWDS
					&& (pApCliEntry != NULL)
					&& ((pApCliEntry->wdev.bSupportMWDS == FALSE) ||
						(pApCliEntry->MlmeAux.bSupportMWDS == FALSE))
#endif /* MWDS */
				)
				{
					os_move_mem(pEntry_SecConfig->Handshake.AAddr, pMacEntry->Addr,MAC_ADDR_LEN);
					os_move_mem(pEntry_SecConfig->Handshake.SAddr, pReptEntry->CurrentAddress,MAC_ADDR_LEN);
				}
				else
#endif /* MAC_REPEATER_SUPPORT */
				{
					os_move_mem(pEntry_SecConfig->Handshake.AAddr, pMacEntry->Addr,MAC_ADDR_LEN);
					os_move_mem(pEntry_SecConfig->Handshake.SAddr, wdev->if_addr,MAC_ADDR_LEN);
				}
				os_zero_mem(pEntry_SecConfig->Handshake.ReplayCounter, LEN_KEY_DESC_REPLAY);
				
				for (rsne_idx=0; rsne_idx < SEC_RSNIE_NUM; rsne_idx++)
				{
					pEntry_SecConfig->RSNE_Type[rsne_idx] = pProfile_SecConfig->RSNE_Type[rsne_idx];
					if (pEntry_SecConfig->RSNE_Type[rsne_idx] == SEC_RSNIE_NONE)
						continue;

					os_move_mem(pEntry_SecConfig->RSNE_EID[rsne_idx], pProfile_SecConfig->RSNE_EID[rsne_idx],  sizeof(UCHAR));
					pEntry_SecConfig->RSNE_Len[rsne_idx] = pProfile_SecConfig->RSNE_Len[rsne_idx];
					os_move_mem(pEntry_SecConfig->RSNE_Content[rsne_idx], pProfile_SecConfig->RSNE_Content[rsne_idx],  sizeof(UCHAR)*MAX_LEN_OF_RSNIE);
				}

				pMacEntry->SecConfig.Handshake.WpaState = AS_INITPSK;
			}
			pEntry_SecConfig->GroupKeyId = pProfile_SecConfig->GroupKeyId;

			MTWF_LOG(DBG_CAT_CLIENT, DBG_SUBCAT_ALL, DBG_LVL_TRACE,
				("%s: (apcli%d) connect AKM(0x%x)=%s, PairwiseCipher(0x%x)=%s, GroupCipher(0x%x)=%s\n",
				__FUNCTION__, ifIndex,
				pEntry_SecConfig->AKMMap, GetAuthModeStr(pEntry_SecConfig->AKMMap),
				pEntry_SecConfig->PairwiseCipher, GetEncryModeStr(pEntry_SecConfig->PairwiseCipher),
				pEntry_SecConfig->GroupCipher, GetEncryModeStr(pEntry_SecConfig->GroupCipher)));
			MTWF_LOG(DBG_CAT_CLIENT, DBG_SUBCAT_ALL, DBG_LVL_TRACE,
				("%s(): PairwiseKeyId=%d, GroupKeyId=%d\n",
				__FUNCTION__, pEntry_SecConfig->PairwiseKeyId, pEntry_SecConfig->GroupKeyId));
		}

		if (pMacEntry)
		{
			UCHAR Rates[MAX_LEN_OF_SUPPORTED_RATES];
			PUCHAR pRates = Rates;
			UCHAR RatesLen;
			UCHAR MaxSupportedRate = 0;

			tr_entry = &pAd->MacTab.tr_entry[pMacEntry->wcid];
			pMacEntry->Sst = SST_ASSOC;


#ifdef HTC_DECRYPT_IOT
	if ((pMacEntry->HTC_ICVErrCnt)
		|| (pMacEntry->HTC_AAD_OM_Force)
		|| (pMacEntry->HTC_AAD_OM_CountDown)
		|| (pMacEntry->HTC_AAD_OM_Freeze)
		)
	{
		MTWF_LOG(DBG_CAT_RX, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("@@@ %s(): (wcid=%u), HTC_ICVErrCnt(%u), HTC_AAD_OM_Freeze(%u), HTC_AAD_OM_CountDown(%u),  HTC_AAD_OM_Freeze(%u) is in Asso. stage!\n",
		__FUNCTION__, pMacEntry->wcid, pMacEntry->HTC_ICVErrCnt, pMacEntry->HTC_AAD_OM_Force, pMacEntry->HTC_AAD_OM_CountDown, pMacEntry->HTC_AAD_OM_Freeze));

		//Force clean.	
		pMacEntry->HTC_ICVErrCnt = 0;
		pMacEntry->HTC_AAD_OM_Force = 0;
		pMacEntry->HTC_AAD_OM_CountDown = 0;
		pMacEntry->HTC_AAD_OM_Freeze = 0;
	}
#endif /* HTC_DECRYPT_IOT */

			
			//pMacEntry->wdev = &pApCliEntry->wdev;//duplicate assign.
#ifdef MAC_REPEATER_SUPPORT
			if ((CliIdx != 0xFF)
#ifdef MWDS
				&& (pApCliEntry != NULL)
				&& ((pApCliEntry->wdev.bSupportMWDS == FALSE) ||
					(pApCliEntry->MlmeAux.bSupportMWDS == FALSE))
#endif /* MWDS */
			)
			{
                pReptEntry = &pAd->ApCfg.pRepeaterCliPool[CliIdx];
				pReptEntry->MacTabWCID = pMacEntry->wcid;
				pReptEntry->CliValid = TRUE;
				pMacEntry->bReptCli = TRUE;
				pMacEntry->bReptEthBridgeCli = FALSE;
				pMacEntry->MatchReptCliIdx = CliIdx;
				pMacEntry->ReptCliIdleCount = 0;
				COPY_MAC_ADDR(pMacEntry->ReptCliAddr, pReptEntry->CurrentAddress);
                tr_entry->OmacIdx = HcGetRepeaterOmac(pAd,pMacEntry);

				if (pReptEntry->bEthCli == TRUE) {
					pMacEntry->bReptEthCli = TRUE;
					if (MAC_ADDR_EQUAL(pAd->ApCfg.BridgeAddress, pAd->ApCfg.pRepeaterCliPool[CliIdx].OriginalAddress)) {
						pMacEntry->bReptEthBridgeCli = TRUE;
					}
				} else {
					pMacEntry->bReptEthCli = FALSE;
				}
			}
			else
#endif /* MAC_REPEATER_SUPPORT */
			{
				pApCliEntry->Valid = TRUE;
				pApCliEntry->MacTabWCID = pMacEntry->wcid;
#ifdef MAC_REPEATER_SUPPORT
				pMacEntry->bReptCli = FALSE;
#endif /* MAC_REPEATER_SUPPORT */
				COPY_MAC_ADDR(&wdev->bssid[0], &pApCliEntry->MlmeAux.Bssid[0]);
                os_move_mem(wdev->bss_info_argument.Bssid,wdev->bssid,MAC_ADDR_LEN);
				COPY_MAC_ADDR(APCLI_ROOT_BSSID_GET(pAd, pApCliEntry->MacTabWCID), pApCliEntry->MlmeAux.Bssid);
				pApCliEntry->SsidLen = pApCliEntry->MlmeAux.SsidLen;
				NdisMoveMemory(pApCliEntry->Ssid, pApCliEntry->MlmeAux.Ssid, pApCliEntry->SsidLen);
				ComposePsPoll(pAd, &(pApCliEntry->PsPollFrame), pApCliEntry->MlmeAux.Aid,
									 pApCliEntry->MlmeAux.Bssid, pApCliEntry->wdev.if_addr);
				ComposeNullFrame(pAd, &(pApCliEntry->NullFrame), pApCliEntry->MlmeAux.Bssid,
										pApCliEntry->wdev.if_addr, pApCliEntry->MlmeAux.Bssid);
			}


			if (IS_AKM_WPA_CAPABILITY_Entry(pMacEntry)
#ifdef WSC_AP_SUPPORT
				&& ((pApCliEntry->WscControl.WscConfMode == WSC_DISABLE) ||
                		(pApCliEntry->WscControl.bWscTrigger == FALSE))
#endif /* WSC_AP_SUPPORT */
				)
				tr_entry->PortSecured = WPA_802_1X_PORT_NOT_SECURED;
			else
			{
					tr_entry->PortSecured = WPA_802_1X_PORT_SECURED;

#ifdef MAC_REPEATER_SUPPORT
				if ((CliIdx != 0xFF)
#ifdef MWDS
				&& (pApCliEntry != NULL)
				&& ((pApCliEntry->wdev.bSupportMWDS == FALSE) ||
					(pApCliEntry->MlmeAux.bSupportMWDS == FALSE))
#endif /* MWDS */
				)
					pReptEntry->CliConnectState = REPT_ENTRY_CONNTED;
#endif /* MAC_REPEATER_SUPPORT */
			}

#ifdef APCLI_AUTO_CONNECT_SUPPORT
			if ((pAd->ApCfg.ApCliAutoConnectRunning[apcli_ifIndex] == TRUE) &&
#ifdef MAC_REPEATER_SUPPORT
				(CliIdx == 0xFF) &&
#endif /* MAC_REPEATER_SUPPORT */
				(tr_entry->PortSecured == WPA_802_1X_PORT_SECURED))
			{
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("ApCli auto connected: ApCliLinkUp()\n"));
				pAd->ApCfg.ApCliAutoConnectRunning[apcli_ifIndex] = FALSE;
			}
#endif /* APCLI_AUTO_CONNECT_SUPPORT */

#ifdef MAC_REPEATER_SUPPORT
			if (CliIdx == 0xFF)
#endif /* MAC_REPEATER_SUPPORT */
				NdisGetSystemUpTime(&pApCliEntry->ApCliLinkUpTime);

			SupportRate(pApCliEntry->MlmeAux.SupRate, pApCliEntry->MlmeAux.SupRateLen, pApCliEntry->MlmeAux.ExtRate,
				pApCliEntry->MlmeAux.ExtRateLen, &pRates, &RatesLen, &MaxSupportedRate);

			pMacEntry->MaxSupportedRate = min(wdev->rate.MaxTxRate, MaxSupportedRate);
			pMacEntry->RateLen = RatesLen;
			MacTableSetEntryPhyCfg(pAd, pMacEntry);

			pMacEntry->CapabilityInfo = pApCliEntry->MlmeAux.CapabilityInfo;

			pApCliEntry->ApCliBeaconPeriod = pApCliEntry->MlmeAux.BeaconPeriod;


#ifdef DOT11_N_SUPPORT
			/* If this Entry supports 802.11n, upgrade to HT rate. */
			if (pApCliEntry->MlmeAux.HtCapabilityLen != 0)
			{
				PHT_CAPABILITY_IE pHtCapability = (PHT_CAPABILITY_IE)&pApCliEntry->MlmeAux.HtCapability;

				ht_mode_adjust(pAd, pMacEntry, NULL, pHtCapability, &pAd->CommonCfg.DesiredHtPhy);

				/* find max fixed rate */
				pMacEntry->MaxHTPhyMode.field.MCS = get_ht_max_mcs(pAd, &wdev->DesiredHtPhyInfo.MCSSet[0],
																	&pHtCapability->MCSSet[0]);

				if (wdev->DesiredTransmitSetting.field.MCS != MCS_AUTO)
				{
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("IF-apcli%d : Desired MCS = %d\n",
								ifIndex, wdev->DesiredTransmitSetting.field.MCS));

					set_ht_fixed_mcs(pAd, pMacEntry, wdev->DesiredTransmitSetting.field.MCS, wdev->HTPhyMode.field.MCS);
				}
				pMacEntry->HTPhyMode.word = pMacEntry->MaxHTPhyMode.word;

				/* Move to ht_mode_adjust(), which could be reused by WDS and P2P */
				/*
				pMacEntry->MaxHTPhyMode.field.STBC = (pHtCapability->HtCapInfo.RxSTBC & (pAd->CommonCfg.DesiredHtPhy.TxSTBC));
				*/
				/* Move to set_sta_ht_cap(), which could be reused by AP, WDS and P2P */
				/*
				pMacEntry->MpduDensity = pHtCapability->HtCapParm.MpduDensity;
				pMacEntry->MaxRAmpduFactor = pHtCapability->HtCapParm.MaxRAmpduFactor;
				pMacEntry->MmpsMode = (UCHAR)pHtCapability->HtCapInfo.MimoPs;
				pMacEntry->AMsduSize = (UCHAR)pHtCapability->HtCapInfo.AMsduSize;

				if (pAd->CommonCfg.DesiredHtPhy.AmsduEnable && (pAd->CommonCfg.REGBACapability.field.AutoBA == FALSE))
					CLIENT_STATUS_SET_FLAG(pMacEntry, fCLIENT_STATUS_AMSDU_INUSED);
				*/

				set_sta_ht_cap(pAd, pMacEntry, pHtCapability);

				NdisMoveMemory(&pMacEntry->HTCapability, &pApCliEntry->MlmeAux.HtCapability, sizeof(HT_CAPABILITY_IE));
				NdisMoveMemory(pMacEntry->HTCapability.MCSSet, pApCliEntry->RxMcsSet, 16);
				assoc_ht_info_debugshow(pAd, pMacEntry, sizeof(HT_CAPABILITY_IE), &pApCliEntry->MlmeAux.HtCapability);

#ifdef APCLI_CERT_SUPPORT
				if (pAd->bApCliCertTest == TRUE)
				{
					ADD_HTINFO2 *ht_info2 = &pApCliEntry->MlmeAux.AddHtInfo.AddHtInfo2;

					pApCliEntry->wdev.protection = 0;

					wdev->protection |= SET_PROTECT(ht_info2->OperaionMode);

					if (ht_info2->NonGfPresent == 1) {
						wdev->protection |= SET_PROTECT(GREEN_FIELD_PROTECT);
					}
					else {
						wdev->protection &= ~(SET_PROTECT(GREEN_FIELD_PROTECT));
					}

					AsicUpdateProtect(pAd, (USHORT)pApCliEntry->wdev.protection,
										ALLN_SETPROTECT, FALSE,
									(ht_info2->NonGfPresent == 1) ? TRUE : FALSE);

					MTWF_LOG(DBG_CAT_CLIENT, DBG_SUBCAT_ALL, DBG_LVL_WARN,
								("SYNC - Root AP changed N OperaionMode to %d\n", ht_info2->OperaionMode));
				}
#endif /* APCLI_CERT_SUPPORT */
			}
			else
			{
				pAd->MacTab.fAnyStationIsLegacy = TRUE;
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("ApCliLinkUp - MaxSupRate=%d Mbps\n",
								  RateIdToMbps[pMacEntry->MaxSupportedRate]));
			}
#endif /* DOT11_N_SUPPORT */


#ifdef DOT11_VHT_AC
			if (WMODE_CAP_AC(pApCliEntry->wdev.PhyMode) && pApCliEntry->MlmeAux.vht_cap_len &&  pApCliEntry->MlmeAux.vht_op_len)
            		{
				vht_mode_adjust(pAd, pMacEntry, &(pApCliEntry->MlmeAux.vht_cap), &(pApCliEntry->MlmeAux.vht_op));
				dot11_vht_mcs_to_internal_mcs(pAd, wdev, &pApCliEntry->MlmeAux.vht_cap, &pMacEntry->MaxHTPhyMode);

                		/* Move to set_vht_cap(), which could be reused by AP, WDS and P2P */
				/*
                		pMacEntry->MaxRAmpduFactor =
                        		(pApCliEntry->MlmeAux.vht_cap.vht_cap.max_ampdu_exp > pMacEntry->MaxRAmpduFactor) ?
                        		pApCliEntry->MlmeAux.vht_cap.vht_cap.max_ampdu_exp : pMacEntry->MaxRAmpduFactor;
                		pMacEntry->AMsduSize = pApCliEntry->MlmeAux.vht_cap.vht_cap.max_mpdu_len;
				*/
				set_vht_cap(pAd, pMacEntry, &(pApCliEntry->MlmeAux.vht_cap));
				NdisMoveMemory(&pMacEntry->vht_cap_ie, &pApCliEntry->MlmeAux.vht_cap, sizeof(VHT_CAP_IE));

                		assoc_vht_info_debugshow(pAd, pMacEntry, &pApCliEntry->MlmeAux.vht_cap, &pApCliEntry->MlmeAux.vht_op);
            		}
#endif /* DOT11_VHT_AC */

			//update per wdev bw
			if (pMacEntry->MaxHTPhyMode.field.BW < BW_80) {
				wlan_operate_set_ht_bw(&pApCliEntry->wdev,pMacEntry->MaxHTPhyMode.field.BW);
			} else {
#ifdef DOT11_VHT_AC
				switch(pMacEntry->MaxHTPhyMode.field.BW) {
					case BW_80:
						wlan_operate_set_vht_bw(&pApCliEntry->wdev,VHT_BW_80);
						wlan_operate_set_ht_bw(&pApCliEntry->wdev,BW_40);
						break;
					case BW_160:
						wlan_operate_set_vht_bw(&pApCliEntry->wdev,VHT_BW_160);
						wlan_operate_set_ht_bw(&pApCliEntry->wdev,BW_40);
						break;
					default:
						wlan_operate_set_vht_bw(&pApCliEntry->wdev,VHT_BW_2040);
						wlan_operate_set_ht_bw(&pApCliEntry->wdev,BW_40);
						break;
				}
#endif
			}
			pMacEntry->HTPhyMode.word = pMacEntry->MaxHTPhyMode.word;
			pMacEntry->CurrTxRate = pMacEntry->MaxSupportedRate;

			RTMPSetSupportMCS(pAd,
							OPMODE_AP,
							pMacEntry,
							pApCliEntry->MlmeAux.SupRate,
							pApCliEntry->MlmeAux.SupRateLen,
							pApCliEntry->MlmeAux.ExtRate,
							pApCliEntry->MlmeAux.ExtRateLen,
#ifdef DOT11_VHT_AC
							pApCliEntry->MlmeAux.vht_cap_len,
							&pApCliEntry->MlmeAux.vht_cap,
#endif /* DOT11_VHT_AC */
							&pApCliEntry->MlmeAux.HtCapability,
							pApCliEntry->MlmeAux.HtCapabilityLen);

#if defined(RTMP_MAC) || defined(RLT_MAC)
			if ((pAd->chipCap.hif_type == HIF_RTMP) || (pAd->chipCap.hif_type == HIF_RLT))
			{
				if (wdev->bAutoTxRateSwitch == FALSE)
				{
					pMacEntry->bAutoTxRateSwitch = FALSE;
					/* If the legacy mode is set, overwrite the transmit setting of this entry. */
					RTMPUpdateLegacyTxSetting((UCHAR)wdev->DesiredTransmitSetting.field.FixedTxMode, pMacEntry);
				}
				else
				{
					UCHAR TableSize = 0;

					pMacEntry->bAutoTxRateSwitch = TRUE;

					MlmeSelectTxRateTable(pAd, pMacEntry, &pMacEntry->pTable, &TableSize, &pMacEntry->CurrTxRateIndex);
					MlmeNewTxRate(pAd, pMacEntry);
#ifdef NEW_RATE_ADAPT_SUPPORT
					if (! ADAPT_RATE_TABLE(pMacEntry->pTable))
#endif /* NEW_RATE_ADAPT_SUPPORT */
						pMacEntry->HTPhyMode.field.ShortGI = GI_800;
				}
			}
#endif /* defined(RTMP_MAC) || defined(RLT_MAC) */

#ifdef MT_MAC
#ifdef MT7615
			//if (pAd->chipCap.hif_type == HIF_MT) 
			{
				if (pApCliEntry->MlmeAux.APEdcaParm.bValid) {
					pMacEntry->bACMBit[WMM_AC_BK] = pApCliEntry->MlmeAux.APEdcaParm.bACM[WMM_AC_BK];
					pMacEntry->bACMBit[WMM_AC_BE] = pApCliEntry->MlmeAux.APEdcaParm.bACM[WMM_AC_BE];
					pMacEntry->bACMBit[WMM_AC_VI] = pApCliEntry->MlmeAux.APEdcaParm.bACM[WMM_AC_VI];
					pMacEntry->bACMBit[WMM_AC_VO] = pApCliEntry->MlmeAux.APEdcaParm.bACM[WMM_AC_VO];
				}
			}
#endif /* MT7615 */
#endif /* MT_MAC */
			/*
				set this entry WMM capable or not
				It need to before WifiSysApCliLinkUp, or it cannot link non_wmm AP
			*/
			if ((pApCliEntry->MlmeAux.APEdcaParm.bValid)
#ifdef DOT11_N_SUPPORT
				|| IS_HT_STA(pMacEntry)
#endif /* DOT11_N_SUPPORT */
			)
			{
				CLIENT_STATUS_SET_FLAG(pMacEntry, fCLIENT_STATUS_WMM_CAPABLE);
			}
			else
			{
				CLIENT_STATUS_CLEAR_FLAG(pMacEntry, fCLIENT_STATUS_WMM_CAPABLE);
			}

            NdisGetSystemUpTime(&pApCliEntry->ApCliRcvBeaconTime);
            /* set the apcli interface be valid. */
            WifiSysApCliLinkUp(pAd,pApCliEntry,CliIdx, pMacEntry);

            MacTableSetEntryRaCap(pAd, pMacEntry, &pApCliEntry->MlmeAux.vendor_ie);

#ifdef MT_MAC
            //if (pAd->chipCap.hif_type == HIF_MT) 
			{

				if(pMacEntry && CliIdx == 0xff)
				{
			        RTMP_STA_ENTRY_ADD(pAd,
                        wdev->bss_info_argument.ucBcMcWlanIdx,
                        (PUCHAR)pApCliEntry->MlmeAux.Bssid,
                        TRUE,
                        TRUE);
				}
                RTMP_STA_ENTRY_ADD(pAd, pMacEntry->wcid, (PUCHAR)pApCliEntry->MlmeAux.Bssid,FALSE, TRUE);

                if (wdev->bAutoTxRateSwitch == TRUE)
                {
                    pMacEntry->bAutoTxRateSwitch = TRUE;
                }
                else
                {
                    pMacEntry->HTPhyMode.field.MCS = wdev->HTPhyMode.field.MCS;
                    pMacEntry->bAutoTxRateSwitch = FALSE;

                    /* If the legacy mode is set, overwrite the transmit setting of this entry. */
                    RTMPUpdateLegacyTxSetting((UCHAR)wdev->DesiredTransmitSetting.field.FixedTxMode, pMacEntry);
                }
                RAInit(pAd, pMacEntry);

#ifdef TXBF_SUPPORT
                if (HcIsBfCapSupport(wdev))
                {           
                    HW_APCLI_BF_CAP_CONFIG(pAd, pMacEntry);
				}
#endif /* TXBF_SUPPORT */                
            }
#endif


			if (IS_CIPHER_WEP_Entry(pMacEntry)
			)
			{
				INT BssIdx;
				ASIC_SEC_INFO Info = {0};
				struct _SECURITY_CONFIG *pSecConfig = &pMacEntry->SecConfig;

				BssIdx = pAd->ApCfg.BssidNum + MAX_MESH_NUM + ifIndex;
#ifdef MAC_APCLI_SUPPORT
				BssIdx = APCLI_BSS_BASE + ifIndex;
#endif /* MAC_APCLI_SUPPORT */

				/* Set Group key material to Asic */
				os_zero_mem(&Info, sizeof(ASIC_SEC_INFO));
				Info.Operation = SEC_ASIC_ADD_GROUP_KEY;
				Info.Direction = SEC_ASIC_KEY_RX;
				Info.Wcid = wdev->bss_info_argument.ucBcMcWlanIdx;
				Info.BssIndex = BssIdx;
				Info.Cipher = pSecConfig->GroupCipher;
				Info.KeyIdx = pSecConfig->GroupKeyId;
				os_move_mem(&Info.PeerAddr[0], pMacEntry->Addr, MAC_ADDR_LEN);
				os_move_mem(&Info.Key,&pSecConfig->WepKey[Info.KeyIdx],sizeof(SEC_KEY_INFO));

				HW_ADDREMOVE_KEYTABLE(pAd, &Info);

				/* Update WCID attribute table and IVEIV table */
				RTMPSetWcidSecurityInfo(pAd,
										BssIdx,
										Info.KeyIdx,
										pSecConfig->GroupCipher,
										pMacEntry->wcid,
										SHAREDKEYTABLE);


				/* Set Pairwise key material to Asic */
				os_zero_mem(&Info, sizeof(ASIC_SEC_INFO));
				Info.Operation = SEC_ASIC_ADD_PAIRWISE_KEY;
				Info.Direction = SEC_ASIC_KEY_BOTH;
				Info.Wcid = pMacEntry->wcid;
				Info.BssIndex = BssIdx;
				Info.Cipher = pSecConfig->PairwiseCipher;
				Info.KeyIdx = pSecConfig->PairwiseKeyId;
				os_move_mem(&Info.PeerAddr[0], pMacEntry->Addr, MAC_ADDR_LEN);
				os_move_mem(&Info.Key,&pSecConfig->WepKey[Info.KeyIdx],sizeof(SEC_KEY_INFO));

				HW_ADDREMOVE_KEYTABLE(pAd, &Info);
			}

#ifdef PIGGYBACK_SUPPORT
			if (CLIENT_STATUS_TEST_FLAG(pMacEntry, fCLIENT_STATUS_PIGGYBACK_CAPABLE))
			{
				AsicSetPiggyBack(pAd, TRUE);
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("Turn on Piggy-Back\n"));
			}
#endif /* PIGGYBACK_SUPPORT */

#ifdef WH_EZ_SETUP
			if (CliIdx == 0xff) {
				if (IS_EZ_SETUP_ENABLED(wdev)) {
					if (pApCliEntry->MlmeAux.support_easy_setup) {
						result = ez_port_secured(pAd, pMacEntry, ifIndex, FALSE);
						if(result == FALSE){
							EZ_DEBUG(DBG_CAT_ALL, DBG_SUBCAT_ALL, DBG_LVL_OFF,("ApCliLinkUp: connection failed/broken\n"));
							break;
						}
					}
#ifndef EZ_MOD_SUPPORT
					else {
						struct wifi_dev *ap_wdev;
						ap_wdev = &pAd->ApCfg.MBSSID[ifIndex].wdev;
						EZ_SET_CAP_CONNECTED(ap_wdev->ez_security.capability);
						UpdateBeaconHandler(pAd, ap_wdev, IE_CHANGE);
					}
#endif					
				}
			}
#endif /* WH_EZ_SETUP */

#ifdef MWDS
            if((CliIdx == 0xff) && (tr_entry->PortSecured == WPA_802_1X_PORT_SECURED))
            {	
		MWDSAPCliPeerEnable(pAd, pApCliEntry, pMacEntry);
#ifdef WH_EZ_SETUP
		//! If security is disabled on this link, 
		//!ez_hanle_pairmsg4 needs to be called to trigger config push
		if (IS_EZ_SETUP_ENABLED(wdev) && !IS_AKM_PSK_Entry(pMacEntry) 
#ifdef WSC_AP_SUPPORT
			&& (!((pApCliEntry->WscControl.WscConfMode != WSC_DISABLE) && (pApCliEntry->WscControl.bWscTrigger == TRUE)))
#endif /* WSC_AP_SUPPORT */
		) {
			ez_handle_pairmsg4(pAd, pMacEntry);
		}
#endif				
            }
#endif /* MWDS */

#ifdef MT_MAC
#ifdef MAC_REPEATER_SUPPORT
			if ((CliIdx != 0xff) /*&& (pAd->chipCap.hif_type == HIF_MT)*/
#ifdef MWDS
				&& (pApCliEntry != NULL)
				&& ((pApCliEntry->wdev.bSupportMWDS == FALSE) ||
					(pApCliEntry->MlmeAux.bSupportMWDS == FALSE))
#endif /* MWDS */
                ) {
				AsicInsertRepeaterRootEntry(
                                    pAd,
                                    pMacEntry->wcid,
                                    (PUCHAR)(pApCliEntry->MlmeAux.Bssid),
                                    CliIdx);
#ifdef TXBF_SUPPORT
                if ((pAd->fgClonedStaWithBfeeSelected) && (pAd->ReptClonedStaEntry_CliIdx == CliIdx))
			    {
					HW_APCLI_BF_REPEATER_CONFIG(pAd, pMacEntry); // Move cloned STA's MAC addr from MUAR table to ownmac
				}
#endif /* TXBF_SUPPORT */
			}
#endif /* MAC_REPEATER_SUPPORT */
#endif /* MT_MAC */

			result = TRUE;
			pAd->ApCfg.ApCliInfRunned++;

			break;
		}
		result = FALSE;
	} while(FALSE);


	if (result == FALSE)
	{
	 	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, (" (%s) alloc mac entry fail!!!\n", __FUNCTION__));
		return result;
	}


#ifdef WSC_AP_SUPPORT
    /* WSC initial connect to AP, jump to Wsc start action and set the correct parameters */
	if ((result == TRUE) &&
		(pAd->ApCfg.ApCliTab[ifIndex].WscControl.WscConfMode == WSC_ENROLLEE) &&
		(pAd->ApCfg.ApCliTab[ifIndex].WscControl.bWscTrigger == TRUE))
	{
		pAd->ApCfg.ApCliTab[ifIndex].WscControl.WscState = WSC_STATE_LINK_UP;
		pAd->ApCfg.ApCliTab[ifIndex].WscControl.WscStatus = WSC_STATE_LINK_UP;
		pAd->ApCfg.ApCliTab[ifIndex].WscControl.WscConfStatus = WSC_SCSTATE_UNCONFIGURED;
		NdisZeroMemory(pApCliEntry->WscControl.EntryAddr, MAC_ADDR_LEN);
		NdisMoveMemory(pApCliEntry->WscControl.EntryAddr, pApCliEntry->MlmeAux.Bssid, MAC_ADDR_LEN);
		WscSendEapolStart(pAd, pMacEntry->Addr, AP_MODE);
	}
    else
    {
        WscStop(pAd, TRUE, &pAd->ApCfg.ApCliTab[ifIndex].WscControl);
    }
#endif /* WSC_AP_SUPPORT */

#ifdef DOT11_N_SUPPORT
#ifdef DOT11N_DRAFT3
#ifdef APCLI_CERT_SUPPORT
	if (pAd->bApCliCertTest == TRUE)
	{
		if ((pAd->CommonCfg.bBssCoexEnable == TRUE) &&
			(wdev->channel <= 14) &&
			(pApCliEntry->wdev.DesiredHtPhyInfo.bHtEnable == TRUE) &&
			(pApCliEntry->MlmeAux.ExtCapInfo.BssCoexistMgmtSupport == 1)) {
			OPSTATUS_SET_FLAG(pAd, fOP_STATUS_SCAN_2040);
			BuildEffectedChannelList(pAd, wdev);

			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("LinkUP AP supports 20/40 BSS COEX !!! Dot11BssWidthTriggerScanInt[%d]\n",
				pAd->CommonCfg.Dot11BssWidthTriggerScanInt));
		} 
		else {
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("not supports 20/40 BSS COEX !!! \n"));
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("pAd->CommonCfg.bBssCoexEnable %d !!! \n",
				pAd->CommonCfg.bBssCoexEnable));
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("pAd->CommonCfg.Channel %d !!! \n",
				pAd->CommonCfg.Channel));
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("pApCliEntry->DesiredHtPhyInfo.bHtEnable %d !!! \n",
				pApCliEntry->wdev.DesiredHtPhyInfo.bHtEnable));
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("pAd->MlmeAux.ExtCapInfo.BssCoexstSup %d !!! \n",
				pApCliEntry->MlmeAux.ExtCapInfo.BssCoexistMgmtSupport));
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("pAd->CommonCfg.CentralChannel %d !!! \n",
				pAd->CommonCfg.CentralChannel));
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("pAd->CommonCfg.PhyMode %d !!! \n",
				pAd->CommonCfg.PhyMode));
		}
	}
#endif /* APCLI_CERT_SUPPORT */	
#endif /* DOT11N_DRAFT3 */
#endif /* DOT11_N_SUPPORT */

#ifdef WH_EVENT_NOTIFIER
    if ((result == TRUE) && pMacEntry &&
        tr_entry && (tr_entry->PortSecured == WPA_802_1X_PORT_SECURED))
    {
        EventHdlr pEventHdlrHook = NULL;
        pEventHdlrHook = GetEventNotiferHook(WHC_DRVEVNT_EXT_UPLINK_STAT);
        if(pEventHdlrHook && pMacEntry->wdev)
            pEventHdlrHook(pAd, pMacEntry, (UINT32)WHC_UPLINK_STAT_CONNECTED);
    }
#endif /* WH_EVENT_NOTIFIER */

	return result;
}

static CHAR LinkDownReason[APCLI_LINKDOWN_PEER_DEASSOC_RSP+1][30] =
{
"none",
"send deauth req",
"send deassociate req",
"recv deassociate req",
"got disconnect req",
"recv deassociate resp"};

#if (defined(WH_EZ_SETUP) && defined(NEW_CONNECTION_ALGO))
static CHAR SubLinkDownReason[APCLI_DISCONNECT_SUB_REASON_APCLI_EZ_CONNECTION_LOOP+1][30] =
{
"none",
"rept connect too long",
"idle too long",
"remove sta",
"apcli if down",
"beacon miss",
"recv sta disassociate req",
"recv sta deauth req",
"manually del mac entry",
"repter bind to other apcli",
"apcli connect too long",
"Ez Connection Loop Detected"};


#else
static CHAR SubLinkDownReason[APCLI_DISCONNECT_SUB_REASON_APCLI_TRIGGER_TOO_LONG+1][30] =
{
"none",
"rept connect too long",
"idle too long",
"remove sta",
"apcli if down",
"beacon miss",
"recv sta disassociate req",
"recv sta deauth req",
"manually del mac entry",
"repter bind to other apcli",
"apcli connect too long"};
#endif
/*
    ==========================================================================

	Routine	Description:
		Disconnect current BSSID

	Arguments:
		pAd				- Pointer to our adapter
		ApCliIdx		- Which ApCli interface
	Return Value:
		None

	Note:

	==========================================================================
*/
VOID ApCliLinkDown(RTMP_ADAPTER *pAd, UCHAR ifIndex)
{
	APCLI_STRUCT *pApCliEntry = NULL;
	struct wifi_dev *wdev = NULL;

	UCHAR CliIdx = 0xFF;
#ifdef MAC_REPEATER_SUPPORT
    REPEATER_CLIENT_ENTRY *pReptEntry = NULL;
#endif /* MAC_REPEATER_SUPPORT */
	UCHAR MacTabWCID = 0;

	if ((ifIndex < MAX_APCLI_NUM)
#ifdef MAC_REPEATER_SUPPORT
		|| (ifIndex >= REPT_MLME_START_IDX)
#endif /* MAC_REPEATER_SUPPORT */
		)
	{
#ifdef MAC_REPEATER_SUPPORT
		if (ifIndex >= REPT_MLME_START_IDX)
		{
			CliIdx = ifIndex - REPT_MLME_START_IDX;
            pReptEntry = &pAd->ApCfg.pRepeaterCliPool[CliIdx];
			ifIndex = pReptEntry->wdev->func_idx;
			if (pReptEntry->LinkDownReason == APCLI_LINKDOWN_PEER_DEASSOC_REQ) {
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
						 ("!!! REPEATER CLI LINK DOWN - IF(apcli%d) Cli %d (Reason=%s,Reason code=%lu)!!!\n",
						  ifIndex, CliIdx, LinkDownReason[pReptEntry->LinkDownReason],pReptEntry->Disconnect_Sub_Reason));
			} else {
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
                    ("!!! REPEATER CLI LINK DOWN - IF(apcli%d) Cli %d (Reason=%s,sub=%s)!!!\n",
                            ifIndex, CliIdx, LinkDownReason[pReptEntry->LinkDownReason],SubLinkDownReason[pReptEntry->Disconnect_Sub_Reason]));
			}
		}
		else
#endif /* MAC_REPEATER_SUPPORT */
		{
			if (pAd->ApCfg.ApCliTab[ifIndex].LinkDownReason == APCLI_LINKDOWN_PEER_DEASSOC_REQ) {
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("!!! APCLI LINK DOWN - IF(apcli%d) (Reason=%s,Reason code=%lu)!!!\n", ifIndex, LinkDownReason[pAd->ApCfg.ApCliTab[ifIndex].LinkDownReason], pAd->ApCfg.ApCliTab[ifIndex].Disconnect_Sub_Reason));
			} else {
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("!!! APCLI LINK DOWN - IF(apcli%d) (Reason=%s,sub=%s)!!!\n", ifIndex, LinkDownReason[pAd->ApCfg.ApCliTab[ifIndex].LinkDownReason], SubLinkDownReason[pAd->ApCfg.ApCliTab[ifIndex].Disconnect_Sub_Reason]));
			}
		}
	}
	else
	{
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("!!! ERROR : APCLI LINK DOWN - IF(apcli%d)!!!\n", ifIndex));
		return;
	}

	pApCliEntry = &pAd->ApCfg.ApCliTab[ifIndex];
    wdev = &pApCliEntry->wdev;

	if ((pApCliEntry->Valid == FALSE)
#ifdef MAC_REPEATER_SUPPORT
		&& (CliIdx == 0xFF)
#endif /* MAC_REPEATER_SUPPORT */
		)
		return;

#if defined(CONFIG_WIFI_PKT_FWD) || defined(CONFIG_WIFI_PKT_FWD_MODULE)
#ifdef MAC_REPEATER_SUPPORT
	if (CliIdx == 0xFF)
#endif /* MAC_REPEATER_SUPPORT */
	{
		if (wf_fwd_entry_delete_hook)
			wf_fwd_entry_delete_hook (pApCliEntry->wdev.if_dev, pAd->net_dev, 1);

		if (wf_fwd_check_device_hook)
			wf_fwd_check_device_hook (pApCliEntry->wdev.if_dev, INT_APCLI, CliIdx, pApCliEntry->wdev.channel, 0);
	}
#ifdef MAC_REPEATER_SUPPORT
	else
	{
		if (packet_source_delete_entry_hook)
			packet_source_delete_entry_hook(100);
	}
#endif /* MAC_REPEATER_SUPPORT */
#endif /* CONFIG_WIFI_PKT_FWD */

#ifdef RADIO_LINK_SELECTION
#ifdef MAC_REPEATER_SUPPORT
	if (CliIdx == 0xFF)
#endif /* MAC_REPEATER_SUPPORT */
	{
		if (pAd->ApCfg.RadioLinkSelection)
			Rls_InfCliLinkDown(pAd, wdev);
	}
#endif /* RADIO_LINK_SELECTION */

#ifdef MAC_REPEATER_SUPPORT
	if (CliIdx == 0xFF)
#endif /* MAC_REPEATER_SUPPORT */
	pAd->ApCfg.ApCliInfRunned--;

#ifdef WH_EVENT_NOTIFIER
    {
        MAC_TABLE_ENTRY *pMacEntry = NULL;
        EventHdlr pEventHdlrHook = NULL;
#ifdef MAC_REPEATER_SUPPORT
        if (CliIdx != 0xFF)
        {
            if(VALID_UCAST_ENTRY_WCID(pAd, pReptEntry->MacTabWCID))
                pMacEntry = &pAd->MacTab.Content[pReptEntry->MacTabWCID];
        }
        else
#endif /* MAC_REPEATER_SUPPORT */
        {
            if(VALID_UCAST_ENTRY_WCID(pAd, pApCliEntry->MacTabWCID))
                pMacEntry = &pAd->MacTab.Content[pApCliEntry->MacTabWCID];
        }

        pEventHdlrHook = GetEventNotiferHook(WHC_DRVEVNT_EXT_UPLINK_STAT);
        if(pEventHdlrHook && pMacEntry->wdev)
            pEventHdlrHook(pAd, pMacEntry, (UINT32)WHC_UPLINK_STAT_DISCONNECT);
    }
#endif /* WH_EVENT_NOTIFIER */

#ifdef MAC_REPEATER_SUPPORT
	if (CliIdx != 0xFF) {
		MacTabWCID = pReptEntry->MacTabWCID;
		MacTableDeleteEntry(pAd, MacTabWCID, pAd->MacTab.Content[MacTabWCID].Addr);
#ifdef FAST_EAPOL_WAR
		pReptEntry->pre_entry_alloc = FALSE;
#endif /* FAST_EAPOL_WAR */
	} else
#endif /* MAC_REPEATER_SUPPORT */
        {
            MacTabWCID = pApCliEntry->MacTabWCID;
#ifdef WH_EZ_SETUP
			if(IS_EZ_SETUP_ENABLED(&pApCliEntry->wdev))
				ez_apcli_link_down(pAd, pApCliEntry, ifIndex);
#endif /* WH_EZ_SETUP */
            MacTableDeleteEntry(pAd, MacTabWCID, APCLI_ROOT_BSSID_GET(pAd, MacTabWCID));
#ifdef FAST_EAPOL_WAR
			pApCliEntry->pre_entry_alloc = FALSE;
#endif /* FAST_EAPOL_WAR */
        }

#ifdef MAC_REPEATER_SUPPORT
    if (CliIdx != 0xFF)
    {
        HW_REMOVE_REPT_ENTRY(pAd, ifIndex, CliIdx);
    }
    else
#endif /* MAC_REPEATER_SUPPORT */
	{
		pApCliEntry->Valid = FALSE;	/* This link doesn't associated with any remote-AP */
		MSDU_FORBID_SET(&pApCliEntry->wdev, MSDU_FORBID_CONNECTION_NOT_READY);
		pApCliEntry->wdev.PortSecured = WPA_802_1X_PORT_NOT_SECURED;
        HW_SET_DEL_ASIC_WCID(pAd, wdev->bss_info_argument.ucBcMcWlanIdx);
#ifdef DOT11W_PMF_SUPPORT
		BssTableDeleteEntry(&pAd->ScanTab, pApCliEntry->MlmeAux.Bssid, wdev->channel);
#else
#ifdef WH_EZ_SETUP
		if(IS_ADPTR_EZ_SETUP_ENABLED(pAd))
			BssTableDeleteEntry(&pAd->ScanTab, pApCliEntry->MlmeAux.Bssid, wdev->channel);
#endif
#endif
	}
#ifdef MWDS
	pApCliEntry->bEnableMWDS = FALSE;
#endif /* MWDS */

	pAd->ApCfg.ApCliTab[ifIndex].bPeerExist = FALSE;

    /*TODO & FIXME: Carter, REPEATER CASE */
    WifiSysApCliLinkDown(pAd,pApCliEntry,CliIdx);

#ifdef TXBF_SUPPORT
	if ((pAd->fgClonedStaWithBfeeSelected) && (pAd->ReptClonedStaEntry_CliIdx == CliIdx))
	{
	    pAd->fgClonedStaWithBfeeSelected = FALSE;
		// Remove cloned STA's MAC addr from ownmac
	}
#endif /* TXBF_SUPPORT */


#if defined(RT_CFG80211_P2P_CONCURRENT_DEVICE) || defined(CFG80211_MULTI_STA)
	RT_CFG80211_LOST_GO_INFORM(pAd);

	// TODO: need to consider driver without no FW offload @20140728
	/* NoA Stop */
	//if (bP2pCliPmEnable)
	CmdP2pNoaOffloadCtrl(pAd, P2P_NOA_DISABLED);
#endif /* RT_CFG80211_P2P_CONCURRENT_DEVICE || CFG80211_MULTI_STA */

	/*for APCLI linkdown*/
	if(CliIdx==0xFF)
	{
#ifdef DOT11_N_SUPPORT
		wlan_operate_set_ht_bw(&pApCliEntry->wdev,wlan_config_get_ht_bw(&pApCliEntry->wdev));
		wlan_operate_set_ext_cha(&pApCliEntry->wdev,wlan_config_get_ext_cha(&pApCliEntry->wdev));
		N_ChannelCheck(pAd,pApCliEntry->wdev.PhyMode,pApCliEntry->wdev.channel);
#endif
#ifdef DOT11_VHT_AC
		wlan_operate_set_vht_bw(&pApCliEntry->wdev,wlan_config_get_vht_bw(&pApCliEntry->wdev));
#endif
		ApCliLinkDownComplete(pApCliEntry);
	}
}


/*
    ==========================================================================
    Description:
        APCLI Interface Up.
    ==========================================================================
 */
VOID ApCliIfUp(RTMP_ADAPTER *pAd)
{
	UCHAR ifIndex;
	APCLI_STRUCT *pApCliEntry;
#ifdef APCLI_CONNECTION_TRIAL
	PULONG pCurrState = NULL;
#endif /* APCLI_CONNECTION_TRIAL */

	/* Reset is in progress, stop immediately */
	if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_HALT_IN_PROGRESS|fRTMP_ADAPTER_RADIO_OFF) ||
		(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_START_UP)))
		return;

	for(ifIndex = 0; ifIndex < MAX_APCLI_NUM; ifIndex++)
	{
		pApCliEntry = &pAd->ApCfg.ApCliTab[ifIndex];
        /* sanity check whether the interface is initialized. */
	    if (pApCliEntry->ApCliInit != TRUE)
		    continue;
#ifdef APCLI_CONNECTION_TRIAL
		pCurrState = &pAd->ApCfg.ApCliTab[ifIndex].CtrlCurrState;
#endif /* APCLI_CONNECTION_TRIAL */

		if(!HcIsRadioAcq(&pApCliEntry->wdev))
			continue;
		if (APCLI_IF_UP_CHECK(pAd, ifIndex)
			&& (pApCliEntry->Enable == TRUE)
			&& (pApCliEntry->Valid == FALSE)
#ifdef APCLI_CONNECTION_TRIAL
			&& (ifIndex != (pAd->ApCfg.ApCliNum-1)) // last IF is for apcli connection trial
#endif /* APCLI_CONNECTION_TRIAL */
			)
		{
			if (IS_DOT11_H_RADAR_STATE(pAd, RD_SILENCE_MODE, pApCliEntry->wdev.channel))
			{	
				if (pApCliEntry->bPeerExist == TRUE)
				{
					/* Got peer's beacon; change to normal mode */
					pAd->Dot11_H.RDCount = pAd->Dot11_H.ChMovingTime;
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, 
								("ApCliIfUp - PeerExist\n"));
				}
				else
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,
								("ApCliIfUp - Stop probing while Radar state is silent\n"));
				
				continue;
			}
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("(%s) ApCli interface[%d] startup.\n", __FUNCTION__, ifIndex));
			MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_JOIN_REQ, 0, NULL, ifIndex);
			/* Reset bPeerExist each time in case we could keep old status */
			pApCliEntry->bPeerExist = FALSE;
		}
#ifdef APCLI_CONNECTION_TRIAL
		else if (
			APCLI_IF_UP_CHECK(pAd, ifIndex)
			&& (*pCurrState == APCLI_CTRL_DISCONNECTED)//Apcli1 is not connected state.
			&& (pApCliEntry->TrialCh != 0)
			//&& NdisCmpMemory(pApCliEntry->ApCliMlmeAux.Ssid, pApCliEntry->CfgSsid, pApCliEntry->SsidLen) != 0
			&& (pApCliEntry->CfgSsidLen != 0)
			&& (pApCliEntry->Enable != 0)
			//new ap ssid shall different from the origin one.
		)
		{
			MTWF_LOG(DBG_CAT_ALL, DBG_SUBCAT_ALL, DBG_LVL_TRACE, ("(%s) Enqueue APCLI_CTRL_TRIAL_CONNECT\n", __func__));
			MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_TRIAL_CONNECT, 0, NULL, ifIndex);
			/* Reset bPeerExist each time in case we could keep old status */
			pApCliEntry->bPeerExist = FALSE;
		}
#endif /* APCLI_CONNECTION_TRIAL */
	}

	return;
}

/*
    ==========================================================================
    Description:
        APCLI Interface Down.
    ==========================================================================
 */
VOID ApCliIfDown(RTMP_ADAPTER *pAd)
{
	UCHAR ifIndex;
	PAPCLI_STRUCT pApCliEntry;
#ifdef MAC_REPEATER_SUPPORT
	UCHAR CliIdx,idx;
	INVAILD_TRIGGER_MAC_ENTRY *pEntry = NULL;
    	REPEATER_CLIENT_ENTRY *pReptEntry = NULL;
    	RTMP_CHIP_CAP *cap = &pAd->chipCap;
#endif /* MAC_REPEATER_SUPPORT */

	for(ifIndex = 0; ifIndex < MAX_APCLI_NUM; ifIndex++)
	{
		pApCliEntry = &pAd->ApCfg.ApCliTab[ifIndex];
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("%s():ApCli interface[%d] start down.\n", __FUNCTION__, ifIndex));

		if (pApCliEntry->Enable == TRUE)
			continue;

#if defined(CONFIG_WIFI_PKT_FWD) || defined(CONFIG_WIFI_PKT_FWD_MODULE)
		if (wf_fwd_entry_delete_hook)
            wf_fwd_entry_delete_hook (pApCliEntry->wdev.if_dev, pAd->net_dev, 1);
#endif /* CONFIG_WIFI_PKT_FWD */

#ifdef MAC_REPEATER_SUPPORT
		if (pAd->ApCfg.bMACRepeaterEn)
		{
			for(CliIdx = 0; CliIdx < GET_MAX_REPEATER_ENTRY_NUM(cap); CliIdx++)
			{
                		pReptEntry = &pAd->ApCfg.pRepeaterCliPool[CliIdx];
                		/*disconnect the ReptEntry which is bind on the CliLink*/
				if ((pReptEntry->CliEnable) && (pReptEntry->wdev == &pApCliEntry->wdev))
				{
					RTMP_OS_INIT_COMPLETION(&pReptEntry->free_ack);
					pReptEntry->Disconnect_Sub_Reason = APCLI_DISCONNECT_SUB_REASON_APCLI_IF_DOWN;
					MlmeEnqueue(pAd,
                                	APCLI_CTRL_STATE_MACHINE,
                                	APCLI_CTRL_DISCONNECT_REQ,
                                	0,
                                	NULL,
                                	(REPT_MLME_START_IDX + CliIdx));
					RTMP_MLME_HANDLER(pAd);
					ReptWaitLinkDown(pReptEntry);
				}
			}
		}
#endif /* MAC_REPEATER_SUPPORT */
		RTMP_OS_INIT_COMPLETION(&pApCliEntry->linkdown_complete);
		pApCliEntry->Disconnect_Sub_Reason = APCLI_DISCONNECT_SUB_REASON_APCLI_IF_DOWN;
		MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_DISCONNECT_REQ, 0, NULL, ifIndex);
        	RTMP_MLME_HANDLER(pAd);
		ApCliWaitLinkDown(pApCliEntry);
	}

#ifdef MAC_REPEATER_SUPPORT
	for (idx = 0; idx< MAX_IGNORE_AS_REPEATER_ENTRY_NUM; idx++)
	{
		pEntry = &pAd->ApCfg.ReptControl.IgnoreAsRepeaterEntry[idx];
		if (pAd->ApCfg.ApCliInfRunned == 0) {
		RepeaterRemoveIngoreEntry(pAd, idx, pEntry->MacAddr);
	}
	}
#endif /* MAC_REPEATER_SUPPORT */
	return;
}


/*
    ==========================================================================
    Description:
        APCLI Interface Monitor.
    ==========================================================================
 */
VOID ApCliIfMonitor(RTMP_ADAPTER *pAd)
{
	UCHAR index;
	APCLI_STRUCT *pApCliEntry;

	/* Reset is in progress, stop immediately */
	if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_HALT_IN_PROGRESS) ||
		!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_START_UP))
		return;

	if (ScanRunning(pAd) == TRUE)
		return;

	for(index = 0; index < MAX_APCLI_NUM; index++)
	{
		UCHAR Wcid;
		PMAC_TABLE_ENTRY pMacEntry;
		STA_TR_ENTRY *tr_entry;
		BOOLEAN bForceBrocken = FALSE;
		BOOLEAN bWpa_4way_too_log = FALSE;
		BOOLEAN bBeacon_miss = FALSE;

#ifdef APCLI_CONNECTION_TRIAL
		if (index == (pAd->ApCfg.ApCliNum-1))// last IF is for apcli connection trial
			continue;//skip apcli1 monitor. FIXME:Carter shall find a better way.
#endif /* APCLI_CONNECTION_TRIAL */		

		pApCliEntry = &pAd->ApCfg.ApCliTab[index];

        /* sanity check whether the interface is initialized. */
	    if (pApCliEntry->ApCliInit != TRUE)
		    continue;
#ifdef MAC_REPEATER_SUPPORT
        RepeaterLinkMonitor(pAd);
#endif /* MAC_REPEATER_SUPPORT */

		if (pApCliEntry->Valid == TRUE)
		{
			BOOLEAN ApclibQosNull = FALSE;

			Wcid = pAd->ApCfg.ApCliTab[index].MacTabWCID;
			if (!VALID_UCAST_ENTRY_WCID(pAd, Wcid))
				continue;
			pMacEntry = &pAd->MacTab.Content[Wcid];
 			tr_entry = &pAd->MacTab.tr_entry[Wcid];

			if ((IS_AKM_WPA_CAPABILITY(pMacEntry->SecConfig.AKMMap))
				&& (tr_entry->PortSecured != WPA_802_1X_PORT_SECURED)
				&& (RTMP_TIME_AFTER(pAd->Mlme.Now32 , (pApCliEntry->ApCliLinkUpTime + (60 * OS_HZ))))) {
				bWpa_4way_too_log = TRUE;
				bForceBrocken = TRUE;
			}
			// Generic Change done by Kun-ze in WHC codebase for improving Beacon loss detection logic, so keeping in easy enabled case
#ifdef CONFIG_MULTI_CHANNEL
			//increase to 12
			if (RTMP_TIME_AFTER(pAd->Mlme.Now32 , (pApCliEntry->ApCliRcvBeaconTime + (60 * OS_HZ)))) {
#ifdef WH_EZ_SETUP
				if(IS_EZ_SETUP_ENABLED(&pApCliEntry->wdev)) {
#ifdef EZ_MOD_SUPPORT 
					unsigned char delay_disconnect_count = ez_get_delay_disconnect_count(&pApCliEntry->wdev);
					if (delay_disconnect_count > 0) {
						delay_disconnect_count--;
						ez_set_delay_disconnect_count(&pApCliEntry->wdev, delay_disconnect_count);
					}else
#else
						if (pApCliEntry->wdev.ez_security.delay_disconnect_count > 0) {
							pApCliEntry->wdev.ez_security.delay_disconnect_count--;
						}else
#endif
						{
							bBeacon_miss = TRUE;
							bForceBrocken = TRUE;
						}
				}
				else
#endif
				{
					bBeacon_miss = TRUE;
					bForceBrocken = TRUE;
				}
			}
#else
#ifdef RACTRL_FW_OFFLOAD_SUPPORT
			if (pAd->chipCap.fgRateAdaptFWOffload == TRUE &&
					(tr_entry->PortSecured == WPA_802_1X_PORT_SECURED))
			{		  
				if (!pMacEntry->bTxPktChk &&
						(RTMP_TIME_AFTER(pAd->Mlme.Now32, pApCliEntry->ApCliRcvBeaconTime + (60 * OS_HZ))))
				{
					pMacEntry->bTxPktChk = TRUE;
					pMacEntry->TxStatRspCnt = 0;
					pMacEntry->TotalTxSuccessCnt = 0;
					pApCliEntry->ApCliLastRcvBeaconTime = pApCliEntry->ApCliRcvBeaconTime;
					HW_GET_TX_STATISTIC(pAd, GET_TX_STAT_ENTRY_TX_CNT, pMacEntry->wcid);
				}
				else if(pMacEntry->bTxPktChk)
				{
					if(pApCliEntry->ApCliLastRcvBeaconTime != pApCliEntry->ApCliRcvBeaconTime)
					{
						pApCliEntry->ApCliRcvBeaconTime = pAd->Mlme.Now32;
						pMacEntry->bTxPktChk = FALSE;
					}
					else
					{
						if(RTMP_TIME_AFTER(pAd->Mlme.Now32, pApCliEntry->ApCliRcvBeaconTime + (60 * OS_HZ)))
						{
							HW_GET_TX_STATISTIC(pAd, GET_TX_STAT_ENTRY_TX_CNT, pMacEntry->wcid);
							if ((pMacEntry->TxStatRspCnt >= 1))
							{
								if(pMacEntry->TotalTxSuccessCnt)
								{
									pApCliEntry->ApCliRcvBeaconTime = pAd->Mlme.Now32;
									pMacEntry->bTxPktChk = FALSE;
								}
								else
								{
#ifdef WH_EZ_SETUP
									if(IS_EZ_SETUP_ENABLED(&pApCliEntry->wdev)) {
#ifdef EZ_MOD_SUPPORT 
										unsigned char delay_disconnect_count = ez_get_delay_disconnect_count(&pApCliEntry->wdev);
										if (delay_disconnect_count > 0) {
											delay_disconnect_count--;
											ez_set_delay_disconnect_count(&pApCliEntry->wdev, delay_disconnect_count);
										}else
#else
											if (pApCliEntry->wdev.ez_security.delay_disconnect_count > 0) {
												pApCliEntry->wdev.ez_security.delay_disconnect_count--;
											}else
#endif
											{
												bBeacon_miss = TRUE;
												bForceBrocken = TRUE;
											}
									}
									else
#endif
									{
										bBeacon_miss = TRUE;
										bForceBrocken = TRUE;
									}
								}
							}
						}
					}
				}
			}
#endif /* RACTRL_FW_OFFLOAD_SUPPORT */
#endif /* CONFIG_MULTI_CHANNEL */
			

			if (CLIENT_STATUS_TEST_FLAG(pMacEntry, fCLIENT_STATUS_WMM_CAPABLE))
				ApclibQosNull = TRUE;

			if ((bForceBrocken == FALSE)
#ifdef CONFIG_MULTI_CHANNEL
			&& (pAd->Mlme.bStartMcc == FALSE)
#endif /* CONFIG_MULTI_CHANNEL */
			)
            {
#if (defined(WH_EZ_SETUP) && defined(RACTRL_FW_OFFLOAD_SUPPORT))
                UCHAR idx, Total;

				if(IS_ADPTR_EZ_SETUP_ENABLED(pAd)){
                	if(pAd->chipCap.fgRateAdaptFWOffload == TRUE)
                    	Total = 3;
                	else
                    	Total = 1;

	                for(idx = 0; idx < Total; idx++)
		        		ApCliRTMPSendNullFrame(pAd, pMacEntry->CurrTxRate, ApclibQosNull, pMacEntry, PWR_ACTIVE);
				}
				else
#endif
				ApCliRTMPSendNullFrame(pAd, pMacEntry->CurrTxRate, ApclibQosNull, pMacEntry, PWR_ACTIVE);
		}
		}
		else
			continue;

		if (bForceBrocken == TRUE)
		{
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("ApCliIfMonitor: IF(apcli%d) - no Beancon is received from root-AP.\n", index));
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("ApCliIfMonitor: Reconnect the Root-Ap again.\n"));

#ifdef CONFIG_MULTI_CHANNEL
			if(pAd->Mlme.bStartMcc == TRUE)
				return;
#endif /* CONFIG_MULTI_CHANNEL */
			if (bBeacon_miss) {
				ULONG Now32;
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("ApCliIfMonitor apcli%d time1: %lu\n",index,pApCliEntry->ApCliRcvBeaconTime_MlmeEnqueueForRecv));
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("ApCliIfMonitor apcli%d time2: %lu\n",index,pApCliEntry->ApCliRcvBeaconTime_MlmeEnqueueForRecv_2));
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("ApCliIfMonitor apcli%d time3: %lu\n",index,pApCliEntry->ApCliRcvBeaconTime));
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("ApCliIfMonitor apcli%d OS_HZ: %d\n",index,OS_HZ));
				NdisGetSystemUpTime(&Now32);
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("ApCliIfMonitor apcli%d time now: %lu\n",index,Now32));
			}
			//MCC TODO: WCID Not Correct when MCC on
#ifdef MCC_TEST
#else
#ifdef MAC_REPEATER_SUPPORT
			if (pAd->ApCfg.bMACRepeaterEn)
			{
				APCLI_STRUCT *apcli_entry = pApCliEntry;
				REPEATER_CLIENT_ENTRY *pReptEntry;
				UCHAR CliIdx;
				RTMP_CHIP_CAP   *cap = &pAd->chipCap;
				for(CliIdx = 0; CliIdx < GET_MAX_REPEATER_ENTRY_NUM(cap); CliIdx++)
				{
					pReptEntry = &pAd->ApCfg.pRepeaterCliPool[CliIdx];
					if ((pReptEntry->CliEnable) && (pReptEntry->wdev == &apcli_entry->wdev))
					{
						if (bBeacon_miss)
						pReptEntry->Disconnect_Sub_Reason = APCLI_DISCONNECT_SUB_REASON_MNT_NO_BEACON;
						else
							pReptEntry->Disconnect_Sub_Reason = APCLI_DISCONNECT_SUB_REASON_APCLI_TRIGGER_TOO_LONG;
						MlmeEnqueue(pAd,
									APCLI_CTRL_STATE_MACHINE,
									APCLI_CTRL_DISCONNECT_REQ,
									0,
									NULL,
									(REPT_MLME_START_IDX + CliIdx));
						RTMP_MLME_HANDLER(pAd);
					}
				}
			}
#endif /* MAC_REPEATER_SUPPORT */
			if (bBeacon_miss)
			pApCliEntry->Disconnect_Sub_Reason = APCLI_DISCONNECT_SUB_REASON_MNT_NO_BEACON;
			else
				pApCliEntry->Disconnect_Sub_Reason = APCLI_DISCONNECT_SUB_REASON_APCLI_TRIGGER_TOO_LONG;
			MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_DISCONNECT_REQ, 0, NULL, index);
			RTMP_MLME_HANDLER(pAd);
#endif /* MCC_TEST */
		}
	}

	return;
}


/*! \brief   To substitute the message type if the message is coming from external
 *  \param  pFrame         The frame received
 *  \param  *Machine       The state machine
 *  \param  *MsgType       the message type for the state machine
 *  \return TRUE if the substitution is successful, FALSE otherwise
 *  \pre
 *  \post
 */
BOOLEAN ApCliMsgTypeSubst(
	IN PRTMP_ADAPTER pAd,
	IN PFRAME_802_11 pFrame,
	OUT INT *Machine,
	OUT INT *MsgType)
{
	USHORT Seq;
	UCHAR EAPType;
	BOOLEAN Return = FALSE;
#ifdef WSC_AP_SUPPORT
	UCHAR EAPCode;
    PMAC_TABLE_ENTRY pEntry;
#endif /* WSC_AP_SUPPORT */


	/* only PROBE_REQ can be broadcast, all others must be unicast-to-me && is_mybssid; otherwise, */
	/* ignore this frame */

	/* WPA EAPOL PACKET */
	if (pFrame->Hdr.FC.Type == FC_TYPE_DATA)
	{
#ifdef WSC_AP_SUPPORT
	        /*WSC EAPOL PACKET */
	        pEntry = MacTableLookup(pAd, pFrame->Hdr.Addr2);
	        if (pEntry && IS_ENTRY_APCLI(pEntry) && pAd->ApCfg.ApCliTab[pEntry->func_tb_idx].WscControl.WscConfMode == WSC_ENROLLEE)
	        {
	            *Machine = WSC_STATE_MACHINE;
	            EAPType = *((UCHAR*)pFrame + LENGTH_802_11 + LENGTH_802_1_H + 1);
	            EAPCode = *((UCHAR*)pFrame + LENGTH_802_11 + LENGTH_802_1_H + 4);
	            Return = WscMsgTypeSubst(EAPType, EAPCode, MsgType);
	        }
	        if (!Return)
#endif /* WSC_AP_SUPPORT */
	        {
	    		*Machine = WPA_STATE_MACHINE;
	    		EAPType = *((UCHAR*)pFrame + LENGTH_802_11 + LENGTH_802_1_H + 1);
	    		Return = WpaMsgTypeSubst(EAPType, MsgType);
	        }
		return Return;
	}
	else if (pFrame->Hdr.FC.Type == FC_TYPE_MGMT)
	{
		switch (pFrame->Hdr.FC.SubType)
		{
			case SUBTYPE_ASSOC_RSP:
				*Machine = APCLI_ASSOC_STATE_MACHINE;
				*MsgType = APCLI_MT2_PEER_ASSOC_RSP;
				break;

			case SUBTYPE_DISASSOC:
				*Machine = APCLI_ASSOC_STATE_MACHINE;
				*MsgType = APCLI_MT2_PEER_DISASSOC_REQ;
				break;

			case SUBTYPE_DEAUTH:				
				*Machine = APCLI_AUTH_STATE_MACHINE;
				*MsgType = APCLI_MT2_PEER_DEAUTH;				
				break;

			case SUBTYPE_AUTH:
				/* get the sequence number from payload 24 Mac Header + 2 bytes algorithm */
				NdisMoveMemory(&Seq, &pFrame->Octet[2], sizeof(USHORT));
				if (Seq == 2 || Seq == 4)
				{
					*Machine = APCLI_AUTH_STATE_MACHINE;
					*MsgType = APCLI_MT2_PEER_AUTH_EVEN;
				}
				else
				{
					return FALSE;
				}
				break;

			case SUBTYPE_ACTION:
				*Machine = ACTION_STATE_MACHINE;
				/*  Sometimes Sta will return with category bytes with MSB = 1, if they receive catogory out of their support */
				if ((pFrame->Octet[0]&0x7F) > MAX_PEER_CATE_MSG)
					*MsgType = MT2_ACT_INVALID;
				else
					*MsgType = (pFrame->Octet[0]&0x7F);
				break;

			default:
				return FALSE;
		}

		return TRUE;
	}

	return FALSE;
}


BOOLEAN preCheckMsgTypeSubset(
	IN PRTMP_ADAPTER  pAd,
	IN PFRAME_802_11 pFrame,
	OUT INT *Machine,
	OUT INT *MsgType)
{
	if (pFrame->Hdr.FC.Type == FC_TYPE_MGMT)
	{
		switch (pFrame->Hdr.FC.SubType)
		{
			/* Beacon must be processed by AP Sync state machine. */
			case SUBTYPE_BEACON:
				*Machine = AP_SYNC_STATE_MACHINE;
				*MsgType = APMT2_PEER_BEACON;
				break;

			/* Only Sta have chance to receive Probe-Rsp. */
			case SUBTYPE_PROBE_RSP:
				*Machine = APCLI_SYNC_STATE_MACHINE;
				*MsgType = APCLI_MT2_PEER_PROBE_RSP;
				break;

			default:
				return FALSE;
		}
		return TRUE;
	}
	return FALSE;
}


/*
    ==========================================================================
    Description:
        MLME message sanity check
    Return:
        TRUE if all parameters are OK, FALSE otherwise

    IRQL = DISPATCH_LEVEL

    ==========================================================================
 */
BOOLEAN ApCliPeerAssocRspSanity(
    IN PRTMP_ADAPTER pAd,
    IN VOID *pMsg,
    IN ULONG MsgLen,
    OUT PUCHAR pAddr2,
    OUT USHORT *pCapabilityInfo,
    OUT USHORT *pStatus,
    OUT USHORT *pAid,
    OUT UCHAR SupRate[],
    OUT UCHAR *pSupRateLen,
    OUT UCHAR ExtRate[],
    OUT UCHAR *pExtRateLen,
    OUT HT_CAPABILITY_IE *pHtCapability,
    OUT ADD_HT_INFO_IE *pAddHtInfo,	/* AP might use this additional ht info IE */
    OUT UCHAR *pHtCapabilityLen,
    OUT UCHAR *pAddHtInfoLen,
    OUT UCHAR *pNewExtChannelOffset,
    OUT PEDCA_PARM pEdcaParm,
    OUT UCHAR *pCkipFlag,
    OUT IE_LISTS *ie_list)
{
	CHAR          IeType, *Ptr;
	PFRAME_802_11 pFrame = (PFRAME_802_11)pMsg;
	PEID_STRUCT   pEid;
	ULONG         Length = 0;

	*pNewExtChannelOffset = 0xff;
	*pHtCapabilityLen = 0;
	*pAddHtInfoLen = 0;
	COPY_MAC_ADDR(pAddr2, pFrame->Hdr.Addr2);
	Ptr = (CHAR *) pFrame->Octet;
	Length += LENGTH_802_11;

	NdisMoveMemory(pCapabilityInfo, &pFrame->Octet[0], 2);
	Length += 2;
	NdisMoveMemory(pStatus,         &pFrame->Octet[2], 2);
	Length += 2;
	*pCkipFlag = 0;
	*pExtRateLen = 0;
	pEdcaParm->bValid = FALSE;

	if (*pStatus != MLME_SUCCESS)
		return TRUE;

	NdisMoveMemory(pAid, &pFrame->Octet[4], 2);
	Length += 2;

	/* Aid already swaped byte order in RTMPFrameEndianChange() for big endian platform */
	*pAid = (*pAid) & 0x3fff; /* AID is low 14-bit */

	/* -- get supported rates from payload and advance the pointer */
	IeType = pFrame->Octet[6];
	*pSupRateLen = pFrame->Octet[7];
	if ((IeType != IE_SUPP_RATES) || (*pSupRateLen > MAX_LEN_OF_SUPPORTED_RATES))
	{
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("%s(): fail - wrong SupportedRates IE\n", __FUNCTION__));
		return FALSE;
	}
	else
		NdisMoveMemory(SupRate, &pFrame->Octet[8], *pSupRateLen);

	Length = Length + 2 + *pSupRateLen;

	/* many AP implement proprietary IEs in non-standard order, we'd better */
	/* tolerate mis-ordered IEs to get best compatibility */
	pEid = (PEID_STRUCT) &pFrame->Octet[8 + (*pSupRateLen)];

	/* get variable fields from payload and advance the pointer */
	while ((Length + 2 + pEid->Len) <= MsgLen)
	{
		switch (pEid->Eid)
		{
			case IE_EXT_SUPP_RATES:
				if (pEid->Len <= MAX_LEN_OF_SUPPORTED_RATES)
				{
					NdisMoveMemory(ExtRate, pEid->Octet, pEid->Len);
					*pExtRateLen = pEid->Len;
				}
				break;
#ifdef DOT11_N_SUPPORT
			case IE_HT_CAP:
			case IE_HT_CAP2:
				if (pEid->Len >= SIZE_HT_CAP_IE)  /*Note: allow extension.!! */
				{
					NdisMoveMemory(pHtCapability, pEid->Octet, SIZE_HT_CAP_IE);
					*(USHORT *) (&pHtCapability->HtCapInfo) = cpu2le16(*(USHORT *)(&pHtCapability->HtCapInfo));
					*(USHORT *) (&pHtCapability->ExtHtCapInfo) = cpu2le16(*(USHORT *)(&pHtCapability->ExtHtCapInfo));
					*pHtCapabilityLen = SIZE_HT_CAP_IE;
				}
				else
				{
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_WARN, ("%s():wrong IE_HT_CAP\n", __FUNCTION__));
				}

				break;
			case IE_ADD_HT:
			case IE_ADD_HT2:
				if (pEid->Len >= sizeof(ADD_HT_INFO_IE))
				{
					/* This IE allows extension, but we can ignore extra bytes beyond our knowledge , so only */
					/* copy first sizeof(ADD_HT_INFO_IE) */
					NdisMoveMemory(pAddHtInfo, pEid->Octet, sizeof(ADD_HT_INFO_IE));
					*pAddHtInfoLen = SIZE_ADD_HT_INFO_IE;
				}
				else
				{
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_WARN, ("%s():wrong IE_ADD_HT\n", __FUNCTION__));
				}
				break;
			case IE_SECONDARY_CH_OFFSET:
				if (pEid->Len == 1)
				{
					*pNewExtChannelOffset = pEid->Octet[0];
				}
				else
				{
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_WARN, ("%s():wrong IE_SECONDARY_CH_OFFSET\n", __FUNCTION__));
				}
				break;
#ifdef DOT11_VHT_AC
			case IE_VHT_CAP:
				if (pEid->Len == sizeof(VHT_CAP_IE)) {
					NdisMoveMemory(&ie_list->vht_cap, pEid->Octet, sizeof(VHT_CAP_IE));
					ie_list->vht_cap_len = sizeof(VHT_CAP_IE);
				} else {
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_WARN, ("%s():wrong IE_VHT_CAP\n", __FUNCTION__));
				}
				break;
			case IE_VHT_OP:
				if (pEid->Len == sizeof(VHT_OP_IE)) {
					NdisMoveMemory(&ie_list->vht_op, pEid->Octet, sizeof(VHT_OP_IE));
					ie_list->vht_op_len = sizeof(VHT_OP_IE);
				}else {
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_WARN, ("%s():wrong IE_VHT_OP\n", __FUNCTION__));
				}
				break;
#endif /* DOT11_VHT_AC */
#endif /* DOT11_N_SUPPORT */
			/* CCX2, WMM use the same IE value */
			/* case IE_CCX_V2: */
			case IE_VENDOR_SPECIFIC:
				/* handle WME PARAMTER ELEMENT */
				if (NdisEqualMemory(pEid->Octet, WME_PARM_ELEM, 6) && (pEid->Len == 24))
				{
					PUCHAR ptr;
					int i;

					/* parsing EDCA parameters */
					pEdcaParm->bValid          = TRUE;
					pEdcaParm->bQAck           = FALSE; /* pEid->Octet[0] & 0x10; */
					pEdcaParm->bQueueRequest   = FALSE; /* pEid->Octet[0] & 0x20; */
					pEdcaParm->bTxopRequest    = FALSE; /* pEid->Octet[0] & 0x40; */
					/*pEdcaParm->bMoreDataAck    = FALSE; // pEid->Octet[0] & 0x80; */
					pEdcaParm->EdcaUpdateCount = pEid->Octet[6] & 0x0f;
					pEdcaParm->bAPSDCapable    = (pEid->Octet[6] & 0x80) ? 1 : 0;
					ptr = (PUCHAR) &pEid->Octet[8];
					for (i=0; i<4; i++)
					{
						UCHAR aci = (*ptr & 0x60) >> 5; /* b5~6 is AC INDEX */
						pEdcaParm->bACM[aci]  = (((*ptr) & 0x10) == 0x10);   /* b5 is ACM */
						pEdcaParm->Aifsn[aci] = (*ptr) & 0x0f;               /* b0~3 is AIFSN */
						pEdcaParm->Cwmin[aci] = *(ptr+1) & 0x0f;             /* b0~4 is Cwmin */
						pEdcaParm->Cwmax[aci] = *(ptr+1) >> 4;               /* b5~8 is Cwmax */
						pEdcaParm->Txop[aci]  = *(ptr+2) + 256 * (*(ptr+3)); /* in unit of 32-us */
						ptr += 4; /* point to next AC */
					}
				}
				break;
				default:
					MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("%s():ignore unrecognized EID = %d\n", __FUNCTION__, pEid->Eid));
					break;
		}

		Length = Length + 2 + pEid->Len;
		pEid = (PEID_STRUCT)((UCHAR*)pEid + 2 + pEid->Len);
	}

	return TRUE;
}


MAC_TABLE_ENTRY *ApCliTableLookUpByWcid(RTMP_ADAPTER *pAd, UCHAR wcid, UCHAR *pAddrs)
{
	ULONG ApCliIndex;
	PMAC_TABLE_ENTRY pCurEntry = NULL;
	PMAC_TABLE_ENTRY pEntry = NULL;

	RETURN_ZERO_IF_PAD_NULL(pAd);
	
	if (!VALID_UCAST_ENTRY_WCID(pAd, wcid))
		return NULL;

	NdisAcquireSpinLock(&pAd->MacTabLock);

	do
	{
		pCurEntry = &pAd->MacTab.Content[wcid];

		ApCliIndex = 0xff;
		if ((pCurEntry) &&
            (IS_ENTRY_APCLI(pCurEntry) || IS_ENTRY_REPEATER(pCurEntry))
        )
		{
			ApCliIndex = pCurEntry->func_tb_idx;
		}

		if ((ApCliIndex == 0xff) || (ApCliIndex >= MAX_APCLI_NUM))
			break;

		if (pAd->ApCfg.ApCliTab[ApCliIndex].Valid != TRUE)
			break;

		if (MAC_ADDR_EQUAL(pCurEntry->Addr, pAddrs))
		{
			pEntry = pCurEntry;
			break;
		}
	} while(FALSE);

	NdisReleaseSpinLock(&pAd->MacTabLock);

	return pEntry;
}


/*
	==========================================================================
	Description:
		Check the Apcli Entry is valid or not.
	==========================================================================
 */
inline BOOLEAN ValidApCliEntry(RTMP_ADAPTER *pAd, INT apCliIdx)
{
	BOOLEAN result;
	PMAC_TABLE_ENTRY pMacEntry;
	APCLI_STRUCT *pApCliEntry;

	do
	{
		if ((apCliIdx < 0) || (apCliIdx >= MAX_APCLI_NUM))
		{
			result = FALSE;
			break;
		}

		pApCliEntry = (APCLI_STRUCT *)&pAd->ApCfg.ApCliTab[apCliIdx];
		if (pApCliEntry->Valid != TRUE)
		{
			result = FALSE;
			break;
		}

		if (pApCliEntry->Enable != TRUE)
		{
			result = FALSE;
			break;
		}

		if ((!VALID_UCAST_ENTRY_WCID(pAd, pApCliEntry->MacTabWCID))
		/* || (pApCliEntry->MacTabWCID < 0)  //MacTabWCID is UCHAR, no need to check */
		)
		{
			result = FALSE;
			break;
		}

		pMacEntry = &pAd->MacTab.Content[pApCliEntry->MacTabWCID];
		if (!IS_ENTRY_APCLI(pMacEntry))
		{
			result = FALSE;
			break;
		}

		result = TRUE;
	} while(FALSE);

	return result;
}

#define FLG_IS_OUTPUT 1
#define FLAG_IS_INPUT 0

INT ApCliAllowToSendPacket(
	IN RTMP_ADAPTER *pAd,
	IN struct wifi_dev *wdev,
	IN PNDIS_PACKET pPacket,
	OUT UCHAR *pWcid)
{
	UCHAR idx;
	BOOLEAN	allowed = FALSE;
	APCLI_STRUCT *apcli_entry;
#ifdef MAC_REPEATER_SUPPORT
    UINT Ret = 0;
#endif


	for(idx = 0; idx < MAX_APCLI_NUM; idx++)
	{
		apcli_entry = &pAd->ApCfg.ApCliTab[idx];
		if (&apcli_entry->wdev == wdev)
		{
			if (ValidApCliEntry(pAd, idx) == FALSE)
				break;
			if (!IS_ASIC_CAP(pAd, fASIC_CAP_WMM_PKTDETECT_OFFLOAD))
			{
				mt_detect_wmm_traffic(pAd, pPacket, 
					QID_AC_BE, FLG_IS_OUTPUT);
			}

#ifdef MAC_REPEATER_SUPPORT
			if ((pAd->ApCfg.bMACRepeaterEn == TRUE)
#ifdef MWDS
				&& (apcli_entry->bEnableMWDS == FALSE)
#endif /* MWDS */

			)
			{
                Ret = ReptTxPktCheckHandler(pAd, wdev, pPacket, pWcid);
                if (Ret == REPEATER_ENTRY_EXIST)
                    return TRUE;
                else if (Ret == INSERT_REPT_ENTRY){
					//MTWF_LOG(DBG_CAT_ALL, DBG_SUBCAT_ALL, DBG_LVL_WARN,
					//    ("ApCliAllowToSendPacket: return FALSE as ReptTxPktCheckHandler indicated INSERT_REPT_ENTRY\n"));
                    return FALSE;
                }
                else if (Ret == USE_CLI_LINK_INFO)
                    *pWcid = apcli_entry->MacTabWCID;
			}
			else
#endif /* MAC_REPEATER_SUPPORT */
			{
				pAd->RalinkCounters.PendingNdisPacketCount ++;
				RTMP_SET_PACKET_WDEV(pPacket, wdev->wdev_idx);
				*pWcid = apcli_entry->MacTabWCID;
			}
			allowed = TRUE;
#ifdef MWDS
            if((apcli_entry->bEnableMWDS == FALSE) &&
                (apcli_entry->MlmeAux.bSupportMWDS) &&
                (wdev->bSupportMWDS)){
				//MTWF_LOG(DBG_CAT_ALL, DBG_SUBCAT_ALL, DBG_LVL_WARN,
				//    ("ApCliAllowToSendPacket: return FALSE as MWDS not enabled even though supported\n"));
                allowed = FALSE;
            }
#endif /* MWDS */
			break;
		}
	}

	return allowed;
}


/*
	========================================================================

	Routine Description:
		Validate the security configuration against the RSN information
		element

	Arguments:
		pAdapter	Pointer	to our adapter
		eid_ptr 	Pointer to VIE

	Return Value:
		TRUE 	for configuration match
		FALSE	for otherwise

	Note:

	========================================================================
*/
BOOLEAN ApCliValidateRSNIE(
    IN RTMP_ADAPTER *pAd,
    IN PEID_STRUCT pEid_ptr,
    IN USHORT eid_len,
    IN USHORT idx,
    IN UCHAR Privacy)
{
    PUCHAR pVIE, pTmp;
    UCHAR len;
    PEID_STRUCT pEid;
    PAPCLI_STRUCT pApCliEntry = NULL;
    USHORT Count;
    PRSN_IE_HEADER_STRUCT pRsnHeader;
    PCIPHER_SUITE_STRUCT pCipher;
    PAKM_SUITE_STRUCT pAKM;
    struct _SECURITY_CONFIG *pSecConfig;

#ifdef WH_EZ_SETUP
	struct wifi_dev *ap_wdev;
	ap_wdev = &pAd->ApCfg.MBSSID[idx].wdev;
#endif	

    pVIE = (PUCHAR) pEid_ptr;
    len  = eid_len;
    pApCliEntry = &pAd->ApCfg.ApCliTab[idx];
    pSecConfig = &pApCliEntry->wdev.SecConfig;

    if (IS_SECURITY(pSecConfig) && (Privacy == 0))
    {
        return FALSE; /* None matched*/
    }
    else if (IS_NO_SECURITY(pSecConfig) && (Privacy == 1))
    {
        return FALSE; /* None matched*/
    }
    else if (IS_NO_SECURITY(pSecConfig) && (Privacy == 0))
    {
		CLEAR_SEC_AKM(pApCliEntry->MlmeAux.AKMMap);
		CLEAR_CIPHER(pApCliEntry->MlmeAux.PairwiseCipher);
		CLEAR_CIPHER(pApCliEntry->MlmeAux.GroupCipher);

        SET_AKM_OPEN(pApCliEntry->MlmeAux.AKMMap);
        SET_CIPHER_NONE(pApCliEntry->MlmeAux.PairwiseCipher);
        SET_CIPHER_NONE(pApCliEntry->MlmeAux.GroupCipher);
        return TRUE; /* No Security */
    }

	CLEAR_SEC_AKM(pApCliEntry->MlmeAux.AKMMap);
	CLEAR_CIPHER(pApCliEntry->MlmeAux.PairwiseCipher);
	CLEAR_CIPHER(pApCliEntry->MlmeAux.GroupCipher);

    /* 1. Parse Cipher this received RSNIE */
    while (len > 0)
    {
        pTmp = pVIE;
        pEid = (PEID_STRUCT) pTmp;

        switch(pEid->Eid)
        {
            case IE_WPA:
                if (NdisEqualMemory(pEid->Octet, WPA_OUI, 4) != 1)
                {
                    /* if unsupported vendor specific IE */
                    break;
                }
                /* Skip OUI ,version and multicast suite OUI */
                pTmp += 11;

                /*
                    Cipher Suite Selectors from Spec P802.11i/D3.2 P26.
                    Value	   Meaning
                    0			None
                    1			WEP-40
                    2			Tkip
                    3			WRAP
                    4			AES
                    5			WEP-104
                */
                /* Parse group cipher*/
                switch (*pTmp)
                {
                    case 1:
                        SET_CIPHER_WEP40(pApCliEntry->MlmeAux.GroupCipher);
                        break;
                    case 5:
                        SET_CIPHER_WEP104(pApCliEntry->MlmeAux.GroupCipher);
                        break;
                    case 2:
                        SET_CIPHER_TKIP(pApCliEntry->MlmeAux.GroupCipher);
                        break;
                    case 4:
                        SET_CIPHER_CCMP128(pApCliEntry->MlmeAux.GroupCipher);
                        break;
                    default:
                        break;
                }
                /* number of unicast suite*/
                pTmp   += 1;

                /* skip all unicast cipher suites*/
                Count = (pTmp[1]<<8) + pTmp[0];
                pTmp   += sizeof(USHORT);

                /* Parsing all unicast cipher suite*/
                while (Count > 0)
                {
                    /* Skip OUI*/
                    pTmp += 3;
                    switch (*pTmp)
                    {
                        case 1:
                            SET_CIPHER_WEP40(pApCliEntry->MlmeAux.PairwiseCipher);
                            break;
                        case 5: /* Although WEP is not allowed in WPA related auth mode, we parse it anyway*/
                            SET_CIPHER_WEP104(pApCliEntry->MlmeAux.PairwiseCipher);
                            break;
                        case 2:
                            SET_CIPHER_TKIP(pApCliEntry->MlmeAux.PairwiseCipher);
                            break;
                        case 4:
                            SET_CIPHER_CCMP128(pApCliEntry->MlmeAux.PairwiseCipher);
                            break;
                        default:
                            break;
                    }
                    pTmp++;
                    Count--;
            }


            /* 4. get AKM suite counts*/
            Count = (pTmp[1]<<8) + pTmp[0];
            pTmp   += sizeof(USHORT);
            pTmp   += 3;
            switch (*pTmp)
            {
                case 1:
                    /* Set AP support WPA-enterprise mode*/
                    SET_AKM_WPA1(pApCliEntry->MlmeAux.AKMMap);
                    break;
                case 2:
                    /* Set AP support WPA-PSK mode*/
                    SET_AKM_WPA1PSK(pApCliEntry->MlmeAux.AKMMap);
                    break;
                default:
                    break;
            }
            pTmp   += 1;

            break; /* End of case IE_WPA */
        case IE_RSN:
            pRsnHeader = (PRSN_IE_HEADER_STRUCT) pTmp;

            /* 0. Version must be 1*/
            if (le2cpu16(pRsnHeader->Version) != 1)
                break;
            pTmp   += sizeof(RSN_IE_HEADER_STRUCT);

            /* 1. Check group cipher*/
            pCipher = (PCIPHER_SUITE_STRUCT) pTmp;
            if (!RTMPEqualMemory(&pCipher->Oui, RSN_OUI, 3))
                break;


            /* Parse group cipher*/
            switch (pCipher->Type)
            {
                case 1:
                    SET_CIPHER_WEP40(pApCliEntry->MlmeAux.GroupCipher);
                    break;
                case 2:
                    SET_CIPHER_TKIP(pApCliEntry->MlmeAux.GroupCipher);
                    break;
                case 4:
                    SET_CIPHER_CCMP128(pApCliEntry->MlmeAux.GroupCipher);
                    break;
                case 5:
                    SET_CIPHER_WEP104(pApCliEntry->MlmeAux.GroupCipher);
                    break;
                case 8:
                    SET_CIPHER_GCMP128(pApCliEntry->MlmeAux.GroupCipher);
                    break;
                case 9:
                    SET_CIPHER_GCMP256(pApCliEntry->MlmeAux.GroupCipher);
                    break;
                case 10:
                    SET_CIPHER_CCMP256(pApCliEntry->MlmeAux.GroupCipher);
                    break;
                default:
                    break;
            }

            /* set to correct offset for next parsing*/
            pTmp   += sizeof(CIPHER_SUITE_STRUCT);

            /* 2. Get pairwise cipher counts*/
            Count = (pTmp[1]<<8) + pTmp[0];
            pTmp   += sizeof(USHORT);


            /* 3. Get pairwise cipher*/
            /* Parsing all unicast cipher suite*/
            while (Count > 0)
            {
                /* Skip OUI*/
                pCipher = (PCIPHER_SUITE_STRUCT) pTmp;
                switch (pCipher->Type)
                {
                    case 1:
                        SET_CIPHER_WEP40(pApCliEntry->MlmeAux.PairwiseCipher);
                        break;
                    case 2:
                        SET_CIPHER_TKIP(pApCliEntry->MlmeAux.PairwiseCipher);
                        break;
                    case 4:
                        SET_CIPHER_CCMP128(pApCliEntry->MlmeAux.PairwiseCipher);
                        break;
                    case 5:
                        SET_CIPHER_WEP104(pApCliEntry->MlmeAux.PairwiseCipher);
                        break;
                    case 8:
                        SET_CIPHER_GCMP128(pApCliEntry->MlmeAux.PairwiseCipher);
                        break;
                    case 9:
                        SET_CIPHER_GCMP256(pApCliEntry->MlmeAux.PairwiseCipher);
                        break;
                    case 10:
                        SET_CIPHER_CCMP256(pApCliEntry->MlmeAux.PairwiseCipher);
                        break;
                    default:
                        break;
                }
                pTmp += sizeof(CIPHER_SUITE_STRUCT);
                Count--;
            }

            /* 4. get AKM suite counts*/
            Count = (pTmp[1]<<8) + pTmp[0];
            pTmp   += sizeof(USHORT);

            /* 5. Get AKM ciphers*/
            /* Parsing all AKM ciphers*/
            while (Count > 0)
            {
                pAKM = (PAKM_SUITE_STRUCT) pTmp;
                if (!RTMPEqualMemory(pTmp, RSN_OUI, 3))
                    break;

                switch (pAKM->Type)
                {
                    case 0:
                        SET_AKM_WPANONE(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 1:
                        SET_AKM_WPA2(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 2:
                        SET_AKM_WPA2PSK(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 3:
                        SET_AKM_FT_WPA2(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 4:
                        SET_AKM_FT_WPA2PSK(pApCliEntry->MlmeAux.AKMMap);
                        break;

#ifdef DOT11W_PMF_SUPPORT
                    case 5:
                        SET_AKM_WPA2(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 6:
                        SET_AKM_WPA2PSK(pApCliEntry->MlmeAux.AKMMap);
                        break;
#else /* DOT11W_PMF_SUPPORT */
                    case 5:
                        SET_AKM_WPA2_SHA256(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 6:
                        SET_AKM_WPA2PSK_SHA256(pApCliEntry->MlmeAux.AKMMap);
                        break;
#endif /* !DOT11W_PMF_SUPPORT */

                    case 7:
                        SET_AKM_TDLS(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 8:
                        SET_AKM_SAE_SHA256(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 9:
                        SET_AKM_FT_SAE_SHA256(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 11:
                        SET_AKM_SUITEB_SHA256(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 12:
                        SET_AKM_SUITEB_SHA384(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    case 13:
                        SET_AKM_FT_WPA2_SHA384(pApCliEntry->MlmeAux.AKMMap);
                        break;
                    default:
                        break;
                }
                pTmp   += sizeof(AKM_SUITE_STRUCT);
                Count--;
            }
    	}

	/* skip this Eid */
	pVIE += (pEid->Len + 2);
	len  -= (pEid->Len + 2);
    }

    if ((pApCliEntry->MlmeAux.AKMMap == 0x0) && (Privacy == 1))
    { 
        /* WEP mode */
	if (IS_AKM_AUTOSWITCH(pSecConfig->AKMMap)) {
            SET_AKM_AUTOSWITCH(pApCliEntry->MlmeAux.AKMMap);
        } else if (IS_AKM_OPEN(pSecConfig->AKMMap)) {
            SET_AKM_OPEN(pApCliEntry->MlmeAux.AKMMap);
        } else if (IS_AKM_SHARED(pSecConfig->AKMMap)) {
            SET_AKM_SHARED(pApCliEntry->MlmeAux.AKMMap);
        } else {
            SET_AKM_OPEN(pApCliEntry->MlmeAux.AKMMap);
        }
        SET_CIPHER_WEP(pApCliEntry->MlmeAux.PairwiseCipher);
        SET_CIPHER_WEP(pApCliEntry->MlmeAux.GroupCipher);

    }

    pApCliEntry->MlmeAux.AKMMap &=  pSecConfig->AKMMap;
    pApCliEntry->MlmeAux.PairwiseCipher &=  pSecConfig->PairwiseCipher;

    if ((pApCliEntry->MlmeAux.AKMMap == 0)
        || (pApCliEntry->MlmeAux.PairwiseCipher == 0))
		return FALSE; /* None matched*/

    /* Decide Pairwise and group cipher with AP */
    if (IS_AKM_WPA1(pApCliEntry->MlmeAux.AKMMap) && IS_AKM_WPA2(pApCliEntry->MlmeAux.AKMMap))
    {
    	CLEAR_SEC_AKM(pApCliEntry->MlmeAux.AKMMap);
        SET_AKM_WPA2(pApCliEntry->MlmeAux.AKMMap);
    }
    else if (IS_AKM_WPA1PSK(pApCliEntry->MlmeAux.AKMMap) && IS_AKM_WPA2PSK(pApCliEntry->MlmeAux.AKMMap))
    {
    	CLEAR_SEC_AKM(pApCliEntry->MlmeAux.AKMMap);
        SET_AKM_WPA2PSK(pApCliEntry->MlmeAux.AKMMap);
    }

    if (IS_CIPHER_TKIP(pApCliEntry->MlmeAux.PairwiseCipher) && IS_CIPHER_CCMP128(pApCliEntry->MlmeAux.PairwiseCipher))
    {
    	CLEAR_CIPHER(pApCliEntry->MlmeAux.PairwiseCipher);
        SET_CIPHER_CCMP128(pApCliEntry->MlmeAux.PairwiseCipher);
    }

    MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("%s(): Candidate Security AKMMap=%s, PairwiseCipher=%s, GroupCipher=%s\n",
		__FUNCTION__,
		GetAuthModeStr(pApCliEntry->MlmeAux.AKMMap),
		GetEncryModeStr(pApCliEntry->MlmeAux.PairwiseCipher),
		GetEncryModeStr(pApCliEntry->MlmeAux.GroupCipher)));

    return TRUE;
}

BOOLEAN  ApCliHandleRxBroadcastFrame(
	IN RTMP_ADAPTER *pAd,
	IN RX_BLK *pRxBlk,
	IN MAC_TABLE_ENTRY *pEntry)
{
	APCLI_STRUCT *pApCliEntry = NULL;
#ifdef MAC_REPEATER_SUPPORT
	REPEATER_CLIENT_ENTRY *pReptEntry = NULL;
#endif /* MAC_REPEATER_SUPPORT */

	/*
		It is possible to receive the multicast packet when in AP Client mode
		ex: broadcast from remote AP to AP-client,
				addr1=ffffff, addr2=remote AP's bssid, addr3=sta4_mac_addr
	*/
	pApCliEntry = &pAd->ApCfg.ApCliTab[pEntry->func_tb_idx];

	/* Filter out Bcast frame which AP relayed for us */
	/* Multicast packet send from AP1 , received by AP2 and send back to AP1, drop this frame */
#ifdef MWDS
    if(IS_MWDS_OPMODE_APCLI(pEntry) &&
       MAC_ADDR_EQUAL(pRxBlk->Addr4, pApCliEntry->wdev.if_addr))
        return FALSE;
#endif /* MWDS */

	if (MAC_ADDR_EQUAL(pRxBlk->Addr3, pApCliEntry->wdev.if_addr))
		return FALSE;

	if (pEntry->PrivacyFilter != Ndis802_11PrivFilterAcceptAll)
		return FALSE;

#ifdef MAC_REPEATER_SUPPORT
	if (pAd->ApCfg.bMACRepeaterEn
#ifdef MWDS
        && (pApCliEntry->bEnableMWDS == FALSE)
#endif /* MWDS */
    )
	{
		pReptEntry = RTMPLookupRepeaterCliEntry(pAd, FALSE, pRxBlk->Addr3, TRUE);
		if (pReptEntry){
			//MTWF_LOG(DBG_CAT_ALL, DBG_SUBCAT_ALL, DBG_LVL_WARN,
			//    ("ApCliHandleRxBroadcastFrame: return FALSE  pReptEntry found\n"));
			return FALSE;	/* give up this frame */
	}
	}
#endif /* MAC_REPEATER_SUPPORT */

	return TRUE;
}

/*
	========================================================================

	Routine Description:
		Verify the support rate for different PHY type

	Arguments:
		pAd 				Pointer to our adapter

	Return Value:
		None

	IRQL = PASSIVE_LEVEL

	========================================================================
*/
// TODO: shiang-6590, modify this due to it's really a duplication of "RTMPUpdateMlmeRate()" in common/mlme.c
VOID ApCliUpdateMlmeRate(RTMP_ADAPTER *pAd, USHORT ifIndex)
{
	UCHAR	MinimumRate;
	UCHAR	ProperMlmeRate; /*= RATE_54; */
	UCHAR	i, j, RateIdx = 12; /* 1, 2, 5.5, 11, 6, 9, 12, 18, 24, 36, 48, 54 */
	BOOLEAN	bMatch = FALSE;
	struct wifi_dev *wdev;
	struct dev_rate_info *rate;

	PAPCLI_STRUCT pApCliEntry = NULL;

	if (ifIndex >= MAX_APCLI_NUM)
		return;

	pApCliEntry = &pAd->ApCfg.ApCliTab[ifIndex];
	wdev = &pApCliEntry->wdev;
	rate = &wdev->rate;

	switch (wdev->PhyMode)
	{
		case (WMODE_B):
			ProperMlmeRate = RATE_11;
			MinimumRate = RATE_1;
			break;
		case (WMODE_B | WMODE_G):
#ifdef DOT11_N_SUPPORT
		case (WMODE_A |WMODE_B | WMODE_G | WMODE_GN | WMODE_AN):
		case (WMODE_B | WMODE_G | WMODE_GN):
#ifdef DOT11_VHT_AC
		case (WMODE_A |WMODE_B | WMODE_G | WMODE_GN | WMODE_AN | WMODE_AC):
#endif /* DOT11_VHT_AC */
#endif /* DOT11_N_SUPPORT */
			if ((pApCliEntry->MlmeAux.SupRateLen == 4) &&
				(pApCliEntry->MlmeAux.ExtRateLen == 0))
				ProperMlmeRate = RATE_11; /* B only AP */
			else
				ProperMlmeRate = RATE_24;

			if (pApCliEntry->MlmeAux.Channel <= 14)
				MinimumRate = RATE_1;
			else
				MinimumRate = RATE_6;
			break;
		case (WMODE_A):
#ifdef DOT11_N_SUPPORT
		case (WMODE_GN):
		case (WMODE_G | WMODE_GN):
		case (WMODE_A | WMODE_G | WMODE_AN | WMODE_GN):
		case (WMODE_A | WMODE_AN):
		case (WMODE_AN):
#ifdef DOT11_VHT_AC
		case (WMODE_AC):
		case (WMODE_AN | WMODE_AC):
		case (WMODE_A | WMODE_AN | WMODE_AC):
#endif /* DOT11_VHT_AC */
#endif /* DOT11_N_SUPPORT */
			ProperMlmeRate = RATE_24;
			MinimumRate = RATE_6;
			break;
		case (WMODE_B | WMODE_A | WMODE_G):
			ProperMlmeRate = RATE_24;
			if (pApCliEntry->MlmeAux.Channel <= 14)
			   MinimumRate = RATE_1;
			else
				MinimumRate = RATE_6;
			break;
		default: /* error */
			ProperMlmeRate = RATE_1;
			MinimumRate = RATE_1;
			break;
	}

	for (i = 0; i < pApCliEntry->MlmeAux.SupRateLen; i++)
	{
		for (j = 0; j < RateIdx; j++)
		{
			if ((pApCliEntry->MlmeAux.SupRate[i] & 0x7f) == RateIdTo500Kbps[j])
			{
				if (j == ProperMlmeRate)
				{
					bMatch = TRUE;
					break;
				}
			}
		}

		if (bMatch)
			break;
	}

	if (bMatch == FALSE)
	{
		for (i = 0; i < pApCliEntry->MlmeAux.ExtRateLen; i++)
		{
			for (j = 0; j < RateIdx; j++)
			{
				if ((pApCliEntry->MlmeAux.ExtRate[i] & 0x7f) == RateIdTo500Kbps[j])
				{
					if (j == ProperMlmeRate)
					{
						bMatch = TRUE;
						break;
					}
				}
			}

			if (bMatch)
				break;
		}
	}

	if (bMatch == FALSE)
		ProperMlmeRate = MinimumRate;

	if(!OPSTATUS_TEST_FLAG(pAd, fOP_AP_STATUS_MEDIA_STATE_CONNECTED))
	{
		pAd->CommonCfg.MlmeRate = MinimumRate;
		pAd->CommonCfg.RtsRate = ProperMlmeRate;
		if (pAd->CommonCfg.MlmeRate >= RATE_6)
		{
			rate->MlmeTransmit.field.MODE = MODE_OFDM;
			rate->MlmeTransmit.field.MCS = OfdmRateToRxwiMCS[pAd->CommonCfg.MlmeRate];
			pAd->MacTab.Content[BSS0Mcast_WCID].HTPhyMode.field.MODE = MODE_OFDM;
			pAd->MacTab.Content[BSS0Mcast_WCID].HTPhyMode.field.MCS = OfdmRateToRxwiMCS[pAd->CommonCfg.MlmeRate];
		}
		else
		{
			rate->MlmeTransmit.field.MODE = MODE_CCK;
			rate->MlmeTransmit.field.MCS = pAd->CommonCfg.MlmeRate;
			pAd->MacTab.Content[BSS0Mcast_WCID].HTPhyMode.field.MODE = MODE_CCK;
			pAd->MacTab.Content[BSS0Mcast_WCID].HTPhyMode.field.MCS = pAd->CommonCfg.MlmeRate;
		}
	}

	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("%s():=>MlmeTransmit=0x%x, MinimumRate=%d, ProperMlmeRate=%d\n",
				__FUNCTION__, rate->MlmeTransmit.word, MinimumRate, ProperMlmeRate));
}


VOID ApCliCheckPeerExistence(RTMP_ADAPTER *pAd, CHAR *Ssid, UCHAR SsidLen, UCHAR Channel)
{
	UCHAR ifIndex;
	APCLI_STRUCT *pApCliEntry;
	
	for (ifIndex = 0; ifIndex < MAX_APCLI_NUM; ifIndex++)
	{
		pApCliEntry = &pAd->ApCfg.ApCliTab[ifIndex];


		if (pApCliEntry->bPeerExist == TRUE)
			continue;
		else if (Channel == pApCliEntry->wdev.channel &&
			((SsidLen == pApCliEntry->CfgSsidLen && NdisEqualMemory(Ssid, pApCliEntry->CfgSsid, SsidLen)) ||
			SsidLen == 0 /* Hidden */))
		{
			pApCliEntry->bPeerExist = TRUE;
		}
		else
		{
			/* No Root AP match the SSID */
		}
	}
}


VOID ApCliPeerCsaAction(RTMP_ADAPTER *pAd, struct wifi_dev *wdev, BCN_IE_LIST *ie_list)
{
	if (pAd == NULL || ie_list == NULL)
		return;

	if ( (pAd->CommonCfg.bIEEE80211H == 1) &&
		ie_list->NewChannel != 0 &&
		pAd->CommonCfg.Channel != ie_list->NewChannel &&
		pAd->Dot11_H.RDMode != RD_SWITCHING_MODE)
	{
#ifdef DOT11_VHT_AC
		if (IS_CAP_BW160(pAd))
		{
			VHT_OP_INFO *vht_op = &ie_list->vht_op_ie.vht_op_info;
#ifdef DOT11_VHT_R2
			UCHAR bw = 0;

			bw = check_vht_op_bw (vht_op);
			if ((bw == VHT_BW_160) && (pAd->CommonCfg.vht_bw == VHT_BW_160)) {
				pAd->CommonCfg.vht_cent_ch = vht_op->center_freq_2;
			} else if ((bw == VHT_BW_8080) && (pAd->CommonCfg.vht_bw == VHT_BW_8080)) {
				pAd->CommonCfg.vht_cent_ch = vht_op->center_freq_1;
				pAd->CommonCfg.vht_cent_ch2 = vht_op->center_freq_2;
			}
#else
			print_vht_op_info(vht_op);
			if (vht_op->ch_width == 3 && (pAd->CommonCfg.vht_bw == VHT_BW_8080)) {
				pAd->CommonCfg.vht_cent_ch = vht_op->center_freq_1;
	            		pAd->CommonCfg.vht_cent_ch2 = vht_op->center_freq_2;
			}
#endif /* DOT11_VHT_R2 */
		}
#endif /* DOT11_VHT_AC */

		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,
			("[APCLI]  Following root AP to switch channel to ch%u\n",
			ie_list->NewChannel));
		rtmp_set_channel(pAd, wdev, ie_list->NewChannel);
	}
}


extern INT sta_rx_fwd_hnd(RTMP_ADAPTER *pAd, struct wifi_dev *wdev, PNDIS_PACKET pPacket);
extern INT sta_rx_pkt_allow(RTMP_ADAPTER *pAd, RX_BLK *pRxBlk);

static void apcli_sync_wdev(struct _RTMP_ADAPTER *pAd,struct wifi_dev *wdev)
{
	if (pAd->CommonCfg.dbdc_mode == TRUE) {
		int mbss_idx;
		for(mbss_idx=0; mbss_idx < pAd->ApCfg.BssidNum ; mbss_idx++)
		{
			if (pAd->ApCfg.MBSSID[mbss_idx].wdev.PhyMode == wdev->PhyMode) {
				update_att_from_wdev(wdev,&pAd->ApCfg.MBSSID[mbss_idx].wdev);
#ifdef WH_EZ_SETUP // Rakesh: optimization added
				if(IS_ADPTR_EZ_SETUP_ENABLED(pAd))
					break;
#endif
			}
		}
	} else {
		/* align phy mode to BSS0 by default */
		wdev->PhyMode = pAd->ApCfg.MBSSID[BSS0].wdev.PhyMode;
		update_att_from_wdev(wdev,&pAd->ApCfg.MBSSID[BSS0].wdev);
	}
}

extern INT	multi_profile_apcli_devname_req(struct _RTMP_ADAPTER *ad, UCHAR *final_name, INT *ifidx);
VOID APCli_Init(RTMP_ADAPTER *pAd, RTMP_OS_NETDEV_OP_HOOK *pNetDevOps)
{
#define APCLI_MAX_DEV_NUM	32
	PNET_DEV new_dev_p;
	INT idx;
	APCLI_STRUCT *pApCliEntry;
	struct wifi_dev *wdev;
	UINT8 MaxNumApcli;

	
	/* sanity check to avoid redundant virtual interfaces are created */
	if (pAd->flg_apcli_init != FALSE){
		for(idx = 0; idx < pAd->ApCfg.ApCliNum; idx++){
            pApCliEntry = &pAd->ApCfg.ApCliTab[idx];
			wdev = &pApCliEntry->wdev;
			apcli_sync_wdev(pAd,wdev);
		}
		return;
	}

	/* init */
	for(idx = 0; idx < MAX_APCLI_NUM; idx++)
	{   
            pApCliEntry = &pAd->ApCfg.ApCliTab[idx];
            if(pApCliEntry->ApCliInit != FALSE)
                continue;
            pApCliEntry->wdev.if_dev = NULL;
	}

	MaxNumApcli = pAd->ApCfg.ApCliNum;
	if (MaxNumApcli > MAX_APCLI_NUM)
	{
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
				 ("ApCliNum(%d) exceeds MAX_APCLI_NUM(%d)!\n", MaxNumApcli, MAX_APCLI_NUM));
		return;
	}
	/* create virtual network interface */
	for (idx = 0; idx < MaxNumApcli; idx++)
	{
		UINT32 MC_RowID = 0, IoctlIF = 0;
		INT32 Ret = 0;
		char *dev_name;
#ifdef MULTI_PROFILE
		UCHAR final_name[32]="";
#endif

#ifdef MULTIPLE_CARD_SUPPORT
		MC_RowID = pAd->MC_RowID;
#endif /* MULTIPLE_CARD_SUPPORT */
#ifdef HOSTAPD_SUPPORT
		IoctlIF = pAd->IoctlIF;
#endif /* HOSTAPD_SUPPORT */

        pApCliEntry = &pAd->ApCfg.ApCliTab[idx];
        /* sanity check to avoid redundant virtual interfaces are created */
        if(pApCliEntry->ApCliInit != FALSE)
            continue;
		dev_name = get_dev_name_prefix(pAd, INT_APCLI);
#ifdef MULTI_PROFILE
		if (dev_name == NULL)
		{
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("%s(): apcli interface name is null,apcli idx=%d!\n",
					 __FUNCTION__, idx));
			break;
		}
		snprintf(final_name,sizeof(final_name),"%s",dev_name);
		multi_profile_apcli_devname_req(pAd,final_name,&idx);
		if (pAd->CommonCfg.dbdc_mode == TRUE)
		{
			/* MULTI_PROFILE enable, apcli interface name will be apcli0,apclix0*/
			new_dev_p = RtmpOSNetDevCreate(MC_RowID, &IoctlIF, INT_APCLI, 0,
										   sizeof(struct mt_dev_priv), final_name);
		} 
		else
		{
			new_dev_p = RtmpOSNetDevCreate(MC_RowID, &IoctlIF, INT_APCLI, idx,
										   sizeof(struct mt_dev_priv), final_name);
		}
#else
		new_dev_p = RtmpOSNetDevCreate(MC_RowID, &IoctlIF, INT_APCLI, idx,
									sizeof(struct mt_dev_priv), dev_name);
#endif /*MULTI_PROFILE*/
		if (!new_dev_p) {
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("%s(): Create net_device for %s(%d) fail!\n",
						__FUNCTION__, dev_name, idx));
			break;
		}
#ifdef HOSTAPD_SUPPORT
		pAd->IoctlIF = IoctlIF;
#endif /* HOSTAPD_SUPPORT */
#ifdef FAST_EAPOL_WAR
		pApCliEntry->MacTabWCID = 0;
		pApCliEntry->pre_entry_alloc = FALSE;
#endif /* FAST_EAPOL_WAR */
		pApCliEntry->ifIndex = idx;
		pApCliEntry->pAd = pAd;
		ApCliCompleteInit(pApCliEntry);
		wdev = &pApCliEntry->wdev;

		Ret = wdev_init(pAd,
						wdev,
						WDEV_TYPE_APCLI,
						new_dev_p,
						idx,
						(VOID *) pApCliEntry,
						(VOID *) pAd);

		if (Ret == FALSE) {
            MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
						("Assign wdev idx for %s failed, free net device!\n",
                        RTMP_OS_NETDEV_GET_DEVNAME(new_dev_p)));
			RtmpOSNetDevFree(new_dev_p);
            break;
		}

		apcli_sync_wdev(pAd,wdev);
		/*update rate info*/
		SetCommonHtVht(pAd,wdev);
		RTMPUpdateRateInfo(wdev->PhyMode,&wdev->rate);
		RTMP_OS_NETDEV_SET_PRIV(new_dev_p, pAd);
		RTMP_OS_NETDEV_SET_WDEV(new_dev_p, wdev);

#ifdef MT_MAC
		if (pAd->chipCap.hif_type != HIF_MT)
		{
#endif /* MT_MAC */
			if (pAd->chipCap.MBSSIDMode >= MBSSID_MODE1)
			{
				if ((pAd->ApCfg.BssidNum > 0) || (MAX_MESH_NUM > 0))
				{
					UCHAR MacMask = 0;

					if ((pAd->ApCfg.BssidNum + MAX_APCLI_NUM + MAX_MESH_NUM) <= 2)
						MacMask = 0xFE;
					else if ((pAd->ApCfg.BssidNum + MAX_APCLI_NUM + MAX_MESH_NUM) <= 4)
						MacMask = 0xFC;
					else if ((pAd->ApCfg.BssidNum + MAX_APCLI_NUM + MAX_MESH_NUM) <= 8)
						MacMask = 0xF8;

					/*
						Refer to HW definition -
							Bit1 of MAC address Byte0 is local administration bit
							and should be set to 1 in extended multiple BSSIDs'
							Bit3~ of MAC address Byte0 is extended multiple BSSID index.
					*/
					if (pAd->chipCap.MBSSIDMode == MBSSID_MODE1)
					{
						/*
							Refer to HW definition -
								Bit1 of MAC address Byte0 is local administration bit
								and should be set to 1 in extended multiple BSSIDs'
								Bit3~ of MAC address Byte0 is extended multiple BSSID index.
						*/
#ifdef ENHANCE_NEW_MBSSID_MODE
						wdev->if_addr[0] &= (MacMask << 2);
#endif /* ENHANCE_NEW_MBSSID_MODE */
						wdev->if_addr[0] |= 0x2;
						wdev->if_addr[0] += (((pAd->ApCfg.BssidNum + MAX_MESH_NUM) - 1) << 2);
					}
#ifdef ENHANCE_NEW_MBSSID_MODE
					else
					{
						wdev->if_addr[0] |= 0x2;
						wdev->if_addr[pAd->chipCap.MBSSIDMode - 1] &= (MacMask);
						wdev->if_addr[pAd->chipCap.MBSSIDMode - 1] += ((pAd->ApCfg.BssidNum + MAX_MESH_NUM) - 1);
					}
#endif /* ENHANCE_NEW_MBSSID_MODE */
				}
			}
			else
			{
				wdev->if_addr[MAC_ADDR_LEN - 1] = (wdev->if_addr[MAC_ADDR_LEN - 1] + pAd->ApCfg.BssidNum + MAX_MESH_NUM) & 0xFF;
			}
#ifdef MT_MAC
		}
		else {
			UCHAR MacByte = 0;
			UCHAR MacMask = 0xef;
			UINT32 Value = 0;
			RTMP_IO_READ32(pAd, LPON_BTEIR, &Value);
			MacByte = Value >> 29;
			if (MaxNumApcli <= 2) {
				MacMask = 0xef;
			} else if (MaxNumApcli <= 4) {
				MacMask = 0xcf;
			}
			wdev->if_addr[0] |= 0x2; // bit 1 needs to turn on for local mac address definition
			/* apcli can not use the same mac as MBSS,so change if_addr[0] to separate */
			if ((wdev->if_addr[0] & 0x4) == 0x4)
				wdev->if_addr[0] &= ~0x4;
			else
				wdev->if_addr[0] |= 0x4;

			switch (MacByte) {
				case 0x1: /* choose bit[23:20]*/
					wdev->if_addr[2] = wdev->if_addr[2] & MacMask;//clear high 4 bits,
					wdev->if_addr[2] = (wdev->if_addr[2] | (idx << 4));
					break;
				case 0x2: /* choose bit[31:28]*/
					wdev->if_addr[3] = wdev->if_addr[3] & MacMask;//clear high 4 bits,
					wdev->if_addr[3] = (wdev->if_addr[3] | (idx << 4));
					break;
				case 0x3: /* choose bit[39:36]*/
					wdev->if_addr[4] = wdev->if_addr[4] & MacMask;//clear high 4 bits,
					wdev->if_addr[4] = (wdev->if_addr[4] | (idx << 4));
					break;
				case 0x4: /* choose bit [47:44]*/
					wdev->if_addr[5] = wdev->if_addr[5] & MacMask;//clear high 4 bits,
					wdev->if_addr[5] = (wdev->if_addr[5] | (idx << 4));
					break;
				default: /* choose bit[15:12]*/
					wdev->if_addr[1] = wdev->if_addr[1] & MacMask;//clear high 4 bits,
					wdev->if_addr[1] = (wdev->if_addr[1] | (idx << 4));
					break;
			}
		}
#endif /* MT_MAC */

		pNetDevOps->priv_flags = INT_APCLI; /* we are virtual interface */
		pNetDevOps->needProtcted = TRUE;
		pNetDevOps->wdev = wdev;
		NdisMoveMemory(pNetDevOps->devAddr, &wdev->if_addr[0], MAC_ADDR_LEN);

		/* register this device to OS */
		RtmpOSNetDevAttach(pAd->OpMode, new_dev_p, pNetDevOps);
		pApCliEntry->ApCliInit = TRUE;
	}
#ifdef MAC_REPEATER_SUPPORT 	
    CliLinkMapInit(pAd);
#endif

	pAd->flg_apcli_init = TRUE;

}


VOID ApCli_Remove(RTMP_ADAPTER *pAd)
{
	UINT index;
	struct wifi_dev *wdev;

	for(index = 0; index < MAX_APCLI_NUM; index++)
	{
		wdev = &pAd->ApCfg.ApCliTab[index].wdev;
		if (wdev->if_dev)
		{
			RtmpOSNetDevProtect(1);
			RtmpOSNetDevDetach(wdev->if_dev);
			RtmpOSNetDevProtect(0);

			rtmp_wdev_idx_unreg(pAd, wdev);
			RtmpOSNetDevFree(wdev->if_dev);

			/* Clear it as NULL to prevent latter access error. */
			pAd->ApCfg.ApCliTab[index].ApCliInit = FALSE;
			pAd->flg_apcli_init = FALSE;
			wdev->if_dev = NULL;
		}
	}
}


BOOLEAN ApCli_Open(RTMP_ADAPTER *pAd, PNET_DEV dev_p)
{
	UCHAR ifIndex;

    APCLI_STRUCT *pApCliEntry;
    struct wifi_dev *wdev = NULL;

#if defined(CONFIG_WIFI_PKT_FWD) || defined(CONFIG_WIFI_PKT_FWD_MODULE)
	if (wf_fwd_probe_adapter)
		wf_fwd_probe_adapter(pAd);
#endif 

	for (ifIndex = 0; ifIndex < MAX_APCLI_NUM; ifIndex++)
	{
		if (pAd->ApCfg.ApCliTab[ifIndex].wdev.if_dev == dev_p)
		{
			apcli_sync_wdev(pAd,&pAd->ApCfg.ApCliTab[ifIndex].wdev);

			RTMP_OS_NETDEV_START_QUEUE(dev_p);

			pApCliEntry = &pAd->ApCfg.ApCliTab[ifIndex];
			wdev = &pApCliEntry->wdev;
			
			ApCliWaitIfDown(pApCliEntry);
			
			/* Security initial  */
			if (wdev->SecConfig.AKMMap == 0x0)
				SET_AKM_OPEN(wdev->SecConfig.AKMMap);
			
			if (wdev->SecConfig.PairwiseCipher == 0x0)
			{
				SET_CIPHER_NONE(wdev->SecConfig.PairwiseCipher);
				SET_CIPHER_NONE(wdev->SecConfig.GroupCipher);
			}
			WifiSysOpen(pAd,wdev);

			RTMPSetPhyMode( pAd,wdev,wdev->PhyMode);
			RTMPUpdateRateInfo(wdev->PhyMode,&wdev->rate);

#ifdef WSC_INCLUDED
			WscUUIDInit(pAd, ifIndex, TRUE);
#endif /* WSC_INCLUDED */

#ifdef DOT11_N_SUPPORT
			N_ChannelCheck(pAd,wdev->PhyMode,wdev->channel);
#endif /*DOT11_N_SUPPORT*/

#ifdef MWDS
			if (wdev->bDefaultMwdsStatus == TRUE)
				MWDSEnable(pAd,ifIndex,FALSE,FALSE);
#endif

#ifdef WH_EZ_SETUP
			if(IS_EZ_SETUP_ENABLED(wdev)){
				ez_start(wdev, FALSE);
#ifndef EZ_MOD_SUPPORT
				if(pApCliEntry->Enable == TRUE){
					EZ_UPDATE_CAPABILITY_INFO(pAd, EZ_SET_ACTION, HAS_APCLI_INF, ifIndex);
				}
#endif				
			}
#endif /* WH_EZ_SETUP */
			ApCliIfUp(pAd);
#ifdef RADIO_LINK_SELECTION
		if (pAd->ApCfg.RadioLinkSelection)
			Rls_SetInfInfo(pAd, TRUE, wdev);
#endif /* RADIO_LINK_SELECTION */


			return TRUE;
		}
	}

	return FALSE;
}


BOOLEAN ApCli_Close(RTMP_ADAPTER *pAd, PNET_DEV dev_p)
{
	UCHAR ifIndex;
#ifdef MAC_REPEATER_SUPPORT
    REPEATER_CLIENT_ENTRY *pReptEntry;
	UCHAR CliIdx;
    RTMP_CHIP_CAP   *cap = &pAd->chipCap;
#endif /* MAC_REPEATER_SUPPORT */
	struct wifi_dev *wdev;
	APCLI_STRUCT *apcli_entry;

	for (ifIndex = 0; ifIndex < MAX_APCLI_NUM; ifIndex++)
	{
		apcli_entry = &pAd->ApCfg.ApCliTab[ifIndex];
		wdev = &apcli_entry->wdev;
		if (wdev->if_dev == dev_p)
		{
#ifdef MWDS
			MWDSDisable(pAd, ifIndex, FALSE, TRUE);
#endif /* MWDS */



			RTMP_OS_NETDEV_STOP_QUEUE(dev_p);

			/* send disconnect-req to sta State Machine. */
			if (apcli_entry->Enable)
			{
#ifdef MAC_REPEATER_SUPPORT
				if (pAd->ApCfg.bMACRepeaterEn)
				{
					for(CliIdx = 0; CliIdx < GET_MAX_REPEATER_ENTRY_NUM(cap); CliIdx++)
					{
                        pReptEntry = &pAd->ApCfg.pRepeaterCliPool[CliIdx];
						if ((pReptEntry->CliEnable) && (pReptEntry->wdev == &apcli_entry->wdev))
						{
							RTMP_OS_INIT_COMPLETION(&pReptEntry->free_ack);	
							pReptEntry->Disconnect_Sub_Reason = APCLI_DISCONNECT_SUB_REASON_NONE;
							MlmeEnqueue(pAd,
										APCLI_CTRL_STATE_MACHINE,
										APCLI_CTRL_DISCONNECT_REQ,
										0,
										NULL,
										(REPT_MLME_START_IDX + CliIdx));
							RTMP_MLME_HANDLER(pAd);

							ReptWaitLinkDown(pReptEntry);
						}
					}
				}
#endif /* MAC_REPEATER_SUPPORT */
				RTMP_OS_INIT_COMPLETION(&apcli_entry->linkdown_complete);	
				apcli_entry->Disconnect_Sub_Reason = APCLI_DISCONNECT_SUB_REASON_NONE;
				MlmeEnqueue(pAd,
                            APCLI_CTRL_STATE_MACHINE,
                            APCLI_CTRL_DISCONNECT_REQ,
                            0,
                            NULL,
                            ifIndex);
				RTMP_MLME_HANDLER(pAd);

				ApCliWaitLinkDown(apcli_entry);
				ApCliWaitStateDisconnect(apcli_entry);
#ifdef APCLI_AUTO_CONNECT_SUPPORT
				pAd->ApCfg.ApCliAutoConnectRunning[apcli_entry->ifIndex] = FALSE;
#endif
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,
                        ("(%s) ApCli interface[%d] startdown.\n", __FUNCTION__, ifIndex));
			}
#ifdef WH_EZ_SETUP
			if (IS_EZ_SETUP_ENABLED(wdev)) {
#ifndef EZ_MOD_SUPPORT
				EZ_UPDATE_CAPABILITY_INFO(pAd, EZ_CLEAR_ACTION, HAS_APCLI_INF, ifIndex);
#endif
				ez_stop(wdev);
			}
#endif /* WH_EZ_SETUP */
			WifiSysClose(pAd,wdev);

			/*send for ApOpen can know interface down is done*/
			ApCliIfDownComplete(apcli_entry);

#ifdef RADIO_LINK_SELECTION
			if (pAd->ApCfg.RadioLinkSelection)
				Rls_SetInfInfo(pAd, FALSE, wdev);
#endif /* RADIO_LINK_SELECTION */
			return TRUE;
		}

	}
	return FALSE;
}

#ifdef APCLI_AUTO_CONNECT_SUPPORT
#ifdef APCLI_AUTO_BW_TMP /* should be removed after apcli auto-bw is applied */
BOOLEAN ApCliAutoConnectBWAdjust(
	IN RTMP_ADAPTER	*pAd,
	IN struct wifi_dev	*wdev,
	IN BSS_ENTRY		*bss_entry)
{
    BOOLEAN bAdjust = FALSE;
	BOOLEAN bAdjust_by_channel = FALSE;
	BOOLEAN bAdjust_by_ht = FALSE;
	BOOLEAN bAdjust_by_vht = FALSE;
    UCHAR 	orig_op_ht_bw;
	UCHAR 	orig_op_vht_bw;
	UCHAR	orig_ext_cha;

	if (pAd == NULL || wdev == NULL || bss_entry == NULL) {
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR,
            	          ("(%s)  Error! entry is NULL.\n", __FUNCTION__));
		return FALSE;
	}

	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,
        	          ("BW info of root AP (%s):\n",bss_entry->Ssid));
	orig_op_ht_bw = wlan_operate_get_ht_bw(wdev);
	orig_op_vht_bw = wlan_operate_get_vht_bw(wdev);
	orig_ext_cha = wlan_operate_get_ext_cha(wdev);

    if((wdev->channel != bss_entry->Channel)) {
		bAdjust = TRUE;
		bAdjust_by_channel = TRUE;
	}

#ifdef DOT11_N_SUPPORT
    if (WMODE_CAP_N(wdev->PhyMode) && (bss_entry->AddHtInfoLen != 0))
    {
        ADD_HTINFO *add_ht_info = &bss_entry->AddHtInfo.AddHtInfo;
		UCHAR op_ht_bw = wlan_operate_get_ht_bw(wdev);
		UCHAR cfg_ht_bw = wlan_config_get_ht_bw(wdev);
		UCHAR ext_cha = wlan_operate_get_ext_cha(wdev);
        
        if(!bAdjust && 
           ((ext_cha != add_ht_info->ExtChanOffset) ||
			(op_ht_bw != add_ht_info->RecomWidth))) {
			bAdjust = TRUE;
		}

        if (bAdjust)
        {
			switch (add_ht_info->RecomWidth) //peer side vht bw
			{
				case BW_20:
					if (op_ht_bw == BW_40) {
						{//set to config bw to let ap use new configuration
							UCHAR mbss_idx = 0;
							for(mbss_idx = 0; mbss_idx < pAd->ApCfg.BssidNum; mbss_idx++)
							{
								struct wifi_dev *mbss_wdev;
								mbss_wdev = &pAd->ApCfg.MBSSID[mbss_idx].wdev;
								if (HcGetBandByWdev(mbss_wdev) == HcGetBandByWdev(wdev))
								{
									wlan_config_set_ht_bw(mbss_wdev,add_ht_info->RecomWidth);
								}
							}
							wlan_operate_set_ht_bw(wdev,add_ht_info->RecomWidth);
						}
						bAdjust_by_ht = TRUE;
					}
					break;
				case BW_40:
					if (op_ht_bw == BW_20) {
						if(cfg_ht_bw == BW_40) {
{//set to config extension channel/bw to let ap use new configuration
								UCHAR mbss_idx = 0;
								for(mbss_idx = 0; mbss_idx < pAd->ApCfg.BssidNum; mbss_idx++)
								{
									struct wifi_dev *mbss_wdev;
									mbss_wdev = &pAd->ApCfg.MBSSID[mbss_idx].wdev;
									if (HcGetBandByWdev(mbss_wdev) == HcGetBandByWdev(wdev))
									{
										wlan_config_set_ht_bw(mbss_wdev,add_ht_info->RecomWidth);
										wlan_config_set_ext_cha(mbss_wdev, add_ht_info->ExtChanOffset);
									}
								}
								wlan_operate_set_ht_bw(wdev,add_ht_info->RecomWidth);
								wlan_config_set_ext_cha(wdev, add_ht_info->ExtChanOffset);
							}
							bAdjust_by_ht = TRUE;
						}
					} else {
						if(cfg_ht_bw == BW_40) {
							{//set to config extension channel to let ap use new configuration
								UCHAR mbss_idx = 0;
								for(mbss_idx = 0; mbss_idx < pAd->ApCfg.BssidNum; mbss_idx++)
								{
									struct wifi_dev *mbss_wdev;
									mbss_wdev = &pAd->ApCfg.MBSSID[mbss_idx].wdev;
									if (HcGetBandByWdev(mbss_wdev) == HcGetBandByWdev(wdev))
									{
										wlan_config_set_ext_cha(mbss_wdev, add_ht_info->ExtChanOffset);
									}
								}
								wlan_config_set_ext_cha(wdev, add_ht_info->ExtChanOffset);
							}
							bAdjust_by_ht = TRUE;
						}
					}
					break;
			}
        }
    }
#endif /* DOT11_N_SUPPORT */


#ifdef DOT11_VHT_AC
    if (WMODE_CAP_AC(wdev->PhyMode) && IS_CAP_BW160(pAd) && (bss_entry->vht_cap_len != 0) && (bss_entry->vht_op_len !=0))
    {
        BOOLEAN bResetVHTBw = FALSE, bDownBW = FALSE;
        UCHAR bw = VHT_BW_2040;
        VHT_OP_INFO *vht_op = &bss_entry->vht_op_ie.vht_op_info;
		UCHAR op_vht_bw = wlan_operate_get_vht_bw(wdev);
		UCHAR cfg_vht_bw = wlan_config_get_vht_bw(wdev);

#ifdef DOT11_VHT_R2
        bw = check_vht_op_bw (vht_op);
#else
        bw = vht_op->ch_width;
        print_vht_op_info(vht_op);
#endif /* DOT11_VHT_R2 */

        if(!bAdjust && 
           (bw != op_vht_bw))
            bAdjust = TRUE;

        if (bAdjust)
        {
			switch(bw) {//peer side vht bw
				case VHT_BW_2040:
					if (cfg_vht_bw > VHT_BW_2040) {
						bResetVHTBw = TRUE;
						bDownBW = TRUE;
						bAdjust_by_vht = TRUE;
					}
					break;
				case VHT_BW_80:
					if (cfg_vht_bw > VHT_BW_80) {
						bResetVHTBw = TRUE;
						bDownBW = TRUE;
						bAdjust_by_vht = TRUE;
					}
					break;
				case VHT_BW_160:
					if (cfg_vht_bw == VHT_BW_160) {
						pAd->CommonCfg.vht_cent_ch = vht_op->center_freq_2;
						bAdjust_by_vht = TRUE;
						bResetVHTBw = 1;
					}
					break;
				case VHT_BW_8080:
					if (cfg_vht_bw == VHT_BW_8080) {
						//pAd->CommonCfg.vht_cent_ch = vht_op->center_freq_1;
						pAd->CommonCfg.vht_cent_ch2 = vht_op->center_freq_2;
						bResetVHTBw = 1;
						bAdjust_by_vht = TRUE;
					}
					break;
			}
        }

        if(bResetVHTBw)
        {
            INT Idx = -1;
            BOOLEAN bMatch = FALSE;
            for (Idx = 0; Idx < pAd->ChannelListNum; Idx++)
            {
            	if (bss_entry->Channel == pAd->ChannelList[Idx].Channel)
                {
                    bMatch = TRUE;
            		break;
                }
            }

            if(bMatch && (Idx < MAX_NUM_OF_CHANNELS))
            {
                if(bDownBW){
                    MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_OFF,
                      ("(%s): Follow BW info of root AP (%s) from vht_bw = %d to %d. (MAX=%d)\n",
                        __FUNCTION__, bss_entry->Ssid,
						op_vht_bw, bw,
						cfg_vht_bw));
					wlan_operate_set_vht_bw(wdev,bw);
                } else if(!bDownBW && (pAd->ChannelList[Idx].Flags & CHANNEL_80M_CAP)){
					wlan_operate_set_vht_bw(wdev,pAd->CommonCfg.cfg_vht_bw);
                }
                pAd->CommonCfg.vht_cent_ch2 = vht_op->center_freq_2;
            }
        }
    }
#endif /* DOT11_VHT_AC */
	bAdjust = FALSE;
	if (bAdjust_by_channel == TRUE)
		bAdjust = TRUE;
	if (bAdjust_by_ht == TRUE)
		bAdjust = TRUE;
	if (bAdjust_by_vht == TRUE)
		bAdjust = TRUE;
	if (bAdjust) {
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,("%s:Adjust (%d %d %d)\n\r",__func__,
			bAdjust_by_channel,bAdjust_by_ht,bAdjust_by_vht));
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,("%s:HT BW:%d to %d. MAX(%d)\n\r",__func__,
			orig_op_ht_bw,wlan_operate_get_ht_bw(wdev),wlan_config_get_ht_bw(wdev)));
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,("%s:VHT BW:%d to %d. MAX(%d)\n\r",__func__,
				 orig_op_vht_bw,wlan_operate_get_vht_bw(wdev),wlan_config_get_vht_bw(wdev)));
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,("%s:EXT CH:%d to %d\n\r",__func__,
				 orig_ext_cha,wlan_operate_get_ext_cha(wdev)));
	}

	return bAdjust;
}
#endif /* APCLI_AUTO_BW_TMP */

/*
	===================================================

	Description:
		Find the AP that is configured in the ApcliTab, and switch to
		the channel of that AP

	Arguments:
		pAd: pointer to our adapter

	Return Value:
		TRUE: no error occured
		FALSE: otherwise

	Note:
	===================================================
*/
BOOLEAN ApCliAutoConnectExec(
	IN  PRTMP_ADAPTER   pAd,
	IN struct wifi_dev *wdev)
{
	UCHAR			ifIdx, CfgSsidLen, entryIdx;
	RTMP_STRING *pCfgSsid;
	BSS_TABLE		*pScanTab, *pSsidBssTab;
	POS_COOKIE pObj = (POS_COOKIE) pAd->OS_Cookie;
	INT old_ioctl_if;
	INT old_ioctl_if_type;

	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("---> ApCliAutoConnectExec()\n"));

	if (wdev)
		ifIdx = wdev->func_idx;
	else
		return FALSE;

	if(ifIdx >= MAX_APCLI_NUM)
	{
		MTWF_LOG(DBG_CAT_ALL, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("Error  ifIdx=%d \n", ifIdx));
		return FALSE;
	}
	if (pAd->ApCfg.ApCliTab[ifIdx].AutoConnectFlag != TRUE)
		return FALSE;

	CfgSsidLen = pAd->ApCfg.ApCliTab[ifIdx].CfgSsidLen;
	pCfgSsid = pAd->ApCfg.ApCliTab[ifIdx].CfgSsid;
	pScanTab = &pAd->ScanTab;
	pSsidBssTab = &pAd->ApCfg.ApCliTab[ifIdx].MlmeAux.SsidBssTab;
	pSsidBssTab->BssNr = 0;

	/*
		Find out APs with the desired SSID.
	*/
	for (entryIdx=0; entryIdx<pScanTab->BssNr;entryIdx++)
	{
		BSS_ENTRY *pBssEntry = &pScanTab->BssEntry[entryIdx];

		if ( pBssEntry->Channel == 0)
			break;

		if (NdisEqualMemory(pCfgSsid, pBssEntry->Ssid, CfgSsidLen) &&
							pBssEntry->SsidLen &&
							(pBssEntry->SsidLen == CfgSsidLen) &&
#if defined(DBDC_MODE) && defined(DOT11K_RRM_SUPPORT)
/*
	double check the SSID in the same band could be in candidate list
*/
							(((pBssEntry->Channel > 14) && WMODE_CAP_5G(wdev->PhyMode)) || ((pBssEntry->Channel <= 14) && WMODE_CAP_2G(wdev->PhyMode))) &&
#endif /* defined(DBDC_MODE) && defined(DOT11K_RRM_SUPPORT) */
							(pSsidBssTab->BssNr < MAX_LEN_OF_BSS_TABLE)
							)
		{
			if ((((wdev->SecConfig.AKMMap & pBssEntry->AKMMap) != 0) ||
			    (IS_AKM_AUTOSWITCH(wdev->SecConfig.AKMMap) && IS_AKM_SHARED(pBssEntry->AKMMap))) && 
                            (wdev->SecConfig.PairwiseCipher & pBssEntry->PairwiseCipher) != 0)
			{
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,
						("Found desired ssid in Entry %2d:\n", entryIdx));
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,
						("I/F(apcli%d) ApCliAutoConnectExec:(Len=%d,Ssid=%s, Channel=%d, Rssi=%d)\n",
						ifIdx, pBssEntry->SsidLen, pBssEntry->Ssid,
						pBssEntry->Channel, pBssEntry->Rssi));
				MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE,
						("I/F(apcli%d) ApCliAutoConnectExec::(AuthMode=%s, EncrypType=%s)\n", ifIdx,
						GetAuthMode(pBssEntry->AuthMode),
						GetEncryptType(pBssEntry->WepStatus)) );
				NdisMoveMemory(&pSsidBssTab->BssEntry[pSsidBssTab->BssNr++],
								pBssEntry, sizeof(BSS_ENTRY));
			}
		}
	}

	NdisZeroMemory(&pSsidBssTab->BssEntry[pSsidBssTab->BssNr], sizeof(BSS_ENTRY));

	/*
		Sort by Rssi in the increasing order, and connect to
		the last entry (strongest Rssi)
	*/
	BssTableSortByRssi(pSsidBssTab, TRUE);


	if ((pSsidBssTab->BssNr == 0))
	{
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("No match entry.\n"));
		pAd->ApCfg.ApCliAutoConnectRunning[ifIdx] = FALSE;
	}
	else if (pSsidBssTab->BssNr > 0 &&
			pSsidBssTab->BssNr <=MAX_LEN_OF_BSS_TABLE)
	{
		/*
			Switch to the channel of the candidate AP
		*/
		BSS_ENTRY *pBssEntry = &pSsidBssTab->BssEntry[pSsidBssTab->BssNr -1];

#ifdef APCLI_AUTO_BW_TMP /* should be removed after apcli auto-bw is applied */
		if (ApCliAutoConnectBWAdjust(pAd, wdev, pBssEntry)
			|| (!IS_INVALID_HT_SECURITY(pBssEntry->PairwiseCipher)))		//In TKIP security, ht_phy info is null, so in succussive case, we need to refill ht_phy_info
#endif /* APCLI_AUTO_BW_TMP */
		{
			pAd->ApCfg.ApCliAutoBWAdjustCnt[ifIdx]++;
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("Switch to channel :%d\n", pBssEntry->Channel));
			rtmp_set_channel(pAd, wdev, pBssEntry->Channel);
		}
	}
	else
	{
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_ERROR, ("Error! Out of table range: (BssNr=%d).\n", pSsidBssTab->BssNr) );
		RtmpOSNetDevProtect(1);
		old_ioctl_if = pObj->ioctl_if;
		old_ioctl_if_type = pObj->ioctl_if_type;
		pObj->ioctl_if = ifIdx;
		pObj->ioctl_if_type = INT_APCLI;
		Set_ApCli_Enable_Proc(pAd, "1");
		pObj->ioctl_if = old_ioctl_if;
		pObj->ioctl_if_type = old_ioctl_if_type;
		RtmpOSNetDevProtect(0);
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("<--- ApCliAutoConnectExec()\n"));
		return FALSE;
	}
	RtmpOSNetDevProtect(1);
	old_ioctl_if = pObj->ioctl_if;
	old_ioctl_if_type = pObj->ioctl_if_type;
	pObj->ioctl_if = ifIdx;
	pObj->ioctl_if_type = INT_APCLI;
	Set_ApCli_Enable_Proc(pAd, "1");
	pObj->ioctl_if = old_ioctl_if;
	pObj->ioctl_if_type = old_ioctl_if_type;
	RtmpOSNetDevProtect(0);
	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("<--- ApCliAutoConnectExec()\n"));
	return TRUE;

}

/*
	===================================================

	Description:
		If the previous selected entry connected failed, this function will
		choose next entry to connect. The previous entry will be deleted.

	Arguments:
		pAd: pointer to our adapter

	Note:
		Note that the table is sorted by Rssi in the "increasing" order, thus
		the last entry in table has stringest Rssi.
	===================================================
*/

VOID ApCliSwitchCandidateAP(
	IN PRTMP_ADAPTER pAd,
	IN struct wifi_dev *wdev)
{
	BSS_TABLE 		*pSsidBssTab;
	PAPCLI_STRUCT	pApCliEntry;
	UCHAR			lastEntryIdx, ifIdx;
	POS_COOKIE pObj = (POS_COOKIE) pAd->OS_Cookie;
	INT old_ioctl_if;
	INT old_ioctl_if_type;

	if (pAd->ScanCtrl.PartialScan.bScanning == TRUE)
		return;

	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("---> ApCliSwitchCandidateAP()\n"));

	if (wdev)
		ifIdx = wdev->func_idx;
	else
		return;
	if(ifIdx >= MAX_APCLI_NUM)
	{
		MTWF_LOG(DBG_CAT_ALL, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("Error  ifIdx=%d \n", ifIdx));
		return ;
	}
	if (pAd->ApCfg.ApCliTab[ifIdx].AutoConnectFlag != TRUE)
		return;
	
	pApCliEntry = &pAd->ApCfg.ApCliTab[ifIdx];
	pSsidBssTab = &pApCliEntry->MlmeAux.SsidBssTab;
	if (pSsidBssTab->BssNr == 0) {
		MTWF_LOG(DBG_CAT_ALL, DBG_SUBCAT_ALL, DBG_LVL_TRACE, ("No Bss\n"));
		pAd->ApCfg.ApCliAutoConnectRunning[ifIdx] = FALSE;
		return;
	}
	/*
		delete (zero) the previous connected-failled entry and always
		connect to the last entry in talbe until the talbe is empty.
	*/
	NdisZeroMemory(&pSsidBssTab->BssEntry[--pSsidBssTab->BssNr], sizeof(BSS_ENTRY));
	lastEntryIdx = pSsidBssTab->BssNr -1;

	if ((pSsidBssTab->BssNr > 0) && (pSsidBssTab->BssNr < MAX_LEN_OF_BSS_TABLE))
	{
		BSS_ENTRY *pBssEntry = &pSsidBssTab->BssEntry[pSsidBssTab->BssNr -1];

#ifdef APCLI_AUTO_BW_TMP /* should be removed after apcli auto-bw is applied */
		if (ApCliAutoConnectBWAdjust(pAd, wdev, pBssEntry))
#endif /* APCLI_AUTO_BW_TMP */
		{
			MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("Switch to channel :%d\n", pBssEntry->Channel));
			rtmp_set_channel(pAd, wdev, pBssEntry->Channel);
		}
	}
	else
	{
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("No candidate AP, the process is about to stop.\n"));
		pAd->ApCfg.ApCliAutoConnectRunning[ifIdx] = FALSE;
	}
	RtmpOSNetDevProtect(1);
	old_ioctl_if = pObj->ioctl_if;
	old_ioctl_if_type = pObj->ioctl_if_type;
	pObj->ioctl_if = ifIdx;
	pObj->ioctl_if_type = INT_APCLI;
	Set_ApCli_Enable_Proc(pAd, "1");
	pObj->ioctl_if = old_ioctl_if;
	pObj->ioctl_if_type = old_ioctl_if_type;
	RtmpOSNetDevProtect(0);
	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("---> ApCliSwitchCandidateAP()\n"));

}
#endif /* APCLI_AUTO_CONNECT_SUPPORT */

VOID ApCliRTMPReportMicError(
	IN RTMP_ADAPTER *pAd, 
	IN UCHAR uniCastKey,
	IN INT ifIndex)
{
	ULONG	Now;
	PAPCLI_STRUCT pApCliEntry = NULL;

	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, (" ApCliRTMPReportMicError <---\n"));

	pApCliEntry = &pAd->ApCfg.ApCliTab[ifIndex];
	/* Record Last MIC error time and count */
	NdisGetSystemUpTime(&Now);
	if (pAd->ApCfg.ApCliTab[ifIndex].MicErrCnt == 0)
	{
		pAd->ApCfg.ApCliTab[ifIndex].MicErrCnt++;
		pAd->ApCfg.ApCliTab[ifIndex].LastMicErrorTime = Now;
		NdisZeroMemory(pAd->ApCfg.ApCliTab[ifIndex].ReplayCounter, 8);        
	}
	else if (pAd->ApCfg.ApCliTab[ifIndex].MicErrCnt == 1)
	{
		if ((pAd->ApCfg.ApCliTab[ifIndex].LastMicErrorTime + (60 * OS_HZ)) < Now)
		{
			/* Update Last MIC error time, this did not violate two MIC errors within 60 seconds */
			pAd->ApCfg.ApCliTab[ifIndex].LastMicErrorTime = Now; 		
		}
		else
		{

			/* RTMPSendWirelessEvent(pAd, IW_COUNTER_MEASURES_EVENT_FLAG, pAd->MacTab.Content[BSSID_WCID].Addr, BSS0, 0); */

			pAd->ApCfg.ApCliTab[ifIndex].LastMicErrorTime = Now; 		
			/* Violate MIC error counts, MIC countermeasures kicks in */
			pAd->ApCfg.ApCliTab[ifIndex].MicErrCnt++;
		}
	}
	else
	{
		/* MIC error count >= 2 */
		/* This should not happen */
		;
	}
	MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_MIC_FAILURE_REPORT_FRAME, 1, &uniCastKey, ifIndex);

	if (pAd->ApCfg.ApCliTab[ifIndex].MicErrCnt == 2)
	{
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, (" MIC Error count = 2 Trigger Block timer....\n"));
		MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, (" pAd->ApCfg.ApCliTab[%d].LastMicErrorTime = %ld\n",ifIndex,
			pAd->ApCfg.ApCliTab[ifIndex].LastMicErrorTime));

#ifdef APCLI_CERT_SUPPORT
		if (pAd->bApCliCertTest == TRUE)
			RTMPSetTimer(&pApCliEntry->MlmeAux.WpaDisassocAndBlockAssocTimer, 100);
#endif /* APCLI_CERT_SUPPORT */
	}

	MTWF_LOG(DBG_CAT_CLIENT, CATCLIENT_APCLI, DBG_LVL_TRACE, ("ApCliRTMPReportMicError --->\n"));

}

#ifdef DOT11W_PMF_SUPPORT
/* chane the cmd depend on security mode first, and update to run time flag*/
INT Set_ApCliPMFMFPC_Proc (
	IN PRTMP_ADAPTER pAd, 
	IN RTMP_STRING *arg)
{
	POS_COOKIE pObj;
	PMF_CFG *pPmfCfg = NULL;
	struct wifi_dev *wdev = NULL;

	if(strlen(arg) == 0)
		return FALSE;

	pObj = (POS_COOKIE) pAd->OS_Cookie;

	pPmfCfg = &pAd->ApCfg.ApCliTab[pObj->ioctl_if].wdev.SecConfig.PmfCfg;
	wdev = &pAd->ApCfg.ApCliTab[pObj->ioctl_if].wdev;

	if (!pPmfCfg || !wdev)
	{
		MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: pPmfCfg=%p, wdev=%p\n",
					__FUNCTION__ , pPmfCfg, wdev));
		return FALSE;
	}
	
	if (simple_strtol(arg, 0, 10))			
		pPmfCfg->Desired_MFPC = TRUE; 
	else
	{
		pPmfCfg->Desired_MFPC = FALSE; 
		pPmfCfg->MFPC = FALSE;
		pPmfCfg->MFPR = FALSE;
	}

	MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: Desired MFPC=%d\n",
				__FUNCTION__ , pPmfCfg->Desired_MFPC));

	if ((IS_AKM_WPA2_Entry(wdev) ||IS_AKM_WPA2PSK_Entry(wdev))
		&& IS_CIPHER_AES_Entry(wdev))
	{
		pPmfCfg->PMFSHA256 = pPmfCfg->Desired_PMFSHA256;
		if (pPmfCfg->Desired_MFPC)
		{
			pPmfCfg->MFPC = TRUE;
			pPmfCfg->MFPR = pPmfCfg->Desired_MFPR;

			if (pPmfCfg->MFPR)
				pPmfCfg->PMFSHA256 = TRUE;
		}
	}
	else if (pPmfCfg->Desired_MFPC)
	{
		MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: Security is not WPA2/WPA2PSK AES\n", __FUNCTION__));
	}

	MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: MFPC=%d, MFPR=%d, SHA256=%d\n",
						__FUNCTION__,
						pPmfCfg->MFPC,
						pPmfCfg->MFPR,
						pPmfCfg->PMFSHA256));

    	return TRUE;
}

/* chane the cmd depend on security mode first, and update to run time flag*/
INT Set_ApCliPMFMFPR_Proc (
	IN PRTMP_ADAPTER pAd, 
	IN RTMP_STRING *arg)
{
	POS_COOKIE pObj;
	PMF_CFG *pPmfCfg = NULL;
	struct wifi_dev *wdev = NULL;


 	if(strlen(arg) == 0)
		return FALSE;

	pObj = (POS_COOKIE) pAd->OS_Cookie;

	pPmfCfg = &pAd->ApCfg.ApCliTab[pObj->ioctl_if].wdev.SecConfig.PmfCfg;
	wdev = &pAd->ApCfg.ApCliTab[pObj->ioctl_if].wdev;

	if (!pPmfCfg || !wdev)
	{
		MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: pPmfCfg=%p, wdev=%p\n",
					__FUNCTION__ , pPmfCfg, wdev));
		return FALSE;
	}
	
	if (simple_strtol(arg, 0, 10))			
		pPmfCfg->Desired_MFPR = TRUE; 
	else
	{
		pPmfCfg->Desired_MFPR = FALSE;
		/* only close the MFPR */
		pPmfCfg->MFPR = FALSE;
	}

	MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: Desired MFPR=%d\n",
					__FUNCTION__, pPmfCfg->Desired_MFPR));

	if ((IS_AKM_WPA2_Entry(wdev) ||IS_AKM_WPA2PSK_Entry(wdev))
		&& IS_CIPHER_AES_Entry(wdev))
	{
		pPmfCfg->PMFSHA256 = pPmfCfg->Desired_PMFSHA256;
		if (pPmfCfg->Desired_MFPC)
		{
			pPmfCfg->MFPC = TRUE;
			pPmfCfg->MFPR = pPmfCfg->Desired_MFPR;

			if (pPmfCfg->MFPR)
				pPmfCfg->PMFSHA256 = TRUE;
		}
	}
	else if (pPmfCfg->Desired_MFPC)
	{
		MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: Security is not WPA2/WPA2PSK AES\n", __FUNCTION__));
	}

	MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: MFPC=%d, MFPR=%d, SHA256=%d\n",
						__FUNCTION__, pPmfCfg->MFPC,
						pPmfCfg->MFPR,
						pPmfCfg->PMFSHA256));
		
	return TRUE;
}

INT Set_ApCliPMFSHA256_Proc (
	IN PRTMP_ADAPTER pAd, 
	IN RTMP_STRING *arg)
{
	POS_COOKIE pObj;
	PMF_CFG *pPmfCfg = NULL;

 	if(strlen(arg) == 0)
		return FALSE;

	pObj = (POS_COOKIE) pAd->OS_Cookie;

	pPmfCfg = &pAd->ApCfg.ApCliTab[pObj->ioctl_if].wdev.SecConfig.PmfCfg;

	if (!pPmfCfg)
	{
		MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: pPmfCfg=%p\n",
					__FUNCTION__ , pPmfCfg));
		return FALSE;
	}
	
	if (simple_strtol(arg, 0, 10))			
		pPmfCfg->Desired_PMFSHA256 = TRUE; 
	else
		pPmfCfg->Desired_PMFSHA256 = FALSE; 

	MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR, ("[PMF]%s:: Desired PMFSHA256=%d\n",
					__FUNCTION__, pPmfCfg->Desired_PMFSHA256));

	return TRUE;
}
#endif /* DOT11W_PMF_SUPPORT */

VOID apcli_dync_txop_alg(
	PRTMP_ADAPTER pAd,
	struct wifi_dev *wdev,
	UINT tx_tp,
	UINT rx_tp)
{
#define COND3_COOL_DOWN_TIME 240
	
	if (!pAd || !wdev) {
		MTWF_LOG(DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR,
					("%s:: pAd or wdev is NULL!\n",
					__FUNCTION__));
		return;
	} else {
		INT i;
		BOOLEAN cond[NUM_OF_APCLI_TXOP_COND] = {FALSE};
		UINT16 cond_txop_level[NUM_OF_APCLI_TXOP_COND] = {0};
		UINT16 cond_thrd[NUM_OF_APCLI_TXOP_COND] = {0};
		UINT16 txop_level = TXOP_0;
		UINT16 current_txop_level = TXOP_0;
		BOOLEAN apcli_txop_en = FALSE;
		APCLI_STRUCT *apcli_entry = NULL;

		cond_txop_level[1] = TXOP_30;
		cond_txop_level[2] = TXOP_FE;
		cond_txop_level[3] = TXOP_80;
		cond_thrd[1] = TP_PEEK_BOUND_THRESHOLD;
		cond_thrd[2] = TX_MODE_TP_CHECK;
		current_txop_level = wdev->bss_info_argument.txop_level[PRIO_APCLI_REPEATER];
		apcli_entry = &pAd->ApCfg.ApCliTab[wdev->func_idx];

		/* if cond_1 is taking effect, adjust the threshold to prevent instability */
		if (current_txop_level == cond_txop_level[1]) {
			/* Adjust cond_thrd[1] */
			if (apcli_entry->dync_txop_histogram[1] >= 4) {
				UINT32 tolerance_adjust_factor = 10;
				UINT32 tolerance_adjust_value = 0;

				tolerance_adjust_value = TOLERANCE_OF_TP_THRESHOLD + \
										(apcli_entry->dync_txop_histogram[1]*tolerance_adjust_factor);
				if (tolerance_adjust_value > 150)
					tolerance_adjust_value = 150;
				cond_thrd[1] = TP_PEEK_BOUND_THRESHOLD - tolerance_adjust_value;
			} else {
				cond_thrd[1] = TP_PEEK_BOUND_THRESHOLD - TOLERANCE_OF_TP_THRESHOLD;
			}

			/* Check if t.p. has degrade right after apply cond1 */
			if (tx_tp <= (TP_PEEK_BOUND_THRESHOLD - TOLERANCE_OF_TP_THRESHOLD) && 
				apcli_entry->dync_txop_histogram[1] < 4) {
				/* If t.p. is bad right after cond1, we trigger cond3 to recover old txop */
				cond[3] = TRUE;
			}
		} else if (current_txop_level == cond_txop_level[2]) {
			/* if cond_2 is taking effect, adjust the threshold to prevent instability */
			cond_thrd[2] = TX_MODE_TP_CHECK - TOLERANCE_OF_TP_THRESHOLD;
		}

		if (tx_tp > cond_thrd[1]) {
			cond[1] = TRUE;
		} else if (tx_tp > cond_thrd[2]  && WMODE_CAP_2G(wdev->PhyMode)) {
			/* We don't check "divided by 0" because the "if condition" already do that */
			UINT tx_ratio = (tx_tp*100)/(tx_tp + rx_tp);
			if (tx_ratio > TX_MODE_RATIO_THRESHOLD)
				cond[2] = TRUE;
		}

		if (apcli_entry->dync_txop_histogram[3] != 0) {
            cond[3] = TRUE;
			txop_level = cond_txop_level[3];
			apcli_txop_en = TRUE;
			if (tx_tp < TP_PEEK_BOUND_THRESHOLD) {
				/* If cond3 triggered but t.p cannot keep high, we raise the decade rate */
				UINT8 cond3_decade_factor = 0;
				UINT32 cond3_accumulate_value = 0;
				cond[4] = TRUE;
				cond3_decade_factor = (1 << apcli_entry->dync_txop_histogram[4]); /* exponential decade */
				cond3_accumulate_value = apcli_entry->dync_txop_histogram[3] + cond3_decade_factor;
				apcli_entry->dync_txop_histogram[3] = \
					(cond3_accumulate_value) > COND3_COOL_DOWN_TIME ?\
					COND3_COOL_DOWN_TIME:cond3_accumulate_value;
			}
		} else if (cond[1] == TRUE) {
			txop_level = cond_txop_level[1];
			apcli_txop_en = TRUE;
		} else if (cond[2] == TRUE) {
			txop_level = cond_txop_level[2];
			apcli_txop_en = TRUE;
		} else {
			txop_level = TXOP_0;
			apcli_txop_en = FALSE;
		}


		if (txop_level != current_txop_level) {
			if (apcli_txop_en == TRUE)
				enable_tx_burst(pAd, wdev, AC_BE, PRIO_APCLI_REPEATER, txop_level);
			else
				disable_tx_burst(pAd, wdev, AC_BE, PRIO_APCLI_REPEATER, txop_level);
		}

		/* update histogram */
		for (i = 0; i < NUM_OF_APCLI_TXOP_COND; i++) {
			if (cond[i] == TRUE)
				apcli_entry->dync_txop_histogram[i]++;
			else
				apcli_entry->dync_txop_histogram[i] = 0;
		}
		
		/* clear histogram */
		if (apcli_entry->dync_txop_histogram[3] > COND3_COOL_DOWN_TIME) {
			apcli_entry->dync_txop_histogram[3] = 0;
		}
	}
}

INT apcli_phy_rrm_init_byRf(RTMP_ADAPTER *pAd, UCHAR RfIC)
{
	UCHAR PhyMode = HcGetPhyModeByRf(pAd,RfIC);
	UCHAR Channel = HcGetChannelByRf(pAd,RfIC);
	UCHAR BandIdx = HcGetBandByChannel(pAd,Channel);

    if (Channel== 0 )
	{
        MTWF_LOG(DBG_CAT_AP, DBG_SUBCAT_ALL, DBG_LVL_OFF,
				 ("%sBandIdx %d Channel setting is 0\n", __FUNCTION__,BandIdx));

        return FALSE;
	}

	pAd->CommonCfg.CentralChannel = Channel;

	AsicSetTxStream(pAd, pAd->Antenna.field.TxPath, OPMODE_AP, TRUE,BandIdx);
	AsicSetRxStream(pAd, pAd->Antenna.field.RxPath, BandIdx);

	// TODO: shiang-usw, get from MT7620_MT7610 Single driver, check this!!
	N_ChannelCheck(pAd,PhyMode,Channel);//correct central channel offset
	MTWF_LOG(DBG_CAT_AP, DBG_SUBCAT_ALL, DBG_LVL_TRACE,("pAd->CommonCfg.CentralChannel=%d\n",pAd->CommonCfg.CentralChannel));
	AsicBBPAdjust(pAd,Channel);
	MTWF_LOG(DBG_CAT_AP, DBG_SUBCAT_ALL, DBG_LVL_TRACE,("pAd->CommonCfg.CentralChannel=%d\n",pAd->CommonCfg.CentralChannel));


#ifdef DOT11_VHT_AC

	if ((pAd->CommonCfg.BBPCurrentBW == BW_80) || (pAd->CommonCfg.BBPCurrentBW == BW_160) || (pAd->CommonCfg.BBPCurrentBW == BW_8080))
		pAd->hw_cfg.cent_ch = pAd->CommonCfg.vht_cent_ch;
	else
#endif /* DOT11_VHT_AC */
		pAd->hw_cfg.cent_ch = pAd->CommonCfg.CentralChannel;


	MTWF_LOG(DBG_CAT_AP, DBG_SUBCAT_ALL, DBG_LVL_TRACE,("pAd->CommonCfg.CentralChannel=%d\n",pAd->CommonCfg.CentralChannel));
	MTWF_LOG(DBG_CAT_AP, DBG_SUBCAT_ALL, DBG_LVL_TRACE,("pAd->hw_cfg.cent_ch=%d,BW=%d\n",pAd->hw_cfg.cent_ch,pAd->CommonCfg.BBPCurrentBW));

	AsicSwitchChannel(pAd, pAd->hw_cfg.cent_ch, FALSE);
	AsicLockChannel(pAd, pAd->hw_cfg.cent_ch);

#ifdef DOT11_VHT_AC
	//+++Add by shiang for debug
	MTWF_LOG(DBG_CAT_AP, DBG_SUBCAT_ALL, DBG_LVL_TRACE, ("%s(): AP Set CentralFreq at %d(Prim=%d, HT-CentCh=%d, VHT-CentCh=%d, BBP_BW=%d)\n",
			 __FUNCTION__, pAd->hw_cfg.cent_ch, Channel,
			 pAd->CommonCfg.CentralChannel, pAd->CommonCfg.vht_cent_ch,
			 pAd->CommonCfg.BBPCurrentBW));
	//---Add by shiang for debug
#endif /* DOT11_VHT_AC */
	return TRUE;
}

#ifdef ROAMING_ENHANCE_SUPPORT
#ifndef ETH_HDR_LEN
#define ETH_HDR_LEN 14 /* dstMac(6) + srcMac(6) + protoType(2) */
#endif

#ifndef ETH_P_VLAN
#define ETH_P_VLAN       	0x8100          /* 802.1q (VLAN)  */
#endif

#ifndef VLAN_ETH_HDR_LEN
#define VLAN_ETH_HDR_LEN (ETH_HDR_LEN+4) /* 4 for h_vlan_TCI and h_vlan_encapsulated_proto */
#endif

#ifndef IP_HDR_SRC_OFFSET
#define IP_HDR_SRC_OFFSET 12 /* shift 12 for IP header len. */
#endif

#ifndef ARP_OP_OFFSET
#define ARP_OP_OFFSET 6 /* shift 6 len for ARP. */
#endif

BOOLEAN ApCliDoRoamingRefresh(
    IN RTMP_ADAPTER *pAd, 
    IN MAC_TABLE_ENTRY *pEntry, 
    IN PNDIS_PACKET pRxPacket, 
    IN struct wifi_dev *wdev,
    IN UCHAR *DestAddr)
{
    UCHAR *pPktHdr = NULL, *pLayerHdr = NULL;
    UINT16 ProtoType;
    BOOLEAN bUnicastARPReq = FALSE, bSendARP = FALSE;
    PNDIS_PACKET pPacket = NULL;

    if(!pRxPacket || !wdev)
        return FALSE;

    /* Get the upper layer protocol type of this 802.3 pkt */
    pPktHdr = GET_OS_PKT_DATAPTR(pRxPacket);
    ProtoType = OS_NTOHS(get_unaligned((PUINT16)(pPktHdr + (ETH_HDR_LEN-2))));
    if (ProtoType == ETH_P_VLAN)
    {
		pLayerHdr = (pPktHdr + VLAN_ETH_HDR_LEN);
        ProtoType = OS_NTOHS(get_unaligned((PUINT16)pLayerHdr));
    }
    else
		pLayerHdr = (pPktHdr + ETH_HDR_LEN);

    if(ProtoType == ETH_P_ARP)
    {
        UINT16 OpType;
        OpType = OS_NTOHS(get_unaligned((PUINT16)(pLayerHdr+ARP_OP_OFFSET))); /* ARP Operation */
        if(OpType == 0x0001) // ARP Request
        {
            if(DestAddr && !MAC_ADDR_IS_GROUP(DestAddr))
                bUnicastARPReq = TRUE;

            if(bUnicastARPReq)
            {
                struct sk_buff *skb = NULL;
                skb = skb_copy(RTPKT_TO_OSPKT(pRxPacket), GFP_ATOMIC);
                if(skb)
                {
                    bSendARP = TRUE;
                    skb->dev = wdev->if_dev;
                    pPacket = OSPKT_TO_RTPKT(skb);
                    NdisMoveMemory(GET_OS_PKT_DATAPTR(pPacket), BROADCAST_ADDR, MAC_ADDR_LEN);
                }
            }
            else
                pEntry->bRoamingRefreshDone = TRUE;
            MTWF_LOG(DBG_CAT_AP, DBG_SUBCAT_ALL, DBG_LVL_OFF,
                ("Got original ARP Request(Unicast:%d) from wireless STA!\n",bUnicastARPReq));
        }
    }
    else if(ProtoType == ETH_P_IP)
    {
        UINT32 SrcIP = 0;
        NdisMoveMemory(&SrcIP, (pLayerHdr + IP_HDR_SRC_OFFSET), 4);
        if(SrcIP != 0)
        {
            bSendARP = TRUE;
            pPacket = (PNDIS_PACKET)arp_create(ARPOP_REQUEST, ETH_P_ARP, SrcIP, wdev->if_dev,
                                    SrcIP, BROADCAST_ADDR, pEntry->Addr, BROADCAST_ADDR);
        }
    }

    if(bSendARP && pPacket)
    {
#if defined(CONFIG_WIFI_PKT_FWD) || defined(CONFIG_WIFI_PKT_FWD_MODULE)
        set_wf_fwd_cb(pAd, pPacket, wdev);
#endif /* CONFIG_WIFI_PKT_FWD */
        RtmpOsPktProtocolAssign(pPacket);
        RtmpOsPktRcvHandle(pPacket);
        pEntry->bRoamingRefreshDone = TRUE;
        MTWF_LOG(DBG_CAT_AP, DBG_SUBCAT_ALL, DBG_LVL_OFF,("Send roaming refresh done!\n"));
        return TRUE;
    }

    return FALSE;
}
#endif /* ROAMING_ENHANCE_SUPPORT */
#endif /* APCLI_SUPPORT */

