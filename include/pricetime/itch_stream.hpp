#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "pricetime/itch_reader.hpp"

namespace pricetime::itch {

// Streams a framed ITCH file through a fixed-size buffer with plain buffered
// reads, stitching messages across chunk boundaries.
//
// Why this exists when mmap_file.hpp already does: mmap is not free. Mapping
// keeps every touched page in the process working set, so replaying a file
// larger than RAM makes the file fight the order books for memory, measured
// on the real 8.25 GB NASDAQ day against ~4 GB of books in 15.5 GB of RAM,
// the mapped replay crawled at 10 MB/s on a disk that streams at 200+, even
// with explicit prefetching. A read() loop through a 32 MB buffer never
// grows the working set, gets the cache manager's sequential readahead for
// free, and pays only one memmove of at most ~50 bytes per chunk for the
// stitch. The mmap path remains available and measured; this is the default
// for exactly the workload the tools serve.
//
// The handler and the ReadResult semantics match for_each_framed_message;
// offsets are absolute file offsets.
template <class Handler>
ReadResult for_each_framed_stream(const std::string& path, Handler&& h,
                                  std::size_t chunk_bytes = 32u << 20) {
  ReadResult total;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    total.status = ReadStatus::TruncatedPrefix;
    return total;
  }

  // Room for one chunk plus the largest possible carried fragment: a length
  // prefix and one maximal message.
  std::vector<std::uint8_t> buf(chunk_bytes + 64);
  std::size_t have = 0;       // bytes currently in the buffer
  std::uint64_t consumed = 0;  // absolute file offset of buf[0]

  for (;;) {
    in.read(reinterpret_cast<char*>(buf.data() + have),
            static_cast<std::streamsize>(chunk_bytes));
    const std::size_t got = static_cast<std::size_t>(in.gcount());
    const bool at_eof = got < chunk_bytes;
    have += got;
    if (have == 0) return total;  // clean end exactly on a message boundary

    const ReadResult r = for_each_framed_message(buf.data(), have, h);
    total.messages += r.messages;

    if (r.status == ReadStatus::Ok) {
      consumed += have;
      have = 0;
      total.offset = static_cast<std::size_t>(consumed);
      if (at_eof) return total;
      continue;
    }
    if (r.status == ReadStatus::TruncatedPrefix || r.status == ReadStatus::TruncatedMessage) {
      if (at_eof) {
        // A fragment at the true end of the file really is truncation.
        total.status = r.status;
        total.offset = static_cast<std::size_t>(consumed + r.offset);
        total.declared_length = r.declared_length;
        return total;
      }
      // Stitch: carry the fragment to the front and refill behind it.
      const std::size_t rem = have - r.offset;
      std::memmove(buf.data(), buf.data() + r.offset, rem);
      consumed += r.offset;
      have = rem;
      continue;
    }
    // LengthMismatch / ZeroLength / UnknownType: genuinely fatal, report with
    // the absolute offset so the byte can be inspected.
    total.status = r.status;
    total.offset = static_cast<std::size_t>(consumed + r.offset);
    total.bad_type = r.bad_type;
    total.declared_length = r.declared_length;
    total.expected_length = r.expected_length;
    return total;
  }
}

}  // namespace pricetime::itch
