#include "Limelight-internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <windows.h>
#include "Queue.h"


#define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64


FILE *file;
FILE *fp;
FILE *enQRateFile;
bool hasFirstLine = false;

// Buffer & Queue variables
enum buffer_states {FILL, PLAY};
static Queue *frameQ;
static Queue *drstatusQ;
int createdQ = 0;
int frameQSize, drstatusQSize;
static PLT_THREAD testQMain;
static PLT_THREAD testQSecond;
static PLT_MUTEX qMainMutex;
static PLT_MUTEX qSecondMutex;
static PLT_MUTEX mutex;
static VIDEO_FRAME_HANDLE frameHandle;
static int drstatusHandle;
static int bufferSize = 10;

// Hard-coded 16fps time for diff
static int targetTime = 17;

// Catch up time for dequeuing thread
long catchUp = 0;

// Variables and logs for Li..() call rate
uint64_t interframeRates[10];
static long double avgEnQRate = 0;
static int counter = 0;
uint64_t elaspedT = 0;
uint64_t newT = 0;
uint64_t oldT = 0;
static uint64_t initialT = 0;
uint64_t eTimeForLog = 0;
bool gotInitTime = false;
bool gotFirstTime = false;
struct timeval newTimer;

// Variables for elapsed time for log
struct timeval streamStart;
static uint64_t startStreamTime = 0;
bool gottime = false;



struct timezone
{
  int  tz_minuteswest; /* minutes W of Greenwich */
  int  tz_dsttime;     /* type of dst correction */
};



// Uncomment to test 3 byte Annex B start sequences with GFE
//#define FORCE_3_BYTE_START_SEQUENCES

static PLENTRY nalChainHead;
static PLENTRY nalChainTail;
static int nalChainDataLength;

static unsigned int nextFrameNumber;
static unsigned int startFrameNumber;
static bool waitingForNextSuccessfulFrame;
static bool waitingForIdrFrame;
static bool waitingForRefInvalFrame;
static unsigned int lastPacketInStream;
static bool decodingFrame;
static bool strictIdrFrameWait;
static uint64_t firstPacketReceiveTime;
static unsigned int firstPacketPresentationTime;
static bool dropStatePending;
static bool idrFrameProcessed;

#define DR_CLEANUP -1000

#define CONSECUTIVE_DROP_LIMIT 120
static unsigned int consecutiveFrameDrops;

static LINKED_BLOCKING_QUEUE decodeUnitQueue;

typedef struct _BUFFER_DESC {
    char* data;
    unsigned int offset;
    unsigned int length;
} BUFFER_DESC, *PBUFFER_DESC;

typedef struct _LENTRY_INTERNAL {
    LENTRY entry;
    void* allocPtr;
} LENTRY_INTERNAL, *PLENTRY_INTERNAL;

#define H264_NAL_TYPE(x) ((x) & 0x1F)
#define HEVC_NAL_TYPE(x) (((x) & 0x7E) >> 1)

#define H264_NAL_TYPE_SEI 6
#define H264_NAL_TYPE_SPS 7
#define H264_NAL_TYPE_PPS 8
#define H264_NAL_TYPE_AUD 9
#define HEVC_NAL_TYPE_VPS 32
#define HEVC_NAL_TYPE_SPS 33
#define HEVC_NAL_TYPE_PPS 34
#define HEVC_NAL_TYPE_AUD 35
#define HEVC_NAL_TYPE_SEI 39


// Init
void initializeVideoDepacketizer(int pktSize) {
    LbqInitializeLinkedBlockingQueue(&decodeUnitQueue, 15);

    nextFrameNumber = 1;
    startFrameNumber = 0;
    waitingForNextSuccessfulFrame = false;
    waitingForIdrFrame = true;
    waitingForRefInvalFrame = false;
    lastPacketInStream = UINT32_MAX;
    decodingFrame = false;
    firstPacketReceiveTime = 0;
    firstPacketPresentationTime = 0;
    dropStatePending = false;
    idrFrameProcessed = false;
    strictIdrFrameWait = !isReferenceFrameInvalidationEnabled();
//    PltCreateMutex(&qSecondMutex);
    PltCreateMutex(&qMainMutex);
    //fp = fopen("timelog.csv","w+");
}

// Free the NAL chain
static void cleanupFrameState(void) {
    PLENTRY_INTERNAL lastEntry;

    while (nalChainHead != NULL) {
        lastEntry = (PLENTRY_INTERNAL)nalChainHead;
        nalChainHead = lastEntry->entry.next;
        free(lastEntry->allocPtr);
    }

    nalChainTail = NULL;

    nalChainDataLength = 0;
}

// Cleanup frame state and set that we're waiting for an IDR Frame
static void dropFrameState(void) {
    // This may only be called at frame boundaries
    LC_ASSERT(!decodingFrame);

    // We're dropping frame state now
    dropStatePending = false;

    if (strictIdrFrameWait || !idrFrameProcessed || waitingForIdrFrame) {
        // We'll need an IDR frame now if we're in non-RFI mode, if we've never
        // received an IDR frame, or if we explicitly need an IDR frame.
        waitingForIdrFrame = true;
    }
    else {
        waitingForRefInvalFrame = true;
    }

    // Count the number of consecutive frames dropped
    consecutiveFrameDrops++;

    // If we reach our limit, immediately request an IDR frame and reset
    if (consecutiveFrameDrops == CONSECUTIVE_DROP_LIMIT) {
        Limelog("Reached consecutive drop limit\n");

        // Restart the count
        consecutiveFrameDrops = 0;

        // Request an IDR frame
        waitingForIdrFrame = true;
        LiRequestIdrFrame();
    }

    cleanupFrameState();
}

// Cleanup the list of decode units
static void freeDecodeUnitList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;

        // Complete this with a failure status
        LiCompleteVideoFrame(entry->data, DR_CLEANUP);

        entry = nextEntry;
    }
}

void stopVideoDepacketizer(void) {
    LbqSignalQueueShutdown(&decodeUnitQueue);
}

// Cleanup video depacketizer and free malloced memory
void destroyVideoDepacketizer(void) {
    freeDecodeUnitList(LbqDestroyLinkedBlockingQueue(&decodeUnitQueue));
    cleanupFrameState();
}

static bool getAnnexBStartSequence(PBUFFER_DESC current, PBUFFER_DESC startSeq) {
    if (current->length < 3) {
        return false;
    }

    if (current->data[current->offset] == 0 &&
        current->data[current->offset + 1] == 0) {
        if (current->data[current->offset + 2] == 0) {
            if (current->length >= 4 && current->data[current->offset + 3] == 1) {
                // Frame start
                if (startSeq != NULL) {
                    startSeq->data = current->data;
                    startSeq->offset = current->offset;
                    startSeq->length = 4;
                }
                return true;
            }
        }
        else if (current->data[current->offset + 2] == 1) {
            // NAL start
            if (startSeq != NULL) {
                startSeq->data = current->data;
                startSeq->offset = current->offset;
                startSeq->length = 3;
            }
            return true;
        }
    }

    return false;
}

bool LiWaitForNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit) {
    PQUEUED_DECODE_UNIT qdu;

    int err = LbqWaitForQueueElement(&decodeUnitQueue, (void**)&qdu);
    if (err != LBQ_SUCCESS) {
        return false;
    }

    *frameHandle = qdu;
    *decodeUnit = &qdu->decodeUnit;
    return true;
}

bool LiPollNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit) {
    PQUEUED_DECODE_UNIT qdu;

    int err = LbqPollQueueElement(&decodeUnitQueue, (void**)&qdu);
    if (err != LBQ_SUCCESS) {
        return false;
    }

    *frameHandle = qdu;
    *decodeUnit = &qdu->decodeUnit;
    return true;
}

bool LiPeekNextVideoFrame(PDECODE_UNIT* decodeUnit) {
    PQUEUED_DECODE_UNIT qdu;

    int err = LbqPeekQueueElement(&decodeUnitQueue, (void**)&qdu);
    if (err != LBQ_SUCCESS) {
        return false;
    }

    *decodeUnit = &qdu->decodeUnit;
    return true;
}

void LiWakeWaitForVideoFrame(void) {
    LbqSignalQueueUserWake(&decodeUnitQueue);
}


FILE *ql;
int usedforQl = 1;
int openedQl = 0;

void openQl() {
    ql = fopen("sylarQLog.csv","w+");
    enQRateFile = fopen("sylarAvgRateLog.csv","w+");
    fprintf(enQRateFile, "Time,AvgEnQRate\n");
}

uint64_t logTime = 0;

int playoutBufferMain() {

    gettimeofday(&streamStart, NULL);
    time_t startStream;
    if (!gottime) {
        startStreamTime = (streamStart.tv_sec) * 1000 + (streamStart.tv_usec) / 1000;
        gottime = true;
    }


    openQl();
    Limelog("In playoutBufferMain");

    if (usedforQl != 0) {
        fprintf(ql, "state,startTime,frameQSize,drstatusQSize,time,enqRate,avgEnQRate\n");
        usedforQl = 0;
    }

    enum buffer_states state;

    // sylar: variables for connecting dequeued item with the rest of the program
    PQUEUED_DECODE_UNIT qdu;
    int drStatus;
    PLENTRY_INTERNAL lastEntry;

    if (createdQ == 0) {
        frameQ = createQueue();
        drstatusQ = createQueue();
        createdQ = 1;
    }
    struct timeval startT;
    struct timeval endT;


    PltCreateMutex(&mutex);
    frameQSize = frameQ->size;
    drstatusQSize  = drstatusQ->size;

    while (1) {
        gettimeofday(&startT, NULL);
        time_t ltimeStart;
        uint64_t startMillsec = (startT.tv_sec * (uint64_t) 1000) + (startT.tv_usec / 1000 );

        PltLockMutex(&mutex);

        // sylar: Condition of switching between PLAY and FILL state will change
        //        Right now it is streaming as long as the q isn't empty
        if (frameQSize >= 25) {
            state = PLAY;
            Limelog("In PLAY state.");
        } else if (frameQSize <= 0) {
            state = FILL;
            Limelog("In FILL state.");
        }

        if (state == PLAY) {
            Limelog("Dequeuing");
            qdu = front(frameQ);
            drStatus = front(drstatusQ);
            dequeue(frameQ);
            dequeue(drstatusQ);
            Limelog("Q size = %d", frameQ->size);
            logTime = startMillsec - startStreamTime;
            Limelog("PLAY: startMillsec(%llu) - startStreamTime(%llu) = %llu", startMillsec, startStreamTime, logTime);
            fprintf(ql, "PLAY,%llu,%d,%d,%llu,%llu,%ld\n", (startMillsec - startStreamTime), frameQ->size, drstatusQ->size, NULL, NULL, NULL);
        } else if (state == FILL) {
            logTime = startMillsec - startStreamTime;
            Limelog("FILL: startMillsec(%llu) - startStreamTime(%llu) = %llu", startMillsec, startStreamTime, logTime);
            Limelog("In FILL: Did not dequeue because q size = %d", frameQSize);
            fprintf(ql, "FILL,%llu,%d,%d,%llu,%llu,%ld\n", (startMillsec - startStreamTime), frameQ->size, drstatusQ->size, NULL, NULL, NULL);


        }

        frameQSize = frameQ->size;
        drstatusQSize = drstatusQ->size;
        Limelog("Frame Q size = %d, drstatus Q size = %d", frameQSize, drstatusQSize);
        logTime = startMillsec - startStreamTime;
        Limelog("OUTSIDE: startMillsec(%llu) - startStreamTime(%llu) = %llu", startMillsec, startStreamTime, logTime);
        fprintf(ql, "OUTSIDE,%llu,%d,%d,%llu,%llu,%ld\n", (startMillsec - startStreamTime), frameQSize, drstatusQSize, NULL, NULL, NULL);

        PltUnlockMutex(&mutex);

        // sylar: Connecting dequeued item to the rest of the program if in PLAY

        if (state == PLAY) {
            if (qdu->decodeUnit.frameType == FRAME_TYPE_IDR) {
                notifyKeyFrameReceived();
            }

            if (drStatus == DR_NEED_IDR) {
                Limelog("Requesting IDR frame on behalf of DR\n");
                requestDecoderRefresh();
            }
            else if (drStatus == DR_OK && qdu->decodeUnit.frameType == FRAME_TYPE_IDR) {
                // Remember that the IDR frame was processed. We can now use
                // reference frame invalidation.
                idrFrameProcessed = true;
            }

            while (qdu->decodeUnit.bufferList != NULL) {
                lastEntry = (PLENTRY_INTERNAL)qdu->decodeUnit.bufferList;
                qdu->decodeUnit.bufferList = lastEntry->entry.next;
                free(lastEntry->allocPtr);
            }

            // We will have stack-allocated entries iff we have a direct-submit decoder
            if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                free(qdu);
            }

        }
        struct timeval startSleep;
        struct timeval endSleep;

        gettimeofday(&endT, NULL);
        //gettimeofday(&startT, NULL);
        time_t ltimeEnd;
        uint64_t endMillsec = (endT.tv_sec * (uint64_t) 1000) + (endT.tv_usec / 1000 );
        uint64_t ellapsedMilli = endMillsec - startMillsec;
        long diff = targetTime - ellapsedMilli;
        long diff2 = (uint64_t)avgEnQRate - ellapsedMilli;

        Limelog("avg = %llu, start = %llu, end = %llu, elapsed = %llu", (uint64_t)avgEnQRate, startMillsec, endMillsec, ellapsedMilli);
        Limelog("diff = %llu, diff2 = %llu", diff, diff2);

        if (diff2 <= 0) {
            // sylar: if diff2 is <=0, use the hard-coded sleep time, otherwise the program crashes.
            if (diff >= 0) {
                gettimeofday(&startSleep, NULL);
                time_t lstartsleepT;
                long startSleepMs = (startSleep.tv_sec) * 1000 + (startSleep.tv_usec) / 1000 ;
                Limelog("Going to sleep, using diff");
                Sleep(diff - catchUp);
                Limelog("Slept for %ld\n ms", (diff - catchUp));
                gettimeofday(&endSleep, NULL);
                time_t lendsleepT;
                long endSleepMs = (endSleep.tv_sec) * 1000 + (endSleep.tv_usec) / 1000 ;
                long slepTime = endSleepMs - startSleepMs;
                catchUp = slepTime - diff;
            }

        // sylar: this is where avgEnQRate is not 0 and the program tries to matche the deq rate with enq rate
        } else if (diff2 > 0) {

            gettimeofday(&startSleep, NULL);
            time_t lstartsleepT;
            long startSleepMs = (startSleep.tv_sec) * 1000 + (startSleep.tv_usec) / 1000 ;
            Limelog("Going to sleep, using diff2");
            Sleep(diff2 - catchUp);
            Limelog("Slept for %ld\n ms", (diff2 - catchUp));
            gettimeofday(&endSleep, NULL);
            time_t lendsleepT;
            long endSleepMs = (endSleep.tv_sec) * 1000 + (endSleep.tv_usec) / 1000 ;
            long slepTime = endSleepMs - startSleepMs;
            catchUp = slepTime - diff2;
        }


    }

    return 0;
}



void openenQl() {
    enQRateFile = fopen("sylarAvgRateLog.csv","w+");
    fprintf(enQRateFile, "Time,AvgEnQRate\n");
}



// Cleanup a decode unit by freeing the buffer chain and the holder
void LiCompleteVideoFrame(VIDEO_FRAME_HANDLE handle, int drStatus) {

    if (!hasFirstLine) {
        //openenQl();
        hasFirstLine = true;
    }

    Limelog("In LiCompleteVideoFrame");
    PQUEUED_DECODE_UNIT qdu = handle;
    int err;
    frameHandle = handle;
    drstatusHandle = drStatus;
    PLENTRY_INTERNAL lastEntry;


    // sylar: log LiCompleteVideoFrame() called time
    gettimeofday(&newTimer);
    time_t lnewT;
    if (!gotInitTime) {
        initialT = (newTimer.tv_sec * (uint64_t)1000) + (newTimer.tv_usec / 1000);
        gotInitTime = true;
    }


    // sylar: set the first elapsed time to 0, otherwise the number is massive
    if (!gotFirstTime) {
        elaspedT = newT - oldT;
        eTimeForLog = ((newTimer.tv_sec * (uint64_t)1000) + (newTimer.tv_usec / 1000)) - initialT;
        oldT = (newTimer.tv_sec * (uint64_t)1000) + (newTimer.tv_usec / 1000);
        gotFirstTime = true;
    } else if (gotFirstTime) {
        newT = (newTimer.tv_sec * (uint64_t)1000) + (newTimer.tv_usec / 1000);
        elaspedT = newT - oldT;
        eTimeForLog = newT - initialT;
        oldT = newT;
    }

    Limelog("Interframe Time: %llu", elaspedT);

    // filling up the interframe rates array for the first time
    if (counter < 9) {
        interframeRates[counter] = elaspedT;
        Limelog("Counter = %d, framerate = %llu", counter, elaspedT);
        fprintf(ql, "ENQ,%llu,%d,%d,%llu,%llu,%lf\n", NULL, NULL, NULL, eTimeForLog, elaspedT, NULL);

        counter++;
    }
    // array filled, start calculating the enqueue rate
    else if (counter == 9) {
        interframeRates[counter] = elaspedT;
        Limelog("Counter = %d, framerate = %llu", counter, elaspedT);
        uint64_t sum = 0;
        // start computing average enqueue rate
        for (int i = 0; i < 10; i++) {
            sum += interframeRates[i];
        }
        Limelog("Sum = %llu", sum);
        avgEnQRate = sum / (uint64_t)10;

        // shifting the average window right by 1
        for (int i = 0; i < 9; i++) {
            interframeRates[i] = interframeRates[i + 1];
        }
        fprintf(ql, "ENQ,%llu,%d,%d,%llu,%llu,%lf\n", NULL, NULL, NULL, eTimeForLog, elaspedT, avgEnQRate);
        Limelog("Time: %llu, Average Enqueue Rate: %lf", eTimeForLog, avgEnQRate);
    }


    //fprintf(enQRateFile, "%llu,%lf\n", eTimeForLog, avgEnQRate);

    PltLockMutex(&mutex);
    enqueue(frameQ, frameHandle);
    enqueue(drstatusQ, drstatusHandle);
    PltUnlockMutex(&mutex);
    Limelog("Enqueued");



    PQUEUED_DECODE_UNIT qduHandle = frameHandle;

    char name[] = "LiCompleteVideoFrame";
    logMsg(name, NULL, .0, NULL, NULL);

#define DO_SPLIT

#ifdef DO_SPLIT
        Limelog("In DO_SPLIT.");
        return;
#endif


    if (qdu->decodeUnit.frameType == FRAME_TYPE_IDR) {
        notifyKeyFrameReceived();
    }

    if (drStatus == DR_NEED_IDR) {
        Limelog("Requesting IDR frame on behalf of DR\n");
        requestDecoderRefresh();
    }
    else if (drStatus == DR_OK && qdu->decodeUnit.frameType == FRAME_TYPE_IDR) {
        // Remember that the IDR frame was processed. We can now use
        // reference frame invalidation.
        idrFrameProcessed = true;
    }


    while (qdu->decodeUnit.bufferList != NULL) {
        lastEntry = (PLENTRY_INTERNAL)qdu->decodeUnit.bufferList;
        qdu->decodeUnit.bufferList = lastEntry->entry.next;
        free(lastEntry->allocPtr);
    }

    // We will have stack-allocated entries iff we have a direct-submit decoder
    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        free(qdu);
    }

}

static bool isSeqReferenceFrameStart(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    if (!getAnnexBStartSequence(buffer, &startSeq)) {
        return false;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        return H264_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == 5;
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        switch (HEVC_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length])) {
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
                return true;

            default:
                return false;
        }
    }
    else {
        LC_ASSERT(false);
        return false;
    }
}

static bool isAccessUnitDelimiter(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    if (!getAnnexBStartSequence(buffer, &startSeq)) {
        return false;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        return H264_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == H264_NAL_TYPE_AUD;
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        return HEVC_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == HEVC_NAL_TYPE_AUD;
    }
    else {
        LC_ASSERT(false);
        return false;
    }
}

static bool isSeiNal(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    if (!getAnnexBStartSequence(buffer, &startSeq)) {
        return false;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        return H264_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == H264_NAL_TYPE_SEI;
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        return HEVC_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == HEVC_NAL_TYPE_SEI;
    }
    else {
        LC_ASSERT(false);
        return false;
    }
}

// Advance the buffer descriptor to the start of the next NAL or end of buffer
static void skipToNextNalOrEnd(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    // If we're starting on a NAL boundary, skip to the next one
    if (getAnnexBStartSequence(buffer, &startSeq)) {
        buffer->offset += startSeq.length;
        buffer->length -= startSeq.length;
    }

    // Loop until we find an Annex B start sequence (3 or 4 byte)
    while (!getAnnexBStartSequence(buffer, NULL)) {
        if (buffer->length == 0) {
            // Reached the end of the buffer
            return;
        }

        buffer->offset++;
        buffer->length--;
    }
}

// Advance the buffer descriptor to the start of the next NAL
static void skipToNextNal(PBUFFER_DESC buffer) {
    skipToNextNalOrEnd(buffer);

    // If we skipped all the data, something has gone horribly wrong
    LC_ASSERT(buffer->length > 0);
}

static bool isIdrFrameStart(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    if (!getAnnexBStartSequence(buffer, &startSeq)) {
        return false;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        return H264_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == H264_NAL_TYPE_SPS;
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        return HEVC_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == HEVC_NAL_TYPE_VPS;
    }
    else {
        LC_ASSERT(false);
        return false;
    }
}

// Reassemble the frame with the given frame number
static void reassembleFrame(int frameNumber) {
    char name[] = "reassembleFrame";
    logMsg(name, NULL, .0, NULL, NULL);

    if (nalChainHead != NULL) {
        QUEUED_DECODE_UNIT qduDS;
        PQUEUED_DECODE_UNIT qdu;

        // Use a stack allocation if we won't be queuing this
        if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
            qdu = (PQUEUED_DECODE_UNIT)malloc(sizeof(*qdu));
        }
        else {
            qdu = &qduDS;
        }

        if (qdu != NULL) {
            qdu->decodeUnit.bufferList = nalChainHead;
            qdu->decodeUnit.fullLength = nalChainDataLength;
            qdu->decodeUnit.frameNumber = frameNumber;
            qdu->decodeUnit.receiveTimeMs = firstPacketReceiveTime;
            qdu->decodeUnit.presentationTimeMs = firstPacketPresentationTime;
            qdu->decodeUnit.enqueueTimeMs = LiGetMillis();

            // IDR frames will have leading CSD buffers
            if (nalChainHead->bufferType != BUFFER_TYPE_PICDATA) {
                qdu->decodeUnit.frameType = FRAME_TYPE_IDR;
            }
            else {
                qdu->decodeUnit.frameType = FRAME_TYPE_PFRAME;
            }

            nalChainHead = nalChainTail = NULL;
            nalChainDataLength = 0;

            if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {


                if (LbqOfferQueueItem(&decodeUnitQueue, qdu, &qdu->entry) == LBQ_BOUND_EXCEEDED) {
                    Limelog("Video decode unit queue overflow\n");

                    // RFI recovery is not supported here
                    waitingForIdrFrame = true;

                    // Clear NAL state for the frame that we failed to enqueue
                    nalChainHead = qdu->decodeUnit.bufferList;
                    nalChainDataLength = qdu->decodeUnit.fullLength;
                    dropFrameState();

                    // Free the DU we were going to queue
                    free(qdu);

                    // Free all frames in the decode unit queue
                    freeDecodeUnitList(LbqFlushQueueItems(&decodeUnitQueue));

                    // Request an IDR frame to recover
                    LiRequestIdrFrame();
                    return;
                }
            }
            else {
                // Submit the frame to the decoder
                LiCompleteVideoFrame(qdu, VideoCallbacks.submitDecodeUnit(&qdu->decodeUnit));
            }

            // Notify the control connection
            connectionReceivedCompleteFrame(frameNumber);

            // Clear frame drops
            consecutiveFrameDrops = 0;

            // Move the start of our (potential) RFI window to the next frame
            startFrameNumber = nextFrameNumber;
        }
    }
}

static int getBufferFlags(char* data, int length) {
    BUFFER_DESC buffer;
    BUFFER_DESC candidate;

    buffer.data = data;
    buffer.length = (unsigned int)length;
    buffer.offset = 0;

    if (!getAnnexBStartSequence(&buffer, &candidate)) {
        return BUFFER_TYPE_PICDATA;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        switch (H264_NAL_TYPE(candidate.data[candidate.offset + candidate.length])) {
        case H264_NAL_TYPE_SPS:
            return BUFFER_TYPE_SPS;

        case H264_NAL_TYPE_PPS:
            return BUFFER_TYPE_PPS;

        default:
            return BUFFER_TYPE_PICDATA;
        }
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        switch (HEVC_NAL_TYPE(candidate.data[candidate.offset + candidate.length])) {
            case HEVC_NAL_TYPE_SPS:
                return BUFFER_TYPE_SPS;

            case HEVC_NAL_TYPE_PPS:
                return BUFFER_TYPE_PPS;

            case HEVC_NAL_TYPE_VPS:
                return BUFFER_TYPE_VPS;

            default:
                return BUFFER_TYPE_PICDATA;
        }
    }
    else {
        LC_ASSERT(false);
        return BUFFER_TYPE_PICDATA;
    }
}

// As an optimization, we can cast the existing packet buffer to a PLENTRY and avoid
// a malloc() and a memcpy() of the packet data.
static void queueFragment(PLENTRY_INTERNAL* existingEntry, char* data, int offset, int length) {
    PLENTRY_INTERNAL entry;

    if (existingEntry == NULL || *existingEntry == NULL) {
        entry = (PLENTRY_INTERNAL)malloc(sizeof(*entry) + length);
    }
    else {
        entry = *existingEntry;
    }

    if (entry != NULL) {
        entry->entry.next = NULL;
        entry->entry.length = length;

        // If we had to allocate a new entry, we must copy the data. If not,
        // the data already resides within the LENTRY allocation.
        if (existingEntry == NULL || *existingEntry == NULL) {
            entry->allocPtr = entry;

            entry->entry.data = (char*)(entry + 1);
            memcpy(entry->entry.data, &data[offset], entry->entry.length);
        }
        else {
            entry->entry.data = &data[offset];

            // The caller should have already set this up for us
            LC_ASSERT(entry->allocPtr != NULL);

            // We now own the packet buffer and will manage freeing it
            *existingEntry = NULL;
        }

        entry->entry.bufferType = getBufferFlags(entry->entry.data, entry->entry.length);

        nalChainDataLength += entry->entry.length;

        if (nalChainTail == NULL) {
            LC_ASSERT(nalChainHead == NULL);
            nalChainHead = nalChainTail = (PLENTRY)entry;
        }
        else {
            LC_ASSERT(nalChainHead != NULL);
            nalChainTail->next = (PLENTRY)entry;
            nalChainTail = nalChainTail->next;
        }
    }
}

// Process an RTP Payload using the slow path that handles multiple NALUs per packet
static void processRtpPayloadSlow(PBUFFER_DESC currentPos, PLENTRY_INTERNAL* existingEntry) {
    // We should not have any NALUs when processing the first packet in an IDR frame
    LC_ASSERT(nalChainHead == NULL);
    LC_ASSERT(nalChainTail == NULL);

    while (currentPos->length != 0) {
        // Skip through any padding bytes
        if (!getAnnexBStartSequence(currentPos, NULL)) {
            skipToNextNal(currentPos);
        }

        // Skip any prepended AUD or SEI NALUs. We may have padding between
        // these on IDR frames, so the check in processRtpPayload() is not
        // completely sufficient to handle that case.
        while (isAccessUnitDelimiter(currentPos) || isSeiNal(currentPos)) {
            skipToNextNal(currentPos);
        }

        int start = currentPos->offset;
        bool containsPicData = false;

#ifdef FORCE_3_BYTE_START_SEQUENCES
        start++;
#endif

        // Now we're decoding a frame
        decodingFrame = true;

        if (isSeqReferenceFrameStart(currentPos)) {
            // No longer waiting for an IDR frame
            waitingForIdrFrame = false;
            waitingForRefInvalFrame = false;

            // Cancel any pending IDR frame request
            waitingForNextSuccessfulFrame = false;

            // Use the cached LENTRY for this NALU since it will be
            // the bulk of the data in this packet.
            containsPicData = true;
        }

        // Move to the next NALU
        skipToNextNalOrEnd(currentPos);

        // If this is the picture data, we expect it to extend to the end of the packet
        if (containsPicData) {
            while (currentPos->length != 0) {
                // Any NALUs we encounter on the way to the end of the packet must be reference frame slices
                LC_ASSERT(isSeqReferenceFrameStart(currentPos));
                skipToNextNalOrEnd(currentPos);
            }
        }

        // To minimize copies, we'll allocate for SPS, PPS, and VPS to allow
        // us to reuse the packet buffer for the picture data in the I-frame.
        queueFragment(containsPicData ? existingEntry : NULL,
                      currentPos->data, start, currentPos->offset - start);
    }
}

// Dumps the decode unit queue and ensures the next frame submitted to the decoder will be
// an IDR frame
void requestDecoderRefresh(void) {
    // Wait for the next IDR frame
    waitingForIdrFrame = true;

    // Flush the decode unit queue
    freeDecodeUnitList(LbqFlushQueueItems(&decodeUnitQueue));

    // Request the receive thread drop its state
    // on the next call. We can't do it here because
    // it may be trying to queue DUs and we'll nuke
    // the state out from under it.
    dropStatePending = true;

    // Request the IDR frame
    LiRequestIdrFrame();
}

// Return 1 if packet is the first one in the frame
static int isFirstPacket(uint8_t flags, uint8_t fecBlockNumber) {
    // Clear the picture data flag
    flags &= ~FLAG_CONTAINS_PIC_DATA;

    // Check if it's just the start or both start and end of a frame
    return (flags == (FLAG_SOF | FLAG_EOF) || flags == FLAG_SOF) && fecBlockNumber == 0;
}

// Process an RTP Payload
// The caller will free *existingEntry unless we NULL it
static void processRtpPayload(PNV_VIDEO_PACKET videoPacket, int length,
                       uint64_t receiveTimeMs, unsigned int presentationTimeMs,
                       PLENTRY_INTERNAL* existingEntry) {
    char name[] = "processRtpPayload";
    logMsg(name, NULL, .0, NULL, NULL);

    BUFFER_DESC currentPos;
    uint32_t frameIndex;
    uint8_t flags;
    uint32_t firstPacket;
    uint32_t streamPacketIndex;
    uint8_t fecCurrentBlockNumber;
    uint8_t fecLastBlockNumber;

    // Mask the top 8 bits from the SPI
    videoPacket->streamPacketIndex >>= 8;
    videoPacket->streamPacketIndex &= 0xFFFFFF;

    currentPos.data = (char*)(videoPacket + 1);
    currentPos.offset = 0;
    currentPos.length = length - sizeof(*videoPacket);

    fecCurrentBlockNumber = (videoPacket->multiFecBlocks >> 4) & 0x3;
    fecLastBlockNumber = (videoPacket->multiFecBlocks >> 6) & 0x3;
    frameIndex = videoPacket->frameIndex;
    flags = videoPacket->flags;
    firstPacket = isFirstPacket(flags, fecCurrentBlockNumber);

    LC_ASSERT((flags & ~(FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA)) == 0);

    streamPacketIndex = videoPacket->streamPacketIndex;

    // Drop packets from a previously corrupt frame
    if (isBefore32(frameIndex, nextFrameNumber)) {
        return;
    }

    // The FEC queue can sometimes recover corrupt frames (see comments in RtpFecQueue).
    // It almost always detects them before they get to us, but in case it doesn't
    // the streamPacketIndex not matching correctly should find nearly all of the rest.
    if (isBefore24(streamPacketIndex, U24(lastPacketInStream + 1)) ||
            (!(flags & FLAG_SOF) && streamPacketIndex != U24(lastPacketInStream + 1))) {
        Limelog("Depacketizer detected corrupt frame: %d", frameIndex);
        decodingFrame = false;
        nextFrameNumber = frameIndex + 1;
        dropFrameState();
        if (waitingForIdrFrame) {
            LiRequestIdrFrame();
        }
        else {
            connectionDetectedFrameLoss(startFrameNumber, frameIndex);
        }
        return;
    }

    // Verify that we didn't receive an incomplete frame
    LC_ASSERT(firstPacket ^ decodingFrame);

    // Check sequencing of this frame to ensure we didn't
    // miss one in between
    if (firstPacket) {
        // Make sure this is the next consecutive frame
        if (isBefore32(nextFrameNumber, frameIndex)) {
            if (nextFrameNumber + 1 == frameIndex) {
                Limelog("Network dropped 1 frame (frame %d)\n", frameIndex - 1);
            }
            else {
                Limelog("Network dropped %d frames (frames %d to %d)\n",
                        frameIndex - nextFrameNumber,
                        nextFrameNumber,
                        frameIndex - 1);
            }

            nextFrameNumber = frameIndex;

            // Wait until next complete frame
            waitingForNextSuccessfulFrame = true;
            dropFrameState();
        }
        else {
            LC_ASSERT(nextFrameNumber == frameIndex);
        }

        // We're now decoding a frame
        decodingFrame = true;
        firstPacketReceiveTime = receiveTimeMs;
        firstPacketPresentationTime = presentationTimeMs;
    }

    lastPacketInStream = streamPacketIndex;

    // If this is the first packet, skip the frame header (if one exists)
    if (firstPacket) {
        // Parse the frame type from the header
        if (APP_VERSION_AT_LEAST(7, 1, 350)) {
            switch (currentPos.data[currentPos.offset + 3]) {
            case 1: // Normal P-frame
                break;
            case 2: // IDR frame
            case 4: // Intra-refresh
            case 5: // P-frame with reference frames invalidated
                if (waitingForRefInvalFrame) {
                    Limelog("Next post-invalidation frame is: %d (%s-frame)\n",
                            frameIndex,
                            currentPos.data[currentPos.offset + 3] == 5 ? "P" : "I");
                    waitingForRefInvalFrame = false;
                    waitingForNextSuccessfulFrame = false;
                }
                break;
            case 104: // Sunshine hardcoded header
                break;
            default:
                Limelog("Unrecognized frame type: %d", currentPos.data[currentPos.offset + 3]);
                LC_ASSERT(false);
                break;
            }
        }
        else {
            // Hope for the best with older servers
            if (waitingForRefInvalFrame) {
                connectionDetectedFrameLoss(startFrameNumber, frameIndex - 1);
                waitingForRefInvalFrame = false;
                waitingForNextSuccessfulFrame = false;
            }
        }

        if (APP_VERSION_AT_LEAST(7, 1, 446)) {
            // >= 7.1.446 uses 2 different header lengths based on the first byte:
            // 0x01 indicates an 8 byte header
            // 0x81 indicates a 41 byte header
            if (currentPos.data[0] == 0x01) {
                currentPos.offset += 8;
                currentPos.length -= 8;
            }
            else {
                LC_ASSERT(currentPos.data[0] == (char)0x81);
                currentPos.offset += 41;
                currentPos.length -= 41;
            }
        }
        else if (APP_VERSION_AT_LEAST(7, 1, 415)) {
            // [7.1.415, 7.1.446) uses 2 different header lengths based on the first byte:
            // 0x01 indicates an 8 byte header
            // 0x81 indicates a 24 byte header
            if (currentPos.data[0] == 0x01) {
                currentPos.offset += 8;
                currentPos.length -= 8;
            }
            else {
                LC_ASSERT(currentPos.data[0] == (char)0x81);
                currentPos.offset += 24;
                currentPos.length -= 24;
            }
        }
        else if (APP_VERSION_AT_LEAST(7, 1, 350)) {
            // [7.1.350, 7.1.415) should use the 8 byte header again
            currentPos.offset += 8;
            currentPos.length -= 8;
        }
        else if (APP_VERSION_AT_LEAST(7, 1, 320)) {
            // [7.1.320, 7.1.350) should use the 12 byte frame header
            currentPos.offset += 12;
            currentPos.length -= 12;
        }
        else if (APP_VERSION_AT_LEAST(5, 0, 0)) {
            // [5.x, 7.1.320) should use the 8 byte header
            currentPos.offset += 8;
            currentPos.length -= 8;
        }
        else {
            // Other versions don't have a frame header at all
        }

        // The Annex B NALU start prefix must be next
        if (!getAnnexBStartSequence(&currentPos, NULL)) {
            // If we aren't starting on a start prefix, something went wrong.
            LC_ASSERT(false);

            // For release builds, we will try to recover by searching for one.
            // This mimics the way most decoders handle this situation.
            skipToNextNal(&currentPos);
        }

        // If an AUD NAL is prepended to this frame data, remove it.
        // Other parts of this code are not prepared to deal with a
        // NAL of that type, so stripping it is the easiest option.
        if (isAccessUnitDelimiter(&currentPos)) {
            skipToNextNal(&currentPos);
        }

        // There may be one or more SEI NAL units prepended to the
        // frame data *after* the (optional) AUD.
        while (isSeiNal(&currentPos)) {
            skipToNextNal(&currentPos);
        }
    }

    if (firstPacket && isIdrFrameStart(&currentPos))
    {
        // SPS and PPS prefix is padded between NALs, so we must decode it with the slow path
        processRtpPayloadSlow(&currentPos, existingEntry);
    }
    else
    {
#ifdef FORCE_3_BYTE_START_SEQUENCES
        if (firstPacket) {
            currentPos.offset++;
            currentPos.length--;
        }
#endif
        queueFragment(existingEntry, currentPos.data, currentPos.offset, currentPos.length);
    }

    if ((flags & FLAG_EOF) && fecCurrentBlockNumber == fecLastBlockNumber) {
        // Move on to the next frame
        decodingFrame = false;
        nextFrameNumber = frameIndex + 1;

        // If we can't submit this frame due to a discontinuity in the bitstream,
        // inform the host (if needed) and drop the data.
        if (waitingForIdrFrame || waitingForRefInvalFrame) {
            // IDR wait takes priority over RFI wait (and an IDR frame will satisfy both)
            if (waitingForIdrFrame) {
                Limelog("Waiting for IDR frame\n");

                // We wait for the first fully received frame after a loss to approximate
                // detection of the recovery of the network. Requesting an IDR frame while
                // the network is unstable will just contribute to congestion collapse.
                if (waitingForNextSuccessfulFrame) {
                    LiRequestIdrFrame();
                }
            }
            else {
                // If we need an RFI frame first, then drop this frame
                // and update the reference frame invalidation window.
                Limelog("Waiting for RFI frame\n");
                connectionDetectedFrameLoss(startFrameNumber, frameIndex);
            }

            waitingForNextSuccessfulFrame = false;
            dropFrameState();
            return;
        }

        LC_ASSERT(!waitingForNextSuccessfulFrame);

        // Carry out any pending state drops. We can't just do this
        // arbitrarily in the middle of processing a frame because
        // may cause the depacketizer state to become corrupted. For
        // example, if we drop state after the first packet, the
        // depacketizer will next try to process a non-SOF packet,
        // and cause it to assert.
        if (dropStatePending) {
            if (nalChainHead && nalChainHead->bufferType != BUFFER_TYPE_PICDATA) {
                // Don't drop the frame state if this frame is an IDR frame itself,
                // otherwise we'll lose this IDR frame without another in flight
                // and have to wait until we hit our consecutive drop limit to
                // request a new one (potentially several seconds).
                dropStatePending = false;
            }
            else {
                dropFrameState();
                return;
            }
        }

        reassembleFrame(frameIndex);
    }
}

// Called by the video RTP FEC queue to notify us of a lost frame
// if it determines the frame to be unrecoverable. This lets us
// avoid having to wait until the next received frame to determine
// that we lost a frame and submit an RFI request.
void notifyFrameLost(unsigned int frameNumber, bool speculative) {
    // This may only be called at frame boundaries
    LC_ASSERT(!decodingFrame);

    // We may not invalidate frames that we've already received
    LC_ASSERT(frameNumber >= startFrameNumber);

    // If RFI is enabled, we will notify the host PC now
    if (isReferenceFrameInvalidationEnabled()) {
        if (speculative) {
            Limelog("Sending speculative RFI request for predicted loss of frame %d\n", frameNumber);
        }
        else {
            Limelog("Sending RFI request for unrecoverable frame %d\n", frameNumber);
        }

        // Advance the frame number since we won't be expecting this one anymore
        nextFrameNumber = frameNumber + 1;

        // Drop any existing frame state (shouldn't have any) and set the RFI wait flag
        dropFrameState();

        // Notify the host that we lost this one
        connectionDetectedFrameLoss(startFrameNumber, frameNumber);
    }
}

// Add an RTP Packet to the queue
void queueRtpPacket(PRTPV_QUEUE_ENTRY queueEntryPtr) {
    char name[] = "queueRtpPacket";
    logMsg(name, NULL, .0, NULL, NULL);

    int dataOffset;
    RTPV_QUEUE_ENTRY queueEntry = *queueEntryPtr;

    LC_ASSERT(!queueEntry.isParity);
    LC_ASSERT(queueEntry.receiveTimeMs != 0);

    dataOffset = sizeof(*queueEntry.packet);
    if (queueEntry.packet->header & FLAG_EXTENSION) {
        dataOffset += 4; // 2 additional fields
    }

    // Reuse the memory reserved for the RTPFEC_QUEUE_ENTRY to store the LENTRY_INTERNAL
    // now that we're in the depacketizer. We saved a copy of the real FEC queue entry
    // on the stack here so we can safely modify this memory in place.
    LC_ASSERT(sizeof(LENTRY_INTERNAL) <= sizeof(RTPV_QUEUE_ENTRY));
    PLENTRY_INTERNAL existingEntry = (PLENTRY_INTERNAL)queueEntryPtr;
    existingEntry->allocPtr = queueEntry.packet;

    processRtpPayload((PNV_VIDEO_PACKET)(((char*)queueEntry.packet) + dataOffset),
                      queueEntry.length - dataOffset,
                      queueEntry.receiveTimeMs,
                      queueEntry.presentationTimeMs,
                      &existingEntry);

    if (existingEntry != NULL) {
        // processRtpPayload didn't want this packet, so just free it
        free(existingEntry->allocPtr);
    }
}

int LiGetPendingVideoFrames(void) {
    return LbqGetItemCount(&decodeUnitQueue);
}
