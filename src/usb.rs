//! USB enumeration, descriptor ceilings, control round-trip, and bulk reads.
//!
//! Control round-trip samples are one GET_STATUS transfer each. The clock is
//! read immediately before submission and immediately after completion. Warmup
//! samples are discarded. Timer overhead is reported beside the samples and is
//! not subtracted.

use std::time::Duration;

pub use nusb::DeviceInfo;
use nusb::descriptors::EndpointDescriptor;
use nusb::transfer::{Bulk, ControlIn, ControlType, Direction, In, Recipient};
use nusb::{Device, MaybeFuture};
use serde::Serialize;

use crate::Error;
use crate::budget::{LinkSpeed, PeriodicEndpoint, TransferKind, periodic_ceiling_bytes_per_sec};
use crate::clock;
use crate::identify::{self, CameraIdentity};
use crate::stats::{self, Summary};
use crate::stream;

#[derive(Debug, Clone)]
pub struct DeviceFilter {
    pub vendor_id: Option<u16>,
    pub product_id: Option<u16>,
    pub bus_id: Option<String>,
    pub address: Option<u8>,
    pub cameras_only: bool,
}

impl DeviceFilter {
    pub fn is_unrestricted(&self) -> bool {
        self.vendor_id.is_none()
            && self.product_id.is_none()
            && self.bus_id.is_none()
            && self.address.is_none()
            && !self.cameras_only
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct DeviceRecord {
    pub vendor_id: u16,
    pub product_id: u16,
    pub bus_id: String,
    pub address: u8,
    pub manufacturer: Option<String>,
    pub product: Option<String>,
    pub serial: Option<String>,
    pub usb_bcd: String,
    pub device_bcd: String,
    pub class_name: String,
    pub speed: LinkSpeed,
    pub speed_source: String,
    pub line_mbit_per_sec: Option<f64>,
    pub controller: Option<String>,
    pub family: Option<String>,
    pub catalog_name: Option<String>,
    pub has_video_class: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sysfs_path: Option<String>,
    pub video_nodes: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ListReport {
    pub measurement_kind: &'static str,
    pub devices: Vec<DeviceRecord>,
    pub notes: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct EndpointRecord {
    pub address: u8,
    pub direction: &'static str,
    pub transfer: TransferKind,
    pub max_packet: u32,
    pub packets_per_microframe: u8,
    pub interval: u8,
    pub burst_packets: Option<u32>,
    pub mult_packets: Option<u32>,
    pub bytes_per_interval: Option<u32>,
    pub periodic_ceiling_bytes_per_sec: Option<u64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct AltRecord {
    pub alternate: u8,
    pub class_name: String,
    pub endpoints: Vec<EndpointRecord>,
}

#[derive(Debug, Clone, Serialize)]
pub struct InterfaceRecord {
    pub number: u8,
    pub alternates: Vec<AltRecord>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ConfigRecord {
    pub value: u8,
    pub active: bool,
    pub interfaces: Vec<InterfaceRecord>,
}

#[derive(Debug, Clone, Serialize)]
pub struct AuditReport {
    pub measurement_kind: &'static str,
    pub device: DeviceRecord,
    pub configurations: Vec<ConfigRecord>,
    pub max_periodic_ceiling_bytes_per_sec: Option<u64>,
    pub notes: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct RttReport {
    pub measurement_kind: &'static str,
    pub device: DeviceRecord,
    pub request: &'static str,
    pub warmup_discarded: u32,
    pub timeout_ms: u64,
    pub samples_ns: Summary,
    pub timer_overhead_ns: Summary,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sample_ns: Option<Vec<u64>>,
    pub errors: u32,
    pub notes: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct BulkReport {
    pub measurement_kind: &'static str,
    pub device: DeviceRecord,
    pub interface: u8,
    pub endpoint: u8,
    pub queue_depth: u32,
    pub detached_kernel_driver: bool,
    pub warmup_discarded: u32,
    pub reads: u64,
    pub bytes: u64,
    pub elapsed_ns: u64,
    pub bytes_per_sec: f64,
    pub timer_overhead_ns: Summary,
    pub notes: Vec<String>,
}

pub struct RttOptions {
    pub count: u32,
    pub warmup: u32,
    pub timeout: Duration,
    pub keep_samples: bool,
}

pub struct BulkOptions {
    pub interface: u8,
    pub endpoint: u8,
    pub seconds: f64,
    pub buffer_bytes: usize,
    pub warmup: u32,
    pub timeout: Duration,
    pub detach: bool,
}

fn notes_link() -> Vec<String> {
    vec![
        "speed 是操作系统报告的协商链路速率，不是吞吐。".into(),
        "Linux sysfs 的 speed 能区分 10 Gbit/s 和 20 Gbit/s；nusb 的 SuperPlus 只表示 10 Gbit/s。"
            .into(),
    ]
}

fn notes_ceiling() -> Vec<String> {
    vec![
        "periodic_ceiling_bytes_per_sec 由端点描述符和服务间隔公式算出，不是实测吞吐。".into(),
        "Bulk 端点没有服务间隔预算。相机视频通常走等时端点，用 stream 测载荷。".into(),
        "未推流时生效的多是 alternate 0；推流后驱动会改选带宽更大的 alternate。".into(),
    ]
}

fn notes_rtt() -> Vec<String> {
    vec![
        "每个样本是一次 GET_STATUS，区间从提交前到完成，包含一次系统调用。".into(),
        "预热样本已丢弃。计时开销单独列出，不从样本扣除。".into(),
        "超时和 STALL 计入 errors，不进入分位数。".into(),
    ]
}

pub fn list_devices(filter: &DeviceFilter) -> Result<ListReport, Error> {
    let controllers = controller_map()?;
    let nodes = stream::video_nodes();
    let mut devices = Vec::new();
    for info in connected()? {
        if !matches_filter(&info, filter) {
            continue;
        }
        devices.push(record_from(&info, &controllers, &nodes));
    }
    devices.sort_by(|a, b| (&a.bus_id, a.address).cmp(&(&b.bus_id, b.address)));
    Ok(ListReport {
        measurement_kind: "negotiated_link",
        devices,
        notes: notes_link(),
    })
}

pub fn select_one(filter: &DeviceFilter) -> Result<DeviceInfo, Error> {
    let matched: Vec<_> = connected()?
        .into_iter()
        .filter(|info| matches_filter(info, filter))
        .collect();
    match matched.len() {
        0 => Err(Error::NotFound),
        1 => Ok(matched.into_iter().next().expect("len checked")),
        n => Err(Error::Message(format!(
            "匹配到 {n} 个设备，请用 --vid、--pid、--bus 或 --addr 收窄"
        ))),
    }
}

pub fn select_many(filter: &DeviceFilter) -> Result<Vec<DeviceInfo>, Error> {
    let matched: Vec<_> = connected()?
        .into_iter()
        .filter(|info| matches_filter(info, filter))
        .collect();
    if matched.is_empty() {
        return Err(Error::NotFound);
    }
    Ok(matched)
}

pub fn audit(info: &DeviceInfo) -> Result<AuditReport, Error> {
    let device = open_device(info)?;
    let record = record_from(info, &controller_map()?, &stream::video_nodes());
    let speed = record.speed;
    let active = device
        .active_configuration()
        .ok()
        .map(|config| config.configuration_value());
    let mut configurations = Vec::new();
    let mut max_ceiling = None;
    for config in device.configurations() {
        let mut interfaces = Vec::new();
        for iface in config.interfaces() {
            let mut alternates = Vec::new();
            for alt in iface.alt_settings() {
                let mut endpoints = Vec::new();
                for ep in alt.endpoints() {
                    let record = endpoint_record(speed, &ep);
                    if let Some(ceiling) = record.periodic_ceiling_bytes_per_sec {
                        max_ceiling = Some(max_ceiling.unwrap_or(0).max(ceiling));
                    }
                    endpoints.push(record);
                }
                alternates.push(AltRecord {
                    alternate: alt.alternate_setting(),
                    class_name: identify::class_name(alt.class(), alt.subclass()).to_string(),
                    endpoints,
                });
            }
            interfaces.push(InterfaceRecord {
                number: iface.interface_number(),
                alternates,
            });
        }
        configurations.push(ConfigRecord {
            value: config.configuration_value(),
            active: Some(config.configuration_value()) == active,
            interfaces,
        });
    }
    Ok(AuditReport {
        measurement_kind: "descriptor_ceiling",
        device: record,
        configurations,
        max_periodic_ceiling_bytes_per_sec: max_ceiling,
        notes: notes_ceiling(),
    })
}

pub fn control_rtt(info: &DeviceInfo, options: &RttOptions) -> Result<RttReport, Error> {
    if options.count == 0 {
        return Err(Error::Message("count 必须大于 0".into()));
    }
    let device = open_device(info)?;
    let record = record_from(info, &controller_map()?, &stream::video_nodes());
    let request = get_status_request();
    let mut samples = Vec::with_capacity(options.count as usize);
    let mut errors = 0u32;

    #[cfg(not(windows))]
    {
        for index in 0..options.warmup + options.count {
            let (elapsed, result) =
                time_control(|| device.control_in(request, options.timeout).wait());
            record_rtt_sample(
                index,
                options.warmup,
                elapsed,
                result,
                &mut samples,
                &mut errors,
            );
        }
    }

    #[cfg(windows)]
    {
        let interface = claim_any_interface(&device)?;
        for index in 0..options.warmup + options.count {
            let (elapsed, result) =
                time_control(|| interface.control_in(request, options.timeout).wait());
            record_rtt_sample(
                index,
                options.warmup,
                elapsed,
                result,
                &mut samples,
                &mut errors,
            );
        }
    }

    let summary = stats::summarize(&samples)
        .ok_or_else(|| Error::Message(format!("没有成功的 GET_STATUS 样本，失败 {errors} 次")))?;
    Ok(RttReport {
        measurement_kind: "wire_round_trip",
        device: record,
        request: "GET_STATUS",
        warmup_discarded: options.warmup,
        timeout_ms: options.timeout.as_millis() as u64,
        samples_ns: summary,
        timer_overhead_ns: timer_floor(),
        sample_ns: options.keep_samples.then_some(samples),
        errors,
        notes: notes_rtt(),
    })
}

pub fn bulk_in(info: &DeviceInfo, options: &BulkOptions) -> Result<BulkReport, Error> {
    if options.seconds <= 0.0 || options.buffer_bytes == 0 {
        return Err(Error::Message("秒数和缓冲区必须大于 0".into()));
    }
    if options.buffer_bytes > 16 * 1024 * 1024 {
        return Err(Error::Message("缓冲区最大 16 MiB".into()));
    }
    let device = open_device(info)?;
    let record = record_from(info, &controller_map()?, &stream::video_nodes());
    ensure_bulk_in(&device, options.interface, options.endpoint)?;

    let mut reattach = false;
    if options.detach {
        device
            .detach_kernel_driver(options.interface)
            .map_err(|err| Error::Usb(format!("卸载内核驱动失败: {err}")))?;
        reattach = true;
        eprintln!(
            "已卸载接口 {} 的内核驱动，测试结束后会尝试挂回。",
            options.interface
        );
    }
    let interface = device.claim_interface(options.interface).wait().map_err(|err| {
        if reattach {
            let _ = device.attach_kernel_driver(options.interface);
        }
        Error::Usb(format!(
            "无法占用接口 {}: {err}。相机视频接口通常由内核占用，请改用 stream；只有确认要独占设备时才加 --detach",
            options.interface
        ))
    })?;

    let mut endpoint = interface
        .endpoint::<Bulk, In>(options.endpoint)
        .map_err(|err| Error::Usb(err.to_string()))?;

    for _ in 0..options.warmup {
        let buffer = endpoint.allocate(options.buffer_bytes);
        let completion = endpoint.transfer_blocking(buffer, options.timeout);
        if let Err(err) = completion.status {
            finish_bulk(&device, options.interface, reattach);
            return Err(Error::Usb(format!("预热传输失败: {err}")));
        }
    }

    const DEPTH: usize = 4;
    let window_ns = (options.seconds * 1_000_000_000.0) as u64;
    let started = clock::now_ns();
    for _ in 0..DEPTH {
        let buffer = endpoint.allocate(options.buffer_bytes);
        endpoint.submit(buffer);
    }
    let mut bytes = 0u64;
    let mut reads = 0u64;
    while clock::elapsed_ns(started) < window_ns {
        let Some(completion) = endpoint.wait_next_complete(options.timeout) else {
            break;
        };
        if completion.status.is_err() {
            break;
        }
        bytes += completion.actual_len as u64;
        reads += 1;
        if clock::elapsed_ns(started) < window_ns {
            endpoint.submit(completion.buffer);
        }
    }
    let elapsed_ns = clock::elapsed_ns(started);
    endpoint.cancel_all();
    while endpoint.pending() > 0 {
        if endpoint
            .wait_next_complete(Duration::from_secs(1))
            .is_none()
        {
            break;
        }
    }
    finish_bulk(&device, options.interface, reattach);

    if reads == 0 || elapsed_ns == 0 {
        return Err(Error::Message(
            "Bulk IN 没有在超时前返回数据。这个端点可能不是持续的数据源。".into(),
        ));
    }
    let mut notes = vec![
        "字节数只计已完成的传输。结束时仍在途的传输已取消，不计入。".into(),
        "队列深度为 4。分母是这段传输的实测墙钟。".into(),
        "这不是 UVC 等时视频的载荷。相机码流请用 stream。".into(),
    ];
    if options.detach {
        notes.push("本次卸载过内核驱动，并已请求重新挂接。".into());
    }
    Ok(BulkReport {
        measurement_kind: "bulk_in_goodput",
        device: record,
        interface: options.interface,
        endpoint: options.endpoint,
        queue_depth: DEPTH as u32,
        detached_kernel_driver: options.detach,
        warmup_discarded: options.warmup,
        reads,
        bytes,
        elapsed_ns,
        bytes_per_sec: bytes as f64 / (elapsed_ns as f64 / 1e9),
        timer_overhead_ns: timer_floor(),
        notes,
    })
}

fn finish_bulk(device: &Device, interface: u8, reattach: bool) {
    if reattach && let Err(err) = device.attach_kernel_driver(interface) {
        eprintln!("重新挂接内核驱动失败: interface {interface}: {err}");
    }
}

fn ensure_bulk_in(device: &Device, interface: u8, endpoint: u8) -> Result<(), Error> {
    let mut found = false;
    for config in device.configurations() {
        for iface in config.interfaces() {
            if iface.interface_number() != interface {
                continue;
            }
            for alt in iface.alt_settings() {
                for ep in alt.endpoints() {
                    if ep.address() == endpoint {
                        found = true;
                        if ep.direction() != Direction::In
                            || TransferKind::from_nusb(ep.transfer_type()) != TransferKind::Bulk
                        {
                            return Err(Error::Message(format!(
                                "端点 {endpoint:#04x} 不是 Bulk IN（实际 {} {:?}）。视频载荷请用 stream",
                                direction_name(ep.direction()),
                                ep.transfer_type()
                            )));
                        }
                    }
                }
            }
        }
    }
    if !found {
        return Err(Error::Message(format!(
            "接口 {interface} 上没有端点 {endpoint:#04x}"
        )));
    }
    Ok(())
}

fn endpoint_record(speed: LinkSpeed, ep: &EndpointDescriptor<'_>) -> EndpointRecord {
    let (bytes_per_interval, burst_packets, mult_packets) = companion(ep);
    let kind = TransferKind::from_nusb(ep.transfer_type());
    let ceiling = periodic_ceiling_bytes_per_sec(PeriodicEndpoint {
        speed,
        kind,
        max_packet: ep.max_packet_size() as u32,
        packets_per_microframe: ep.packets_per_microframe(),
        interval: ep.interval(),
        bytes_per_interval,
        burst_packets,
        mult_packets,
    });
    EndpointRecord {
        address: ep.address(),
        direction: direction_name(ep.direction()),
        transfer: kind,
        max_packet: ep.max_packet_size() as u32,
        packets_per_microframe: ep.packets_per_microframe(),
        interval: ep.interval(),
        burst_packets,
        mult_packets,
        bytes_per_interval,
        periodic_ceiling_bytes_per_sec: ceiling,
    }
}

fn companion(ep: &EndpointDescriptor<'_>) -> (Option<u32>, Option<u32>, Option<u32>) {
    let mut bytes_per_interval = None;
    let mut burst_packets = None;
    let mut mult_packets = None;
    let isoc = matches!(
        ep.transfer_type(),
        nusb::descriptors::TransferType::Isochronous
    );
    for desc in ep.descriptors() {
        let raw = desc.as_bytes();
        match desc.descriptor_type() {
            0x30 if raw.len() >= 6 => {
                burst_packets = Some(u32::from(raw[2]) + 1);
                if isoc {
                    mult_packets = Some(u32::from(raw[3] & 0x03) + 1);
                }
                let declared = u32::from(u16::from_le_bytes([raw[4], raw[5]]));
                if declared > 0 {
                    bytes_per_interval = Some(declared);
                }
            }
            0x31 if raw.len() >= 8 => {
                let declared = u32::from_le_bytes([raw[4], raw[5], raw[6], raw[7]]);
                if declared > 0 {
                    bytes_per_interval = Some(declared);
                }
            }
            _ => {}
        }
    }
    (bytes_per_interval, burst_packets, mult_packets)
}

fn direction_name(direction: Direction) -> &'static str {
    match direction {
        Direction::In => "in",
        Direction::Out => "out",
    }
}

fn get_status_request() -> ControlIn {
    ControlIn {
        control_type: ControlType::Standard,
        recipient: Recipient::Device,
        request: 0x00,
        value: 0,
        index: 0,
        length: 2,
    }
}

fn time_control<T>(submit: impl FnOnce() -> T) -> (u64, T) {
    let started = clock::now_ns();
    let result = submit();
    (clock::now_ns().saturating_sub(started), result)
}

fn record_rtt_sample(
    index: u32,
    warmup: u32,
    elapsed: u64,
    result: Result<Vec<u8>, nusb::transfer::TransferError>,
    samples: &mut Vec<u64>,
    errors: &mut u32,
) {
    if index < warmup {
        return;
    }
    match result {
        Ok(data) if data.len() == 2 => samples.push(elapsed),
        _ => *errors += 1,
    }
}

#[cfg(windows)]
fn claim_any_interface(device: &Device) -> Result<nusb::Interface, Error> {
    let mut numbers = Vec::new();
    for config in device.configurations() {
        for iface in config.interfaces() {
            let number = iface.interface_number();
            if !numbers.contains(&number) {
                numbers.push(number);
            }
        }
    }
    let mut last = None;
    for number in numbers {
        match device.claim_interface(number).wait() {
            Ok(interface) => return Ok(interface),
            Err(err) => last = Some(err.to_string()),
        }
    }
    Err(Error::Message(format!(
        "Windows 上 GET_STATUS 需要占用一个接口，但接口都被驱动占用（{}）。不把描述符缓存当作线级回传。",
        last.unwrap_or_else(|| "没有可占用的接口".into())
    )))
}

fn timer_floor() -> Summary {
    stats::summarize(&clock::overhead_samples(200)).expect("overhead samples are non-empty")
}

fn connected() -> Result<Vec<DeviceInfo>, Error> {
    nusb::list_devices()
        .wait()
        .map(|iter| iter.collect())
        .map_err(|err| Error::Usb(err.to_string()))
}

fn open_device(info: &DeviceInfo) -> Result<Device, Error> {
    info.open().wait().map_err(|err| {
        Error::Usb(format!(
            "打开 {:04x}:{:04x} 失败: {err}。Linux 上需要 /dev/bus/usb 的写权限，可参考 contrib/60-usb-linkbench.rules",
            info.vendor_id(),
            info.product_id()
        ))
    })
}

fn controller_map() -> Result<Vec<(String, String)>, Error> {
    let buses = nusb::list_buses()
        .wait()
        .map_err(|err| Error::Usb(err.to_string()))?;
    let mut out = Vec::new();
    for bus in buses {
        if let Some(kind) = bus.controller_type() {
            out.push((bus.bus_id().to_string(), format!("{kind:?}")));
        }
    }
    Ok(out)
}

fn matches_filter(info: &DeviceInfo, filter: &DeviceFilter) -> bool {
    if filter.vendor_id.is_some_and(|vid| info.vendor_id() != vid) {
        return false;
    }
    if filter
        .product_id
        .is_some_and(|pid| info.product_id() != pid)
    {
        return false;
    }
    if filter
        .bus_id
        .as_ref()
        .is_some_and(|bus| info.bus_id() != bus)
    {
        return false;
    }
    if filter
        .address
        .is_some_and(|addr| info.device_address() != addr)
    {
        return false;
    }
    if filter.cameras_only && camera_of(info).is_none() {
        return false;
    }
    true
}

pub fn describe_device(info: &DeviceInfo) -> Result<DeviceRecord, Error> {
    Ok(record_from(
        info,
        &controller_map()?,
        &stream::video_nodes(),
    ))
}

pub fn record_from(
    info: &DeviceInfo,
    controllers: &[(String, String)],
    nodes: &[stream::VideoNode],
) -> DeviceRecord {
    let (speed, speed_source) = speed_of(info);
    let camera = camera_of(info);
    let video_nodes = nodes
        .iter()
        .filter(|node| {
            node.vendor_id == Some(info.vendor_id()) && node.product_id == Some(info.product_id())
        })
        .map(|node| node.path.clone())
        .collect();
    DeviceRecord {
        vendor_id: info.vendor_id(),
        product_id: info.product_id(),
        bus_id: info.bus_id().to_string(),
        address: info.device_address(),
        manufacturer: info.manufacturer_string().map(str::to_string),
        product: info.product_string().map(str::to_string),
        serial: info.serial_number().map(str::to_string),
        usb_bcd: format_bcd(info.usb_version()),
        device_bcd: format_bcd(info.device_version()),
        class_name: identify::class_name(info.class(), info.subclass()).to_string(),
        speed,
        speed_source: speed_source.to_string(),
        line_mbit_per_sec: speed.line_mbit_per_sec(),
        controller: controllers
            .iter()
            .find(|(bus, _)| bus == info.bus_id())
            .map(|(_, kind)| kind.clone()),
        family: camera.map(|item| item.family.to_string()),
        catalog_name: camera.and_then(|item| item.catalog_name.map(str::to_string)),
        has_video_class: info.interfaces().any(|iface| iface.class() == 0x0E),
        sysfs_path: sysfs_path(info),
        video_nodes,
    }
}

fn camera_of(info: &DeviceInfo) -> Option<CameraIdentity> {
    identify::identify_camera(info.vendor_id(), info.product_id(), info.product_string())
}

fn format_bcd(value: u16) -> String {
    format!("{:x}.{:02x}", value >> 8, value & 0xff)
}

fn speed_of(info: &DeviceInfo) -> (LinkSpeed, &'static str) {
    if let Some(speed) = sysfs_speed(info) {
        return (speed, "sysfs");
    }
    match info.speed() {
        Some(speed) => (LinkSpeed::from_nusb(speed), "nusb"),
        None => (LinkSpeed::Unknown, "unavailable"),
    }
}

#[cfg(target_os = "linux")]
fn sysfs_speed(info: &DeviceInfo) -> Option<LinkSpeed> {
    let text = std::fs::read_to_string(info.sysfs_path().join("speed")).ok()?;
    LinkSpeed::from_sysfs(&text)
}

#[cfg(not(target_os = "linux"))]
fn sysfs_speed(_info: &DeviceInfo) -> Option<LinkSpeed> {
    None
}

#[cfg(target_os = "linux")]
fn sysfs_path(info: &DeviceInfo) -> Option<String> {
    Some(info.sysfs_path().display().to_string())
}

#[cfg(not(target_os = "linux"))]
fn sysfs_path(_info: &DeviceInfo) -> Option<String> {
    None
}
