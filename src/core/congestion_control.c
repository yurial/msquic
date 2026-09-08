/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Algorithm for using (but not exceeding) available network bandwidth.

    The send rate is limited to the available bandwidth by
    limiting the number of bytes in flight to CongestionWindow.

--*/

#include "precomp.h"
#ifdef QUIC_CLOG
#include "congestion_control.c.clog.h"
#endif

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicCongestionControlInitialize(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_SETTINGS_INTERNAL* Settings
    )
{
    CXPLAT_DBG_ASSERT(Settings->CongestionControlAlgorithm < QUIC_CONGESTION_CONTROL_ALGORITHM_MAX);

    switch (Settings->CongestionControlAlgorithm) {
    default:
        QuicTraceLogConnWarning(
            InvalidCongestionControlAlgorithm,
            QuicCongestionControlGetConnection(Cc),
            "Unknown congestion control algorithm: %hu, fallback to Cubic",
            Settings->CongestionControlAlgorithm);
        __fallthrough;
    case QUIC_CONGESTION_CONTROL_ALGORITHM_CUBIC:
        CubicCongestionControlInitialize(Cc, Settings);
        break;
    case QUIC_CONGESTION_CONTROL_ALGORITHM_BBR:
        BbrCongestionControlInitialize(Cc, Settings);
        break;
    }

    //
    // The embedded shaper starts unlimited with the default (0, 0) pair.
    // Initialized after the plugin switch above, because plugin
    // initialization assigns its state template over the whole structure.
    // The (0, 0) pair is always valid, so this cannot fail
    // (specs/bandwidth.md §17, §24). The shaper stores no MTU: the packet
    // size is passed per math call (Path->Mtu at the call sites, §3.3).
    //
    QUIC_STATUS Status =
        QuicBandwidthShaperInit(
            &Cc->Pacer,
            /*BandwidthBitsPerSecond =*/ (uint64_t)0,
            /*BurstWindowUsec        =*/ (uint64_t)0);  // (0, 0) is always a valid unlimited pair
    CXPLAT_FRE_ASSERT(QUIC_SUCCEEDED(Status));
    UNREFERENCED_PARAMETER(Status);
}
