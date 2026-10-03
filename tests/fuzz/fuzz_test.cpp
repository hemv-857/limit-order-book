// The portable fuzz driver.
//
// Runs every target over a committed corpus and then over a deterministic stream
// of mutated inputs. This exists because libFuzzer's coverage-guided search is
// better but its runtime is not available on every toolchain this project builds
// with (docs/PLAN.md deviation D-2), and a fuzz target nobody can run is not a
// fuzz target. Under ASan and UBSan this is a real memory-safety and
// invariant-safety net that runs in ordinary CI.
//
// Deterministic by construction: same binary, same corpus, same failures. A
// fuzzer that finds a different bug on every run is impossible to act on.

#include "fuzz/targets.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/// The corpus is generated rather than committed as opaque blobs, so a reviewer
/// can read what each seed is meant to cover. Opaque binary files in a repository
/// are unreviewable and unmergeable; this is the same coverage in text form.
struct Seed {
  const char* name;
  const char* bytes;
};

/// Embedded so the corpus needs no data files at runtime and cannot go missing.
const Seed kCorpus[] = {
    {"empty", ""},
    {"single_zero", "\x00"},
    {"hello_frame_truncated", "LO\x01"},
    {"hello_frame_no_payload", "LO\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00"},
    {"hello_frame_bad_crc", "LO\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\xff"},
    {"hello_frame_bad_version", "LO\x09\x01\x00\x00\x00\x00\x00\x00\x00\x00"},
    {"hello_frame_bad_magic", "XO\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00"},
    {"absurd_length", "LO\x01\x01\xff\xff\xff\xff\x00\x00\x00\x00"},
    {"unknown_type", "LO\x01\xee\x00\x00\x00\x00\x00\x00\x00\x00"},
    {"string_length_overrun", "LO\x01\x01\x04\x00\x00\x00\xff\xff\xff\xff\x00\x00\x00\x00"},
    {"neworder_short_payload", "LO\x01\x03\x08\x00\x00\x00\x01\x02\x03\x04"},
    {"trailing_bytes", "LO\x01\x03\x09\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09"},
    {"snapshot_frame",
     "LO\x01\x0a\x1d\x00\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x00\x01"
     "\x64\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00\x01\x00\x00\x00\x00"
     "\x00\x00\x00"},
    {"journal_short", "LOBJ\x01\x48\x00\x00\x00"},
    {"journal_two_records",
     "LOBJ\x01\x48\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
    {"journal_bad_crc",
     "LOBJ\x01\x48\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff"},
    {"engine_orders",
     "\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x64\x00\x00\x00\x00"
     "\x00\x00\x00\x05\x00\x00\x00\x00\x00\x00\x00\x00\x01"
     "\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x68\x00\x00\x00\x00"
     "\x00\x00\x00\x05\x00\x00\x00\x00\x00\x00\x00\x00\x02"},
    {"engine_replace_to_filled",
     "\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x64\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x0a\x00\x00\x00\x00\x00\x00\x00\x01"
     "\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x64\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x0a\x00\x00\x00\x00\x00\x00\x00\x02"
     "\x03\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x0a\x00\x00\x00\x00\x00\x00\x00\x01"},
    {"all_ones", "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"},
    {"all_zero", "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"},
};

constexpr lob::fuzz::Target kTargets[] = {
    lob::fuzz::Target::Codec,
    lob::fuzz::Target::Journal,
    lob::fuzz::Target::Engine,
};

/// Every committed seed, against every target. A single looping test rather than a
/// parameterised one: the seeds are data, and a failure should name the seed that
/// broke rather than producing twenty-one near-identical test names.
TEST(FuzzDriver, EveryCommittedSeedIsHandledByEveryTarget) {
  for (const Seed& seed : kCorpus) {
    const std::string bytes(seed.bytes);
    for (lob::fuzz::Target target : kTargets) {
      // Must return rather than crash. A crash or an invariant abort fails here.
      SCOPED_TRACE(std::string("seed=") + seed.name + " target=" + lob::fuzz::name(target));
      (void)lob::fuzz::run(target, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                           bytes.size());
    }
  }
}

/// The scenarios for bugs already found by hand. These must fail deterministically
/// if any of those bugs is reintroduced -- random input reaches them only rarely,
/// so a fuzzer budget alone is not a regression guard.
TEST(FuzzDriver, KnownEngineScenariosHoldTheBookConsistent) {
  ASSERT_GE(lob::fuzz::engine_scenario_count(), 4u);
  lob::fuzz::run_engine_scenarios();
}

/// xorshift64*: tiny, deterministic, and good enough to drive mutation. Not
/// security-relevant, so no need for anything better.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}
  std::uint64_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }
  std::size_t below(std::size_t n) {
    return n == 0 ? 0 : static_cast<std::size_t>(next() % n);
  }

 private:
  std::uint64_t state_;
};

/// Mutation driver: take a seed, flip bits, splice, extend, truncate. This is
/// what a mutator does, minus coverage feedback, and it is enough to find the
/// class of bug these targets guard against.
TEST(FuzzDriver, MutatedCorpusStaysSafe) {
  constexpr int kIterations = 3000;
  Rng rng(0x5EED'1234'ABCD'0001ULL);
  std::vector<std::uint8_t> buffer;

  for (int i = 0; i < kIterations; ++i) {
    // Start from a random seed so different iterations explore different regions.
    const Seed& seed = kCorpus[rng.below(std::size(kCorpus))];
    const std::string base(seed.bytes);
    buffer.assign(base.begin(), base.end());

    const int mutations = 1 + static_cast<int>(rng.below(6));
    for (int m = 0; m < mutations && !buffer.empty(); ++m) {
      switch (rng.below(5)) {
        case 0:  // flip a bit
          buffer[rng.below(buffer.size())] ^= static_cast<std::uint8_t>(1U << rng.below(8));
          break;
        case 1:  // set a byte to an interesting value
          buffer[rng.below(buffer.size())] =
              static_cast<std::uint8_t>(rng.below(4) == 0 ? 0xFF : 0x00);
          break;
        case 2:  // truncate
          buffer.resize(rng.below(buffer.size()));
          break;
        case 3:  // extend
          buffer.push_back(static_cast<std::uint8_t>(rng.next()));
          break;
        default:  // corrupt the magic or version, where parsers branch
          if (!buffer.empty()) {
            buffer[0] = static_cast<std::uint8_t>(rng.below(4) == 0 ? 0x00 : buffer[0]);
          }
          break;
      }
    }

    for (lob::fuzz::Target target : kTargets) {
      (void)lob::fuzz::run(target, buffer.data(), buffer.size());
    }
  }
}

/// Purely random inputs, which reach shapes no seeded mutation would.
TEST(FuzzDriver, RandomInputsStaySafe) {
  Rng rng(0xC0FF'EE00'1234'5678ULL);
  std::vector<std::uint8_t> buffer;
  for (int i = 0; i < 3000; ++i) {
    const std::size_t n = rng.below(96);
    buffer.resize(n);
    for (std::size_t k = 0; k < n; ++k) {
      // Bias toward structural bytes so frames are sometimes well formed.
      buffer[k] = rng.below(3) == 0 ? static_cast<std::uint8_t>(rng.below(256))
                                    : static_cast<std::uint8_t>(rng.below(8));
    }
    for (lob::fuzz::Target target : kTargets) {
      (void)lob::fuzz::run(target, buffer.data(), buffer.size());
    }
  }
}

/// Every truncation of every seed. For a framed protocol the highest-yield single
/// mutation is cutting a valid message short, so it is worth doing exhaustively
/// rather than hoping the mutator finds it.
TEST(FuzzDriver, EveryTruncationOfEverySeedStaysSafe) {
  for (const Seed& seed : kCorpus) {
    const std::string base(seed.bytes);
    for (std::size_t cut = 0; cut <= base.size(); ++cut) {
      const std::string_view prefix(base.data(), cut);
      for (lob::fuzz::Target target : kTargets) {
        (void)lob::fuzz::run(target, reinterpret_cast<const std::uint8_t*>(prefix.data()),
                             prefix.size());
      }
    }
  }
}

/// A null pointer with a non-zero size must be treated as an empty input, not
/// dereferenced. Callers get this wrong, and the target documents the contract.
TEST(FuzzDriver, NullDataIsAnEmptyInputNotACrash) {
  for (lob::fuzz::Target target : kTargets) {
    (void)lob::fuzz::run(target, nullptr, 16);
    (void)lob::fuzz::run(target, nullptr, 0);
  }
}

}  // namespace
