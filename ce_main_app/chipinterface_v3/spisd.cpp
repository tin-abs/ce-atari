#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#ifndef ONPC
    #include <bcm2835.h>
#endif

#include "spisd.h"
#include "../utils.h"
#include "../debug.h"
#include "../global.h"
#include "../update.h"

extern THwConfig hwConfig;

SpiSD::SpiSD()
{
#ifndef ONPC
    // Start SPI operations. Forces RPi SPI0 pins P1-19 (MOSI), P1-21 (MISO), P1-23 (CLK), P1-24 (CE0)
    // and P1-26 (CE1) to alternate function ALT0, which enables those pins for SPI interface.
    bcm2835_spi_begin();

    bcm2835_spi_setBitOrder(BCM2835_SPI_BIT_ORDER_MSBFIRST);        // The default
    bcm2835_spi_setDataMode(BCM2835_SPI_MODE0);                     // The default

    bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_1024);    // 1024 => 244 kHz or 390 kHz

    bcm2835_spi_setChipSelectPolarity(BCM2835_SPI_CS0, LOW);        // the default
    bcm2835_spi_chipSelect(BCM2835_SPI_CS_NONE);                    // Specify the SPI chip select pin - BCM2835_SPI_CS_NONE = No CS, control it yourself

    bcm2835_gpio_fsel(PIN_CS_SDCARD, BCM2835_GPIO_FSEL_OUTP);       // PIN_CS_SDCARD
    spiCShigh();                                                    // CS to H
#endif

    txBufferFF = new BYTE[1024];
    memset(txBufferFF, 0xff, 1024);

    rxBuffer = new BYTE[1024];
}

SpiSD::~SpiSD()
{
#ifndef ONPC
    // End SPI operations. SPI0 pins P1-19 (MOSI), P1-21 (MISO), P1-23 (CLK),
    // P1-24 (CE0) and P1-26 (CE1) are returned to their default INPUT behaviour.
    bcm2835_spi_end();
#endif

    delete []txBufferFF;
    delete []rxBuffer;
}

bool SpiSD::isInitialized(void)
{
    return isInit;
}

void SpiSD::spiSetFrequency(BYTE highNotLow)
{
#ifndef ONPC
    if(highNotLow == SPI_FREQ_HIGH) {               // high frequency? 18 MHz (72 / 4)
        bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_16);      // 16 =  64ns = 15.625MHz
    } else {                                        // low frequency? 0.56 MHz (72 / 128)
        bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_1024);    // 1024 => 244 kHz or 390 kHz
    }
#endif
}

void SpiSD::clearStruct(void)
{
    isInit       = false;
    type         = DEVICETYPE_NOTHING;
    mediaChanged = false;
    BCapacity    = 0;
    SCapacity    = 0;
}

void SpiSD::initCard(void)
{
    BYTE res, stat;
    DWORD dRes;

    //Debug::out(LOG_DEBUG, "SpiSD::initCard start");
    timeoutStart();

    //--------------------------
    // and now try to init all that is not initialized
    spiSetFrequency(SPI_FREQ_LOW);          // low SPI frequency

    res = reset();                          // init the MMC card

    if(res == DEVICETYPE_NOTHING) {         // if failed to init
        //Debug::out(LOG_DEBUG, "SpiSD::initCard - failed on reset()");
        return;
    }

    // now to test if we can read from the card
    stat = mmcReadJustForTest(0);

    if(stat) {
        //Debug::out(LOG_DEBUG, "SpiSD::initCard - failed on mmcReadJustForTest()");
        return;
    }

    // get card capacity in blocks
    if(res == DEVICETYPE_SDHC) {
        dRes = SDHC_Capacity();
    } else {
        dRes = MMC_Capacity();
    }

    type      = res;                    // store device type
    SCapacity = dRes;                   // store capacity in sectors
    BCapacity = ((int64_t) dRes) << 9;  // store capacity in bytes
    isInit    = true;                   // set the flag
    mediaChanged = true;

    float capMB = BCapacity / (1024.0 * 1024.0);
    float capGB = BCapacity / (1024.0 * 1024.0 * 1024.0);
    bool showMBs = capGB < 1.0;         // if less than 1 GB, show capacity in MB
    Debug::out(LOG_DEBUG, "SpiSD::initCard - success, card capacity: %.1f %s", showMBs ? capMB : capGB, showMBs ? "MB" : "GB");

    spiSetFrequency(SPI_FREQ_HIGH);
}

void SpiSD::getCardInfo(bool& isInit, bool& mediaChanged, int64_t& byteCapacity, int64_t& sectorCapacity)
{
    isInit          = this->isInit;
    mediaChanged    = this->mediaChanged;
    byteCapacity    = BCapacity;
    sectorCapacity  = SCapacity;
}

//-----------------------------------------------
// It seems that on some cards the init sequence (or something from it) might cause the card
// to want to talk a little bit more than expected, and thus the 1st command issued to this
// specific card might fail. For this situation just sending FFs and reading back the results
// might cure the situation. (Happened to me on 1 out of 5 cards, other 4 were working fine).

void SpiSD::tryToEmptySdCardBuffer(void)
{
#ifndef ONPC
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 2);

    spiCSlow();
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 1024);

    spiCShigh();
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 2);
#endif
}

BYTE SpiSD::reset(void)
{
#ifndef ONPC
    BYTE r1;
    BYTE buff[5];
    int  devType = DEVICETYPE_NOTHING;

    timeoutStart();
    //Debug::out(LOG_DEBUG, "SpiSD::reset starting");

    spiSetFrequency(SPI_FREQ_LOW);
    spiCShigh();

    // send dummy bytes with CS high before accessing
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 10);

    //---------------
    // now send the card to IDLE state
    r1 = mmcSendCommand(MMC_GO_IDLE_STATE, 0);

    if(r1 != 1) {
        //Debug::out(LOG_DEBUG, "SpiSD::reset - failed on MMC_GO_IDLE_STATE, wanted 1, got %d", r1);
        return DEVICETYPE_NOTHING;
    }

    //---------------

    r1 = mmcSendCommand5B(SDHC_SEND_IF_COND, 0x1AA, buff);         // init SDHC card

    if(r1 == 0x01)              // if got the right response -> it's an SD or SDHC card
    {
        if(buff[3] == 0x01 && buff[4] == 0xaa)      // if the rest of R7 is OK (OK echo and voltage)
        {
            while(true)
            {
                if(timeout()) {
                    //Debug::out(LOG_DEBUG, "SpiSD::reset - failed on SHDC_SEND_IF_COND");
                    return DEVICETYPE_NOTHING;
                }

                r1 = mmcSendCommand(SD_APP_CMD, 0); // ACMD41 = CMD55 + CMD41

                if(r1 != 1) {                       // if invalid reply to CMD55, then it's MMC card
                    devType = DEVICETYPE_MMC;
                    break;
                }

                r1 = mmcSendCommand(SD_SEND_OP_COND, 0x40000000); // ACMD41 = CMD55 + CMD41 ---  HCS (High Capacity Support) bit set

                if(r1 == 0) {                       // if everything is OK, then it's SD card
                    devType = DEVICETYPE_SD;
                    break;
                }
            }
            //--------------
            r1 = mmcSendCommand5B(MMC_READ_OCR, 0, buff);

            if(r1 == 0)                         // if command succeeded
            {
                if(buff[1] & 0x40) {            // if CCS bit is set -> SDHC card, else SD card
                    return DEVICETYPE_SDHC;
                }
            }
        }
    }

    if(devType == DEVICETYPE_SD && r1 != 0) {     // if it's SD but failed to initialize
        //Debug::out(LOG_DEBUG, "SpiSD::reset - failed on DEVTYPE_SD but failed to initialize");
        return DEVICETYPE_NOTHING;
    }

    //-------------------------------
    if(devType == DEVICETYPE_MMC) {
        // try to initialize the MMC card
        r1 = mmcCmdLow(MMC_SEND_OP_COND, 0, 0);

        if(r1 != 0) {
            //Debug::out(LOG_DEBUG, "SpiSD::reset - failed on DEVTYPE_MMC got %d instead of 0", r1);
            return DEVICETYPE_NOTHING;
        }
    }

    // set block length to 512 bytes
    r1 = mmcSendCommand(MMC_SET_BLOCKLEN, 512);

    //Debug::out(LOG_DEBUG, "SpiSD::reset - returning devType %d", devType);
    return devType;
#else

    return DEVICETYPE_NOTHING;
#endif
}
//-----------------------------------------------
BYTE SpiSD::mmcCmd(BYTE cmd, DWORD arg, BYTE val)
{
    BYTE r1;

    do {
        r1 = mmcSendCommand(cmd, arg);

        if(timeout()) {
            return 0xff;
        }

    } while(r1 != val);

    return r1;
}
//-----------------------------------------------
BYTE SpiSD::mmcCmdLow(BYTE cmd, DWORD arg, BYTE val)
{
#ifndef ONPC
    BYTE r1;

    spiCSlow();          // CS to L

    do
    {
        // issue the command
        r1 = mmcSendCommand(cmd, arg);

        bcm2835_spi_transfer(0xff);

        if(timeout()) {
            bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 10);

            spiCShigh();         // CS to H
            return 0xff;
        }

    } while(r1 != val);

    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 10);
    spiCShigh();         // CS to H

    return r1;
#else
    return 0;
#endif
}
//-----------------------------------------------
BYTE SpiSD::mmcSendCommand(BYTE cmd, DWORD arg)
{
#ifndef ONPC
    BYTE r1;

    // assert chip select
    spiCSlow();          // CS to L

    //bcm2835_spi_transfer(0xff);

    // issue the command
    r1 = mmcCommand(cmd, arg);

    spiCShigh_ff_return(r1);    // send 0xff, CS high, return value
#else
    return 0;
#endif
}
//-----------------------------------------------
BYTE SpiSD::mmcSendCommand5B(BYTE cmd, DWORD arg, BYTE *buff)
{
#ifndef ONPC
    // assert chip select
    spiCSlow();                         // CS to L

    //bcm2835_spi_transfer(0xff);

    // issue the command
    buff[0] = mmcCommand(cmd, arg);

    // receive the rest of R7 register
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) buff+1, 4);

    spiCShigh_ff_return(buff[0]);       // send 0xff, CS high, return response to the command
#else
    return 0;
#endif
}
//-----------------------------------------------
BYTE SpiSD::mmcCommand(BYTE cmd, DWORD arg)
{
#ifndef ONPC
    // construct command
    BYTE bfr[7];
//  bfr[0] = 0xff;
    bfr[0] = cmd | 0x40;
    bfr[1] = arg >> 24;
    bfr[2] = arg >> 16;
    bfr[3] = arg >>  8;
    bfr[4] = arg;
    bfr[5] = (cmd == SDHC_SEND_IF_COND) ? 0x86 : 0x95;  // crc valid only for: SDHC_SEND_IF_COND / MMC_GO_IDLE_STATE

    // send command
    bcm2835_spi_transfernb((char *) bfr, (char *) rxBuffer, 6);

    // wait for response - while it returning the busy value
    bool good;

    if(cmd == MMC_GO_IDLE_STATE) {  // for MMC_GO_IDLE_STATE - wait until we get response value of 1
        good = waitWhileBusy_notEqualToValue(1);
    } else {                        // for other commands - anything other than 0xff is valid response
        good = waitWhileBusy_equalToValue(0xff);
    }

    if(!good) {                                 // if failed, return lastRxByte
        spiCShigh_ff_return(lastRxByte);
    }

    // return response
    return lastRxByte;                          // return lastRxByte
#else
    return 0;
#endif
}
//**********************************************************************
BYTE SpiSD::mmcRead(DWORD sector, BYTE *data)
{
#ifndef ONPC
    BYTE r1;

    timeoutStart();

    // assert chip select
    spiCSlow();          // CS to L

    if(type != DEVICETYPE_SDHC)                  // for non SDHC cards change sector into address
        sector = sector<<9;

    // issue command
    r1 = mmcCommand(MMC_READ_SINGLE_BLOCK, sector);

    // check for valid response
    if(r1 != 0) {
        spiCShigh_ff_return(r1);    // send 0xff, CS high, return value
    }

    //------------------------
    // wait for block start - while it's not returning expected value
    bool good = waitWhileBusy_notEqualToValue(MMC_STARTBLOCK_READ);

    if(!good) {                                 // if failed, return 0xff
        spiCShigh_ff_return(0xff);
    }

    // get whole sector into data buffer
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) data, 512);

    // read 16-bit CRC and add 1 padding byte
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 3);

    spiCShigh();         // CS to H

    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 3);        // !!! THIS HELPS TO SYNCHRONIZE THE THING !!!
#endif

    // return success
    return 0;
}
//-----------------------------------------------
BYTE SpiSD::mmcReadMultipleStart(DWORD sector)
{
#ifndef ONPC
    BYTE r1;

    timeoutStart();                     // start timeout

    // assert chip select
    spiCSlow();                         // CS to L

    if(type != DEVICETYPE_SDHC) {       // for non SDHC cards change sector into address
        sector = sector << 9;
    }

    r1 = mmcCommand(MMC_READ_MULTIPLE_BLOCKS, sector);  // issue command

    // on fail, deactivate CS and return non-zero
    if(r1 != 0) {                       // if failed to start READ_MULTIPLE_BLOCKS
        spiCShigh_ff_return(r1);        // send 0xff, CS high, return value
    }
#endif

    // on success - keep CS active, return zero
    return 0;
}

//-----------------------------------------------
BYTE SpiSD::mmcReadMultipleOne(BYTE *data, bool isLast)
{
#ifndef ONPC
    timeoutStart();

    // wait for block start - while it's not returning expected value
    bool good = waitWhileBusy_notEqualToValue(MMC_STARTBLOCK_READ);

    // if failed, deactivate CS and return 0xff
    if(!good) {
        mmcCommand(MMC_STOP_TRANSMISSION, 0);
        spiCShigh_ff_return(0xff);
    }

    // read sector from SD card
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) data, 512);

    if(isLast) {    // is last sector in the read op - stop the transmition of next sector
        mmcCommand(MMC_STOP_TRANSMISSION, 0);   // send command instead of CRC
        spiCShigh_ff_return(0);                 // send 0xff, CS high, return value

    } else {        // not last sector in the read op - we need to read more - just read 16-bit CRC
        bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 2);
        return 0;
    }

#endif

    return 0;
}
//-----------------------------------------------
BYTE SpiSD::mmcWriteMultipleStart(DWORD sector)
{
#ifndef ONPC
    BYTE r1;

    timeoutStart();                 // timeout start
    spiCSlow();                     // CS to L

    if(type != DEVICETYPE_SDHC) {   // for non SDHC cards change sector into address
        sector = sector << 9;
    }

    r1 = mmcCommand(MMC_WRITE_MULTIPLE_BLOCKS, sector); // issue command

    // if failed to start write multiple blocks
    if(r1 != 0) {
        spiCShigh_ff_return(r1);    // deactivate CS, return non-zero
    }
#endif

    // on success - keep CS active and return zero
    return 0;
}
//-------------------------------------------------
BYTE SpiSD::mmcWriteMultipleOne(BYTE *data, bool isLast)
{
#ifndef ONPC
    bool good;

    timeoutStart();

    //--------------
    // wait while card is busy - while it's not returning 0xff
    good = waitWhileBusy_notEqualToFF();

    if(!good) {     // if failed, stop transfer, return error
        mmcWriteMultipleStop();
        return 0xff;
    }

    //--------------
    // 0xfc as start write multiple blocks
    bcm2835_spi_transfer(MMC_STARTBLOCK_MWRITE);

    // send buffer to card
    bcm2835_spi_transfernb((char *) data, (char *) rxBuffer, 512);

    // send more: 16-bit CRC
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 2);

    //-------------------------------

    if(isLast) {    // on last sector stop transfer
        // wait while card is busy - while it's not returning 0xff
        good = waitWhileBusy_notEqualToFF();

        if(!good) {                                 // if failed, return 0xff
            spiCShigh_ff_return(0xff);
        }

        return mmcWriteMultipleStop();
    }
#endif

    // on other sectors - return success
    return 0;
}
//-----------------------------------------------
BYTE SpiSD::mmcWriteMultipleStop(void)
{
#ifndef ONPC

    bool good;

    bcm2835_spi_transfer(MMC_STOPTRAN_WRITE);   // 0xfd to stop write multiple blocks

    // wait while card is busy - while it's not returning 0xff
    good = waitWhileBusy_notEqualToFF();

    if(!good) {                                 // if failed, return 0xff
        spiCShigh_ff_return(0xff);
    }

    //-------------------------------
    // for the MMC cards send the STOP TRANSMISSION also
    if(type == DEVICETYPE_MMC)
    {
        mmcCommand(MMC_STOP_TRANSMISSION, 0);

        // wait while card is busy - while it's not returning 0xff
        good = waitWhileBusy_notEqualToFF();

        if(!good) {                                 // if failed, return 0xff
            spiCShigh_ff_return(0xff);
        }
    }
#endif

    spiCShigh_ff_return(0);    // send 0xff, CS high, return value
}

//-----------------------------------------------
BYTE SpiSD::mmcReadJustForTest(DWORD sector)
{
    BYTE bfr[512];
    return mmcRead(sector, bfr);
}
//-----------------------------------------------
BYTE SpiSD::mmcWrite(DWORD sector, BYTE *data)
{
#ifndef ONPC
    BYTE r1;

    timeoutStart();

    //-----------------
    // assert chip select
    spiCSlow();                                 // CS to L

    if(type != DEVICETYPE_SDHC)          // for non SDHC cards change sector into address
        sector = sector<<9;

    // issue command
    r1 = mmcCommand(MMC_WRITE_BLOCK, sector);

    // check for valid response
    if(r1 != 0) {
        spiCShigh_ff_return(r1);    // send 0xff, CS high, return value
    }

    // send dummy
    bcm2835_spi_transfer(0xff);

    // send data start token
    bcm2835_spi_transfer(MMC_STARTBLOCK_WRITE);
    // write data

    // send data
    bcm2835_spi_transfernb((char *) data, (char *) rxBuffer, 512);

    // write 16-bit CRC (dummy values)
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) rxBuffer, 2);

    // read data response token
    r1 = bcm2835_spi_transfer(0xff);

    if( (r1 & MMC_DR_MASK) != MMC_DR_ACCEPT) {
        spiCShigh_ff_return(r1);    // send 0xff, CS high, return value
    }

    // wait until card not busy
    bool good = waitWhileBusy_equalToZero();    // wait while the card is busy

    if(!good) {                                 // if failed, return 0xff
        spiCShigh_ff_return(0xff);
    }

    //------------------------
    spiCShigh_ff_return(0);                     // send 0xff, CS high, return value

#else
    return 0;
#endif
}
//--------------------------------------------------------
/**
*   Retrieves the CSD Register from the mmc
*
*   @return      Status response from cmd
**/
BYTE SpiSD::MMC_CardType(unsigned char *buff)
{
#ifndef ONPC
    BYTE byte;

    // assert chip select
    spiCSlow();                     // CS to L

    // issue the command
    byte = mmcCommand(MMC_SEND_CSD, 0);

    if (byte != 0) {                    // if error
        spiCShigh_ff_return(byte);      // send 0xff, CS high, return value
    }

    // wait for block start
    bool good = waitWhileBusy_notEqualToValue(MMC_STARTBLOCK_READ);

    if(!good) {                                 // if failed, return 0xff
        spiCShigh_ff_return(0xff);
    }

    // read the data
    bcm2835_spi_transfernb((char *) txBufferFF, (char *) buff, 16);

    spiCShigh_ff_return(0);            // send 0xff, CS high, return value
#else
    return 0;
#endif
}
//--------------------------------------------------------
DWORD SpiSD::SDHC_Capacity(void)
{
    BYTE byte, blk_len;
    DWORD c_size;
    DWORD sectors;
    BYTE buff[16];

    timeoutStart();

    byte = MMC_CardType(buff);

    if(byte!=0)                     // if failed to get card type
        return 0xffffffff;

    blk_len = 0x0F & buff[5];       // this should ALWAYS BE equal 9 on SDHC

    if(blk_len != 9)
        return 0xffffffff;

    c_size = ((DWORD)(buff[7] & 0x3F) << 16) | ((DWORD)buff[8] << 8) | ((DWORD)(buff[9]));
    sectors = (c_size+1) << 10;     // get MaxSectors -> mcap=(csize+1)*512k -> msec=mcap/BytsPerSec(fix)

    return sectors;
}
//--------------------------------------------------------
/**
*   Calculates the capacity of the MMC in blocks
*
*   @return   uint32 capacity of MMC in blocks or -1 in error;
**/
DWORD SpiSD::MMC_Capacity(void)
{
    BYTE byte,data, multi, blk_len;
    DWORD c_size;
    DWORD sectors;
    BYTE buff[16];

    timeoutStart();

    byte = MMC_CardType(buff);

    if (byte!=0)
    return 0xffffffff;

    // got info okay
    blk_len = 0x0F & buff[5]; // this should equal 9 -> 512 bytes for cards <= 1 GB

    /*   ; get size into reg
      ;     6            7         8
      ; xxxx xxxx    xxxx xxxx    xxxx xxxx
      ;        ^^    ^^^^ ^^^^    ^^
    */

    data    = (buff[6] & 0x03) << 6;
    data   |= (buff[7] >> 2);
    c_size  = data << 4;
    data    = (buff[7] << 2) | ((buff[8] & 0xC0)>>6);
    c_size |= data;

    /*   ; get multiplier
     ;   9         10
     ; xxxx xxxx    xxxx xxxx
     ;        ^^    ^
    */

    multi    = ((buff[9] & 0x03 ) << 1);
    multi   |= ((buff[10] & 0x80) >> 7);
    sectors  = (c_size + 1) << (multi + 2);

    if(blk_len != 9)                           // sector size > 512B?
    sectors = sectors << (blk_len - 9);     // then capacity of card is bigger

    return sectors;
}
//--------------------------------------------------------
BYTE SpiSD::eraseCard(void)
{
#ifndef ONPC
    BYTE res;

    timeoutStart();

    if(type != DEVICETYPE_MMC && type != DEVICETYPE_SD && type != DEVICETYPE_SDHC) {   // not a SD/MMC card?
        return 1;
    }

    if(type == DEVICETYPE_MMC)                                  // MMC
        res = mmcSendCommand(MMC_TAG_ERASE_GROUP_START, 0);     // start
    else                                                        // SD or SDHC
        res = mmcSendCommand(MMC_TAG_SECTOR_START, 0);          // start

    if(res!=0) {
        spiCShigh_ff_return(res);    // send 0xff, CS high, return value
    }

    if(type == DEVICETYPE_MMC)        // MMC
        res = mmcSendCommand(MMC_TAG_ERARE_GROUP_END, BCapacity - 512);      // end

    if(type == DEVICETYPE_SD)         // SD
        res = mmcSendCommand(MMC_TAG_SECTOR_END, BCapacity - 512);           // end

    if(type == DEVICETYPE_SDHC)       // SDHC
        res = mmcSendCommand(MMC_TAG_SECTOR_END, SCapacity - 1);             // end

    if(res!=0) {
        spiCShigh_ff_return(res);    // send 0xff, CS high, return value
    }
    //-----------------------------
    // assert chip select
    spiCSlow();                             // CS to L

    res = mmcCommand(MMC_ERASE, 0);         // issue the 'erase' command

    if(res != 0) {                          // if failed
        spiCShigh_ff_return(res);           // send 0xff, CS high, return value
    }

    // wait while card is busy
    bool good = waitWhileBusy_equalToZero();    // wait while the card is busy

    if(!good) {                                 // if failed, return 0xff
        spiCShigh_ff_return(0xff);
    }

    spiCShigh_ff_return(0);         // send 0xff, CS high, return value
#else
    return 0;
#endif
}

//--------------------------------------------------------
void SpiSD::timeoutStart(DWORD timeoutTime)
{
    opEndTime = Utils::getEndTime(timeoutTime);
}
//--------------------------------------------------------
bool SpiSD::timeout(void)
{
    DWORD now = Utils::getCurrentMs();
    return (now >= opEndTime) || sigintReceived;    // it's a timeout when current time is greater than end time OR when SIGINT is received
}
//--------------------------------------------------------
bool SpiSD::waitWhileBusy_equalToZero(void)
{
    return waitWhileBusy_equalToValue(0);
}
//--------------------------------------------------------
bool SpiSD::waitWhileBusy_equalToValue(BYTE busyValue)
{
//Debug::out(LOG_DEBUG, "waitWhileBusy_equalToValue - busyValue: %02X", busyValue);

#ifndef ONPC
    // wait while the card is busy -- while output == busyValue
    while(true) {
        lastRxByte = bcm2835_spi_transfer(0xff);

        if(lastRxByte != busyValue) {   // finally not busy? good
            return true;
        }

        if(timeout() || sigintReceived) {       // timeout or SIGINT happened? fail
            //Debug::out(LOG_DEBUG, "waitWhileBusy_equalToValue - timeout", busyValue);
            return false;
        }
    }
#else
    return false;
#endif
}
//--------------------------------------------------------
bool SpiSD::waitWhileBusy_notEqualToFF(void)
{
    return waitWhileBusy_notEqualToValue(0xff);
}
//--------------------------------------------------------
bool SpiSD::waitWhileBusy_notEqualToValue(BYTE wantedValue)
{
#ifndef ONPC
    // wait while the card is busy -- while output != wantedValue
    while(true) {
        lastRxByte = bcm2835_spi_transfer(0xff);

        if(lastRxByte == wantedValue) {     // finally not busy? good
            return true;
        }

        if(timeout() || sigintReceived) {   // timeout or SIGINT happened? fail
            return false;
        }
    }
#else
    return false;
#endif
}
//--------------------------------------------------------
