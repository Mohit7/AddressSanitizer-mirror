/* Copyright 2011 Google Inc.
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

// This file is a part of AddressSanitizer, an address sanity checker.

#include "asan_interceptors.h"
#include "asan_lock.h"
#include "asan_stack.h"
#include "asan_thread.h"

#include <string.h>

// ----------------------- ProcSelfMaps ----------------------------- {{{1
#ifdef ASAN_USE_SYSINFO
#include "sysinfo.h"
class ProcSelfMaps {
 public:
  void Init() {
    ScopedLock lock(&mu_);
    if (map_size_ != 0) return;  // already inited
    if (__asan_flag_v >= 2) {
      Printf("ProcSelfMaps::Init()\n");
    }
    ProcMapsIterator it(0, &proc_self_maps_);   // 0 means "current pid"

    uint64 start, end, offset;
    int64 inode;
    char *flags, *filename;
    CHECK(map_size_ == 0);
    while (it.Next(&start, &end, &flags, &offset, &inode, &filename)) {
      CHECK(map_size_ < kMaxProcSelfMapsSize);
      Mapping &mapping = memory_map[map_size_];
      mapping.beg = start;
      mapping.end = end;
      mapping.offset = offset;
      __asan::real_strncpy(mapping.name,
                           filename, ASAN_ARRAY_SIZE(mapping.name));
      mapping.name[ASAN_ARRAY_SIZE(mapping.name) - 1] = 0;
      if (__asan_flag_v >= 2) {
        Printf("[%ld] ["PP","PP"] off "PP" %s\n", map_size_,
               mapping.beg, mapping.end, mapping.offset, mapping.name);
      }
      map_size_++;
    }
  }

  void Print() {
    Printf("%s\n", proc_self_maps_);
  }

  void PrintPc(uintptr_t pc, int idx) {
    for (size_t i = 0; i < map_size_; i++) {
      Mapping &m = memory_map[i];
      if (pc >= m.beg && pc < m.end) {
        uintptr_t offset = pc - m.beg;
        if (i == 0) offset = pc;
        Printf("    #%d 0x%lx (%s+0x%lx)\n", idx, pc, m.name, offset);
        return;
      }
    }
    Printf("  #%d 0x%lx\n", idx, pc);
  }

 private:
  void copy_until_new_line(const char *str, char *dest, size_t max_size) {
    size_t i = 0;
    for (; str[i] && str[i] != '\n' && i < max_size - 1; i++) {
      dest[i] = str[i];
    }
    dest[i] = 0;
  }


  struct Mapping {
    uintptr_t beg, end, offset;
    char name[1000];
  };
  static const size_t kMaxNumMapEntries = 4096;
  static const size_t kMaxProcSelfMapsSize = 1 << 20;
  ProcMapsIterator::Buffer proc_self_maps_;
  size_t map_size_;
  Mapping memory_map[kMaxNumMapEntries];

  static AsanLock mu_;
};

static ProcSelfMaps proc_self_maps;
AsanLock ProcSelfMaps::mu_;


void AsanStackTrace::PrintStack(uintptr_t *addr, size_t size) {
  proc_self_maps.Init();
  for (size_t i = 0; i < size && addr[i]; i++) {
    uintptr_t pc = addr[i];
    // int line;
    proc_self_maps.PrintPc(pc, i);
    // Printf("  #%ld 0x%lx %s\n", i, pc, rtn.c_str());
  }
}
#elif defined(ASAN_USE_EXTERNAL_SYMBOLIZER)
extern bool
ASAN_USE_EXTERNAL_SYMBOLIZER(const void *pc, char *out, int out_size);

void AsanStackTrace::PrintStack(uintptr_t *addr, size_t size) {
  for (size_t i = 0; i < size && addr[i]; i++) {
    uintptr_t pc = addr[i];
    char buff[4096];
    ASAN_USE_EXTERNAL_SYMBOLIZER((void*)pc, buff, sizeof(buff));
    Printf("  #%ld 0x%lx %s\n", i, pc, buff);
  }
}

#else  // ASAN_USE_SYSINFO
void AsanStackTrace::PrintStack(uintptr_t *addr, size_t size) {
  for (size_t i = 0; i < size && addr[i]; i++) {
    uintptr_t pc = addr[i];
    Printf("  #%ld 0x%lx\n", i, pc);
  }
}
#endif  // ASAN_USE_SYSINFO

// ----------------------- AsanStackTrace ----------------------------- {{{1
_Unwind_Reason_Code AsanStackTrace::Unwind_Trace(
    struct _Unwind_Context *ctx, void *param) {
  AsanStackTrace *b = (AsanStackTrace*)param;
  CHECK(b->size < b->max_size);
  b->trace[b->size] = _Unwind_GetIP(ctx);
  // Printf("ctx: %p ip: %lx\n", ctx, b->buff[b->cur]);
  b->size++;
  if (b->size == b->max_size) return _URC_NORMAL_STOP;
  return _URC_NO_REASON;
}

void AsanStackTrace::FastUnwindStack(uintptr_t *frame) {
  size = 0;
  if (!__asan_inited) return;
  trace[size++] = GET_CALLER_PC();
  AsanThread *t = AsanThread::GetCurrent();
  if (!t) return;
  uintptr_t *prev_frame = frame;
  uintptr_t *top = (uintptr_t*)t->stack_top();
  while (frame >= prev_frame &&
         frame < top &&
         size < max_size) {
    uintptr_t pc = frame[1];
    trace[size++] = pc;
    prev_frame = frame;
    frame = (uintptr_t*)frame[0];
  }
}

void AsanStackTrace::PrintCurrent(uintptr_t pc) {
  GET_STACK_TRACE_HERE(kStackTraceMax, /*fast unwind*/false);
  CHECK(stack.size >= 2);
  size_t skip_frames = 1;
  if (pc) {
    // find this pc, should be somewehre around 3-rd frame
    for (size_t i = skip_frames; i < stack.size; i++) {
      if (stack.trace[i] == pc) {
        skip_frames = i;
        break;
      }
    }
  }
  PrintStack(stack.trace + skip_frames, stack.size - skip_frames);
}

// On 32-bits we don't compress stack traces.
// On 64-bits we compress stack traces: if a given pc differes slightly from
// the previous one, we record a 31-bit offset instead of the full pc.
size_t AsanStackTrace::CompressStack(AsanStackTrace *stack,
                                   uint32_t *compressed, size_t size) {
#if __WORDSIZE == 32
  // Don't compress, just copy.
  size_t res = 0;
  for (size_t i = 0; i < stack->size && i < size; i++) {
    compressed[i] = stack->trace[i];
    res++;
  }
  for (size_t i = stack->size; i < size; i++) {
    compressed[i] = 0;
  }
#else  // 64 bits, compress.
  uintptr_t prev_pc = 0;
  const uintptr_t kMaxOffset = (1ULL << 30) - 1;
  uintptr_t c_index = 0;
  size_t res = 0;
  for (size_t i = 0, n = stack->size; i < n; i++) {
    uintptr_t pc = stack->trace[i];
    if (!pc) break;
    if ((int64_t)pc < 0) break;
    // Printf("C pc[%ld] %lx\n", i, pc);
    if (prev_pc - pc < kMaxOffset || pc - prev_pc < kMaxOffset) {
      uintptr_t offset = (int64_t)(pc - prev_pc);
      offset |= (1U << 31);
      if (c_index >= size) break;
      // Printf("C co[%ld] offset %lx\n", i, offset);
      compressed[c_index++] = offset;
    } else {
      uintptr_t hi = pc >> 32;
      uintptr_t lo = (pc << 32) >> 32;
      CHECK((hi & (1 << 31)) == 0);
      if (c_index + 1 >= size) break;
      // Printf("C co[%ld] hi/lo: %lx %lx\n", c_index, hi, lo);
      compressed[c_index++] = hi;
      compressed[c_index++] = lo;
    }
    res++;
    prev_pc = pc;
  }
  for (size_t i = c_index; i < size; i++) {
    compressed[i] = 0;
  }
#endif  // __WORDSIZE

  // debug-only code
#if 0
  AsanStackTrace check_stack;
  UncompressStack(&check_stack, compressed, size);
  if (res < check_stack.size) {
    Printf("res %ld check_stack.size %ld; c_size %ld\n", res,
           check_stack.size, size);
  }
  // |res| may be greater than check_stack.size, because
  // UncompressStack(CompressStack(stack)) eliminates the 0x0 frames.
  CHECK(res >= check_stack.size);
  CHECK(0 == memcmp(check_stack.trace, stack->trace,
                    check_stack.size * sizeof(uintptr_t)));
#endif

  return res;
}

void AsanStackTrace::UncompressStack(AsanStackTrace *stack,
                                     uint32_t *compressed, size_t size) {
#if __WORDSIZE == 32
  // Don't uncompress, just copy.
  stack->size = 0;
  for (size_t i = 0; i < size && i < kStackTraceMax; i++) {
    if (!compressed[i]) break;
    stack->size++;
    stack->trace[i] = compressed[i];
  }
#else  // 64 bits, uncompress
  uintptr_t prev_pc = 0;
  stack->size = 0;
  for (size_t i = 0; i < size && stack->size < kStackTraceMax; i++) {
    uint32_t x = compressed[i];
    uintptr_t pc = 0;
    if (x & (1U << 31)) {
      // Printf("U co[%ld] offset: %x\n", i, x);
      // this is an offset
      int32_t offset = x;
      offset = (offset << 1) >> 1;  // remove the 31-byte and sign-extend.
      pc = prev_pc + offset;
      CHECK(pc);
    } else {
      // CHECK(i + 1 < size);
      if (i + 1 >= size) break;
      uintptr_t hi = x;
      uintptr_t lo = compressed[i+1];
      // Printf("U co[%ld] hi/lo: %lx %lx\n", i, hi, lo);
      i++;
      pc = (hi << 32) | lo;
      if (!pc) break;
    }
    // Printf("U pc[%ld] %lx\n", stack->size, pc);
    stack->trace[stack->size++] = pc;
    prev_pc = pc;
  }
#endif  // __WORDSIZE
}
