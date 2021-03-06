/*  *********************************************************************
    *  Broadcom Common Firmware Environment (CFE)
    *  
    *  SPD (memory serial presence detect) 	File: ui_spdcmds.c
    *  
    *  Commands to display contents of memory SPD ROMs
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
#include "ui_command.h"
#include "jedec.h"

/*#define _PROGRAM_SPD_*/

/*  *********************************************************************
    *  Configuration
    ********************************************************************* */


/*  *********************************************************************
    *  prototypes
    ********************************************************************* */

int ui_init_spdcmds(void);
static int ui_cmd_showspd(ui_cmdline_t *cmd,int argc,char *argv[]);

#ifdef _PROGRAM_SPD_
static int ui_cmd_programspd(ui_cmdline_t *cmd,int argc,char *argv[]);
#endif

/*  *********************************************************************
    *  Data
    ********************************************************************* */


#define SPD_DEC_BCD	1
#define SPD_DEC_QTR	2
#define SPD_ENCODED	3
#define SPD_ENCODED2	4

typedef struct spdbyte_s {
    char *name;
    int spdidx;
    int decimal;
    char *description;
} spdbyte_t;

static spdbyte_t spdinfo[] = {
    {"memtype",JEDEC_SPD_MEMTYPE,    0,"[2 ] Memory type"},
    {"rows",  JEDEC_SPD_ROWS,        0,"[3 ] Number of row bits"},
    {"cols",  JEDEC_SPD_COLS,        0,"[4 ] Number of column bits"},
    {"sides", JEDEC_SPD_SIDES,       0,"[5 ] Number of sides"},
    {"width", JEDEC_SPD_WIDTH,       0,"[6 ] Module width"},
    {"banks", JEDEC_SPD_BANKS,       0,"[17] Number of banks"},
    {"tCK25", JEDEC_SPD_tCK25,       SPD_DEC_BCD,"[9 ] tCK value for CAS Latency 2.5"},
    {"tCK20", JEDEC_SPD_tCK20,       SPD_DEC_BCD,"[23] tCK value for CAS Latency 2.0"},
    {"tCK10", JEDEC_SPD_tCK10,       SPD_DEC_BCD,"[25] tCK value for CAS Latency 1.0"},
    {"rfsh",  JEDEC_SPD_RFSH,        SPD_ENCODED,"[12] Refresh rate (KHz)"},
    {"caslat",JEDEC_SPD_CASLATENCIES,SPD_ENCODED,"[18] CAS Latencies supported"},
    {"attrib",JEDEC_SPD_ATTRIBUTES,  SPD_ENCODED,"[21] Module attributes"},
    {"tRAS",  JEDEC_SPD_tRAS,        0,"[30]"},
    {"tRP",   JEDEC_SPD_tRP,         SPD_DEC_QTR,"[27]"},
    {"tRRD",  JEDEC_SPD_tRRD,        SPD_DEC_QTR,"[28]"},
    {"tRCD",  JEDEC_SPD_tRCD,        SPD_DEC_QTR,"[29]"},
    {"tRFC",  JEDEC_SPD_tRFC,        0,"[42]"},
    {"tRC",   JEDEC_SPD_tRC,         0,"[41]"},
    {NULL,  	0,			0, NULL}};


/*  *********************************************************************
    *  ui_init_spdcmds()
    *  
    *  Add SPD-specific commands to the command table
    *  
    *  Input parameters: 
    *  	   nothing
    *  	   
    *  Return value:
    *  	   0
    ********************************************************************* */

int ui_init_spdcmds(void)
{

    cmd_addcmd("show spd",
	       ui_cmd_showspd,
	       NULL,
	       "Display contents of memory SPD",
	       "show spd chan device",
	       "-v;Display entire SPD content in hex");

#ifdef _PROGRAM_SPD_
    cmd_addcmd("program spd",
	       ui_cmd_programspd,
	       NULL,
	       "DANGER Program SPD with memory parameters DANGER",
	       "program spd chan device [ {-offset=* -byte=*} | [MEMORY_PARAMETERS] ]\n\n"
	       "This commands first reads from SPD, then change only the specified parameter.",
	       "-offset=*;Dev addr within SPD(deflt=0)|"
	       "-byte=*;Byte value if -offset used|"
	       "-memtype=*;Memory type|"
	       "-rows=*;Number of row bits|"
	       "-cols=*;Number of column bits|"
	       "-sides=*;Number of sides|"
	       "-width=*;Module width|"
	       "-banks=*;Number of banks|"
	       "-tck25=*;tCK value for CAS Latency 2.5|"
	       "-tck20=*;tCK value for CAS Latency 2.0|"
	       "-tck10=*;tCK value for CAS Latency 1.0|"
	       "-rfsh=*;Refresh rate setting|"
	       "-caslat=*;CAS Latencies supported|"
	       "-attrib=*;Module Attributes|"
	       "-tras=*;tRAS|"
	       "-trp=*;tRP|"
	       "-trrd=*;tRRD|"
	       "-trcd=*;tRCD|"
	       "-trfc=*;tRFC|"
	       "-trc=*;tRC"
              );
#endif  // _PROGRAM_SPD_

    return 0;
}


/*  *********************************************************************
    *  spd_smbus_read(chan,slaveaddr,devaddr)
    *  
    *  Read a byte from the chip
    *  
    *  Input parameters: 
    *  	   chan - SMBus channel
    *  	   slaveaddr -  SMBus slave address
    *  	   devaddr - byte with in the sensor device to read
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   else -1
    ********************************************************************* */

static int spd_smbus_read(cfe_smbus_channel_t *chan,int slaveaddr,int devaddr)
{
    uint8_t buf[1];
    int err;

    /*
     * Read the data byte
     */

    err = SMBUS_XACT(chan,slaveaddr,devaddr,buf,1);
    if (err < 0) return err;

    return buf[0];
}

/*  *********************************************************************
    *  spd_smbus_write(chan,slaveaddr,devaddr,b)
    *  
    *  write a byte to the chip.
    *  
    *  Input parameters: 
    *  	   chan - SMBus channel
    *  	   slaveaddr -  SMBus slave address
    *  	   devaddr - byte within the at24c02 device to read
    *      b - byte to write
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   else -1
    ********************************************************************* */

#ifdef	_PROGRAM_SPD_

static int spd_smbus_write(cfe_smbus_channel_t *chan,int slaveaddr,int devaddr,int b)
{
    uint8_t buf[2];
    int err;

    /*
     * Write the data byte
     */

    buf[0] = devaddr;
    buf[1] = b;

    err = SMBUS_WRITE(chan,slaveaddr,buf,2);
    return err;
}

#endif  // _PROGRAM_SPD_

/*  *********************************************************************
    *  ui_cmd_showspd(cmd,argc,argv)
    *  
    *  Show SPD contents.
    *  
    *  Input parameters: 
    *  	   cmd - command structure
    *  	   argc,argv - parameters
    *  	   
    *  Return value:
    *  	   -1 if error occured.  
    ********************************************************************* */

static int ui_cmd_showspd(ui_cmdline_t *cmd,int argc,char *argv[])
{
    int chan,dev;
    char *x;
    int idx;
    int b;
    cfe_smbus_channel_t *ch;

    x = cmd_getarg(cmd,0);
    if (!x) return ui_showusage(cmd);
    chan = lib_atoi(x);
    if ((chan < 0) || (chan > SMBUS_CHANNELS_MAX)) return ui_showusage(cmd);

    x = cmd_getarg(cmd,1);
    if (!x) return ui_showusage(cmd);
    dev = lib_atoi(x);

    ch = SMBUS_CHANNEL(chan);
    if (!ch) return ui_showusage(cmd);

    if (cmd_sw_isset(cmd,"-v")) {
	for (idx = 0; idx < JEDEC_SPD_SIZE; idx++) {
	    if ((idx % 16) == 0) printf("SPD[%2d..%2d]:",idx,idx+15);
	    b = spd_smbus_read(ch,dev,idx);
	    if (b < 0) {
		printf("\nCould not read SPD at %d/0x%02X\n",chan,dev);
		return -1;
		}
	    printf(" %02X",b);
	    printf(((idx % 16) == 15 || idx == JEDEC_SPD_SIZE-1) ? "\n" : " ");
	    }
	}
    else {
	spdbyte_t *s = spdinfo;
	char buf[20];

	while (s->name) {
	    b = spd_smbus_read(ch,dev,s->spdidx);
	    if (b < 0) {
		printf("Could not read SPD at %d/0x%02X\n",chan,dev);
		return -1;
		}

	    switch (s->decimal) {
		case 0:
		    sprintf(buf,"%02X (%u)",b,b);
		    break;
		case SPD_DEC_BCD:
		    sprintf(buf,"%d.%d",(b >> 4), b & 0x0F);
		    break;
		case SPD_ENCODED:
		    sprintf(buf,"0x%02X",b);
		    break;
		case SPD_DEC_QTR:
		    sprintf(buf,"%d.%02d",(b >> 2), (b & 0x03) *25);;
		    break;
		}

	    printf("%8s = %15s | %30s\n",s->name,buf,s->description);
	    s++;
	    }

	}

    return 0;
}

/*  *********************************************************************
    *  ui_cmd_programspd(cmd,argc,argv)
    *  
    *  Program SPD with memory parameters.
    *  
    *  Input parameters: 
    *  	   cmd - command structure
    *  	   argc,argv - parameters
    *  	   
    *  Return value:
    *  	   -1 if error occured.  
    ********************************************************************* */

#ifdef _PROGRAM_SPD_

static int ui_cmd_programspd(ui_cmdline_t *cmd,int argc,char *argv[])
{
    int chan,dev;
    char *x;
    unsigned char spd[JEDEC_SPD_SIZE];
    int res;
    int idx=0;
    int offset=0;
    cfe_smbus_channel_t *ch;

    x = cmd_getarg(cmd,0);
    if (!x) return ui_showusage(cmd);
    chan = lib_atoi(x);
    if ((chan < 0) || (chan > SMBUS_CHANNELS_MAX)) return ui_showusage(cmd);

    x = cmd_getarg(cmd,1);
    if (!x) return ui_showusage(cmd);
    dev = lib_atoi(x);

    ch = SMBUS_CHANNEL(chan);
    if (!ch) return ui_showusage(cmd);
    
    /* Save what's on the SPD */
    idx = 0;
    while (idx < JEDEC_SPD_SIZE) {
	res = spd_smbus_read(ch,dev,idx);
	if (res < 0) {
	    printf("Could not read byte %d at %d/0x%02X\n",idx,chan,dev);
	    return -1;
	    }
	spd[idx] = res;
	idx++;
	}
    
    /* Get user values */

    x = NULL;
    cmd_sw_value(cmd,"-offset",&x);
    if (x != NULL) offset = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-byte",&x);
    if (x != NULL) spd[offset] = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-memtype",&x);
    if (x != NULL) spd[JEDEC_SPD_MEMTYPE] = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-rows",&x);
    if (x != NULL) spd[JEDEC_SPD_ROWS] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-cols",&x);
    if (x != NULL) spd[JEDEC_SPD_COLS] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-sides",&x);
    if (x != NULL) spd[JEDEC_SPD_SIDES] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-width",&x);
    if (x != NULL) spd[JEDEC_SPD_WIDTH] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-banks",&x);
    if (x != NULL) spd[JEDEC_SPD_BANKS] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-tck25",&x);
    if (x != NULL) spd[JEDEC_SPD_tCK25] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-tck20",&x);
    if (x != NULL) spd[JEDEC_SPD_tCK20] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-tck10",&x);
    if (x != NULL) spd[JEDEC_SPD_tCK10] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-rfsh",&x);
    if (x != NULL) spd[JEDEC_SPD_RFSH] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-caslat",&x);
    if (x != NULL) spd[JEDEC_SPD_CASLATENCIES] = lib_atoi(x);

     x = NULL;
    cmd_sw_value(cmd,"-attrib",&x);
    if (x != NULL) spd[JEDEC_SPD_ATTRIBUTES] = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-tras",&x);
    if (x != NULL) spd[JEDEC_SPD_tRAS] = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-trp",&x);
    if (x != NULL) spd[JEDEC_SPD_tRP] = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-trrd",&x);
    if (x != NULL) spd[JEDEC_SPD_tRRD] = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-trcd",&x);
    if (x != NULL) spd[JEDEC_SPD_tRCD] = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-trfc",&x);
    if (x != NULL) spd[JEDEC_SPD_tRFC] = lib_atoi(x);

    x = NULL;
    cmd_sw_value(cmd,"-trc",&x);
    if (x != NULL) spd[JEDEC_SPD_tRC] = lib_atoi(x);
 
    /* Program SPD */
    idx = 0;
    while (idx < JEDEC_SPD_SIZE) {
	res = spd_smbus_write(ch,dev,idx,spd[idx]);
	if (res < 0) {
	    printf("Could not write byte %d at %d/0x%02X\n",idx,chan,dev);
	    return -1;
	    }
	idx++;
	}

    return 0;
}

#endif // _PROGRAM_SPD_
