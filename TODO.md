# TODO

## Known issues / deviations
- [ ] Deliberate deviation from RFC 9000 Section 4.1 (both connection and
  stream level): an incoming MAX_DATA frame always sets the connection
  flow control limit, and an incoming MAX_STREAM_DATA frame always sets the
  stream flow control limit, including lowering it (receive-pause advertises
  MAX_DATA/MAX_STREAM_DATA equal to the already-received byte count/offset).
  Sender-side accounting is clamped to the largest byte offset already sent
  (`Send.PeerMaxData = max(Frame.MaximumData, OrderedStreamBytesSent)`,
  `Stream.MaxAllowedSendOffset = max(Frame.MaximumData, MaxSentLength)`), so
  the invariants `OrderedStreamBytesSent <= PeerMaxData` and
  `MaxSentLength <= MaxAllowedSendOffset` hold unconditionally and
  retransmissions are never blocked by flow control.
  Do not propagate to other branches without deciding on the protocol
  deviation. Verified with TLA+/TLC (specs/TLA/QuicConnFlowControlRfc.tla vs
  QuicConnFlowControlMsquic.tla, 2026-08-28): both models are safety-green
  (NoFlowControlError, ReceivedWithinLimit, DataNotLostOrOverAccepted,
  NoWraparound, PeerMaxDataSanity, SenderWithinAdvertisedLimit), the msquic
  model's liveness is green; FLOW_CONTROL_ERROR and data loss/over-acceptance
  are unreachable. Known behavioral notes: while receive is paused,
  connection-level credit earned from deliveries/RESET_STREAM is parked in
  `Send.DeferredMaxData` and applied to `Send.MaxData` on resume (so
  pause/resume cycles don't permanently shrink the advertised window);
  ETW QUIC_TRACE_API_TYPE values for the new APIs are appended at the end of
  the enum and mirrored in the manifest valueMap.

## Pre-existing (unrelated to MAX_DATA)
- [ ] `SpinFrame.SpinFrame1000000` hangs in msquiccoretest (also before the
  MAX_DATA changes).
- [ ] `MsQuicEtw.man` valueMap lacks entries for enum values 33/34
  (`QUIC_TRACE_API_REGISTRATION_CLOSE2`,
  `QUIC_TRACE_API_CONNECTION_EXPORT_KEYING_MATERIAL`): ETW decoding shows
  unmapped values for those two APIs. Pre-existing; found during review.
