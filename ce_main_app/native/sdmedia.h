#ifndef _SDMEDIA_H
#define _SDMEDIA_H

#include <stdio.h>
#include "../datatypes.h"
#include "imedia.h"

class SdMedia: public IMedia
{
public:
    SdMedia();
    virtual ~SdMedia();

    virtual bool iopen(const char *path, bool createIfNotExists);
    virtual void iclose(void);

    virtual bool isInit(void);
    virtual bool mediaChanged(void);
    virtual void setMediaChanged(bool changed);
    virtual void getCapacity(int64_t &bytes, int64_t &sectors);

    virtual bool readSectors(int64_t sectorNo, DWORD count, BYTE *bfr);
    virtual bool writeSectors(int64_t sectorNo, DWORD count, BYTE *bfr);

    virtual DWORD maxSectorsForSmallReadWrite(void);    // what will be the maximum value where small transfer is used (single transfer to/from media + single transfer to/from ST) and where the larger transfer is used above that
    virtual DWORD maxSectorsForSingleReadWrite(void);   // what is the maximum sector count we can use on readSectors() and writeSectors()

    virtual void  startBackgroundTransfer(bool readNotWrite, int64_t sectorNo, DWORD count);    // let media know the whole transfer params, so it can do some background pre-read to speed up the transfer
    virtual bool  waitForBackgroundTransferFinish(void);    // wait until the background transfer finishes and get the final success / failure
private:
    bool    isInitialized;
    bool    mediaChangedFlag;
    int64_t capacityInSectors;
    int64_t capacityInBytes;
    BYTE    senseKey;
};

#endif // _SDMEDIA_H
