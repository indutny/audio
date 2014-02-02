#include "channel.h"
#include "common.h"
#include "unit.h"

#include "aec/include/echo_cancellation.h"
#include "signal_processing/include/signal_processing_library.h"

namespace audio {

Channel::Channel() : ns_(NULL) {
  // Clear filters for QMF
  memset(filters_.a_lo, 0, sizeof(filters_.a_lo));
  memset(filters_.a_hi, 0, sizeof(filters_.a_hi));
  memset(filters_.s_lo, 0, sizeof(filters_.s_lo));
  memset(filters_.s_hi, 0, sizeof(filters_.s_hi));
}


void Channel::Init(Unit* unit) {
  // Initialize buffers
  PaUtilRingBuffer* rings[] = { &aec_.in, &aec_.out, &io_.in, &io_.out };
  for (size_t i = 0; i < ARRAY_SIZE(rings); i++) {
    PaUtil_InitializeRingBuffer(rings[i],
                                Unit::kSampleSize,
                                kBufferCapacity,
                                new char[Unit::kSampleSize * kBufferCapacity]);
  }

  // Initailize AEC
  int err;
  ASSERT(0 == WebRtcAec_Create(&aec_.handle), "Failed to create AEC");
  err = WebRtcAec_Init(
      aec_.handle,
      Unit::kSampleRate,
      static_cast<int32_t>(unit->GetHWSampleRate(Unit::kOutput)));
  ASSERT(err == 0, "Failed to initialize AEC");

  // Initialize NS
  ASSERT(0 == WebRtcNs_Create(&ns_), "Failed to create NS");
  ASSERT(0 == WebRtcNs_Init(ns_, Unit::kSampleRate / 2), "Failed to init NS");
}


Channel::~Channel() {
  PaUtilRingBuffer* rings[] = { &aec_.in, &aec_.out, &io_.in, &io_.out };
  for (size_t i = 0; i < ARRAY_SIZE(rings); i++) {
    delete[] rings[i]->buffer;
    rings[i]->buffer = NULL;
  }

  ASSERT(0 == WebRtcAec_Free(aec_.handle), "Failed to destroy AEC");
  aec_.handle = NULL;

  ASSERT(0 == WebRtcNs_Free(ns_), "Failed to free NS");
  ns_ = NULL;
}


void Channel::Cycle(ring_buffer_size_t avail_in, ring_buffer_size_t avail_out) {
  int16_t buf[Unit::kChunkSize];
  ring_buffer_size_t avail;

  // Feed playback data into AEC
  if (avail_out >= Unit::kChunkSize) {
    avail = PaUtil_ReadRingBuffer(&aec_.out, buf, Unit::kChunkSize);
    ASSERT(avail == Unit::kChunkSize, "Read less than expected");
    ASSERT(0 == WebRtcAec_BufferFarend(aec_.handle, buf, Unit::kChunkSize),
           "Failed to queue AEC far end");
  }

  if ((size_t) avail_in >= ARRAY_SIZE(buf)) {
    // Feed capture data into AEC
    avail = PaUtil_ReadRingBuffer(&aec_.in, buf, ARRAY_SIZE(buf));
    ASSERT(avail == ARRAY_SIZE(buf), "Read less than expected");

    int16_t lo[ARRAY_SIZE(buf) / 2];
    int16_t hi[ARRAY_SIZE(lo)];

    // Split signal
    WebRtcSpl_AnalysisQMF(buf,
                          ARRAY_SIZE(buf),
                          lo,
                          hi,
                          filters_.a_lo,
                          filters_.a_hi);

    // Apply AEC
    ASSERT(0 == WebRtcAec_Process(aec_.handle,
                                  lo,
                                  hi,
                                  lo,
                                  hi,
                                  ARRAY_SIZE(lo),
                                  0,
                                  0),
           "Failed to queue AEC near end");

    // Apply NS
    ASSERT(0 == WebRtcNs_Process(ns_, lo, hi, lo, hi),
           "Failed to apply NS");

    // Join signal
    WebRtcSpl_SynthesisQMF(lo,
                           hi,
                           ARRAY_SIZE(lo),
                           buf,
                           filters_.s_lo,
                           filters_.s_hi);

    // Write it out
    PaUtil_WriteRingBuffer(&io_.in, buf, ARRAY_SIZE(buf));
  }
}

}  // namespace audio
