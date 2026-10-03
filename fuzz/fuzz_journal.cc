// libFuzzer entry point for the journal target.
//
// The body is one line because the target lives in targets.cpp: the driver and
// the thing being driven must not drift apart, or a coverage-guided run and a
// portable run would be testing different code.

#include "fuzz/targets.hpp"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, unsigned long size) {
  return lob::fuzz::run(lob::fuzz::Target::Journal, data, size) ? 1 : 0;
}
