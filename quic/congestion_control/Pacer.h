/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#pragma once

#include <quic/state/StateData.h>

namespace quic {

/**
 * Returns a pair consisting of the length of the burst interval and the number
 * of packets in a burst interval.
 */
using PacingRateCalculator = folly::Function<PacingRate(
    const QuicConnectionStateBase&,
    uint64_t cwndBytes,
    uint64_t minCwndInMss,
    std::chrono::microseconds rtt)>;

class DefaultPacer : public Pacer {
 public:
  explicit DefaultPacer(
      const QuicConnectionStateBase& conn,
      uint64_t minCwndInMss);

  void refreshPacingRate(
      uint64_t cwndBytes,
      std::chrono::microseconds rtt,
      TimePoint currentTime = Clock::now()) override;

  // rate_bps is *bytes* per second
  void setPacingRate(QuicConnectionStateBase& conn, uint64_t rate_bps) override;

  void resetPacingTokens() override;

  std::chrono::microseconds getTimeUntilNextWrite() const override;

  uint64_t updateAndGetWriteBatchSize(TimePoint currentTime) override;

  void setPacingRateCalculator(PacingRateCalculator pacingRateCalculator);

  uint64_t getCachedWriteBatchSize() const override;

  void onPacketSent() override;
  void onPacketsLoss() override;

 private:
  const QuicConnectionStateBase& conn_;
  uint64_t minCwndInMss_;
  uint64_t batchSize_;
  std::chrono::microseconds writeInterval_{0};
  PacingRateCalculator pacingRateCalculator_;
  uint64_t cachedBatchSize_;
  uint64_t tokens_;
  folly::Optional<TimePoint> lastWriteTime_;
};
} // namespace quic
