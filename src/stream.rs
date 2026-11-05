//! Video payload goodput.
//!
//! On Linux this reads V4L2 mmap buffers from the kernel, not through
//! libv4lconvert, and counts `bytesused`. That is the frame the driver
//! delivered. It is not the USB wire byte count: UVC headers and packet
//! overhead stay outside the number. Windows and macOS do not expose that
//! same buffer, so this measurement is skipped there instead of substituting
//! a decoded RGB length.

use serde::Serialize;

use crate::Error;
use crate::stats::Summary;

#[derive(Debug, Clone, Serialize)]
pub struct VideoNode {
    pub name: String,
    pub path: String,
    pub vendor_id: Option<u16>,
    pub product_id: Option<u16>,
}

#[derive(Debug, Clone)]
pub struct CaptureRequest {
    pub paths: Vec<String>,
    pub seconds: f64,
    pub warmup_frames: u32,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub fourcc: Option<[u8; 4]>,
    pub fps: Option<u32>,
    pub min_fps: Option<f64>,
    pub expect_frame_bytes: Option<u64>,
    /// When true, any node that cannot stream fails the call.
    pub strict_nodes: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct StreamVerdict {
    pub min_fps: Option<f64>,
    pub achieved_fps: f64,
    pub fps_ok: Option<bool>,
    pub expected_frame_bytes: Option<u64>,
    pub mean_frame_bytes: f64,
    pub frame_bytes_ok: Option<bool>,
}

#[derive(Debug, Clone, Serialize)]
pub struct FrameStream {
    pub node: String,
    pub driver: String,
    pub card: String,
    pub width: u32,
    pub height: u32,
    pub fourcc: String,
    pub sizeimage: u32,
    pub driver_interval: String,
    pub warmup_frames: u32,
    pub frames: u64,
    pub error_frames: u64,
    pub empty_frames: u64,
    pub dropped_frames: u64,
    pub bytes: u64,
    pub elapsed_ns: u64,
    pub window_fps: f64,
    pub window_bytes_per_sec: f64,
    pub mean_frame_bytes: f64,
    pub interval_source: String,
    pub driver_fps: Option<f64>,
    pub interval_ns: Option<Summary>,
    pub timestamp_monotonic: bool,
    pub stalled: bool,
    pub verdict: Option<StreamVerdict>,
}

#[derive(Debug, Clone, Serialize)]
pub struct SkippedNode {
    pub node: String,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct StreamReport {
    pub measurement_kind: &'static str,
    pub streams: Vec<FrameStream>,
    pub skipped: Vec<SkippedNode>,
    pub aggregate_bytes_per_sec: f64,
    pub timer_overhead_ns: Summary,
    pub notes: Vec<String>,
}

pub fn video_nodes() -> Vec<VideoNode> {
    #[cfg(target_os = "linux")]
    {
        linux::video_nodes()
    }
    #[cfg(not(target_os = "linux"))]
    {
        Vec::new()
    }
}

pub fn capture(request: &CaptureRequest) -> Result<StreamReport, Error> {
    if request.seconds <= 0.0 {
        return Err(Error::Message("采集秒数必须大于 0".into()));
    }
    if request.paths.is_empty() {
        return Err(Error::Message(
            "没有视频节点。Linux 上用 --device /dev/videoX，或用 --vid 匹配相机。".into(),
        ));
    }
    #[cfg(target_os = "linux")]
    {
        linux::capture(request)
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = request;
        Err(Error::Message(
            "码流载荷只在 Linux 上通过 V4L2 统计驱动交出的 bytesused。Windows 和 macOS 的相机会话会改像素格式，本工具不用解码后的长度冒充 USB 载荷。链路协商、控制回传、描述符上限和存储直读在这三个系统上可用。"
                .into(),
        ))
    }
}

pub fn verdict_failed(report: &StreamReport) -> bool {
    report.streams.iter().any(|stream| {
        stream.verdict.as_ref().is_some_and(|verdict| {
            verdict.fps_ok == Some(false) || verdict.frame_bytes_ok == Some(false)
        })
    })
}

#[cfg(target_os = "linux")]
mod linux {
    use std::fs;
    use std::path::Path;
    use std::sync::{Arc, Barrier};
    use std::thread;
    use std::time::Duration;

    use v4l::buffer::{Flags, Type};
    use v4l::io::mmap::Stream;
    use v4l::io::traits::CaptureStream;
    use v4l::video::Capture;
    use v4l::video::capture::Parameters;
    use v4l::{Device, Format, FourCC};

    use super::{CaptureRequest, FrameStream, SkippedNode, StreamReport, StreamVerdict, VideoNode};
    use crate::Error;
    use crate::clock;
    use crate::stats;

    struct Arrive<'a> {
        barrier: &'a Barrier,
        armed: bool,
    }

    impl Drop for Arrive<'_> {
        fn drop(&mut self) {
            if self.armed {
                self.barrier.wait();
            }
        }
    }

    pub fn video_nodes() -> Vec<VideoNode> {
        let mut nodes = Vec::new();
        let entries = match fs::read_dir("/sys/class/video4linux") {
            Ok(entries) => entries,
            Err(_) => return nodes,
        };
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            let canonical = fs::canonicalize(entry.path().join("device")).ok();
            let (vendor_id, product_id) = canonical.as_deref().map(usb_ids).unwrap_or((None, None));
            nodes.push(VideoNode {
                path: format!("/dev/{name}"),
                name,
                vendor_id,
                product_id,
            });
        }
        nodes.sort_by(|a, b| a.name.cmp(&b.name));
        nodes
    }

    fn usb_ids(start: &Path) -> (Option<u16>, Option<u16>) {
        let mut current = Some(start.to_path_buf());
        while let Some(path) = current {
            let vendor = path.join("idVendor");
            let product = path.join("idProduct");
            if vendor.is_file() && product.is_file() {
                return (read_hex_id(&vendor), read_hex_id(&product));
            }
            current = path.parent().map(Path::to_path_buf);
        }
        (None, None)
    }

    fn read_hex_id(path: &Path) -> Option<u16> {
        let text = fs::read_to_string(path).ok()?;
        u16::from_str_radix(text.trim(), 16).ok()
    }

    struct Job {
        path: String,
        seconds: f64,
        warmup_frames: u32,
        width: Option<u32>,
        height: Option<u32>,
        fourcc: Option<[u8; 4]>,
        fps: Option<u32>,
        min_fps: Option<f64>,
        expect_frame_bytes: Option<u64>,
    }

    pub fn capture(request: &CaptureRequest) -> Result<StreamReport, Error> {
        let jobs: Vec<Job> = request
            .paths
            .iter()
            .cloned()
            .map(|path| Job {
                path,
                seconds: request.seconds,
                warmup_frames: request.warmup_frames,
                width: request.width,
                height: request.height,
                fourcc: request.fourcc,
                fps: request.fps,
                min_fps: request.min_fps,
                expect_frame_bytes: request.expect_frame_bytes,
            })
            .collect();
        let barrier = Arc::new(Barrier::new(jobs.len()));
        let mut handles = Vec::with_capacity(jobs.len());
        for job in jobs {
            let barrier = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                let path = job.path.clone();
                capture_one(job, &barrier).map_err(|err| (path, err.to_string()))
            }));
        }

        let mut streams = Vec::new();
        let mut skipped = Vec::new();
        for handle in handles {
            match handle.join() {
                Ok(Ok(stream)) => streams.push(stream),
                Ok(Err((node, reason))) => skipped.push(SkippedNode { node, reason }),
                Err(_) => skipped.push(SkippedNode {
                    node: String::new(),
                    reason: "视频采集线程异常退出".into(),
                }),
            }
        }
        if request.strict_nodes && !skipped.is_empty() {
            return Err(Error::Message(
                skipped
                    .iter()
                    .map(|item| format!("{}: {}", item.node, item.reason))
                    .collect::<Vec<_>>()
                    .join("; "),
            ));
        }
        if streams.is_empty() {
            return Err(Error::Message(
                skipped
                    .iter()
                    .map(|item| format!("{}: {}", item.node, item.reason))
                    .collect::<Vec<_>>()
                    .join("; "),
            ));
        }
        Ok(finish_report(streams, skipped))
    }

    fn capture_one(job: Job, barrier: &Barrier) -> Result<FrameStream, Error> {
        let mut gate = Arrive {
            barrier,
            armed: true,
        };
        let dev = Device::with_path(&job.path)
            .map_err(|err| Error::Message(format!("打开 {} 失败: {err}", job.path)))?;
        if let (Some(width), Some(height), Some(fourcc)) = (job.width, job.height, job.fourcc) {
            dev.set_format(&Format::new(width, height, FourCC::new(&fourcc)))
                .map_err(|err| Error::Message(format!("设置格式失败: {err}")))?;
        }
        if let Some(fps) = job.fps {
            dev.set_params(&Parameters::with_fps(fps))
                .map_err(|err| Error::Message(format!("设置帧率失败: {err}")))?;
        }
        let caps = dev
            .query_caps()
            .map_err(|err| Error::Message(format!("QUERYCAP 失败: {err}")))?;
        let format = dev
            .format()
            .map_err(|err| Error::Message(format!("读取格式失败: {err}")))?;
        let interval = dev
            .params()
            .map(|params| {
                format!(
                    "{}/{}",
                    params.interval.numerator, params.interval.denominator
                )
            })
            .unwrap_or_else(|_| "unavailable".into());
        let mut stream = Stream::with_buffers(&dev, Type::VideoCapture, 4)
            .map_err(|err| Error::Message(format!("申请 mmap 缓冲失败: {err}")))?;
        stream.set_timeout(Duration::from_secs(1));
        for _ in 0..job.warmup_frames {
            stream
                .next()
                .map_err(|err| Error::Message(format!("预热取帧失败: {err}")))?;
        }
        gate.armed = false;
        barrier.wait();
        Ok(measure(
            &job,
            &caps.driver,
            &caps.card,
            &format,
            interval,
            &mut stream,
        ))
    }

    fn measure(
        job: &Job,
        driver: &str,
        card: &str,
        format: &Format,
        interval: String,
        stream: &mut Stream<'_>,
    ) -> FrameStream {
        let limit_ns = (job.seconds * 1_000_000_000.0) as u64;
        let started = clock::now_ns();
        let mut frames = 0u64;
        let mut error_frames = 0u64;
        let mut empty_frames = 0u64;
        let mut dropped_frames = 0u64;
        let mut bytes = 0u64;
        let mut last_seq: Option<u32> = None;
        let mut dequeue_intervals = Vec::new();
        let mut last_dequeue: Option<u64> = None;
        let mut driver_intervals = Vec::new();
        let mut last_driver_ts: Option<u64> = None;
        let mut first_driver_ts: Option<u64> = None;
        let mut last_driver_ts_seen: Option<u64> = None;
        let mut monotonic_frames = 0u64;
        let mut other_frames = 0u64;
        let mut stalled = false;

        loop {
            if clock::elapsed_ns(started) >= limit_ns {
                break;
            }
            let next = stream.next();
            let dequeued = clock::now_ns();
            let (bytesused, flags, timestamp, sequence) = match next {
                Ok((_buf, meta)) => (meta.bytesused, meta.flags, meta.timestamp, meta.sequence),
                Err(err) if err.kind() == std::io::ErrorKind::TimedOut => {
                    stalled = true;
                    break;
                }
                Err(_) => {
                    stalled = true;
                    break;
                }
            };
            if let Some(prev) = last_dequeue {
                dequeue_intervals.push(dequeued.saturating_sub(prev));
            }
            last_dequeue = Some(dequeued);
            if let Some(prev) = last_seq {
                let gap = sequence.wrapping_sub(prev);
                if gap > 1 {
                    dropped_frames += u64::from(if gap > 1_000 { 1 } else { gap - 1 });
                }
            }
            last_seq = Some(sequence);
            if flags.contains(Flags::TIMESTAMP_MONOTONIC)
                && timestamp.sec >= 0
                && timestamp.usec >= 0
            {
                monotonic_frames += 1;
                let ts = timestamp.sec as u64 * 1_000_000_000 + timestamp.usec as u64 * 1_000;
                if first_driver_ts.is_none() {
                    first_driver_ts = Some(ts);
                }
                if let Some(prev) = last_driver_ts
                    && ts >= prev
                {
                    driver_intervals.push(ts - prev);
                }
                last_driver_ts = Some(ts);
                last_driver_ts_seen = Some(ts);
            } else {
                other_frames += 1;
            }
            frames += 1;
            if flags.contains(Flags::ERROR) {
                error_frames += 1;
                continue;
            }
            if bytesused == 0 {
                empty_frames += 1;
                continue;
            }
            bytes += u64::from(bytesused);
        }

        let elapsed_ns = clock::elapsed_ns(started).max(1);
        let elapsed_s = elapsed_ns as f64 / 1e9;
        let payload_frames = frames.saturating_sub(error_frames + empty_frames);
        let mean_frame_bytes = if payload_frames == 0 {
            0.0
        } else {
            bytes as f64 / payload_frames as f64
        };
        let timestamp_monotonic = monotonic_frames > 0 && other_frames == 0;
        let (interval_source, interval_ns) = if timestamp_monotonic {
            ("driver_monotonic", stats::summarize(&driver_intervals))
        } else {
            ("userspace_dequeue", stats::summarize(&dequeue_intervals))
        };
        let driver_fps = if timestamp_monotonic {
            match (first_driver_ts, last_driver_ts_seen) {
                (Some(first), Some(last)) if monotonic_frames >= 2 && last > first => {
                    Some((monotonic_frames - 1) as f64 * 1e9 / (last - first) as f64)
                }
                _ => None,
            }
        } else {
            None
        };
        FrameStream {
            node: job.path.clone(),
            driver: driver.to_string(),
            card: card.to_string(),
            width: format.width,
            height: format.height,
            fourcc: fourcc_string(&format.fourcc),
            sizeimage: format.size,
            driver_interval: interval,
            warmup_frames: job.warmup_frames,
            frames,
            error_frames,
            empty_frames,
            dropped_frames,
            bytes,
            elapsed_ns,
            window_fps: frames as f64 / elapsed_s,
            window_bytes_per_sec: bytes as f64 / elapsed_s,
            mean_frame_bytes,
            interval_source: interval_source.into(),
            driver_fps,
            interval_ns,
            timestamp_monotonic,
            stalled,
            verdict: build_verdict(
                frames as f64 / elapsed_s,
                mean_frame_bytes,
                job.min_fps,
                job.expect_frame_bytes,
            ),
        }
    }

    fn build_verdict(
        achieved_fps: f64,
        mean_frame_bytes: f64,
        min_fps: Option<f64>,
        expect_frame_bytes: Option<u64>,
    ) -> Option<StreamVerdict> {
        if min_fps.is_none() && expect_frame_bytes.is_none() {
            return None;
        }
        Some(StreamVerdict {
            min_fps,
            achieved_fps,
            fps_ok: min_fps.map(|min| achieved_fps + f64::EPSILON >= min),
            expected_frame_bytes: expect_frame_bytes,
            mean_frame_bytes,
            frame_bytes_ok: expect_frame_bytes.map(|expected| {
                let low = expected as f64 * 0.95;
                let high = expected as f64 * 1.25;
                mean_frame_bytes >= low && mean_frame_bytes <= high
            }),
        })
    }

    fn fourcc_string(fourcc: &FourCC) -> String {
        String::from_utf8_lossy(&fourcc.repr).trim().to_string()
    }

    fn finish_report(streams: Vec<FrameStream>, skipped: Vec<SkippedNode>) -> StreamReport {
        let aggregate_bytes_per_sec = streams
            .iter()
            .map(|stream| stream.window_bytes_per_sec)
            .sum();
        StreamReport {
            measurement_kind: "payload_goodput",
            streams,
            skipped,
            aggregate_bytes_per_sec,
            timer_overhead_ns: stats::summarize(&clock::overhead_samples(200))
                .expect("overhead samples"),
            notes: vec![
                "window_bytes_per_sec 使用驱动交出的 bytesused 和实测墙钟。预热帧不计入。".into(),
                "分母是实际经过的时间，不是请求的秒数。".into(),
                "这是视频载荷，不含 UVC 包头和总线编码开销。".into(),
                "多路节点在预热后同时进入计时窗；合计速率是各路窗内速率之和。".into(),
            ],
        }
    }
}
