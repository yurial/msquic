# TLA Specification Index

- specs/TLA/QuicConnFlowControlRfc.tla — QUIC connection-level flow control, RFC 9000 §4.1/§19.9 semantics: sender MUST NOT exceed announced limit, receiver raises FLOW_CONTROL_ERROR on violation, sender ignores non-increasing MAX_DATA (L1 protocol model; safety)
- specs/TLA/QuicConnFlowControlMsquic.tla — the same protocol with the msquic deviation (msquic main, receive-pause with lowered-MAX_DATA accounting): incoming MAX_DATA applied with the sender-side clamp to the largest offset already sent (may lower the limit; the full invariant SentBytes <= PeerMaxData is preserved), receive-pause announces MAX_DATA = ReceivedBytes while the receiver keeps checking deliveries against the un-lowered internal limit (L1 protocol model; safety + liveness)
