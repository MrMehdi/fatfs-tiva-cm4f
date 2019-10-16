/*-----------------------------------------------------------------------*/
/* MMC/SDC (in SPI mode) control module  (C)ChaN, 2007                   */
/*-----------------------------------------------------------------------*/
/* Only rcvr_spi(), xmit_spi(), disk_timerproc() and some macros         */
/* are platform dependent.                                               */
/*-----------------------------------------------------------------------*/

/*
 * (C) 2014, Jon Magnuson <my.name at google's email service>
 * This file is based on the sample driver provided by TI, and uses DMA
 * for sector transmission.
 */

/* Standard includes */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* Platform includes */
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_ints.h"
#include "inc/hw_ssi.h"
#include "inc/hw_udma.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/ssi.h"
#include "driverlib/sysctl.h"
#include "driverlib/interrupt.h"
#include "driverlib/udma.h"

/* Debug includes */
#include "utils/uartstdio.h"

/* FatFs includes */
#include "diskio.h"

#define USE_FREERTOS

#if defined (USE_FREERTOS)
/* FreeRTOS Includes */
#include "FreeRTOS.h"
#include "semphr.h"

/* Semaphore for interrupt completion */
static xSemaphoreHandle sd_int_semphr;
#endif

/* Definitions for MMC/SDC command */
#define CMD0    (0x40+0)    /* GO_IDLE_STATE */
#define CMD1    (0x40+1)    /* SEND_OP_COND */
#define CMD8    (0x40+8)    /* SEND_IF_COND */
#define CMD9    (0x40+9)    /* SEND_CSD */
#define CMD10    (0x40+10)    /* SEND_CID */
#define CMD12    (0x40+12)    /* STOP_TRANSMISSION */
#define CMD16    (0x40+16)    /* SET_BLOCKLEN */
#define CMD17    (0x40+17)    /* READ_SINGLE_BLOCK */
#define CMD18    (0x40+18)    /* READ_MULTIPLE_BLOCK */
#define CMD23    (0x40+23)    /* SET_BLOCK_COUNT */
#define CMD24    (0x40+24)    /* WRITE_BLOCK */
#define CMD25    (0x40+25)    /* WRITE_MULTIPLE_BLOCK */
#define CMD41    (0x40+41)    /* SEND_OP_COND (ACMD) */
#define CMD55    (0x40+55)    /* APP_CMD */
#define CMD58    (0x40+58)    /* READ_OCR */

/* Peripheral definitions for DK-TM4C123G board */
// SSI port
#define SDC_SSI_BASE            SSI0_BASE
#define SDC_SSI_SYSCTL_PERIPH   SYSCTL_PERIPH_SSI0
#define SDC_SSI_INT             INT_SSI0
#define SDC_SSI_TX_UDMA_CHAN    UDMA_CHANNEL_SSI0TX
#define SDC_SSI_RX_UDMA_CHAN    UDMA_CHANNEL_SSI0RX

// GPIO for SSI pins
#define SDC_GPIO_PORT_BASE      GPIO_PORTA_BASE
#define SDC_GPIO_SYSCTL_PERIPH  SYSCTL_PERIPH_GPIOA
#define SDC_SSI_CLK             GPIO_PIN_2
#define SDC_SSI_TX              GPIO_PIN_5
#define SDC_SSI_RX              GPIO_PIN_4
#define SDC_SSI_FSS             GPIO_PIN_3
#define SDC_SSI_PINS            (SDC_SSI_TX | SDC_SSI_RX | SDC_SSI_CLK |      \
                                 SDC_SSI_FSS)

#define USE_SCATTERGATHER
#define USE_DMA_TX
#define USE_DMA_RX

void init_dma(uint8_t send);
uint32_t sector_send_dma(uint8_t *buff, uint32_t len);
uint32_t sector_receive_dma(uint8_t *buff, uint32_t len);

static uint8_t ui8ControlTable[1024] __attribute__ ((aligned(1024)));

static uint32_t dma_complete=0;
static uint8_t dummy_rx = 0x00;
static uint8_t dummy_tx = 0xff;

void set_ssi_data_width(uint32_t ui32Base, uint32_t ui32dataWidth) {

    ui32dataWidth--;

    uint32_t ui32Idx=0;
    for(; ui32Idx < 4; ui32Idx++){
        HWREGBITW(ui32Base + SSI_O_CR0, 3 - ui32Idx) = (ui32dataWidth >> (3 - ui32Idx)) & 1;
    }

}

#if defined(USE_SCATTERGATHER)
static uint8_t token_stat = 0xfc;
static uint8_t *buff_ptr = 0x00; /* Gets changed dynamically */
static uint8_t crc_and_response[2] = {0xff, 0xff};//, 0xff}; /* only handle crc for now */

static tDMAControlTable dma_send_sg_list[3] =
{
    /* Token (1) */
    uDMATaskStructEntry(1, UDMA_SIZE_8,
                        UDMA_SRC_INC_8, &token_stat,
                        UDMA_DST_INC_NONE,
                        (void *)(SDC_SSI_BASE + SSI_O_DR),
                        UDMA_ARB_4, UDMA_MODE_PER_SCATTER_GATHER),
    /* Sector buffer (512) */
    uDMATaskStructEntry(512, UDMA_SIZE_8,
                        UDMA_SRC_INC_8, 0, /* Needs set_sg_list_buff() to set */
                        UDMA_DST_INC_NONE,
                        (void *)(SDC_SSI_BASE + SSI_O_DR),
                        UDMA_ARB_4, UDMA_MODE_PER_SCATTER_GATHER),
    /* Fake CRC + response (3) */
    uDMATaskStructEntry(2, UDMA_SIZE_8,
                        UDMA_SRC_INC_8, crc_and_response,
                        UDMA_DST_INC_NONE,
                        (void *)(SDC_SSI_BASE + SSI_O_DR),
                        UDMA_ARB_4, UDMA_MODE_BASIC) /* BASIC because last task */

};

static tDMAControlTable dma_receive_sg_list[3] =
{
    /* Token (1) */
    uDMATaskStructEntry(1, UDMA_SIZE_8,
                        UDMA_SRC_INC_NONE,
                        (void *)(SDC_SSI_BASE + SSI_O_DR),
                        UDMA_DST_INC_NONE, &token_stat,
                        UDMA_ARB_4, UDMA_MODE_PER_SCATTER_GATHER),
    /* Sector buffer (512) */
    uDMATaskStructEntry(512, UDMA_SIZE_8,
                        UDMA_SRC_INC_NONE,
                        (void *)(SDC_SSI_BASE + SSI_O_DR),
                        UDMA_DST_INC_8, 0,
                        UDMA_ARB_4, UDMA_MODE_PER_SCATTER_GATHER),
    /* Fake CRC + response (3) */
    uDMATaskStructEntry(2, UDMA_SIZE_8,
                        UDMA_SRC_INC_NONE,
                        (void *)(SDC_SSI_BASE + SSI_O_DR),
                        UDMA_DST_INC_8, crc_and_response,
                        UDMA_ARB_4, UDMA_MODE_BASIC) /* BASIC because last task */

};

static
void set_sg_list_buff(uint8_t* buff)
{
    /* Snippet out of uDMATaskStructEntry which only sets SrcEndAddr */
    dma_send_sg_list[1].pvSrcEndAddr = &buff[511];
}

static
void set_sg_list_rxbuff(uint8_t* buff)
{
    /* Snippet out of uDMATaskStructEntry which only sets SrcEndAddr */
    dma_receive_sg_list[1].pvDstEndAddr = &buff[511];
}
#endif

// asserts the CS pin to the card
static
void SELECT (void)
{
    ROM_GPIOPinWrite(SDC_GPIO_PORT_BASE, SDC_SSI_FSS, 0);
}

// de-asserts the CS pin to the card
static
void DESELECT (void)
{
    ROM_GPIOPinWrite(SDC_GPIO_PORT_BASE, SDC_SSI_FSS, SDC_SSI_FSS);
}

/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/

static volatile
DSTATUS Stat = STA_NOINIT;    /* Disk status */

static volatile
BYTE Timer1, Timer2;    /* 100Hz decrement timer */

static
BYTE CardType;            /* b0:MMC, b1:SDC, b2:Block addressing */

static
BYTE PowerFlag = 0;     /* indicates if "power" is on */

/*-----------------------------------------------------------------------*/
/* Transmit a byte to MMC via SPI  (Platform dependent)                  */
/*-----------------------------------------------------------------------*/

static
void xmit_spi(BYTE dat)
{
    uint32_t ui32RcvDat;

    ROM_SSIDataPut(SDC_SSI_BASE, dat); /* Write the data to the tx fifo */

    ROM_SSIDataGet(SDC_SSI_BASE, &ui32RcvDat); /* flush data read during the write */
}

static
void xmit_spi16(WORD dat)
{
    uint32_t ui32RcvDat;

    ROM_SSIDataPut(SDC_SSI_BASE, dat); /* Write the data to the tx fifo */

    ROM_SSIDataGet(SDC_SSI_BASE, &ui32RcvDat); /* flush data read during the write */
}



/*-----------------------------------------------------------------------*/
/* Receive a byte from MMC via SPI  (Platform dependent)                 */
/*-----------------------------------------------------------------------*/

static
BYTE rcvr_spi (void)
{
    uint32_t ui32RcvDat;

    ROM_SSIDataPut(SDC_SSI_BASE, 0xFF); /* write dummy data */

    ROM_SSIDataGet(SDC_SSI_BASE, &ui32RcvDat); /* read data frm rx fifo */

    return (BYTE)ui32RcvDat;
}
static
WORD rcvr_spi16 (void)
{
    uint32_t ui32RcvDat;

    ROM_SSIDataPut(SDC_SSI_BASE, 0xFFFF); /* write dummy data */

    ROM_SSIDataGet(SDC_SSI_BASE, &ui32RcvDat); /* read data frm rx fifo */

    return (WORD)ui32RcvDat;
}

static
void rcvr_spi_m (BYTE *dst)
{
    *dst = rcvr_spi();
}

static
void rcvr_spi_m16 (WORD *dst)
{
    *dst = rcvr_spi16();
}

/*-----------------------------------------------------------------------*/
/* Wait for card ready                                                   */
/*-----------------------------------------------------------------------*/

static
BYTE wait_ready (void)
{
    BYTE res;


    Timer2 = 50;    /* Wait for ready in timeout of 500ms */
    rcvr_spi();
    do
        res = rcvr_spi();
    while ((res != 0xFF) && Timer2);

    return res;
}

/*-----------------------------------------------------------------------*/
/* Send 80 or so clock transitions with CS and DI held high. This is     */
/* required after card power up to get it into SPI mode                  */
/*-----------------------------------------------------------------------*/
static
void send_initial_clock_train(void)
{
    unsigned int i;
    uint32_t ui32Dat;

    /* Ensure CS is held high. */
    DESELECT();

    /* Switch the SSI TX line to a GPIO and drive it high too. */
    ROM_GPIOPinTypeGPIOOutput(SDC_GPIO_PORT_BASE, SDC_SSI_TX);
    ROM_GPIOPinWrite(SDC_GPIO_PORT_BASE, SDC_SSI_TX, SDC_SSI_TX);

    /* Send 10 bytes over the SSI. This causes the clock to wiggle the */
    /* required number of times. */
    for(i = 0 ; i < 10 ; i++)
    {
        /* Write DUMMY data. SSIDataPut() waits until there is room in the */
        /* FIFO. */
        ROM_SSIDataPut(SDC_SSI_BASE, 0xFF);

        /* Flush data read during data write. */
        ROM_SSIDataGet(SDC_SSI_BASE, &ui32Dat);
    }

    /* Revert to hardware control of the SSI TX line. */
    ROM_GPIOPinTypeSSI(SDC_GPIO_PORT_BASE, SDC_SSI_TX);
}

/*-----------------------------------------------------------------------*/
/* Power Control  (Platform dependent)                                   */
/*-----------------------------------------------------------------------*/
/* When the target system does not support socket power control, there   */
/* is nothing to do in these functions and chk_power always returns 1.   */

static
void power_on (void)
{
    /*
     * This doesn't really turn the power on, but initializes the
     * SSI port and pins needed to talk to the card.
     */

    /* Enable the peripherals used to drive the SDC on SSI */
    ROM_SysCtlPeripheralEnable(SDC_SSI_SYSCTL_PERIPH);
    ROM_SysCtlPeripheralEnable(SDC_GPIO_SYSCTL_PERIPH);

    /*
     * Configure the appropriate pins to be SSI instead of GPIO. The FSS (CS)
     * signal is directly driven to ensure that we can hold it low through a
     * complete transaction with the SD card.
     */
    ROM_GPIOPinTypeSSI(SDC_GPIO_PORT_BASE, SDC_SSI_TX | SDC_SSI_RX | SDC_SSI_CLK);
    ROM_GPIOPinTypeGPIOOutput(SDC_GPIO_PORT_BASE, SDC_SSI_FSS);

    /*
     * Set the SSI output pins to 4MA drive strength and engage the
     * pull-up on the receive line.
     */
    ROM_GPIOPadConfigSet(SDC_GPIO_PORT_BASE, SDC_SSI_RX, GPIO_STRENGTH_4MA,
                         GPIO_PIN_TYPE_STD_WPU);
    ROM_GPIOPadConfigSet(SDC_GPIO_PORT_BASE, SDC_SSI_CLK | SDC_SSI_TX | SDC_SSI_FSS,
                         GPIO_STRENGTH_4MA, GPIO_PIN_TYPE_STD);

    /* Configure the SSI0 port */
    ROM_SSIConfigSetExpClk(SDC_SSI_BASE, ROM_SysCtlClockGet(),
                           SSI_FRF_MOTO_MODE_3, SSI_MODE_MASTER, 400000, 8);
    ROM_SSIEnable(SDC_SSI_BASE);

    /* Set DI and CS high and apply more than 74 pulses to SCLK for the card */
    /* to be able to accept a native command. */
    send_initial_clock_train();

    ROM_uDMAControlBaseSet(ui8ControlTable);
    ROM_IntDisable(SDC_SSI_INT);
    ROM_SSIDMADisable(SDC_SSI_BASE, SSI_DMA_TX | SSI_DMA_RX);

    PowerFlag = 1;
}

// set the SSI speed to the max setting
static
void set_max_speed(void)
{
    unsigned long i;

    /* Disable the SSI */
    ROM_SSIDisable(SDC_SSI_BASE);

    /* Set the maximum speed as half the system clock, with a max of 12.5 MHz. */
    i = ROM_SysCtlClockGet() / 2;
    if(i > 12500000)
    {
        i = 12500000;
    }

    /* Configure the SSI0 port to run at 12.5MHz */
    ROM_SSIConfigSetExpClk(SDC_SSI_BASE, ROM_SysCtlClockGet(),
                           SSI_FRF_MOTO_MODE_3, SSI_MODE_MASTER, i, 8);

    /* Enable the SSI */
    ROM_SSIEnable(SDC_SSI_BASE);
}

static
void power_off (void)
{
    PowerFlag = 0;
}

static
int chk_power(void)        /* Socket power state: 0=off, 1=on */
{
    return PowerFlag;
}



/*-----------------------------------------------------------------------*/
/* Receive a data packet from MMC                                        */
/*-----------------------------------------------------------------------*/

static
BOOL rcvr_datablock (
    BYTE *buff,            /* Data buffer to store received data */
    UINT btr            /* Byte count (must be even number) */
)
{
    BYTE token;
    WORD dat16;

    Timer1 = 100;
    do {                            /* Wait for data packet in timeout of 100ms */
        token = rcvr_spi();
    } while ((token == 0xFF) && Timer1);
    if(token != 0xFE) return FALSE;    /* If not valid data token, retutn with error */

#if defined(USE_SCATTERGATHER) && defined(USE_DMA_RX)
        /* Handles data + CRC for now (not token) */
        sector_receive_dma((uint8_t*)buff, btr);
#elif defined(USE_DMA_RX)
        sector_receive_dma((uint8_t*)buff, btr);
        rcvr_spi();
        rcvr_spi();
#else

    /* Set data width to 16 bits */
    set_ssi_data_width(SDC_SSI_BASE, 16);

    do {                            /* Receive the data block into buffer */
        rcvr_spi_m16(&dat16);
        *buff++ = (BYTE)((dat16) >> 8);
        *buff++ = (BYTE)((dat16) & 0xFF);
    } while (btr -= 2);

    /* Set data width to 8 bits */
    set_ssi_data_width(SDC_SSI_BASE, 8);

    rcvr_spi();                        /* Discard CRC */
    rcvr_spi();
#endif

    return TRUE;                    /* Return with success */
}



/*-----------------------------------------------------------------------*/
/* Send a data packet to MMC                                             */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0
static
BOOL xmit_datablock (
    const BYTE *buff,    /* 512 byte data block to be transmitted */
    BYTE token            /* Data/Stop token */
)
{
    BYTE resp, wc;
    WORD dat16;

    if (wait_ready() != 0xFF) return FALSE;


    if (token != 0xFD) {    /* Is data token */
        wc = 0;

#if defined(USE_SCATTERGATHER) && defined(USE_DMA_TX)
        token_stat = token;
        sector_send_dma((uint8_t*)buff, 512);
#elif defined(USE_DMA_TX)
        xmit_spi(token);
        sector_send_dma((uint8_t*)buff, 512);
        xmit_spi(0xFF);                    /* CRC (Dummy) */
        xmit_spi(0xFF);

#else
        xmit_spi(token);                    /* Xmit data token */

        /* Set data width to 16 bits */
        set_ssi_data_width(SDC_SSI_BASE, 16);

        do {                            /* Xmit the 512 byte data block to MMC */
            dat16 = (*buff++) << 8;
            dat16 |= (*buff++);
            xmit_spi16(dat16);
        } while (--wc);

        /* Set data width to 8 bits */
        set_ssi_data_width(SDC_SSI_BASE, 8);

        xmit_spi(0xFF);                    /* CRC (Dummy) */
        xmit_spi(0xFF);
#endif
        resp = rcvr_spi();
        if ((resp & 0x1F) != 0x05) {    /* If not accepted, return with error */
            return FALSE;
        }
    }
    else { /* token == 0xFD */
        xmit_spi(token);                    /* Xmit data token */
    }


    return TRUE;
}
#endif /* _READONLY */



/*-----------------------------------------------------------------------*/
/* Send a command packet to MMC                                          */
/*-----------------------------------------------------------------------*/

static
BYTE send_cmd (
    BYTE cmd,        /* Command byte */
    DWORD arg        /* Argument */
)
{
    BYTE n, res;


    if (wait_ready() != 0xFF) return 0xFF;

    /* Send command packet */
    xmit_spi(cmd);                        /* Command */
    xmit_spi((BYTE)(arg >> 24));        /* Argument[31..24] */
    xmit_spi((BYTE)(arg >> 16));        /* Argument[23..16] */
    xmit_spi((BYTE)(arg >> 8));            /* Argument[15..8] */
    xmit_spi((BYTE)arg);                /* Argument[7..0] */
    n = 0xff;
    if (cmd == CMD0) n = 0x95;            /* CRC for CMD0(0) */
    if (cmd == CMD8) n = 0x87;            /* CRC for CMD8(0x1AA) */
    xmit_spi(n);

    /* Receive command response */
    if (cmd == CMD12) rcvr_spi();        /* Skip a stuff byte when stop reading */
    n = 10;                                /* Wait for a valid response in timeout of 10 attempts */
    do
        res = rcvr_spi();
    while ((res & 0x80) && --n);

    return res;            /* Return with the response value */
}

/*-----------------------------------------------------------------------*
 * Send the special command used to terminate a multi-sector read.
 *
 * This is the only command which can be sent while the SDCard is sending
 * data. The SDCard spec indicates that the data transfer will stop 2 bytes
 * after the 6 byte CMD12 command is sent and that the card will then send
 * 0xFF for between 2 and 6 more bytes before the R1 response byte.  This
 * response will be followed by another 0xFF byte.  In testing, however, it
 * seems that some cards don't send the 2 to 6 0xFF bytes between the end of
 * data transmission and the response code.  This function, therefore, merely
 * reads 10 bytes and, if the last one read is 0xFF, returns the value of the
 * latest non-0xFF byte as the response code.
 *
 *-----------------------------------------------------------------------*/

static
BYTE send_cmd12 (void)
{
    BYTE n, res, val;

    /* For CMD12, we don't wait for the card to be idle before we send
     * the new command.
     */

    /* Send command packet - the argument for CMD12 is ignored. */
    xmit_spi(CMD12);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);

    /* Read up to 10 bytes from the card, remembering the value read if it's
       not 0xFF */
    for(n = 0; n < 10; n++)
    {
        val = rcvr_spi();
        if(val != 0xFF)
        {
            res = val;
        }
    }

    return res;            /* Return with the response value */
}

/*--------------------------------------------------------------------------

   Public Functions

---------------------------------------------------------------------------*/


/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
    BYTE drv        /* Physical drive nmuber (0) */
)
{
    BYTE n, ty, ocr[4];


    if (drv) return STA_NOINIT;            /* Supports only single drive */
    if (Stat & STA_NODISK) return Stat;    /* No card in the socket */

    power_on();                            /* Force socket power on */
    send_initial_clock_train();            /* Ensure the card is in SPI mode */

    SELECT();                /* CS = L */
    ty = 0;
    if (send_cmd(CMD0, 0) == 1) {            /* Enter Idle state */
        Timer1 = 100;                        /* Initialization timeout of 1000 msec */
        if (send_cmd(CMD8, 0x1AA) == 1) {    /* SDC Ver2+ */
            for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {    /* The card can work at vdd range of 2.7-3.6V */
                do {
                    if (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 1UL << 30) == 0)    break;    /* ACMD41 with HCS bit */
                } while (Timer1);
                if (Timer1 && send_cmd(CMD58, 0) == 0) {    /* Check CCS bit */
                    for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();
                    ty = (ocr[0] & 0x40) ? 6 : 2;
                }
            }
        } else {                            /* SDC Ver1 or MMC */
            ty = (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 0) <= 1) ? 2 : 1;    /* SDC : MMC */
            do {
                if (ty == 2) {
                    if (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 0) == 0) break;    /* ACMD41 */
                } else {
                    if (send_cmd(CMD1, 0) == 0) break;                                /* CMD1 */
                }
            } while (Timer1);
            if (!Timer1 || send_cmd(CMD16, 512) != 0)    /* Select R/W block length */
                ty = 0;
        }
    }
    CardType = ty;
    DESELECT();            /* CS = H */
    rcvr_spi();            /* Idle (Release DO) */

    if (ty) {            /* Initialization succeded */
        Stat &= ~STA_NOINIT;        /* Clear STA_NOINIT */
        set_max_speed();
    } else {            /* Initialization failed */
        power_off();
    }

    return Stat;
}



/*-----------------------------------------------------------------------*/
/* Get Disk Status                                                       */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (
    BYTE drv        /* Physical drive nmuber (0) */
)
{
    if (drv) return STA_NOINIT;        /* Supports only single drive */
    return Stat;
}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (
    BYTE drv,            /* Physical drive nmuber (0) */
    BYTE *buff,            /* Pointer to the data buffer to store read data */
    DWORD sector,        /* Start sector number (LBA) */
    BYTE count            /* Sector count (1..255) */
)
{
    if (drv || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    if (!(CardType & 4)) sector *= 512;    /* Convert to byte address if needed */

    SELECT();            /* CS = L */

    if (count == 1) {    /* Single block read */
        if ((send_cmd(CMD17, sector) == 0)    /* READ_SINGLE_BLOCK */
            && rcvr_datablock(buff, 512))
            count = 0;
    }
    else {                /* Multiple block read */
        if (send_cmd(CMD18, sector) == 0) {    /* READ_MULTIPLE_BLOCK */
            do {
                if (!rcvr_datablock(buff, 512)) break;
                buff += 512;
            } while (--count);
            send_cmd12();                /* STOP_TRANSMISSION */
        }
    }

    DESELECT();            /* CS = H */
    rcvr_spi();            /* Idle (Release DO) */

    return count ? RES_ERROR : RES_OK;
}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0
DRESULT disk_write (
    BYTE drv,            /* Physical drive nmuber (0) */
    const BYTE *buff,    /* Pointer to the data to be written */
    DWORD sector,        /* Start sector number (LBA) */
    BYTE count            /* Sector count (1..255) */
)
{
    if (drv || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (Stat & STA_PROTECT) return RES_WRPRT;

    if (!(CardType & 4)) sector *= 512;    /* Convert to byte address if needed */

    SELECT();            /* CS = L */

    if (count == 1) {    /* Single block write */
        if ((send_cmd(CMD24, sector) == 0)    /* WRITE_BLOCK */
            && xmit_datablock(buff, 0xFE))
            count = 0;
    }
    else {                /* Multiple block write */
        if (CardType & 2) {
            send_cmd(CMD55, 0); send_cmd(CMD23, count);    /* ACMD23 */
        }
        if (send_cmd(CMD25, sector) == 0) {    /* WRITE_MULTIPLE_BLOCK */
            do {
                if (!xmit_datablock(buff, 0xFC)) break;
                buff += 512;
            } while (--count);
            if (!xmit_datablock(0, 0xFD))    /* STOP_TRAN token */
                count = 1;
        }
    }

    DESELECT();            /* CS = H */
    rcvr_spi();            /* Idle (Release DO) */

    return count ? RES_ERROR : RES_OK;
}
#endif /* _READONLY */



/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (
    BYTE drv,        /* Physical drive nmuber (0) */
    BYTE ctrl,        /* Control code */
    void *buff        /* Buffer to send/receive control data */
)
{
    DRESULT res;
    BYTE n, csd[16], *ptr = buff;
    WORD csize;


    if (drv) return RES_PARERR;

    res = RES_ERROR;

    if (ctrl == CTRL_POWER) {
        switch (*ptr) {
        case 0:        /* Sub control code == 0 (POWER_OFF) */
            if (chk_power())
                power_off();        /* Power off */
            res = RES_OK;
            break;
        case 1:        /* Sub control code == 1 (POWER_ON) */
            power_on();                /* Power on */
            res = RES_OK;
            break;
        case 2:        /* Sub control code == 2 (POWER_GET) */
            *(ptr+1) = (BYTE)chk_power();
            res = RES_OK;
            break;
        default :
            res = RES_PARERR;
        }
    }
    else {
        if (Stat & STA_NOINIT) return RES_NOTRDY;

        SELECT();        /* CS = L */

        switch (ctrl) {
        case GET_SECTOR_COUNT :    /* Get number of sectors on the disk (DWORD) */
            if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
                if ((csd[0] >> 6) == 1) {    /* SDC ver 2.00 */
                    csize = csd[9] + ((WORD)csd[8] << 8) + 1;
                    *(DWORD*)buff = (DWORD)csize << 10;
                } else {                    /* MMC or SDC ver 1.XX */
                    n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                    csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                    *(DWORD*)buff = (DWORD)csize << (n - 9);
                }
                res = RES_OK;
            }
            break;

        case GET_SECTOR_SIZE :    /* Get sectors on the disk (WORD) */
            *(WORD*)buff = 512;
            res = RES_OK;
            break;

        case CTRL_SYNC :    /* Make sure that data has been written */
            if (wait_ready() == 0xFF)
                res = RES_OK;
            break;

        case MMC_GET_CSD :    /* Receive CSD as a data block (16 bytes) */
            if (send_cmd(CMD9, 0) == 0        /* READ_CSD */
                && rcvr_datablock(ptr, 16))
                res = RES_OK;
            break;

        case MMC_GET_CID :    /* Receive CID as a data block (16 bytes) */
            if (send_cmd(CMD10, 0) == 0        /* READ_CID */
                && rcvr_datablock(ptr, 16))
                res = RES_OK;
            break;

        case MMC_GET_OCR :    /* Receive OCR as an R3 resp (4 bytes) */
            if (send_cmd(CMD58, 0) == 0) {    /* READ_OCR */
                for (n = 0; n < 4; n++)
                    *ptr++ = rcvr_spi();
                res = RES_OK;
            }

//        case MMC_GET_TYPE :    /* Get card type flags (1 byte) */
//            *ptr = CardType;
//            res = RES_OK;
//            break;

        default:
            res = RES_PARERR;
        }

        DESELECT();            /* CS = H */
        rcvr_spi();            /* Idle (Release DO) */
    }

    return res;
}



/*-----------------------------------------------------------------------*/
/* Device Timer Interrupt Procedure  (Platform dependent)                */
/*-----------------------------------------------------------------------*/
/* This function must be called in period of 10ms                        */

void disk_timerproc (void)
{
//    BYTE n, s;
    BYTE n;


    n = Timer1;                        /* 100Hz decrement timer */
    if (n) Timer1 = --n;
    n = Timer2;
    if (n) Timer2 = --n;

}

/*---------------------------------------------------------*/
/* User Provided Timer Function for FatFs module           */
/*---------------------------------------------------------*/
/* This is a real time clock service to be called from     */
/* FatFs module. Any valid time must be returned even if   */
/* the system does not support a real time clock.          */

DWORD get_fattime (void)
{

    return    ((2007UL-1980) << 25)    // Year = 2007
            | (6UL << 21)            // Month = June
            | (5UL << 16)            // Day = 5
            | (11U << 11)            // Hour = 11
            | (38U << 5)            // Min = 38
            | (0U >> 1)                // Sec = 0
            ;

}

/*****************************************************************************
 *
 *                           SD DMA FUNCTIONS
 *
 *****************************************************************************/
void
SDCSSIIntHandler(void)
{
#if defined (USE_FREERTOS)
    portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;
#endif

    uint32_t ui32Status;
    uint32_t ui32Mode;

    /* Get status */
    ui32Status = ROM_SSIIntStatus(SDC_SSI_BASE, TRUE);

    /* Clear status */
    ROM_SSIIntClear(SDC_SSI_BASE, ui32Status);

    ui32Mode = ROM_uDMAChannelModeGet(SDC_SSI_RX_UDMA_CHAN | UDMA_PRI_SELECT);

    if(ui32Mode == UDMA_MODE_STOP /*UDMA_MODE_BASIC*/)
    {

#if defined (USE_FREERTOS)
        /* Signal transfer completion with semaphore */
        xSemaphoreGive(sd_int_semphr);
#else
        /* Signal txfer complete */
        dma_complete = 1;
#endif
    }

    /* If the SSI DMA TX channel is disabled, that means the TX DMA txfer is complete */
    if(!ROM_uDMAChannelIsEnabled(SDC_SSI_TX_UDMA_CHAN))
    {
        asm(" nop");
    }

#if defined (USE_FREERTOS)
    /* Switch tasks if necessary. */
    if ( xHigherPriorityTaskWoken != pdFALSE ) {
        portEND_SWITCHING_ISR( xHigherPriorityTaskWoken );
    }
#endif

}

unsigned int
sector_send_dma(uint8_t *buff, uint32_t len)
{

    volatile uint32_t discard;

    dma_complete = 0;

    /* Re-initialize DMA every transmission */
    init_dma(1);

#if !defined(USE_SCATTERGATHER)
    ROM_uDMAChannelTransferSet(SDC_SSI_RX_UDMA_CHAN | UDMA_PRI_SELECT,
                               UDMA_MODE_BASIC,
                               (void *)(SDC_SSI_BASE + SSI_O_DR),
                               &dummy_rx,
                               len);

    ROM_uDMAChannelTransferSet(SDC_SSI_TX_UDMA_CHAN | UDMA_PRI_SELECT,
                                   UDMA_MODE_BASIC,
                                   buff,
                                   (void *)(SDC_SSI_BASE + SSI_O_DR),
                                   len);

#else
    /* Point Scatter-Gather list buffer ptr to buff */
    set_sg_list_buff(buff);

    ROM_uDMAChannelTransferSet(SDC_SSI_RX_UDMA_CHAN | UDMA_PRI_SELECT,
                               UDMA_MODE_BASIC,
                               (void *)(SDC_SSI_BASE + SSI_O_DR),
                               &dummy_rx,
                               len+3);

    /* Newer method for setting up scatter-gather.  Ref: 'udma_uart_sg.c' */
    uDMAChannelScatterGatherSet(SDC_SSI_TX_UDMA_CHAN, 3, dma_send_sg_list, 1);
#endif

    /* Initiate DMA txfer */
    ROM_uDMAChannelEnable(SDC_SSI_RX_UDMA_CHAN);
    ROM_uDMAChannelEnable(SDC_SSI_TX_UDMA_CHAN);

#if defined(USE_FREERTOS)
    xSemaphoreTake(sd_int_semphr, portMAX_DELAY);
#else
    while (!dma_complete);
#endif

    while(1)
    {
        /* Double-check to make sure DMA txfer is done (can probably remove) */
        uint32_t evFlags = ROM_uDMAChannelModeGet(SDC_SSI_RX_UDMA_CHAN | UDMA_PRI_SELECT);
        if (evFlags==UDMA_MODE_STOP) break;
    }

    //for (discard=100; discard; discard--);

    ROM_uDMAChannelDisable(SDC_SSI_RX_UDMA_CHAN);
    ROM_uDMAChannelDisable(SDC_SSI_TX_UDMA_CHAN);
    ROM_SSIDMADisable(SDC_SSI_BASE, SSI_DMA_TX | SSI_DMA_RX);

    return 0;
}

unsigned int
sector_receive_dma(uint8_t *buff, uint32_t len)
{

    volatile uint32_t discard;

    dma_complete = 0;

    /* Re-initialize DMA every transmission */
    init_dma(0);

#if !defined(USE_SCATTERGATHER)
    ROM_uDMAChannelTransferSet(SDC_SSI_RX_UDMA_CHAN | UDMA_PRI_SELECT,
                               UDMA_MODE_BASIC,
                               (void *)(SSI0_BASE + SSI_O_DR),
                               buff,
                               len);

    ROM_uDMAChannelTransferSet(SDC_SSI_TX_UDMA_CHAN | UDMA_PRI_SELECT,
                               UDMA_MODE_BASIC,
                               &dummy_tx,
                               (void *)(SDC_SSI_BASE + SSI_O_DR),
                               len);

#else
    /* Point Scatter-Gather list buffer ptr to buff */
    set_sg_list_rxbuff(buff);

    /* Newer method for setting up scatter-gather.  Ref: 'udma_uart_sg.c' */
    uDMAChannelScatterGatherSet(SDC_SSI_RX_UDMA_CHAN, 2, (void*)(dma_receive_sg_list+1), 1);


    ROM_uDMAChannelTransferSet(SDC_SSI_TX_UDMA_CHAN | UDMA_PRI_SELECT,
                               UDMA_MODE_BASIC,
                               &dummy_tx,
                               (void *)(SDC_SSI_BASE + SSI_O_DR),
                               len+2);
#endif

    /* Initiate DMA txfer */
    ROM_uDMAChannelEnable(SDC_SSI_RX_UDMA_CHAN);
    ROM_uDMAChannelEnable(SDC_SSI_TX_UDMA_CHAN);

#if defined(USE_FREERTOS)
    xSemaphoreTake(sd_int_semphr, portMAX_DELAY);
#else
    while (!dma_complete);
#endif

    while(1)
    {
        /* Double-check to make sure DMA txfer is done (can probably remove) */
        uint32_t evFlags = ROM_uDMAChannelModeGet(SDC_SSI_RX_UDMA_CHAN | UDMA_PRI_SELECT);
        if (evFlags==UDMA_MODE_STOP) break;
    }

    //for (discard=100; discard; discard--);

    ROM_uDMAChannelDisable(SDC_SSI_RX_UDMA_CHAN);
    ROM_uDMAChannelDisable(SDC_SSI_TX_UDMA_CHAN);
    ROM_SSIDMADisable(SDC_SSI_BASE, SSI_DMA_TX | SSI_DMA_RX);

    return 0;
}

void
init_dma(uint8_t send)
{

#if defined(USE_FREERTOS)
    /* If interrupt semaphore not created yet, do so now */
    if (sd_int_semphr == NULL) {
        sd_int_semphr = xSemaphoreCreateBinary(); // FreeRTOS v8.0
    }
#endif

    /* Init SPI DMA & SPI interrupt */
    ROM_SSIDMAEnable(SDC_SSI_BASE, SSI_DMA_TX | SSI_DMA_RX);
    ROM_IntEnable(SDC_SSI_INT);

    if (send) {

        /* RX */
        ROM_uDMAChannelAttributeDisable(SDC_SSI_RX_UDMA_CHAN, UDMA_ATTR_ALL);
        ROM_uDMAChannelControlSet(SDC_SSI_RX_UDMA_CHAN | UDMA_PRI_SELECT,
                                  UDMA_SIZE_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_NONE | UDMA_ARB_4);

        /* TX */
        ROM_uDMAChannelAttributeDisable(SDC_SSI_TX_UDMA_CHAN,
                                        UDMA_ATTR_ALTSELECT
                                        | UDMA_ATTR_HIGH_PRIORITY
                                        | UDMA_ATTR_REQMASK);
        ROM_uDMAChannelControlSet(SDC_SSI_TX_UDMA_CHAN | UDMA_PRI_SELECT,
                                  UDMA_SIZE_8 | UDMA_SRC_INC_8 | UDMA_DST_INC_NONE | UDMA_ARB_4);
    }
    else { /* receive */

        /* RX */
        ROM_uDMAChannelAttributeDisable(SDC_SSI_RX_UDMA_CHAN, UDMA_ATTR_ALL);
        ROM_uDMAChannelControlSet(SDC_SSI_RX_UDMA_CHAN | UDMA_PRI_SELECT,
                                  UDMA_SIZE_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_8 | UDMA_ARB_4);


        /* TX */
        ROM_uDMAChannelAttributeDisable(SDC_SSI_TX_UDMA_CHAN,
                                        UDMA_ATTR_ALTSELECT
                                        | UDMA_ATTR_HIGH_PRIORITY
                                        | UDMA_ATTR_REQMASK);
        ROM_uDMAChannelControlSet(SDC_SSI_TX_UDMA_CHAN | UDMA_PRI_SELECT,
                                  UDMA_SIZE_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_NONE | UDMA_ARB_4);
    }

    /* Clear SSI0 FIFO just to be safe */
    while((HWREG(SSI0_BASE + SSI_O_SR) & SSI_SR_RNE))
    {
        uint32_t discard = HWREG(SSI0_BASE + SSI_O_DR);
    }

}
