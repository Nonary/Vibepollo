#pragma once

namespace video::hdr_stream_policy {
  // Native HDR needs an HDR source. Synthesized HDR deliberately needs SDR;
  // the source's verified SDR state must not downgrade the encoded stream.
  constexpr bool requires_hdr_source(bool hdr_stream_requested, bool synthesizing_hdr) {
    return hdr_stream_requested && !synthesizing_hdr;
  }
}  // namespace video::hdr_stream_policy
