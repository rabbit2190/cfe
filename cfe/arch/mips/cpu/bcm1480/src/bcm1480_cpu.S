/*  *********************************************************************
    *  BCM1280/BCM1480 Board Support Package
    *  
    *  CPU initialization			File: bcm1480_cpu.S
    *  
    *  This module contains code to initialize the CPU.
    *  
    *  Note: all the routines in this module rely on registers only,
    *        since DRAM may not be active yet.
    *
    *  Author:  Mitch Lichtenberg
    *  
    *********************************************************************  
    *
    *  Copyright 2000,2001,2002,2003,2004
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

#include "sbmips.h"
#include "bsp_config.h"
#include "cpu_config.h"
#include "mipsmacros.h"
#include "exception.h"

#include "bcm1480_regs.h"
#include "bcm1480_scd.h"


		.text
		.set mips64


/*  *********************************************************************
    *  Macros
    ********************************************************************* */

/*
 * Duplicates from cfe_iocb.h -- warning!
 */

#define CFE_CACHE_FLUSH_D	1
#define CFE_CACHE_INVAL_I	2
#define CFE_CACHE_INVAL_D	4
#define CFE_CACHE_INVAL_L2	8
#define CFE_CACHE_FLUSH_L2	16
#define CFE_CACHE_INVAL_RANGE	32
#define CFE_CACHE_FLUSH_RANGE	64

#define L1CACHE_NUMWAYS	4
#define L1CACHE_NUMIDX  256
#define L1CACHE_LINESIZE 32
#define L1CACHE_IDXHIGH (L1CACHE_LINESIZE*L1CACHE_NUMWAYS*L1CACHE_NUMIDX)

#define L1CACHEOP(cachename,op) ((cachename) | ((op) << 2))

#define L1C_OP_IDXINVAL     0
#define L1C_OP_IDXLOADTAG   1
#define L1C_OP_IDXSTORETAG  2
#define L1C_OP_IMPLRSVD     3
#define L1C_OP_HITINVAL     4
#define L1C_OP_FILL         5
#define L1C_OP_HITWRITEBACK 6
#define L1C_OP_FETCHLOCK    7

#define L1C_I		    0
#define L1C_D		    1


/*  *********************************************************************
    *  LED macros
    ********************************************************************* */

#define SETLEDS1(a,b,c,d)                     \
       li     a0,(((a)<<24)|((b)<<16)|((c)<<8)|(d)) ;    \
       JAL_KSEG1(board_setleds)
#define SETLEDS(a,b,c,d)                     \
       li     a0,(((a)<<24)|((b)<<16)|((c)<<8)|(d)) ;    \
       JAL_KSEG1(board_setleds)




/*  *********************************************************************
    *  BCM1480_CPUINIT
    *  
    *  Initialize the CPU core.
    *  
    *  Input parameters: 
    *  	   nothing
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */

LEAF(bcm1480_cpuinit)

		move	k0,ra			/* will be trashing RA */


	/*
	 * Clear the SOFTRES bit in the SYSTEM_CONFIG register.
	 */

		li	t0,PHYS_TO_K1(A_SCD_SYSTEM_CFG)
		ld	t1,0(t0)
		dli	t2,~M_BCM1480_SYS_SB_SOFTRES
		and	t1,t1,t2
		BCM1480_WRITE_SYSTEM_CFG(t0, t1)


	/*
	 * Note that the initialization order here is important.
	 * In particular, the cache needs to be running before
	 * DRAM is turned on, since the DRAM init code is 
	 * in cacheable space.  This code assumes that we are
	 * linked at KSEG0's address but are executed starting
	 * in KSEG1.  Therefore, we need to be somewhat careful
	 * until the cache is alive to call routines
	 * making sure we are calling in KSEG1.
	 */

		JAL_KSEG1(sb1_cpu_init)


#------------------------------------------------------------------------------

	/*
	 * Init the L1 cache.  
	 */

#if CFG_INIT_L1
		SETLEDS1('L','1','C','I')
		JAL_KSEG1(bcm1480_l1cache_init)
#endif

	/*
         * Init the L2 cache.  We don't init L2 on 
	 * secondary CPU(s), since they are shared by all CPUs.  We'll
         * do it here on CPU0.
         */

#if CFG_INIT_L2
	 	SETLEDS1('L','2','C','I')
		JAL_KSEG1(bcm1480_l2cache_init)
#endif

		move	ra,k0			/* saved return address */
		j	ra


END(bcm1480_cpuinit)

/*  *********************************************************************
    *  BCM1480_CPURESTART
    *  
    *  'Restart' the CPU (reset things back to some sane state after
    *  a program returns to the firmware)
    *  
    *  Input parameters: 
    *  	   nothing
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */

LEAF(bcm1480_cpurestart)

		move	k0,ra

		JAL(sb1_cp0_init)

		LR	v0,cfe_pagetable		# reestablish 
		dsll	v0,v0,13			# see mips_arena.c for this
		dmtc0	v0,C0_CTEXT			# boot area TLBs

		move	ra,k0
		j	ra

END(bcm1480_cpurestart)


/*  *********************************************************************
    *  BCM1480_CACHEOPS
    *  
    *  This routine handles the various cache-related functions
    *  needed by CFE.
    *  
    *  Input parameters: 
    *  	   a0 - cache functions (bit mask)
    *  	   
    *  Return value:
    *  	   nothing
    *  	   
    *  Registers used:
    *  	   trashes t0..t7, a0, v0  (especially for L2)
    ********************************************************************* */

LEAF(bcm1480_cacheops)

		move	s0,ra

		move	v1,a0

	/*
	 * With no flags, we flush L1D and invalid L1I
	 */

		bne	v1,zero,1f
		li	v1,CFE_CACHE_FLUSH_D | CFE_CACHE_INVAL_I
1:

	/*
	 * Flush the D-Cache, since the program we loaded is "data".
	 */

		and	a0,v1,CFE_CACHE_FLUSH_D
		beq	a0,zero,1f
		JAL(bcm1480_l1cache_flush_d)
1:

	/*
	 * Invalidate the I-Cache, so that addresses in the program
	 * region will miss and need to be filled from the data we 
	 * just flushed above.
	 */

		and	a0,v1,CFE_CACHE_INVAL_I
		beq	a0,zero,1f
		JAL(bcm1480_l1cache_inval_i)
1:

	/*
	 * Invalidate the L2, if requested.  Use this cautiously,
	 * since it invalidates both I and D!
	 */

		and	a0,v1,CFE_CACHE_INVAL_L2
		beq	a0,zero,1f
		JAL(bcm1480_l2cache_init)
1:


	/*
	 * Invalidate cache range
	 */

		and	a0,v1,CFE_CACHE_INVAL_RANGE
		beq	a0,zero,2f

		move	t0,a1
1:		cache	L1CACHEOP(L1C_D,L1C_OP_HITINVAL),0(t0)
		add	t0,L1CACHE_LINESIZE
		blt	t0,a2,1b

	/*
	 * Flush cache range
	 */
		

2:
		and	a0,v1,CFE_CACHE_FLUSH_RANGE
		beq	a0,zero,2f

		move	t0,a1
1:		cache	L1CACHEOP(L1C_D,L1C_OP_HITWRITEBACK),0(t0)
		add	t0,L1CACHE_LINESIZE
		blt	t0,a2,1b

2:

	/*
	 * Invalidate the L2, if requested.  Use this cautiously,
	 * since it invalidates both I and D!
	 */

		and	a0,v1,CFE_CACHE_FLUSH_L2
		beq	a0,zero,1f
		JAL(bcm1480_l2cache_flush)	/* Trashes T0..T7, V0, A0 */
1:

		move	ra,s0
		j	ra

END(bcm1480_cacheops)


/*  *********************************************************************
    *  BCM1480_TLBHANDLER
    *  
    *  This is the TLB exception handler for the BCM1480.
    *  
    *  Note: only K0 and K1 are available to us at this time.
    *  
    *  Input parameters: 
    *  	   nothing
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */


LEAF(bcm1480_tlbhandler)
                .set    noreorder
                .set    noat

/*
 * This requires a bit of explanation:  We only support 256KB
 * of mapped space for the boot program.  This space will be 
 * mapped from 0x2000_0000 to 0x2004_0000 to some physical 
 * memory allocated by the firmware.  This is 64 pages
 * of 4KB each.
 *
 * We know our BadVPN2 will be in the range
 * 0x100000 to 0x1001F0, since the memory is mapped from
 * 0x2000_0000 to 0x2004_0000.  BadVPN2 plus the four bits
 * of zeroes at the end are bits 31..9
 * 
 * We also want to place the PTEbase on something other than
 * a 16MB boundary.  Each entry is 16 bytes, and there
 * are 64 entries, so we need only 10 bits to address
 * the entire table (it can therefore be aligned on a
 * 1KB boundary).
 *
 * To make this work, we'll shift PTEbase to the right, leaving
 * the bottom ten bits for the page number, as:
 *
 *    Bits 31..10: PTEbase
 *    Bits 9..4:   BadVPN
 *    Bits 3..0:   16 bytes for table entry
 *
 * Therefore:
 *    PTEbase gets shifted right 13 bits.
 *    BadVPN  gets masked at 6 bits (mask is 0x3F0)
 *    The bottom 4 bits are zero.
 *
 * To range check the address, we can shift the Bad VPN
 * right by 9 bits, and check for values of 0x1000 and
 * 0x1001.
 */


	/*
	 * This part range checks the VPN2 field in the 
	 * context register.  We only handle
	 * VPN2s in the range 0x100000 to 0x1001F0
	 */
		dmfc0	k0,C0_TLBHI

		dmfc0	k0,C0_CTEXT		# Get context
		dsra	k0,8			# keep hi part
		and	k0,0x1FFF		# of VPN2
		li	k1,0x1000		# 0x1000 is ok
		beq	k0,k1,1f		#
		nop				# BDSLOT
		li	k1,0x1001		# 0x1001 is ok
		beq	k0,k1,1f		#
		nop				# BDSLOT

		li	k0,XTYPE_TLBFILL	# all other bits are not
		JMP(_exc_entry)
		nop				# BDSLOT

1:		dmfc0	k0,C0_CTEXT		# Get context
		dsra	k0,13			# Shift PTEbase 
		li	k1,0x3FF		# Generate mask to kill 
		not	k1			# BadVPN2 bits
		and	k0,k1			# keep only PTEBase part.

		dmfc0	k1,C0_CTEXT		# Get Context
		and	k1,0x3F0		# Keep only BadVPN2 bits
		or	k1,k0			# Replace PTEBase

		ld	k0,0(k1)		# Load entrylo0
		ld	k1,8(k1)		# Load entrylo1
		mtc0	k0,C0_TLBLO0		# and write to CP0
		mtc0	k1,C0_TLBLO1
		tlbwr				# put it in the TLB
		eret
		nop

		.set	reorder
		.set	at

END(bcm1480_tlbhandler)

/*  *********************************************************************
    *  bcm1480_cerrhandler
    *  
    *  Cache error handler.  We get called here on a cache error
    *  exception.   Registers k0, k1, gp, and sp have been saved
    *  for us.  We are running uncached, with ERL set, so kuseg
    *  is mapped into uncached memory.
    *  
    *  Input parameters: 
    *  	   nothing
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */

LEAF(bcm1480_cerrhandler)
        
		mfc0	k0, $26
		bgez	k0, real_cerr

		mtc0	zero, $26
		mtc0	zero, $27
		mtc0	zero, $27, 1
		mtc0	zero, $27, 3
		eret

real_cerr:	SETLEDS1('C','e','r','r')

1:		b	1b		/* death */

END(bcm1480_cerrhandler)


/*  *********************************************************************
    *  End
    ********************************************************************* */


