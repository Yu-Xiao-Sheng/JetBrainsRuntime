/*
 * Copyright (c) 1997, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_SHARED_GENERATION_HPP
#define SHARE_GC_SHARED_GENERATION_HPP

#include "gc/shared/collectorCounters.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"
#include "memory/virtualspace.hpp"
#include "runtime/mutex.hpp"
#include "runtime/perfData.hpp"

// A Generation models a heap area for similarly-aged objects.
// It will contain one ore more spaces holding the actual objects.
//
// The Generation class hierarchy:
//
// Generation                      - abstract base class
// - DefNewGeneration              - allocation area (copy collected)
// - TenuredGeneration             - tenured (old object) space (markSweepCompact)
//
// The system configuration currently allowed is:
//
//   DefNewGeneration + TenuredGeneration
//

class DefNewGeneration;
class GCMemoryManager;
class ContiguousSpace;

class OopClosure;
class GCStats;

// A "ScratchBlock" represents a block of memory in one generation usable by
// another.  It represents "num_words" free words, starting at and including
// the address of "this".
struct ScratchBlock {
  ScratchBlock* next;
  size_t num_words;
  HeapWord scratch_space[1];  // Actually, of size "num_words-2" (assuming
                              // first two fields are word-sized.)
};

class Generation: public CHeapObj<mtGC> {
  friend class VMStructs;
 private:
  MemRegion _prev_used_region; // for collectors that want to "remember" a value for
                               // used region at some specific point during collection.

  GCMemoryManager* _gc_manager;

 protected:
  // Minimum and maximum addresses for memory reserved (not necessarily
  // committed) for generation.
  // Used by card marking code. Must not overlap with address ranges of
  // other generations.
  MemRegion _reserved;

  // Memory area reserved for generation
  VirtualSpace _virtual_space;

  // Performance Counters
  CollectorCounters* _gc_counters;

  // Statistics for garbage collection
  GCStats* _gc_stats;

  // Initialize the generation.
  Generation(ReservedSpace rs, size_t initial_byte_size);

 public:
  // The set of possible generation kinds.
  enum Name {
    DefNew,
    MarkSweepCompact,
    Other
  };

  enum SomePublicConstants {
    // Generations are GenGrain-aligned and have size that are multiples of
    // GenGrain.
    // Note: on ARM we add 1 bit for card_table_base to be properly aligned
    // (we expect its low byte to be zero - see implementation of post_barrier)
    LogOfGenGrain = 16 ARM32_ONLY(+1),
    GenGrain = 1 << LogOfGenGrain
  };


  virtual Generation::Name kind() { return Generation::Other; }

  virtual size_t capacity() const = 0;  // The maximum number of object bytes the
                                        // generation can currently hold.
  virtual size_t used() const = 0;      // The number of used bytes in the gen.
  virtual size_t free() const = 0;      // The number of free bytes in the gen.

  // Support for java.lang.Runtime.maxMemory(); see CollectedHeap.
  // Returns the total number of bytes  available in a generation
  // for the allocation of objects.
  virtual size_t max_capacity() const;

  // If this is a young generation, the maximum number of bytes that can be
  // allocated in this generation before a GC is triggered.
  virtual size_t capacity_before_gc() const { return 0; }

  // The largest number of contiguous free bytes in the generation,
  // including expansion  (Assumes called at a safepoint.)
  virtual size_t contiguous_available() const = 0;
  // The largest number of contiguous free bytes in this or any higher generation.
  virtual size_t max_contiguous_available() const;

  // Returns true if promotions of the specified amount are
  // likely to succeed without a promotion failure.
  // Promotion of the full amount is not guaranteed but
  // might be attempted in the worst case.
  virtual bool promotion_attempt_is_safe(size_t max_promotion_in_bytes) const;

  // Return an estimate of the maximum allocation that could be performed
  // in the generation without triggering any collection or expansion
  // activity.  It is "unsafe" because no locks are taken; the result
  // should be treated as an approximation, not a guarantee, for use in
  // heuristic resizing decisions.
  virtual size_t unsafe_max_alloc_nogc() const = 0;

  // Returns true if this generation cannot be expanded further
  // without a GC. Override as appropriate.
  virtual bool is_maximal_no_gc() const {
    return _virtual_space.uncommitted_size() == 0;
  }

  MemRegion reserved() const { return _reserved; }

  // Returns a region guaranteed to contain all the objects in the
  // generation.
  virtual MemRegion used_region() const { return _reserved; }

  MemRegion prev_used_region() const { return _prev_used_region; }
  virtual void  save_used_region()   { _prev_used_region = used_region(); }

  // Returns "TRUE" iff "p" points into the committed areas in the generation.
  // For some kinds of generations, this may be an expensive operation.
  // To avoid performance problems stemming from its inadvertent use in
  // product jvm's, we restrict its use to assertion checking or
  // verification only.
  virtual bool is_in(const void* p) const;

  /* Returns "TRUE" iff "p" points into the reserved area of the generation. */
  bool is_in_reserved(const void* p) const {
    return _reserved.contains(p);
  }

  // Iteration - do not use for time critical operations
  virtual void space_iterate(SpaceClosure* blk, bool usedOnly = false) = 0;

  // Returns the first space, if any, in the generation that can participate
  // in compaction, or else "null".
  virtual ContiguousSpace* first_compaction_space() const = 0;

  // Returns "true" iff this generation should be used to allocate an
  // object of the given size.  Young generations might
  // wish to exclude very large objects, for example, since, if allocated
  // often, they would greatly increase the frequency of young-gen
  // collection.
  virtual bool should_allocate(size_t word_size, bool is_tlab) {
    bool result = false;
    size_t overflow_limit = (size_t)1 << (BitsPerSize_t - LogHeapWordSize);
    if (!is_tlab || supports_tlab_allocation()) {
      result = (word_size > 0) && (word_size < overflow_limit);
    }
    return result;
  }

  // Allocate and returns a block of the requested size, or returns "null".
  // Assumes the caller has done any necessary locking.
  virtual HeapWord* allocate(size_t word_size, bool is_tlab) = 0;

  // Like "allocate", but performs any necessary locking internally.
  virtual HeapWord* par_allocate(size_t word_size, bool is_tlab) = 0;

  // Thread-local allocation buffers
  virtual bool supports_tlab_allocation() const { return false; }
  virtual size_t tlab_capacity() const {
    guarantee(false, "Generation doesn't support thread local allocation buffers");
    return 0;
  }
  virtual size_t tlab_used() const {
    guarantee(false, "Generation doesn't support thread local allocation buffers");
    return 0;
  }
  virtual size_t unsafe_max_tlab_alloc() const {
    guarantee(false, "Generation doesn't support thread local allocation buffers");
    return 0;
  }

  // "obj" is the address of an object in a younger generation.  Allocate space
  // for "obj" in the current (or some higher) generation, and copy "obj" into
  // the newly allocated space, if possible, returning the result (or null if
  // the allocation failed).
  //
  // The "obj_size" argument is just obj->size(), passed along so the caller can
  // avoid repeating the virtual call to retrieve it.
  virtual oop promote(oop obj, size_t obj_size);

  // Returns "true" iff collect() should subsequently be called on this
  // this generation. See comment below.
  // This is a generic implementation which can be overridden.
  //
  // Note: in the current (1.4) implementation, when genCollectedHeap's
  // incremental_collection_will_fail flag is set, all allocations are
  // slow path (the only fast-path place to allocate is DefNew, which
  // will be full if the flag is set).
  // Thus, older generations which collect younger generations should
  // test this flag and collect if it is set.
  virtual bool should_collect(bool   full,
                              size_t word_size,
                              bool   is_tlab) {
    return (full || should_allocate(word_size, is_tlab));
  }

  // Returns true if the collection is likely to be safely
  // completed. Even if this method returns true, a collection
  // may not be guaranteed to succeed, and the system should be
  // able to safely unwind and recover from that failure, albeit
  // at some additional cost.
  virtual bool collection_attempt_is_safe() {
    guarantee(false, "Are you sure you want to call this method?");
    return true;
  }

  // Perform a garbage collection.
  // If full is true attempt a full garbage collection of this generation.
  // Otherwise, attempting to (at least) free enough space to support an
  // allocation of the given "word_size".
  virtual void collect(bool   full,
                       bool   clear_all_soft_refs,
                       size_t word_size,
                       bool   is_tlab) = 0;

  // Perform a heap collection, attempting to create (at least) enough
  // space to support an allocation of the given "word_size".  If
  // successful, perform the allocation and return the resulting
  // "oop" (initializing the allocated block). If the allocation is
  // still unsuccessful, return "null".
  virtual HeapWord* expand_and_allocate(size_t word_size, bool is_tlab) = 0;

  // Some generations may require some cleanup or preparation actions before
  // allowing a collection.  The default is to do nothing.
  virtual void gc_prologue(bool full) {}

  // Some generations may require some cleanup actions after a collection.
  // The default is to do nothing.
  virtual void gc_epilogue(bool full) {}

  // Save the high water marks for the used space in a generation.
  virtual void record_spaces_top() {}

  // Generations may keep statistics about collection. This method
  // updates those statistics. current_generation is the generation
  // that was most recently collected. This allows the generation to
  // decide what statistics are valid to collect. For example, the
  // generation can decide to gather the amount of promoted data if
  // the collection of the young generation has completed.
  GCStats* gc_stats() const { return _gc_stats; }
  virtual void update_gc_stats(Generation* current_generation, bool full) {}

  // Accessing "marks".

  // This function gives a generation a chance to note a point between
  // collections.  For example, a contiguous generation might note the
  // beginning allocation point post-collection, which might allow some later
  // operations to be optimized.
  virtual void save_marks() {}

  // This function is "true" iff any no allocations have occurred in the
  // generation since the last call to "save_marks".
  virtual bool no_allocs_since_save_marks() = 0;

  // When an older generation has been collected, and perhaps resized,
  // this method will be invoked on all younger generations (from older to
  // younger), allowing them to resize themselves as appropriate.
  virtual void compute_new_size() = 0;

  // Printing
  virtual const char* name() const = 0;
  virtual const char* short_name() const = 0;

  // Iteration.

  // Iterate over all objects in the generation, calling "cl.do_object" on
  // each.
  virtual void object_iterate(ObjectClosure* cl);

  // Block abstraction.

  // Returns the address of the start of the "block" that contains the
  // address "addr".  We say "blocks" instead of "object" since some heaps
  // may not pack objects densely; a chunk may either be an object or a
  // non-object.
  virtual HeapWord* block_start(const void* addr) const;

  // Requires "addr" to be the start of a block, and returns "TRUE" iff
  // the block is an object.
  virtual bool block_is_obj(const HeapWord* addr) const;

  virtual void print() const;
  virtual void print_on(outputStream* st) const;

  virtual void verify() = 0;

  struct StatRecord {
    int invocations;
    elapsedTimer accumulated_time;
    StatRecord() :
      invocations(0),
      accumulated_time(elapsedTimer()) {}
  };
private:
  StatRecord _stat_record;
public:
  StatRecord* stat_record() { return &_stat_record; }

  virtual void print_summary_info_on(outputStream* st);

  // Performance Counter support
  virtual void update_counters() = 0;
  virtual CollectorCounters* counters() { return _gc_counters; }

  GCMemoryManager* gc_manager() const {
    assert(_gc_manager != nullptr, "not initialized yet");
    return _gc_manager;
  }

  void set_gc_manager(GCMemoryManager* gc_manager) {
    _gc_manager = gc_manager;
  }

};

#endif // SHARE_GC_SERIAL_GENERATION_HPP
