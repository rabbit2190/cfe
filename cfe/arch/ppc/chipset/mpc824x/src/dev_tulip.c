/*  *********************************************************************
    *  Broadcom Common Firmware Environment (CFE)
    *
    *  DC21x4x Ethernet Driver			File: dev_tulip.c
    *  
    *  Author:  Ed Satterthwaite
    *  
    *********************************************************************  
    *
    *  Copyright 2000,2001,2002,2003
    *  Broadcom Corporation. All rights reserved.
    *  
    *  This software is furnished under license and may be used and 
    *  copied only in accordance with the following terms and 
    *  conditions.  Subject to these conditions, you may download, 
    *  copy, install, use, modify and distribute modified or unmodified 
    *  copies of this software in source and/or binary form.  No title 
    *  or ownership is transferred hereby.
    *  
    *  1) Any source code used, modified or distributed must reproduce 
    *     and retain this copyright notice and list of conditions 
    *     as they appear in the source file.
    *  
    *  2) No right is granted to use any trade name, trademark, or 
    *     logo of Broadcom Corporation.  The "Broadcom Corporation" 
    *     name may not be used to endorse or promote products derived 
    *     from this software without the prior written permission of 
    *     Broadcom Corporation.
    *  
    *  3) THIS SOFTWARE IS PROVIDED "AS-IS" AND ANY EXPRESS OR
    *     IMPLIED WARRANTIES, INCLUDING BUT NOT LIMITED TO, ANY IMPLIED
    *     WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR 
    *     PURPOSE, OR NON-INFRINGEMENT ARE DISCLAIMED. IN NO EVENT 
    *     SHALL BROADCOM BE LIABLE FOR ANY DAMAGES WHATSOEVER, AND IN 
    *     PARTICULAR, BROADCOM SHALL NOT BE LIABLE FOR DIRECT, INDIRECT,
    *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
    *     (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
    *     GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
    *     BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY 
    *     OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR 
    *     TORT (INCLUDING NEGLIGENCE OR OTHERWISE), EVEN IF ADVISED OF 
    *     THE POSSIBILITY OF SUCH DAMAGE.
    ********************************************************************* */

#include "cfe.h"
#include "lib_physio.h"
#ifdef CPUCFG_MEMCPY
#error "this doesn't work yet."
extern void *CPUCFG_MEMCPY(void *dest, const void *src, size_t cnt);
#define blockcopy CPUCFG_MEMCPY
#else
#define blockcopy memcpy
#endif

#include "cfe_irq.h"

#include "net_enet.h"

#include "pcivar.h"
#include "pcireg.h"

#include "tulip.h"
#include "mii.h"

/* This is a driver for specific configurations of the DC21040, DC21041,
   DC21140A and DC21143, not a generic Tulip driver.  The prefix
   "tulip_" is used to indicate generic Tulip functions, while
   "dc21040_", "dc21041_", "dc21140_" or "dc21143_" indicates functions
   specific to a chip variant.

   The 21041 driver assumes a 10BT HD interface, since autonegotiation
   is known to be broken in the early revisons of that chip.  Example
   cards come from DEC and SMC.  Essentially the same driver is used
   for 21040 cards.

   The 21140 driver assumes that the PHY uses a standard MII interface
   for both 100BT and 10BT.  Example cards come from DEC (National DP83840
   plus Twister PHY) and Netgear (Level One PHY).

      Some early 21140 boards are exceptions and use SYM plus SRL
      with different PHY chips for 10 and 100 (limited support).

   The 21143 driver assumes by default that the PHY uses the SYM ("5
   wire") interface for 100BT with pass-through for 10BT.  Example
   cards come from DEC (MicroLinear ML6694 PHY) and Znyx (QS6611 or
   Kendin KS8761 PHY).  It also supports an MII interface for
   recognized adapters.  An example card comes from Adaptec (National
   DP83840A and Twister PHY).  There is no support for AUI interfaces.

   The Davicom 9102(A) is a chip with an integrated PHY that is almost
   compatible with the 21140 and is in current production.

   This SB1250 version takes advantage of DMA coherence and uses
   "preserve bit lanes" addresses for all accesses that cross the
   ZBbus-PCI bridge.  */

#ifndef TULIP_DEBUG
#define TULIP_DEBUG 0
#endif

#if ((ENDIAN_BIG + ENDIAN_LITTLE) != 1)
#error "dev_tulip: system endian not set"
#endif

/* Temporary, until configs supply MATCH_BYTES */
#if defined(_MOUSSE_) || defined(_BCM98245CPCI_)  /* any PPC */
#define MATCH_BYTES  1
#else
#define MATCH_BYTES  0
#endif

/* Set IPOLL to drive processing through the pseudo-interrupt
   dispatcher.  Set XPOLL to drive processing by an external polling
   agent.  Setting both is ok. */

#ifndef IPOLL
#define IPOLL 0
#endif
#ifndef XPOLL
#define XPOLL 1
#endif

#define MAX_ETHER_PACK  (ENET_MAX_PKT+ENET_CRC_SIZE)   /* size of max packet */

/* Packet buffers.  For tulip, the packet must be aligned to a 32-bit
   word boundary, and we would like it aligned to a cache line
   boundary for performance. */

#define CACHE_ALIGN      32

typedef struct eth_pkt_s {
    queue_t next;			/*  8 */
    uint8_t *buffer;			/*  4 */
    uint32_t flags;			/*  4 */
    int32_t length;			/*  4 */
    uint32_t unused[3];			/* 12 */
    uint8_t data[MAX_ETHER_PACK];
} eth_pkt_t;

#define ETH_PKTBUF_LINES  ((sizeof(eth_pkt_t) + (CACHE_ALIGN-1))/CACHE_ALIGN)
#define ETH_PKTBUF_SIZE   (ETH_PKTBUF_LINES*CACHE_ALIGN)
#define ETH_PKTBUF_OFFSET (offsetof(eth_pkt_t, data))

#define ETH_PKT_BASE(data) ((eth_pkt_t *)((data) - ETH_PKTBUF_OFFSET))

/* packet flags */
#define ETH_TX_SETUP	 1     /* assumes Perfect Filtering format */

static void
show_packet(char c, eth_pkt_t *pkt)
{
    int i;
    int n = (pkt->length < 32 ? pkt->length : 32);

    xprintf("%c[%4d]:", c, pkt->length);
    for (i = 0; i < n; i++) {
	if (i % 4 == 0)
	    xprintf(" ");
	xprintf("%02x", pkt->buffer[i]);
	}
    xprintf("\n");
}


/* Descriptor structures */

typedef struct rx_dscr {
    uint32_t   rxd_flags;
    uint32_t   rxd_bufsize;
    pci_addr_t rxd_bufaddr1;
    pci_addr_t rxd_bufaddr2;
} rx_dscr;
	
typedef struct tx_dscr {
    uint32_t   txd_flags;
    uint32_t   txd_bufsize;
    pci_addr_t txd_bufaddr1;
    pci_addr_t txd_bufaddr2;
} tx_dscr;

/* CAM structure */

typedef union {
    struct {
	uint32_t physical[CAM_PERFECT_ENTRIES][3];
    } p;
    struct {
	uint32_t hash[32];
	uint32_t mbz[7];
	uint32_t physical[3];
    } h;
} tulip_cam;


/* Driver data structures */

typedef enum {
    K_CHIP_UNKNOWN,
    K_CHIP_DC21040,
    K_CHIP_DC21041,
    K_CHIP_DC21140,
    K_CHIP_DC21143,
    K_CHIP_DM9102,
    K_CHIP_82C169
} tulip_chip_t;

typedef enum {
    eth_state_uninit,
    eth_state_setup,
    eth_state_off,
    eth_state_on, 
    eth_state_broken
} eth_state_t;

#define ETH_PKTPOOL_SIZE 32
#define ETH_PKT_SIZE	 MAX_ETHER_PACK

typedef struct tulip_softc_s tulip_softc;

struct tulip_softc_s {
    uint32_t membase;
    uint8_t irq;		/* interrupt mapping (used if IPOLL) */
    pcitag_t tag;               /* tag for configuration registers */

    uint8_t hwaddr[ENET_ADDR_LEN];

    uint16_t device;            /* chip device code */
    uint8_t revision;		/* chip revision and step (Table 3-7) */

    /* current state */
    eth_state_t state;

    /* These fields are the chip startup values. */
//  uint16_t media;		/* media type */
    uint32_t opmode;            /* operating mode */
    uint32_t intmask;           /* interrupt mask */
    uint32_t gpdata;            /* output bits for csr15 (21143) */

    /* These fields are the summary of byte swapping requirements */
    int      swap;              /* Swap bytes in descriptors */
    uint32_t endian;            /* Prefix bits for byte lane swapping */

    /* These fields are set before calling dc21x4x_hwinit */
    int linkspeed;		/* encodings from cfe_ioctl */
    int loopback;

    /* Packet free list */
    queue_t freelist;
    uint8_t *pktpool;
    queue_t rxqueue;

    /* The descriptor tables */
    uint8_t    *rxdscrmem;	/* receive descriptors */
    uint8_t    *txdscrmem;	/* transmit descriptors */

    /* These fields keep track of where we are in tx/rx processing */
    volatile rx_dscr *rxdscr_start;	/* beginning of ring */
    volatile rx_dscr *rxdscr_end;	/* end of ring */
    volatile rx_dscr *rxdscr_remove;	/* next one we expect tulip to use */
    volatile rx_dscr *rxdscr_add;	/* next place to put a buffer */
    int      rxdscr_onring;

    volatile tx_dscr *txdscr_start;	/* beginning of ring */
    volatile tx_dscr *txdscr_end;	/* end of ring */
    volatile tx_dscr *txdscr_remove;	/* next one we will use for tx */
    volatile tx_dscr *txdscr_add;	/* next place to put a buffer */

    /* The descriptor options */
    uint32_t rxdscr_owned;       /* RDES0_OWNADAP in proper byte order */
    uint32_t rxdscr_ctrl;        /* RDES1 control bits (chaining) */
    uint32_t txdscr_owned;       /* TDES0 OWNADAP in proper byte order */
    uint32_t txdscr_ctrl;        /* TDES1 control bits */
    
    cfe_devctx_t *devctx;

    /* These fields describe the PHY */
    enum {NONE, SRL, MII, SYM} phy_type;
    int phy_addr;
    uint32_t phy_vendor;
    uint16_t phy_device;
    uint16_t (*mii_read_register)(tulip_softc *sc, unsigned int index);
    void     (*mii_write_register)(tulip_softc *sc, unsigned int index,
				   uint16_t value);

    /* Statistics */
    uint32_t inpkts;
    uint32_t outpkts;
    uint32_t interrupts;
    uint32_t rx_interrupts;
    uint32_t tx_interrupts;
    uint32_t bus_errors;
};


/* Entry to and exit from critical sections (currently relative to
   interrupts only, not SMP) */

#if CFG_INTERRUPTS
#define CS_ENTER(sc) cfe_disable_irq(sc->irq)
#define CS_EXIT(sc)  cfe_enable_irq(sc->irq)
#else
#define CS_ENTER(sc) ((void)0)
#define CS_EXIT(sc)  ((void)0)
#endif


/* Driver parameterization */

#define MAXRXDSCR      32
#define MAXTXDSCR      32
#define MINRXRING	8

#define MEDIA_UNKNOWN           0
#define MEDIA_AUI               1
#define MEDIA_BNC               2
#define MEDIA_UTP_FULL_DUPLEX   3
#define MEDIA_UTP_NO_LINK_TEST  4
#define MEDIA_UTP               5

/* Prototypes */

static void tulip_ether_probe(cfe_driver_t *drv,
			      unsigned long probe_a, unsigned long probe_b, 
			      void *probe_ptr);


/* Byte swap utilities. */

#if ENDIAN_BIG

static uint32_t
swap4(uint32_t x)
{
    uint32_t t;

    t = ((x & 0xFF00FF00) >> 8) | ((x & 0x00FF00FF) << 8);
    return (t >> 16) | ((t & 0xFFFF) << 16);
}

#define DMA_SWAP(sc,x) ((sc)->swap ? swap4(x) : (x))
#else
#define DMA_SWAP(sc,x) (x)
#endif


/* Address mapping macros */

/* Byte lane swapping is controlled by prefix bits of the PCI address. */
#define M_ENDIAN_BIT      0x20000000
#define M_MATCH_BITS      (M_ENDIAN_BIT)
#define M_MATCH_BYTES     (0x00000000)

/* Note that PTR_TO_PHYS only works with 32-bit addresses, but then
   so does the Tulip. */
#define PTR_TO_PHYS(x) (PHYSADDR((uintptr_t)(x)))
#define PHYS_TO_PTR(a) ((uint8_t *)KERNADDR(a))

/* Not all tulip clones can do big-endian DMA, so we cannot use the
   default definitions of PHYS_TO_PCI and PCI_TO_PHYS, which assume
   that capability. */
#undef PHYS_TO_PCI
#undef PCI_TO_PHYS
#define PHYS_TO_PCI(sc,a) ((uint32_t) (a) | (sc)->endian)
#define PCI_TO_PHYS(sc,a) ((uint32_t) (a) & ~M_ENDIAN_BIT)

#define PCI_TO_PTR(sc,a)  (PHYS_TO_PTR(PCI_TO_PHYS((sc),(a))))
#define PTR_TO_PCI(sc,x)  (PHYS_TO_PCI((sc),PTR_TO_PHYS(x)))

#if (ENDIAN_BIG && MATCH_BYTES)
#define CSR_MATCH_MODE       PCI_MATCH_BYTES
#define READCSR(sc,csr)      (phys_read32_swapped((sc)->membase + (csr)))
#define WRITECSR(sc,csr,val) (phys_write32_swapped((sc)->membase + (csr), (val)))
#else
#define CSR_MATCH_MODE       PCI_MATCH_BITS
#define READCSR(sc,csr)      (phys_read32((sc)->membase + (csr)))
#define WRITECSR(sc,csr,val) (phys_write32((sc)->membase + (csr), (val)))
#endif

#define RESET_ADAPTER(sc)				\
	{						\
	WRITECSR((sc), R_CSR_BUSMODE, M_CSR0_SWRESET);	\
	cfe_sleep(CFE_HZ/10);				\
	}


/* Debugging */

static void
dumpstat(tulip_softc *sc)
{
    xprintf("-- CSR 5 = %08X  CSR 6 = %08x\n",
	    READCSR(sc, R_CSR_STATUS), READCSR(sc, R_CSR_OPMODE));
}

static void
dumpcsrs(tulip_softc *sc)
{
    int idx;

    xprintf("-------------\n");
    for (idx = 0; idx < 16; idx++) {
	xprintf("CSR %2d = %08X\n", idx, READCSR(sc, idx*8));
	}
    xprintf("-------------\n");

}


/* Packet management */

/*  *********************************************************************
    *  ETH_ALLOC_PKT(sc)
    *  
    *  Allocate a packet from the free list.
    *  
    *  Input parameters: 
    *  	   sc - eth structure
    *  	   
    *  Return value:
    *  	   pointer to packet structure, or NULL if none available
    ********************************************************************* */
static eth_pkt_t *
eth_alloc_pkt(tulip_softc *sc)
{
    eth_pkt_t *pkt;

    CS_ENTER(sc);
    pkt = (eth_pkt_t *) q_deqnext(&sc->freelist);
    CS_EXIT(sc);
    if (!pkt) return NULL;

    pkt->buffer = pkt->data;
    pkt->length = ETH_PKT_SIZE;
    pkt->flags = 0;

    return pkt;
}


/*  *********************************************************************
    *  ETH_FREE_PKT(sc,pkt)
    *  
    *  Return a packet to the free list
    *  
    *  Input parameters: 
    *  	   sc - sbmac structure
    *  	   pkt - packet to return
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */
static void
eth_free_pkt(tulip_softc *sc, eth_pkt_t *pkt)
{
    CS_ENTER(sc);
    q_enqueue(&sc->freelist, &pkt->next);
    CS_EXIT(sc);
}


/*  *********************************************************************
    *  ETH_INITFREELIST(sc)
    *  
    *  Initialize the buffer free list for this mac.  The memory
    *  allocated to the free list is carved up and placed on a linked
    *  list of buffers for use by the mac.
    *  
    *  Input parameters: 
    *  	   sc - eth structure
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */
static void
eth_initfreelist(tulip_softc *sc)
{
    int idx;
    uint8_t *ptr;
    eth_pkt_t *pkt;

    q_init(&sc->freelist);

    ptr = sc->pktpool;
    for (idx = 0; idx < ETH_PKTPOOL_SIZE; idx++) {
	pkt = (eth_pkt_t *) ptr;
	eth_free_pkt(sc, pkt);
	ptr += ETH_PKTBUF_SIZE;
	}
}


/* Utilities */

static const char *
tulip_devname(tulip_softc *sc)
{
    return (sc->devctx != NULL ? cfe_device_name(sc->devctx) : "eth?");
}


/* Descriptor ring management */

static int
tulip_add_rcvbuf(tulip_softc *sc, eth_pkt_t *pkt)
{
    volatile rx_dscr *rxd;
    volatile rx_dscr *nextrxd;
    uint32_t ctrl;

    rxd = sc->rxdscr_add;
    ctrl = sc->rxdscr_ctrl;

    /* Figure out where the next descriptor will go */
    nextrxd = rxd+1;
    if (nextrxd == sc->rxdscr_end) {
	nextrxd = sc->rxdscr_start;
	ctrl = M_RDES1_ENDOFRING;
	}

    /* If the next one is the same as our remove pointer, the ring is
       considered full.  */
    if (nextrxd == sc->rxdscr_remove) return -1;

    rxd->rxd_bufsize  = DMA_SWAP(sc, V_RDES1_BUF1SIZE(1520) | ctrl);
    rxd->rxd_bufaddr1 = DMA_SWAP(sc, PTR_TO_PCI(sc,pkt->buffer));
    rxd->rxd_flags    = sc->rxdscr_owned;

    /* success, advance the pointer */
    sc->rxdscr_add = nextrxd;

    return 0;
}

static void
tulip_fillrxring(tulip_softc *sc)
{
    eth_pkt_t *pkt;

    CS_ENTER(sc);
    while (1) {
	if (sc->rxdscr_onring >= MINRXRING) {
	    CS_EXIT(sc);
	    break;
	    }
	CS_EXIT(sc);
	pkt = eth_alloc_pkt(sc);
	if (pkt == NULL) {
	    /* could not allocate a buffer */
	    break;
	    }
	if (tulip_add_rcvbuf(sc, pkt) != 0) {
	    /* could not add buffer to ring */
	    eth_free_pkt(sc, pkt);
	    break;
	    }
	CS_ENTER(sc);
	sc->rxdscr_onring++;
	}
}


/*  *********************************************************************
    *  TULIP_RX_CALLBACK(sc, pkt)
    *  
    *  Receive callback routine.  This routine is invoked when a
    *  buffer queued for receives is filled. In this simple driver,
    *  all we do is add the packet to a per-MAC queue for later
    *  processing, and try to put a new packet in the place of the one
    *  that was removed from the queue.
    *  
    *  Input parameters: 
    *  	   sc - interface
    *  	   ptk - packet context (eth_pkt structure)
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */
static void
tulip_rx_callback(tulip_softc *sc, eth_pkt_t *pkt)
{
    if (TULIP_DEBUG) show_packet('>', pkt);   /* debug */

    CS_ENTER(sc);
    q_enqueue(&sc->rxqueue, &pkt->next);
    CS_EXIT(sc);
    sc->inpkts++;
}


static void
tulip_procrxring(tulip_softc *sc)
{
    volatile rx_dscr *rxd;
    eth_pkt_t *pkt;
    eth_pkt_t *newpkt;
    uint32_t flags;

    for (;;) {
	rxd = sc->rxdscr_remove;

	flags = rxd->rxd_flags;
	if (flags & sc->rxdscr_owned) {
	    /* end of ring, no more packets */
	    break;
	    }
	flags = DMA_SWAP(sc, flags);

	pkt = ETH_PKT_BASE(PCI_TO_PTR(sc, DMA_SWAP(sc, rxd->rxd_bufaddr1)));

	/* Drop error packets */
	if (flags & M_RDES0_ERRORSUM) {
	    xprintf("%s: rx error %04X\n", tulip_devname(sc), flags & 0xFFFF);
	    newpkt = pkt;        /* recycle the buffer */
	    }
	else {
	    /* Pass up the packet */
	    pkt->length = G_RDES0_FRAMELEN(flags) - ENET_CRC_SIZE;
	    tulip_rx_callback(sc, pkt);

	    /* put a buffer back on the ring to replace this one */
	    newpkt = eth_alloc_pkt(sc);
	    }

	/* update the pointer, accounting for buffer wrap. */
	rxd++;
	if (rxd == sc->rxdscr_end)
	    rxd = sc->rxdscr_start;
	sc->rxdscr_remove = rxd;

	if (newpkt) {
	    /* The ring must have space now. */
	    tulip_add_rcvbuf(sc, newpkt);
	    }
	else {
	    CS_ENTER(sc);
	    sc->rxdscr_onring--;
	    CS_EXIT(sc);
	    }
	}
}


static int
tulip_add_txbuf(tulip_softc *sc, eth_pkt_t *pkt)
{
    volatile tx_dscr *txd;
    volatile tx_dscr *nexttxd;
    uint32_t bufsize;

    txd = sc->txdscr_add;
    bufsize = sc->txdscr_ctrl;

    /* Figure out where the next descriptor will go */
    nexttxd = (txd+1);
    if (nexttxd == sc->txdscr_end) {
	nexttxd = sc->txdscr_start;
	bufsize = M_TDES1_ENDOFRING;
	}

    /* If the next one is the same as our remove pointer,
       the ring is considered full.  (it actually has room for
       one more, but we reserve the remove == add case for "empty") */

    if (nexttxd == sc->txdscr_remove) return -1;

    bufsize  |= V_TDES1_BUF1SIZE(pkt->length) |
	M_TDES1_FIRSTSEG | M_TDES1_LASTSEG | M_TDES1_INTERRUPT;
    if (pkt->flags & ETH_TX_SETUP) {
        /* For a setup packet, FIRSTSEG and LASTSEG should be clear (!) */
	bufsize ^= M_TDES1_SETUP | M_TDES1_FIRSTSEG | M_TDES1_LASTSEG;
	}
    txd->txd_bufsize  = DMA_SWAP(sc, bufsize);
    txd->txd_bufaddr1 = DMA_SWAP(sc, PTR_TO_PCI(sc, pkt->buffer));
    txd->txd_flags    = sc->txdscr_owned;

    /* success, advance the pointer */
    sc->txdscr_add = nexttxd;

    return 0;
}


static int
tulip_transmit(tulip_softc *sc,eth_pkt_t *pkt)
{
    int rv;

    if (TULIP_DEBUG) show_packet('<', pkt);   /* debug */

    rv = tulip_add_txbuf(sc, pkt);
    sc->outpkts++;

    WRITECSR(sc, R_CSR_TXPOLL, 1);
    return rv;
}


static void
tulip_proctxring(tulip_softc *sc)
{
    volatile tx_dscr *txd;
    eth_pkt_t *pkt;
    uint32_t flags;

    for (;;) {
	txd = sc->txdscr_remove;

	if (txd == sc->txdscr_add) {
	    /* ring is empty, no buffers to process */
	    break;
	    }

	flags = txd->txd_flags;
	if (flags & sc->txdscr_owned) {
	    /* Reached a packet still being transmitted */
	    break;
	    }

	flags = DMA_SWAP(sc, flags);

	/* Check for a completed setup packet */
	pkt = ETH_PKT_BASE(PCI_TO_PTR(sc, DMA_SWAP(sc, txd->txd_bufaddr1)));
	if (pkt->flags & ETH_TX_SETUP) {
	    if (sc->state == eth_state_setup) {
	        uint32_t opmode;

		/* check flag bits */
		opmode = READCSR(sc, R_CSR_OPMODE);
		opmode |= M_CSR6_RXSTART;
		WRITECSR(sc, R_CSR_OPMODE, opmode);
		sc->inpkts = sc->outpkts = 0;
		sc->state = eth_state_on;
		}
	    pkt->flags &=~ ETH_TX_SETUP;
	    }

	/* Just free the packet */
	eth_free_pkt(sc, pkt);

	/* update the pointer, accounting for buffer wrap. */
	txd++;
	if (txd == sc->txdscr_end)
	    txd = sc->txdscr_start;

	sc->txdscr_remove = txd;
	}
}


static void
tulip_initrings(tulip_softc *sc)
{
    volatile tx_dscr *txd, *txn;
    volatile rx_dscr *rxd, *rxn;

    /* Claim ownership of all descriptors for the driver */

    for (txd = sc->txdscr_start; txd != sc->txdscr_end; txd++) {
	txn = txd + 1;
	if (txn == sc->txdscr_end) txn = sc->txdscr_start;
        txd->txd_flags = 0;
	txd->txd_bufsize = DMA_SWAP(sc, sc->txdscr_ctrl);
	if (sc->txdscr_ctrl & M_TDES1_CHAINED)
	    txd->txd_bufaddr2 = DMA_SWAP(sc, PTR_TO_PCI(sc, txn));
	else
	    txd->txd_bufaddr2 = 0;
	}
	
    for (rxd = sc->rxdscr_start; rxd != sc->rxdscr_end; rxd++) {
	rxn = rxd + 1;
	if (rxn == sc->rxdscr_end) rxn = sc->rxdscr_start;
        rxd->rxd_flags = 0;
	rxd->rxd_bufsize = DMA_SWAP(sc, sc->rxdscr_ctrl);
	if (sc->rxdscr_ctrl & M_RDES1_CHAINED)
	    rxd->rxd_bufaddr2 = DMA_SWAP(sc, PTR_TO_PCI(sc, rxn));
	else
	    rxd->rxd_bufaddr2 = 0;
	}

    /* Init the ring pointers */

    sc->txdscr_add = sc->txdscr_remove = sc->txdscr_start;
    sc->rxdscr_add = sc->rxdscr_remove = sc->rxdscr_start;
    sc->rxdscr_onring = 0;

    /* Add stuff to the receive ring */

    tulip_fillrxring(sc);
}


static int
tulip_init(tulip_softc *sc)
{
    /* Allocate descriptor rings */
    sc->rxdscrmem = KMALLOC(MAXRXDSCR*sizeof(rx_dscr), sizeof(rx_dscr));
    sc->txdscrmem = KMALLOC(MAXTXDSCR*sizeof(tx_dscr), sizeof(tx_dscr));

    /* Allocate buffer pool */
    sc->pktpool = KMALLOC(ETH_PKTPOOL_SIZE*ETH_PKTBUF_SIZE, CACHE_ALIGN);
    eth_initfreelist(sc);
    q_init(&sc->rxqueue);

    /* Fill in pointers to the rings */
    sc->rxdscr_start = (volatile rx_dscr *) (sc->rxdscrmem);
    sc->rxdscr_end = sc->rxdscr_start + MAXRXDSCR;
    sc->rxdscr_add = sc->rxdscr_start;
    sc->rxdscr_remove = sc->rxdscr_start;
    sc->rxdscr_onring = 0;

    sc->txdscr_start = (volatile tx_dscr *) (sc->txdscrmem);
    sc->txdscr_end = sc->txdscr_start + MAXTXDSCR;
    sc->txdscr_add = sc->txdscr_start;
    sc->txdscr_remove = sc->txdscr_start;

    tulip_initrings(sc);

    return 0;       
}


static void
tulip_resetrings(tulip_softc *sc)
{
    volatile tx_dscr *txd;
    volatile rx_dscr *rxd;
    eth_pkt_t *pkt;

    /* Free already-sent descriptors and buffers */
    tulip_proctxring(sc);

    /* Free any pending but unsent */
    txd = sc->txdscr_remove;
    while (txd != sc->txdscr_add) {

	txd->txd_flags &=~ sc->txdscr_owned;
	pkt = ETH_PKT_BASE(PCI_TO_PTR(sc, DMA_SWAP(sc, txd->txd_bufaddr1)));
	eth_free_pkt(sc, pkt);

	txd++;
	if (txd == sc->txdscr_end)
	  txd = sc->txdscr_start;
        }
    sc->txdscr_add = sc->txdscr_remove;

    /* Discard any received packets as well as all free buffers */
    rxd = sc->rxdscr_remove;
    while (rxd != sc->rxdscr_add) {
	rxd->rxd_flags &=~ sc->rxdscr_owned;
	pkt = ETH_PKT_BASE(PCI_TO_PTR(sc, DMA_SWAP(sc, rxd->rxd_bufaddr1)));
	eth_free_pkt(sc, pkt);
	
	rxd++;
	if (rxd == sc->rxdscr_end)
	    rxd = sc->rxdscr_start;
	CS_ENTER(sc);
	sc->rxdscr_onring--;
	CS_EXIT(sc);
	}

    /* Reestablish the initial state. */
    tulip_initrings(sc);
}


/* CRCs */

static uint32_t
tulip_crc32(const uint8_t *databuf, unsigned int datalen) 
{       
    unsigned int idx, bit, data;
    uint32_t crc;

    crc = 0xFFFFFFFFUL;
    for (idx = 0; idx < datalen; idx++)
	for (data = *databuf++, bit = 0; bit < 8; bit++, data >>= 1)
	    crc = (crc >> 1) ^ (((crc ^ data) & 1) ? ENET_CRC32_POLY : 0);
    return crc;
}

#define tulip_mchash(mca)       (tulip_crc32((mca), 6) & 0x1FF)
  

/* Serial ROM access */

/*
 * Delays below (nsec) are chosen to meet specs for NS93C46 (slow M variant).
 * Current parts are faster.
 *     Reference:  NS Memory Data Book, 1994
 */

#define SROM_SIZE                128
#define SROM_MAX_CYCLES          32

#define SROM_CMD_BITS            3
#define SROM_ADDR_BITS           6

#define K_SROM_READ_CMD          06
#define K_SROM_WRITE_CMD         05
#define K_SROM_WEN_CMD           04   /* WEN, WDS, also WRAL, ERAL */

#define SROM_VENDOR_INDEX        0x00
#define SROM_FORMAT_INDEX        0x12
#define SROM_ADDR_INDEX          0x14

#define SROM_DEVICE0_INDEX       0x1A
#define SROM_LEAF0_OFFSET_INDEX  0x1B

#define SROM_CRC_INDEX           (SROM_SIZE-2)
/* Note recent chips supporting wake-on-lan have CRC in bytes 94, 95 */

#define SROM_WORD(rom,offset) ((rom)[offset] | ((rom)[offset+1] << 8))

static void
srom_idle_state(tulip_softc *sc)
{
    uint32_t csr9;
    unsigned int i;

    csr9 = READCSR(sc, R_CSR_ROM_MII);

    csr9 |= M_CSR9_SROMCHIPSEL;
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(100);                  /* CS setup (Tcss=100) */

    /* Run the clock through the maximum number of pending read cycles */
    for (i = 0; i < SROM_MAX_CYCLES*2; i++) {
	csr9 ^= M_CSR9_SROMCLOCK;
	WRITECSR(sc, R_CSR_ROM_MII, csr9);
	cfe_nsleep(1000);             /* SK period (Fsk=0.5MHz) */
	}

    /* Deassert SROM Chip Select */
    csr9 &=~ M_CSR9_SROMCHIPSEL;
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(50);                   /* CS recovery (Tsks=50) */
}

static void
srom_write_bit(tulip_softc *sc, unsigned int data)
{
    uint32_t  csr9;

    csr9 = READCSR(sc, R_CSR_ROM_MII);

    /* Place the data bit on the bus */
    if (data == 1)
	csr9 |= M_CSR9_SROMDATAIN;
    else
	csr9 &=~ M_CSR9_SROMDATAIN;

    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(360);                      /* setup: Tdis=200 */

    /* Now clock the data into the SROM */
    WRITECSR(sc, R_CSR_ROM_MII, csr9 | M_CSR9_SROMCLOCK);
    cfe_nsleep(900);                      /* clock high, Tskh=500 */
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(450);                      /* clock low, Tskl=250 */

    /* Now clear the data bit */
    csr9 &=~ M_CSR9_SROMDATAIN;           /* data invalid, Tidh=20 for SK^ */
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(270);                      /* min cycle, 1/Fsk=2000 */
}

static uint16_t
srom_read_bit(tulip_softc *sc)
{
    uint32_t  csr9;

    csr9 = READCSR(sc, R_CSR_ROM_MII);

    /* Generate a clock cycle before doing a read */
    WRITECSR(sc, R_CSR_ROM_MII, csr9 | M_CSR9_SROMCLOCK);  /* rising edge */
    cfe_nsleep(1000);                 /* clock high, Tskh=500, Tpd=1000 */
    WRITECSR(sc, R_CSR_ROM_MII, csr9);                    /* falling edge */
    cfe_nsleep(1000);                 /* clock low, 1/Fsk=2000 */

    csr9 = READCSR(sc, R_CSR_ROM_MII);
    return ((csr9 & M_CSR9_SROMDATAOUT) != 0 ? 1 : 0);
}

#define CMD_BIT_MASK (1 << (SROM_CMD_BITS+SROM_ADDR_BITS-1))

static uint16_t
srom_read_word(tulip_softc *sc, unsigned int index)
{
    uint16_t command, word;
    uint32_t csr9;
    unsigned int i;

    csr9 = READCSR(sc, R_CSR_ROM_MII) | M_CSR9_SROMCHIPSEL;

    /* Assert the SROM CS line */
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(100);                /* CS setup, Tcss = 100 */

    /* Send the read command to the SROM */
    command = (K_SROM_READ_CMD << SROM_ADDR_BITS) | index;
    for (i = 0; i < SROM_CMD_BITS+SROM_ADDR_BITS; i++) {
	srom_write_bit(sc, (command & CMD_BIT_MASK) != 0 ? 1 : 0);
	command <<= 1;
	}

    /* Now read the bits from the SROM (MSB first) */
    word = 0;
    for (i = 0; i < 16; ++i) {
	word <<= 1;
	word |= srom_read_bit(sc);
	}

    /* Clear the SROM CS Line,  CS hold, Tcsh = 0 */
    WRITECSR(sc, R_CSR_ROM_MII, csr9 &~ M_CSR9_SROMCHIPSEL);

    return word;
}

static void
srom_set_enable(tulip_softc *sc, int enable)
{
    uint16_t command;
    uint32_t csr9;
    unsigned int i;
    unsigned int bits = (enable ? 0x3F : 0x00);

    csr9 = READCSR(sc, R_CSR_ROM_MII) | M_CSR9_SROMCHIPSEL;

    /* Assert the SROM CS line */
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(100);                /* CS setup, Tcss = 100 */

    /* Send the enable command to the SROM */
    command = (K_SROM_WEN_CMD << SROM_ADDR_BITS) | bits;
    for (i = 0; i < SROM_CMD_BITS+SROM_ADDR_BITS; i++) {
	srom_write_bit(sc, (command & CMD_BIT_MASK) != 0 ? 1 : 0);
	command <<= 1;
	}

    /* Clear the SROM CS Line,  CS hold, Tcsh = 0 */
    WRITECSR(sc, R_CSR_ROM_MII, csr9 &~ M_CSR9_SROMCHIPSEL);
}

static int
srom_write_word(tulip_softc *sc, unsigned int index, uint16_t word)
{
    uint16_t command;
    uint32_t csr9;
    unsigned int i;

    csr9 = READCSR(sc, R_CSR_ROM_MII) | M_CSR9_SROMCHIPSEL;

    /* Assert the SROM CS line */
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(100);                /* CS setup, Tcss = 100 */

    /* Send the write command to the SROM */
    command = (K_SROM_WRITE_CMD << SROM_ADDR_BITS) | index;
    for (i = 0; i < SROM_CMD_BITS+SROM_ADDR_BITS; i++) {
	srom_write_bit(sc, (command & CMD_BIT_MASK) != 0 ? 1 : 0);
	command <<= 1;
	}

    /* Now write the bits to the SROM (MSB first) */
    for (i = 0; i < 16; ++i) {
	srom_write_bit(sc, (word & 0x8000) != 0 ? 1 : 0);
	word <<= 1;
	}

    /* Clear and reassert the SROM CS Line */
    csr9 = READCSR(sc, R_CSR_ROM_MII);
    csr9 &= ~(M_CSR9_SROMCHIPSEL | M_CSR9_SROMCLOCK | M_CSR9_SROMDATAIN);
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(500);
    WRITECSR(sc, R_CSR_ROM_MII, csr9 | M_CSR9_SROMCHIPSEL);
    cfe_nsleep(500);

    /* Wait while busy */
    for (i = 1000; i > 0; --i) {
	csr9 = READCSR(sc, R_CSR_ROM_MII);
	if ((csr9 & M_CSR9_SROMDATAOUT) != 0)
	    break;
	cfe_usleep(1000);    /* Wait 1 ms. */
	}

    /* Clear the SROM CS Line,  CS hold, Tcsh = 0 */
    WRITECSR(sc, R_CSR_ROM_MII, csr9 &~ M_CSR9_SROMCHIPSEL);

    return (i > 0) ? 0 : -1;   /* Fail on timeout */
}


/****************************************************************************
 *  srom_calc_crc()
 *
 *  Calculate the CRC of the SROM and return it.  We compute the
 *  CRC per Appendix A of the 21140A ROM/external register data
 *  sheet (EC-QPQWA-TE).
 ***************************************************************************/

static uint16_t
srom_calc_crc(tulip_softc *sc, uint8_t srom[], int length)
{
    uint32_t crc = tulip_crc32(srom, length) ^ 0xFFFFFFFF;

    return (uint16_t)(crc & 0xFFFF);
}

/****************************************************************************
 *  srom_read_all(sc, uint8_t dest)
 *
 *  Read the entire SROM into the srom array
 *
 *  Input parameters:
 *         sc - tulip state
 ***************************************************************************/

static int
srom_read_all(tulip_softc *sc, uint8_t dest[])
{
    int  i;
    uint16_t crc, temp;

    WRITECSR(sc, R_CSR_ROM_MII, M_CSR9_SERROMSEL|M_CSR9_ROMREAD);

    srom_idle_state(sc);

    for (i = 0; i < SROM_SIZE/2; i++) {
	temp = srom_read_word(sc, i);
	dest[2*i] = temp & 0xFF;
	dest[2*i+1] =temp >> 8;
	}

    WRITECSR(sc, R_CSR_ROM_MII, 0);   /* CS hold, Tcsh=0 */

    crc = srom_calc_crc(sc, dest, SROM_CRC_INDEX);
    if (crc != SROM_WORD(dest, SROM_CRC_INDEX)) {
	crc = srom_calc_crc(sc, dest, 94);  /* "alternative" */
	if (crc != SROM_WORD(dest, 94)) {
	    xprintf("%s: Invalid SROM CRC, calc %04x, stored %04x\n",
		    tulip_devname(sc), crc, SROM_WORD(dest, 94));
	    return -1;
	    }
	}
    return 0;
}

static int
srom_read_addr(tulip_softc *sc, uint8_t buf[])
{
    uint8_t srom[SROM_SIZE];

    if (srom_read_all(sc, srom) == 0) {
	memcpy(buf, &srom[SROM_ADDR_INDEX], ENET_ADDR_LEN);
	return 0;
	}

    return -1;
}


/****************************************************************************
 *  earom_read_all(sc, uint8_t dest)
 *
 *  Read the entire Ethernet address ROM into the srom array (21040 only)
 *
 *  Input parameters:
 *         sc - tulip state
 ***************************************************************************/

static int
earom_read_all(tulip_softc *sc, uint8_t dest[])
{
    int  i;
    uint32_t csr9;

    WRITECSR(sc, R_CSR_ROM_MII, 0);    /* reset pointer */

    for (i = 0; i < SROM_SIZE; i++) {
	for (;;) {
	    csr9 = READCSR(sc, R_CSR_ROM_MII);
	    if ((csr9 & M_CSR9_DATANOTVALID) == 0)
		break;
	    POLL();   /* XXX need a timeout */
	    }
	dest[i] = G_CSR9_ROMDATA(csr9);
	}

    return 0;
}

static int
earom_read_addr(tulip_softc *sc, uint8_t buf[])
{
    uint8_t srom[SROM_SIZE];

    if (earom_read_all(sc, srom) == 0) {
	memcpy(buf, &srom[0], ENET_ADDR_LEN);
	return 0;
	}

    return -1;
}


/****************************************************************************
 *  pnic_read_all(sc, uint8_t dest)
 *
 *  Read the entire Ethernet address ROM into the srom array (PNIC only)
 *
 *  Input parameters:
 *         sc - tulip state
 ***************************************************************************/

static int
pnic_read_all(tulip_softc *sc, uint8_t dest[])
{
    int  i;
    uint32_t csr9, csr19;
    uint32_t cmd;
    uint16_t data;

    csr19 = READCSR(sc, R_CSR_ROMCTL);

    for (i = 0; i < SROM_SIZE/2; i++) {
	cmd = M_CSR19_BUSY
	    | V_CSR19_OPCODE(K_CSR19_OPCODE_READ)
	    | V_CSR19_ADDR(i);
	WRITECSR(sc, R_CSR_ROMCTL, cmd);
	for (;;) {
	    csr9 = READCSR(sc, R_CSR_ROMDATA);
	    if ((csr9 & M_CSR9_BUSY) == 0)
		break;
	    POLL();   /* XXX need a timeout */
	    }
	data = G_CSR9_SROMDATA(csr9);
	dest[2*i] = data >> 8;
	dest[2*i+1] = data & 0xFF;
	}

    WRITECSR(sc, R_CSR_ROMCTL, csr19);
    return 0;
}

static int
pnic_read_addr(tulip_softc *sc, uint8_t buf[])
{
    uint8_t srom[SROM_SIZE];

    if (pnic_read_all(sc, srom) == 0) {
	memcpy(buf, &srom[0], ENET_ADDR_LEN);
	return 0;
	}

    return -1;
}


static int
rom_read_all(tulip_softc *sc, uint8_t buf[])
{
    switch (sc->device) {
	case K_CHIP_DC21040:
	    return earom_read_all(sc, buf);
        case K_CHIP_82C169:
	     return pnic_read_all(sc, buf);
	default:
	    return srom_read_all(sc, buf);
	}
}

static int
rom_read_addr(tulip_softc *sc, uint8_t buf[])
{
    switch (sc->device) {
	case K_CHIP_DC21040:
	    return earom_read_addr(sc, buf);
	case K_CHIP_82C169:
	    return pnic_read_addr(sc, buf);
	default:
	    return srom_read_addr(sc, buf);
	}
}

#if 0
static void
rom_dump(uint8_t srom[])
{
    int  i;

    xprintf("DC21x4x: SROM data:");
    for (i = 0; i < SROM_SIZE; i++) {
	if (i % 16 == 0)
	    xprintf("\n %02x: ", i);
	xprintf(" %02x", srom[i]);
	}
    xprintf("\n");
}
#else
#define rom_dump(srom)
#endif
  

/****************************************************************************
 *                 MII access utility routines
 ***************************************************************************/

/* MII clock limited to 2.5 MHz, transactions end with MDIO tristated */

static void
mii_write_bits(tulip_softc *sc, uint32_t data, unsigned int count)
{
    uint32_t   csr9;
    uint32_t   bitmask;

    csr9 = READCSR(sc, R_CSR_ROM_MII) &~ (M_CSR9_MDC | M_CSR9_MIIMODE);

    for (bitmask = 1 << (count-1); bitmask != 0; bitmask >>= 1) {
	csr9 &=~ M_CSR9_MDO;
	if ((data & bitmask) != 0) csr9 |= M_CSR9_MDO;
	WRITECSR(sc, R_CSR_ROM_MII, csr9);

	cfe_nsleep(2000);     /* setup */
	WRITECSR(sc, R_CSR_ROM_MII, csr9 | M_CSR9_MDC);
	cfe_nsleep(2000);     /* hold */
	WRITECSR(sc, R_CSR_ROM_MII, csr9);
	}
}

static void
mii_turnaround(tulip_softc *sc)
{
    uint32_t  csr9;

    csr9 = READCSR(sc, R_CSR_ROM_MII) | M_CSR9_MIIMODE;

    /* stop driving data */
    WRITECSR(sc, R_CSR_ROM_MII, csr9);
    cfe_nsleep(2000);       /* setup */
    WRITECSR(sc, R_CSR_ROM_MII, csr9 | M_CSR9_MDC);
    cfe_nsleep(2000);       /* clock high */
    WRITECSR(sc, R_CSR_ROM_MII, csr9);

    /* read back and check for 0 here? */
}

/****************************************************************************
 *  mii_read_register
 *
 *  This routine reads a register from the PHY chip using the MII
 *  serial management interface.
 *
 *  Input parameters:
 *         index - index of register to read (0-31)
 *
 *  Return value:
 *         word read from register
 ***************************************************************************/

static uint16_t
mii_read_register(tulip_softc *sc, unsigned int index)
{
    /* Send the command and address to the PHY.  The sequence is
       a synchronization sequence (32 1 bits)
       a "start" command (2 bits)
       a "read" command (2 bits)
       the PHY addr (5 bits)
       the register index (5 bits)
     */
    uint32_t  csr9;
    uint16_t  word;
    int i;

    mii_write_bits(sc, 0xFF, 8);
    mii_write_bits(sc, 0xFFFFFFFF, 32);
    mii_write_bits(sc, MII_COMMAND_START, 2);
    mii_write_bits(sc, MII_COMMAND_READ, 2);
    mii_write_bits(sc, sc->phy_addr, 5);
    mii_write_bits(sc, index, 5);

    mii_turnaround(sc);

    csr9 = (READCSR(sc, R_CSR_ROM_MII) &~ M_CSR9_MDC) | M_CSR9_MIIMODE;
    word = 0;

    for (i = 0; i < 16; i++) {
	WRITECSR(sc, R_CSR_ROM_MII, csr9);
	cfe_nsleep(2000);    /* clock width low */
	WRITECSR(sc, R_CSR_ROM_MII, csr9 | M_CSR9_MDC);
	cfe_nsleep(2000);    /* clock width high */
	WRITECSR(sc, R_CSR_ROM_MII, csr9);
	cfe_nsleep(1000);    /* output delay */
	word <<= 1;
	if ((READCSR(sc, R_CSR_ROM_MII) & M_CSR9_MDI) != 0)
	    word |= 0x0001;
	}

    return word;

    /* reset to output mode? */
}

/****************************************************************************
 *  mii_write_register
 *
 *  This routine writes a register in the PHY chip using the MII
 *  serial management interface.
 *
 *  Input parameters:
 *         index - index of register to write (0-31)
 *         value - word to write
 ***************************************************************************/

static void
mii_write_register(tulip_softc *sc, unsigned int index, uint16_t value)
{
    mii_write_bits(sc, 0xFF, 8);
    mii_write_bits(sc, 0xFFFFFFFF, 32);
    mii_write_bits(sc, MII_COMMAND_START, 2);
    mii_write_bits(sc, MII_COMMAND_WRITE, 2);
    mii_write_bits(sc, sc->phy_addr, 5);
    mii_write_bits(sc, index, 5);
    mii_write_bits(sc, MII_COMMAND_ACK, 2);
    mii_write_bits(sc, value, 16);

    /* reset to input mode? */
}


static int
mii_probe(tulip_softc *sc)
{
    int i;
    uint16_t id1, id2;

    for (i = 0; i < 32; i++) {
        sc->phy_addr = i;
	id1 = (*sc->mii_read_register)(sc, MII_PHYIDR1);
	id2 = (*sc->mii_read_register)(sc, MII_PHYIDR2);
	if ((id1 != 0x0000 && id1 != 0xFFFF) ||
	    (id2 != 0x0000 && id2 != 0xFFFF)) {
	    sc->phy_vendor = ((uint32_t)id1 << 6) | ((id2 >> 10) & 0x3F);
	    sc->phy_device = (id2 >> 4) & 0x3F;
	    xprintf("%s: PHY %d, vendor: %06x, device %02x\n",
		    tulip_devname(sc),
		    sc->phy_addr, sc->phy_vendor, sc->phy_device);
	    return 0;
	    }
	}
    return -1;
}

#define OUI_NAT_SEMI   0x080017
#define DEV_DP83840    0x00

#if 0
static void
mii_dump(tulip_softc *sc, const char *label)
{
    int i;
    uint16_t  r;

    xprintf("%s\n", label);
    for (i = 0; i <= 6; ++i) {
	r = (*sc->mii_read_register)(sc, i);
	xprintf("MII_REG%02x: %04x\n", i, r);
	}
    xprintf("OUI: %08x, Part %02x\n", sc->phy_vendor, sc->phy_device);
}
#else
#define mii_dump(sc,label)
#endif


/* The following functions are suitable for all tulips with MII
   interfaces. */

static void
mii_set_speed(tulip_softc *sc, int speed, int autoneg)
{
    uint16_t  control;
    uint32_t  opmode = 0;

    if (sc->phy_vendor == OUI_NAT_SEMI && sc->phy_device == DEV_DP83840) {
	uint16_t  pcr;

	/* Work-around for NS DP83840/A from ???.  Needed? */
	pcr = (*sc->mii_read_register)(sc, 0x17);
	pcr |= (0x400|0x100|0x40|0x20);
	(*sc->mii_write_register)(sc, 0x17, pcr);
	}

    control = (*sc->mii_read_register)(sc, MII_BMCR);

    if (!autoneg) {
	control &=~ (BMCR_ANENABLE | BMCR_RESTARTAN);
	(*sc->mii_write_register)(sc, MII_BMCR, control);
	control &=~ (BMCR_SPEED0 | BMCR_SPEED1 | BMCR_DUPLEX);
	}

    switch (speed) {
	case ETHER_SPEED_10HDX:
	default:
	    opmode = M_CSR6_SPEED_10_MII;
	    break;
	case ETHER_SPEED_10FDX:
	    control |= BMCR_DUPLEX;
	    opmode = M_CSR6_SPEED_10_MII | M_CSR6_FULLDUPLEX;
	    break;
	case ETHER_SPEED_100HDX:
	    control |= BMCR_SPEED100;
	    opmode = M_CSR6_SPEED_100_MII;
	    break;
	case ETHER_SPEED_100FDX:
	    control |= BMCR_SPEED100 | BMCR_DUPLEX ;
	    opmode = M_CSR6_SPEED_100_MII | M_CSR6_FULLDUPLEX;
	    break;
	    }

    if (!autoneg)
	(*sc->mii_write_register)(sc, MII_BMCR, control);

    opmode |= M_CSR6_MBO;
    opmode |= V_CSR6_THRESHCONTROL(K_CSR6_TXTHRES_128_72);
    WRITECSR(sc, R_CSR_OPMODE, opmode);
    mii_dump(sc, "setspeed PHY");
}

static void
mii_autonegotiate(tulip_softc *sc)
{
    uint16_t  control, status, cap;
    unsigned int  timeout;
    int linkspeed;
    int autoneg;

    linkspeed = ETHER_SPEED_UNKNOWN;

    /* Read twice to clear latching bits */
    status = (*sc->mii_read_register)(sc, MII_BMSR);
    status = (*sc->mii_read_register)(sc, MII_BMSR);
    mii_dump(sc, "query PHY");

    if ((status & (BMSR_AUTONEG | BMSR_LINKSTAT)) ==
        (BMSR_AUTONEG | BMSR_LINKSTAT))
	control = (*sc->mii_read_register)(sc, MII_BMCR);
    else {
	/* reset the PHY */
	(*sc->mii_write_register)(sc, MII_BMCR, BMCR_RESET);
	timeout = 3000;
	for (;;) {
	    control = (*sc->mii_read_register)(sc, MII_BMCR);
	    if ((control && BMCR_RESET) == 0) break;
	    cfe_sleep(CFE_HZ/2);
	    timeout -= 500;
	    if (timeout <= 0) break;
	    }
	if ((control & BMCR_RESET) != 0) {
	    xprintf("%s: PHY reset failed\n", tulip_devname(sc));
	    return;
	    }

	status = (*sc->mii_read_register)(sc, MII_BMSR);
	cap = ((status >> 6) & (ANAR_TXFD | ANAR_TXHD | ANAR_10FD | ANAR_10HD))
	      | PSB_802_3;
	(*sc->mii_write_register)(sc, MII_ANAR, cap);
	control |= (BMCR_ANENABLE | BMCR_RESTARTAN);
	(*sc->mii_write_register)(sc, MII_BMCR, control);

	timeout = 3000;
	for (;;) {
	    status = (*sc->mii_read_register)(sc, MII_BMSR);
	    if ((status & BMSR_ANCOMPLETE) != 0) break;
	    cfe_sleep(CFE_HZ/2);
	    timeout -= 500;
	    if (timeout <= 0) break;
	    }
	mii_dump(sc, "done PHY");
	}

    xprintf("%s: Link speed: ", tulip_devname(sc));
    if ((status & BMSR_ANCOMPLETE) != 0) {
	/* A link partner was negogiated... */

	uint16_t remote = (*sc->mii_read_register)(sc, MII_ANLPAR);

	autoneg = 1;
	if ((remote & ANLPAR_TXFD) != 0) {
	    xprintf("100BaseT FDX");
	    linkspeed = ETHER_SPEED_100FDX;	 
	    }
	else if ((remote & ANLPAR_TXHD) != 0) {
	    xprintf("100BaseT HDX");
	    linkspeed = ETHER_SPEED_100HDX;	 
	    }
	else if ((remote & ANLPAR_10FD) != 0) {
	    xprintf("10BaseT FDX");
	    linkspeed = ETHER_SPEED_10FDX;	 
	    }
	else if ((remote & ANLPAR_10HD) != 0) {
	    xprintf("10BaseT HDX");
	    linkspeed = ETHER_SPEED_10HDX;	 
	    }
	xprintf("\n");
	}
    else {
	/* no link partner negotiation */

	autoneg = 0;
	xprintf("Unknown, assuming 10BaseT\n");
	control &=~ (BMCR_ANENABLE | BMCR_RESTARTAN);
	(*sc->mii_write_register)(sc, MII_BMCR, control);
	linkspeed = ETHER_SPEED_10HDX;
	}

    if ((status & BMSR_LINKSTAT) == 0)
	(*sc->mii_write_register)(sc, MII_BMCR, control);
    mii_set_speed(sc, linkspeed, autoneg);

    status = (*sc->mii_read_register)(sc, MII_BMSR);  /* clear latching bits */
    mii_dump(sc, "final PHY");
}


/* Chip specific code */

/* For the 21140 and 21143, we support only the MII (10/100) and SYM
   (100) interfaces.  We attempt to discover the appropriate interface
   and port settings by scanning the vendor ROMs, but some are
   sufficiently confused or quirky to require special case code. */

/* Known vendors with cards requiring special initialization. */
#define K_PCI_VENDOR_COGENT   0x1109    /* inherited by Adaptec */
#define K_PCI_VENDOR_ZNYX     0x110D
#define K_PCI_VENDOR_PHOBOS   0x13D8

static void
dc21143_set_speed(tulip_softc *sc, int speed)
{
    uint32_t opmode = 0;

    WRITECSR(sc, R_CSR_SIAMODE0, 0);

    switch (speed) {
	case ETHER_SPEED_AUTO:
	    break;
	case ETHER_SPEED_10HDX:
	default:
	    WRITECSR(sc, R_CSR_SIAMODE1, M_CSR14_10BT_HD);
	    WRITECSR(sc, R_CSR_SIAMODE2, sc->gpdata);
	    opmode = M_CSR6_SPEED_10;
	    break;
	case ETHER_SPEED_10FDX:
	    WRITECSR(sc, R_CSR_SIAMODE1, M_CSR14_10BT_FD);
	    WRITECSR(sc, R_CSR_SIAMODE2, sc->gpdata);
	    opmode = M_CSR6_SPEED_10 | M_CSR6_FULLDUPLEX;
	    break;
	case ETHER_SPEED_100HDX:
	    WRITECSR(sc, R_CSR_SIAMODE1, 0);
	    WRITECSR(sc, R_CSR_SIAMODE2, sc->gpdata);
	    opmode = M_CSR6_SPEED_100;
	    break;
	case ETHER_SPEED_100FDX:
	    WRITECSR(sc, R_CSR_SIAMODE1, 0);
	    WRITECSR(sc, R_CSR_SIAMODE2, sc->gpdata);
	    opmode = M_CSR6_SPEED_100 | M_CSR6_FULLDUPLEX;
	    break;
	}

    WRITECSR(sc, R_CSR_SIAMODE0, M_CSR13_CONN_NOT_RESET);  

    opmode |= M_CSR6_MBO;
    opmode |= V_CSR6_THRESHCONTROL(K_CSR6_TXTHRES_128_72);
    WRITECSR(sc, R_CSR_OPMODE, opmode);
}

static void
dc21143_autonegotiate(tulip_softc *sc)
{
    uint32_t opmode;
    uint32_t tempword;
    int count;
    int linkspeed;

    linkspeed = ETHER_SPEED_UNKNOWN;

    /* Program the media setup into the CSRs. */
    /* reset SIA */
    WRITECSR(sc, R_CSR_SIAMODE0, 0);

    /* set to speed_10, fullduplex to start_nway */
    opmode =
        M_CSR6_SPEED_10 |
        M_CSR6_FULLDUPLEX |
        M_CSR6_MBO;
    WRITECSR(sc, R_CSR_OPMODE, opmode);

    /* Choose advertised capabilities */
    tempword =
	M_CSR14_100BASETHALFDUP |
	M_CSR14_100BASETFULLDUP |
	M_CSR14_HALFDUPLEX10BASET;
    WRITECSR(sc, R_CSR_SIAMODE1, tempword);

    /* Enable autonegotiation */
    tempword |= M_CSR14_AUTONEGOTIATE | 0xFFFF;
    WRITECSR(sc, R_CSR_SIAMODE1, tempword);
    WRITECSR(sc, R_CSR_SIAMODE2, sc->gpdata);
    WRITECSR(sc, R_CSR_OPMODE, opmode);
    WRITECSR(sc, R_CSR_SIAMODE0, M_CSR13_CONN_NOT_RESET);

    /* STATE check nway, poll until a valid 10/100mbs signal seen */
    WRITECSR(sc, R_CSR_STATUS, M_CSR5_LINKPASS);  /* try to clear this... */

    /* (Re)start negotiation */
    tempword = READCSR(sc, R_CSR_SIASTATUS);
    tempword &=~ M_CSR12_AUTONEGARBIT;
    tempword |=  V_CSR12_AUTONEGARBIT(0x1);
    
    for (count = 0; count <= 13; count++) {
	tempword = READCSR(sc, R_CSR_STATUS);
	if (tempword & M_CSR5_LINKPASS)
	    break;
	cfe_sleep(CFE_HZ/10);
	}

    if (count > 13)
        xprintf("%s: Link autonegotiation failed\n", tulip_devname(sc));

    /* STATE configure nway, check to see if any abilities common to us.
       If they do, set to highest mode, if not, we will see if the partner
       will do 100mb or 10mb - then set it */

    tempword = READCSR(sc, R_CSR_SIASTATUS);
    /* clear the autonegogiate complete bit */
    WRITECSR(sc, R_CSR_STATUS, M_CSR5_LINKPASS);

    if (tempword & M_CSR12_LINKPARTNEG) {
	/* A link partner was negogiated... */

        xprintf("%s: Link speed: ", tulip_devname(sc));
	if (tempword & 0x01000000) {      /* 100FD */
	    xprintf("100BaseT FDX");
	    linkspeed = ETHER_SPEED_100FDX;
	    }
	else if (tempword & 0x00800000) { /* 100HD */
	    xprintf("100BaseT HDX");
	    linkspeed = ETHER_SPEED_100HDX;
	    }
	else if (tempword & 0x00400000) { /* 10FD */
	    xprintf("10BaseT FDX");
	    linkspeed = ETHER_SPEED_10FDX;
	    }
	else if (tempword & 0x00200000) { /* 10HD */
	    xprintf("10BaseT HDX");
	    linkspeed = ETHER_SPEED_10HDX;
	    }
	xprintf("\n");
	}
    else {
	/* no link partner negotiation */
	/* disable link for 1.3 seconds to break any existing connections */

        xprintf("%s: ", tulip_devname(sc));
	dc21143_set_speed(sc, ETHER_SPEED_10HDX);
	cfe_sleep(CFE_HZ/8);

	tempword = READCSR(sc, R_CSR_SIASTATUS);

	if ((tempword & 0x02) == 0) {
	    /* 100 mb signal present set to 100mb */
	    xprintf("No link partner... setting to 100BaseT HDX\n");
	    linkspeed = ETHER_SPEED_100HDX;
	    }
	else if ((tempword & 0x04) == 0) {
	    /* 10 mb signal present */
	    xprintf("No link partner... setting to 10BaseT HDX\n");
	    linkspeed = ETHER_SPEED_10HDX;
	    }
	else {
	    /* couldn't determine line speed, so set to 10mbs */
	    xprintf("Unknown; defaulting to 10BaseT HDX\n");
	    linkspeed = ETHER_SPEED_10HDX;
	    }
	}

    dc21143_set_speed(sc, linkspeed);
}

static void
dc21143_set_loopback(tulip_softc *sc, int mode)
{
    uint32_t v;

    WRITECSR(sc, R_CSR_SIAMODE0, 0);
    if (mode == ETHER_LOOPBACK_EXT) {
	/* deal with CSRs 13-15 */
	}
    cfe_sleep(CFE_HZ/10);   /* check this */

    /* Update the SIA registers */
    v = READCSR(sc, R_CSR_SIAMODE0);
    WRITECSR(sc, R_CSR_SIAMODE0, v &~ 0xFFFF);
    v = READCSR(sc, R_CSR_SIAMODE1);
    WRITECSR(sc, R_CSR_SIAMODE1, v &~ 0xFFFF);
    v = READCSR(sc, R_CSR_SIAMODE2);
    WRITECSR(sc, R_CSR_SIAMODE2, v | 0xC000);   /* WC of HCKR, RMP */
    if (mode == ETHER_LOOPBACK_OFF)
	WRITECSR(sc, R_CSR_SIAMODE2, sc->gpdata);
    else
	WRITECSR(sc, R_CSR_SIAMODE2, (v &~ 0xFFFF) | M_CSR15_GP_AUIBNC);
    
    WRITECSR(sc, R_CSR_SIAMODE0, M_CSR13_CONN_NOT_RESET);

    sc->loopback = mode;
}

static void
dc21143_hwinit(tulip_softc *sc, uint8_t srom[])
{
    uint16_t vendor;
    uint16_t leaf, block, info;
    uint8_t  i;
    uint16_t gpr_control;
    uint32_t v;
    uint32_t csr6word, csr14word;

    vendor = SROM_WORD(srom, SROM_VENDOR_INDEX);
    sc->phy_type = NONE;
    info = 0;

    if (srom[SROM_FORMAT_INDEX] != 0 && srom[SROM_FORMAT_INDEX] <= 4) {
	leaf = SROM_WORD(srom, SROM_LEAF0_OFFSET_INDEX);
	block = leaf + 3;

	/* Note that blocks are arranged by increasing preference. */
	for (i = 0; i < srom[leaf+2]; i++) {
	    switch (srom[block+1]) {
		case 0x03:
		    sc->phy_type = MII;
		    info = block;
		    break;
		case 0x04:
		    sc->phy_type = SYM;
		    info = block;
		    break;
		}
	    block += (srom[block+0] & 0x7F);
	    }
	}
#if defined(_MOUSSE_) || defined(_BCM98245CPCI_)
    else if (srom[SROM_FORMAT_INDEX] == 0xFF)
      {
	/* The Mousse board has no srom and returns all 1's for reads. */
	sc->phy_type = MII;
	vendor = 0xFFFF;
      }
#endif

    if (sc->phy_type == MII) {
        sc->gpdata = 0;
	if (vendor == K_PCI_VENDOR_COGENT) {
	    /* Cogent/Adaptec MII (ANA-6911A). */
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0821 << 16);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0001 << 16);
	    cfe_sleep(CFE_HZ/10);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0000 << 16);
	    cfe_sleep(CFE_HZ/2);
	    sc->gpdata = 0;
	    }
	else if (vendor == K_PCI_VENDOR_PHOBOS) {
	    /* Phobos 430TX quad card.  The advertised values appear
               not to work.  Note that Phobos also neglects to set the
               0x80 bit on the block length.  */
#if 0  /* per EEPROM */
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x080E << 16);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x000E << 16);
	    cfe_sleep(CFE_HZ/10);
	    sc->gpdata = 0x0E;
#else  /* following Adaptec 21143 with MII interface */
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0821 << 16);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0001 << 16);
	    cfe_sleep(CFE_HZ/10);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0000 << 16);
	    cfe_sleep(CFE_HZ/2);
	    sc->gpdata = 0;
#endif
	    }
#if defined(_MOUSSE_) || defined(_BCM98245CPCI_)
	else if (vendor == 0xFFFF) {
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x080F << 16);
	    sc->gpdata = 0x0;
	    }
#endif
	mii_probe(sc);
	}
    else if (sc->phy_type == SYM) {
        gpr_control = SROM_WORD(srom, info+3) << 16;
	if (vendor == K_PCI_VENDOR_ZNYX) {
	    /* Znyz 34xQ family of adapters */

	    /* The ZX345Q with wake-on-LAN enabled apparently clears
	       ANE and TAS on power up (but not cold reset) */
	    WRITECSR(sc, R_CSR_SIAMODE1, 0xFFFFFFFF);

	    /* The following is a reset workaround for QS/Kendin PHYs
	       as suggested by an Intel app note.  Bit 0x40000 is the
	       PHY reset (low true) on Znyx cards. */
	    WRITECSR(sc, R_CSR_SIAMODE2, gpr_control | M_CSR15_GP_AUIBNC);
	    cfe_sleep(CFE_HZ/5);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x40000);  /* release reset */
	    cfe_sleep(CFE_HZ/5);
	    sc->gpdata = 0x40000;
	    }
	else {
	    WRITECSR(sc, R_CSR_SIAMODE2, gpr_control);
	    sc->gpdata = 0;
	    }
	sc->gpdata |= (SROM_WORD(srom, info+5) << 16) | M_CSR15_GP_AUIBNC;
	}
    else {
	/* Most 21143 cards use SYM.  Match the DEC reference design. */
	sc->phy_type = SYM;
	WRITECSR(sc, R_CSR_SIAMODE2, M_CSR15_CONFIG_GEPS_LEDS);
	sc->gpdata = M_CSR15_DEFAULT_VALUE;
	}

    /* CSR0 - bus mode */
    v = V_CSR0_SKIPLEN(0) | 
	V_CSR0_CACHEALIGN(K_CSR0_ALIGN32) | 
	M_CSR0_READMULTENAB | M_CSR0_READLINEENAB |
        M_CSR0_WRITEINVALENAB |
	V_CSR0_BURSTLEN(K_CSR0_BURSTANY);
#if ENDIAN_BIG
    if (!sc->swap)
	v |= M_CSR0_BIGENDIAN;     /* big-endian data serialization */
#endif
    WRITECSR(sc, R_CSR_BUSMODE, v);

    /* CSR6 - operation mode */
    v = M_CSR6_PORTSEL |
	V_CSR6_THRESHCONTROL(K_CSR6_TXTHRES_128_72) |
	M_CSR6_MBO;
    if (sc->phy_type == SYM)
	v |= M_CSR6_PCSFUNC |M_CSR6_SCRAMMODE;
    WRITECSR(sc, R_CSR_OPMODE, v);

    /* About to muck with the SIA, reset it.(?) */
    /* WRITECSR(sc, R_CSR_SIASTATUS, 0); */

    /* Must shut off all transmit/receive in order to attempt to 
       achieve Full Duplex */
    csr6word = READCSR(sc, R_CSR_OPMODE);
    WRITECSR(sc, R_CSR_OPMODE, csr6word &~ (M_CSR6_TXSTART | M_CSR6_RXSTART));
    csr6word = READCSR(sc, R_CSR_OPMODE);
    
    WRITECSR(sc, R_CSR_RXRING, PTR_TO_PCI(sc, sc->rxdscr_start));
    WRITECSR(sc, R_CSR_TXRING, PTR_TO_PCI(sc, sc->txdscr_start));

    if (sc->phy_type == MII) {
	if (sc->linkspeed == ETHER_SPEED_AUTO)
	    mii_autonegotiate(sc);
	else
	    mii_set_speed(sc, sc->linkspeed, 0);
        }
    else {
	if (sc->linkspeed == ETHER_SPEED_AUTO) {
	    dc21143_autonegotiate(sc);
	    }
	else {
	    /* disable autonegotiate so we can set full duplex to on */
	    WRITECSR(sc, R_CSR_SIAMODE0, 0);
	    csr14word = READCSR(sc, R_CSR_SIAMODE1);
	    csr14word &=~ M_CSR14_AUTONEGOTIATE;
	    WRITECSR(sc, R_CSR_SIAMODE1, csr14word);
	    WRITECSR(sc, R_CSR_SIAMODE0, M_CSR13_CONN_NOT_RESET);

	    dc21143_set_speed(sc, sc->linkspeed);
	    }
        }
}


static void
dc21140_set_speed(tulip_softc *sc, int speed, int autoneg)
{
    mii_set_speed(sc, speed, autoneg);
}

static void
dc21140_set_loopback(tulip_softc *sc, int mode)
{
    if (mode == ETHER_LOOPBACK_EXT) {
	xprintf("%s: external loopback mode NYI\n", tulip_devname(sc));
	mode = ETHER_LOOPBACK_OFF;
	}
    else if (mode != ETHER_LOOPBACK_INT)
        mode = ETHER_LOOPBACK_OFF;

    sc->loopback = mode;
}

static void
dc21140_hwinit(tulip_softc *sc, uint8_t srom[])
{
    uint16_t vendor;
    uint16_t leaf, block;
    uint8_t  i;
    uint8_t gpr_control, gpr_data;
    uint32_t v;
    uint32_t opmode;

    /* Assume that we will use an MII or a SYM interface for 100 Mbps
       operation and default to MII, the most common arrangement for
       21140 boards. */

    vendor = SROM_WORD(srom, SROM_VENDOR_INDEX);
    if (srom[SROM_FORMAT_INDEX] == 0 || srom[SROM_FORMAT_INDEX] > 4) {
	/* Most 21140 cards use MII.  Match the DEC reference design. */
        gpr_control = 0x1F;
	gpr_data = 0x00;
	sc->phy_type = MII;
	}
    else if (srom[SROM_ADDR_INDEX+0] == 0x00 && srom[SROM_ADDR_INDEX+1] == 0xC0
	     && srom[SROM_ADDR_INDEX+2] == 0x95) {
	/* Znyx 34x 21140 cards implement only an MII interface but
           the ROM uses version 1 encoding, which has no provision for
           MII blocks.  Also, an additional copy of the MAC address
           appears at the usual location for the vendor id; 00-C0-95
           is Znyx' IEEE OUI. */
	gpr_control = 0x00;     /* All inputs, per Znyx docs */
	gpr_data = 0x00;
	sc->phy_type = MII;
	}
    else {
	leaf = SROM_WORD(srom, SROM_LEAF0_OFFSET_INDEX);
	gpr_control = srom[leaf+2];
	gpr_data = 0;
	block = leaf + 4;
	sc->phy_type = NONE;
	
	/* Note that blocks are arranged by increasing preference. */
	for (i = 0; i < srom[leaf+3]; i++) {
	    if ((srom[block] & 0x80) == 0) {
		if (srom[block] == 0x03 || srom[block] == 0x05) {
		    gpr_data = srom[block+1];
		    sc->phy_type = SYM;
		    }
		block += 4;
		}
	    else {
		if (srom[block+1] == 0x01) {
		    gpr_data = 0x00;
		    sc->phy_type = MII;
		    }
		else if (srom[block+1] == 0x00 &&
			 (srom[block+2] == 0x03 || srom[block+2] == 0x05)) {
		    gpr_data = srom[block+3];
		    sc->phy_type = SYM;
		    }
		block += (srom[block] & 0x7F) + 1;
		}
	    if (sc->phy_type == NONE) {
		sc->phy_type = MII;    /* Try MII */
		}
	    }
	}

    WRITECSR(sc, R_CSR_OPMODE, M_CSR6_PORTSEL);
    RESET_ADAPTER(sc);

    WRITECSR(sc, R_CSR_GENPORT, M_CSR12_CONTROL | gpr_control);
    cfe_nsleep(100);                  /* CS setup (Tcss=100) */
    WRITECSR(sc, R_CSR_GENPORT, gpr_data);   /* setup PHY */

    if (sc->phy_type == MII) {
	mii_probe(sc);
	}

    /* CSR0 - bus mode */
    v = V_CSR0_SKIPLEN(0) | 
	V_CSR0_CACHEALIGN(K_CSR0_ALIGN32) | 
	M_CSR0_READMULTENAB | M_CSR0_READLINEENAB |
	M_CSR0_WRITEINVALENAB |
	V_CSR0_BURSTLEN(K_CSR0_BURSTANY);
#if ENDIAN_BIG
    if (!sc->swap)
	v |= M_CSR0_BIGENDIAN;     /* big-endian data serialization */
#endif
    WRITECSR(sc, R_CSR_BUSMODE, v);

    /* CSR6 - operation mode */
    v = M_CSR6_PORTSEL |
	V_CSR6_THRESHCONTROL(K_CSR6_TXTHRES_128_72) |
	M_CSR6_MBO;
    WRITECSR(sc, R_CSR_OPMODE, v);

    /* Must shut off all transmit/receive in order to attempt to 
       achieve Full Duplex */
    opmode = READCSR(sc, R_CSR_OPMODE);
    WRITECSR(sc, R_CSR_OPMODE, opmode &~ (M_CSR6_TXSTART | M_CSR6_RXSTART));
    opmode = READCSR(sc, R_CSR_OPMODE);
    
    WRITECSR(sc, R_CSR_RXRING, PTR_TO_PCI(sc, sc->rxdscr_start));
    WRITECSR(sc, R_CSR_TXRING, PTR_TO_PCI(sc, sc->txdscr_start));

    if (sc->phy_type == MII) {
	if (sc->linkspeed == ETHER_SPEED_AUTO)
	    mii_autonegotiate(sc);
	else
	    mii_set_speed(sc, sc->linkspeed, 0);
	}
    else {
	/* XXX The 21140 requires a soft reset after changing PORTSEL.
	   For now, remain committed to the SYM port (100 Mb/s) */
	switch (sc->linkspeed) {
	    default:
		sc->linkspeed = ETHER_SPEED_100HDX;   /* for now */
		/* fall through */
	    case ETHER_SPEED_100HDX:
		opmode |= M_CSR6_SPEED_100;
		break;
	    case ETHER_SPEED_100FDX:
		opmode |= M_CSR6_SPEED_100 | M_CSR6_FULLDUPLEX;
		break;
		}

	/* XXX Need to reset and reinitialize if we choose SPEED_10 above */
	WRITECSR(sc, R_CSR_OPMODE, opmode);
	}
}


static void
dc21041_set_speed(tulip_softc *sc, int speed)
{
    uint32_t opmode = 0;

    WRITECSR(sc, R_CSR_SIAMODE0, 0);

    /* For now, always force 10BT, HDX (21041, Table 3-62) */
    switch (speed) {
	case ETHER_SPEED_10HDX:
	default:
	    WRITECSR(sc, R_CSR_SIAMODE1, 0x7F3F);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0008);
	    opmode = M_CSR6_SPEED_10;
	    break;
	}

    WRITECSR(sc, R_CSR_SIAMODE0, 0xEF00 | M_CSR13_CONN_NOT_RESET);  
    cfe_sleep(CFE_HZ/10);

    opmode |= V_CSR6_THRESHCONTROL(K_CSR6_TXTHRES_128_72);
    WRITECSR(sc, R_CSR_OPMODE, opmode);
}

static void
dc21041_set_loopback(tulip_softc *sc, int mode)
{
    /* For now, always assume 10BT */
    uint32_t mode0;

    WRITECSR(sc, R_CSR_SIAMODE0, 0);
    cfe_sleep(CFE_HZ/10);   /* check this */

    /* Update the SIA registers */
    if (mode == ETHER_LOOPBACK_EXT) {
	/* NB: this is really just internal but through the 10BT endec */
        WRITECSR(sc, R_CSR_SIAMODE1, 0x7A3F);
	WRITECSR(sc, R_CSR_SIAMODE2, 0x0008);
	mode0 = 0;
	}
    else if (mode == ETHER_LOOPBACK_INT) {
	/* MAC internal loopback, no SIA */
	WRITECSR(sc, R_CSR_SIAMODE1, 0x0000);
	WRITECSR(sc, R_CSR_SIAMODE2, 0x000E);
	mode0 = M_CSR13_CONN_AUI_10BT;
	}
    else {
	mode = ETHER_LOOPBACK_OFF;
	WRITECSR(sc, R_CSR_SIAMODE1, 0x7F3F);
	WRITECSR(sc, R_CSR_SIAMODE2, 0x0008);
	mode0 = 0;
	}
        
    WRITECSR(sc, R_CSR_SIAMODE0, 0xEF00 | mode0 | M_CSR13_CONN_NOT_RESET );

    sc->loopback = mode;
}

static void
dc21041_hwinit(tulip_softc *sc, uint8_t srom[])
{
    uint32_t v;

    sc->phy_type = SRL;

    /* CSR0 - bus mode */
    v = V_CSR0_SKIPLEN(0) | 
	V_CSR0_CACHEALIGN(K_CSR0_ALIGN32) | 
	V_CSR0_BURSTLEN(K_CSR0_BURSTANY);
#if ENDIAN_BIG
    if (!sc->swap)
	v |= M_CSR0_BIGENDIAN;     /* big-endian data serialization */
#endif
    WRITECSR(sc, R_CSR_BUSMODE, v);

    WRITECSR(sc, R_CSR_INTMASK, 0);

    WRITECSR(sc, R_CSR_RXRING, PTR_TO_PCI(sc, sc->rxdscr_start));
    WRITECSR(sc, R_CSR_TXRING, PTR_TO_PCI(sc, sc->txdscr_start));

    /* For now, always force 10BT, HDX (21041, Table 3-62) */
    dc21041_set_speed(sc, ETHER_SPEED_10HDX);
}


static void
dc21040_set_speed(tulip_softc *sc, int speed)
{
    uint32_t opmode = 0;

    WRITECSR(sc, R_CSR_SIAMODE0, 0);

    /* For now, force 10BT, HDX unless FDX requested (21040, Table 3-53) */
    switch (speed) {
	case ETHER_SPEED_10HDX:
	default:
	    WRITECSR(sc, R_CSR_SIAMODE1, 0xFFFF);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0000);
	    opmode = 0;
	    break;
	case ETHER_SPEED_10FDX:
	    WRITECSR(sc, R_CSR_SIAMODE1, 0xFFFD);
	    WRITECSR(sc, R_CSR_SIAMODE2, 0x0000);
	    opmode = M_CSR6_FULLDUPLEX;
	    break;
	}

    WRITECSR(sc, R_CSR_SIAMODE0, 0xEF00 | M_CSR13_CONN_NOT_RESET);  
    cfe_sleep(CFE_HZ/10);

    opmode |= V_CSR6_THRESHCONTROL(K_CSR6_TXTHRES_128_72);
    WRITECSR(sc, R_CSR_OPMODE, opmode);
}

static void
dc21040_set_loopback(tulip_softc *sc, int mode)
{
    WRITECSR(sc, R_CSR_SIAMODE0, 0);
    cfe_sleep(CFE_HZ/10);   /* check this */

    /* Update the SIA registers */
    if (mode == ETHER_LOOPBACK_EXT) {
	/* NB: this is on-chip loopback through the 10BT endec */
        WRITECSR(sc, R_CSR_SIAMODE1, 0xFEFB);
	WRITECSR(sc, R_CSR_SIAMODE2, 0x0008);
	}
    else if (mode == ETHER_LOOPBACK_INT) {
	/* MAC internal loopback, no SIA */
	WRITECSR(sc, R_CSR_SIAMODE1, 0x0000);
	WRITECSR(sc, R_CSR_SIAMODE2, 0x0000);
	}
    else {
	mode = ETHER_LOOPBACK_OFF;
	WRITECSR(sc, R_CSR_SIAMODE1, 0xFFFF);
	WRITECSR(sc, R_CSR_SIAMODE2, 0x0000);
	}

    WRITECSR(sc, R_CSR_SIAMODE0, 0x8F00 | M_CSR13_CONN_NOT_RESET );

    sc->loopback = mode;
}

static void
dc21040_hwinit(tulip_softc *sc, uint8_t srom[])
{
    uint32_t v;

    sc->phy_type = SRL;

    /* CSR0 - bus mode */
    v = V_CSR0_SKIPLEN(0) | 
	V_CSR0_CACHEALIGN(K_CSR0_ALIGN32) | 
	V_CSR0_BURSTLEN(K_CSR0_BURSTANY);
#if ENDIAN_BIG
    if (!sc->swap)
	v |= M_CSR0_BIGENDIAN;     /* big-endian data serialization */
#endif
    WRITECSR(sc, R_CSR_BUSMODE, v);

    WRITECSR(sc, R_CSR_INTMASK, 0);

    dc21040_set_speed(sc, sc->linkspeed);
}


static void
dm9102_hwinit(tulip_softc *sc, uint8_t srom[])
{
    uint8_t gpr_control, gpr_data;
    uint32_t v;
    uint32_t opmode;

    gpr_control = 0x80;
    gpr_data = 0x00;
    sc->phy_type = MII;   /* DM9102(A) has built-in PHY with MII */

    WRITECSR(sc, R_CSR_OPMODE, M_CSR6_PORTSEL);
    RESET_ADAPTER(sc);

    WRITECSR(sc, R_CSR_GENPORT, M_CSR12_CONTROL | gpr_control);
    cfe_nsleep(100);                  /* CS setup (Tcss=100) */
    WRITECSR(sc, R_CSR_GENPORT, gpr_data);   /* setup PHY */

    mii_probe(sc);

    /* CSR0 - bus mode.  The DM9102(A) requires all bits clear. */
    v = 0;
    WRITECSR(sc, R_CSR_BUSMODE, v);

    /* CSR6 - operation mode.  9102 apparently requires store-and-forward  */
    v = M_CSR6_PORTSEL |
        M_CSR6_STOREFWD |
	M_CSR6_MBO;
    WRITECSR(sc, R_CSR_OPMODE, v);

    /* Must shut off all transmit/receive in order to attempt to 
       achieve Full Duplex */
    opmode = READCSR(sc, R_CSR_OPMODE);
    WRITECSR(sc, R_CSR_OPMODE, opmode &~ (M_CSR6_TXSTART | M_CSR6_RXSTART));
    opmode = READCSR(sc, R_CSR_OPMODE);
    
    WRITECSR(sc, R_CSR_RXRING, PTR_TO_PCI(sc, sc->rxdscr_start));
    WRITECSR(sc, R_CSR_TXRING, PTR_TO_PCI(sc, sc->txdscr_start));

    if (sc->linkspeed == ETHER_SPEED_AUTO)
	mii_autonegotiate(sc);
    else
	mii_set_speed(sc, sc->linkspeed, 0);
}

static void
dm9102_set_speed(tulip_softc *sc, int speed, int autoneg)
{
    dc21140_set_speed(sc, speed, autoneg);
}

static void
dm9102_set_loopback(tulip_softc *sc, int mode)
{
    dc21140_set_loopback(sc, mode);
}


static uint16_t
pnic_mii_read_register(tulip_softc *sc, unsigned int index)
{
    uint32_t cmd, csr20;
    uint16_t data;

    cmd = M_CSR20_BUSY
        | V_CSR20_OPCODE(K_CSR20_OPCODE_READ)
        | V_CSR20_PHYADDR(sc->phy_addr)
        | V_CSR20_REGADDR(index)
        | M_CSR20_MBO;
    WRITECSR(sc, R_CSR_MIIM, cmd);
    for (;;) {
	csr20 = READCSR(sc, R_CSR_MIIM);
	if ((csr20 & M_CSR20_BUSY) == 0)
	    break;
	POLL();   /* XXX need a timeout */
	}
    data = G_CSR20_DATA(csr20);

    return data;
}


static void
pnic_mii_write_register(tulip_softc *sc, unsigned int index, uint16_t value)
{
    uint32_t cmd, csr20;

    cmd = M_CSR20_BUSY
        | V_CSR20_OPCODE(K_CSR20_OPCODE_WRITE)
        | V_CSR20_PHYADDR(sc->phy_addr)
        | V_CSR20_REGADDR(index)
        | V_CSR20_DATA(value)
        | M_CSR20_MBO;
    WRITECSR(sc, R_CSR_MIIM, cmd);
    for (;;) {
	csr20 = READCSR(sc, R_CSR_MIIM);
	if ((csr20 & M_CSR20_BUSY) == 0)
	    break;
	POLL();   /* XXX need a timeout */
	}
}

static void
pnic_hwinit(tulip_softc *sc, uint8_t srom[])
{
    uint8_t gpr_control;
    uint32_t v;
    uint32_t opmode;

    sc->mii_read_register = pnic_mii_read_register;
    sc->mii_write_register = pnic_mii_write_register;

    gpr_control = 0x32;   /* XXX Correct ???  Make symbolic. */
    sc->phy_type = MII;   /* PNIC has built-in PHY with MII */

    WRITECSR(sc, R_CSR_OPMODE, M_CSR6_PORTSEL);
    RESET_ADAPTER(sc);

    /* from NetBSD tulip.c (maybe) */
    WRITECSR(sc, R_CSR_GENPORT, gpr_control);
    WRITECSR(sc, R_CSR_ENDEC, M_CSR15_EG_JDIS);

    mii_probe(sc);

    /* CSR0 - bus mode.  The PNIC requires a magic bit set. */
    v = 0x04000000;
    WRITECSR(sc, R_CSR_BUSMODE, v);

    /* CSR6 - operation mode.  PNIC may require store-and-forward  */
    v = M_CSR6_PORTSEL |
        M_CSR6_STOREFWD |
	M_CSR6_MBO;
    WRITECSR(sc, R_CSR_OPMODE, v);

    /* Must shut off all transmit/receive in order to attempt to 
       achieve Full Duplex */
    opmode = READCSR(sc, R_CSR_OPMODE);
    WRITECSR(sc, R_CSR_OPMODE, opmode &~ (M_CSR6_TXSTART | M_CSR6_RXSTART));
    opmode = READCSR(sc, R_CSR_OPMODE);
    
    WRITECSR(sc, R_CSR_RXRING, PTR_TO_PCI(sc, sc->rxdscr_start));
    WRITECSR(sc, R_CSR_TXRING, PTR_TO_PCI(sc, sc->txdscr_start));

    if (sc->linkspeed == ETHER_SPEED_AUTO)
	mii_autonegotiate(sc);
    else
	mii_set_speed(sc, sc->linkspeed, 0);

    /* Empirically, PNIC requires store-and-forward at 100 Mbs */
    opmode = READCSR(sc, R_CSR_OPMODE);
    opmode |= M_CSR6_STOREFWD;
    WRITECSR(sc, R_CSR_OPMODE, opmode);
}

static void
pnic_set_speed(tulip_softc *sc, int speed, int autoneg)
{
    dc21140_set_speed(sc, speed, autoneg);
}

static void
pnic_set_loopback(tulip_softc *sc, int mode)
{
    dc21140_set_loopback(sc, mode);
}


static void
tulip_hwinit(tulip_softc *sc)
{
    if (sc->state == eth_state_uninit) {
	uint8_t srom[SROM_SIZE];
       
	/* Set default values */
	sc->mii_read_register = mii_read_register;
	sc->mii_write_register = mii_write_register;

	/* Wake-on-LAN apparently powers up with PORTSEL = 1 */
	WRITECSR(sc, R_CSR_OPMODE,
		 READCSR(sc, R_CSR_OPMODE) &~ M_CSR6_PORTSEL);
	
	RESET_ADAPTER(sc);
	sc->state = eth_state_off;
	sc->bus_errors = 0;

	rom_read_all(sc, srom);   /* XXX read just once? */
	rom_dump(srom);
	(void)srom_write_word;    /* XXX suppress compiler warning */

	switch (sc->device) {
	    case K_CHIP_DC21040:
		dc21040_hwinit(sc, srom);
		break;
	    case K_CHIP_DC21041:
		dc21041_hwinit(sc, srom);
		break;
	    case K_CHIP_DC21140:
		dc21140_hwinit(sc, srom);
		break;
	    case K_CHIP_DC21143:
		dc21143_hwinit(sc, srom);
		break;
	    case K_CHIP_DM9102:
		dm9102_hwinit(sc, srom);
		break;
	    case K_CHIP_82C169:
	        pnic_hwinit(sc, srom);
	    default:
		break;
	    }
	}
}

static void
tulip_setaddr(tulip_softc *sc)
{
    int idx;
    tulip_cam *cam;
    eth_pkt_t *pkt;

    pkt = eth_alloc_pkt(sc);
    if (pkt) {
	pkt->length = CAM_SETUP_BUFFER_SIZE;
	cam = (tulip_cam *) pkt->buffer;

#if ENDIAN_BIG
	cam->p.physical[0][0] = (((uint32_t) sc->hwaddr[0] << 8) |
				 (uint32_t) sc->hwaddr[1]) << 16;
	cam->p.physical[0][1] = (((uint32_t) sc->hwaddr[2] << 8) |
	                         (uint32_t) sc->hwaddr[3]) << 16;
	cam->p.physical[0][2] = (((uint32_t) sc->hwaddr[4] << 8) |
				 (uint32_t) sc->hwaddr[5]) << 16;
	for (idx = 1; idx < CAM_PERFECT_ENTRIES; idx++) {
	    cam->p.physical[idx][0] = 0xFFFF0000;
	    cam->p.physical[idx][1] = 0xFFFF0000;
	    cam->p.physical[idx][2] = 0xFFFF0000;
	    }
#else
	cam->p.physical[0][0] = ((uint32_t) sc->hwaddr[0]) |
	    (((uint32_t) sc->hwaddr[1]) << 8);
	cam->p.physical[0][1] = ((uint32_t) sc->hwaddr[2]) |
	    (((uint32_t) sc->hwaddr[3]) << 8);
	cam->p.physical[0][2] = ((uint32_t) sc->hwaddr[4]) |
	    (((uint32_t) sc->hwaddr[5]) << 8);
	for (idx = 1; idx < CAM_PERFECT_ENTRIES; idx++) {
	    cam->p.physical[idx][0] = 0x0000FFFF;
	    cam->p.physical[idx][1] = 0x0000FFFF;
	    cam->p.physical[idx][2] = 0x0000FFFF;
	    }
#endif

	pkt->flags |= ETH_TX_SETUP;
	sc->state = eth_state_setup;
	if (tulip_transmit(sc, pkt) != 0) {
	    xprintf("%s: failed setup\n", tulip_devname(sc));
	    dumpstat(sc);
	    eth_free_pkt(sc, pkt);
	    }
	/* Note: the PNIC apparently will not signal TXINT for this setup
           packet, but will signal TXBUFUNAVAIL. */
	}
}

static void
tulip_setspeed(tulip_softc *sc, int speed)
{
    switch (sc->device) {
	case K_CHIP_DC21040:
	    dc21040_set_speed(sc, speed);
	    break;
	case K_CHIP_DC21041:
	    dc21041_set_speed(sc, speed);
	    break;
	case K_CHIP_DC21140:
	    dc21140_set_speed(sc, speed, 0);
	    break;
	case K_CHIP_DC21143:
	    dc21143_set_speed(sc, speed);
	    break;
	case K_CHIP_DM9102:
	    dm9102_set_speed(sc, speed, 0);
	    break;
	case K_CHIP_82C169:
	    pnic_set_speed(sc, speed, 0);
	    break;
	default:
	    break;
	}
}

static void
tulip_setloopback(tulip_softc *sc, int mode)
{
    switch (sc->device) {
	case K_CHIP_DC21040:
	    dc21040_set_loopback(sc, mode);
	    break;
	case K_CHIP_DC21041:
	    dc21041_set_loopback(sc, mode);
	    break;
	case K_CHIP_DC21140:
	    dc21140_set_loopback(sc, mode);
	    break;
	case K_CHIP_DC21143:
	    dc21143_set_loopback(sc, mode);
	    break;
	case K_CHIP_DM9102:
	    dm9102_set_loopback(sc, mode);
	    break;
	case K_CHIP_82C169:
	    pnic_set_loopback(sc, mode);
	    break;
	default:
	    break;
	}
    cfe_sleep(CFE_HZ/10);
}


static void
tulip_isr(void *arg)
{
    uint32_t status;
    uint32_t csr5;
    tulip_softc *sc = (tulip_softc *)arg;

#if IPOLL
    sc->interrupts++;
#endif

    for (;;) {

	/* Read the interrupt status. */
	csr5 = READCSR(sc, R_CSR_STATUS);
	status = csr5 & (
			 M_CSR5_RXINT | M_CSR5_TXINT | M_CSR5_TXBUFUNAVAIL |
			 M_CSR5_RXBUFUNAVAIL | M_CSR5_TXUNDERFLOW |
			 M_CSR5_FATALBUSERROR);

	/* if there are no more interrupts, leave now. */
	if (status == 0) break;

	/* Clear the pending interrupt. */
	WRITECSR(sc, R_CSR_STATUS, status);

	/* Now, test each unmasked bit in the interrupt register and
           handle each interrupt type appropriately. */

	if (status & M_CSR5_FATALBUSERROR) {
	    WRITECSR(sc, R_CSR_INTMASK, 0);

	    xprintf("%s: bus error %02x\n",
		    tulip_devname(sc), G_CSR5_ERRORBITS(csr5));
	    dumpstat(sc);
	    sc->bus_errors++;
	    if (sc->bus_errors >= 2) {
	        dumpcsrs(sc);
	        RESET_ADAPTER(sc);
		sc->state = eth_state_off;
		sc->bus_errors = 0;
		status = 0;
	        }
#if IPOLL
	    else
	        WRITECSR(sc, R_CSR_INTMASK, sc->intmask);
#endif
	    }

	if (status & M_CSR5_RXINT) {
#if IPOLL
	    sc->rx_interrupts++;
#endif
	    tulip_procrxring(sc);
	    }

	if (status & (M_CSR5_TXINT | M_CSR5_TXBUFUNAVAIL)) {
#if IPOLL
            sc->tx_interrupts++;
#endif
	    tulip_proctxring(sc);
	    }

	if (status & (M_CSR5_TXUNDERFLOW | M_CSR5_RXBUFUNAVAIL)) {
	    if (status & M_CSR5_TXUNDERFLOW) {
		xprintf("%s: tx underrun, %08x\n", tulip_devname(sc), csr5);
		/* Try to restart */
		WRITECSR(sc, R_CSR_TXPOLL, 1);
		}
	    if (status & M_CSR5_RXBUFUNAVAIL) {
		/* Try to restart */
		WRITECSR(sc, R_CSR_RXPOLL, 1);
		}
	    }
	}
}


static void
tulip_start(tulip_softc *sc)
{
    uint32_t opmode;

    tulip_hwinit(sc);

    WRITECSR(sc, R_CSR_RXRING, PTR_TO_PCI(sc, sc->rxdscr_start));
    WRITECSR(sc, R_CSR_TXRING, PTR_TO_PCI(sc, sc->txdscr_start));

    opmode = READCSR(sc, R_CSR_OPMODE);
    opmode &=~ M_CSR6_OPMODE;                   /* no loopback */
    if (sc->loopback != ETHER_LOOPBACK_OFF) {
	opmode &=~ M_CSR6_FULLDUPLEX;
	opmode |= M_CSR6_PORTSEL;
	if (sc->loopback == ETHER_LOOPBACK_EXT)
	    opmode |= M_CSR6_EXTLOOPBACK;
	else
	    opmode |= M_CSR6_INTLOOPBACK;
	}

    sc->intmask = 0;
    WRITECSR(sc, R_CSR_INTMASK, 0);		/* no interrupts */
    WRITECSR(sc, R_CSR_STATUS, 0x1FFFF);        /* clear any pending */
    READCSR(sc, R_CSR_STATUS);                  /* push the write */

    sc->interrupts = 0;
    sc->rx_interrupts = sc->tx_interrupts = 0;

#if IPOLL
    cfe_request_irq(sc->irq, tulip_isr, sc, CFE_IRQ_FLAGS_SHARED, 0);

    sc->intmask =  M_CSR7_RXINT | M_CSR7_TXINT | M_CSR7_TXBUFUNAVAIL |
                   M_CSR7_NORMALINT;
    sc->intmask |= M_CSR7_FATALBUSERROR | M_CSR7_TXUNDERFLOW |
                   M_CSR7_ABNORMALINT;
    WRITECSR(sc, R_CSR_INTMASK, sc->intmask);
#endif

    if (sc->loopback == ETHER_LOOPBACK_OFF) {
	opmode |= M_CSR6_TXSTART;
	WRITECSR(sc, R_CSR_OPMODE, opmode);
	tulip_setaddr(sc);
	}
    else {
	opmode |= M_CSR6_TXSTART | M_CSR6_RXSTART;
	WRITECSR(sc, R_CSR_OPMODE, opmode);
	}
}

static void
tulip_stop(tulip_softc *sc)
{
    uint32_t opmode;
    uint32_t status;
    int count;

    WRITECSR(sc, R_CSR_INTMASK, 0);
    sc->intmask = 0;
#if IPOLL
    cfe_free_irq(sc->irq, 0);
#endif
    WRITECSR(sc, R_CSR_STATUS, 0x1FFFF);
    opmode = READCSR(sc, R_CSR_OPMODE);
    opmode &=~ (M_CSR6_TXSTART | M_CSR6_RXSTART);
    WRITECSR(sc, R_CSR_OPMODE, opmode);

    /* wait for any DMA activity to terminate */
    for (count = 0; count <= 13; count++) {
	status = READCSR(sc, R_CSR_STATUS);
	if ((status & (M_CSR5_RXPROCSTATE | M_CSR5_TXPROCSTATE)) == 0)
	    break;
	cfe_sleep(CFE_HZ/10);
	}
    if (count > 13) {
	xprintf("%s: idle state not achieved\n", tulip_devname(sc));
	dumpstat(sc);
	RESET_ADAPTER(sc);
	sc->state = eth_state_uninit;
#if 1
	sc->linkspeed = ETHER_SPEED_AUTO;
#endif
	}
    else if (sc->loopback != ETHER_LOOPBACK_OFF) {
	tulip_setloopback(sc, ETHER_LOOPBACK_OFF);
	opmode &=~ M_CSR6_OPMODE;
	WRITECSR(sc, R_CSR_OPMODE, opmode);
	}

    if (sc->outpkts > 1) {
	/* heuristic: suppress stats for initial mode changes */
	xprintf("%s: %d sent, %d received, %d interrupts\n",
		tulip_devname(sc), sc->outpkts, sc->inpkts, sc->interrupts);
	xprintf("  %d rx interrupts, %d tx interrupts\n",
		sc->rx_interrupts, sc->tx_interrupts);
	}
}


/*  *********************************************************************
    *  Declarations for CFE Device Driver Interface routines
    ********************************************************************* */

static int tulip_ether_open(cfe_devctx_t *ctx);
static int tulip_ether_read(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int tulip_ether_inpstat(cfe_devctx_t *ctx,iocb_inpstat_t *inpstat);
static int tulip_ether_write(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int tulip_ether_ioctl(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int tulip_ether_close(cfe_devctx_t *ctx);
static void tulip_ether_poll(cfe_devctx_t *ctx, int64_t ticks);
#if 0
static void tulip_ether_reset(void *softc);
#endif

/*  *********************************************************************
    *  CFE Device Driver dispatch structure
    ********************************************************************* */

const static cfe_devdisp_t tulip_ether_dispatch = {
    tulip_ether_open,
    tulip_ether_read,
    tulip_ether_inpstat,
    tulip_ether_write,
    tulip_ether_ioctl,
    tulip_ether_close,
    tulip_ether_poll,
    NULL   /* tulip_ether_reset */
};

/*  *********************************************************************
    *  CFE Device Driver descriptor
    ********************************************************************* */

const cfe_driver_t tulipdrv = {
    "DC21x4x Ethernet",
    "eth",
    CFE_DEV_NETWORK,
    &tulip_ether_dispatch,
    tulip_ether_probe
};


static int
tulip_ether_attach(cfe_driver_t *drv,
		   pcitag_t tag, int index, uint8_t hwaddr[])
{
    tulip_softc *sc;
    uint32_t device;
    uint32_t class;
    uint32_t reg;
    phys_addr_t pa;
    const char *devname;
    char descr[100];
    uint8_t romaddr[ENET_ADDR_LEN];

    device = pci_conf_read(tag, R_CFG_CFID);
    class = pci_conf_read(tag, R_CFG_CFRV);

    reg = pci_conf_read(tag, R_CFG_CPMS);

    reg = pci_conf_read(tag, R_CFG_CFDD);
    pci_conf_write(tag, R_CFG_CFDD, 0);
    reg = pci_conf_read(tag, R_CFG_CFDD);

#if 1
    /* Use memory space for the CSRs */
    pci_map_mem(tag, R_CFG_CBMA, CSR_MATCH_MODE, &pa);
#else
    /* Use i/o space for the CSRs */
    pci_map_io(tag, R_CFG_CBIO, CSR_MATCH_MODE, &pa);
#endif

    sc = (tulip_softc *) KMALLOC(sizeof(tulip_softc), 0);
    if (sc == NULL) {
	xprintf("DC21x4x: No memory to complete probe\n");
	return 0;
	}
    memset(sc, 0, sizeof(*sc));

    sc->membase = (uint32_t)pa;
    sc->irq = pci_conf_read(tag, R_CFG_CFIT) & 0xFF;

    sc->tag = tag;
    sc->device = K_CHIP_UNKNOWN;
    switch (PCI_VENDOR(device)) {
	case K_PCI_VENDOR_DEC:
	    switch (PCI_PRODUCT(device)) {
		case K_PCI_ID_DC21040:
		    sc->device = K_CHIP_DC21040;  break;
		case K_PCI_ID_DC21041:
		    sc->device = K_CHIP_DC21041;  break;
		case K_PCI_ID_DC21140:
		    sc->device = K_CHIP_DC21140;  break;
		case K_PCI_ID_DC21143:
		    sc->device = K_CHIP_DC21143;  break;
		}
	    break;
	case K_PCI_VENDOR_DAVICOM:
	    switch (PCI_PRODUCT(device)) {
		case K_PCI_ID_DM9102:
		    sc->device = K_CHIP_DM9102;  break;
		}
	    break;
	case K_PCI_VENDOR_LITEON:
	    switch (PCI_PRODUCT(device)) {
		case K_PCI_ID_82C169:
		    sc->device = K_CHIP_82C169;  break;
		}
	    break;
	}
    sc->revision = PCI_REVISION(class);
    sc->devctx = NULL;

#if 1
    sc->linkspeed = ETHER_SPEED_AUTO;    /* select autonegotiation */
#else
    sc->linkspeed = ETHER_SPEED_100FDX;  /* 100 Mbps, full duplex */
#endif
    sc->loopback = ETHER_LOOPBACK_OFF;
    memcpy(sc->hwaddr, hwaddr, ENET_ADDR_LEN);

    switch (sc->device) {
	case K_CHIP_DM9102:
	    /* Davicom 9102 has no endian control bit */
#if ENDIAN_BIG
	    sc->swap = 1;
	    sc->rxdscr_owned = swap4(M_RDES0_OWNADAP);
	    sc->txdscr_owned = swap4(M_TDES0_OWNADAP);
#else
	    sc->swap = 0;
	    sc->rxdscr_owned = M_RDES0_OWNADAP;
	    sc->txdscr_owned = M_TDES0_OWNADAP;
#endif
	    sc->endian = M_MATCH_BYTES;
	    /* ... and no ring mode */
	    sc->rxdscr_ctrl = M_RDES1_CHAINED;
	    sc->txdscr_ctrl = M_TDES1_CHAINED;
	    break;
	case K_CHIP_82C169:
	    /* Empirically, LiteOn PNIC has a broken endian control bit. */
#if ENDIAN_BIG
	    sc->swap = 1;
	    sc->rxdscr_owned = swap4(M_RDES0_OWNADAP);
	    sc->txdscr_owned = swap4(M_TDES0_OWNADAP);
#else
	    sc->swap = 0;
	    sc->rxdscr_owned = M_RDES0_OWNADAP;
	    sc->txdscr_owned = M_TDES0_OWNADAP;
#endif
	    sc->endian = M_MATCH_BYTES;
	    break;
        default:
#if (ENDIAN_BIG && MATCH_BYTES)
	    sc->swap = 1;
	    sc->rxdscr_owned = swap4(M_RDES0_OWNADAP);
	    sc->txdscr_owned = swap4(M_TDES0_OWNADAP);
	    sc->endian = M_MATCH_BYTES;
#else
	    sc->swap = 0;
	    sc->rxdscr_owned = M_RDES0_OWNADAP;
	    sc->txdscr_owned = M_TDES0_OWNADAP;
	    sc->endian = M_MATCH_BITS;
#endif
	    sc->rxdscr_ctrl = sc->txdscr_ctrl = 0;
	    break;
	}
    tulip_init(sc);

    /* Prefer address in srom */
    if (rom_read_addr(sc, romaddr) == 0) {
	memcpy(sc->hwaddr, romaddr, ENET_ADDR_LEN);
	}

    sc->state = eth_state_uninit;

    switch (sc->device) {
	case K_CHIP_DC21040:
	    devname = "DC21040";  break;
	case K_CHIP_DC21041:
	    devname = "DC21041";  break;
	case K_CHIP_DC21140:
	    devname = "DC21140";  break;
	case K_CHIP_DC21143:
	    devname = "DC21143";  break;
	case K_CHIP_DM9102:
	    devname = "DM9102";  break;
	case K_CHIP_82C169:
	    devname = "82C169";  break;
        default:
	    devname = "DC21x4x";  break;
	}

    xsprintf(descr, "%s Ethernet at 0x%X (%a)",
	     devname, sc->membase, sc->hwaddr);

    cfe_attach(drv, sc, NULL, descr);
    return 1;
}


/*  *********************************************************************
    *  TULIP_ETHER_PROBE(drv,probe_a,probe_b,probe_ptr)
    *  
    *  Probe and install drivers for all DC21x4x Ethernet controllers.
    *  For each, create a context structure and attach to the
    *  specified network device.
    *  
    *  Input parameters: 
    *  	   drv - driver descriptor
    *  	   probe_a - not used
    *  	   probe_b - not used
    *  	   probe_ptr - string pointer to hardware address for the first
    *  	               MAC, in the form xx:xx:xx:xx:xx:xx
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */
static void
tulip_ether_probe(cfe_driver_t *drv,
		  unsigned long probe_a, unsigned long probe_b, 
		  void *probe_ptr)
{
    int index;
    int n;
    uint8_t hwaddr[ENET_ADDR_LEN];                 

    if (probe_ptr)
	enet_parse_hwaddr((char *) probe_ptr, hwaddr);
    else {
	/* use default address 02-00-00-10-11-11 */
	hwaddr[0] = 0x02;  hwaddr[1] = 0x00;  hwaddr[2] = 0x00;
	hwaddr[3] = 0x10;  hwaddr[4] = 0x11;  hwaddr[5] = 0x11;
	}

    n = 0;
    index = 0;
    for (;;) {
	pcitag_t tag;
	pcireg_t device;

	if (pci_find_class(PCI_CLASS_NETWORK, index, &tag) != 0)
	    break;

	index++;

	device = pci_conf_read(tag, R_CFG_CFID);
	if (PCI_VENDOR(device) == K_PCI_VENDOR_DEC) {
#if 0  /* this currently (2.1.1) generates a bad code in PIC mode */
	    switch (PCI_PRODUCT(device)) {
		case K_PCI_ID_DC21040:
		case K_PCI_ID_DC21041:
		case K_PCI_ID_DC21140:
		case K_PCI_ID_DC21143:
		    tulip_ether_attach(drv, tag, n, hwaddr);
		    n++;
		    enet_incr_hwaddr(hwaddr, 1);
		    break;
		default:
		    break;
		}
#else
	    if (PCI_PRODUCT(device) == K_PCI_ID_DC21040 ||
	        PCI_PRODUCT(device) == K_PCI_ID_DC21041 ||
	        PCI_PRODUCT(device) == K_PCI_ID_DC21140 ||
	        PCI_PRODUCT(device) == K_PCI_ID_DC21143) {

		tulip_ether_attach(drv, tag, n, hwaddr);
		n++;
		enet_incr_hwaddr(hwaddr, 1);
		}
#endif
	    }
	else if (PCI_VENDOR(device) == K_PCI_VENDOR_DAVICOM) {
	    if (PCI_PRODUCT(device) == K_PCI_ID_DM9102) {
		tulip_ether_attach(drv, tag, n, hwaddr);
		n++;
		enet_incr_hwaddr(hwaddr, 1);
		}
	    }
	else if (PCI_VENDOR(device) == K_PCI_VENDOR_LITEON) {
	    if (PCI_PRODUCT(device) == K_PCI_ID_82C169) {
		tulip_ether_attach(drv, tag, n, hwaddr);
		n++;
		enet_incr_hwaddr(hwaddr, 1);
		}
	    }
	}
}


/* The functions below are called via the dispatch vector for the 21x4x. */

/*  *********************************************************************
    *  TULIP_ETHER_OPEN(ctx)
    *  
    *  Open the Ethernet device.  The MAC is reset, initialized, and
    *  prepared to receive and send packets.
    *  
    *  Input parameters: 
    *  	   ctx - device context (includes ptr to our softc)
    *  	   
    *  Return value:
    *  	   status, 0 = ok
    ********************************************************************* */
static int
tulip_ether_open(cfe_devctx_t *ctx)
{
    tulip_softc *sc = ctx->dev_softc;

    if (sc->state == eth_state_on)
	tulip_stop(sc);

    sc->devctx = ctx;
    tulip_start(sc);

#if XPOLL
    tulip_isr(sc);
#endif

    return 0;
}

/*  *********************************************************************
    *  TULIP_ETHER_READ(ctx,buffer)
    *  
    *  Read a packet from the Ethernet device.  If no packets are
    *  available, the read will succeed but return 0 bytes.
    *  
    *  Input parameters: 
    *  	   ctx - device context (includes ptr to our softc)
    *      buffer - pointer to buffer descriptor.  
    *  	   
    *  Return value:
    *  	   status, 0 = ok
    ********************************************************************* */
static int
tulip_ether_read(cfe_devctx_t *ctx, iocb_buffer_t *buffer)
{
    tulip_softc *sc = ctx->dev_softc;
    eth_pkt_t *pkt;
    int blen;

#if XPOLL
    tulip_isr(sc);
#endif

    if (sc->state != eth_state_on) return -1;

    CS_ENTER(sc);
    pkt = (eth_pkt_t *) q_deqnext(&(sc->rxqueue));
    CS_EXIT(sc);

    if (pkt == NULL) {
	buffer->buf_retlen = 0;
	return 0;
	}

    blen = buffer->buf_length;
    if (blen > pkt->length) blen = pkt->length;

    hs_memcpy_to_hs(buffer->buf_ptr, pkt->buffer, blen);
    buffer->buf_retlen = blen;

    eth_free_pkt(sc, pkt);
    tulip_fillrxring(sc);

#if XPOLL
    tulip_isr(sc);
#endif

    return 0;
}

/*  *********************************************************************
    *  TULIP_ETHER_INPSTAT(ctx,inpstat)
    *  
    *  Check for received packets on the Ethernet device
    *  
    *  Input parameters: 
    *  	   ctx - device context (includes ptr to our softc)
    *      inpstat - pointer to input status structure
    *  	   
    *  Return value:
    *  	   status, 0 = ok
    ********************************************************************* */
static int
tulip_ether_inpstat(cfe_devctx_t *ctx, iocb_inpstat_t *inpstat)
{
    tulip_softc *sc = ctx->dev_softc;

#if XPOLL
    tulip_isr(sc);
#endif

    if (sc->state != eth_state_on) return -1;

    /* We avoid an interlock here because the result is a hint and an
       interrupt cannot turn a non-empty queue into an empty one. */
    inpstat->inp_status = (q_isempty(&(sc->rxqueue))) ? 0 : 1;

    return 0;
}

/*  *********************************************************************
    *  TULIP_ETHER_WRITE(ctx,buffer)
    *  
    *  Write a packet to the Ethernet device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (includes ptr to our softc)
    *      buffer - pointer to buffer descriptor.  
    *  	   
    *  Return value:
    *  	   status, 0 = ok
    ********************************************************************* */
static int
tulip_ether_write(cfe_devctx_t *ctx, iocb_buffer_t *buffer)
{
    tulip_softc *sc = ctx->dev_softc;
    eth_pkt_t *pkt;
    int blen;

#if XPOLL
    tulip_isr(sc);
#endif

    if (sc->state != eth_state_on) return -1;

    pkt = eth_alloc_pkt(sc);
    if (!pkt) return CFE_ERR_NOMEM;

    blen = buffer->buf_length;
    if (blen > pkt->length) blen = pkt->length;

    hs_memcpy_from_hs(pkt->buffer, buffer->buf_ptr, blen);
    pkt->length = blen;

    if (tulip_transmit(sc, pkt) != 0) {
	eth_free_pkt(sc,pkt);
	return CFE_ERR_IOERR;
	}

#if XPOLL
    tulip_isr(sc);
#endif

    return 0;
}

/*  *********************************************************************
    *  TULIP_ETHER_IOCTL(ctx,buffer)
    *  
    *  Do device-specific I/O control operations for the device
    *  
    *  Input parameters: 
    *  	   ctx - device context (includes ptr to our softc)
    *      buffer - pointer to buffer descriptor.  
    *  	   
    *  Return value:
    *  	   status, 0 = ok
    ********************************************************************* */
static int
tulip_ether_ioctl(cfe_devctx_t *ctx, iocb_buffer_t *buffer) 
{
    tulip_softc *sc = ctx->dev_softc;
    int  *argp;
    int   mode;
    int   speed;

    switch ((int)buffer->buf_ioctlcmd) {
	case IOCTL_ETHER_GETHWADDR:
	    hs_memcpy_to_hs(buffer->buf_ptr, sc->hwaddr, sizeof(sc->hwaddr));
	    return 0;

	case IOCTL_ETHER_SETHWADDR:
	    return -1;    /* not supported */

	case IOCTL_ETHER_GETSPEED:
	    argp = (int *) buffer->buf_ptr;
	    *argp = sc->linkspeed;
	    return 0;

	case IOCTL_ETHER_SETSPEED:
	    tulip_stop(sc);
	    tulip_resetrings(sc);
	    speed = *((int *) buffer->buf_ptr);
	    tulip_setspeed(sc, speed);
	    tulip_start(sc);
	    sc->state = eth_state_on;
	    return 0;

	case IOCTL_ETHER_GETLINK:
	    argp = (int *) buffer->buf_ptr;
	    *argp = sc->linkspeed;
	    return 0;

	case IOCTL_ETHER_GETLOOPBACK:
	    *((int *) buffer) = sc->loopback;
	    return 0;

	case IOCTL_ETHER_SETLOOPBACK:
	    tulip_stop(sc);
	    tulip_resetrings(sc);
	    mode = *((int *) buffer->buf_ptr);
	    sc->loopback = ETHER_LOOPBACK_OFF;  /* default */
	    if (mode == ETHER_LOOPBACK_INT || mode == ETHER_LOOPBACK_EXT) {
		tulip_setloopback(sc, mode);
		}
	    tulip_start(sc);
	    sc->state = eth_state_on;
	    return 0;

	default:
	    return -1;
	}
}

/*  *********************************************************************
    *  TULIP_ETHER_CLOSE(ctx)
    *  
    *  Close the Ethernet device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (includes ptr to our softc)
    *  	   
    *  Return value:
    *  	   status, 0 = ok
    ********************************************************************* */
static int
tulip_ether_close(cfe_devctx_t *ctx)
{
    tulip_softc *sc = ctx->dev_softc;

    sc->state = eth_state_off;
    tulip_stop(sc);

    /* resynchronize descriptor rings */
    tulip_resetrings(sc);

    sc->devctx = NULL;
    return 0;
}


/*  *********************************************************************
    *  TULIP_ETHER_POLL(ctx,ticks)
    *  
    *  TBD
    *  
    *  Input parameters: 
    *  	   ctx - device context (includes ptr to our softc)
    *      ticks- current time in ticks
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */

static void
tulip_ether_poll(cfe_devctx_t *ctx, int64_t ticks)
{
#if XPOLL
    tulip_softc *sc = ctx->dev_softc;

    tulip_isr(sc);
#endif
}

#if 0
/*  *********************************************************************
    *  TULIP_ETHER_RESET(softc)
    *  
    *  This routine is called when CFE is restarted after a 
    *  program exits.  We can clean up pending I/Os here.
    *  
    *  Input parameters: 
    *  	   softc - pointer to tulip_softc
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */

static void
tulip_ether_reset(void *softc)
{
    tulip_softc *sc = (tulip_softc *)softc;

    /* Turn off the Ethernet interface. */

    RESET_ADAPTER(sc);
}
#endif



/* The functions below are for support of serial EEPROM manipulation. */

uint16_t tulip_srom_read(void *softc, unsigned int addr);
void tulip_srom_enable(void *softc, int enable);
int tulip_srom_write(void *softc, unsigned int addr, uint16_t word);

uint16_t
tulip_srom_read(void *softc, unsigned int addr)
{
    tulip_softc *sc = (tulip_softc *)softc;
    uint32_t csr9;
    uint16_t v;

    if (sc->device == K_CHIP_DC21040) {
	xprintf("%s: SROM read not supported\n", tulip_devname(sc));
	return 0xFFFF;
	}

    csr9 = READCSR(sc, R_CSR_ROM_MII);
    WRITECSR(sc, R_CSR_ROM_MII, M_CSR9_SERROMSEL|M_CSR9_ROMREAD);
    srom_idle_state(sc);

    v = srom_read_word(sc, addr);

    WRITECSR(sc, R_CSR_ROM_MII, 0/*csr9*/);
    return v;
}

void
tulip_srom_enable(void *softc, int enable)
{
    tulip_softc *sc = (tulip_softc *)softc;
    uint32_t csr9;

    if (sc->device == K_CHIP_DC21040) {
	xprintf("%s: SROM enable not supported\n", tulip_devname(sc));
	return;
	}

    csr9 = READCSR(sc, R_CSR_ROM_MII);
    WRITECSR(sc, R_CSR_ROM_MII, M_CSR9_SERROMSEL|M_CSR9_ROMWRITE);
    srom_idle_state(sc);

    srom_set_enable(sc, enable);

    WRITECSR(sc, R_CSR_ROM_MII, 0/*csr9*/);
}


int
tulip_srom_write(void *softc, unsigned int addr, uint16_t word)
{
    tulip_softc *sc = (tulip_softc *)softc;
    uint32_t csr9;
    int rv;

    if (sc->device == K_CHIP_DC21040) {
	xprintf("%s: SROM write not supported\n", tulip_devname(sc));
	return -1;
	}

    csr9 = READCSR(sc, R_CSR_ROM_MII);
    WRITECSR(sc, R_CSR_ROM_MII, M_CSR9_SERROMSEL|M_CSR9_ROMWRITE);
    srom_idle_state(sc);

    rv = srom_write_word(sc, addr, word);

    WRITECSR(sc, R_CSR_ROM_MII, 0/*csr9*/);
    return rv;
}
