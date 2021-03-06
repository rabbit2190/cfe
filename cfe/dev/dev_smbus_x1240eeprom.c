/*  *********************************************************************
    *  Broadcom Common Firmware Environment (CFE)
    *  
    *  Xicor RTC/EEPROM driver		File: dev_smbus_x1240eeprom.c
    *  
    *  This module contains a CFE driver for a Xicor X1240 SMBus
    *  real-time-clock & EEPROM module.  The only functionality
    *  we currently export is the EEPROM, for use as environment
    *  storage.
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


#include "cfe.h"
#include "cfe_smbus.h"


/*  *********************************************************************
    *  Xicor X1241 RTC constants
    ********************************************************************* */

/*
 * Register bits
 */

#define X1241REG_SR_BAT	0x80		/* currently on battery power */
#define X1241REG_SR_RWEL 0x04		/* r/w latch is enabled, can write RTC */
#define X1241REG_SR_WEL 0x02		/* r/w latch is unlocked, can enable r/w now */
#define X1241REG_SR_RTCF 0x01		/* clock failed */
#define X1241REG_BL_BP2 0x80		/* block protect 2 */
#define X1241REG_BL_BP1 0x40		/* block protect 1 */
#define X1241REG_BL_BP0 0x20		/* block protect 0 */
#define X1241REG_BL_WD1	0x10
#define X1241REG_BL_WD0	0x08
#define X1241REG_HR_MIL 0x80		/* military time format */

/*
 * Register numbers
 */

#define X1241REG_BL	0x10		/* block protect bits */
#define X1241REG_INT	0x11		/*  */
#define X1241REG_SC	0x30		/* Seconds */
#define X1241REG_MN	0x31		/* Minutes */
#define X1241REG_HR	0x32		/* Hours */
#define X1241REG_DT	0x33		/* Day of month */
#define X1241REG_MO	0x34		/* Month */
#define X1241REG_YR	0x35		/* Year */
#define X1241REG_DW	0x36		/* Day of Week */
#define X1241REG_Y2K	0x37		/* Year 2K */
#define X1241REG_SR	0x3F		/* Status register */

#define X1241_CCR_ADDRESS	0x6F
#define X1241_ARRAY_ADDRESS	0x57

#define X1241_EEPROM_SIZE 2048

/*  *********************************************************************
    *  Forward Declarations
    ********************************************************************* */

static void sb1250_x1240eeprom_probe(cfe_driver_t *drv,
				     unsigned long probe_a, unsigned long probe_b, 
				     void *probe_ptr);


static int sb1250_x1240eeprom_open(cfe_devctx_t *ctx);
static int sb1250_x1240eeprom_read(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int sb1250_x1240eeprom_inpstat(cfe_devctx_t *ctx,iocb_inpstat_t *inpstat);
static int sb1250_x1240eeprom_write(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int sb1250_x1240eeprom_ioctl(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int sb1250_x1240eeprom_close(cfe_devctx_t *ctx);

/*  *********************************************************************
    *  Dispatch tables
    ********************************************************************* */

const static cfe_devdisp_t sb1250_x1240eeprom_dispatch = {
    sb1250_x1240eeprom_open,
    sb1250_x1240eeprom_read,
    sb1250_x1240eeprom_inpstat,
    sb1250_x1240eeprom_write,
    sb1250_x1240eeprom_ioctl,
    sb1250_x1240eeprom_close,
    NULL,
    NULL
};

const cfe_driver_t smbus_x1240eeprom = {
    "Xicor X1241 EEPROM",
    "eeprom",
    CFE_DEV_NVRAM,
    &sb1250_x1240eeprom_dispatch,
    sb1250_x1240eeprom_probe
};

typedef struct sb1250_x1240eeprom_s {
    cfe_smbus_channel_t *smbus_channel;
    int env_offset;
    int env_size;
} sb1250_x1240eeprom_t;




/*  *********************************************************************
    *  smbus_readrtc(chan,slaveaddr,devaddr)
    *  
    *  Read a byte from the chip.  The 'slaveaddr' parameter determines
    *  whether we're reading from the RTC section or the EEPROM section.
    *  
    *  Input parameters: 
    *  	   chan - SMBus channel
    *  	   slaveaddr -  SMBus slave address
    *  	   devaddr - byte with in the X1240 device to read
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   else -1
    ********************************************************************* */

static int smbus_readrtc(cfe_smbus_channel_t *chan,int slaveaddr,int devaddr)
{
    uint8_t buf[2];
    int err;

    /*
     * Write the device address to the controller. There are two
     * parts, the high part goes in the "CMD" field, and the 
     * low part is the data field.
     */

    buf[0] = (devaddr >> 8) & 0x07;
    buf[1] = (devaddr & 0xFF);

    err = SMBUS_WRITE(chan,slaveaddr,buf,2);
    if (err < 0) return err;

    /*
     * Read the data byte
     */

    err = SMBUS_READ(chan,slaveaddr,buf,1);
    if (err < 0) return err;

    return (buf[0]);
}

/*  *********************************************************************
    *  smbus_writertc(chan,slaveaddr,devaddr,b)
    *  
    *  write a byte from the chip.  The 'slaveaddr' parameter determines
    *  whethe we're writing to the RTC section or the EEPROM section.
    *  
    *  Input parameters: 
    *  	   chan - SMBus channel
    *  	   slaveaddr -  SMBus slave address
    *  	   devaddr - byte with in the X1240 device to read
    *      b - byte to write
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   else -1
    ********************************************************************* */


static int smbus_writertc(cfe_smbus_channel_t *chan,int slaveaddr,int devaddr,int b)
{
    uint8_t buf[3];
    int err;
    int64_t timer;


    /*
     * Write the data to the controller
     */


    buf[0] = (devaddr >> 8) & 0x07;
    buf[1] = (devaddr & 0xFF);
    buf[2] = b;

    err = SMBUS_WRITE(chan,slaveaddr,buf,3);
    if (err < 0) return err;

    /*
     * Pound on the device with a current address read
     * to poll for the write complete
     */

    TIMER_SET(timer,50);
    err = -1; 

    while (!TIMER_EXPIRED(timer)) {
	POLL();

	err = SMBUS_READ(chan,slaveaddr,buf,1);
	if (err == 0) break;
	}

    return err;
}


/*  *********************************************************************
    *  sb1250_x1240eeprom_probe(drv,a,b,ptr)
    *  
    *  Probe routine for this driver.  This routine creates the 
    *  local device context and attaches it to the driver list
    *  within CFE.
    *  
    *  Input parameters: 
    *  	   drv - driver handle
    *  	   a,b - probe hints (longs)
    *  	   ptr - probe hint (pointer)
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */

static void sb1250_x1240eeprom_probe(cfe_driver_t *drv,
				     unsigned long probe_a, unsigned long probe_b, 
				     void *probe_ptr)
{
    sb1250_x1240eeprom_t *softc;
    char descr[80];

    softc = (sb1250_x1240eeprom_t *) KMALLOC(sizeof(sb1250_x1240eeprom_t),0);

    /*
     * Probe_a is the SMBus channel number
     * Probe_b is the SMBus device offset
     * Probe_ptr is unused.
     */

    softc->smbus_channel = SMBUS_CHANNEL((int)probe_a);
    softc->env_offset  = 0;
    softc->env_size = X1241_EEPROM_SIZE;

    xsprintf(descr,"%s on SMBus channel %d dev 0x%02X",
	     drv->drv_description,probe_a,X1241_ARRAY_ADDRESS);
    cfe_attach(drv,softc,NULL,descr);
}



/*  *********************************************************************
    *  sb1250_x1240eeprom_open(ctx)
    *  
    *  Open this device.  For the X1240, we do a quick test 
    *  read to be sure the device is out there.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   else error code
    ********************************************************************* */

static int sb1250_x1240eeprom_open(cfe_devctx_t *ctx)
{
    sb1250_x1240eeprom_t *softc = ctx->dev_softc;
    int b;
    int64_t timer;

    /*
     * Try to read byte 0 from the device.  If it does not
     * respond, fail the open.  We may need to do this for
     * up to 300ms in case the X1240 is busy wiggling its
     * RESET line.
     */

    TIMER_SET(timer,300);
    while (!TIMER_EXPIRED(timer)) {
	POLL();
	b = smbus_readrtc(softc->smbus_channel,
			  X1241_ARRAY_ADDRESS,
			  0);
	if (b >= 0) break;		/* read is ok */
	}

    /*
     * See if the watchdog is enabled.  If it is, turn it off.
     */

    b = smbus_readrtc(softc->smbus_channel,
		      X1241_CCR_ADDRESS,
		      X1241REG_BL);

    if (b != (X1241REG_BL_WD1 | X1241REG_BL_WD0)) {

	smbus_writertc(softc->smbus_channel,
		       X1241_CCR_ADDRESS,
		       X1241REG_SR,
		       X1241REG_SR_WEL);

	smbus_writertc(softc->smbus_channel,
		       X1241_CCR_ADDRESS,
		       X1241REG_SR,
		       X1241REG_SR_WEL | X1241REG_SR_RWEL);

	smbus_writertc(softc->smbus_channel,
		       X1241_CCR_ADDRESS,
		       X1241REG_BL,
		       (X1241REG_BL_WD1 | X1241REG_BL_WD0));

	smbus_writertc(softc->smbus_channel,
		       X1241_CCR_ADDRESS,
		       X1241REG_SR,
		       0);
	}



    return (b < 0) ? -1 : 0;
}

/*  *********************************************************************
    *  sb1250_x1240eeprom_read(ctx,buffer)
    *  
    *  Read bytes from the device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   buffer - buffer descriptor (target buffer, length, offset)
    *  	   
    *  Return value:
    *  	   number of bytes read
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_x1240eeprom_read(cfe_devctx_t *ctx,iocb_buffer_t *buffer)
{
    sb1250_x1240eeprom_t *softc = ctx->dev_softc;
    hsaddr_t bptr;
    int blen;
    int idx;
    int b = 0;

    bptr = buffer->buf_ptr;
    blen = buffer->buf_length;

    if ((buffer->buf_offset + blen) > X1241_EEPROM_SIZE) return -1;

    idx = (int) buffer->buf_offset;

    while (blen > 0) {
	b = smbus_readrtc(softc->smbus_channel,
			  X1241_ARRAY_ADDRESS,
			  idx);
	if (b < 0) break;
	hs_write8(bptr,(unsigned char)b);
	bptr++;
	blen--;
	idx++;
	}

    buffer->buf_retlen = bptr - buffer->buf_ptr;
    return (b < 0) ? -1 : 0;
}

/*  *********************************************************************
    *  sb1250_x1240eeprom_inpstat(ctx,inpstat)
    *  
    *  Test input (read) status for the device
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   inpstat - input status descriptor to receive value
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_x1240eeprom_inpstat(cfe_devctx_t *ctx,iocb_inpstat_t *inpstat)
{
    inpstat->inp_status = 1;

    return 0;
}

/*  *********************************************************************
    *  sb1250_x1240eeprom_write(ctx,buffer)
    *  
    *  Write bytes from the device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   buffer - buffer descriptor (target buffer, length, offset)
    *  	   
    *  Return value:
    *  	   number of bytes read
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_x1240eeprom_write(cfe_devctx_t *ctx,iocb_buffer_t *buffer)
{
    sb1250_x1240eeprom_t *softc = ctx->dev_softc;
    hsaddr_t bptr;
    int blen;
    int idx;
    int b = 0;

    bptr = buffer->buf_ptr;
    blen = buffer->buf_length;

    if ((buffer->buf_offset + blen) > X1241_EEPROM_SIZE) return -1;

    idx = (int) buffer->buf_offset;

    smbus_writertc(softc->smbus_channel,
		   X1241_CCR_ADDRESS,
		   X1241REG_SR,
		   X1241REG_SR_WEL);


    while (blen > 0) {
	b = hs_read8(bptr);
	bptr++;
	b = smbus_writertc(softc->smbus_channel,
			   X1241_ARRAY_ADDRESS,
			   idx,
			   b);
	if (b < 0) break;
	blen--;
	idx++;
	}

    smbus_writertc(softc->smbus_channel,
		   X1241_CCR_ADDRESS,
		   X1241REG_SR,
		   0);

    buffer->buf_retlen = bptr - buffer->buf_ptr;
    return (b < 0) ? -1 : 0;
}

/*  *********************************************************************
    *  sb1250_x1240eeprom_ioctl(ctx,buffer)
    *  
    *  Perform miscellaneous I/O control operations on the device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   buffer - buffer descriptor (target buffer, length, offset)
    *  	   
    *  Return value:
    *  	   number of bytes read
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_x1240eeprom_ioctl(cfe_devctx_t *ctx,iocb_buffer_t *buffer) 
{
    sb1250_x1240eeprom_t *softc = ctx->dev_softc;
    nvram_info_t info;

    switch ((int)buffer->buf_ioctlcmd) {
	case IOCTL_NVRAM_GETINFO:
	    if (buffer->buf_length != sizeof(nvram_info_t)) return -1;
	    info.nvram_offset = softc->env_offset;
	    info.nvram_size =   softc->env_size;
	    info.nvram_eraseflg = FALSE;
	    buffer->buf_retlen = sizeof(nvram_info_t);
	    hs_memcpy_to_hs(buffer->buf_ptr,&info,sizeof(info));
	    return 0;
	default:
	    return -1;
	}
}

/*  *********************************************************************
    *  sb1250_x1240eeprom_close(ctx,buffer)
    *  
    *  Close the device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_x1240eeprom_close(cfe_devctx_t *ctx)
{
    return 0;
}


