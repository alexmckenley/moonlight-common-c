#include "Limelight-internal.h"

// Exercise the real RTP queue with a deterministic clock and synthetic packets.
// The control stream and depacketizer endpoints only record queue outputs.
int AppVersionQuad[4] = {7, 1, 431, -1};
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;

static uint64_t nowUs;
static unsigned int speculativeReports;
static unsigned int finalReports;
static unsigned int lastLostFrame;
static unsigned int submittedPackets;
static uint16_t expectedSequence;
static bool checkSequence;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

uint64_t PltGetMicroseconds(void) { return nowUs; }
void connectionSawFrame(uint32_t frameIndex) { }
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS fecStatus) { }
void notifyFrameLost(unsigned int frameNumber, bool speculative) {
    lastLostFrame = frameNumber;
    if (speculative) speculativeReports++;
    else finalReports++;
}
void queueRtpPacket(PRTPV_QUEUE_ENTRY entry) {
    if (checkSequence) {
        CHECK(entry->packet->sequenceNumber == expectedSequence);
        expectedSequence++;
    }
    submittedPackets++;
    free(entry->packet);
}

static void initialize(PRTP_VIDEO_QUEUE queue) {
    nowUs = 1000000;
    speculativeReports = finalReports = lastLostFrame = submittedPackets = 0;
    expectedSequence = 0;
    checkSequence = false;
    StreamConfig.packetSize = 64;
    RtpvInitializeQueue(queue);
}

static int sendPacket(PRTP_VIDEO_QUEUE queue, unsigned int frame, uint16_t base,
                      unsigned int index, unsigned int dataPackets,
                      unsigned int fecPercentage, unsigned int block,
                      unsigned int lastBlock) {
    int length = MAX_RTP_HEADER_SIZE + StreamConfig.packetSize;
    PRTP_PACKET packet = calloc(1, length + sizeof(RTPV_QUEUE_ENTRY));
    CHECK(packet != NULL);
    PRTPV_QUEUE_ENTRY entry = (PRTPV_QUEUE_ENTRY)((char*)packet + length);
    packet->header = FLAG_EXTENSION;
    packet->sequenceNumber = (uint16_t)(base + index);
    packet->timestamp = frame * 1500;
    PNV_VIDEO_PACKET video = (PNV_VIDEO_PACKET)((char*)packet + sizeof(*packet) + 4);
    video->frameIndex = LE32(frame);
    video->streamPacketIndex = LE32(index << 8);
    video->fecInfo = LE32((dataPackets << 22) | (index << 12) | (fecPercentage << 4));
    video->multiFecFlags = 0x10;
    video->multiFecBlocks = (uint8_t)((lastBlock << 6) | (block << 4));
    video->flags = FLAG_CONTAINS_PIC_DATA;
    if (index == 0) video->flags |= FLAG_SOF;
    if (index == dataPackets - 1) video->flags |= FLAG_EOF;
    int result = RtpvAddPacket(queue, packet, length, entry);
    if (result == RTPF_RET_REJECTED) free(packet);
    return result;
}

#define SEND(q, frame, base, index) sendPacket(q, frame, base, index, 48, 0, 0, 0)

static void testOrdered(void) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    checkSequence = true;
    for (unsigned int i = 0; i < 48; i++) CHECK(SEND(&q, 1, 0, i) == RTPF_RET_QUEUED);
    CHECK(submittedPackets == 48);
    CHECK(speculativeReports == 0 && finalReports == 0);
    CHECK(nowUs == 1000000); // A complete frame never waits for the grace period.
    RtpvCleanupQueue(&q);
}

static void testReordered(unsigned int delay, uint16_t base) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    checkSequence = true;
    expectedSequence = base;
    CHECK(SEND(&q, 1, base, 46) == RTPF_RET_QUEUED);
    CHECK(SEND(&q, 1, base, 47) == RTPF_RET_QUEUED);
    CHECK(speculativeReports == 0);
    nowUs += delay;
    for (unsigned int i = 0; i < 46; i++) CHECK(SEND(&q, 1, base, i) == RTPF_RET_QUEUED);
    CHECK(submittedPackets == 48);
    CHECK(speculativeReports == 0 && finalReports == 0);
    CHECK(q.receivedOosData);
    RtpvCleanupQueue(&q);
}

static void testPersistentLoss(void) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    SEND(&q, 1, 0, 10);
    CHECK(speculativeReports == 0);
    nowUs += 1999;
    SEND(&q, 1, 0, 11);
    CHECK(speculativeReports == 0);
    nowUs++;
    SEND(&q, 1, 0, 12);
    CHECK(speculativeReports == 1 && lastLostFrame == 1);
    nowUs += 5000;
    SEND(&q, 1, 0, 13);
    SEND(&q, 2, 48, 0);
    CHECK(speculativeReports == 1 && finalReports == 0);
    RtpvCleanupQueue(&q);
}

static void testBoundaryFallback(void) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    SEND(&q, 1, 0, 46);
    nowUs += 16000; // No more packets from the lost frame.
    SEND(&q, 2, 48, 0);
    CHECK(speculativeReports == 0 && finalReports == 1 && lastLostFrame == 1);
    RtpvCleanupQueue(&q);
}

static void testDuplicate(void) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    SEND(&q, 1, 0, 46);
    nowUs += 3000;
    CHECK(SEND(&q, 1, 0, 46) == RTPF_RET_REJECTED);
    CHECK(speculativeReports == 0);
    SEND(&q, 1, 0, 47);
    CHECK(speculativeReports == 1);
    RtpvCleanupQueue(&q);
}

static void testFrameReset(void) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    SEND(&q, 1, 0, 46);
    nowUs += 10000;
    SEND(&q, 2, 48, 46);
    CHECK(finalReports == 1 && speculativeReports == 0);
    nowUs += 1999;
    SEND(&q, 2, 48, 47);
    CHECK(speculativeReports == 0);
    RtpvCleanupQueue(&q);
}

static void testCooldown(void) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    SEND(&q, 1, 0, 46);
    SEND(&q, 1, 0, 0); // Observed reordering disables speculation.
    nowUs += 10000;
    SEND(&q, 1, 0, 47);
    CHECK(q.receivedOosData && speculativeReports == 0);
    RtpvCleanupQueue(&q);
}

static void testFecBudget(void) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    // 48 data + 24 parity. A 10-packet hole is recoverable in both normal
    // and debug FEC-validation modes; a 45-packet hole exceeds either budget.
    sendPacket(&q, 1, 0, 10, 48, 50, 0, 0);
    nowUs += 10000;
    sendPacket(&q, 1, 0, 11, 48, 50, 0, 0);
    CHECK(speculativeReports == 0);
    sendPacket(&q, 1, 0, 47, 48, 50, 0, 0);
    CHECK(speculativeReports == 0);
    nowUs += 2000;
    sendPacket(&q, 1, 0, 48, 48, 50, 0, 0);
    CHECK(speculativeReports == 1);
    RtpvCleanupQueue(&q);
}

static void testMissingFecBlock(void) {
    RTP_VIDEO_QUEUE q;
    initialize(&q);
    sendPacket(&q, 1, 0, 46, 48, 0, 0, 1);
    nowUs += 500;
    CHECK(sendPacket(&q, 1, 48, 0, 48, 0, 1, 1) == RTPF_RET_REJECTED);
    CHECK(finalReports == 1 && speculativeReports == 0 && lastLostFrame == 1);
    RtpvCleanupQueue(&q);
}

int main(void) {
    testOrdered();
    // All seven reported capture delays, plus the edge of the grace window.
    const unsigned int delays[] = {68, 1, 3, 2, 3, 2, 3, 1999};
    for (unsigned int i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) testReordered(delays[i], 0);
    testReordered(68, 65520); // RTP sequence wraparound.
    testPersistentLoss();
    testBoundaryFallback();
    testDuplicate();
    testFrameReset();
    testCooldown();
    testFecBudget();
    testMissingFecBlock();
    puts("PASS: 17 RTP queue cases (ordering, reordering, loss, boundaries, duplicates, cooldown, FEC budget, wraparound)");
    return 0;
}
