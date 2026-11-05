//! Human-readable reports. JSON is produced from the same structs.

use crate::budget::{self, BudgetReport, LinkPlan};
use crate::clock::TimingReport;
use crate::links::LinksReport;
use crate::run::RunReport;
use crate::stats::{self, Summary};
use crate::storage::StorageReport;
use crate::stream::StreamReport;
use crate::usb::{AuditReport, BulkReport, DeviceRecord, ListReport, RttReport};

pub fn list(report: &ListReport) -> String {
    let mut out = format!("协商链路  {} 台设备\n", report.devices.len());
    if report.devices.is_empty() {
        out.push_str("没有匹配的 USB 设备。\n");
    }
    for device in &report.devices {
        out.push_str(&device_line(device));
        out.push('\n');
    }
    push_notes(&mut out, &report.notes);
    out
}

pub fn audit(report: &AuditReport) -> String {
    let mut out = format!(
        "描述符上限  {}\n活动配置里的周期端点最大上限: {}\n",
        device_line(&report.device),
        report
            .max_periodic_ceiling_bytes_per_sec
            .map(budget::fmt_bytes_per_sec_u)
            .unwrap_or_else(|| "无".into())
    );
    for config in &report.configurations {
        out.push_str(&format!(
            "配置 {}{}\n",
            config.value,
            if config.active { " (活动)" } else { "" }
        ));
        for iface in &config.interfaces {
            for alt in &iface.alternates {
                out.push_str(&format!(
                    "  接口 {} alt {} {}\n",
                    iface.number, alt.alternate, alt.class_name
                ));
                for ep in &alt.endpoints {
                    let ceiling = ep
                        .periodic_ceiling_bytes_per_sec
                        .map(budget::fmt_bytes_per_sec_u)
                        .unwrap_or_else(|| "-".into());
                    out.push_str(&format!(
                        "    {:#04x} {} {} 包 {} 间隔 {} 上限 {}\n",
                        ep.address,
                        ep.direction,
                        ep.transfer.as_str(),
                        ep.max_packet,
                        ep.interval,
                        ceiling
                    ));
                }
            }
        }
    }
    push_notes(&mut out, &report.notes);
    out
}

pub fn rtt(report: &RttReport) -> String {
    let mut out = format!(
        "控制回传  {}  {}\n预热丢弃 {}  超时 {} ms  失败 {}\n{}\n计时地板 p50 {}\n",
        report.request,
        device_line(&report.device),
        report.warmup_discarded,
        report.timeout_ms,
        report.errors,
        summary_line("样本", &report.samples_ns),
        stats::fmt_us(report.timer_overhead_ns.p50)
    );
    push_notes(&mut out, &report.notes);
    out
}

pub fn bulk(report: &BulkReport) -> String {
    let mut out = format!(
        "Bulk IN  {}  接口 {} 端点 {:#04x}\n{}  次数 {}  用时 {:.3} s  卸载驱动 {}\n",
        device_line(&report.device),
        report.interface,
        report.endpoint,
        budget::fmt_bytes_per_sec(report.bytes_per_sec),
        report.reads,
        report.elapsed_ns as f64 / 1e9,
        report.detached_kernel_driver
    );
    push_notes(&mut out, &report.notes);
    out
}

pub fn stream(report: &StreamReport) -> String {
    let mut out = format!(
        "视频载荷  合计 {}\n",
        budget::fmt_bytes_per_sec(report.aggregate_bytes_per_sec)
    );
    for item in &report.streams {
        out.push_str(&format!(
            "{}  {} {}x{} {}  {:.2} fps  {}  帧 {}  丢帧 {}  用时 {:.3} s\n",
            item.node,
            item.card,
            item.width,
            item.height,
            item.fourcc,
            item.window_fps,
            budget::fmt_bytes_per_sec(item.window_bytes_per_sec),
            item.frames,
            item.dropped_frames,
            item.elapsed_ns as f64 / 1e9
        ));
        if let Some(driver_fps) = item.driver_fps {
            out.push_str(&format!(
                "  驱动时间戳 {:.2} fps  间隔来源 {}\n",
                driver_fps, item.interval_source
            ));
        }
        if let Some(verdict) = &item.verdict {
            out.push_str(&format!(
                "  判定 fps {:?}  帧字节 {:?}\n",
                verdict.fps_ok, verdict.frame_bytes_ok
            ));
        }
    }
    for skipped in &report.skipped {
        out.push_str(&format!("跳过 {}  {}\n", skipped.node, skipped.reason));
    }
    push_notes(&mut out, &report.notes);
    out
}

pub fn storage(report: &StorageReport) -> String {
    let mut out = format!(
        "直接读  {}\n{}  字节 {}  用时 {:.3} s  圈数 {}\n",
        report.path,
        budget::fmt_bytes_per_sec(report.bytes_per_sec),
        report.bytes,
        report.elapsed_ns as f64 / 1e9,
        report.wraps
    );
    push_notes(&mut out, &report.notes);
    out
}

pub fn links(report: &LinksReport) -> String {
    let mut out = String::from("协商链路  Thunderbolt / PCIe\n");
    if let Some(reason) = &report.thunderbolt_skip_reason {
        out.push_str(&format!("Thunderbolt 跳过: {reason}\n"));
    }
    for port in &report.thunderbolt {
        out.push_str(&format!(
            "{}  {}  TX {} Gbit/s  RX {} Gbit/s  代 {}\n",
            port.path,
            port.name.as_deref().unwrap_or("-"),
            opt_f64(port.tx_speed_gbps),
            opt_f64(port.rx_speed_gbps),
            port.generation.as_deref().unwrap_or("-")
        ));
    }
    out.push_str(&format!(
        "PCIe 有链路的功能号 {}，降速 {} 个\n",
        report.pcie_functions_with_link,
        report.pcie_degraded.len()
    ));
    for (speed, count) in &report.pcie_speed_counts {
        out.push_str(&format!("  {speed}: {count}\n"));
    }
    for function in &report.pcie_degraded {
        out.push_str(&format!(
            "  降速 {} 当前 {} x{} 最大 {} x{}\n",
            function.address,
            function.current_speed.as_deref().unwrap_or("-"),
            opt_num(function.current_width),
            function.max_speed.as_deref().unwrap_or("-"),
            opt_num(function.max_width)
        ));
    }
    push_notes(&mut out, &report.notes);
    out
}

pub fn budget(report: &BudgetReport) -> String {
    let mut out = format!(
        "载荷预算  合计 {}\n",
        budget::fmt_bytes_per_sec_u(report.aggregate_bytes_per_sec)
    );
    for label in &report.labels {
        out.push_str(label);
        out.push('\n');
    }
    for plan in &report.plans {
        out.push_str(&plan_line(plan));
    }
    if let Some(plan) = &report.highlighted {
        out.push_str("指定链路: ");
        out.push_str(&plan_line(plan));
    }
    push_notes(&mut out, &report.notes);
    out
}

pub fn run(report: &RunReport) -> String {
    let mut out = format!(
        "相机测试  {} 台  检查{}\n",
        report.devices.len(),
        if report.checks_failed {
            "未通过"
        } else {
            "未发现失败项"
        }
    );
    for device in &report.devices {
        out.push_str(&device_line(&device.device));
        out.push('\n');
        out.push_str(&format!(
            "  描述符: {}\n",
            step_status(device.audit.status, &device.audit.reason)
        ));
        if let Some(audit) = &device.audit.value {
            out.push_str(&format!(
                "  周期端点上限 {}\n",
                audit
                    .max_periodic_ceiling_bytes_per_sec
                    .map(budget::fmt_bytes_per_sec_u)
                    .unwrap_or_else(|| "无".into())
            ));
        }
        out.push_str(&format!(
            "  回传: {}\n",
            step_status(device.rtt.status, &device.rtt.reason)
        ));
        if let Some(rtt) = &device.rtt.value {
            out.push_str(&format!(
                "  {}\n",
                summary_line("GET_STATUS", &rtt.samples_ns)
            ));
        }
        out.push_str(&format!(
            "  码流: {}\n",
            step_status(device.stream.status, &device.stream.reason)
        ));
        if let Some(stream) = &device.stream.value {
            out.push_str(&format!(
                "  合计 {}\n",
                budget::fmt_bytes_per_sec(stream.aggregate_bytes_per_sec)
            ));
            for item in &stream.streams {
                out.push_str(&format!(
                    "  {} {:.2} fps {}\n",
                    item.node,
                    item.window_fps,
                    budget::fmt_bytes_per_sec(item.window_bytes_per_sec)
                ));
            }
        }
        out.push_str(&format!(
            "  预设对描述符上限: {}\n",
            device.preset_vs_descriptor.as_str()
        ));
        if let Some(plan) = &device.planning.highlighted {
            out.push_str(&plan_line(plan));
        }
    }
    push_notes(&mut out, &report.notes);
    out
}

pub fn timing(report: &TimingReport) -> String {
    let mut out = format!(
        "计时检查  地板 p50 {}  睡眠请求 {} 实测 {}\n结果: {}\n",
        stats::fmt_us(report.timer_overhead_ns.p50),
        stats::fmt_us(report.sleep_requested_ns),
        stats::fmt_us(report.sleep_measured_ns),
        if report.sleep_ok { "通过" } else { "异常" }
    );
    push_notes(&mut out, &report.notes);
    out
}

fn device_line(device: &DeviceRecord) -> String {
    let name = device
        .product
        .clone()
        .or_else(|| device.catalog_name.clone())
        .unwrap_or_default();
    let family = device.family.as_deref().unwrap_or("-");
    format!(
        "{:04x}:{:04x} bus {} addr {}  {}  {}  {name}  视频节点 {}",
        device.vendor_id,
        device.product_id,
        device.bus_id,
        device.address,
        device.speed.as_str(),
        family,
        if device.video_nodes.is_empty() {
            "-".into()
        } else {
            device.video_nodes.join(",")
        }
    )
}

fn summary_line(label: &str, summary: &Summary) -> String {
    format!(
        "{label} n={} min {} p50 {} p95 {} p99 {} max {} mean {} stdev {}",
        summary.count,
        stats::fmt_us(summary.min),
        stats::fmt_us(summary.p50),
        stats::fmt_us(summary.p95),
        stats::fmt_us(summary.p99),
        stats::fmt_us(summary.max),
        stats::fmt_us_f(summary.mean),
        stats::fmt_us_f(summary.stdev)
    )
}

fn plan_line(plan: &LinkPlan) -> String {
    format!(
        "{}  合计相对总线编码上限 {}，上限 {}；最大一路相对单端点上限 {}，上限 {}\n",
        plan.speed.as_str(),
        plan.aggregate_vs_bus.as_str(),
        fit_detail(plan.line_coding_payload_bytes_per_sec),
        plan.largest_stream_vs_one_endpoint.as_str(),
        fit_detail(plan.default_one_isoc_endpoint_bytes_per_sec)
    )
}

fn fit_detail(ceiling: Option<u64>) -> String {
    ceiling
        .map(budget::fmt_bytes_per_sec_u)
        .unwrap_or_else(|| "-".into())
}

fn opt_f64(value: Option<f64>) -> String {
    value
        .map(|value| format!("{value}"))
        .unwrap_or_else(|| "-".into())
}

fn opt_num(value: Option<u32>) -> String {
    value
        .map(|value| value.to_string())
        .unwrap_or_else(|| "-".into())
}

fn step_status(status: &str, reason: &Option<String>) -> String {
    match reason {
        Some(reason) => format!("{status} ({reason})"),
        None => status.to_string(),
    }
}

fn push_notes(out: &mut String, notes: &[String]) {
    for note in notes {
        out.push_str("注: ");
        out.push_str(note);
        out.push('\n');
    }
}
