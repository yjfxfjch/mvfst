/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/state/AckHandlers.h>

#include <folly/Overload.h>
#include <quic/logging/QuicLogger.h>
#include <quic/loss/QuicLossFunctions.h>
#include <quic/state/QuicStateFunctions.h>
#include <iterator>

namespace quic {

/**
 * Process ack frame and acked outstanding packets.
 *
 * This function process incoming ack blocks which is sorted in the descending
 * order of packet number. For each ack block, we try to find a continuous range
 * of outstanding packets in the connection's outstanding packets list that is
 * acked by the current ack block. The search is in the reverse order of the
 * outstandings.packets given that the list is sorted in the ascending order of
 * packet number. For each outstanding packet that is acked by current ack
 * frame, ack and loss visitors are invoked on the sent frames. The outstanding
 * packets may contain packets from all three packet number spaces. But ack is
 * always restrained to a single space. So we also need to skip packets that are
 * not in the current packet number space.
 *
 */

void processAckFrame(
    QuicConnectionStateBase& conn,
    PacketNumberSpace pnSpace,
    const ReadAckFrame& frame,
    const AckVisitor& ackVisitor,
    const LossVisitor& lossVisitor,
    const TimePoint& ackReceiveTime) {
  // TODO: send error if we get an ack for a packet we've not sent t18721184
  CongestionController::AckEvent ack;
  ack.ackTime = ackReceiveTime;
  // Using kDefaultRxPacketsBeforeAckAfterInit to reseve for ackedPackets
  // container is a hueristic. Other quic implementations may have very
  // different acking policy. It's also possibly that all acked packets are pure
  // acks which leads to different number of packets being acked usually.
  ack.ackedPackets.reserve(kDefaultRxPacketsBeforeAckAfterInit);
  auto currentPacketIt = getLastOutstandingPacket(conn, pnSpace);
  uint64_t initialPacketAcked = 0;
  uint64_t handshakePacketAcked = 0;
  uint64_t clonedPacketsAcked = 0;
  folly::Optional<decltype(conn.lossState.lastAckedPacketSentTime)>
      lastAckedPacketSentTime;
  auto ackBlockIt = frame.ackBlocks.cbegin();
  while (ackBlockIt != frame.ackBlocks.cend() &&
         currentPacketIt != conn.outstandings.packets.rend()) {
    // In reverse order, find the first outstanding packet that has a packet
    // number LE the endPacket of the current ack range.
    auto rPacketIt = std::lower_bound(
        currentPacketIt,
        conn.outstandings.packets.rend(),
        ackBlockIt->endPacket,
        [&](const auto& packetWithTime, const auto& val) {
          return packetWithTime.packet.header.getPacketSequenceNum() > val;
        });
    if (rPacketIt == conn.outstandings.packets.rend()) {
      // This means that all the packets are greater than the end packet.
      // Since we iterate the ACK blocks in reverse order of end packets, our
      // work here is done.
      VLOG(10) << __func__ << " less than all outstanding packets outstanding="
               << conn.outstandings.packets.size() << " range=["
               << ackBlockIt->startPacket << ", " << ackBlockIt->endPacket
               << "]"
               << " " << conn;
      ackBlockIt++;
      break;
    }

    // TODO: only process ACKs from packets which are sent from a greater than
    // or equal to crypto protection level.
    auto eraseEnd = rPacketIt;
    while (rPacketIt != conn.outstandings.packets.rend()) {
      auto currentPacketNum = rPacketIt->packet.header.getPacketSequenceNum();
      auto currentPacketNumberSpace =
          rPacketIt->packet.header.getPacketNumberSpace();
      if (pnSpace != currentPacketNumberSpace) {
        // When the next packet is not in the same packet number space, we need
        // to skip it in current ack processing. If the iterator has moved, that
        // means we have found packets in the current space that are acked by
        // this ack block. So the code erases the current iterator range and
        // move the iterator to be the next search point.
        if (rPacketIt != eraseEnd) {
          auto nextElem = conn.outstandings.packets.erase(
              rPacketIt.base(), eraseEnd.base());
          rPacketIt = std::reverse_iterator<decltype(nextElem)>(nextElem) + 1;
          eraseEnd = rPacketIt;
        } else {
          rPacketIt++;
          eraseEnd = rPacketIt;
        }
        continue;
      }
      if (currentPacketNum < ackBlockIt->startPacket) {
        break;
      }
      VLOG(10) << __func__ << " acked packetNum=" << currentPacketNum
               << " space=" << currentPacketNumberSpace
               << " handshake=" << (int)rPacketIt->isHandshake << " " << conn;
      bool needsProcess = !rPacketIt->associatedEvent ||
          conn.outstandings.packetEvents.count(*rPacketIt->associatedEvent);
      if (rPacketIt->isHandshake && needsProcess) {
        if (currentPacketNumberSpace == PacketNumberSpace::Initial) {
          ++initialPacketAcked;
        } else {
          CHECK_EQ(PacketNumberSpace::Handshake, currentPacketNumberSpace);
          ++handshakePacketAcked;
        }
      }
      ack.ackedBytes += rPacketIt->encodedSize;
      if (rPacketIt->associatedEvent) {
        ++clonedPacketsAcked;
      }
      // Update RTT if current packet is the largestAcked in the frame:
      auto ackReceiveTimeOrNow =
          ackReceiveTime > rPacketIt->time ? ackReceiveTime : Clock::now();
      auto rttSample = std::chrono::duration_cast<std::chrono::microseconds>(
          ackReceiveTimeOrNow - rPacketIt->time);
      if (currentPacketNum == frame.largestAcked) {
        updateRtt(conn, rttSample, frame.ackDelay);
      }
      // Only invoke AckVisitor if the packet doesn't have an associated
      // PacketEvent; or the PacketEvent is in conn.outstandings.packetEvents
      if (needsProcess /*!rPacketIt->associatedEvent ||
          conn.outstandings.packetEvents.count(*rPacketIt->associatedEvent)*/) {
        for (auto& packetFrame : rPacketIt->packet.frames) {
          ackVisitor(*rPacketIt, packetFrame, frame);
        }
        // Remove this PacketEvent from the outstandings.packetEvents set
        if (rPacketIt->associatedEvent) {
          conn.outstandings.packetEvents.erase(*rPacketIt->associatedEvent);
        }
      }
      if (!ack.largestAckedPacket ||
          *ack.largestAckedPacket < currentPacketNum) {
        ack.largestAckedPacket = currentPacketNum;
        ack.largestAckedPacketSentTime = rPacketIt->time;
        ack.largestAckedPacketAppLimited = rPacketIt->isAppLimited;
      }
      if (ackReceiveTime > rPacketIt->time) {
        ack.mrttSample =
            std::min(ack.mrttSample.value_or(rttSample), rttSample);
      }
      conn.lossState.totalBytesAcked += rPacketIt->encodedSize;
      conn.lossState.totalBytesSentAtLastAck = conn.lossState.totalBytesSent;
      conn.lossState.totalBytesAckedAtLastAck = conn.lossState.totalBytesAcked;
      if (!lastAckedPacketSentTime) {
        lastAckedPacketSentTime = rPacketIt->time;
      }
      conn.lossState.lastAckedTime = ackReceiveTime;
      ack.ackedPackets.push_back(
          CongestionController::AckEvent::AckPacket::Builder()
              .setSentTime(rPacketIt->time)
              .setEncodedSize(rPacketIt->encodedSize)
              .setLastAckedPacketInfo(std::move(rPacketIt->lastAckedPacketInfo))
              .setTotalBytesSentThen(rPacketIt->totalBytesSent)
              .setAppLimited(rPacketIt->isAppLimited)
              .build());
      rPacketIt++;
    }
    // Done searching for acked outstanding packets in current ack block. Erase
    // the current iterator range which is the last batch of continuous
    // outstanding packets that are in this ack block. Move the iterator to be
    // the next search point.
    if (rPacketIt != eraseEnd) {
      auto nextElem =
          conn.outstandings.packets.erase(rPacketIt.base(), eraseEnd.base());
      currentPacketIt = std::reverse_iterator<decltype(nextElem)>(nextElem);
    } else {
      currentPacketIt = rPacketIt;
    }
    ackBlockIt++;
  }
  if (lastAckedPacketSentTime) {
    conn.lossState.lastAckedPacketSentTime = *lastAckedPacketSentTime;
  }
  CHECK_GE(conn.outstandings.initialPacketsCount, initialPacketAcked);
  conn.outstandings.initialPacketsCount -= initialPacketAcked;
  CHECK_GE(conn.outstandings.handshakePacketsCount, handshakePacketAcked);
  conn.outstandings.handshakePacketsCount -= handshakePacketAcked;
  CHECK_GE(conn.outstandings.clonedPacketsCount, clonedPacketsAcked);
  conn.outstandings.clonedPacketsCount -= clonedPacketsAcked;
  auto updatedOustandingPacketsCount = conn.outstandings.packets.size();
  CHECK_GE(
      updatedOustandingPacketsCount,
      conn.outstandings.handshakePacketsCount +
          conn.outstandings.initialPacketsCount);
  CHECK_GE(updatedOustandingPacketsCount, conn.outstandings.clonedPacketsCount);
  auto lossEvent = handleAckForLoss(conn, lossVisitor, ack, pnSpace);
  if (conn.congestionController &&
      (ack.largestAckedPacket.has_value() || lossEvent)) {
    if (lossEvent) {
      CHECK(lossEvent->largestLostSentTime && lossEvent->smallestLostSentTime);
      // TODO it's not clear that we should be using the smallest and largest
      // lost times here. It may perhaps be better to only consider the latest
      // contiguous lost block and determine if that block is larger than the
      // congestion period. Alternatively we could consider every lost block
      // and check if any of them constitute persistent congestion.
      lossEvent->persistentCongestion = isPersistentCongestion(
          conn,
          *lossEvent->smallestLostSentTime,
          *lossEvent->largestLostSentTime);
    }
    conn.congestionController->onPacketAckOrLoss(
        std::move(ack), std::move(lossEvent));
  }
}

void commonAckVisitorForAckFrame(
    AckState& ackState,
    const WriteAckFrame& frame) {
  // Remove ack interval from ackState if an outstandingPacket with a AckFrame
  // is acked.
  // We may remove the current largest acked packet here, but keep its receive
  // time behind. But then right after this updateLargestReceivedPacketNum will
  // update that time stamp. Please note that this assume the peer isn't buggy
  // in the sense that packet numbers it issues are only increasing.
  auto iter = frame.ackBlocks.crbegin();
  while (iter != frame.ackBlocks.crend()) {
    ackState.acks.withdraw(*iter);
    iter++;
  }
  if (!frame.ackBlocks.empty()) {
    auto largestAcked = frame.ackBlocks.front().end;
    if (largestAcked > kAckPurgingThresh) {
      ackState.acks.withdraw({0, largestAcked - kAckPurgingThresh});
    }
  }
}
} // namespace quic
