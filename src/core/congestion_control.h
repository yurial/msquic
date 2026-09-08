/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

--*/

#include "bandwidth_shaper.h"
#include "bbr.h"
#include "cubic.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct QUIC_ACK_EVENT {

    uint64_t TimeNow; // microsecond

    uint64_t LargestAck;

    uint64_t LargestSentPacketNumber;

    //
    // Number of retransmittable bytes acked during the connection's lifetime
    //
    uint64_t NumTotalAckedRetransmittableBytes;

    uint32_t NumRetransmittableBytes;

    QUIC_SENT_PACKET_METADATA* AckedPackets;

    //
    // Connection's current SmoothedRtt.
    //
    uint64_t SmoothedRtt;

    //
    // The smallest calculated RTT of the packets that were just ACKed.
    //
    uint64_t MinRtt;

    //
    // The smoothed one-way delay of the send path.
    //
    uint64_t OneWayDelay;

    //
    // Acked time minus ack delay.
    //
    uint64_t AdjustedAckTime;

    BOOLEAN IsImplicit : 1;

    BOOLEAN HasLoss : 1;

    BOOLEAN IsLargestAckedPacketAppLimited : 1;

    BOOLEAN MinRttValid : 1;

} QUIC_ACK_EVENT;

typedef struct QUIC_LOSS_EVENT {

    uint64_t LargestPacketNumberLost;

    uint64_t LargestSentPacketNumber;

    uint32_t NumRetransmittableBytes;

    BOOLEAN PersistentCongestion : 1;

} QUIC_LOSS_EVENT;

typedef struct QUIC_ECN_EVENT {

    uint64_t LargestPacketNumberAcked;

    uint64_t LargestSentPacketNumber;

} QUIC_ECN_EVENT;

typedef struct QUIC_CONGESTION_CONTROL {

    //
    // Name of congestion control algorithm
    //
    const char* Name;

    BOOLEAN (*QuicCongestionControlCanSend)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc
        );

    void (*QuicCongestionControlSetExemption)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc,
        _In_ uint8_t NumPackets
        );

    void (*QuicCongestionControlReset)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc,
        _In_ BOOLEAN FullReset
        );

    uint32_t (*QuicCongestionControlGetSendAllowance)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc,
        _In_ uint64_t TimeSinceLastSend,
        _In_ BOOLEAN TimeSinceLastSendValid
        );

    void (*QuicCongestionControlOnDataSent)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc,
        _In_ uint32_t NumRetransmittableBytes
        );

    BOOLEAN (*QuicCongestionControlOnDataInvalidated)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc,
        _In_ uint32_t NumRetransmittableBytes
        );

    BOOLEAN (*QuicCongestionControlOnDataAcknowledged)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc,
        _In_ const QUIC_ACK_EVENT* AckEvent
        );

    void (*QuicCongestionControlOnDataLost)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc,
        _In_ const QUIC_LOSS_EVENT* LossEvent
        );

    void (*QuicCongestionControlOnEcn)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc,
        _In_ const QUIC_ECN_EVENT* LossEvent
        );

    BOOLEAN (*QuicCongestionControlOnSpuriousCongestionEvent)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc
        );

    void (*QuicCongestionControlLogOutFlowStatus)(
        _In_ const struct QUIC_CONGESTION_CONTROL* Cc
        );

    uint8_t (*QuicCongestionControlGetExemptions)(
        _In_ const struct QUIC_CONGESTION_CONTROL* Cc
        );

    uint32_t (*QuicCongestionControlGetBytesInFlightMax)(
        _In_ const struct QUIC_CONGESTION_CONTROL* Cc
        );

    uint32_t (*QuicCongestionControlGetCongestionWindow)(
        _In_ const struct QUIC_CONGESTION_CONTROL* Cc
        );

    BOOLEAN (*QuicCongestionControlIsAppLimited)(
        _In_ const struct QUIC_CONGESTION_CONTROL* Cc
        );

    void (*QuicCongestionControlSetAppLimited)(
        _In_ struct QUIC_CONGESTION_CONTROL* Cc
        );

    void (*QuicCongestionControlGetNetworkStatistics)(
        _In_ const QUIC_CONNECTION* const Connection,
        _In_ const struct QUIC_CONGESTION_CONTROL* const Cc,
        _Out_ struct QUIC_NETWORK_STATISTICS* NetworkStatistics
        );

    //
    // Algorithm specific state.
    //
    union {
        QUIC_CONGESTION_CONTROL_CUBIC Cubic;
        QUIC_CONGESTION_CONTROL_BBR Bbr;
    };

    //
    // Embedded bandwidth shaper (pacer). Initialized unlimited with the
    // default (0, 0) pair by QuicCongestionControlInitialize; a plugin opts
    // in by configuring it via QuicBandwidthShaperSetConfig
    // (specs/bandwidth.md §17, §24). While BandwidthBitsPerSecond == 0 every
    // shaper operation is a cheap no-op, so existing congestion control
    // behavior is unchanged.
    //
    QUIC_BANDWIDTH_SHAPER Pacer;

} QUIC_CONGESTION_CONTROL;


//
// V1 supports careful resume on 1 path per remote endpoint
//
typedef struct QUIC_CONN_CAREFUL_RESUME_V1 {

    //
    // Path RTT parameters
    //
    uint64_t SmoothedRtt;
    uint64_t MinRtt;

    //
    // Remote endpoint and the Path RTT parameters help match the path during Careful Resume
    //
    QUIC_ADDR RemoteEndpoint;

    //
    // Future Expiration Time in Unix Epoch microsecond units
    //
    uint64_t Expiration;

    //
    // Congestion algorithm last used
    //
    QUIC_CONGESTION_CONTROL_ALGORITHM Algorithm;

    //
    // CWND size in bytes for Careful Resume
    //
    uint32_t CongestionWindow;

} QUIC_CONN_CAREFUL_RESUME_V1;

typedef struct QUIC_CONN_CAREFUL_RESUME_V1 QUIC_CONN_CAREFUL_RESUME_STATE;

//
// Initializes the algorithm specific congestion control algorithm.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicCongestionControlInitialize(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_SETTINGS_INTERNAL* Settings
    );

//
// Returns TRUE if more bytes can be sent on the network.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
BOOLEAN
QuicCongestionControlCanSend(
    _In_ QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->QuicCongestionControlCanSend(Cc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
void
QuicCongestionControlSetExemption(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint8_t NumPackets
    )
{
    Cc->QuicCongestionControlSetExemption(Cc, NumPackets);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
void
QuicCongestionControlReset(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ BOOLEAN FullReset
    )
{
    Cc->QuicCongestionControlReset(Cc, FullReset);
    //
    // Forget the shaper's credit (CreditBaseTimeNsec := 0) while keeping
    // its validated configuration, for both FullReset modes
    // (specs/bandwidth.md §17).
    //
    QuicBandwidthShaperReset(&Cc->Pacer);
}

//
// Returns the number of bytes that can be sent immediately.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint32_t
QuicCongestionControlGetSendAllowance(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint64_t TimeSinceLastSend, // microsec
    _In_ BOOLEAN TimeSinceLastSendValid
    )
{
    return Cc->QuicCongestionControlGetSendAllowance(Cc, TimeSinceLastSend, TimeSinceLastSendValid);
}

//
// Called when any retransmittable data is sent. NowUsec is the monotonic
// send time (microseconds) injected by the send-path caller, which already
// holds it; Mtu is the sending path's packet size per call (Path->Mtu at
// the loss-detection debit site; the shaper stores no MTU, §3.3 of
// specs/bandwidth.md). Neither the shaper nor the CC layer reads the
// system clock (specs/bandwidth.md §14).
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_INLINE
void
QuicCongestionControlOnDataSent(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint32_t NumRetransmittableBytes,
    _In_ uint64_t NowUsec, // microsec
    _In_ uint16_t Mtu
    )
{
    Cc->QuicCongestionControlOnDataSent(Cc, NumRetransmittableBytes);
    //
    // Debit the send from the embedded shaper's credit. A no-op while the
    // shaper is unlimited (BandwidthBitsPerSecond == 0) (§10, §14).
    //
    QuicBandwidthShaperRegisterSend(&Cc->Pacer, NumRetransmittableBytes, NowUsec, Mtu);
}

//
// Called when any data needs to be removed from inflight but cannot be
// considered lost or acknowledged.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
BOOLEAN
QuicCongestionControlOnDataInvalidated(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint32_t NumRetransmittableBytes
    )
{
    return Cc->QuicCongestionControlOnDataInvalidated(Cc, NumRetransmittableBytes);
}

//
// Called when any data is acknowledged.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
BOOLEAN
QuicCongestionControlOnDataAcknowledged(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_ACK_EVENT* AckEvent
    )
{
    return Cc->QuicCongestionControlOnDataAcknowledged(Cc, AckEvent);
}

//
// Called when data is determined lost.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
void
QuicCongestionControlOnDataLost(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_LOSS_EVENT* LossEvent
    )
{
    Cc->QuicCongestionControlOnDataLost(Cc, LossEvent);
}

//
// Called when congestion is signaled by ECN.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
void
QuicCongestionControlOnEcn(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_ECN_EVENT* EcnEvent
    )
{
    if (Cc->QuicCongestionControlOnEcn) {
        Cc->QuicCongestionControlOnEcn(Cc, EcnEvent);
    }
}

//
// Called when all recently considered lost data was actually acknowledged.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
BOOLEAN
QuicCongestionControlOnSpuriousCongestionEvent(
    _In_ QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->QuicCongestionControlOnSpuriousCongestionEvent(Cc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint8_t
QuicCongestionControlGetExemptions(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->QuicCongestionControlGetExemptions(Cc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
void
QuicCongestionControlLogOutFlowStatus(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    Cc->QuicCongestionControlLogOutFlowStatus(Cc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint32_t
QuicCongestionControlGetBytesInFlightMax(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->QuicCongestionControlGetBytesInFlightMax(Cc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint32_t
QuicCongestionControlGetCongestionWindow(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->QuicCongestionControlGetCongestionWindow(Cc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
BOOLEAN
QuicCongestionControlIsAppLimited(
    _In_ struct QUIC_CONGESTION_CONTROL* Cc
    )
{
    return Cc->QuicCongestionControlIsAppLimited(Cc);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
void
QuicCongestionControlSetAppLimited(
    _In_ struct QUIC_CONGESTION_CONTROL* Cc
    )
{
    Cc->QuicCongestionControlSetAppLimited(Cc);
}

#if defined(__cplusplus)
}
#endif
