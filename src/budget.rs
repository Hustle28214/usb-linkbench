//! Payload budgets and spec ceilings.
//!
//! These numbers are planning limits taken from the USB specifications and from
//! uncompressed frame geometry. They are not measurements of a connected device.

use serde::Serialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum LinkSpeed {
    Low,
    Full,
    High,
    Super,
    SuperPlus10,
    SuperPlus20,
    Unknown,
}

impl LinkSpeed {
    pub fn from_nusb(speed: nusb::Speed) -> Self {
        match speed {
            nusb::Speed::Low => Self::Low,
            nusb::Speed::Full => Self::Full,
            nusb::Speed::High => Self::High,
            nusb::Speed::Super => Self::Super,
            nusb::Speed::SuperPlus => Self::SuperPlus10,
            _ => Self::Unknown,
        }
    }

    pub fn from_sysfs(text: &str) -> Option<Self> {
        match text.trim() {
            "1.5" => Some(Self::Low),
            "12" => Some(Self::Full),
            "480" => Some(Self::High),
            "5000" => Some(Self::Super),
            "10000" => Some(Self::SuperPlus10),
            "20000" => Some(Self::SuperPlus20),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Low => "low-speed 1.5 Mbit/s",
            Self::Full => "full-speed 12 Mbit/s",
            Self::High => "high-speed 480 Mbit/s",
            Self::Super => "super-speed 5 Gbit/s",
            Self::SuperPlus10 => "super-speed-plus 10 Gbit/s",
            Self::SuperPlus20 => "super-speed-plus 20 Gbit/s",
            Self::Unknown => "unknown",
        }
    }

    pub fn line_mbit_per_sec(self) -> Option<f64> {
        match self {
            Self::Low => Some(1.5),
            Self::Full => Some(12.0),
            Self::High => Some(480.0),
            Self::Super => Some(5_000.0),
            Self::SuperPlus10 => Some(10_000.0),
            Self::SuperPlus20 => Some(20_000.0),
            Self::Unknown => None,
        }
    }

    /// Payload bytes after the line code, before protocol and host overhead.
    ///
    /// USB 3.x SuperSpeed uses 8b/10b, so 5 Gbit/s of line rate is 4 Gbit/s of
    /// symbols. SuperSpeedPlus uses 128b/132b. USB 2.0 has no 8b/10b block; the
    /// value here is the raw line bit rate divided by eight.
    pub fn line_coding_payload_bytes_per_sec(self) -> Option<u64> {
        match self {
            Self::Low => Some(1_500_000 / 8),
            Self::Full => Some(12_000_000 / 8),
            Self::High => Some(480_000_000 / 8),
            Self::Super => Some(500_000_000),
            Self::SuperPlus10 => Some(encoded_128b132b_bytes_per_sec(10_000_000_000)),
            Self::SuperPlus20 => Some(encoded_128b132b_bytes_per_sec(20_000_000_000)),
            Self::Unknown => None,
        }
    }

    /// Default ceiling for a single periodic isochronous endpoint when the
    /// device descriptor does not provide a tighter number.
    ///
    /// Full-speed: 1023 bytes per 1 ms frame. High-speed high-bandwidth:
    /// 1024 * 3 bytes per 125 µs microframe. SuperSpeed: 1024 * 16 * 3 bytes
    /// per 125 µs service interval. SuperSpeedPlus can declare a larger
    /// `dwBytesPerInterval`; without that descriptor this stays at the
    /// SuperSpeed endpoint maximum.
    pub fn default_one_isoc_endpoint_bytes_per_sec(self) -> Option<u64> {
        match self {
            Self::Full => Some(1_023_000),
            Self::High => Some(1024 * 3 * 8_000),
            Self::Super | Self::SuperPlus10 | Self::SuperPlus20 => Some(1024 * 16 * 3 * 8_000),
            Self::Low | Self::Unknown => None,
        }
    }
}

fn encoded_128b132b_bytes_per_sec(line_bits_per_sec: u64) -> u64 {
    (u128::from(line_bits_per_sec) * 128 / 132 / 8) as u64
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum TransferKind {
    Control,
    Isochronous,
    Bulk,
    Interrupt,
}

impl TransferKind {
    pub fn from_nusb(kind: nusb::descriptors::TransferType) -> Self {
        match kind {
            nusb::descriptors::TransferType::Control => Self::Control,
            nusb::descriptors::TransferType::Isochronous => Self::Isochronous,
            nusb::descriptors::TransferType::Bulk => Self::Bulk,
            nusb::descriptors::TransferType::Interrupt => Self::Interrupt,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Control => "control",
            Self::Isochronous => "isochronous",
            Self::Bulk => "bulk",
            Self::Interrupt => "interrupt",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Fit {
    Fits,
    Tight,
    Exceeds,
    Unknown,
}

impl Fit {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Fits => "fits",
            Self::Tight => "tight",
            Self::Exceeds => "exceeds",
            Self::Unknown => "unknown",
        }
    }
}

pub fn fit_payload(payload_bytes_per_sec: u64, ceiling: Option<u64>) -> Fit {
    let Some(ceiling) = ceiling else {
        return Fit::Unknown;
    };
    if ceiling == 0 {
        return Fit::Unknown;
    }
    if payload_bytes_per_sec > ceiling {
        Fit::Exceeds
    } else if payload_bytes_per_sec.saturating_mul(10) > ceiling.saturating_mul(8) {
        Fit::Tight
    } else {
        Fit::Fits
    }
}

#[derive(Debug, Clone, Copy)]
pub struct PeriodicEndpoint {
    pub speed: LinkSpeed,
    pub kind: TransferKind,
    pub max_packet: u32,
    pub packets_per_microframe: u8,
    pub interval: u8,
    pub bytes_per_interval: Option<u32>,
    pub burst_packets: Option<u32>,
    pub mult_packets: Option<u32>,
}

/// Periodic endpoint ceiling in bytes per second.
///
/// For SuperSpeed and SuperSpeedPlus, a non-zero companion `bytes_per_interval`
/// is the spec's per-service-interval budget and already includes burst and mult.
/// Bulk endpoints are not periodic; this returns `None`.
pub fn periodic_ceiling_bytes_per_sec(endpoint: PeriodicEndpoint) -> Option<u64> {
    let PeriodicEndpoint {
        speed,
        kind,
        max_packet,
        packets_per_microframe,
        interval,
        bytes_per_interval,
        burst_packets,
        mult_packets,
    } = endpoint;
    if max_packet == 0
        || interval == 0
        || matches!(kind, TransferKind::Bulk | TransferKind::Control)
    {
        return None;
    }
    let services = services_per_sec(speed, kind, interval)?;
    let per_service = bytes_per_service(
        speed,
        kind,
        max_packet,
        packets_per_microframe,
        bytes_per_interval,
        burst_packets,
        mult_packets,
    )?;
    Some(per_service.saturating_mul(services))
}

fn services_per_sec(speed: LinkSpeed, kind: TransferKind, interval: u8) -> Option<u64> {
    match speed {
        LinkSpeed::Low => {
            if !matches!(kind, TransferKind::Interrupt) {
                return None;
            }
            Some(1_000 / u64::from(interval))
        }
        LinkSpeed::Full => Some(1_000 / u64::from(interval)),
        LinkSpeed::High | LinkSpeed::Super | LinkSpeed::SuperPlus10 | LinkSpeed::SuperPlus20 => {
            let shift = u32::from(interval.saturating_sub(1));
            if shift >= 16 {
                return None;
            }
            Some(8_000 / (1u64 << shift))
        }
        LinkSpeed::Unknown => None,
    }
}

fn bytes_per_service(
    speed: LinkSpeed,
    kind: TransferKind,
    max_packet: u32,
    packets_per_microframe: u8,
    bytes_per_interval: Option<u32>,
    burst_packets: Option<u32>,
    mult_packets: Option<u32>,
) -> Option<u64> {
    if matches!(
        speed,
        LinkSpeed::Super | LinkSpeed::SuperPlus10 | LinkSpeed::SuperPlus20
    ) {
        if let Some(bytes) = bytes_per_interval.filter(|bytes| *bytes > 0) {
            return Some(u64::from(bytes));
        }
        let burst = u64::from(burst_packets.unwrap_or(1).max(1));
        let mult = if matches!(kind, TransferKind::Isochronous) {
            u64::from(mult_packets.unwrap_or(1).max(1))
        } else {
            1
        };
        return Some(
            u64::from(max_packet)
                .saturating_mul(burst)
                .saturating_mul(mult),
        );
    }
    if matches!(speed, LinkSpeed::High) && matches!(kind, TransferKind::Isochronous) {
        return Some(u64::from(max_packet) * u64::from(packets_per_microframe.max(1)));
    }
    Some(u64::from(max_packet))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct ModeBudget {
    pub id: &'static str,
    pub family: &'static str,
    pub summary: &'static str,
    pub width: u32,
    pub height: u32,
    pub bytes_per_pixel: u32,
    pub fps: u32,
    pub fourcc: &'static str,
}

impl ModeBudget {
    pub fn payload_bytes_per_frame(self) -> u64 {
        u64::from(self.width) * u64::from(self.height) * u64::from(self.bytes_per_pixel)
    }

    pub fn payload_bytes_per_sec(self) -> u64 {
        self.payload_bytes_per_frame()
            .saturating_mul(u64::from(self.fps))
    }
}

pub const PRESETS: &[ModeBudget] = &[
    ModeBudget {
        id: "d435-depth-848x480-z16-90",
        family: "realsense",
        summary: "D435/D435i 深度 848x480 Z16 90 fps",
        width: 848,
        height: 480,
        bytes_per_pixel: 2,
        fps: 90,
        fourcc: "Z16 ",
    },
    ModeBudget {
        id: "d435-depth-1280x720-z16-30",
        family: "realsense",
        summary: "D435/D435i 深度 1280x720 Z16 30 fps",
        width: 1280,
        height: 720,
        bytes_per_pixel: 2,
        fps: 30,
        fourcc: "Z16 ",
    },
    ModeBudget {
        id: "d435-depth-640x480-z16-90",
        family: "realsense",
        summary: "D435/D435i 深度 640x480 Z16 90 fps",
        width: 640,
        height: 480,
        bytes_per_pixel: 2,
        fps: 90,
        fourcc: "Z16 ",
    },
    ModeBudget {
        id: "d435-color-1280x720-yuyv-30",
        family: "realsense",
        summary: "D435/D435i 彩色 1280x720 YUYV 30 fps",
        width: 1280,
        height: 720,
        bytes_per_pixel: 2,
        fps: 30,
        fourcc: "YUYV",
    },
    ModeBudget {
        id: "d435-color-1920x1080-yuyv-30",
        family: "realsense",
        summary: "D435/D435i 彩色 1920x1080 YUYV 30 fps",
        width: 1920,
        height: 1080,
        bytes_per_pixel: 2,
        fps: 30,
        fourcc: "YUYV",
    },
    ModeBudget {
        id: "d435-ir-1280x720-y8-30",
        family: "realsense",
        summary: "D435/D435i 红外 1280x720 Y8 30 fps",
        width: 1280,
        height: 720,
        bytes_per_pixel: 1,
        fps: 30,
        fourcc: "GREY",
    },
    ModeBudget {
        id: "d455-depth-848x480-z16-90",
        family: "realsense",
        summary: "D455 深度 848x480 Z16 90 fps",
        width: 848,
        height: 480,
        bytes_per_pixel: 2,
        fps: 90,
        fourcc: "Z16 ",
    },
    ModeBudget {
        id: "gemini2-depth-1280x800-y16-30",
        family: "orbbec",
        summary: "Gemini 2 深度 1280x800 Y16 30 fps",
        width: 1280,
        height: 800,
        bytes_per_pixel: 2,
        fps: 30,
        fourcc: "Y16 ",
    },
    ModeBudget {
        id: "gemini2-color-1280x720-yuyv-30",
        family: "orbbec",
        summary: "Gemini 2 彩色 1280x720 YUYV 30 fps",
        width: 1280,
        height: 720,
        bytes_per_pixel: 2,
        fps: 30,
        fourcc: "YUYV",
    },
    ModeBudget {
        id: "astra-color-640x480-yuyv-30",
        family: "orbbec",
        summary: "Astra 彩色 640x480 YUYV 30 fps",
        width: 640,
        height: 480,
        bytes_per_pixel: 2,
        fps: 30,
        fourcc: "YUYV",
    },
];

pub fn preset(id: &str) -> Option<&'static ModeBudget> {
    PRESETS.iter().find(|mode| mode.id == id)
}

pub fn presets() -> &'static [ModeBudget] {
    PRESETS
}

#[derive(Debug, Clone, Serialize)]
pub struct LinkPlan {
    pub speed: LinkSpeed,
    pub line_mbit_per_sec: Option<f64>,
    pub line_coding_payload_bytes_per_sec: Option<u64>,
    pub default_one_isoc_endpoint_bytes_per_sec: Option<u64>,
    pub aggregate_vs_bus: Fit,
    pub largest_stream_vs_one_endpoint: Fit,
}

pub fn plan_against(speed: LinkSpeed, payloads: &[u64]) -> LinkPlan {
    let aggregate = payloads.iter().copied().fold(0u64, u64::saturating_add);
    let largest = payloads.iter().copied().max().unwrap_or(0);
    LinkPlan {
        speed,
        line_mbit_per_sec: speed.line_mbit_per_sec(),
        line_coding_payload_bytes_per_sec: speed.line_coding_payload_bytes_per_sec(),
        default_one_isoc_endpoint_bytes_per_sec: speed.default_one_isoc_endpoint_bytes_per_sec(),
        aggregate_vs_bus: fit_payload(aggregate, speed.line_coding_payload_bytes_per_sec()),
        largest_stream_vs_one_endpoint: fit_payload(
            largest,
            speed.default_one_isoc_endpoint_bytes_per_sec(),
        ),
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct BudgetReport {
    pub measurement_kind: &'static str,
    pub labels: Vec<String>,
    pub aggregate_bytes_per_sec: u64,
    pub plans: Vec<LinkPlan>,
    pub highlighted: Option<LinkPlan>,
    pub notes: Vec<String>,
}

pub fn budget_report(
    labels: Vec<String>,
    payloads: &[u64],
    highlight: Option<LinkSpeed>,
) -> BudgetReport {
    BudgetReport {
        measurement_kind: "payload_budget",
        labels,
        aggregate_bytes_per_sec: payloads.iter().copied().fold(0, u64::saturating_add),
        plans: standard_plans(payloads),
        highlighted: highlight.map(|speed| plan_against(speed, payloads)),
        notes: vec![
            "这是未压缩帧字节乘帧率，不含压缩码流、UVC 包头和总线编码。".into(),
            "对照值是规范里的线路编码载荷和单路等时端点上限，不是实测。".into(),
            "tight 表示已经用到上限的 80% 以上。".into(),
        ],
    }
}

pub fn standard_plans(payloads: &[u64]) -> Vec<LinkPlan> {
    [
        LinkSpeed::Full,
        LinkSpeed::High,
        LinkSpeed::Super,
        LinkSpeed::SuperPlus10,
        LinkSpeed::SuperPlus20,
    ]
    .into_iter()
    .map(|speed| plan_against(speed, payloads))
    .collect()
}

pub fn fmt_bytes_per_sec(bytes_per_sec: f64) -> String {
    let mebibyte = bytes_per_sec / (1024.0 * 1024.0);
    let mbit = bytes_per_sec * 8.0 / 1_000_000.0;
    format!("{mebibyte:.2} MiB/s ({mbit:.1} Mbit/s)")
}

pub fn fmt_bytes_per_sec_u(bytes_per_sec: u64) -> String {
    fmt_bytes_per_sec(bytes_per_sec as f64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn superspeed_line_coding_is_500_mib_not_used() {
        assert_eq!(
            LinkSpeed::Super.line_coding_payload_bytes_per_sec(),
            Some(500_000_000)
        );
        assert_eq!(
            LinkSpeed::High.default_one_isoc_endpoint_bytes_per_sec(),
            Some(24_576_000)
        );
        assert_eq!(
            LinkSpeed::Super.default_one_isoc_endpoint_bytes_per_sec(),
            Some(393_216_000)
        );
    }

    #[test]
    fn high_speed_isoc_high_bandwidth_ceiling() {
        let ceiling = periodic_ceiling_bytes_per_sec(PeriodicEndpoint {
            speed: LinkSpeed::High,
            kind: TransferKind::Isochronous,
            max_packet: 1024,
            packets_per_microframe: 3,
            interval: 1,
            bytes_per_interval: None,
            burst_packets: None,
            mult_packets: None,
        });
        assert_eq!(ceiling, Some(24_576_000));
    }

    #[test]
    fn superspeed_companion_budget_uses_bytes_per_interval() {
        let ceiling = periodic_ceiling_bytes_per_sec(PeriodicEndpoint {
            speed: LinkSpeed::Super,
            kind: TransferKind::Isochronous,
            max_packet: 1024,
            packets_per_microframe: 1,
            interval: 1,
            bytes_per_interval: Some(48 * 1024),
            burst_packets: Some(16),
            mult_packets: Some(3),
        });
        assert_eq!(ceiling, Some(48 * 1024 * 8_000));
    }

    #[test]
    fn bulk_has_no_periodic_ceiling() {
        assert_eq!(
            periodic_ceiling_bytes_per_sec(PeriodicEndpoint {
                speed: LinkSpeed::Super,
                kind: TransferKind::Bulk,
                max_packet: 1024,
                packets_per_microframe: 1,
                interval: 1,
                bytes_per_interval: None,
                burst_packets: Some(16),
                mult_packets: None,
            }),
            None
        );
    }

    #[test]
    fn uncompressed_frame_budget_is_exact() {
        let mode = preset("d435-depth-848x480-z16-90").unwrap();
        assert_eq!(mode.payload_bytes_per_frame(), 848 * 480 * 2);
        assert_eq!(mode.payload_bytes_per_sec(), 848 * 480 * 2 * 90);
    }

    #[test]
    fn yuyv_1080p30_exceeds_usb2_isoc_and_fits_usb3() {
        let mode = preset("d435-color-1920x1080-yuyv-30").unwrap();
        let payload = mode.payload_bytes_per_sec();
        assert_eq!(
            fit_payload(
                payload,
                LinkSpeed::High.default_one_isoc_endpoint_bytes_per_sec()
            ),
            Fit::Exceeds
        );
        assert_eq!(
            fit_payload(
                payload,
                LinkSpeed::Super.default_one_isoc_endpoint_bytes_per_sec()
            ),
            Fit::Fits
        );
    }

    #[test]
    fn sysfs_speed_distinguishes_20_gbit() {
        assert_eq!(LinkSpeed::from_sysfs("20000"), Some(LinkSpeed::SuperPlus20));
        assert_eq!(LinkSpeed::from_sysfs("10000"), Some(LinkSpeed::SuperPlus10));
        assert_eq!(LinkSpeed::from_sysfs("1.5"), Some(LinkSpeed::Low));
    }
}
