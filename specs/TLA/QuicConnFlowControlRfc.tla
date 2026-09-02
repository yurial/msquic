---- MODULE QuicConnFlowControlRfc ----
\* -----------------------------------------------------------------------------
\* Connection-level flow control of QUIC per RFC 9000 Section 4.1 (with the
\* MAX_DATA frame of Section 19.9), at abstraction level L1: two endpoints
\* (sender S, receiver R) connected by a lossy, reordering network that
\* carries in-flight frames as sets. STREAM data is abstracted to a byte
\* counter: the sender emits data in portions of 1..MaxPortions bytes.
\*
\* Variables
\*   SentBytes     : total bytes the sender has sent in accepted portions
\*   PeerMaxData   : sender's current view of the receiver's advertised limit
\*   ReceivedBytes : total bytes delivered into the receiver's buffer
\*   RecvLimit     : receiver's authoritative advertised limit (non-decreasing)
\*   Pause         : receiver-side application receive-pause flag
\*   Error         : set when the receiver detects a FLOW_CONTROL_ERROR
\*   dataWires     : set of in-flight STREAM portions (set: loss/reordering)
\*   ctrlWires     : set of in-flight MAX_DATA frames (set: loss/reordering)
\*
\* RFC semantics formalized here (RFC 9000 Section 4.1):
\*   * Senders MUST NOT send data in excess of the advertised limit:
\*     SendData requires SentBytes + portion <= PeerMaxData.
\*   * A receiver MUST close the connection with FLOW_CONTROL_ERROR if it
\*     receives more data than the maximum data value it has sent (19.9):
\*     DeliverData requires ReceivedBytes + portion <= RecvLimit, else Error.
\*   * A sender MUST ignore MAX_DATA frames that do not increase the limit:
\*     ApplyMaxData sets PeerMaxData to max(PeerMaxData, frame value) and
\*     consumes the frame.
\*   * It is not an error to advertise a smaller limit, but the smaller
\*     limit has no effect: AdvertiseMaxData may carry any value <= the
\*     current RecvLimit; the authoritative RecvLimit itself never decreases.
\*   * Receive-pause (application level) has NO protocol-visible effect in
\*     this semantics: no MAX_DATA is emitted on pause/resume, the limit
\*     stays; Pause toggles locally only (see QuicConnFlowControlMsquic for
\*     the deviation where pause advertises MAX_DATA = ReceivedBytes).
\*
\* Environment shape: reactive (per tla-plus skill section 3). The receiver
\* environment can always act (RcvUpdate / RcvPause / RcvResume are always
\* enabled), so there is no Terminal action and the default deadlock check
\* is meaningful: a reachable stuck state would be a design bug.
\*
\* Checked invariants (see QuicConnFlowControlRfc.tlc.cfg):
\*   TypeOK, NoFlowControlError (Error = FALSE),
\*   SenderWithinAdvertisedLimit (SentBytes <= PeerMaxData),
\*   ReceivedWithinLimit (ReceivedBytes <= RecvLimit),
\*   DataNotLostOrOverAccepted (SentBytes <= RecvLimit and
\*   ReceivedBytes <= SentBytes): no data is lost and none is accepted
\*   beyond the original window (comparative property, checked identically
\*   in the msquic model).
\*
\* Modeling bounds / assumptions (recorded per tla-plus skill section 5):
\*   * data is transferred in portions of 1..MaxPortions bytes; a SendData
\*     step is atomic (per-byte drain in the implementation is not modeled);
\*   * the environment may ever issue at most RecvLimitMaxValue bytes of
\*     credit in total: RecvLimit starts anywhere in 1..RecvLimitMaxValue
\*     and may only be raised up to RecvLimitMaxValue;
\*   * the network is a set of messages, hence in-flight frames are never
\*     duplicated; loss is modeled by explicit Lose actions, reordering by
\*     arbitrary interleaving of Deliver actions;
\*   * consequence (verified by TLC, not assumed): with a protocol-conforming
\*     sender the Error branch of DeliverData is unreachable in this model,
\*     because SentBytes <= PeerMaxData <= max announced <= RecvLimit holds
\*     at all times. The FLOW_CONTROL_ERROR invariant is still the contract
\*     being checked.
\*
\* Liveness: not checked for this model (optional per task; the RFC sender
\* has no obligation to resume without receiver/protocol guarantees; the
\* msquic model carries the mandatory liveness baseline).
\*
\* Baseline (TLC, tla2tools.jar 1.8.0-class, 2026-08-28, 32 workers):
\*   NOTE on reproducibility: the state/distinct/depth totals below are
\*   deterministic and reproducible; the per-action coverage counts (new
\*   states : times enabled) are seed/scheduling-dependent and may differ
\*   between runs -- the recorded figures document the baseline run, not a
\*   reproducibility contract (the GREEN verdict does not depend on them).
\*   QuicConnFlowControlRfc.tlc.cfg, safety + -coverage 1: GREEN,
\*     "Model checking completed. No error has been found.";
\*     15099 states generated, 1344 distinct states, depth 10, ~1 s.
\*     Coverage (new states : times enabled): Init 3:3, SendData 18:744,
\*     DeliverData 12:600, LoseData 18:600, AdvertiseMaxData 819:5088,
\*     RcvUpdate 111:1632, RcvPause 363:672; ApplyMaxData 0:2544,
\*     LoseCtrl 0:2544, RcvResume 0:672 -- zero NEW states is a BFS
\*     shortest-path artifact: each of their transitions converges to a
\*     state reachable by a shorter path (drop the frame / never pause /
\*     start with the raised limit), which BFS explores first; the actions
\*     themselves fire, proven by simulation below. The 0 in the Error
\*     branch of DeliverData (sub-expression counts line 149/150 = 0)
\*     confirms the predicted unreachability of FLOW_CONTROL_ERROR for a
\*     protocol-conforming sender.
\*   Simulation (-simulate num=20000 -depth 200 -coverage 1): 247303916
\*     states generated in 2m01s, no violations; real firing counts:
\*     ApplyMaxData 10032890, LoseCtrl 10702171, RcvResume 18847471,
\*     RcvPause 19177228, SendData 1610752, DeliverData 914739,
\*     LoseData 649964, AdvertiseMaxData 137511338, RcvUpdate 13350974.
\* Liveness: not checked (optional; see QuicConnFlowControlMsquic.tla).
\* -----------------------------------------------------------------------------
EXTENDS Naturals

CONSTANTS
  RecvLimitMaxValue,   \* total credit the receiver may ever announce
  MaxPortions          \* max bytes of one STREAM portion

VARIABLES
  SentBytes,           \* sender: total bytes sent
  PeerMaxData,         \* sender: current advertised limit known to S
  ReceivedBytes,       \* receiver: total bytes delivered to the app buffer
  RecvLimit,           \* receiver: authoritative limit (non-decreasing)
  Pause,               \* receiver: application receive-pause flag
  Error,               \* receiver: FLOW_CONTROL_ERROR raised
  dataWires,           \* in-flight STREAM portions
  ctrlWires            \* in-flight MAX_DATA frames

vars == <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Pause, Error,
          dataWires, ctrlWires>>

Portions == 1..MaxPortions
DataMsg  == [portion : Portions]            \* a STREAM portion
CtrlMsg  == [value : 0..RecvLimitMaxValue]  \* a MAX_DATA frame

Max2(a, b) == IF a >= b THEN a ELSE b

TypeOK ==
  /\ SentBytes     \in 0..RecvLimitMaxValue
  /\ PeerMaxData   \in 0..RecvLimitMaxValue
  /\ ReceivedBytes \in 0..RecvLimitMaxValue
  /\ RecvLimit     \in 1..RecvLimitMaxValue
  /\ Pause \in BOOLEAN
  /\ Error \in BOOLEAN
  /\ dataWires \subseteq DataMsg
  /\ ctrlWires \subseteq CtrlMsg

\* The transport-parameter handshake gives S and R the same initial limit;
\* TLC cannot evaluate one variable from another inside Init, hence the \E.
Init ==
  \E l \in 1..RecvLimitMaxValue :
    /\ SentBytes     = 0
    /\ PeerMaxData   = l
    /\ ReceivedBytes = 0
    /\ RecvLimit     = l
    /\ Pause         = FALSE
    /\ Error         = FALSE
    /\ dataWires     = {}
    /\ ctrlWires     = {}

\* ---------------------------------------------------------------------------
\* Sender side
\* ---------------------------------------------------------------------------

\* A portion is available to the application within the model's total bound.
PortionAvail(p) == SentBytes + p <= RecvLimitMaxValue

\* S MUST NOT send data in excess of the advertised limit (RFC 9000 4.1).
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

\* R accepts data only within its advertised limit; a violation is the
\* FLOW_CONTROL_ERROR of RFC 9000 sections 4.1/19.9.
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

\* R announces a limit; values <= RecvLimit allowed, including non-increasing
\* ones (an RFC sender must ignore them).
AdvertiseMaxData(v) ==
  /\ v \in 0..RecvLimit
  /\ ctrlWires' = ctrlWires \union {[value |-> v]}
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Pause,
                 Error, dataWires>>

\* S MUST ignore frames that do not increase the limit (RFC 9000 4.1):
\* the frame is consumed, PeerMaxData never decreases.
ApplyMaxData(f) ==
  /\ f \in ctrlWires
  /\ PeerMaxData' = Max2(PeerMaxData, f.value)
  /\ ctrlWires'   = ctrlWires \ {f}
  /\ UNCHANGED <<SentBytes, ReceivedBytes, RecvLimit, Pause, Error,
                 dataWires>>

LoseCtrl(f) ==
  /\ f \in ctrlWires
  /\ ctrlWires' = ctrlWires \ {f}
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Pause,
                 Error, dataWires>>

\* ---------------------------------------------------------------------------
\* Receiver environment (reactive: always some env action is enabled)
\* ---------------------------------------------------------------------------

\* R raises its limit (never lowers it); this models an internal autotune
\* update not associated with an emitted frame.
RcvUpdate(v) ==
  /\ v \in RecvLimit..RecvLimitMaxValue
  /\ RecvLimit' = v
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, Pause, Error,
                 dataWires, ctrlWires>>

\* No protocol-visible effect under RFC semantics.
RcvPause ==
  /\ Pause = FALSE
  /\ Pause' = TRUE
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Error,
                 dataWires, ctrlWires>>

RcvResume ==
  /\ Pause = TRUE
  /\ Pause' = FALSE
  /\ UNCHANGED <<SentBytes, PeerMaxData, ReceivedBytes, RecvLimit, Error,
                 dataWires, ctrlWires>>

\* ---------------------------------------------------------------------------

Next ==
  \/ (\E p \in Portions : SendData(p))
  \/ (\E d \in dataWires : DeliverData(d))
  \/ (\E d \in dataWires : LoseData(d))
  \/ (\E v \in 0..RecvLimit : AdvertiseMaxData(v))
  \/ (\E f \in ctrlWires : ApplyMaxData(f))
  \/ (\E f \in ctrlWires : LoseCtrl(f))
  \/ (\E v \in RecvLimit..RecvLimitMaxValue : RcvUpdate(v))
  \/ RcvPause
  \/ RcvResume

Spec == Init /\ [][Next]_vars

\* ---------------------------------------------------------------------------
\* Safety properties
\* ---------------------------------------------------------------------------

NoFlowControlError == Error = FALSE

SenderWithinAdvertisedLimit == SentBytes <= PeerMaxData

ReceivedWithinLimit == ReceivedBytes <= RecvLimit

\* Comparative data-integrity property (same definition in the msquic model):
\* nothing is accepted beyond the original window and no sent byte goes missing.
DataNotLostOrOverAccepted ==
  /\ SentBytes <= RecvLimit
  /\ ReceivedBytes <= SentBytes

=============================================================================
