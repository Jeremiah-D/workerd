// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench.h"

#include <capnp/message.h>
#include <kj/async.h>
#include <kj/test.h>

#include <cmath>

namespace workerd::api {
namespace {

// Statistics come from a histogram with three significant figures.
bool near(double actual, double expected) {
  return std::abs(actual - expected) <= expected * 0.001 + 0.001;
}

// The params of a startup group. A `minTimeNs` of 0 leaves the default sampling budget, which the
// fake starts below never reach.
bench::BenchParams::Reader startupParams(capnp::MallocMessageBuilder& message,
    kj::StringPtr caseFilter,
    uint32_t samples,
    bool quick = false,
    uint64_t minTimeNs = 0) {
  auto params = message.initRoot<bench::BenchParams>();
  params.setCaseFilter(caseFilter);
  params.setQuick(quick);
  auto defaults = params.initDefaults();
  defaults.setSamples(samples);
  defaults.setMinTimeNs(minTimeNs);
  return params.asReader();
}

// A start whose phases take 1000, 2000, 3000 and 10000 times `n` ns of wall time, and half that
// of CPU time, where `n` counts the starts from 1.
kj::Function<kj::Promise<WorkerStartupTiming>()> fakeStart(uint& starts) {
  return [&starts]() {
    auto n = static_cast<int64_t>(++starts);
    auto timing = [n](int64_t ns) { return BenchTiming{.wallNs = ns * n, .cpuNs = ns * n / 2}; };
    return kj::Promise<WorkerStartupTiming>(WorkerStartupTiming{
      .isolate = timing(1000),
      .script = timing(2000),
      .worker = timing(3000),
      .total = timing(10000),
    });
  };
}

KJ_TEST("benchWorkerStartup reports each phase of the starts after a warmup") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  capnp::MallocMessageBuilder paramsMessage;
  capnp::MallocMessageBuilder message;
  auto group = message.initRoot<bench::BenchReport::Group>();
  uint starts = 0;
  benchWorkerStartup(startupParams(paramsMessage, "", 3), fakeStart(starts), group).wait(ws);

  KJ_EXPECT(starts == 4);
  KJ_EXPECT(group.getStartup());
  KJ_EXPECT(!group.hasError());
  KJ_EXPECT(!group.hasOverhead());
  auto cases = group.getCases();
  KJ_ASSERT(cases.size() == 4);
  kj::StringPtr names[] = {"startup:isolate", "startup:script", "startup:worker", "startup:total"};
  double scales[] = {1000, 2000, 3000, 10000};
  for (auto i: kj::indices(names)) {
    auto c = cases[i];
    KJ_EXPECT(c.getName() == names[i]);
    KJ_EXPECT(c.getStatus() == bench::BenchReport::Case::Status::OK);
    KJ_EXPECT(c.getIterationsPerSample() == 1);
    // The first start is the warmup.
    auto wall = c.getWallNs().getSamples();
    auto cpu = c.getCpuNs().getSamples();
    KJ_ASSERT(wall.size() == 3);
    KJ_ASSERT(cpu.size() == 3);
    for (auto j: kj::indices(wall)) {
      KJ_EXPECT(wall[j] == scales[i] * static_cast<double>(j + 2), names[i], j);
      KJ_EXPECT(cpu[j] == scales[i] * static_cast<double>(j + 2) / 2, names[i], j);
    }
    KJ_EXPECT(near(c.getWallNs().getMedian(), scales[i] * 3), names[i]);
  }
}

KJ_TEST("benchWorkerStartup reports only the phases the case filter selects") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  {
    capnp::MallocMessageBuilder paramsMessage;
    capnp::MallocMessageBuilder message;
    auto group = message.initRoot<bench::BenchReport::Group>();
    uint starts = 0;
    benchWorkerStartup(startupParams(paramsMessage, "startup:*r*", 2), fakeStart(starts), group)
        .wait(ws);
    KJ_EXPECT(starts == 3);
    auto cases = group.getCases();
    KJ_ASSERT(cases.size() == 2);
    KJ_EXPECT(cases[0].getName() == "startup:script");
    KJ_EXPECT(cases[1].getName() == "startup:worker");
  }
  // Patterns meant for a handler's cases select no phase, so nothing starts.
  for (auto pattern: {"encode*"_kj, "w*"_kj, "total"_kj}) {
    capnp::MallocMessageBuilder paramsMessage;
    capnp::MallocMessageBuilder message;
    auto group = message.initRoot<bench::BenchReport::Group>();
    uint starts = 0;
    benchWorkerStartup(startupParams(paramsMessage, pattern, 2), fakeStart(starts), group).wait(ws);
    KJ_EXPECT(starts == 0, pattern);
    KJ_EXPECT(group.getStartup());
    KJ_EXPECT(!group.hasError());
    KJ_EXPECT(group.getCases().size() == 0, pattern);
  }
}

KJ_TEST("benchWorkerStartup stops at the sampling budget once it has enough samples") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  // The timed starts' totals are 20us, 30us, 40us, ...: five add up to 200us and seven to 350us.
  struct Expected {
    uint64_t minTimeNs;
    uint samples;
  };
  for (auto expected: {Expected{300'000, 7}, Expected{1, 5}}) {
    capnp::MallocMessageBuilder paramsMessage;
    capnp::MallocMessageBuilder message;
    auto group = message.initRoot<bench::BenchReport::Group>();
    uint starts = 0;
    benchWorkerStartup(startupParams(paramsMessage, "startup:total", 50, false, expected.minTimeNs),
        fakeStart(starts), group)
        .wait(ws);
    KJ_EXPECT(starts == expected.samples + 1, expected.minTimeNs, starts);
    auto cases = group.getCases();
    KJ_ASSERT(cases.size() == 1);
    KJ_EXPECT(cases[0].getWallNs().getSamples().size() == expected.samples, expected.minTimeNs);
  }
}

KJ_TEST("benchWorkerStartup under --quick skips the warmup and caps the samples") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  capnp::MallocMessageBuilder paramsMessage;
  capnp::MallocMessageBuilder message;
  auto group = message.initRoot<bench::BenchReport::Group>();
  uint starts = 0;
  benchWorkerStartup(
      startupParams(paramsMessage, "startup:total", 1000, true), fakeStart(starts), group)
      .wait(ws);
  KJ_EXPECT(starts == 5);
  auto cases = group.getCases();
  KJ_ASSERT(cases.size() == 1);
  auto wall = cases[0].getWallNs().getSamples();
  KJ_ASSERT(wall.size() == 5);
  KJ_EXPECT(wall[0] == 10000);
}

KJ_TEST("benchWorkerStartup fails the group when a start fails") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  capnp::MallocMessageBuilder paramsMessage;
  capnp::MallocMessageBuilder message;
  auto group = message.initRoot<bench::BenchReport::Group>();
  uint starts = 0;
  benchWorkerStartup(startupParams(paramsMessage, "", 3),
      [&starts]() {
    // The warmup succeeds; the first timed start fails.
    if (starts++ == 0) return kj::Promise<WorkerStartupTiming>(WorkerStartupTiming{});
    return kj::Promise<WorkerStartupTiming>(KJ_EXCEPTION(FAILED, "the worker failed to start"));
  }, group)
      .wait(ws);
  KJ_EXPECT(starts == 2);
  KJ_EXPECT(group.getStartup());
  KJ_EXPECT(
      kj::StringPtr(group.getError()).contains("the worker failed to start"), group.getError());
  KJ_EXPECT(group.getCases().size() == 0);
}

}  // namespace
}  // namespace workerd::api
