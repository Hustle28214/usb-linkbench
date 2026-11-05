//! Monotonic clocks used for every measured interval.
//!
//! Linux and macOS use `CLOCK_MONOTONIC_RAW`, which is not stepped or slewed by NTP.
//! Windows uses `QueryPerformanceCounter`. Intervals are taken from a single clock.
//! A timestamp from this clock is never subtracted from a driver timestamp.

use serde::Serialize;
use std::time::Duration;

#[cfg(unix)]
mod imp {
    use std::sync::atomic::{AtomicBool, Ordering};

    static WARNED: AtomicBool = AtomicBool::new(false);

    pub fn now_ns() -> u64 {
        let mut ts = libc::timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        // CLOCK_MONOTONIC_RAW is 4 on both Linux and macOS.
        let clock_id = 4;
        let rc = unsafe { libc::clock_gettime(clock_id, &mut ts) };
        if rc != 0 {
            if !WARNED.swap(true, Ordering::Relaxed) {
                eprintln!("clock_gettime(CLOCK_MONOTONIC_RAW) 失败，改用 CLOCK_MONOTONIC");
            }
            let rc = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
            if rc != 0 {
                panic!("clock_gettime 失败");
            }
        }
        (ts.tv_sec as u64)
            .saturating_mul(1_000_000_000)
            .saturating_add(ts.tv_nsec as u64)
    }
}

#[cfg(windows)]
mod imp {
    use std::sync::OnceLock;

    #[link(name = "kernel32")]
    extern "system" {
        fn QueryPerformanceCounter(count: *mut i64) -> i32;
        fn QueryPerformanceFrequency(freq: *mut i64) -> i32;
    }

    fn frequency() -> u64 {
        static FREQ: OnceLock<u64> = OnceLock::new();
        *FREQ.get_or_init(|| {
            let mut hz = 0i64;
            let ok = unsafe { QueryPerformanceFrequency(&mut hz) };
            if ok == 0 || hz <= 0 {
                panic!("QueryPerformanceFrequency 失败");
            }
            hz as u64
        })
    }

    pub fn now_ns() -> u64 {
        let mut count = 0i64;
        let ok = unsafe { QueryPerformanceCounter(&mut count) };
        if ok == 0 || count < 0 {
            panic!("QueryPerformanceCounter 失败");
        }
        let hz = frequency() as u128;
        (count as u128).saturating_mul(1_000_000_000) / hz
    }
}

pub fn now_ns() -> u64 {
    imp::now_ns()
}

pub fn elapsed_ns(start: u64) -> u64 {
    now_ns().saturating_sub(start)
}

pub fn overhead_samples(iters: usize) -> Vec<u64> {
    let mut out = Vec::with_capacity(iters);
    for _ in 0..iters {
        let a = now_ns();
        let b = now_ns();
        out.push(b.saturating_sub(a));
    }
    out
}

#[derive(Debug, Clone, Serialize)]
pub struct TimingReport {
    pub measurement_kind: &'static str,
    pub timer_overhead_ns: crate::stats::Summary,
    pub sleep_requested_ns: u64,
    pub sleep_measured_ns: u64,
    pub sleep_ok: bool,
    pub notes: Vec<String>,
}

pub fn timing_check() -> TimingReport {
    let requested = Duration::from_millis(50);
    let started = now_ns();
    std::thread::sleep(requested);
    let measured = elapsed_ns(started);
    let requested_ns = requested.as_nanos() as u64;
    TimingReport {
        measurement_kind: "timer_check",
        timer_overhead_ns: crate::stats::summarize(&overhead_samples(200))
            .expect("overhead samples"),
        sleep_requested_ns: requested_ns,
        sleep_measured_ns: measured,
        sleep_ok: (20_000_000..=1_000_000_000).contains(&measured),
        notes: vec![
            "开销是连续两次读钟的差值，表示测量地板，不从其他样本里扣除。".into(),
            "睡眠检查只确认时钟没有明显停走或跳变，不是 USB 结果。".into(),
        ],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clock_is_monotonic_and_overhead_is_small() {
        let mut prev = now_ns();
        for _ in 0..1_000 {
            let now = now_ns();
            assert!(now >= prev);
            prev = now;
        }
        let samples = overhead_samples(200);
        let mut sorted = samples.clone();
        sorted.sort_unstable();
        let median = sorted[sorted.len() / 2];
        assert!(
            median < 1_000_000,
            "timer overhead median {median} ns is above 1 ms"
        );
    }
}
