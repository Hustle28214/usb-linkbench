//! One camera: negotiated link, descriptor ceiling, control round-trip, then payload.

use std::time::Duration;

use serde::Serialize;

use crate::Error;
use crate::budget::{self, BudgetReport, Fit, ModeBudget};
use crate::stream::{self, CaptureRequest, StreamReport};
use crate::usb::{
    self, AuditReport, DeviceFilter, DeviceInfo, DeviceRecord, RttOptions, RttReport,
};

#[derive(Debug, Clone)]
pub struct RunRequest {
    pub filter: DeviceFilter,
    pub all_matches: bool,
    pub seconds: f64,
    pub warmup_frames: u32,
    pub rtt_count: u32,
    pub rtt_warmup: u32,
    pub rtt_timeout: Duration,
    pub presets: Vec<&'static ModeBudget>,
    pub strict: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct Step<T> {
    pub status: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub value: Option<T>,
}

#[derive(Debug, Clone, Serialize)]
pub struct DeviceRun {
    pub device: DeviceRecord,
    pub audit: Step<AuditReport>,
    pub rtt: Step<RttReport>,
    pub stream: Step<StreamReport>,
    pub planning: BudgetReport,
    pub preset_vs_descriptor: Fit,
}

#[derive(Debug, Clone, Serialize)]
pub struct RunReport {
    pub measurement_kind: &'static str,
    pub devices: Vec<DeviceRun>,
    pub checks_failed: bool,
    pub notes: Vec<String>,
}

pub fn run(request: &RunRequest) -> Result<RunReport, Error> {
    let devices = if request.all_matches {
        usb::select_many(&request.filter)?
    } else {
        vec![usb::select_one(&request.filter)?]
    };
    let mut runs = Vec::with_capacity(devices.len());
    let mut checks_failed = false;
    for device in devices {
        let (one, failed) = run_one(&device, request)?;
        checks_failed |= failed;
        runs.push(one);
    }
    Ok(RunReport {
        measurement_kind: "camera_run",
        devices: runs,
        checks_failed,
        notes: vec![
            "audit 和 rtt 在打开控制通道后会关掉，再开始码流，避免占住视频接口。".into(),
            "码流失败或本机没有 V4L2 时，该项为 skipped/failed，不改写其他实测。".into(),
        ],
    })
}

fn run_one(info: &DeviceInfo, request: &RunRequest) -> Result<(DeviceRun, bool), Error> {
    let device = usb::describe_device(info)?;
    let audit = match usb::audit(info) {
        Ok(report) => Step {
            status: "ok",
            reason: None,
            value: Some(report),
        },
        Err(err) => Step {
            status: "failed",
            reason: Some(err.to_string()),
            value: None,
        },
    };
    let rtt = match usb::control_rtt(
        info,
        &RttOptions {
            count: request.rtt_count,
            warmup: request.rtt_warmup,
            timeout: request.rtt_timeout,
            keep_samples: false,
        },
    ) {
        Ok(report) => Step {
            status: "ok",
            reason: None,
            value: Some(report),
        },
        Err(err) => Step {
            status: "failed",
            reason: Some(err.to_string()),
            value: None,
        },
    };

    let payloads: Vec<u64> = request
        .presets
        .iter()
        .map(|mode| mode.payload_bytes_per_sec())
        .collect();
    let labels = request
        .presets
        .iter()
        .map(|mode| format!("{} ({})", mode.id, mode.summary))
        .collect();
    let planning = budget::budget_report(labels, &payloads, Some(device.speed));
    let ceiling = audit
        .value
        .as_ref()
        .and_then(|report| report.max_periodic_ceiling_bytes_per_sec);
    let aggregate = planning.aggregate_bytes_per_sec;
    let preset_vs_descriptor = if payloads.is_empty() {
        Fit::Unknown
    } else {
        budget::fit_payload(aggregate, ceiling)
    };

    let stream = capture_step(&device, request);
    let mut checks_failed = stream.value.as_ref().is_some_and(stream::verdict_failed);
    if request.strict {
        if audit.status != "ok" || rtt.status != "ok" || stream.status == "failed" {
            checks_failed = true;
        }
        if matches!(
            planning
                .highlighted
                .as_ref()
                .map(|plan| plan.aggregate_vs_bus),
            Some(Fit::Exceeds)
        ) || matches!(
            planning
                .highlighted
                .as_ref()
                .map(|plan| plan.largest_stream_vs_one_endpoint),
            Some(Fit::Exceeds)
        ) || preset_vs_descriptor == Fit::Exceeds
        {
            checks_failed = true;
        }
    }
    Ok((
        DeviceRun {
            device,
            audit,
            rtt,
            stream,
            planning,
            preset_vs_descriptor,
        },
        checks_failed,
    ))
}

fn capture_step(device: &DeviceRecord, request: &RunRequest) -> Step<StreamReport> {
    if device.video_nodes.is_empty() {
        return Step {
            status: "skipped",
            reason: Some(
                "没有匹配的 V4L2 节点。Windows/macOS 不统计码流载荷；Linux 上确认 uvcvideo 已创建 /dev/video。"
                    .into(),
            ),
            value: None,
        };
    }
    let (min_fps, expect_frame_bytes) =
        if device.video_nodes.len() == 1 && request.presets.len() == 1 {
            let mode = request.presets[0];
            (
                Some(f64::from(mode.fps) * 0.95),
                Some(mode.payload_bytes_per_frame()),
            )
        } else {
            (None, None)
        };
    match stream::capture(&CaptureRequest {
        paths: device.video_nodes.clone(),
        seconds: request.seconds,
        warmup_frames: request.warmup_frames,
        width: None,
        height: None,
        fourcc: None,
        fps: None,
        min_fps,
        expect_frame_bytes,
        strict_nodes: false,
    }) {
        Ok(report) => Step {
            status: "ok",
            reason: None,
            value: Some(report),
        },
        Err(err) => Step {
            status: "failed",
            reason: Some(err.to_string()),
            value: None,
        },
    }
}
