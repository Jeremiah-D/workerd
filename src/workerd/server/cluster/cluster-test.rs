// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::sync::mpsc;
use std::time::Duration;

use cluster_logic::lease::Timing;
use futures::executor::block_on;

use super::Cluster;
use super::Setup;
use super::check_conditional_writes;
use crate::Bucket;
use crate::Error;
use crate::bucket::memory::Memory;

/// Linking, after `join`, opens storage, which on a slow disk takes longer
/// than a lease, in turns of the loop that let nothing else run: the lease
/// thread's rounds, which `join` starts, keep the lease held throughout,
/// and watch the loop only once the node serves (`Cluster::watch`).
#[test]
fn the_lease_holds_however_long_startup_takes_after_join() {
    // Rounds that fence only once the lease thread got no time for most of
    // the lease, and a loop that stands still for less than linking takes.
    let timing = Timing {
        lease_ms: 400,
        interval_ms: 10,
        renew_timeout_ms: 20,
        list_timeout_ms: 20,
        retry_ms: 5,
        renew_attempt_ms: 10,
        list_attempt_ms: 10,
        contended_pause_ms: 5,
        max_skew_ms: 5,
        stall_ms: 50,
    };
    let setup = Setup {
        url: String::new(),
        options: Vec::new(),
        node: "a".into(),
        address: "a:1".into(),
        timing,
        key: vec![0; 32],
    };
    let (fenced, fence) = mpsc::channel();
    let cluster = Cluster::threaded(
        Bucket::memory(""),
        || Ok(Bucket::memory("")),
        setup,
        Box::new(move |reason, _| {
            let _ = fenced.send(reason);
        }),
        Box::new(|body| body()),
    )
    .unwrap();
    block_on(cluster.join()).unwrap();
    // Linking, as one turn of the loop thread that outlasts the lease.
    std::thread::sleep(Duration::from_millis(2 * timing.lease_ms));
    // What an actor database checks before it opens (`require_lease`).
    assert!(cluster.lease_holds());
    assert!(fence.try_recv().is_err());
    // Serving, the loop that stands still from here on fences the node.
    cluster.watch(Box::new(|| 0));
    let reason = fence
        .recv_timeout(Duration::from_millis(10 * timing.lease_ms))
        .unwrap();
    assert!(
        reason.starts_with("its event loop made no progress for "),
        "{reason}"
    );
}

#[test]
fn a_bucket_that_ignores_preconditions_is_refused() {
    block_on(async {
        assert!(
            check_conditional_writes(&Bucket::memory(""), "n.probe")
                .await
                .is_ok()
        );
        let message = |store: Memory| async {
            let bucket = Bucket::new(store, "");
            match check_conditional_writes(&bucket, "n.probe").await {
                Err(Error::Config(message)) => message,
                other => panic!("expected a config error, not {other:?}"),
            }
        };
        let lax = Memory::default();
        lax.ignores_guards.set(true);
        assert!(
            message(lax)
                .await
                .starts_with("the bucket ignores conditional writes")
        );
        let weak = Memory::default();
        weak.never_matches.set(true);
        assert!(
            message(weak)
                .await
                .starts_with("the bucket refused a conditional write whose condition held")
        );
    });
}

/// Each probe write's outcome unknown: what the object holds says which
/// landed, so a store that keeps preconditions passes and one that ignores
/// them does not.
#[test]
fn probe_writes_of_unknown_outcome_are_settled_by_reading_the_probe() {
    block_on(async {
        let check = |store: Memory| async {
            store
                .unknown
                .borrow_mut()
                .extend(["n.probe"; 4].map(str::to_owned));
            let unknown = store.unknown.clone();
            let checked = check_conditional_writes(&Bucket::new(store, ""), "n.probe").await;
            assert!(unknown.borrow().is_empty());
            checked
        };
        check(Memory::default()).await.unwrap();
        let lax = Memory::default();
        lax.ignores_guards.set(true);
        let Err(Error::Config(message)) = check(lax).await else {
            panic!("a store that ignores preconditions passed");
        };
        assert!(message.starts_with("the bucket ignores conditional writes"));
        let weak = Memory::default();
        weak.never_matches.set(true);
        let Err(Error::Config(message)) = check(weak).await else {
            panic!("a store whose versions never match passed");
        };
        assert!(message.starts_with("the bucket refused a conditional write whose condition held"));
    });
}
