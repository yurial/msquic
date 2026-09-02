---- MODULE QuicConnFlowControlMsquic ----
\* -----------------------------------------------------------------------------
\* Connection-level flow control of QUIC with the msquic deviation
\* (msquic main, receive-pause with lowered-MAX_DATA accounting), at
\* abstraction level L1.
\*
\* Same L1 shape as QuicConnFlowControlRfc: sender S, receiver R, lossy
\* reordering network (in-flight frames as sets), STREAM data abstracted to
\* a byte counter sent in portions of 1..MaxPortions bytes.
\*
\* Variables: identical meanings to the RFC model.
\*   SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Pause, Error,
\*   dataWires, ctrlWires
\*
\* msquic semantics formalized here (the three deviations from the RFC model):
\*   (a) S applies ANY incoming MAX_DATA with a sender-side clamp:
\*       PeerMaxData' = Max(f.value, SentBytes), including a lowering
\*       (connection.c QUIC_FRAME_MAX_DATA: always assign, clamped to
\*       OrderedStreamBytesSent; only the blocked-reason flush is gated on
\*       an increase).  The clamp is observably equivalent -- the remaining
\*       window max(0, PeerMaxData - SentBytes) is unchanged -- and it
\*       restores the full invariant SentBytes <= PeerMaxData.
\*   (b) While paused, R advertises MAX_DATA = ReceivedBytes (zero additional
\*       credit; send.c: OrderedStreamBytesReceived), but the INTERNAL limit
\*       RecvLimit is NOT lowered: DeliverData keeps checking against
\*       RecvLimit, so STREAM data already in flight above the lowered
\*       announcement is accepted normally and never raises FLOW_CONTROL_ERROR.
\*       RcvPause is a no-op when already paused (defensive double-pause check
\*       in QuicConnRecvPause).
\*   (c) RcvResume emits MAX_DATA = RecvLimit (full window, "resume"); it is a
\*       no-op when not paused. Pause/resume may toggle arbitrarily (the
\*       application drives them).
\*   RecvLimit itself never changes in this model (msquic: Send.MaxData is
\*   set from the transport-parameter InitialMaxData and never lowered), so
\*   Init pins it to RecvLimitMaxValue.
\*
\* Sender-side invariant: with the clamp of (a) the FULL invariant
\* SenderWithinAdvertisedLimit (SentBytes <= PeerMaxData) holds here, as in
\* the RFC model. The deviation does not weaken any sender-side invariant;
\* it differs from the RFC model only in who may change the limit and when
\* (R MAY lower vs R MUST ignore non-increasing frames).
\*
\* Checked invariants (see QuicConnFlowControlMsquic.tlc.cfg):
\*   TypeOK; NoFlowControlError (Error = FALSE);
\*   ReceivedWithinLimit (ReceivedBytes <= RecvLimit);
\*   NoWraparound (SentBytes <= RecvLimitMaxValue, the original maximum);
\*   PeerMaxDataSanity (PeerMaxData \in 0..RecvLimitMaxValue);
\*   SenderWithinAdvertisedLimit (SentBytes <= PeerMaxData);
\*   DataNotLostOrOverAccepted (SentBytes <= RecvLimit and
\*   ReceivedBytes <= SentBytes) -- the comparative property: no data is
\*   lost and none is accepted beyond the original window.
\*
\* Liveness (QuicConnFlowControlMsquic.tlc.live.cfg, FairSpec): the
\* delivering-network assumption (weak fairness on DeliverCtrl=ApplyMaxData
\* per frame value; MAX_DATA loss excluded from FairNext -- the documented
\* "delivering network" assumption). Checked properties (see the liveness
\* section for the counterexample that shaped them):
\*   Resumed ~> (PeerMaxData = RecvLimit)
\*   (Resumed /\ ~Pause) ~> (NotFcBlocked \/ Pause)
\* Fairness is placed ONLY on the MAX_DATA delivery action, never on the
\* environment (per tla-plus skill section 3).
\*
\* Modeling bounds / assumptions:
\*   * portions of 1..MaxPortions bytes, sent atomically;
\*   * total credit RecvLimitMaxValue, constant here;
\*   * set-based network: no duplication; STREAM loss modeled (LoseData),
\*     MAX_DATA loss modeled in the safety model (LoseCtrl) and excluded
\*     from the liveness model (FairNext);
\*   * consequence (verified by TLC, not assumed): the Error branch of
\*     DeliverData is unreachable here as well -- precisely because R
\*     checks deliveries against the un-lowered RecvLimit; the in-flight
\*     data above a lowered announcement is accepted (this is the point of
\*     the deviation). The FLOW_CONTROL_ERROR invariant is still the
\*     contract being checked.
\*
\* Baseline (TLC, tla2tools.jar 1.8.0-class, 2026-08-28, 32 workers;
\* clamped ApplyMaxData):
\*   NOTE on reproducibility: the state/distinct/depth totals below are
\*   deterministic and reproducible; the per-action coverage counts (new
\*   states : times enabled) are seed/scheduling-dependent and may differ
\*   between runs -- the recorded figures document the baseline run, not a
\*   reproducibility contract (the GREEN verdict does not depend on them).
\*   QuicConnFlowControlMsquic.tlc.cfg, safety + -coverage 1: GREEN,
\*     "Model checking completed. No error has been found.";
\*     2281 states generated, 432 distinct states, depth 15, ~1 s.
\*     (Depth corrected from 17 on 2026-08-29 during review; the other
\*     figures are as originally recorded.)
\*     Coverage (new states : times enabled): Init 1:1, SendData 53:168,
\*     DeliverData 55:176, LoseData 20:176, ApplyMaxData 104:664,
\*     LoseCtrl 46:664, RcvPause 84:216, RcvResume 69:216 -- every
\*     action fired; no dead code.
\*   QuicConnFlowControlMsquic.tlc.live.cfg, liveness: GREEN,
\*     "Model checking completed. No error has been found.";
\*     1542 states generated, 412 distinct states, depth 14, ~1 s.
\*     (LoseCtrl excluded from FairNext by design.)
\*   Finding (recorded on the pre-clamp revision of this model): the naive
\*     property  Resumed ~> (SentBytes < PeerMaxData)  was VIOLATED (see
\*     the liveness section for the full 11-state lasso). Verdict: not an
\*     msquic bug -- the environment legitimately re-paused and the
\*     property ignored supersession and demand exhaustion; the property
\*     was refined (Live1/Live2) and no constraint hides anything. The
\*     rationale carries over to the clamped model unchanged.
\* -----------------------------------------------------------------------------
EXTENDS Naturals

CONSTANTS
  RecvLimitMaxValue,   \* the original (and only) receive window, never lowered
  MaxPortions          \* max bytes of one STREAM portion

VARIABLES
  SentBytes,           \* sender: total bytes sent
  PeerMaxData,         \* sender: current announced limit (may be lowered!)
  ReceivedBytes,       \* receiver: total bytes delivered to the app buffer
  RecvLimit,           \* receiver: internal limit (never lowered in msquic)
  Pause,               \* receiver: application receive-pause flag
  Error,               \* receiver: FLOW_CONTROL_ERROR raised
  dataWires,           \* in-flight STREAM portions
  ctrlWires            \* in-flight MAX_DATA frames

vars == <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Pause, Error,
          dataWires, ctrlWires>>

Portions == 1..MaxPortions
DataMsg  == [portion : Portions]            \* a STREAM portion
CtrlMsg  == [value : 0..RecvLimitMaxValue]  \* a MAX_DATA frame

Max(a, b) == IF a >= b THEN a ELSE b

TypeOK ==
  /\ SentBytes     \in 0..RecvLimitMaxValue
  /\ PeerMaxData   \in 0..RecvLimitMaxValue
  /\ ReceivedBytes \in 0..RecvLimitMaxValue
  /\ RecvLimit     = RecvLimitMaxValue
  /\ Pause \in BOOLEAN
  /\ Error \in BOOLEAN
  /\ dataWires \subseteq DataMsg
  /\ ctrlWires \subseteq CtrlMsg

Init ==
  /\ SentBytes     = 0
  /\ PeerMaxData   = RecvLimitMaxValue   \* initial limit from transport params
  /\ ReceivedBytes = 0
  /\ RecvLimit     = RecvLimitMaxValue
  /\ Pause         = FALSE
  /\ Error         = FALSE
  /\ dataWires     = {}
  /\ ctrlWires     = {}

\* ---------------------------------------------------------------------------
\* Sender side
\* ---------------------------------------------------------------------------

\* A portion is available to the application within the model's total bound.
PortionAvail(p) == SentBytes + p <= RecvLimitMaxValue

\* S MUST NOT send data beyond the announced limit (this holds even in
\* msquic: sending pauses when the window saturates; stream_send.c clamps
\* the remaining window at zero).
SendData(p) ==
  /\ PortionAvail(p)
  /\ SentBytes + p <= PeerMaxData
  /\ SentBytes' = SentBytes + p
  /\ dataWires'  = dataWires \union {[portion |-> p]}
  /\ UNCHANGED <<PeerMaxData, ReceivedBytes, RecvLimit, Pause, Error,
                 ctrlWires>>

\* ---------------------------------------------------------------------------
\* Network
\* ---------------------------------------------------------------------------

\* msquic checks the delivery against the INTERNAL RecvLimit (Send.MaxData,
\* never lowered), not against the paused announcement; a violation would be
\* FLOW_CONTROL_ERROR.
DeliverData(d) ==
  /\ d \in dataWires
  /\ dataWires' = dataWires \ {d}
  /\ IF ReceivedBytes + d.portion <= RecvLimit
       THEN /\ ReceivedBytes' = ReceivedBytes + d.portion
            /\ Error' = Error
       ELSE /\ ReceivedBytes' = ReceivedBytes
            /\ Error' = TRUE
  /\ UNCHANGED <<SentBytes, PeerMaxData, RecvLimit, Pause, ctrlWires>>

LoseData(d) ==
  /\ d \in dataWires
  /\ dataWires' = dataWires \ {d}
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Pause,
                 Error, ctrlWires>>

\* (a) deviation: S applies ANY MAX_DATA value, including a lowering,
\* clamped to the largest offset already sent (msquic: the applied limit
\* is max(frame value, OrderedStreamBytesSent)); the frame is consumed
\* either way. The clamp keeps SentBytes <= PeerMaxData without changing
\* the observable remaining window.
ApplyMaxData(f) ==
  /\ f \in ctrlWires
  /\ PeerMaxData' = Max(f.value, SentBytes)
  /\ ctrlWires'   = ctrlWires \ {f}
  /\ UNCHANGED <<SentBytes, ReceivedBytes, RecvLimit, Pause, Error,
                 dataWires>>

LoseCtrl(f) ==
  /\ f \in ctrlWires
  /\ ctrlWires' = ctrlWires \ {f}
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Pause,
                 Error, dataWires>>

\* ---------------------------------------------------------------------------
\* Receiver environment (reactive: RcvPause/RcvResume can always act)
\* ---------------------------------------------------------------------------

\* (b) deviation: pause announces ReceivedBytes (zero additional credit)
\* but does NOT lower the internal RecvLimit; no-op when already paused.
RcvPause ==
  /\ Pause = FALSE
  /\ Pause' = TRUE
  /\ ctrlWires' = ctrlWires \union {[value |-> ReceivedBytes]}
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Error,
                 dataWires>>

\* (c) deviation: resume announces the full window (resume); no-op when
\* not paused.
RcvResume ==
  /\ Pause = TRUE
  /\ Pause' = FALSE
  /\ ctrlWires' = ctrlWires \union {[value |-> RecvLimit]}
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Error,
                 dataWires>>

\* ---------------------------------------------------------------------------

Next ==
  \/ (\E p \in Portions : SendData(p))
  \/ (\E d \in dataWires : DeliverData(d))
  \/ (\E d \in dataWires : LoseData(d))
  \/ (\E f \in ctrlWires : ApplyMaxData(f))
  \/ (\E f \in ctrlWires : LoseCtrl(f))
  \/ RcvPause
  \/ RcvResume

Spec == Init /\ [][Next]_vars

\* Delivering-network liveness model: MAX_DATA loss excluded, weak fairness
\* on every MAX_DATA delivery. Quantifier is over the CONSTANT frame domain
\* (TLC rejects fairness over variable-dependent sets).
Frames    == [value : 0..RecvLimitMaxValue]
FairNext ==
  \/ (\E p \in Portions : SendData(p))
  \/ (\E d \in dataWires : DeliverData(d))
  \/ (\E d \in dataWires : LoseData(d))
  \/ (\E f \in ctrlWires : ApplyMaxData(f))
  \/ RcvPause
  \/ RcvResume

FairSpec ==
  /\ Init
  /\ [][FairNext]_vars
  /\ \A f \in Frames : WF_vars(ApplyMaxData(f))

\* ---------------------------------------------------------------------------
\* Safety properties
\* ---------------------------------------------------------------------------

NoFlowControlError == Error = FALSE

ReceivedWithinLimit == ReceivedBytes <= RecvLimit

NoWraparound == SentBytes <= RecvLimitMaxValue

PeerMaxDataSanity == PeerMaxData \in 0..RecvLimitMaxValue

\* Full sender-side invariant (restored by the ApplyMaxData clamp): the
\* internal limit never drops below the amount already sent.
SenderWithinAdvertisedLimit == SentBytes <= PeerMaxData

\* Comparative data-integrity property (same definition in the RFC model):
\* nothing is accepted beyond the original window and no sent byte goes missing.
DataNotLostOrOverAccepted ==
  /\ SentBytes <= RecvLimit
  /\ ReceivedBytes <= SentBytes

\* ---------------------------------------------------------------------------
\* Liveness (under FairSpec)
\* ---------------------------------------------------------------------------
\* First formulation and its counterexample (kept as a finding; the trace
\* was recorded on the pre-clamp revision of this model, where an applied
\* frame value was taken verbatim): the naive
\* property  Resumed ~> (SentBytes < PeerMaxData)  FAILS. TLC trace (msquic
\* live model, RecvLimitMaxValue = 3): R pauses (announces 0), S sends all
\* 3 bytes, R resumes (announces 3), R pauses AGAIN (set-dedup: the [0]
\* frame is still in flight), S delivers [0] -> PeerMaxData = 0 (under
\* the clamp: Max(0, 3) = 3), then delivers [3] -> PeerMaxData = 3, then
\* stutters forever with Pause = TRUE.
\* This is NOT an msquic bug: the receiver application legitimately paused
\* again and never resumed; the environment has no fairness, so R is free
\* to supersede its release and then stay silent. The property was too
\* strong because (1) it ignored legitimate re-pauses and (2) it conflated
\* "flow control blocks sending" with "the application has nothing more to
\* send" (at the model's total bound PortionAvail is false for every
\* portion, so no limit can ever enable a send).
\*
\* Corrected properties:
\*   Live1: Resumed ~> (PeerMaxData = RecvLimit)
\*     a full-window announcement is never lost: by WF delivery it is
\*     eventually applied, and deviation (a) (apply ANY value) makes the
\*     effect order-independent: once applied, S holds the full window.
\*   Live2: (Resumed /\ ~Pause) ~> (NotFcBlocked \/ Pause)
\*     after a release, S becomes unblocked (whenever the application
\*     still has data, credit covers it) -- unless R pauses again, which
\*     is a legitimate superseding decision.
\* ---------------------------------------------------------------------------

\* A full-window announcement is on the wire: R has released the pause
\* (or paused at ReceivedBytes = RecvLimit, which announces the same value).
Resumed == \E f \in ctrlWires : f.value = RecvLimit

\* Flow control does not withhold credit for any portion the application
\* could still send (vacuously true when the total bound is exhausted).
NotFcBlocked == \A p \in Portions : PortionAvail(p) => SentBytes + p <= PeerMaxData

Live ==
  /\ Resumed ~> (PeerMaxData = RecvLimit)
  /\ ((Resumed /\ ~Pause) ~> (NotFcBlocked \/ Pause))

=============================================================================
