//! Nearest-rank percentiles over raw samples. Empty input is not summarized.

use serde::Serialize;

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Summary {
    pub count: usize,
    pub min: u64,
    pub p50: u64,
    pub p95: u64,
    pub p99: u64,
    pub max: u64,
    pub mean: f64,
    pub stdev: f64,
}

pub fn summarize(samples: &[u64]) -> Option<Summary> {
    if samples.is_empty() {
        return None;
    }
    let mut sorted = samples.to_vec();
    sorted.sort_unstable();
    let count = sorted.len();
    let sum = sorted.iter().fold(0u128, |acc, v| acc + u128::from(*v));
    let mean = sum as f64 / count as f64;
    let stdev = if count > 1 {
        let var = samples
            .iter()
            .map(|v| {
                let d = *v as f64 - mean;
                d * d
            })
            .sum::<f64>()
            / (count - 1) as f64;
        var.sqrt()
    } else {
        0.0
    };
    Some(Summary {
        count,
        min: sorted[0],
        p50: percentile(&sorted, 50),
        p95: percentile(&sorted, 95),
        p99: percentile(&sorted, 99),
        max: sorted[count - 1],
        mean,
        stdev,
    })
}

/// Nearest rank: `rank = ceil(percent / 100 * n)`, then the value at `rank - 1`.
pub fn percentile(sorted: &[u64], percent: u8) -> u64 {
    assert!(!sorted.is_empty());
    assert!(percent <= 100);
    let n = sorted.len();
    let rank = (usize::from(percent) * n).div_ceil(100).max(1);
    sorted[rank - 1]
}

pub fn fmt_us(ns: u64) -> String {
    if ns < 1_000 {
        format!("{ns} ns")
    } else {
        format!("{:.1} µs", ns as f64 / 1_000.0)
    }
}

pub fn fmt_us_f(ns: f64) -> String {
    if ns < 1_000.0 {
        format!("{ns:.0} ns")
    } else {
        format!("{:.1} µs", ns / 1_000.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nearest_rank_and_mean() {
        let samples = [10, 20, 30, 40, 100];
        let summary = summarize(&samples).unwrap();
        assert_eq!(summary.min, 10);
        assert_eq!(summary.max, 100);
        assert_eq!(summary.p50, 30);
        assert_eq!(summary.count, 5);
        assert!((summary.mean - 40.0).abs() < 1e-9);
    }

    #[test]
    fn empty_is_none() {
        assert!(summarize(&[]).is_none());
    }

    #[test]
    fn single_sample_stdev_is_zero() {
        let summary = summarize(&[42]).unwrap();
        assert_eq!(summary.p99, 42);
        assert_eq!(summary.stdev, 0.0);
    }
}
