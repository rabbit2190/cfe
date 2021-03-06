/*  *********************************************************************
    *  Broadcom Common Firmware Environment (CFE)
    *  
    *  Board device initialization		File: mousse.h
    *  
    *  Stuff particular to the Mousse board
    *  
    *  Author:  Mitch Lichtenberg
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


#define A_MOUSSE_EUMBBAR_VAL	0xFC000000
#define A_MOUSSE_EUMBSIZE	0x00100000

#define PORTX_BASE	0xFF000000	/* base of X-bus on MPC8240 */

#define A_MOUSSE_PLD		(PORTX_BASE+0xE09000)
#define A_MOUSSE_TOYCLOCK	(PORTX_BASE+0xE0A000)
#define A_MOUSSE_SERIAL_0	(PORTX_BASE+0xE08000)
#define A_MOUSSE_SERIAL_1	(PORTX_BASE+0xE08080)

#define MOUSSE_TOYCLOCK_SIZE	8192

#define R_MOUSSE_PLD_REVID	0
#define R_MOUSSE_PLD_LED	1
#define R_MOUSSE_PLD_WDOG	2
#define R_MOUSSE_PLD_TOYPROT	3

#define M_MOUSSE_PLD_FLG	0x80
#define M_MOUSSE_PLD_REVID	0x7F


