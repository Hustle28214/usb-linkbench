//! Measurement library for usb-linkbench.
//!
//! Negotiated link rate, descriptor ceilings, control round-trip, video payload,
//! direct storage reads, and host-link state are separate results. None of them
//! is substituted for another.

mod budget;
mod clock;
mod identify;
mod links;
mod run;
mod stats;
mod storage;
mod stream;
pub mod text;
mod usb;

use serde::Serialize;

pub use budget::{BudgetReport, LinkSpeed, ModeBudget, preset, presets};
pub use clock::TimingReport;
pub use links::LinksReport;
pub use run::{RunReport, RunRequest};
pub use storage::{StorageOptions, StorageReport};
pub use stream::{CaptureRequest, StreamReport};
pub use usb::{
    AuditReport, BulkOptions, BulkReport, DeviceFilter, ListReport, RttOptions, RttReport,
};

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Message(String),
    #[error("没有找到符合条件的 USB 设备")]
    NotFound,
    #[error("I/O: {0}")]
    Io(#[from] std::io::Error),
    #[error("USB: {0}")]
    Usb(String),
    #[error("JSON: {0}")]
    Json(#[from] serde_json::Error),
}

pub fn list_devices(filter: &DeviceFilter) -> Result<ListReport, Error> {
    usb::list_devices(filter)
}

pub fn audit_one(filter: &DeviceFilter) -> Result<AuditReport, Error> {
    usb::audit(&usb::select_one(filter)?)
}

pub fn control_rtt(filter: &DeviceFilter, options: &RttOptions) -> Result<RttReport, Error> {
    usb::control_rtt(&usb::select_one(filter)?, options)
}

pub fn bulk_in(filter: &DeviceFilter, options: &BulkOptions) -> Result<BulkReport, Error> {
    usb::bulk_in(&usb::select_one(filter)?, options)
}

pub fn capture(request: &CaptureRequest) -> Result<StreamReport, Error> {
    stream::capture(request)
}

pub fn video_nodes_for(filter: &DeviceFilter) -> Result<Vec<String>, Error> {
    let report = usb::list_devices(filter)?;
    Ok(report
        .devices
        .into_iter()
        .flat_map(|device| device.video_nodes)
        .collect())
}

pub fn read_direct(options: &StorageOptions) -> Result<StorageReport, Error> {
    storage::read_direct(options)
}

pub fn links(verbose: bool) -> LinksReport {
    links::collect(verbose)
}

pub fn timing_check() -> TimingReport {
    clock::timing_check()
}

pub fn run_camera(request: &RunRequest) -> Result<RunReport, Error> {
    run::run(request)
}

pub fn budget_report(
    labels: Vec<String>,
    payloads: &[u64],
    highlight: Option<LinkSpeed>,
) -> BudgetReport {
    budget::budget_report(labels, payloads, highlight)
}

pub fn emit<T: Serialize>(json: bool, text: String, value: &T) -> Result<(), Error> {
    if json {
        println!("{}", serde_json::to_string_pretty(value)?);
    } else {
        print!("{text}");
        if !text.ends_with('\n') {
            println!();
        }
    }
    Ok(())
}
