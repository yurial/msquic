/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    The algorithm used for adjusting CongestionWindow is CUBIC (RFC8312bis).

--*/

#include "precomp.h"
#ifdef QUIC_CLOG
#include "cubic.c.clog.h"
#endif

#include "cubic.h"

//
// BETA and C from RFC8312. 10x multiples for integer arithmetic.
//
#define TEN_TIMES_BETA_CUBIC 7
#define TEN_TIMES_C_CUBIC 4

//
// Shifting nth root algorithm.
//
// This works sort of like long division: we look at the radicand in aligned
// chunks of 3 bits to compute each bit of the root. This is somewhat
// intuitive, since 2^3 = 8, i.e. one bit is needed to encode the cube root
// of a 3-bit number.
//
// At each step, we have a root value computed "so far" (i.e. the most
// significant bits of the root) and we need to find the correct value of
// the LSB of the (shifted) root so that it satisfies the two conditions:
// y^3 <= x
// (y+1)^3 > x
// ...where y represents the shifted value of the root "computed so far"
// and x represents the bits of the radicand "shifted in so far."
//
// The initial shift of 30 bits gives us 3-bit-aligned chunks.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
uint32_t
CubeRoot(
    uint32_t Radicand
    )
{
    int i;
    uint32_t x = 0;
    uint32_t y = 0;

    for (i = 30; i >= 0; i -= 3) {
        x = x * 8 + ((Radicand >> i) & 7);
        if ((y * 2 + 1) * (y * 2 + 1) * (y * 2 + 1) <= x) {
            y = y * 2 + 1;
        } else {
            y = y * 2;
        }
    }
    return y;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicConnLogCubic(
    _In_ const QUIC_CONNECTION* const Connection
    )
{
    const QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Connection->CongestionControl.Cubic;

    QuicTraceEvent(
        ConnCubic,
        "[conn][%p] CUBIC: SlowStartThreshold=%u K=%u WindowMax=%u WindowLastMax=%u",
        Connection,
        Cubic->SlowStartThreshold,
        Cubic->KCubic,
        Cubic->WindowMax,
        Cubic->WindowLastMax);
}

void
CubicCongestionHyStartChangeState(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ QUIC_CUBIC_HYSTART_STATE NewHyStartState
    )
{
    QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    if (!Connection->Settings.HyStartEnabled) {
        return;
    }
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;
    switch (NewHyStartState) {
    case HYSTART_ACTIVE:
        break;
    case HYSTART_DONE:
    case HYSTART_NOT_STARTED:
        Cubic->CWndSlowStartGrowthDivisor = 1;
        break;
    default:
        CXPLAT_FRE_ASSERT(FALSE);
    }

    if (Cubic->HyStartState != NewHyStartState) {
        QuicTraceEvent(
            ConnHyStartStateChange,
            "[conn][%p] HyStart: State=%u CongestionWindow=%u SlowStartThreshold=%u",
            Connection,
            NewHyStartState,
            Cubic->CongestionWindow,
            Cubic->SlowStartThreshold);

        Cubic->HyStartState = NewHyStartState;
    }
}

void
CubicCongestionHyStartResetPerRttRound(
    _In_ QUIC_CONGESTION_CONTROL_CUBIC* Cubic
    )
{
    Cubic->HyStartAckCount = 0;
    Cubic->MinRttInLastRound = Cubic->MinRttInCurrentRound;
    Cubic->MinRttInCurrentRound = UINT64_MAX;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CubicCongestionControlCanSend(
    _In_ QUIC_CONGESTION_CONTROL* Cc
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;
    return Cubic->BytesInFlight < Cubic->CongestionWindow || Cubic->Exemptions > 0;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CubicCongestionControlSetExemption(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint8_t NumPackets
    )
{
    Cc->Cubic.Exemptions = NumPackets;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CubicCongestionControlReset(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ BOOLEAN FullReset
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    const uint16_t DatagramPayloadLength =
        QuicPathGetDatagramPayloadSize(&Connection->Paths[0]);
    Cubic->SlowStartThreshold = UINT32_MAX;
    Cubic->MinRttInCurrentRound = UINT32_MAX;
    Cubic->HyStartRoundEnd = Connection->Send.NextPacketNumber;
    CubicCongestionHyStartResetPerRttRound(Cubic);
    CubicCongestionHyStartChangeState(Cc, HYSTART_NOT_STARTED);
    Cubic->IsInRecovery = FALSE;
    Cubic->HasHadCongestionEvent = FALSE;
    Cubic->CongestionWindow = DatagramPayloadLength * Cubic->InitialWindowPackets;
    Cubic->BytesInFlightMax = Cubic->CongestionWindow / 2;
    if (FullReset) {
        Cubic->BytesInFlight = 0;
    }

    QuicConnLogOutFlowStats(Connection);
    QuicConnLogCubic(Connection);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
uint32_t
CubicCongestionControlGetSendAllowance(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint64_t TimeSinceLastSend, // microsec
    _In_ BOOLEAN TimeSinceLastSendValid
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    if (Cubic->BytesInFlight >= Cubic->CongestionWindow) {
        //
        // We are CC blocked, so we can't send anything.
        //
        return 0;
    }

    if (!TimeSinceLastSendValid ||
        !Connection->Settings.PacingEnabled ||
        !Connection->Paths[0].GotFirstRttSample ||
        Connection->Paths[0].SmoothedRtt < QUIC_MIN_PACING_RTT) {
        //
        // We're not in the necessary state to pace.
        //
        return Cubic->CongestionWindow - Cubic->BytesInFlight;
    }

    //
    // We are pacing, so the embedded bandwidth shaper (Cc->Pacer) decides
    // how much of the CC window room may go out right now: the pacing rate
    // is (re)configured on every call from the current window estimate
    // (specs/bandwidth.md §27.1) and the allowance is the shaper's credit
    // clamped by the CC window room
    // (QuicBandwidthShaperComputeSendAllowance, §13).
    //

    //
    // Since the window grows via ACK feedback and since we defer packets
    // when pacing, using the current window to calculate the pacing
    // interval can slow the growth of the window. So instead, use the
    // predicted window of the next round trip. In slowstart, this is double
    // the current window. In congestion avoidance the growth function is
    // more complicated, and we use a simple estimate of 25% growth.
    //
    uint64_t EstimatedWnd;
    if (Cubic->CongestionWindow < Cubic->SlowStartThreshold) {
        EstimatedWnd = (uint64_t)Cubic->CongestionWindow << 1;
        if (EstimatedWnd > Cubic->SlowStartThreshold) {
            EstimatedWnd = Cubic->SlowStartThreshold;
        }
    } else {
        EstimatedWnd = Cubic->CongestionWindow + (Cubic->CongestionWindow >> 2); // CongestionWindow * 1.25
    }

    //
    // Pacing rate in bits per second: the estimated window (bytes) divided
    // by the smoothed RTT (microseconds), converted to bits/second (§27.1).
    // EstimatedWnd * BITS_PER_BYTE * USEC_PER_SEC < 2^56, no overflow.
    //
    uint64_t PacingRateBitsPerSec =
        EstimatedWnd * BITS_PER_BYTE * QUIC_BANDWIDTH_SHAPER_USEC_PER_SEC /
        Connection->Paths[0].SmoothedRtt;

    //
    // The caller's current monotonic moment (§2.1: time is injected, never
    // read here). The send path (packet_builder.c) computed
    // TimeSinceLastSend as CxPlatTimeDiff64(Send.LastFlushTime, TimeNow)
    // from the LastFlushTime value that is still current inside this call
    // (the send path updates it only after this call returns), and
    // TimeSinceLastSendValid is guaranteed TRUE on this branch, so the
    // current moment reconstructs exactly.
    //
    uint64_t NowUsec = Connection->Send.LastFlushTime + TimeSinceLastSend;

    //
    // The burst window is set once, together with the first non-zero rate
    // (§17/§25), and is never changed by the plugin afterwards: the current
    // window is passed through on every subsequent configuration. The
    // shaper stores the window verbatim in microseconds (as configured,
    // §3.6), so the read-back is a plain field access.
    //
    uint64_t BurstWindowUsec = Cc->Pacer.BurstWindowUsec;
    if (BurstWindowUsec == 0) {
        BurstWindowUsec = QUIC_DEFAULT_PACING_BURST_WINDOW_USEC;
    }

    //
    // Per §25 the return value is not consumed: on rejection (e.g. the
    // window invariant cannot be verified yet, or a degenerate zero rate)
    // the shaper keeps its previous valid configuration, and the allowance
    // below degrades to that configuration — unlimited (i.e. the old
    // un-paced room) if the shaper was never configured.
    //
    QuicBandwidthShaperSetConfig(
        &Cc->Pacer, PacingRateBitsPerSec, BurstWindowUsec, NowUsec);

    //
    // The path's packet size is passed per call (§3.3): the embedded
    // shaper stores no MTU. Paths[0].Mtu (like Paths[0].SmoothedRtt above)
    // diverges from the sending path's Mtu that loss detection debits with,
    // but only for a strict-mode CC pacer (< ~5-6 Mbit/s) during path
    // migration with differing MTUs.
    //
    return QuicBandwidthShaperComputeSendAllowance(
        &Cc->Pacer,
        NowUsec,
        Cubic->CongestionWindow,
        Cubic->BytesInFlight,
        Connection->Paths[0].Mtu);
}

//
// Returns TRUE if we became unblocked.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CubicCongestionControlUpdateBlockedState(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ BOOLEAN PreviousCanSendState
    )
{
    QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    QuicConnLogOutFlowStats(Connection);
    if (PreviousCanSendState != CubicCongestionControlCanSend(Cc)) {
        if (PreviousCanSendState) {
            QuicConnAddOutFlowBlockedReason(
                Connection, QUIC_FLOW_BLOCKED_CONGESTION_CONTROL);
        } else {
            QuicConnRemoveOutFlowBlockedReason(
                Connection, QUIC_FLOW_BLOCKED_CONGESTION_CONTROL);
            Connection->Send.LastFlushTime = CxPlatTimeUs64(); // Reset last flush time
            return TRUE;
        }
    }
    return FALSE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CubicCongestionControlOnCongestionEvent(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ BOOLEAN IsPersistentCongestion,
    _In_ BOOLEAN Ecn
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    const uint16_t DatagramPayloadLength =
        QuicPathGetDatagramPayloadSize(&Connection->Paths[0]);
    QuicTraceEvent(
        ConnCongestionV2,
        "[conn][%p] Congestion event: IsEcn=%hu",
        Connection,
        Ecn);
    Connection->Stats.Send.CongestionCount++;

    Cubic->IsInRecovery = TRUE;
    Cubic->HasHadCongestionEvent = TRUE;

    //
    // If the congestion event is not triggered by ECN, save previous state,
    // just in case this ends up being spurious.
    //
    if (!Ecn) {
        Cubic->PrevWindowPrior = Cubic->WindowPrior;
        Cubic->PrevWindowMax = Cubic->WindowMax;
        Cubic->PrevWindowLastMax = Cubic->WindowLastMax;
        Cubic->PrevKCubic = Cubic->KCubic;
        Cubic->PrevSlowStartThreshold = Cubic->SlowStartThreshold;
        Cubic->PrevCongestionWindow = Cubic->CongestionWindow;
        Cubic->PrevAimdWindow = Cubic->AimdWindow;
    }

    if (IsPersistentCongestion && !Cubic->IsInPersistentCongestion) {

        CXPLAT_DBG_ASSERT(!Cubic->IsInPersistentCongestion);
        QuicTraceEvent(
            ConnPersistentCongestion,
            "[conn][%p] Persistent congestion event",
            Connection);
        Connection->Stats.Send.PersistentCongestionCount++;

        Connection->Paths[0].Route.State = RouteSuspected; // used only for RAW datapath

        Cubic->IsInPersistentCongestion = TRUE;
        Cubic->WindowPrior =
        Cubic->WindowMax =
        Cubic->WindowLastMax =
        Cubic->SlowStartThreshold =
        Cubic->AimdWindow =
            Cubic->CongestionWindow * TEN_TIMES_BETA_CUBIC / 10;
        Cubic->CongestionWindow =
            DatagramPayloadLength * QUIC_PERSISTENT_CONGESTION_WINDOW_PACKETS;
        Cubic->KCubic = 0;
        CubicCongestionHyStartChangeState(Cc, HYSTART_DONE);

    } else {

        Cubic->WindowPrior =
        Cubic->WindowMax =
            Cubic->CongestionWindow;
        if (Cubic->WindowLastMax > Cubic->WindowMax) {
            //
            // Fast convergence.
            //
            Cubic->WindowLastMax = Cubic->WindowMax;
            Cubic->WindowMax = Cubic->WindowMax * (10 + TEN_TIMES_BETA_CUBIC) / 20;
        } else {
            Cubic->WindowLastMax = Cubic->WindowMax;
        }

        //
        // K = (WindowMax * (1 - BETA) / C) ^ (1/3)
        // BETA := multiplicative window decrease factor.
        //
        // Here we reduce rounding error by left-shifting the CubeRoot argument
        // by 9 before the division and then right-shifting the result by 3
        // (since 2^9 = 2^3^3).
        //
        Cubic->KCubic =
            CubeRoot(
                (Cubic->WindowMax / DatagramPayloadLength * (10 - TEN_TIMES_BETA_CUBIC) << 9) /
                TEN_TIMES_C_CUBIC);
        Cubic->KCubic = S_TO_MS(Cubic->KCubic);
        Cubic->KCubic >>= 3;

        CubicCongestionHyStartChangeState(Cc, HYSTART_DONE);
        Cubic->SlowStartThreshold =
        Cubic->CongestionWindow =
        Cubic->AimdWindow =
            CXPLAT_MAX(
                (uint32_t)DatagramPayloadLength * QUIC_PERSISTENT_CONGESTION_WINDOW_PACKETS,
                Cubic->CongestionWindow * TEN_TIMES_BETA_CUBIC / 10);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
CubicCongestionControlOnDataSent(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint32_t NumRetransmittableBytes
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    BOOLEAN PreviousCanSendState = QuicCongestionControlCanSend(Cc);

    Cubic->BytesInFlight += NumRetransmittableBytes;
    if (Cubic->BytesInFlightMax < Cubic->BytesInFlight) {
        Cubic->BytesInFlightMax = Cubic->BytesInFlight;
        QuicSendBufferConnectionAdjust(QuicCongestionControlGetConnection(Cc));
    }

    if (Cubic->Exemptions > 0) {
        --Cubic->Exemptions;
    }

    CubicCongestionControlUpdateBlockedState(Cc, PreviousCanSendState);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CubicCongestionControlOnDataInvalidated(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint32_t NumRetransmittableBytes
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    BOOLEAN PreviousCanSendState = CubicCongestionControlCanSend(Cc);

    CXPLAT_DBG_ASSERT(Cubic->BytesInFlight >= NumRetransmittableBytes);
    Cubic->BytesInFlight -= NumRetransmittableBytes;

    return CubicCongestionControlUpdateBlockedState(Cc, PreviousCanSendState);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CubicCongestionControlGetNetworkStatistics(
    _In_ const QUIC_CONNECTION* const Connection,
    _In_ const QUIC_CONGESTION_CONTROL* const Cc,
    _Out_ QUIC_NETWORK_STATISTICS* NetworkStatistics
    )
{
    const QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;
    const QUIC_PATH* Path = &Connection->Paths[0];

    NetworkStatistics->BytesInFlight = Cubic->BytesInFlight;
    NetworkStatistics->PostedBytes = Connection->SendBuffer.PostedBytes;
    NetworkStatistics->IdealBytes = Connection->SendBuffer.IdealBytes;
    NetworkStatistics->SmoothedRTT = Path->SmoothedRtt;
    NetworkStatistics->CongestionWindow = Cubic->CongestionWindow;
    NetworkStatistics->Bandwidth = Path->SmoothedRtt == 0 ? 0 : Cubic->CongestionWindow / Path->SmoothedRtt;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CubicCongestionControlOnDataAcknowledged(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_ACK_EVENT* AckEvent
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    const uint64_t TimeNowUs = AckEvent->TimeNow;
    QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    BOOLEAN PreviousCanSendState = CubicCongestionControlCanSend(Cc);
    uint32_t BytesAcked = AckEvent->NumRetransmittableBytes;

    CXPLAT_DBG_ASSERT(Cubic->BytesInFlight >= BytesAcked);
    Cubic->BytesInFlight -= BytesAcked;

    if (Cubic->IsInRecovery) {
        if (AckEvent->LargestAck > Cubic->RecoverySentPacketNumber) {
            //
            // Done recovering. Note that completion of recovery is defined a
            // bit differently here than in TCP: we simply require an ACK for a
            // packet sent after recovery started.
            //
            QuicTraceEvent(
                ConnRecoveryExit,
                "[conn][%p] Recovery complete",
                Connection);
            Cubic->IsInRecovery = FALSE;
            Cubic->IsInPersistentCongestion = FALSE;
            Cubic->TimeOfCongAvoidStart = TimeNowUs;
        }
        goto Exit;
    } else if (BytesAcked == 0) {
        goto Exit;
    }

    //
    // Update HyStart++ RTT sample.
    //
    if (Connection->Settings.HyStartEnabled && Cubic->HyStartState != HYSTART_DONE) {
        if (AckEvent->MinRttValid) {
            //
            // Update Min RTT for the first N ACKs.
            //
            if (Cubic->HyStartAckCount < QUIC_HYSTART_DEFAULT_N_SAMPLING) {
                Cubic->MinRttInCurrentRound =
                    CXPLAT_MIN(
                        Cubic->MinRttInCurrentRound,
                        AckEvent->MinRtt);
                Cubic->HyStartAckCount++;
            } else if (Cubic->HyStartState == HYSTART_NOT_STARTED) {
                const uint64_t Eta =
                    CXPLAT_MIN(
                        QUIC_HYSTART_DEFAULT_MAX_ETA,
                        CXPLAT_MAX(
                            QUIC_HYSTART_DEFAULT_MIN_ETA,
                            Cubic->MinRttInLastRound / 8)); // Use 1/8th RTT from HyStart spec.
                //
                // Looking for delay increase.
                //
                if (Cubic->MinRttInLastRound != UINT64_MAX &&
                    Cubic->MinRttInCurrentRound != UINT64_MAX &&
                    (Cubic->MinRttInCurrentRound >= Cubic->MinRttInLastRound + Eta)) {
                    //
                    // Exit Slow Start. Now we are going to do Conservative Slow Start for
                    // ConservativeSlowStartRounds rounds.
                    //
                    CubicCongestionHyStartChangeState(Cc, HYSTART_ACTIVE);
                    Cubic->CWndSlowStartGrowthDivisor =
                        QUIC_CONSERVATIVE_SLOW_START_DEFAULT_GROWTH_DIVISOR;
                    Cubic->ConservativeSlowStartRounds =
                        QUIC_CONSERVATIVE_SLOW_START_DEFAULT_ROUNDS;
                    Cubic->CssBaselineMinRtt = Cubic->MinRttInCurrentRound;
                }
            } else {
                //
                // RTT decreased. Resume SlowStart since we assume the SlowStart exit was spurious.
                //
                if (Cubic->MinRttInCurrentRound < Cubic->CssBaselineMinRtt) {
                    CubicCongestionHyStartChangeState(Cc, HYSTART_NOT_STARTED);
                }
            }
        }

        //
        // Reset HyStart parameters for each RTT round.
        //
        if (AckEvent->LargestAck >= Cubic->HyStartRoundEnd) {
            Cubic->HyStartRoundEnd = Connection->Send.NextPacketNumber;
            if (Cubic->HyStartState == HYSTART_ACTIVE) {
                if (--Cubic->ConservativeSlowStartRounds == 0) {
                    //
                    // Exit Conservative Slow Start and enter Congestion Avoidance now.
                    //
                    Cubic->SlowStartThreshold = Cubic->CongestionWindow;
                    Cubic->TimeOfCongAvoidStart = TimeNowUs;
                    Cubic->AimdWindow = Cubic->CongestionWindow;
                    CubicCongestionHyStartChangeState(Cc, HYSTART_DONE);
                }
            }
            CubicCongestionHyStartResetPerRttRound(Cubic);
        }
    }

    if (Cubic->CongestionWindow < Cubic->SlowStartThreshold) {

        //
        // Slow Start
        //

        Cubic->CongestionWindow += (BytesAcked / Cubic->CWndSlowStartGrowthDivisor);
        BytesAcked = 0;
        if (Cubic->CongestionWindow >= Cubic->SlowStartThreshold) {
            Cubic->TimeOfCongAvoidStart = TimeNowUs;

            //
            // We only want exponential growth up to SlowStartThreshold. If
            // CongestionWindow has increased beyond SlowStartThreshold, set it back
            // to SlowStartThreshold and treat the spare BytesAcked as if the bytes
            // were acknowledged during Congestion Avoidance below.
            //
            BytesAcked = Cubic->CongestionWindow - Cubic->SlowStartThreshold;
            Cubic->CongestionWindow = Cubic->SlowStartThreshold;
        }
    }

    if (BytesAcked > 0) {

        //
        // Congestion Avoidance
        //

        CXPLAT_DBG_ASSERT(Cubic->CongestionWindow >= Cubic->SlowStartThreshold);

        const uint16_t DatagramPayloadLength =
            QuicPathGetDatagramPayloadSize(&Connection->Paths[0]);

        //
        // We require steady ACK feedback to justify window growth. If there is
        // a long time gap between ACKs, add the gap to TimeOfCongAvoidStart to
        // reduce the value of TimeInCongAvoid, which effectively freezes window
        // growth during the gap.
        //
        if (Cubic->TimeOfLastAckValid) {
            const uint64_t TimeSinceLastAck = CxPlatTimeDiff64(Cubic->TimeOfLastAck, TimeNowUs);
            if (TimeSinceLastAck > MS_TO_US((uint64_t)Cubic->SendIdleTimeoutMs) &&
                TimeSinceLastAck > (Connection->Paths[0].SmoothedRtt + 4 * Connection->Paths[0].RttVariance)) {
                Cubic->TimeOfCongAvoidStart += TimeSinceLastAck;
                if (CxPlatTimeAtOrBefore64(TimeNowUs, Cubic->TimeOfCongAvoidStart)) {
                    Cubic->TimeOfCongAvoidStart = TimeNowUs;
                }
            }
        }

        const uint64_t TimeInCongAvoidUs =
            CxPlatTimeDiff64(Cubic->TimeOfCongAvoidStart, TimeNowUs);

        //
        // Compute the cubic window:
        // W_cubic(t) = C*(t-K)^3 + WindowMax.
        // (t in seconds; window sizes in MSS)
        //
        // NB: The RFC uses W_cubic(t+RTT) rather than W_cubic(t), so we
        // add RTT to DeltaT.
        //
        // Here we have 30 bits' worth of right shift. This is to convert
        // millisec^3 to sec^3. Each ten bit's worth of shift approximates
        // a division by 1000. The order of operations is chosen to strike
        // a balance between rounding error and overflow protection.
        // With C = 0.4 and MTU=0xffff, we are safe from overflow for
        // DeltaT < ~2.5M (about 30min).
        //

        int64_t DeltaT =
            US_TO_MS(
                (int64_t)TimeInCongAvoidUs -
                (int64_t)MS_TO_US(Cubic->KCubic) +
                (int64_t)AckEvent->SmoothedRtt
            );
        if (DeltaT > 2500000) {
            DeltaT = 2500000;
        }

        int64_t CubicWindow =
            ((((DeltaT * DeltaT) >> 10) * DeltaT *
             (int64_t)(DatagramPayloadLength * TEN_TIMES_C_CUBIC / 10)) >> 20) +
            (int64_t)Cubic->WindowMax;

        if (CubicWindow < 0) {
            //
            // The window came out so large it overflowed. We want to limit the
            // huge window below anyway, so just set it to the limiting value.
            //
            CubicWindow = 2 * Cubic->BytesInFlightMax;
        }

        //
        // Update the AIMD window. This window is designed to have the same average
        // size as an AIMD window with BETA=0.5 and a slope (AKA ALPHA) of 1MSS/RTT. Since
        // CUBIC has BETA=0.7, we need a smaller slope than 1MSS/RTT to have this property.
        // The required slope is derived in RFC 8312 to be [3*(1-BETA)/(1+BETA)].
        // For BETA=0.7, [3*(1-BETA)/(1+BETA)] ~= 0.5.
        //
        // This slope of 0.5MSS/RTT is used until AimdWindow reaches WindowPrior, and then
        // the slope is increased to 1MSS/RTT to match the aggressiveness of Reno.
        //
        // Algorithm adapted from RFC3465 (Appropriate Byte Counting). The idea here is to grow only by
        // multiples of MTU: we record ACKed bytes in an accumulator until at least a window
        // (or two windows, if AimdWindow < WindowPrior) worth of bytes are ACKed, and then increase
        // the window by 1 MTU.
        //
        CXPLAT_STATIC_ASSERT(TEN_TIMES_BETA_CUBIC == 7, "TEN_TIMES_BETA_CUBIC must be 7 for simplified calculation.");
        if (Cubic->AimdWindow < Cubic->WindowPrior) {
            Cubic->AimdAccumulator += BytesAcked / 2;
        } else {
            Cubic->AimdAccumulator += BytesAcked;
        }
        if (Cubic->AimdAccumulator > Cubic->AimdWindow) {
            Cubic->AimdWindow += DatagramPayloadLength;
            Cubic->AimdAccumulator -= Cubic->AimdWindow;
        }

        if (Cubic->AimdWindow > CubicWindow) {
            //
            // Reno-Friendly region.
            //
            Cubic->CongestionWindow = Cubic->AimdWindow;
        } else {
            //
            // Concave or Convex region. Constrain TargetWindow within [CongestionWindow, 1.5*CongestionWindow].
            //
            uint64_t TargetWindow = CXPLAT_MAX(Cubic->CongestionWindow, CXPLAT_MIN(CubicWindow, Cubic->CongestionWindow + (Cubic->CongestionWindow >> 1)));
            Cubic->CongestionWindow += (uint32_t)(((TargetWindow - Cubic->CongestionWindow) * DatagramPayloadLength) / Cubic->CongestionWindow);
        }
    }

    //
    // Limit the growth of the window based on the number of bytes we
    // actually manage to put on the wire, which may be limited by flow
    // control or by the app posting a limited number of bytes. This must
    // be done to prevent the window from growing without loss feedback from
    // the network.
    //
    // Using 2 * BytesInFlightMax for the limit allows for exponential growth
    // in the window when not otherwise limited.
    //
    if (Cubic->CongestionWindow > 2 * Cubic->BytesInFlightMax) {
        Cubic->CongestionWindow = 2 * Cubic->BytesInFlightMax;
    }

Exit:

    Cubic->TimeOfLastAck = TimeNowUs;
    Cubic->TimeOfLastAckValid = TRUE;

    if (Connection->Settings.NetStatsEventEnabled) {
        const QUIC_PATH* Path = &Connection->Paths[0];
        QUIC_CONNECTION_EVENT Event;
        Event.Type = QUIC_CONNECTION_EVENT_NETWORK_STATISTICS;
        Event.NETWORK_STATISTICS.BytesInFlight = Cubic->BytesInFlight;
        Event.NETWORK_STATISTICS.PostedBytes = Connection->SendBuffer.PostedBytes;
        Event.NETWORK_STATISTICS.IdealBytes = Connection->SendBuffer.IdealBytes;
        Event.NETWORK_STATISTICS.SmoothedRTT = Path->SmoothedRtt;
        Event.NETWORK_STATISTICS.CongestionWindow = Cubic->CongestionWindow;
        Event.NETWORK_STATISTICS.Bandwidth = Path->SmoothedRtt == 0 ? 0 : Cubic->CongestionWindow / Path->SmoothedRtt;

        QuicTraceLogConnVerbose(
           IndicateDataAcked,
           Connection,
           "Indicating QUIC_CONNECTION_EVENT_NETWORK_STATISTICS [BytesInFlight=%u,PostedBytes=%llu,IdealBytes=%llu,SmoothedRTT=%llu,CongestionWindow=%u,Bandwidth=%llu]",
           Event.NETWORK_STATISTICS.BytesInFlight,
           Event.NETWORK_STATISTICS.PostedBytes,
           Event.NETWORK_STATISTICS.IdealBytes,
           Event.NETWORK_STATISTICS.SmoothedRTT,
           Event.NETWORK_STATISTICS.CongestionWindow,
           Event.NETWORK_STATISTICS.Bandwidth);
       QuicConnIndicateEvent(Connection, &Event);
    }

    return CubicCongestionControlUpdateBlockedState(Cc, PreviousCanSendState);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CubicCongestionControlOnDataLost(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_LOSS_EVENT* LossEvent
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    BOOLEAN PreviousCanSendState = CubicCongestionControlCanSend(Cc);

    //
    // If data is lost after the most recent congestion event (or if there
    // hasn't been a congestion event yet) then treat this loss as a new
    // congestion event.
    //
    if (!Cubic->HasHadCongestionEvent ||
        LossEvent->LargestPacketNumberLost > Cubic->RecoverySentPacketNumber) {

        Cubic->RecoverySentPacketNumber = LossEvent->LargestSentPacketNumber;
        CubicCongestionControlOnCongestionEvent(
            Cc,
            LossEvent->PersistentCongestion,
            FALSE);

        CubicCongestionHyStartChangeState(Cc, HYSTART_DONE);
    }

    CXPLAT_DBG_ASSERT(Cubic->BytesInFlight >= LossEvent->NumRetransmittableBytes);
    Cubic->BytesInFlight -= LossEvent->NumRetransmittableBytes;

    CubicCongestionControlUpdateBlockedState(Cc, PreviousCanSendState);
    QuicConnLogCubic(QuicCongestionControlGetConnection(Cc));
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CubicCongestionControlOnEcn(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_ECN_EVENT* EcnEvent
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    BOOLEAN PreviousCanSendState = CubicCongestionControlCanSend(Cc);

    //
    // If the ECN signal is received after the most recent congestion event
    // (or if there hasn't been a congestion event yet) then treat it as a
    // new congestion event.
    //
    if (!Cubic->HasHadCongestionEvent ||
        EcnEvent->LargestPacketNumberAcked > Cubic->RecoverySentPacketNumber) {

        Cubic->RecoverySentPacketNumber = EcnEvent->LargestSentPacketNumber;
        QuicCongestionControlGetConnection(Cc)->Stats.Send.EcnCongestionCount++;
        CubicCongestionControlOnCongestionEvent(
            Cc,
            FALSE,
            TRUE);
        CubicCongestionHyStartChangeState(Cc, HYSTART_DONE);
    }

    CubicCongestionControlUpdateBlockedState(Cc, PreviousCanSendState);
    QuicConnLogCubic(QuicCongestionControlGetConnection(Cc));
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CubicCongestionControlOnSpuriousCongestionEvent(
    _In_ QUIC_CONGESTION_CONTROL* Cc
    )
{
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    if (!Cubic->IsInRecovery) {
        return FALSE;
    }

    QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    BOOLEAN PreviousCanSendState = QuicCongestionControlCanSend(Cc);

    QuicTraceEvent(
        ConnSpuriousCongestion,
        "[conn][%p] Spurious congestion event",
        Connection);

    //
    // Revert to previous state.
    //
    Cubic->WindowPrior = Cubic->PrevWindowPrior;
    Cubic->WindowMax = Cubic->PrevWindowMax;
    Cubic->WindowLastMax = Cubic->PrevWindowLastMax;
    Cubic->KCubic = Cubic->PrevKCubic;
    Cubic->SlowStartThreshold = Cubic->PrevSlowStartThreshold;
    Cubic->CongestionWindow = Cubic->PrevCongestionWindow;
    Cubic->AimdWindow = Cubic->PrevAimdWindow;

    Cubic->IsInRecovery = FALSE;
    Cubic->HasHadCongestionEvent = FALSE;

    BOOLEAN Result = CubicCongestionControlUpdateBlockedState(Cc, PreviousCanSendState);
    QuicConnLogCubic(Connection);
    return Result;
}

void
CubicCongestionControlLogOutFlowStatus(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    const QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    const QUIC_PATH* Path = &Connection->Paths[0];
    const QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    QuicTraceEvent(
        ConnOutFlowStatsV2,
        "[conn][%p] OUT: BytesSent=%llu InFlight=%u CWnd=%u ConnFC=%llu ISB=%llu PostedBytes=%llu SRtt=%llu 1Way=%llu",
        Connection,
        Connection->Stats.Send.TotalBytes,
        Cubic->BytesInFlight,
        Cubic->CongestionWindow,
        Connection->Send.PeerMaxData - Connection->Send.OrderedStreamBytesSent,
        Connection->SendBuffer.IdealBytes,
        Connection->SendBuffer.PostedBytes,
        Path->GotFirstRttSample ? Path->SmoothedRtt : 0,
        Path->OneWayDelay);
}

uint32_t
CubicCongestionControlGetBytesInFlightMax(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->Cubic.BytesInFlightMax;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
uint8_t
CubicCongestionControlGetExemptions(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->Cubic.Exemptions;
}

uint32_t
CubicCongestionControlGetCongestionWindow(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->Cubic.CongestionWindow;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CubicCongestionControlIsAppLimited(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    UNREFERENCED_PARAMETER(Cc);
    return FALSE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CubicCongestionControlSetAppLimited(
    _In_ struct QUIC_CONGESTION_CONTROL* Cc
    )
{
    UNREFERENCED_PARAMETER(Cc);
}

static const QUIC_CONGESTION_CONTROL QuicCongestionControlCubic = {
    .Name = "Cubic",
    .QuicCongestionControlCanSend = CubicCongestionControlCanSend,
    .QuicCongestionControlSetExemption = CubicCongestionControlSetExemption,
    .QuicCongestionControlReset = CubicCongestionControlReset,
    .QuicCongestionControlGetSendAllowance = CubicCongestionControlGetSendAllowance,
    .QuicCongestionControlOnDataSent = CubicCongestionControlOnDataSent,
    .QuicCongestionControlOnDataInvalidated = CubicCongestionControlOnDataInvalidated,
    .QuicCongestionControlOnDataAcknowledged = CubicCongestionControlOnDataAcknowledged,
    .QuicCongestionControlOnDataLost = CubicCongestionControlOnDataLost,
    .QuicCongestionControlOnEcn = CubicCongestionControlOnEcn,
    .QuicCongestionControlOnSpuriousCongestionEvent = CubicCongestionControlOnSpuriousCongestionEvent,
    .QuicCongestionControlLogOutFlowStatus = CubicCongestionControlLogOutFlowStatus,
    .QuicCongestionControlGetExemptions = CubicCongestionControlGetExemptions,
    .QuicCongestionControlGetBytesInFlightMax = CubicCongestionControlGetBytesInFlightMax,
    .QuicCongestionControlIsAppLimited = CubicCongestionControlIsAppLimited,
    .QuicCongestionControlSetAppLimited = CubicCongestionControlSetAppLimited,
    .QuicCongestionControlGetCongestionWindow = CubicCongestionControlGetCongestionWindow,
    .QuicCongestionControlGetNetworkStatistics = CubicCongestionControlGetNetworkStatistics
};

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CubicCongestionControlInitialize(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_SETTINGS_INTERNAL* Settings
    )
{
    *Cc = QuicCongestionControlCubic;

    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &Cc->Cubic;

    QUIC_CONNECTION* Connection = QuicCongestionControlGetConnection(Cc);
    const uint16_t DatagramPayloadLength =
        QuicPathGetDatagramPayloadSize(&Connection->Paths[0]);
    Cubic->SlowStartThreshold = UINT32_MAX;
    Cubic->SendIdleTimeoutMs = Settings->SendIdleTimeoutMs;
    Cubic->InitialWindowPackets = Settings->InitialWindowPackets;
    Cubic->CongestionWindow = DatagramPayloadLength * Cubic->InitialWindowPackets;
    Cubic->BytesInFlightMax = Cubic->CongestionWindow / 2;
    Cubic->MinRttInCurrentRound = UINT64_MAX;
    Cubic->HyStartRoundEnd = Connection->Send.NextPacketNumber;
    Cubic->HyStartState = HYSTART_NOT_STARTED;
    Cubic->CWndSlowStartGrowthDivisor = 1;
    CubicCongestionHyStartResetPerRttRound(Cubic);

    QuicConnLogOutFlowStats(Connection);
    QuicConnLogCubic(Connection);
}
