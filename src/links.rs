//! Negotiated Thunderbolt and PCIe link state.
//!
//! These files report the link the hardware already trained. They are not
//! throughput tests. A missing sysfs tree is a skip, not a zero result.

use std::collections::BTreeMap;

use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
pub struct ThunderboltPort {
    pub path: String,
    pub name: Option<String>,
    pub vendor: Option<String>,
    pub generation: Option<String>,
    pub tx_speed_gbps: Option<f64>,
    pub rx_speed_gbps: Option<f64>,
    pub tx_lanes: Option<u32>,
    pub rx_lanes: Option<u32>,
    pub authorized: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct PcieFunction {
    pub address: String,
    pub vendor: Option<String>,
    pub device: Option<String>,
    pub current_speed: Option<String>,
    pub max_speed: Option<String>,
    pub current_width: Option<u32>,
    pub max_width: Option<u32>,
    pub degraded: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct LinksReport {
    pub measurement_kind: &'static str,
    pub thunderbolt: Vec<ThunderboltPort>,
    pub thunderbolt_skip_reason: Option<String>,
    pub pcie_degraded: Vec<PcieFunction>,
    pub pcie_speed_counts: BTreeMap<String, usize>,
    pub pcie_functions_with_link: usize,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub pcie_all: Vec<PcieFunction>,
    pub notes: Vec<String>,
}

pub fn collect(verbose: bool) -> LinksReport {
    #[cfg(target_os = "linux")]
    {
        linux::collect(verbose)
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = verbose;
        LinksReport {
            measurement_kind: "negotiated_link",
            thunderbolt: Vec::new(),
            thunderbolt_skip_reason: Some(
                "Thunderbolt 和 PCIe 协商速率目前只读 Linux sysfs。".into(),
            ),
            pcie_degraded: Vec::new(),
            pcie_speed_counts: BTreeMap::new(),
            pcie_functions_with_link: 0,
            pcie_all: Vec::new(),
            notes: vec!["这里没有吞吐测试。没有对端流量时不会编造带宽。".into()],
        }
    }
}

#[cfg(target_os = "linux")]
mod linux {
    use super::{LinksReport, PcieFunction, ThunderboltPort};
    use std::collections::BTreeMap;
    use std::fs;
    use std::path::Path;

    pub fn collect(verbose: bool) -> LinksReport {
        let (thunderbolt, thunderbolt_skip_reason) = thunderbolt();
        let functions = pcie();
        let mut pcie_speed_counts = BTreeMap::new();
        for function in &functions {
            if let Some(speed) = &function.current_speed {
                *pcie_speed_counts.entry(speed.clone()).or_insert(0) += 1;
            }
        }
        let pcie_degraded = functions
            .iter()
            .filter(|function| function.degraded)
            .cloned()
            .collect();
        LinksReport {
            measurement_kind: "negotiated_link",
            thunderbolt,
            thunderbolt_skip_reason,
            pcie_degraded,
            pcie_functions_with_link: functions.len(),
            pcie_speed_counts,
            pcie_all: if verbose { functions } else { Vec::new() },
            notes: vec![
                "Thunderbolt 的 tx_speed/rx_speed 和 PCIe 的 current_link_speed 都是已训练链路，不是吞吐。".into(),
                "degraded 表示当前速率或宽度低于该功能号声明的最大值。空闲链路常会降到 2.5 GT/s，这不一定是故障。".into(),
            ],
        }
    }

    fn thunderbolt() -> (Vec<ThunderboltPort>, Option<String>) {
        let root = Path::new("/sys/bus/thunderbolt/devices");
        let entries = match fs::read_dir(root) {
            Ok(entries) => entries,
            Err(err) => return (Vec::new(), Some(format!("没有 Thunderbolt sysfs（{err}）"))),
        };
        let mut ports = Vec::new();
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.is_dir() {
                continue;
            }
            let interesting = path.join("tx_speed").is_file()
                || path.join("device_name").is_file()
                || path.join("vendor_name").is_file();
            if !interesting {
                continue;
            }
            ports.push(ThunderboltPort {
                path: path.display().to_string(),
                name: read_text(&path.join("device_name")),
                vendor: read_text(&path.join("vendor_name")),
                generation: read_text(&path.join("generation")),
                tx_speed_gbps: read_f64(&path.join("tx_speed")),
                rx_speed_gbps: read_f64(&path.join("rx_speed")),
                tx_lanes: read_u32(&path.join("tx_lanes")),
                rx_lanes: read_u32(&path.join("rx_lanes")),
                authorized: read_text(&path.join("authorized")),
            });
        }
        ports.sort_by(|a, b| a.path.cmp(&b.path));
        (ports, None)
    }

    fn pcie() -> Vec<PcieFunction> {
        let root = Path::new("/sys/bus/pci/devices");
        let entries = match fs::read_dir(root) {
            Ok(entries) => entries,
            Err(_) => return Vec::new(),
        };
        let mut functions = Vec::new();
        for entry in entries.flatten() {
            let path = entry.path();
            let current_speed = read_text(&path.join("current_link_speed"));
            let current_width = read_u32(&path.join("current_link_width"));
            if current_speed.is_none() && current_width.is_none() {
                continue;
            }
            let max_speed = read_text(&path.join("max_link_speed"));
            let max_width = read_u32(&path.join("max_link_width"));
            let degraded = match (
                current_speed.as_deref(),
                max_speed.as_deref(),
                current_width,
                max_width,
            ) {
                (Some(current), Some(max), _, _) if current != max => true,
                (_, _, Some(current), Some(max)) if current < max => true,
                _ => false,
            };
            functions.push(PcieFunction {
                address: entry.file_name().to_string_lossy().to_string(),
                vendor: read_text(&path.join("vendor")),
                device: read_text(&path.join("device")),
                current_speed,
                max_speed,
                current_width,
                max_width,
                degraded,
            });
        }
        functions.sort_by(|a, b| a.address.cmp(&b.address));
        functions
    }

    fn read_text(path: &Path) -> Option<String> {
        let text = fs::read_to_string(path).ok()?;
        let trimmed = text.trim();
        if trimmed.is_empty() {
            None
        } else {
            Some(trimmed.to_string())
        }
    }

    fn read_f64(path: &Path) -> Option<f64> {
        read_text(path)?.parse().ok()
    }

    fn read_u32(path: &Path) -> Option<u32> {
        read_text(path)?.parse().ok()
    }
}
