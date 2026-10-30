// RVBBL-6555: lightweight wall-time profiling of the cosim bridge hot paths.
// Accumulates ns + call counts per category; dumps to stderr every ~60 s of
// wall time (checked cheaply from the retire path) and at process exit.
// Zero heap, zero locks (relaxed atomics); safe to leave in a one-off .so.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace prof6555 {

enum cat : int {
  RETIRE = 0,      // process_dut_instr_retire total
  WHISPER_STEP,    // whisperStepRPC round trip
  RESYNCH,         // bridge::resynch (full state resync)
  RESYNCH_NEEDED,  // resynch_needed (incl. memmap lookups)
  COMPARE,         // compare_dut_whisper_state
  N_CATS
};

inline const char* cat_name(int c) {
  static const char* n[N_CATS] = {
      "process_dut_instr_retire", "whisperStepRPC", "resynch(full)",
      "resynch_needed", "compare_dut_whisper_state"};
  return n[c];
}

struct acc_t {
  std::atomic<uint64_t> ns{0};
  std::atomic<uint64_t> calls{0};
};

inline acc_t& acc(int c) {
  static acc_t a[N_CATS];
  return a[c];
}

inline uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void dump(const char* why) {
  static std::atomic<uint64_t> t0{0};
  uint64_t z = 0;
  t0.compare_exchange_strong(z, now_ns());
  double wall_s = (now_ns() - t0.load()) / 1e9;
  std::fprintf(stderr, "\n[PROF6555 %s] wall=%.1fs\n", why, wall_s);
  for (int c = 0; c < N_CATS; ++c) {
    uint64_t ns = acc(c).ns.load(std::memory_order_relaxed);
    uint64_t calls = acc(c).calls.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[PROF6555]   %-28s calls=%-12llu total=%8.1fs  avg=%7.1fus"
                 "  %%wall=%5.1f\n",
                 cat_name(c), (unsigned long long)calls, ns / 1e9,
                 calls ? ns / 1e3 / calls : 0.0,
                 wall_s > 0 ? 100.0 * ns / 1e9 / wall_s : 0.0);
  }
  std::fflush(stderr);
}

struct exit_dumper {
  ~exit_dumper() { dump("exit"); }
};

// Called from the retire path; dumps every ~60 s of wall time.
inline void periodic() {
  static exit_dumper at_exit;  // ensures a final dump
  static std::atomic<uint64_t> next{0};
  static std::atomic<uint32_t> tick{0};
  if ((tick.fetch_add(1, std::memory_order_relaxed) & 0xFFFF) != 0)
    return;  // only look at the clock every 64k retires
  uint64_t t = now_ns(), n = next.load(std::memory_order_relaxed);
  if (t >= n && next.compare_exchange_strong(n, t + 60ull * 1000000000ull))
    dump("60s");
}

struct scope {
  int c;
  uint64_t t0;
  explicit scope(int c_) : c(c_), t0(now_ns()) {}
  ~scope() {
    acc(c).ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
    acc(c).calls.fetch_add(1, std::memory_order_relaxed);
  }
};

}  // namespace prof6555
