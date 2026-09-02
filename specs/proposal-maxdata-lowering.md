# Lowering QUIC Flow Control Limits: Decreasing MAX_DATA and MAX_STREAM_DATA Announcements

```
Document:  draft-dyachenko-quic-lower-flow-control-limits-00
Author:    Yuri Dyachenko <quic@yurial.ru>
Date:      August 28, 2026
Expires:   February 28, 2027 (+6 months from the Date above)
Intended status: Standards Track
```

## Abstract

RFC 9000 defines QUIC flow control limits that can only be raised: a
MAX_DATA or MAX_STREAM_DATA frame carrying a value that does not increase
the corresponding limit has no effect and MUST be ignored by its receiver.
This document proposes a change to RFC 9000 under which a receiver MAY
advertise a smaller limit, the new limit takes effect for data not yet
sent, and violation of the limit is assessed against the largest limit
advertised during the connection.  Data already sent within a previously
advertised limit never becomes a violation, so no new spurious
FLOW_CONTROL_ERROR can be induced.  The change is backwards compatible
with RFC 9000 implementations in both directions and enables immediate
application-level backpressure (receive pause) at the connection and
stream level.

## 1. Introduction

QUIC's connection-level and stream-level flow control (Section 4 of
[RFC9000]) lets a receiver bound how much stream data its peer may send.
The limits, however, are one-directional: Section 4.1 of [RFC9000] states
that "it is not an error to advertise a smaller limit, but the smaller
limit has no effect", and that a sender "MUST ignore any MAX_STREAM_DATA
or MAX_DATA frames that do not increase flow control limits".  Once a
receiver has advertised a limit, every byte of that credit can -- and,
for an application that keeps producing data, eventually will -- arrive
at the receiver and have to be buffered.

This creates a structural problem for receivers whose applications need
to stop consuming data on short notice.  The typical scenario is
application-level backpressure ("receive pause"): the receiving
application suspends draining the receive buffer, and the transport
wants to stop committing memory to new data immediately.  The only tools
RFC 9000 provides are:

* Stop sending MAX_DATA/MAX_STREAM_DATA updates.  This does not stop a
  sender that still holds un-consumed credit: a sender with a large
  advertised window continues to send -- and the receiver continues to
  buffer -- until the full window is exhausted.  A multi-megabyte
  connection-level window can therefore impose multi-megabyte buffering
  on a receiver that needs to pause *now*.

* RESET_STREAM or connection close.  Both discard application data that
  the receiver in fact wants eventually to consume; they are aborts, not
  backpressure.

This document proposes that RFC 9000 be amended so that a receiver MAY
lower its advertised limits and that the lowered limit takes effect for
data not yet sent.  Credit already granted is never withdrawn
retroactively: the receiver enforces its limits against the largest
value it has advertised, so data sent within an earlier announcement
never becomes a violation.  A sender that never exceeds an advertised
limit observes no change in enforcement.

The amended semantics have been formalized as protocol models and checked
with a model checker; under both the RFC 9000 semantics and the semantics
proposed here, a protocol-compliant sender can neither trigger a flow
control error nor lose data.  The change applies symmetrically to the
connection-level limit (MAX_DATA) and the stream-level limit
(MAX_STREAM_DATA), and both frame types are amended by this document.

## 2. Conventions and Terminology

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
"SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED", "MAY", and
"OPTIONAL" in this document are to be interpreted as described in
BCP 14 [RFC2119] [RFC8174] when, and only when, they appear in all
capitals, as shown here.

This document uses the terminology of [RFC9000].  In addition:

Limit:  The absolute cumulative amount of data that an endpoint is
   permitted to send, at connection level (bounded by MAX_DATA) or on a
   single stream (bounded by MAX_STREAM_DATA).

Advertised limit:  A limit value that an endpoint has communicated to
   its peer in an initial_max_data or initial_max_stream_data_*
   transport parameter (Section 18.2 of [RFC9000]) or in a MAX_DATA or
   MAX_STREAM_DATA frame (Sections 19.9 and 19.10 of [RFC9000]).

Largest advertised limit:  The maximum of all advertised limits for the
   connection or for a given stream, from the beginning of the
   connection up to the point of evaluation.  For a connection that
   never lowers its limits, this equals the current limit.

Lowering:  Sending a MAX_DATA or MAX_STREAM_DATA frame whose value is
   smaller than the limit currently in effect.

## 3. Motivation

### 3.1. Receive Pause Needs a Stop Button

Consider a receiver whose connection-level window has been advertised as
N bytes.  At some point the application needs the receiver to stop
accepting new data entirely -- for example, high-priority data has
arrived on another connection, and the receiver must free the entire
incoming path capacity to receive it promptly.  In such cases the
receiver does not merely consume data more slowly; it must stop
buffering new STREAM data at all, as quickly as possible, without losing
anything already received or in flight.

Under RFC 9000 the receiver has no way to express this.  It can only stop
*increasing* the limit.  Everything already advertised remains usable
credit; the sender keeps sending up to N cumulative bytes, and the
receiver must keep buffering whatever arrives.  The pause therefore takes
effect only after the outstanding window is drained -- exactly the
opposite of what backpressure needs.

### 3.2. The Blocked-Sender Cycle Does Not Help

A receiver that stops granting credit does not stop a sender; it only
guarantees that the sender eventually becomes blocked (Section 4.1 of
[RFC9000]).  At that point the sender enters the blocked-sender cycle: it
holds data it cannot send and, per the guidance in Section 4.1 of
[RFC9000], SHOULD periodically send DATA_BLOCKED or STREAM_DATA_BLOCKED
frames with no ack-eliciting packets in flight, partly to keep the
connection from being closed by the idle timeout (Section 10.1 of
[RFC9000]).  This cycle is a symptom, not a remedy: by the time it
begins, the receiver has already buffered the whole window.  It also
leaves the receiver unable to distinguish "sender is politely waiting"
from "sender is dead" any better than today.

### 3.3. The Remaining Tools Lose Data

RESET_STREAM abruptly terminates the sending part of a stream, and its
receiver is permitted to discard data already received on that stream
(Sections 19.4 and 4.4 of [RFC9000]); closing the connection discards
more.  Neither is backpressure: an application that wants to
pause -- not cancel -- has no protocol support.  The gap is
protocol-level: the sender is behaving correctly, the receiver is
behaving correctly, and yet the receiver cannot stop the flow without
risking buffering it cannot afford or data loss it cannot accept.

### 3.4. Stream-Level and Connection-Level Limits Have the Same Shape

The scenario above is usually described at connection level, but a
single misbehaving or backpressured stream has the same problem with its
MAX_STREAM_DATA limit.  Any fix that only covers MAX_DATA leaves
implementations to handle per-stream pause with per-connection tools.
The change proposed here therefore covers both MAX_DATA (connection
level) and MAX_STREAM_DATA (stream level), with identical semantics.

## 4. Protocol Changes

This section specifies normative amendments to [RFC9000].  All section
references below are to [RFC9000] unless marked "(as amended)".

### 4.1. Amendment to Section 4.1 (Data Flow Control)

#### 4.1.1. Applying Updated Limits

The following paragraph of Section 4.1 of [RFC9000]:

> "Once a receiver advertises a limit for the connection or a stream, it
> is not an error to advertise a smaller limit, but the smaller limit has
> no effect."

is replaced with:

> A receiver MAY advertise a smaller limit than the limit currently in
> effect for the connection (MAX_DATA) or for a stream (MAX_STREAM_DATA).
> A smaller limit takes effect for data not yet sent.

The following paragraph of Section 4.1 of [RFC9000]:

> "A sender MUST ignore any MAX_STREAM_DATA or MAX_DATA frames that do
> not increase flow control limits."

is replaced with:

> On receiving a MAX_DATA or MAX_STREAM_DATA frame, an endpoint MUST set
> the corresponding flow control limit to the value carried in the frame,
> whether that value is larger or smaller than the limit currently in
> effect.  A sender MUST NOT send new data in excess of the limit
> currently in effect.  In its internal flow control accounting, a sender
> MAY clamp an applied limit to the largest byte offset it has already
> sent; the sender's remaining credit is the same quantity under either
> representation, so this accounting choice is observably equivalent and
> does not weaken the requirement that a sender MUST NOT send new data
> in excess of the limit currently in effect.

#### 4.1.2. Enforcement Against the Largest Advertised Limit

The following paragraph of Section 4.1 of [RFC9000]:

> "A receiver MUST close the connection with an error of type
> FLOW_CONTROL_ERROR if the sender violates the advertised connection or
> stream data limits; see Section 11 for details on error handling."

is replaced with:

> A receiver MUST NOT treat received data as a violation of its flow
> control limits if the data does not exceed the largest advertised
> limit: specifically, if the sum of the final sizes of all streams (for
> MAX_DATA) or the largest offset received on the affected stream (for
> MAX_STREAM_DATA) does not exceed the largest limit that the receiver
> has advertised for the connection or for that stream, respectively, at
> any point during the connection.  Advertised limits include the initial
> values carried in the initial_max_data and initial_max_stream_data_*
> transport parameters (Section 18.2 of [RFC9000]) and the values
> remembered for Early Data (Section 7.4.1 of [RFC9000]).  A receiver
> MUST close the connection with an error of type FLOW_CONTROL_ERROR
> only if received data exceeds the largest advertised limit; see
> Section 11 for details on error handling.

Correspondingly, a sender whose cumulative sent data (or whose sent data
on an affected stream) is within the largest advertised limit commits no
violation, even if that amount exceeds a limit lowered after the data was
sent.  Advertising a smaller limit does not retroactively withdraw flow
control credit: data in flight at the time a lowered limit is sent can be
up to the previously advertised limit, its receipt is not a flow control
violation, and a receiver MUST accept it.  A lowering bounds
new sends; it does not recall outstanding data.

#### 4.1.3. Reordering of Flow Control Updates

MAX_DATA and MAX_STREAM_DATA frames, like all QUIC frames, can be
reordered.  An endpoint can therefore apply a stale frame whose value is
larger than a value it has already applied.  This is not an error: the
limit in effect is always the value of the most recently applied frame,
and the largest advertised limit -- the bound used for violation
detection (Section 4.1.2) -- does not depend on the order in which
frames are applied.  A sender that receives a stale frame raising the
limit in effect MAY resume sending accordingly; this cannot create a
violation, because any value it can apply was advertised by the receiver
and hence is within the largest advertised limit.

#### 4.1.4. What Does Not Change

Flow control accounting is unchanged.  Connection-level accounting still
agrees on consumed credit across cancelled streams (Section 4.4 of
[RFC9000]), and stream final sizes are defined, communicated, and
enforced exactly as before (Section 4.5 of [RFC9000]); a lowered limit
does not alter final-size accounting and MUST NOT be treated as
affecting any final size.  The definition of FLOW_CONTROL_ERROR
(Section 20.1 of [RFC9000]) is unchanged: "advertised data limits" is
read as the largest advertised limits per Section 4.1.2.

### 4.2. Conforming Amendments to Sections 19.9 and 19.10 (Frame Definitions)

In Section 19.9 (MAX_DATA Frames) of [RFC9000], the description of the
Maximum Data field is extended with:

> The value in the frame is the new absolute limit for the connection,
> whether larger or smaller than any value previously advertised; see
> Section 4.1 (as amended).

and the enforcement paragraph is replaced with:

> All data sent in STREAM frames counts toward this limit.  The sum of
> the final sizes on all streams -- including streams in terminal states
> -- MUST NOT exceed the largest value advertised by a receiver during
> the connection.  An endpoint MUST terminate a connection with an error
> of type FLOW_CONTROL_ERROR if it receives more data than the largest
> maximum data value that it has sent during the connection.  This
> includes violations of remembered limits in Early Data; see Section
> 7.4.1.

In Section 19.10 (MAX_STREAM_DATA Frames) of [RFC9000], the description
of the Maximum Stream Data field is extended with:

> The value in the frame is the new absolute limit for the identified
> stream, whether larger or smaller than any value previously advertised
> for that stream; see Section 4.1 (as amended).

and the sentence "The data sent on a stream MUST NOT exceed the largest
maximum stream data value advertised by the receiver." is retained, with
"advertised by the receiver" read as "advertised by the receiver during
the connection, including the initial value from the corresponding
initial_max_stream_data_* transport parameter" (Section 4.1.2).  The
existing enforcement sentence of Section 19.10 already refers to the
"largest maximum stream data" sent for the affected stream and requires
no change beyond this clarification.

### 4.3. Conforming Amendments to Section 4.2 and Blocked-Sender Behavior

Section 4.2 of [RFC9000] ("Increasing Flow Control Limits") is retitled
"Updating Flow Control Limits" and extended to cover decreases.  The
following is added:

> The same guidance applies to updates that lower a limit.  A receiver
> that lowers a limit SHOULD expect DATA_BLOCKED or STREAM_DATA_BLOCKED
> frames and, as when increasing a limit, MUST NOT wait for such frames
> before sending a MAX_DATA or MAX_STREAM_DATA frame: a sender blocked by
> a lowered limit is not required to report it.
>
> When an updated limit is equal to or lower than the amount of data
> already sent, the sender has no additional credit and is blocked
> immediately (Section 4.1).  The guidance for blocked senders in
> Section 4.1 of [RFC9000] applies unchanged, including for the entire
> duration of a pause: a sender that is flow control limited SHOULD send
> a DATA_BLOCKED or STREAM_DATA_BLOCKED frame and SHOULD continue to send
> it periodically when it has no ack-eliciting packets in flight, to keep
> the connection from being closed by the idle timeout (Section 10.1).
>
> DATA_BLOCKED and STREAM_DATA_BLOCKED frames are informational
> (Sections 19.12 and 19.13).  An endpoint MUST NOT treat a
> DATA_BLOCKED or STREAM_DATA_BLOCKED frame as an error, regardless of
> whether the limit it reports was reached by consuming a large window or
> by applying a lowered limit.  No change to the frame definitions in
> Sections 19.12 and 19.13 is required: the limit at which blocking
> occurred can legitimately be a lowered limit.

## 5. Backwards Compatibility

This section analyzes the four combinations of an endpoint implementing
this document ("new") and an endpoint implementing [RFC9000] as-is
("old"), in both data directions of a connection.  The central asymmetry
that makes the change safe is that, under the amended rules, violation
detection on the receive side is performed against the largest advertised
limit, while the limit in effect on the send side tracks the most recent
update.  A lowering can constrain a peer only if the peer honors it.

### 5.1. Old Sender, New Receiver

An old sender implements [RFC9000] Section 4.1: it MUST ignore
MAX_STREAM_DATA or MAX_DATA frames that do not increase the flow control
limits.  When a new receiver advertises a lowered limit, the old sender
discards the frame and continues to treat the previous -- and therefore
largest advertised -- limit as in effect.  Because an [RFC9000]-compliant
sender never sends data in excess of the limit in effect, all data it
sends is within the largest advertised limit.

A new receiver enforces received data against the largest advertised
limit (Section 4.1.2), which is exactly the bound an old sender never
exceeds.  Traffic therefore flows without errors and without
degradation; the receive-pause request is simply without effect on that
sender, as it is under [RFC9000] today.  In particular, no
FLOW_CONTROL_ERROR can be induced by advertising high and then lowering.

### 5.2. New Sender, Old Receiver

Consider an endpoint implementing this document that, acting as a data
receiver, advertises a lowered limit to a peer that implements only
[RFC9000].  The old peer is REQUIRED by Section 4.1 of [RFC9000] to
ignore the non-increasing frame and to continue counting against its
existing (larger) limit.  It will not stop sending; it will not apply the
lowered value; and it will not commit any violation, because everything
it sends remains within the largest advertised limit.  The new receiver,
in turn, MUST accept that data per Section 4.1.2 and will not raise
FLOW_CONTROL_ERROR.

The net effect is that a lowering sent to a non-implementing peer
degrades to a no-op: the backpressure feature silently falls back to
today's behavior -- the receiver keeps receiving until the previously
advertised window is consumed -- rather than breaking the connection.
The same holds in the reverse data direction of the same connection,
where the roles are those of Section 5.1.

### 5.3. Compatibility Summary

* Old sender with new receiver: no errors, no degradation; pause
  ineffective on that sender.
* New sender with old receiver: no errors, no degradation; pause
  ineffective (the lowered frame is ignored).
* New sender with new receiver: pause takes effect within one round
  trip; safety properties are unchanged, as confirmed by the formal
  models described in Section 1.

The effect of the change is therefore achievable only between two
endpoints that both implement the new behavior: when the peer implements
only [RFC9000], a lowering is harmless but useless -- the frame is
ignored and the pause request degrades to a no-op (Section 5.2).
No handshake flag or explicit negotiation is provided or required:
the change is a pure semantics amendment, and an endpoint can deploy
it unilaterally without any risk to existing peers.  The error
machinery never requires coordination, because no new error condition
is introduced on either side.

## 6. Security Considerations

### 6.1. Lowering as a Denial-of-Service Vector

A malicious or buggy receiver can lower a limit to zero, freezing a
sender that honors lowered limits.  This power is not new:
[RFC9000] already lets a receiver starve a sender by never sending
MAX_DATA or MAX_STREAM_DATA updates, which freezes credit growth once
the current window is consumed.  A lowering reaches the same state
faster -- within one round trip instead of after the window drains --
but does not otherwise expand the attacker's options.

Senders have the same remedies as under [RFC9000], and one more:

* A sender that is flow control limited keeps the connection alive by
  periodically sending DATA_BLOCKED or STREAM_DATA_BLOCKED frames
  (Section 4.3) and is protected from indefinite stalls by the idle
  timeout (Section 10.1 of [RFC9000]).

* Because violation detection is against the largest advertised limit
  (Section 4.1.2), a sender MAY treat a lowered limit as advisory for
  its own policy -- for example, refusing to reduce below a configured
  floor -- without any risk of a flow control error.  A compliant
  sender SHOULD honor lowered limits, since a receiver that must
  protect memory may otherwise have no option but to close the
  connection, but enforcement of the feature is left to the
  application, not the protocol error machinery.

* An application that cannot tolerate stalls MAY close the connection
  with an application error code; the receiver's lowering never forces
  it to stay.

### 6.2. FLOW_CONTROL_ERROR Cannot Be Weaponized by a Lowering

Under [RFC9000], a receiver that advertises a large limit and then stops
updating cannot cause a violation in its peer.  Under the amended rules,
the same holds a fortiori: a receiver that advertises a large limit and
then lowers it also cannot cause a violation, because received data is
checked against the largest advertised limit.  Reordering of stale
frames in either direction likewise cannot produce a spurious error
(Section 4.1.3).  There is no sequence of MAX_DATA or MAX_STREAM_DATA
values, in any order, that causes a compliant sender's legitimate traffic
to be classified as a violation.

### 6.3. Other Considerations

Flow control frames continue to be carried in encrypted packets and are
invisible to on-path attackers, and the amendment introduces no new
frame types, wire formats, or amplification vectors.  The memory
commitment of a receiver is bounded, during a pause, by the largest
advertised limit -- the same bound it accepted under [RFC9000] when it
made the advertisement.  Remembered limits for Early Data
(Section 7.4.1 of [RFC9000]) are treated as advertised limits
(Section 4.1.2); no new replay or downgrade considerations arise.

## 7. IANA Considerations

This document has no IANA actions.

## 8. References

### 8.1. Normative References

[RFC2119]  Bradner, S., "Key words for use in RFCs to Indicate
           Requirement Levels", BCP 14, RFC 2119,
           DOI 10.17487/RFC2119, March 1997.

[RFC8174]  Leiba, B., "Ambiguity of Uppercase vs Lowercase in RFC 2119
           Key Words", BCP 14, RFC 8174, DOI 10.17487/RFC8174,
           May 2017.

[RFC9000]  Iyengar, J., Ed., and M. Thomson, Ed., "QUIC: A UDP-Based
           Multiplexed and Secure Transport", RFC 9000,
           DOI 10.17487/RFC9000, May 2021.

### 8.2. Informative References

None.

## Appendix A. Summary of Changes Relative to RFC 9000

| RFC 9000 location | RFC 9000 (before) | This document (after) |
|---|---|---|
| Section 4.1 ("Once a receiver advertises a limit ...") | "it is not an error to advertise a smaller limit, but the smaller limit has no effect" | A receiver MAY advertise a smaller limit; it takes effect for data not yet sent (Section 4.1.1) |
| Section 4.1 ("A sender MUST ignore any MAX_STREAM_DATA or MAX_DATA frames ...") | "A sender MUST ignore any MAX_STREAM_DATA or MAX_DATA frames that do not increase flow control limits." | An endpoint MUST set the limit in effect to the value carried in the frame, larger or smaller; a sender MUST NOT send new data beyond the limit in effect (Section 4.1.1) |
| Section 4.1 ("A receiver MUST close the connection ...") | Receiver MUST close with FLOW_CONTROL_ERROR if the sender violates "the advertised ... limits" | Violation is assessed against the largest advertised limit (including transport parameter values and remembered 0-RTT limits); data within it MUST be accepted (Section 4.1.2) |
| Section 4.1 ("If a sender has sent data up to the limit ...") | Blocked sender SHOULD send DATA_BLOCKED/STREAM_DATA_BLOCKED; periodic resends to survive idle timeout | Unchanged in substance; explicitly applies for the entire duration of a pause; blocked frames MUST NOT be treated as errors (Section 4.3) |
| Section 4.2 (title) | "Increasing Flow Control Limits" | Retitled "Updating Flow Control Limits"; guidance extended to lowerings; receiver MUST NOT wait for blocked frames before any update (Section 4.3) |
| Section 4.4, Section 4.5 | Stream-cancellation accounting and stream final size rules | Unchanged; final-size accounting is unaffected by a lowered limit (Section 4.1.4) |
| Section 7.4.1 | Remembered transport parameters bound Early Data usage | Unchanged; remembered limits count as advertised limits for the largest-advertised computation (Sections 4.1.2, 6.3) |
| Section 19.9, Maximum Data field | "indicating the maximum amount of data that can be sent on the entire connection" | Extended: the value is the new absolute limit, larger or smaller than any previously advertised (Section 4.2) |
| Section 19.9, enforcement para. | "...MUST NOT exceed the value advertised by a receiver... if it receives more data than the maximum data value that it has sent" | "...MUST NOT exceed the largest value advertised by a receiver during the connection... if it receives more data than the largest maximum data value that it has sent during the connection" (Section 4.2) |
| Section 19.10, Maximum Stream Data field | "indicating the maximum amount of data that can be sent on the identified stream" | Extended: the value is the new absolute limit, larger or smaller than any previously advertised for that stream (Section 4.2) |
| Section 19.10, enforcement para. | "The data sent on a stream MUST NOT exceed the largest maximum stream data value advertised by the receiver." | Retained; "advertised" is read as "advertised during the connection, including the initial transport parameter value" (Section 4.2) |
| Sections 19.12, 19.13 | DATA_BLOCKED / STREAM_DATA_BLOCKED frame definitions | Unchanged; the reported limit can legitimately be a lowered limit (Section 4.3) |
| Section 20.1, FLOW_CONTROL_ERROR | "An endpoint received more data than it permitted in its advertised data limits" | Unchanged; "advertised data limits" is read per the largest-advertised rule (Section 4.1.4) |

## Appendix B. Formal Verification

This appendix is non-normative.  It summarizes the formal models and
model-checking results that serve as supporting evidence for the safety
claims of Sections 4 and 5 -- specifically, that lowering a MAX_DATA
announcement does not create new flow control errors and preserves
backwards compatibility.  The verification was performed with TLC, a
model checker for TLA+ specifications.  This is bounded model checking
over small, finite instances of the protocol: it is evidence about the
explored state space, not a mathematical proof for connections and
limits of arbitrary size.

Two TLA+ models of connection-level flow control between a Sender and a
Receiver were developed.  The reference model follows Section 4.1 of
[RFC9000] exactly: a MAX_DATA frame that does not increase the limit has
no effect and is ignored.  The modified model implements this document:
the Receiver MAY lower its limit, the Sender applies every received
value to the limit in effect with the sender-side accounting clamp of
Section 4.1.1 (the applied limit is never tracked below the largest
byte offset already sent), and the Receiver validates received data
against the largest limit it has ever advertised rather than against
the most recently advertised value.  In both models, the network layer holds
in-flight MAX_DATA frames that can be lost and, being a set of frames,
covers reordering; application data is abstracted as discrete chunks;
and the receiving endpoint can pause and later resume consumption.  All
model bounds are deliberately small -- limits range over 0..3 and at
most 2 data chunks can exist -- which keeps the state space exhaustive
for the explored configurations.

The checked properties include: TypeOK (well-formedness of all state
variables); NoFlowControlError, asserting that the connection never
reaches the FLOW_CONTROL_ERROR state; ReceivedWithinLimit, asserting
that the Receiver never accepts data exceeding the largest limit it has
advertised; SenderWithinAdvertisedLimit, asserting that sent data never
exceeds the limit in effect; absence of data loss; and a liveness
property stating that after the Receiver resumes consumption and a
MAX_DATA frame is delivered, the Sender eventually regains the ability
to send.  TLC found no violations of the safety properties in either
model (safety checking with full state-space coverage; liveness
checking on the modified model).  With the accounting clamp of Section
4.1.1, the modified model preserves the full sender-side invariant of
the reference model -- sent data never exceeds the limit in effect --
because a lowered announcement is tracked as max(the value in the
frame, the amount already sent), which leaves the sender's remaining
credit unchanged.  The difference between the models is therefore not
any invariant of the sender but who may change the limit and when: in
the reference model the receiver MUST NOT lower an advertised limit
(non-increasing frames are ignored), while in the modified model the
receiver MAY lower it and the lowering then bounds data not yet sent.
That difference is compensated on the receive side by the largest
advertised limit rule of Section 4.1.2, so no spurious
FLOW_CONTROL_ERROR can result.

The scope of the verification is limited.  Only connection-level MAX_DATA
is modeled; MAX_STREAM_DATA is covered by the structural symmetry of the
argument (Section 3.4) and was not given a separate model.  Network
conditions are abstracted: loss and reordering of flow control frames
are covered by the set-based network model, but congestion, packet
retransmission timing, cryptography, and the handshake are out of scope.
Accordingly, the results below constitute evidence in favor of the
design, not a proof of its correctness under all circumstances.

The TLA+ modules and TLC models are available in the msquic repository
at https://github.com/yurial/msquic/blob/main/specs/TLA/ :
QuicConnFlowControlRfc.tla (the reference model) and
QuicConnFlowControlMsquic.tla (the modified model), together with the
corresponding *.tlc.cfg files used to check them.

