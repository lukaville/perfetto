/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACING_SERVICE_TRACE_BUFFER_V2_H_
#define SRC_TRACING_SERVICE_TRACE_BUFFER_V2_H_

#include <stdint.h>
#include <string.h>

#include <limits>
#include <unordered_map>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/thread_annotations.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/slice.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "src/base/intrusive_list.h"
#include "src/tracing/service/histogram.h"

namespace perfetto {

class TracePacket;

class TraceBufferV2 {
 public:
  // See comment in the header above.
  enum OverwritePolicy { kOverwrite, kDiscard };

  // Identifiers that are constant for a packet sequence.
  struct PacketSequenceProperties {
    ProducerID producer_id_trusted;
    ClientIdentity client_identity_trusted;
    WriterID writer_id;

    uid_t producer_uid_trusted() const { return client_identity_trusted.uid(); }
    pid_t producer_pid_trusted() const { return client_identity_trusted.pid(); }
  };

  void CopyChunkUntrusted(ProducerID producer_id_trusted,
                          const ClientIdentity& client_identity_trusted,

                          WriterID writer_id,
                          ChunkID chunk_id,
                          uint16_t num_fragments,
                          uint8_t chunk_flags,
                          bool chunk_complete,
                          const uint8_t* src,
                          size_t size);

  // To read the contents of the buffer the caller needs to:
  //   BeginRead()
  //   while (ReadNextTracePacket(packet_fragments)) { ... }
  // No other calls to any other method should be interleaved between
  // BeginRead() and ReadNextTracePacket().
  // Reads in the TraceBufferV2 are NOT idempotent.
  void BeginRead();

  bool ReadNextTracePacket(TracePacket*,
                           PacketSequenceProperties* sequence_properties,
                           bool* previous_packet_on_sequence_dropped);

 private:
  friend class TraceBufferTest;
  static constexpr size_t kFragShift = 1;
  static constexpr uint8_t kFragMask = (1 << kFragShift) - 1;
  static constexpr uint8_t kFragValid = 1 << 0;

  struct TBChunk {
    struct ListTraits {
      static constexpr size_t node_offset() {
        return offsetof(TBChunk, list_node);
      }
    };

    explicit TBChunk(size_t sz) : size(static_cast<uint32_t>(sz)) {
      PERFETTO_DCHECK(sz >= sizeof(TBChunk));
      PERFETTO_DCHECK(sz <= std::numeric_limits<decltype(size)>::max());
    }

    // NOTE: the default-initialization of the fields below mattersi. It is used
    // by DeleteNextChunksFor() when writing padding chunks.

    // The only case when list_node.prev/next are == nullptr is the case
    // of a padding record, which is not associated to any sequence.
    // TODO replace with two uint32_t offsets.
    base::IntrusiveListNode list_node{};

    // We store the exact (unaligned) size to tell precisely where the last
    // fragment ends. However, TBChunk(s) are stored in the buffer with proper
    // alignment. When we move in the buffer we use aligned_size() not this.
    uint32_t size = 0;  // Size including sizeof(TBChunk).

    // NOTE: In the case of scraping we can have two contiguous TBChunks
    // (in the same sequence) with the same chunk_id. One containing all the
    // fragments scraped, the other one the fragments added after scraping, at
    // commit time.
    // TODO think about how to handle this in case we lose the scraped chunk.
    // How do we avoid duped packets (i'm thinking of write_into_file)?
    ChunkID chunk_id = 0;

    // These are == the SharedMemoryABI's chunk flags.
    uint8_t flags = 0;

    size_t outer_size() { return base::AlignUp<alignof(TBChunk)>(size); }

    bool is_padding() const {
      return list_node.next == nullptr && list_node.prev == nullptr;
    }

    uint8_t* fragments_begin() {
      return reinterpret_cast<uint8_t*>(this) + sizeof(TBChunk);
    }

    uint8_t* fragments_end() { return fragments_begin() + size; }

    // ProducerAndWriterID pr_wr_id = 0;  // TODO maybe not needed
    // TODO do we need chunk_id?
  };

  enum FragType {
    kFragInvalid = 0,
    kFragWholePacket,
    kFragBegin,
    kFragContinue,
    kFragEnd,
  };

  struct Frag {
    // Points to the varint with (size << kFragShift) & flags.
    uint8_t* size_hdr = nullptr;

    // Points to the payload of the fragment (a few bytes after size_hdr).
    uint8_t* begin = nullptr;

    // The size of the payload (without any flags).
    size_t size = 0;

    FragType type = kFragInvalid;

    void MarkInvalid() { *size_hdr &= ~kFragValid; }
  };

  class BufIterator {
   public:
    // TODO who walk back to previous chunks? Likely the caller yeah.
    explicit BufIterator(TraceBufferV2*);
    BufIterator(const BufIterator&) = default;  // Deliberately copyable.
    BufIterator& operator=(const BufIterator&) = default;

    void Reset(TBChunk* chunk) {
      chunk_ = chunk;
      next_frag_ = chunk->fragments_begin();
    }

    bool PrevChunkInSequence();
    bool NextChunkInSequence();
    bool NextChunkInBuffer();
    bool NextChunk();
    std::optional<Frag> NextFragmentInChunk();

    TBChunk* chunk() { return chunk_; }

    // TODO add flags for continues_from_prev, continues_on_next or FragType

   private:
    // These 3 pointers below are never null and always point to a valid portion
    // of the buffer.
    TraceBufferV2* buf_ = nullptr;
    TBChunk* chunk_ = nullptr;

    // TODO add here a target_chunk_ to handle the
    // NextChunk() to tellwhether we did rewind or not.
    // Position of the next fragment within the current `chunk_`.
    uint8_t* next_frag_ = nullptr;
  };

  // Holds the state for each sequence that has TBCHunk(s) in the buffer.
  struct SequenceState {
    ProducerID producer_id = 0;
    WriterID writer_id = 0;
    ClientIdentity client_identity{};
    ChunkID last_chunk_id = 0;
    base::IntrusiveList<TBChunk, TBChunk::ListTraits> tbchunks{};
  };

  friend class BufIterator;

  explicit TraceBufferV2(OverwritePolicy);
  TraceBufferV2(const TraceBufferV2&) = delete;
  TraceBufferV2& operator=(const TraceBufferV2&) = delete;

  // Not using the implicit copy ctor to avoid unintended copies.
  // This tagged ctor should be used only for Clone().
  struct CloneCtor {};
  TraceBufferV2(CloneCtor, const TraceBufferV2&);

  bool Initialize(size_t size);
  ssize_t DeleteNextChunksFor(size_t bytes_to_clear);

  void DcheckIsAlignedAndWithinBounds(size_t off) const {
    PERFETTO_DCHECK((off & (alignof(TBChunk) - 1)) == 0);
    PERFETTO_DCHECK(off <= size_ - sizeof(TBChunk));
  }

  TBChunk* GetTBChunkAtUnchecked(size_t off) {
    DcheckIsAlignedAndWithinBounds(off);
    // We may be accessing a new (empty) record.
    EnsureCommitted(off + sizeof(TBChunk));
    return reinterpret_cast<TBChunk*>(begin() + off);
  }

  TBChunk* GetTBChunkAt(size_t off) {
    TBChunk* tbchunk = GetTBChunkAtUnchecked(off);
    PERFETTO_CHECK(tbchunk->size >= sizeof(TBChunk) &&
                   tbchunk->size <= (size_ - off));
    return tbchunk;
  }

  void EraseTBChunk(TBChunk*);

  void TODONameReadAndConsume(TBChunk*);

  uint8_t* begin() const { return reinterpret_cast<uint8_t*>(data_.Get()); }
  // uint8_t* end() const { return begin() + size_; }
  size_t size_to_end() const { return size_ - wr_; }

  base::PagedMemory data_;
  size_t size_ = 0;  // Size in bytes of |data_|.
  size_t wr_ = 0;    // Write cursor (offset since start()).

  // Statistics about buffer usage.
  TraceStats::BufferStats stats_;

  std::unordered_map<ProducerAndWriterID, SequenceState> sequences_;

  // Iterator used to implement ReadNextTracePacket().
  BufIterator rd_iter_;

  // When true disable some DCHECKs that have been put in place to detect
  // bugs in the producers. This is for tests that feed malicious inputs and
  // hence mimic a buggy producer.
  bool suppress_client_dchecks_for_testing_ = false;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_V2_H_
