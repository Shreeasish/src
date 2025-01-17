/* $OpenBSD: bwfm.c,v 1.22 2017/12/18 18:40:50 patrick Exp $ */
/*
 * Copyright (c) 2010-2016 Broadcom Corporation
 * Copyright (c) 2016,2017 Patrick Wildt <patrick@blueri.se>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "bpfilter.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#if NBPFILTER > 0
#include <net/bpf.h>
#endif
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>

#include <dev/ic/bwfmvar.h>
#include <dev/ic/bwfmreg.h>

/* #define BWFM_DEBUG */
#ifdef BWFM_DEBUG
#define DPRINTF(x)	do { if (bwfm_debug > 0) printf x; } while (0)
#define DPRINTFN(n, x)	do { if (bwfm_debug >= (n)) printf x; } while (0)
static int bwfm_debug = 1;
#else
#define DPRINTF(x)	do { ; } while (0)
#define DPRINTFN(n, x)	do { ; } while (0)
#endif

#define DEVNAME(sc)	((sc)->sc_dev.dv_xname)

void	 bwfm_start(struct ifnet *);
void	 bwfm_init(struct ifnet *);
void	 bwfm_stop(struct ifnet *);
void	 bwfm_watchdog(struct ifnet *);
int	 bwfm_ioctl(struct ifnet *, u_long, caddr_t);
int	 bwfm_media_change(struct ifnet *);

int	 bwfm_chip_attach(struct bwfm_softc *);
int	 bwfm_chip_detach(struct bwfm_softc *, int);
struct bwfm_core *bwfm_chip_get_core(struct bwfm_softc *, int);
struct bwfm_core *bwfm_chip_get_pmu(struct bwfm_softc *);
int	 bwfm_chip_ai_isup(struct bwfm_softc *, struct bwfm_core *);
void	 bwfm_chip_ai_disable(struct bwfm_softc *, struct bwfm_core *,
	     uint32_t, uint32_t);
void	 bwfm_chip_ai_reset(struct bwfm_softc *, struct bwfm_core *,
	     uint32_t, uint32_t, uint32_t);
void	 bwfm_chip_dmp_erom_scan(struct bwfm_softc *);
int	 bwfm_chip_dmp_get_regaddr(struct bwfm_softc *, uint32_t *,
	     uint32_t *, uint32_t *);
int	 bwfm_chip_cr4_set_active(struct bwfm_softc *, uint32_t);
void	 bwfm_chip_cr4_set_passive(struct bwfm_softc *);
int	 bwfm_chip_ca7_set_active(struct bwfm_softc *, uint32_t);
void	 bwfm_chip_ca7_set_passive(struct bwfm_softc *);
int	 bwfm_chip_cm3_set_active(struct bwfm_softc *);
void	 bwfm_chip_cm3_set_passive(struct bwfm_softc *);
void	 bwfm_chip_socram_ramsize(struct bwfm_softc *, struct bwfm_core *);
void	 bwfm_chip_sysmem_ramsize(struct bwfm_softc *, struct bwfm_core *);
void	 bwfm_chip_tcm_ramsize(struct bwfm_softc *, struct bwfm_core *);
void	 bwfm_chip_tcm_rambase(struct bwfm_softc *);

int	 bwfm_proto_bcdc_query_dcmd(struct bwfm_softc *, int,
	     int, char *, size_t *);
int	 bwfm_proto_bcdc_set_dcmd(struct bwfm_softc *, int,
	     int, char *, size_t);

int	 bwfm_fwvar_cmd_get_data(struct bwfm_softc *, int, void *, size_t);
int	 bwfm_fwvar_cmd_set_data(struct bwfm_softc *, int, void *, size_t);
int	 bwfm_fwvar_cmd_get_int(struct bwfm_softc *, int, uint32_t *);
int	 bwfm_fwvar_cmd_set_int(struct bwfm_softc *, int, uint32_t);
int	 bwfm_fwvar_var_get_data(struct bwfm_softc *, char *, void *, size_t);
int	 bwfm_fwvar_var_set_data(struct bwfm_softc *, char *, void *, size_t);
int	 bwfm_fwvar_var_get_int(struct bwfm_softc *, char *, uint32_t *);
int	 bwfm_fwvar_var_set_int(struct bwfm_softc *, char *, uint32_t);

void	 bwfm_connect(struct bwfm_softc *);
void	 bwfm_scan(struct bwfm_softc *);

void	 bwfm_task(void *);
void	 bwfm_do_async(struct bwfm_softc *,
	     void (*)(struct bwfm_softc *, void *), void *, int);

int	 bwfm_set_key(struct ieee80211com *, struct ieee80211_node *,
	     struct ieee80211_key *);
void	 bwfm_delete_key(struct ieee80211com *, struct ieee80211_node *,
	     struct ieee80211_key *);
int	 bwfm_send_mgmt(struct ieee80211com *, struct ieee80211_node *,
	     int, int, int);
int	 bwfm_newstate(struct ieee80211com *, enum ieee80211_state, int);

void	 bwfm_set_key_cb(struct bwfm_softc *, void *);
void	 bwfm_delete_key_cb(struct bwfm_softc *, void *);
void	 bwfm_newstate_cb(struct bwfm_softc *, void *);

void	 bwfm_rx(struct bwfm_softc *, char *, size_t);
void	 bwfm_rx_event(struct bwfm_softc *, char *, size_t);
void	 bwfm_scan_node(struct bwfm_softc *, struct bwfm_bss_info *, size_t);

extern void ieee80211_node2req(struct ieee80211com *,
	     const struct ieee80211_node *, struct ieee80211_nodereq *);
extern void ieee80211_req2node(struct ieee80211com *,
	     const struct ieee80211_nodereq *, struct ieee80211_node *);

uint8_t bwfm_2ghz_channels[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
};
uint8_t bwfm_5ghz_channels[] = {
	34, 36, 38, 40, 42, 44, 46, 48, 52, 56, 60, 64, 100, 104, 108, 112,
	116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165,
};

struct bwfm_proto_ops bwfm_proto_bcdc_ops = {
	.proto_query_dcmd = bwfm_proto_bcdc_query_dcmd,
	.proto_set_dcmd = bwfm_proto_bcdc_set_dcmd,
};

struct cfdriver bwfm_cd = {
	NULL, "bwfm", DV_IFNET
};

void
bwfm_attach(struct bwfm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	uint32_t bandlist[3], tmp;
	int i, nbands, nmode, vhtmode;

	if (bwfm_fwvar_cmd_get_int(sc, BWFM_C_GET_VERSION, &tmp)) {
		printf("%s: could not read io type\n", DEVNAME(sc));
		return;
	} else
		sc->sc_io_type = tmp;
	if (bwfm_fwvar_var_get_data(sc, "cur_etheraddr", ic->ic_myaddr,
	    sizeof(ic->ic_myaddr))) {
		printf("%s: could not read mac address\n", DEVNAME(sc));
		return;
	}
	printf("%s: address %s\n", DEVNAME(sc), ether_sprintf(ic->ic_myaddr));

	/* Init host async commands ring. */
	sc->sc_cmdq.cur = sc->sc_cmdq.next = sc->sc_cmdq.queued = 0;
	sc->sc_taskq = taskq_create(DEVNAME(sc), 1, IPL_SOFTNET, 0);
	task_set(&sc->sc_task, bwfm_task, sc);

	ic->ic_phytype = IEEE80211_T_OFDM;	/* not only, but not used */
	ic->ic_opmode = IEEE80211_M_STA;	/* default to BSS mode */
	ic->ic_state = IEEE80211_S_INIT;

	ic->ic_caps =
	    IEEE80211_C_RSN | 	/* WPA/RSN */
	    IEEE80211_C_SCANALL |	/* device scans all channels at once */
	    IEEE80211_C_SCANALLBAND;	/* device scans all bands at once */

	if (bwfm_fwvar_var_get_int(sc, "nmode", &nmode))
		nmode = 0;
	if (bwfm_fwvar_var_get_int(sc, "vhtmode", &vhtmode))
		vhtmode = 0;
	if (bwfm_fwvar_cmd_get_data(sc, BWFM_C_GET_BANDLIST, bandlist,
	    sizeof(bandlist))) {
		printf("%s: couldn't get supported band list\n", DEVNAME(sc));
		return;
	}
	nbands = letoh32(bandlist[0]);
	for (i = 1; i <= nbands && i < nitems(bandlist); i++) {
		switch (letoh32(bandlist[i])) {
		case BWFM_BAND_2G:
			DPRINTF(("%s: 2G HT %d VHT %d\n",
			    DEVNAME(sc), nmode, vhtmode));
			ic->ic_sup_rates[IEEE80211_MODE_11B] =
			    ieee80211_std_rateset_11b;
			ic->ic_sup_rates[IEEE80211_MODE_11G] =
			    ieee80211_std_rateset_11g;

			for (i = 0; i < nitems(bwfm_2ghz_channels); i++) {
				uint8_t chan = bwfm_2ghz_channels[i];
				ic->ic_channels[chan].ic_freq =
				    ieee80211_ieee2mhz(chan, IEEE80211_CHAN_2GHZ);
				ic->ic_channels[chan].ic_flags =
				    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
				    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
				if (nmode)
					ic->ic_channels[chan].ic_flags |=
					    IEEE80211_CHAN_HT;
			}
			break;
		case BWFM_BAND_5G:
			DPRINTF(("%s: 5G HT %d VHT %d\n",
			    DEVNAME(sc), nmode, vhtmode));
			ic->ic_sup_rates[IEEE80211_MODE_11A] =
			    ieee80211_std_rateset_11a;

			for (i = 0; i < nitems(bwfm_5ghz_channels); i++) {
				uint8_t chan = bwfm_5ghz_channels[i];
				ic->ic_channels[chan].ic_freq =
				    ieee80211_ieee2mhz(chan, IEEE80211_CHAN_5GHZ);
				ic->ic_channels[chan].ic_flags =
				    IEEE80211_CHAN_A;
				if (nmode)
					ic->ic_channels[chan].ic_flags |=
					    IEEE80211_CHAN_HT;
			}
			break;
		default:
			printf("%s: unsupported band 0x%x\n", DEVNAME(sc),
			    letoh32(bandlist[i]));
			break;
		}
	}

	/* IBSS channel undefined for now. */
	ic->ic_ibss_chan = &ic->ic_channels[0];

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = bwfm_ioctl;
	ifp->if_start = bwfm_start;
	ifp->if_watchdog = bwfm_watchdog;
	memcpy(ifp->if_xname, DEVNAME(sc), IFNAMSIZ);

	if_attach(ifp);
	ieee80211_ifattach(ifp);

	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = bwfm_newstate;
	ic->ic_send_mgmt = bwfm_send_mgmt;
	ic->ic_set_key = bwfm_set_key;
	ic->ic_delete_key = bwfm_delete_key;

	ieee80211_media_init(ifp, bwfm_media_change, ieee80211_media_status);
}

int
bwfm_detach(struct bwfm_softc *sc, int flags)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	task_del(sc->sc_taskq, &sc->sc_task);
	taskq_destroy(sc->sc_taskq);
	ieee80211_ifdetach(ifp);
	if_detach(ifp);
	return 0;
}

void
bwfm_start(struct ifnet *ifp)
{
	struct bwfm_softc *sc = ifp->if_softc;
	struct mbuf *m;
	int error;

	if (!(ifp->if_flags & IFF_RUNNING))
		return;
	if (ifq_is_oactive(&ifp->if_snd))
		return;
	if (IFQ_IS_EMPTY(&ifp->if_snd))
		return;

	/* TODO: return if no link? */

	m = ifq_deq_begin(&ifp->if_snd);
	while (m != NULL) {
		error = sc->sc_bus_ops->bs_txdata(sc, m);
		if (error == ENOBUFS) {
			ifq_deq_rollback(&ifp->if_snd, m);
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		if (error == EFBIG) {
			ifq_deq_commit(&ifp->if_snd, m);
			m_freem(m); /* give up: drop it */
			ifp->if_oerrors++;
			continue;
		}

		/* Now we are committed to transmit the packet. */
		ifq_deq_commit(&ifp->if_snd, m);

#if NBPFILTER > 0
		if (ifp->if_bpf)
			bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_OUT);
#endif

		m = ifq_deq_begin(&ifp->if_snd);
	}
}

void
bwfm_init(struct ifnet *ifp)
{
	struct bwfm_softc *sc = ifp->if_softc;
	uint8_t evmask[BWFM_EVENT_MASK_LEN];
	struct bwfm_join_pref_params join_pref[2];

	if (bwfm_fwvar_var_set_int(sc, "mpc", 1)) {
		printf("%s: could not set mpc\n", DEVNAME(sc));
		return;
	}

	/* Select target by RSSI (boost on 5GHz) */
	join_pref[0].type = BWFM_JOIN_PREF_RSSI_DELTA;
	join_pref[0].len = 2;
	join_pref[0].rssi_gain = BWFM_JOIN_PREF_RSSI_BOOST;
	join_pref[0].band = BWFM_JOIN_PREF_BAND_5G;
	join_pref[1].type = BWFM_JOIN_PREF_RSSI;
	join_pref[1].len = 2;
	join_pref[1].rssi_gain = 0;
	join_pref[1].band = 0;
	if (bwfm_fwvar_var_set_data(sc, "join_pref", join_pref,
	    sizeof(join_pref))) {
		printf("%s: could not set join pref\n", DEVNAME(sc));
		return;
	}

	if (bwfm_fwvar_var_get_data(sc, "event_msgs", evmask, sizeof(evmask))) {
		printf("%s: could not get event mask\n", DEVNAME(sc));
		return;
	}
	evmask[BWFM_E_IF / 8] |= 1 << (BWFM_E_IF % 8);
	evmask[BWFM_E_LINK / 8] |= 1 << (BWFM_E_LINK % 8);
	evmask[BWFM_E_ASSOC / 8] |= 1 << (BWFM_E_ASSOC % 8);
	evmask[BWFM_E_SET_SSID / 8] |= 1 << (BWFM_E_SET_SSID % 8);
	evmask[BWFM_E_ESCAN_RESULT / 8] |= 1 << (BWFM_E_ESCAN_RESULT % 8);
	if (bwfm_fwvar_var_set_data(sc, "event_msgs", evmask, sizeof(evmask))) {
		printf("%s: could not set event mask\n", DEVNAME(sc));
		return;
	}

	if (bwfm_fwvar_cmd_set_int(sc, BWFM_C_SET_SCAN_CHANNEL_TIME,
	    BWFM_DEFAULT_SCAN_CHANNEL_TIME)) {
		printf("%s: could not set scan channel time\n", DEVNAME(sc));
		return;
	}
	if (bwfm_fwvar_cmd_set_int(sc, BWFM_C_SET_SCAN_UNASSOC_TIME,
	    BWFM_DEFAULT_SCAN_UNASSOC_TIME)) {
		printf("%s: could not set scan unassoc time\n", DEVNAME(sc));
		return;
	}
	if (bwfm_fwvar_cmd_set_int(sc, BWFM_C_SET_SCAN_PASSIVE_TIME,
	    BWFM_DEFAULT_SCAN_PASSIVE_TIME)) {
		printf("%s: could not set scan passive time\n", DEVNAME(sc));
		return;
	}

	if (bwfm_fwvar_cmd_set_int(sc, BWFM_C_SET_PM, 2)) {
		printf("%s: could not set power\n", DEVNAME(sc));
		return;
	}

	bwfm_fwvar_var_set_int(sc, "txbf", 1);
	bwfm_fwvar_cmd_set_int(sc, BWFM_C_UP, 0);
	bwfm_fwvar_cmd_set_int(sc, BWFM_C_SET_INFRA, 1);
	bwfm_fwvar_cmd_set_int(sc, BWFM_C_SET_AP, 0);

	/* Disable all offloading (ARP, NDP, TCP/UDP cksum). */
	bwfm_fwvar_var_set_int(sc, "arp_ol", 0);
	bwfm_fwvar_var_set_int(sc, "arpoe", 0);
	bwfm_fwvar_var_set_int(sc, "ndoe", 0);
	bwfm_fwvar_var_set_int(sc, "toe", 0);

	/*
	 * The firmware supplicant can handle the WPA handshake for
	 * us, but we honestly want to do this ourselves, so disable
	 * the firmware supplicant and let our stack handle it.
	 */
	bwfm_fwvar_var_set_int(sc, "sup_wpa", 0);

#if 0
	/* TODO: set these on proper ioctl */
	bwfm_fwvar_var_set_int(sc, "allmulti", 1);
	bwfm_fwvar_cmd_set_int(sc, BWFM_C_SET_PROMISC, 1);
#endif

	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);

	ieee80211_begin_scan(ifp);
}

void
bwfm_stop(struct ifnet *ifp)
{
	struct bwfm_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;

	sc->sc_tx_timer = 0;
	ifp->if_timer = 0;
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);

	/* In case we were scanning, release the scan "lock". */
	ic->ic_scan_lock = IEEE80211_SCAN_UNLOCKED;

	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);

	bwfm_fwvar_cmd_set_int(sc, BWFM_C_DOWN, 1);
	bwfm_fwvar_cmd_set_int(sc, BWFM_C_SET_PM, 0);
}

void
bwfm_watchdog(struct ifnet *ifp)
{
	struct bwfm_softc *sc = ifp->if_softc;

	ifp->if_timer = 0;

	if (sc->sc_tx_timer > 0) {
		if (--sc->sc_tx_timer == 0) {
			printf("%s: device timeout\n", DEVNAME(sc));
			ifp->if_oerrors++;
			return;
		}
		ifp->if_timer = 1;
	}
	ieee80211_watchdog(ifp);
}

int
bwfm_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	int s, error = 0;

	s = splnet();
	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING))
				bwfm_init(ifp);
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				bwfm_stop(ifp);
		}
		break;
	default:
		error = ieee80211_ioctl(ifp, cmd, data);
	}
	if (error == ENETRESET) {
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
		    (IFF_UP | IFF_RUNNING)) {
			bwfm_stop(ifp);
			bwfm_init(ifp);
		}
		error = 0;
	}
	splx(s);
	return error;
}

int
bwfm_media_change(struct ifnet *ifp)
{
	int error;

	error = ieee80211_media_change(ifp);
	if (error != ENETRESET)
		return error;

	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
	    (IFF_UP | IFF_RUNNING)) {
		bwfm_stop(ifp);
		bwfm_init(ifp);
	}
	return 0;
}

/* Chip initialization (SDIO, PCIe) */
int
bwfm_chip_attach(struct bwfm_softc *sc)
{
	struct bwfm_core *core;
	int need_socram = 0;
	int has_socram = 0;
	int cpu_found = 0;
	uint32_t val;

	LIST_INIT(&sc->sc_chip.ch_list);

	if (sc->sc_buscore_ops->bc_prepare(sc) != 0) {
		printf("%s: failed buscore prepare\n", DEVNAME(sc));
		return 1;
	}

	val = sc->sc_buscore_ops->bc_read(sc,
	    BWFM_CHIP_BASE + BWFM_CHIP_REG_CHIPID);
	sc->sc_chip.ch_chip = BWFM_CHIP_CHIPID_ID(val);
	sc->sc_chip.ch_chiprev = BWFM_CHIP_CHIPID_REV(val);

	if ((sc->sc_chip.ch_chip > 0xa000) || (sc->sc_chip.ch_chip < 0x4000))
		snprintf(sc->sc_chip.ch_name, sizeof(sc->sc_chip.ch_name),
		    "%d", sc->sc_chip.ch_chip);
	else
		snprintf(sc->sc_chip.ch_name, sizeof(sc->sc_chip.ch_name),
		    "%x", sc->sc_chip.ch_chip);

	switch (BWFM_CHIP_CHIPID_TYPE(val))
	{
	case BWFM_CHIP_CHIPID_TYPE_SOCI_SB:
		printf("%s: SoC interconnect SB not implemented\n",
		    DEVNAME(sc));
		return 1;
	case BWFM_CHIP_CHIPID_TYPE_SOCI_AI:
		sc->sc_chip.ch_core_isup = bwfm_chip_ai_isup;
		sc->sc_chip.ch_core_disable = bwfm_chip_ai_disable;
		sc->sc_chip.ch_core_reset = bwfm_chip_ai_reset;
		bwfm_chip_dmp_erom_scan(sc);
		break;
	default:
		printf("%s: SoC interconnect %d unknown\n",
		    DEVNAME(sc), BWFM_CHIP_CHIPID_TYPE(val));
		return 1;
	}

	LIST_FOREACH(core, &sc->sc_chip.ch_list, co_link) {
		DPRINTF(("%s: 0x%x:%-2d base 0x%08x wrap 0x%08x\n",
		    DEVNAME(sc), core->co_id, core->co_rev,
		    core->co_base, core->co_wrapbase));

		switch (core->co_id) {
		case BWFM_AGENT_CORE_ARM_CM3:
			need_socram = true;
			/* FALLTHROUGH */
		case BWFM_AGENT_CORE_ARM_CR4:
		case BWFM_AGENT_CORE_ARM_CA7:
			cpu_found = true;
			break;
		case BWFM_AGENT_INTERNAL_MEM:
			has_socram = true;
			break;
		default:
			break;
		}
	}

	if (!cpu_found) {
		printf("%s: CPU core not detected\n", DEVNAME(sc));
		return 1;
	}
	if (need_socram && !has_socram) {
		printf("%s: RAM core not provided\n", DEVNAME(sc));
		return 1;
	}

	bwfm_chip_set_passive(sc);

	if (sc->sc_buscore_ops->bc_reset) {
		sc->sc_buscore_ops->bc_reset(sc);
		bwfm_chip_set_passive(sc);
	}

	if ((core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CR4)) != NULL) {
		bwfm_chip_tcm_ramsize(sc, core);
		bwfm_chip_tcm_rambase(sc);
	} else if ((core = bwfm_chip_get_core(sc, BWFM_AGENT_SYS_MEM)) != NULL) {
		bwfm_chip_sysmem_ramsize(sc, core);
		bwfm_chip_tcm_rambase(sc);
	} else if ((core = bwfm_chip_get_core(sc, BWFM_AGENT_INTERNAL_MEM)) != NULL) {
		bwfm_chip_socram_ramsize(sc, core);
	}

	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_CHIPCOMMON);
	sc->sc_chip.ch_cc_caps = sc->sc_buscore_ops->bc_read(sc,
	    core->co_base + BWFM_CHIP_REG_CAPABILITIES);
	sc->sc_chip.ch_cc_caps_ext = sc->sc_buscore_ops->bc_read(sc,
	    core->co_base + BWFM_CHIP_REG_CAPABILITIES_EXT);

	core = bwfm_chip_get_pmu(sc);
	if (sc->sc_chip.ch_cc_caps & BWFM_CHIP_REG_CAPABILITIES_PMU) {
		sc->sc_chip.ch_pmucaps = sc->sc_buscore_ops->bc_read(sc,
		    core->co_base + BWFM_CHIP_REG_PMUCAPABILITIES);
		sc->sc_chip.ch_pmurev = sc->sc_chip.ch_pmucaps &
		    BWFM_CHIP_REG_PMUCAPABILITIES_REV_MASK;
	}

	if (sc->sc_buscore_ops->bc_setup)
		sc->sc_buscore_ops->bc_setup(sc);

	return 0;
}

struct bwfm_core *
bwfm_chip_get_core(struct bwfm_softc *sc, int id)
{
	struct bwfm_core *core;

	LIST_FOREACH(core, &sc->sc_chip.ch_list, co_link) {
		if (core->co_id == id)
			return core;
	}

	return NULL;
}

struct bwfm_core *
bwfm_chip_get_pmu(struct bwfm_softc *sc)
{
	struct bwfm_core *cc, *pmu;

	cc = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_CHIPCOMMON);
	if (cc->co_rev >= 35 && sc->sc_chip.ch_cc_caps_ext &
	    BWFM_CHIP_REG_CAPABILITIES_EXT_AOB_PRESENT) {
		pmu = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_PMU);
		if (pmu)
			return pmu;
	}

	return cc;
}

/* Functions for the AI interconnect */
int
bwfm_chip_ai_isup(struct bwfm_softc *sc, struct bwfm_core *core)
{
	uint32_t ioctl, reset;

	ioctl = sc->sc_buscore_ops->bc_read(sc,
	    core->co_wrapbase + BWFM_AGENT_IOCTL);
	reset = sc->sc_buscore_ops->bc_read(sc,
	    core->co_wrapbase + BWFM_AGENT_RESET_CTL);

	if (((ioctl & (BWFM_AGENT_IOCTL_FGC | BWFM_AGENT_IOCTL_CLK)) ==
	    BWFM_AGENT_IOCTL_CLK) &&
	    ((reset & BWFM_AGENT_RESET_CTL_RESET) == 0))
		return 1;

	return 0;
}

void
bwfm_chip_ai_disable(struct bwfm_softc *sc, struct bwfm_core *core,
    uint32_t prereset, uint32_t reset)
{
	uint32_t val;
	int i;

	val = sc->sc_buscore_ops->bc_read(sc,
	    core->co_wrapbase + BWFM_AGENT_RESET_CTL);
	if ((val & BWFM_AGENT_RESET_CTL_RESET) == 0) {

		sc->sc_buscore_ops->bc_write(sc,
		    core->co_wrapbase + BWFM_AGENT_IOCTL,
		    prereset | BWFM_AGENT_IOCTL_FGC | BWFM_AGENT_IOCTL_CLK);
		sc->sc_buscore_ops->bc_read(sc,
		    core->co_wrapbase + BWFM_AGENT_IOCTL);

		sc->sc_buscore_ops->bc_write(sc,
		    core->co_wrapbase + BWFM_AGENT_RESET_CTL,
		    BWFM_AGENT_RESET_CTL_RESET);
		delay(20);

		for (i = 300; i > 0; i--) {
			if (sc->sc_buscore_ops->bc_read(sc,
			    core->co_wrapbase + BWFM_AGENT_RESET_CTL) ==
			    BWFM_AGENT_RESET_CTL_RESET)
				break;
		}
		if (i == 0)
			printf("%s: timeout on core reset\n", DEVNAME(sc));
	}

	sc->sc_buscore_ops->bc_write(sc,
	    core->co_wrapbase + BWFM_AGENT_IOCTL,
	    reset | BWFM_AGENT_IOCTL_FGC | BWFM_AGENT_IOCTL_CLK);
	sc->sc_buscore_ops->bc_read(sc,
	    core->co_wrapbase + BWFM_AGENT_IOCTL);
}

void
bwfm_chip_ai_reset(struct bwfm_softc *sc, struct bwfm_core *core,
    uint32_t prereset, uint32_t reset, uint32_t postreset)
{
	int i;

	bwfm_chip_ai_disable(sc, core, prereset, reset);

	for (i = 50; i > 0; i--) {
		if ((sc->sc_buscore_ops->bc_read(sc,
		    core->co_wrapbase + BWFM_AGENT_RESET_CTL) &
		    BWFM_AGENT_RESET_CTL_RESET) == 0)
			break;
		sc->sc_buscore_ops->bc_write(sc,
		    core->co_wrapbase + BWFM_AGENT_RESET_CTL, 0);
		delay(60);
	}
	if (i == 0)
		printf("%s: timeout on core reset\n", DEVNAME(sc));

	sc->sc_buscore_ops->bc_write(sc,
	    core->co_wrapbase + BWFM_AGENT_IOCTL,
	    postreset | BWFM_AGENT_IOCTL_CLK);
	sc->sc_buscore_ops->bc_read(sc,
	    core->co_wrapbase + BWFM_AGENT_IOCTL);
}

void
bwfm_chip_dmp_erom_scan(struct bwfm_softc *sc)
{
	uint32_t erom, val, base, wrap;
	uint8_t type = 0;
	uint16_t id;
	uint8_t nmw, nsw, rev;
	struct bwfm_core *core;

	erom = sc->sc_buscore_ops->bc_read(sc,
	    BWFM_CHIP_BASE + BWFM_CHIP_REG_EROMPTR);
	while (type != BWFM_DMP_DESC_EOT) {
		val = sc->sc_buscore_ops->bc_read(sc, erom);
		type = val & BWFM_DMP_DESC_MASK;
		erom += 4;

		if (type != BWFM_DMP_DESC_COMPONENT)
			continue;

		id = (val & BWFM_DMP_COMP_PARTNUM)
		    >> BWFM_DMP_COMP_PARTNUM_S;

		val = sc->sc_buscore_ops->bc_read(sc, erom);
		type = val & BWFM_DMP_DESC_MASK;
		erom += 4;

		if (type != BWFM_DMP_DESC_COMPONENT) {
			printf("%s: not component descriptor\n", DEVNAME(sc));
			return;
		}

		nmw = (val & BWFM_DMP_COMP_NUM_MWRAP)
		    >> BWFM_DMP_COMP_NUM_MWRAP_S;
		nsw = (val & BWFM_DMP_COMP_NUM_SWRAP)
		    >> BWFM_DMP_COMP_NUM_SWRAP_S;
		rev = (val & BWFM_DMP_COMP_REVISION)
		    >> BWFM_DMP_COMP_REVISION_S;

		if (nmw + nsw == 0 && id != BWFM_AGENT_CORE_PMU)
			continue;

		if (bwfm_chip_dmp_get_regaddr(sc, &erom, &base, &wrap))
			continue;

		core = malloc(sizeof(*core), M_DEVBUF, M_WAITOK);
		core->co_id = id;
		core->co_base = base;
		core->co_wrapbase = wrap;
		core->co_rev = rev;
		LIST_INSERT_HEAD(&sc->sc_chip.ch_list, core, co_link);
	}
}

int
bwfm_chip_dmp_get_regaddr(struct bwfm_softc *sc, uint32_t *erom,
    uint32_t *base, uint32_t *wrap)
{
	uint8_t type = 0, mpnum = 0;
	uint8_t stype, sztype, wraptype;
	uint32_t val;

	*base = 0;
	*wrap = 0;

	val = sc->sc_buscore_ops->bc_read(sc, *erom);
	type = val & BWFM_DMP_DESC_MASK;
	if (type == BWFM_DMP_DESC_MASTER_PORT) {
		mpnum = (val & BWFM_DMP_MASTER_PORT_NUM)
		    >> BWFM_DMP_MASTER_PORT_NUM_S;
		wraptype = BWFM_DMP_SLAVE_TYPE_MWRAP;
		*erom += 4;
	} else if ((type & ~BWFM_DMP_DESC_ADDRSIZE_GT32) ==
	    BWFM_DMP_DESC_ADDRESS)
		wraptype = BWFM_DMP_SLAVE_TYPE_SWRAP;
	else
		return 1;

	do {
		do {
			val = sc->sc_buscore_ops->bc_read(sc, *erom);
			type = val & BWFM_DMP_DESC_MASK;
			if (type == BWFM_DMP_DESC_COMPONENT)
				return 0;
			if (type == BWFM_DMP_DESC_EOT)
				return 1;
			*erom += 4;
		} while ((type & ~BWFM_DMP_DESC_ADDRSIZE_GT32) !=
		     BWFM_DMP_DESC_ADDRESS);

		if (type & BWFM_DMP_DESC_ADDRSIZE_GT32)
			*erom += 4;

		sztype = (val & BWFM_DMP_SLAVE_SIZE_TYPE)
		    >> BWFM_DMP_SLAVE_SIZE_TYPE_S;
		if (sztype == BWFM_DMP_SLAVE_SIZE_DESC) {
			val = sc->sc_buscore_ops->bc_read(sc, *erom);
			type = val & BWFM_DMP_DESC_MASK;
			if (type & BWFM_DMP_DESC_ADDRSIZE_GT32)
				*erom += 8;
			else
				*erom += 4;
		}
		if (sztype != BWFM_DMP_SLAVE_SIZE_4K)
			continue;

		stype = (val & BWFM_DMP_SLAVE_TYPE) >> BWFM_DMP_SLAVE_TYPE_S;
		if (*base == 0 && stype == BWFM_DMP_SLAVE_TYPE_SLAVE)
			*base = val & BWFM_DMP_SLAVE_ADDR_BASE;
		if (*wrap == 0 && stype == wraptype)
			*wrap = val & BWFM_DMP_SLAVE_ADDR_BASE;
	} while (*base == 0 || *wrap == 0);

	return 0;
}

/* Core configuration */
int
bwfm_chip_set_active(struct bwfm_softc *sc, uint32_t rstvec)
{
	if (bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CR4) != NULL)
		return bwfm_chip_cr4_set_active(sc, rstvec);
	if (bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CA7) != NULL)
		return bwfm_chip_ca7_set_active(sc, rstvec);
	if (bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CM3) != NULL)
		return bwfm_chip_cm3_set_active(sc);
	return 1;
}

void
bwfm_chip_set_passive(struct bwfm_softc *sc)
{
	if (bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CR4) != NULL) {
		bwfm_chip_cr4_set_passive(sc);
		return;
	}
	if (bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CA7) != NULL) {
		bwfm_chip_ca7_set_passive(sc);
		return;
	}
	if (bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CM3) != NULL) {
		bwfm_chip_cm3_set_passive(sc);
		return;
	}
}

int
bwfm_chip_cr4_set_active(struct bwfm_softc *sc, uint32_t rstvec)
{
	struct bwfm_core *core;

	sc->sc_buscore_ops->bc_activate(sc, rstvec);
	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CR4);
	sc->sc_chip.ch_core_reset(sc, core,
	    BWFM_AGENT_IOCTL_ARMCR4_CPUHALT, 0, 0);

	return 0;
}

void
bwfm_chip_cr4_set_passive(struct bwfm_softc *sc)
{
	struct bwfm_core *core;
	uint32_t val;

	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CR4);
	val = sc->sc_buscore_ops->bc_read(sc,
	    core->co_wrapbase + BWFM_AGENT_IOCTL);
	sc->sc_chip.ch_core_reset(sc, core,
	    val & BWFM_AGENT_IOCTL_ARMCR4_CPUHALT,
	    BWFM_AGENT_IOCTL_ARMCR4_CPUHALT,
	    BWFM_AGENT_IOCTL_ARMCR4_CPUHALT);

	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_80211);
	sc->sc_chip.ch_core_reset(sc, core, BWFM_AGENT_D11_IOCTL_PHYRESET |
	    BWFM_AGENT_D11_IOCTL_PHYCLOCKEN, BWFM_AGENT_D11_IOCTL_PHYCLOCKEN,
	    BWFM_AGENT_D11_IOCTL_PHYCLOCKEN);
}

int
bwfm_chip_ca7_set_active(struct bwfm_softc *sc, uint32_t rstvec)
{
	struct bwfm_core *core;

	sc->sc_buscore_ops->bc_activate(sc, rstvec);
	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CA7);
	sc->sc_chip.ch_core_reset(sc, core,
	    BWFM_AGENT_IOCTL_ARMCR4_CPUHALT, 0, 0);

	return 0;
}

void
bwfm_chip_ca7_set_passive(struct bwfm_softc *sc)
{
	struct bwfm_core *core;
	uint32_t val;

	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CA7);
	val = sc->sc_buscore_ops->bc_read(sc,
	    core->co_wrapbase + BWFM_AGENT_IOCTL);
	sc->sc_chip.ch_core_reset(sc, core,
	    val & BWFM_AGENT_IOCTL_ARMCR4_CPUHALT,
	    BWFM_AGENT_IOCTL_ARMCR4_CPUHALT,
	    BWFM_AGENT_IOCTL_ARMCR4_CPUHALT);

	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_80211);
	sc->sc_chip.ch_core_reset(sc, core, BWFM_AGENT_D11_IOCTL_PHYRESET |
	    BWFM_AGENT_D11_IOCTL_PHYCLOCKEN, BWFM_AGENT_D11_IOCTL_PHYCLOCKEN,
	    BWFM_AGENT_D11_IOCTL_PHYCLOCKEN);
}

int
bwfm_chip_cm3_set_active(struct bwfm_softc *sc)
{
	panic("%s: cm3 not supported", DEVNAME(sc));
}

void
bwfm_chip_cm3_set_passive(struct bwfm_softc *sc)
{
	struct bwfm_core *core;

	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_ARM_CM3);
	sc->sc_chip.ch_core_disable(sc, core, 0, 0);
	core = bwfm_chip_get_core(sc, BWFM_AGENT_CORE_80211);
	sc->sc_chip.ch_core_reset(sc, core, BWFM_AGENT_D11_IOCTL_PHYRESET |
	    BWFM_AGENT_D11_IOCTL_PHYCLOCKEN, BWFM_AGENT_D11_IOCTL_PHYCLOCKEN,
	    BWFM_AGENT_D11_IOCTL_PHYCLOCKEN);
	core = bwfm_chip_get_core(sc, BWFM_AGENT_INTERNAL_MEM);
	sc->sc_chip.ch_core_reset(sc, core, 0, 0, 0);

	if (sc->sc_chip.ch_chip == BRCM_CC_43430_CHIP_ID) {
		sc->sc_buscore_ops->bc_write(sc,
		    core->co_base + BWFM_SOCRAM_BANKIDX, 3);
		sc->sc_buscore_ops->bc_write(sc,
		    core->co_base + BWFM_SOCRAM_BANKPDA, 0);
	}
}

/* RAM size helpers */
void
bwfm_chip_socram_ramsize(struct bwfm_softc *sc, struct bwfm_core *core)
{
	uint32_t coreinfo, nb, lss, banksize, bankinfo;
	uint32_t ramsize = 0, srsize = 0;
	int i;

	if (!sc->sc_chip.ch_core_isup(sc, core))
		sc->sc_chip.ch_core_reset(sc, core, 0, 0, 0);

	coreinfo = sc->sc_buscore_ops->bc_read(sc,
	    core->co_base + BWFM_SOCRAM_COREINFO);
	nb = (coreinfo & BWFM_SOCRAM_COREINFO_SRNB_MASK)
	    >> BWFM_SOCRAM_COREINFO_SRNB_SHIFT;

	if (core->co_rev <= 7 || core->co_rev == 12) {
		banksize = coreinfo & BWFM_SOCRAM_COREINFO_SRBSZ_MASK;
		lss = (coreinfo & BWFM_SOCRAM_COREINFO_LSS_MASK)
		    >> BWFM_SOCRAM_COREINFO_LSS_SHIFT;
		if (lss != 0)
			nb--;
		ramsize = nb * (1 << (banksize + BWFM_SOCRAM_COREINFO_SRBSZ_BASE));
		if (lss != 0)
			ramsize += (1 << ((lss - 1) + BWFM_SOCRAM_COREINFO_SRBSZ_BASE));
	} else {
		for (i = 0; i < nb; i++) {
			sc->sc_buscore_ops->bc_write(sc,
			    core->co_base + BWFM_SOCRAM_BANKIDX,
			    (BWFM_SOCRAM_BANKIDX_MEMTYPE_RAM <<
			    BWFM_SOCRAM_BANKIDX_MEMTYPE_SHIFT) | i);
			bankinfo = sc->sc_buscore_ops->bc_read(sc,
			    core->co_base + BWFM_SOCRAM_BANKINFO);
			banksize = ((bankinfo & BWFM_SOCRAM_BANKINFO_SZMASK) + 1)
			    * BWFM_SOCRAM_BANKINFO_SZBASE;
			ramsize += banksize;
			if (bankinfo & BWFM_SOCRAM_BANKINFO_RETNTRAM_MASK)
				srsize += banksize;
		}
	}

	switch (sc->sc_chip.ch_chip) {
	case BRCM_CC_4334_CHIP_ID:
		if (sc->sc_chip.ch_chiprev < 2)
			srsize = 32 * 1024;
		break;
	case BRCM_CC_43430_CHIP_ID:
		srsize = 64 * 1024;
		break;
	default:
		break;
	}

	sc->sc_chip.ch_ramsize = ramsize;
	sc->sc_chip.ch_srsize = srsize;
}

void
bwfm_chip_sysmem_ramsize(struct bwfm_softc *sc, struct bwfm_core *core)
{
	panic("%s: sysmem ramsize not supported", DEVNAME(sc));
}

void
bwfm_chip_tcm_ramsize(struct bwfm_softc *sc, struct bwfm_core *core)
{
	uint32_t cap, nab, nbb, totb, bxinfo, ramsize = 0;
	int i;

	cap = sc->sc_buscore_ops->bc_read(sc, core->co_base + BWFM_ARMCR4_CAP);
	nab = (cap & BWFM_ARMCR4_CAP_TCBANB_MASK) >> BWFM_ARMCR4_CAP_TCBANB_SHIFT;
	nbb = (cap & BWFM_ARMCR4_CAP_TCBBNB_MASK) >> BWFM_ARMCR4_CAP_TCBBNB_SHIFT;
	totb = nab + nbb;

	for (i = 0; i < totb; i++) {
		sc->sc_buscore_ops->bc_write(sc,
		    core->co_base + BWFM_ARMCR4_BANKIDX, i);
		bxinfo = sc->sc_buscore_ops->bc_read(sc,
		    core->co_base + BWFM_ARMCR4_BANKINFO);
		ramsize += ((bxinfo & BWFM_ARMCR4_BANKINFO_BSZ_MASK) + 1) *
		    BWFM_ARMCR4_BANKINFO_BSZ_MULT;
	}

	sc->sc_chip.ch_ramsize = ramsize;
}

void
bwfm_chip_tcm_rambase(struct bwfm_softc *sc)
{
	switch (sc->sc_chip.ch_chip) {
	case BRCM_CC_4345_CHIP_ID:
		sc->sc_chip.ch_rambase = 0x198000;
		break;
	case BRCM_CC_4335_CHIP_ID:
	case BRCM_CC_4339_CHIP_ID:
	case BRCM_CC_4350_CHIP_ID:
	case BRCM_CC_4354_CHIP_ID:
	case BRCM_CC_4356_CHIP_ID:
	case BRCM_CC_43567_CHIP_ID:
	case BRCM_CC_43569_CHIP_ID:
	case BRCM_CC_43570_CHIP_ID:
	case BRCM_CC_4358_CHIP_ID:
	case BRCM_CC_4359_CHIP_ID:
	case BRCM_CC_43602_CHIP_ID:
	case BRCM_CC_4371_CHIP_ID:
		sc->sc_chip.ch_rambase = 0x180000;
		break;
	case BRCM_CC_43465_CHIP_ID:
	case BRCM_CC_43525_CHIP_ID:
	case BRCM_CC_4365_CHIP_ID:
	case BRCM_CC_4366_CHIP_ID:
		sc->sc_chip.ch_rambase = 0x200000;
		break;
	case CY_CC_4373_CHIP_ID:
		sc->sc_chip.ch_rambase = 0x160000;
		break;
	default:
		printf("%s: unknown chip: %d\n", DEVNAME(sc),
		    sc->sc_chip.ch_chip);
		break;
	}
}

/* BCDC protocol implementation */
int
bwfm_proto_bcdc_query_dcmd(struct bwfm_softc *sc, int ifidx,
    int cmd, char *buf, size_t *len)
{
	struct bwfm_proto_bcdc_dcmd *dcmd;
	size_t size = sizeof(dcmd->hdr) + *len;
	static int reqid = 0;
	int ret = 1;

	reqid++;

	dcmd = malloc(sizeof(*dcmd), M_TEMP, M_WAITOK | M_ZERO);
	if (*len > sizeof(dcmd->buf))
		goto err;

	dcmd->hdr.cmd = htole32(cmd);
	dcmd->hdr.len = htole32(*len);
	dcmd->hdr.flags |= BWFM_BCDC_DCMD_GET;
	dcmd->hdr.flags |= BWFM_BCDC_DCMD_ID_SET(reqid);
	dcmd->hdr.flags |= BWFM_BCDC_DCMD_IF_SET(ifidx);
	dcmd->hdr.flags = htole32(dcmd->hdr.flags);
	memcpy(&dcmd->buf, buf, *len);

	if (sc->sc_bus_ops->bs_txctl(sc, (void *)dcmd,
	     sizeof(dcmd->hdr) + *len)) {
		DPRINTF(("%s: tx failed\n", DEVNAME(sc)));
		goto err;
	}

	do {
		if (sc->sc_bus_ops->bs_rxctl(sc, (void *)dcmd, &size)) {
			DPRINTF(("%s: rx failed\n", DEVNAME(sc)));
			goto err;
		}
		dcmd->hdr.cmd = letoh32(dcmd->hdr.cmd);
		dcmd->hdr.len = letoh32(dcmd->hdr.len);
		dcmd->hdr.flags = letoh32(dcmd->hdr.flags);
		dcmd->hdr.status = letoh32(dcmd->hdr.status);
	} while (BWFM_BCDC_DCMD_ID_GET(dcmd->hdr.flags) != reqid);

	if (BWFM_BCDC_DCMD_ID_GET(dcmd->hdr.flags) != reqid) {
		printf("%s: unexpected request id\n", DEVNAME(sc));
		goto err;
	}

	if (buf) {
		if (size > *len)
			size = *len;
		if (size < *len)
			*len = size;
		memcpy(buf, dcmd->buf, *len);
	}

	if (dcmd->hdr.flags & BWFM_BCDC_DCMD_ERROR)
		ret = dcmd->hdr.status;
	else
		ret = 0;
err:
	free(dcmd, M_TEMP, sizeof(*dcmd));
	return ret;
}

int
bwfm_proto_bcdc_set_dcmd(struct bwfm_softc *sc, int ifidx,
    int cmd, char *buf, size_t len)
{
	struct bwfm_proto_bcdc_dcmd *dcmd;
	size_t size = sizeof(dcmd->hdr) + len;
	int reqid = 0;
	int ret = 1;

	reqid++;

	dcmd = malloc(sizeof(*dcmd), M_TEMP, M_WAITOK | M_ZERO);
	if (len > sizeof(dcmd->buf))
		goto err;

	dcmd->hdr.cmd = htole32(cmd);
	dcmd->hdr.len = htole32(len);
	dcmd->hdr.flags |= BWFM_BCDC_DCMD_SET;
	dcmd->hdr.flags |= BWFM_BCDC_DCMD_ID_SET(reqid);
	dcmd->hdr.flags |= BWFM_BCDC_DCMD_IF_SET(ifidx);
	dcmd->hdr.flags = htole32(dcmd->hdr.flags);
	memcpy(&dcmd->buf, buf, len);

	if (sc->sc_bus_ops->bs_txctl(sc, (void *)dcmd, size)) {
		DPRINTF(("%s: tx failed\n", DEVNAME(sc)));
		goto err;
	}

	do {
		if (sc->sc_bus_ops->bs_rxctl(sc, (void *)dcmd, &size)) {
			DPRINTF(("%s: rx failed\n", DEVNAME(sc)));
			goto err;
		}
		dcmd->hdr.cmd = letoh32(dcmd->hdr.cmd);
		dcmd->hdr.len = letoh32(dcmd->hdr.len);
		dcmd->hdr.flags = letoh32(dcmd->hdr.flags);
		dcmd->hdr.status = letoh32(dcmd->hdr.status);
	} while (BWFM_BCDC_DCMD_ID_GET(dcmd->hdr.flags) != reqid);

	if (BWFM_BCDC_DCMD_ID_GET(dcmd->hdr.flags) != reqid) {
		printf("%s: unexpected request id\n", DEVNAME(sc));
		goto err;
	}

	if (dcmd->hdr.flags & BWFM_BCDC_DCMD_ERROR)
		return dcmd->hdr.status;

	ret = 0;
err:
	free(dcmd, M_TEMP, sizeof(*dcmd));
	return ret;
}

/* FW Variable code */
int
bwfm_fwvar_cmd_get_data(struct bwfm_softc *sc, int cmd, void *data, size_t len)
{
	return sc->sc_proto_ops->proto_query_dcmd(sc, 0, cmd, data, &len);
}

int
bwfm_fwvar_cmd_set_data(struct bwfm_softc *sc, int cmd, void *data, size_t len)
{
	return sc->sc_proto_ops->proto_set_dcmd(sc, 0, cmd, data, len);
}

int
bwfm_fwvar_cmd_get_int(struct bwfm_softc *sc, int cmd, uint32_t *data)
{
	int ret;
	ret = bwfm_fwvar_cmd_get_data(sc, cmd, data, sizeof(*data));
	*data = letoh32(*data);
	return ret;
}

int
bwfm_fwvar_cmd_set_int(struct bwfm_softc *sc, int cmd, uint32_t data)
{
	data = htole32(data);
	return bwfm_fwvar_cmd_set_data(sc, cmd, &data, sizeof(data));
}

int
bwfm_fwvar_var_get_data(struct bwfm_softc *sc, char *name, void *data, size_t len)
{
	char *buf;
	int ret;

	buf = malloc(strlen(name) + 1 + len, M_TEMP, M_WAITOK);
	memcpy(buf, name, strlen(name) + 1);
	memcpy(buf + strlen(name) + 1, data, len);
	ret = bwfm_fwvar_cmd_get_data(sc, BWFM_C_GET_VAR,
	    buf, strlen(name) + 1 + len);
	memcpy(data, buf, len);
	free(buf, M_TEMP, strlen(name) + 1 + len);
	return ret;
}

int
bwfm_fwvar_var_set_data(struct bwfm_softc *sc, char *name, void *data, size_t len)
{
	char *buf;
	int ret;

	buf = malloc(strlen(name) + 1 + len, M_TEMP, M_WAITOK);
	memcpy(buf, name, strlen(name) + 1);
	memcpy(buf + strlen(name) + 1, data, len);
	ret = bwfm_fwvar_cmd_set_data(sc, BWFM_C_SET_VAR,
	    buf, strlen(name) + 1 + len);
	free(buf, M_TEMP, strlen(name) + 1 + len);
	return ret;
}

int
bwfm_fwvar_var_get_int(struct bwfm_softc *sc, char *name, uint32_t *data)
{
	int ret;
	ret = bwfm_fwvar_var_get_data(sc, name, data, sizeof(*data));
	*data = letoh32(*data);
	return ret;
}

int
bwfm_fwvar_var_set_int(struct bwfm_softc *sc, char *name, uint32_t data)
{
	data = htole32(data);
	return bwfm_fwvar_var_set_data(sc, name, &data, sizeof(data));
}

/* 802.11 code */
void
bwfm_connect(struct bwfm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct bwfm_ext_join_params *params;
	uint8_t buf[64];	/* XXX max WPA/RSN/WMM IE length */
	uint8_t *frm;

	/*
	 * OPEN: Open or WPA/WPA2 on newer Chips/Firmware.
	 * SHARED KEY: WEP.
	 * AUTO: Automatic, probably for older Chips/Firmware.
	 */
	if (ic->ic_flags & IEEE80211_F_RSNON) {
		uint32_t wsec = 0;
		uint32_t wpa = 0;

		/* tell firmware to add WPA/RSN IE to (re)assoc request */
		if (ic->ic_bss->ni_rsnprotos == IEEE80211_PROTO_RSN)
			frm = ieee80211_add_rsn(buf, ic, ic->ic_bss);
		else
			frm = ieee80211_add_wpa(buf, ic, ic->ic_bss);
		bwfm_fwvar_var_set_data(sc, "wpaie", buf, frm - buf);

		if (ic->ic_rsnprotos & IEEE80211_PROTO_WPA) {
			if (ic->ic_rsnakms & IEEE80211_AKM_PSK)
				wpa |= BWFM_WPA_AUTH_WPA_PSK;
			if (ic->ic_rsnakms & IEEE80211_AKM_8021X)
				wpa |= BWFM_WPA_AUTH_WPA_UNSPECIFIED;
		}
		if (ic->ic_rsnprotos & IEEE80211_PROTO_RSN) {
			if (ic->ic_rsnakms & IEEE80211_AKM_PSK)
				wpa |= BWFM_WPA_AUTH_WPA2_PSK;
			if (ic->ic_rsnakms & IEEE80211_AKM_SHA256_PSK)
				wpa |= BWFM_WPA_AUTH_WPA2_PSK_SHA256;
			if (ic->ic_rsnakms & IEEE80211_AKM_8021X)
				wpa |= BWFM_WPA_AUTH_WPA2_UNSPECIFIED;
			if (ic->ic_rsnakms & IEEE80211_AKM_SHA256_8021X)
				wpa |= BWFM_WPA_AUTH_WPA2_1X_SHA256;
		}
		if (ic->ic_rsnciphers & IEEE80211_WPA_CIPHER_TKIP ||
		    ic->ic_rsngroupcipher & IEEE80211_WPA_CIPHER_TKIP)
			wsec |= BWFM_WSEC_TKIP;
		if (ic->ic_rsnciphers & IEEE80211_WPA_CIPHER_CCMP ||
		    ic->ic_rsngroupcipher & IEEE80211_WPA_CIPHER_CCMP)
			wsec |= BWFM_WSEC_AES;

		bwfm_fwvar_var_set_int(sc, "wpa_auth", wpa);
		bwfm_fwvar_var_set_int(sc, "wsec", wsec);
	} else {
		bwfm_fwvar_var_set_int(sc, "wpa_auth", BWFM_WPA_AUTH_DISABLED);
		bwfm_fwvar_var_set_int(sc, "wsec", BWFM_WSEC_NONE);
	}
	bwfm_fwvar_var_set_int(sc, "auth", BWFM_AUTH_OPEN);
	bwfm_fwvar_var_set_int(sc, "mfp", BWFM_MFP_NONE);

	if (ic->ic_des_esslen && ic->ic_des_esslen < BWFM_MAX_SSID_LEN) {
		params = malloc(sizeof(*params), M_TEMP, M_WAITOK | M_ZERO);
		memcpy(params->ssid.ssid, ic->ic_des_essid, ic->ic_des_esslen);
		params->ssid.len = htole32(ic->ic_des_esslen);
		memcpy(params->assoc.bssid, ic->ic_bss->ni_bssid,
		    sizeof(params->assoc.bssid));
		params->scan.scan_type = -1;
		params->scan.nprobes = htole32(-1);
		params->scan.active_time = htole32(-1);
		params->scan.passive_time = htole32(-1);
		params->scan.home_time = htole32(-1);
		if (bwfm_fwvar_var_set_data(sc, "join", params, sizeof(*params))) {
			struct bwfm_join_params join;
			memset(&join, 0, sizeof(join));
			memcpy(join.ssid.ssid, ic->ic_des_essid,
			    ic->ic_des_esslen);
			join.ssid.len = htole32(ic->ic_des_esslen);
			memcpy(join.assoc.bssid, ic->ic_bss->ni_bssid,
			    sizeof(join.assoc.bssid));
			bwfm_fwvar_cmd_set_data(sc, BWFM_C_SET_SSID, &join,
			    sizeof(join));
		}
		free(params, M_TEMP, sizeof(*params));
	}
}

void
bwfm_scan(struct bwfm_softc *sc)
{
	struct bwfm_escan_params *params;
	uint32_t nssid = 0, nchannel = 0;
	size_t params_size;

	params_size = sizeof(*params);
	params_size += sizeof(uint32_t) * ((nchannel + 1) / 2);
	params_size += sizeof(struct bwfm_ssid) * nssid;

	params = malloc(params_size, M_TEMP, M_WAITOK | M_ZERO);
	memset(params->scan_params.bssid, 0xff,
	    sizeof(params->scan_params.bssid));
	params->scan_params.bss_type = 2;
	params->scan_params.scan_type = BWFM_SCANTYPE_PASSIVE;
	params->scan_params.nprobes = htole32(-1);
	params->scan_params.active_time = htole32(-1);
	params->scan_params.passive_time = htole32(-1);
	params->scan_params.home_time = htole32(-1);
	params->version = htole32(BWFM_ESCAN_REQ_VERSION);
	params->action = htole16(WL_ESCAN_ACTION_START);
	params->sync_id = htole16(0x1234);

#if 0
	/* Scan a specific channel */
	params->scan_params.channel_list[0] = htole16(
	    (1 & 0xff) << 0 |
	    (3 & 0x3) << 8 |
	    (2 & 0x3) << 10 |
	    (2 & 0x3) << 12
	    );
	params->scan_params.channel_num = htole32(
	    (1 & 0xffff) << 0
	    );
#endif

	bwfm_fwvar_var_set_data(sc, "escan", params, params_size);
	free(params, M_TEMP, params_size);
}

void
bwfm_rx(struct bwfm_softc *sc, char *buf, size_t len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct bwfm_event *e = (void *)buf;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf *m;
	char *mb;

	if (len >= sizeof(e->ehdr) &&
	    ntohs(e->ehdr.ether_type) == BWFM_ETHERTYPE_LINK_CTL &&
	    memcmp(BWFM_BRCM_OUI, e->hdr.oui, sizeof(e->hdr.oui)) == 0 &&
	    ntohs(e->hdr.usr_subtype) == BWFM_BRCM_SUBTYPE_EVENT)
		bwfm_rx_event(sc, buf, len);

	if (__predict_false(len > MCLBYTES))
		return;
	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (__predict_false(m == NULL))
		return;
	if (len > MHLEN) {
		MCLGET(m, M_DONTWAIT);
		if (!(m->m_flags & M_EXT)) {
			m_free(m);
			return;
		}
	}
	mb = mtod(m, char *);
	memcpy(mb, buf, len);
	m->m_pkthdr.len = m->m_len = len;

	if ((ic->ic_flags & IEEE80211_F_RSNON) &&
	    len >= sizeof(e->ehdr) &&
	    ntohs(e->ehdr.ether_type) == ETHERTYPE_PAE) {
		ifp->if_ipackets++;
#if NBPFILTER > 0
		if (ifp->if_bpf)
			bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_IN);
#endif
		ieee80211_eapol_key_input(ic, m, ic->ic_bss);
	} else {
		ml_enqueue(&ml, m);
		if_input(ifp, &ml);
	}
}

void
bwfm_rx_event(struct bwfm_softc *sc, char *buf, size_t len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct bwfm_event *e = (void *)buf;

	if (ntohl(e->msg.event_type) >= BWFM_E_LAST)
		return;

	switch (ntohl(e->msg.event_type)) {
	case BWFM_E_ESCAN_RESULT: {
		struct bwfm_escan_results *res = (void *)(buf + sizeof(*e));
		struct bwfm_bss_info *bss;
		int i;
		if (ntohl(e->msg.status) != BWFM_E_STATUS_PARTIAL) {
			ieee80211_end_scan(ifp);
			break;
		}
		len -= sizeof(*e);
		if (len < sizeof(*res) || len < letoh32(res->buflen)) {
			printf("%s: results too small\n", DEVNAME(sc));
			return;
		}
		len -= sizeof(*res);
		if (len < letoh16(res->bss_count) * sizeof(struct bwfm_bss_info)) {
			printf("%s: results too small\n", DEVNAME(sc));
			return;
		}
		bss = &res->bss_info[0];
		for (i = 0; i < letoh16(res->bss_count); i++) {
			bwfm_scan_node(sc, &res->bss_info[i], len);
			len -= sizeof(*bss) + letoh32(bss->length);
			bss = (void *)((char *)bss) + letoh32(bss->length);
			if (len <= 0)
				break;
		}
		break;
		}
	case BWFM_E_SET_SSID:
		if (ntohl(e->msg.status) == BWFM_E_STATUS_SUCCESS)
			ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
		else
			ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		break;
	case BWFM_E_ASSOC:
		if (ntohl(e->msg.status) == BWFM_E_STATUS_SUCCESS)
			ieee80211_new_state(ic, IEEE80211_S_ASSOC, -1);
		else
			ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		break;
	case BWFM_E_LINK:
		if (ntohl(e->msg.status) == BWFM_E_STATUS_SUCCESS &&
		    ntohl(e->msg.reason) == 0)
			break;
		/* Link status has changed */
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		break;
	default:
		printf("%s: buf %p len %lu datalen %u code %u status %u"
		    " reason %u\n", __func__, buf, len, ntohl(e->msg.datalen),
		    ntohl(e->msg.event_type), ntohl(e->msg.status),
		    ntohl(e->msg.reason));
		break;
	}
}

void
bwfm_scan_node(struct bwfm_softc *sc, struct bwfm_bss_info *bss, size_t len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_rxinfo rxi;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct mbuf *m;
	uint32_t pktlen, ieslen;
	uint16_t iesoff;

	iesoff = letoh16(bss->ie_offset);
	ieslen = letoh32(bss->ie_length);
	if (ieslen > len - iesoff)
		return;

	/* Build a fake beacon frame to let net80211 do all the parsing. */
	pktlen = sizeof(*wh) + ieslen + 12;
	if (__predict_false(pktlen > MCLBYTES))
		return;
	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (__predict_false(m == NULL))
		return;
	if (pktlen > MHLEN) {
		MCLGET(m, M_DONTWAIT);
		if (!(m->m_flags & M_EXT)) {
			m_free(m);
			return;
		}
	}
	wh = mtod(m, struct ieee80211_frame *);
	wh->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_MGT |
	    IEEE80211_FC0_SUBTYPE_BEACON;
	wh->i_fc[1] = IEEE80211_FC1_DIR_NODS;
	*(uint16_t *)wh->i_dur = 0;
	IEEE80211_ADDR_COPY(wh->i_addr1, etherbroadcastaddr);
	IEEE80211_ADDR_COPY(wh->i_addr2, bss->bssid);
	IEEE80211_ADDR_COPY(wh->i_addr3, bss->bssid);
	*(uint16_t *)wh->i_seq = 0;
	memset(&wh[1], 0, 12);
	((uint16_t *)(&wh[1]))[4] = bss->beacon_period;
	((uint16_t *)(&wh[1]))[5] = bss->capability;
	memcpy(((uint8_t *)&wh[1]) + 12, ((uint8_t *)bss) + iesoff, ieslen);

	/* Finalize mbuf. */
	m->m_pkthdr.len = m->m_len = pktlen;
	ni = ieee80211_find_rxnode(ic, wh);
	rxi.rxi_flags = 0;
	rxi.rxi_rssi = letoh32(bss->rssi);
	rxi.rxi_tstamp = 0;
	ieee80211_input(ifp, m, ni, &rxi);
	/* Node is no longer needed. */
	ieee80211_release_node(ic, ni);
}

void
bwfm_task(void *arg)
{
	struct bwfm_softc *sc = arg;
	struct bwfm_host_cmd_ring *ring = &sc->sc_cmdq;
	struct bwfm_host_cmd *cmd;
	int s;

	s = splsoftnet();
	while (ring->next != ring->cur) {
		cmd = &ring->cmd[ring->next];
		splx(s);
		cmd->cb(sc, cmd->data);
		s = splsoftnet();
		ring->queued--;
		ring->next = (ring->next + 1) % BWFM_HOST_CMD_RING_COUNT;
	}
	splx(s);
}

void
bwfm_do_async(struct bwfm_softc *sc,
    void (*cb)(struct bwfm_softc *, void *), void *arg, int len)
{
	struct bwfm_host_cmd_ring *ring = &sc->sc_cmdq;
	struct bwfm_host_cmd *cmd;
	int s;

	s = splsoftnet();
	cmd = &ring->cmd[ring->cur];
	cmd->cb = cb;
	KASSERT(len <= sizeof(cmd->data));
	memcpy(cmd->data, arg, len);
	ring->cur = (ring->cur + 1) % BWFM_HOST_CMD_RING_COUNT;
	task_add(sc->sc_taskq, &sc->sc_task);
	splx(s);
}

int
bwfm_send_mgmt(struct ieee80211com *ic, struct ieee80211_node *ni,
    int type, int arg1, int arg2)
{
#ifdef BWFM_DEBUG
	struct bwfm_softc *sc = ic->ic_softc;
	DPRINTF(("%s: %s\n", DEVNAME(sc), __func__));
#endif
	return 0;
}

int
bwfm_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct bwfm_softc *sc = ic->ic_softc;
	struct bwfm_cmd_key cmd;

	cmd.ni = ni;
	cmd.k = k;
	bwfm_do_async(sc, bwfm_set_key_cb, &cmd, sizeof(cmd));
	return 0;
}

void
bwfm_set_key_cb(struct bwfm_softc *sc, void *arg)
{
	struct bwfm_cmd_key *cmd = arg;
	struct ieee80211_key *k = cmd->k;
	struct ieee80211_node *ni = cmd->ni;
	struct bwfm_wsec_key key;
	uint32_t wsec, wsec_enable;
	int ext_key = 0;

	if ((k->k_flags & IEEE80211_KEY_GROUP) == 0 &&
	    k->k_cipher != IEEE80211_CIPHER_WEP40 &&
	    k->k_cipher != IEEE80211_CIPHER_WEP104)
		ext_key = 1;

	memset(&key, 0, sizeof(key));
	if (ext_key && !IEEE80211_IS_MULTICAST(ni->ni_bssid))
		memcpy(key.ea, ni->ni_bssid, sizeof(key.ea));
	key.index = htole32(k->k_id);
	key.len = htole32(k->k_len);
	memcpy(key.data, k->k_key, sizeof(key.data));
	if (!ext_key)
		key.flags = htole32(BWFM_WSEC_PRIMARY_KEY);

	switch (k->k_cipher) {
	case IEEE80211_CIPHER_WEP40:
		key.algo = htole32(BWFM_CRYPTO_ALGO_WEP1);
		wsec_enable = BWFM_WSEC_WEP;
		break;
	case IEEE80211_CIPHER_WEP104:
		key.algo = htole32(BWFM_CRYPTO_ALGO_WEP128);
		wsec_enable = BWFM_WSEC_WEP;
		break;
	case IEEE80211_CIPHER_TKIP:
		key.algo = htole32(BWFM_CRYPTO_ALGO_TKIP);
		wsec_enable = BWFM_WSEC_TKIP;
		break;
	case IEEE80211_CIPHER_CCMP:
		key.algo = htole32(BWFM_CRYPTO_ALGO_AES_CCM);
		wsec_enable = BWFM_WSEC_AES;
		break;
	default:
		printf("%s: cipher %x not supported\n", DEVNAME(sc),
		    k->k_cipher);
		return;
	}

	bwfm_fwvar_var_set_data(sc, "wsec_key", &key, sizeof(key));
	bwfm_fwvar_var_get_int(sc, "wsec", &wsec);
	wsec |= wsec_enable;
	bwfm_fwvar_var_set_int(sc, "wsec", wsec);
}

void
bwfm_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct bwfm_softc *sc = ic->ic_softc;
	struct bwfm_cmd_key cmd;

	cmd.ni = ni;
	cmd.k = k;
	bwfm_do_async(sc, bwfm_delete_key_cb, &cmd, sizeof(cmd));
}

void
bwfm_delete_key_cb(struct bwfm_softc *sc, void *arg)
{
	struct bwfm_cmd_key *cmd = arg;
	struct ieee80211_key *k = cmd->k;
	struct bwfm_wsec_key key;

	memset(&key, 0, sizeof(key));
	key.index = htole32(k->k_id);
	key.flags = htole32(BWFM_WSEC_PRIMARY_KEY);
	bwfm_fwvar_var_set_data(sc, "wsec_key", &key, sizeof(key));
}

int
bwfm_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct bwfm_softc *sc = ic->ic_softc;
	struct bwfm_cmd_newstate cmd;

	cmd.state = nstate;
	cmd.arg = arg;
	bwfm_do_async(sc, bwfm_newstate_cb, &cmd, sizeof(cmd));
	return 0;
}

void
bwfm_newstate_cb(struct bwfm_softc *sc, void *arg)
{
	struct bwfm_cmd_newstate *cmd = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	enum ieee80211_state ostate, nstate;
	int s;

	s = splnet();
	ostate = ic->ic_state;
	nstate = cmd->state;
	DPRINTF(("newstate %s -> %s\n",
	    ieee80211_state_name[ostate],
	    ieee80211_state_name[nstate]));

	switch (nstate) {
	case IEEE80211_S_SCAN:
		bwfm_scan(sc);
		ic->ic_state = nstate;
		splx(s);
		return;
	case IEEE80211_S_AUTH:
		ic->ic_bss->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;
		bwfm_connect(sc);
		ic->ic_state = cmd->state;
		if (ic->ic_flags & IEEE80211_F_RSNON)
			ic->ic_bss->ni_rsn_supp_state = RSNA_SUPP_PTKSTART;
		splx(s);
		return;
	default:
		break;
	}
	sc->sc_newstate(ic, nstate, cmd->arg);
	splx(s);
	return;
}
