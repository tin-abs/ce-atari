// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <signal.h>

#include "../utils.h"
#include "../debug.h"
#include "../settings.h"
#include "acsidatatrans.h"
#include "imagesilo.h"
#include "floppysetup.h"
#include "../display/displaythread.h"

pthread_mutex_t floppyEncoderMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  floppyEncoderShouldWork = PTHREAD_COND_INITIALIZER;

volatile bool floppyEncodingRunning = false;
volatile bool shouldStop;

//-------------------------------
// silo slots are global objects now, as CCoreThread needs to stream from them and
// encoder thread needs to check all of them for tracks that need (re)encoding.
// Access is protected by floppyEncoderMutex.
// Individual tracks in slot can be streamed without using mutex when: slot.encImage.tracks[index].isReady
// (that means we're not encoding that track at that moment).

volatile int currentSlot;

SiloSlot slots[SLOT_COUNT];
//-------------------------------

extern TFlags flags;

void floppyEncoder_addEncodeWholeImageRequest(int slotNo, const char *imageFileName)
{
    Debug::out(LOG_DEBUG, "ImageSilo::addEncodeWholeImageRequest -- slotNo: %d, image: %s", slotNo, imageFileName);

    floppyEncodingRunning = true;

    pthread_mutex_lock(&floppyEncoderMutex);    // lock the mutex
    SiloSlot *slot = &slots[slotNo];            // get pointer to the right slot

    slot->encImage.clearWholeCachedImage();     // clear remains of previous stream

    if(slot->image) {                           // if slot already contains image, get rid of it
        Debug::out(LOG_DEBUG, "ImageSilo::addEncodeWholeImageRequest -- deleting old image from memory", slotNo);
        delete slot->image;
    }

    // try to load image from disk
    slot->image = FloppyImageFactory::getImage(imageFileName);

    if(!slot->image || !slot->image->isOpen()) { // not supported image format or failed to open file?
        Debug::out(LOG_DEBUG, "ImageSilo::addEncodeWholeImageRequest - failed to load image %s", imageFileName);

        if(slot->image) {                       // if got the object, but failed to open, destory object and set pointer to null
            delete slot->image;
            slot->image = NULL;
        }
    } else {                                    // image loaded? good
        Debug::out(LOG_DEBUG, "ImageSilo::addEncodeWholeImageRequest - image %s loaded", imageFileName);
        slot->encImage.storeImageParams(slot->image);   // sets tracksToBeEncoded to all tracks count
    }

    pthread_cond_signal(&floppyEncoderShouldWork);  // wake up encoder
    pthread_mutex_unlock(&floppyEncoderMutex);      // unlock the mutex
}

void floppyEncoder_addReencodeTrackRequest(int slotNo, int track, int side)
{
    Debug::out(LOG_DEBUG, "ImageSilo::addEncodeWholeImageRequest");

    pthread_mutex_lock(&floppyEncoderMutex);        // lock the mutex

    SiloSlot *slot = &slots[slotNo];                // get pointer to the right slot
    slot->encImage.askToReencodeTrack(track, side); // this specific track needs to be reencoded

    pthread_cond_signal(&floppyEncoderShouldWork);  // wake up encoder
    pthread_mutex_unlock(&floppyEncoderMutex);      // unlock the mutex
}

void floppyEncoder_stop(void)
{
    pthread_mutex_lock(&floppyEncoderMutex);        // lock the mutex

    shouldStop = true;

    pthread_cond_signal(&floppyEncoderShouldWork);  // wake up encoder
    pthread_mutex_unlock(&floppyEncoderMutex);      // unlock the mutex
}

SiloSlot *findSlotToEncode(void)
{
    // if the current slot has something to be encoded, return it - this is now priority (to be available for streaming)
    if(slots[currentSlot].encImage.somethingToBeEncoded()) {
        return &slots[currentSlot];
    }

    // go through the other slots, if one of them has something for encoding, return that
    for(int i=0; i<SLOT_COUNT; i++) {
        if(slots[i].encImage.somethingToBeEncoded()) {
            return &slots[i];
        }
    }

    // nothing found for encoding
    return NULL;
}

void *floppyEncodeThreadCode(void *ptr)
{
    Debug::out(LOG_DEBUG, "Floppy encode thread starting...");

    pthread_mutex_lock(&floppyEncoderMutex);    // lock the mutex

    while(true) {                               // while we shouldn't stop
        // code should come to the start of this loop when there is nothing to be encoded and the encoder should sleep
        floppyEncodingRunning = false;              // not encoding right now

        SiloSlot *slot = findSlotToEncode();    // check if there is something to encode

        if(!slot) {                             // nothing to encode? sleep
            // unlock mutex, wait until should work. When should work, lock mutex and continue
            pthread_cond_wait(&floppyEncoderShouldWork, &floppyEncoderMutex);
        }

        if(shouldStop) {    // if we should already stop
            pthread_mutex_unlock(&floppyEncoderMutex);  // unlock the mutex
            Debug::out(LOG_DEBUG, "Floppy encode thread terminated.");
            return 0;
        }

        floppyEncodingRunning = true;               // we're encoding

        // the following code should encode all the slots, but one track at the time, so when there are multiple not encoded images,
        // the one which is selected for streaming, will be encoded first, and then the rest. This should also act fine if you switch
        // current slot in the middle of encoding image (to prioritize the selected one)
        DWORD after50ms = Utils::getEndTime(50);    // this will help to add pauses at least every 50 ms to allow other threads to do stuff

        while(true) {
            slot = findSlotToEncode();  // find some slot which needs encoding

            if(!slot) {                 // nothing to encode or should stop? quit loop and sleep
                floppyEncodingRunning = false;              // not encoding right now
                break;
            }

            if(shouldStop) {            // if we should stop, unlock mutex and quit
                floppyEncodingRunning = false;              // not encoding right now
                pthread_mutex_unlock(&floppyEncoderMutex);  // unlock the mutex
                Debug::out(LOG_DEBUG, "Floppy encode thread terminated.");
                return 0;
            }

            if(Utils::getCurrentMs() > after50ms) {     // if at least 50 ms passed since start or previous pause, add a small pause so other threads could do stuff
                Utils::sleepMs(5);
                after50ms = Utils::getEndTime(50);
            }

            // encode single track in image
            DWORD start = Utils::getCurrentMs();

            int track, side;
            slot->encImage.findNotReadyTrackAndEncodeIt(slot->image, track, side);

            if(track != -1) {       // if something was encoded, dump it to log
                DWORD end = Utils::getCurrentMs();
                Debug::out(LOG_DEBUG, "Encoding of [track %d, side %d] of image %s done, took %d ms", track, side, slot->image->getFileName(), (int) (end - start));
            }
        }
    }

    Debug::out(LOG_DEBUG, "Floppy encode thread terminated.");
    return 0;
}