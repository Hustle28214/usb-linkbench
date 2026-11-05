use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Duration;

use clap::{Args, Parser, Subcommand};
use usb_linkbench::{
    BulkOptions, CaptureRequest, DeviceFilter, Error, LinkSpeed, ModeBudget, RttOptions,
    RunRequest, StorageOptions, budget_report, preset, presets, text,
};

#[derive(Parser)]
#[command(
    name = "usb-linkbench",
    version,
    about = "相机 USB 载荷、控制回传，以及高速链路协商测试"
)]
struct Cli {
    /// 机器可读 JSON 写到 stdout
    #[arg(long, global = true)]
    json: bool,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// 列出协商链路速率
    List {
        #[command(flatten)]
        filter: FilterArgs,
    },
    /// 用描述符计算周期端点上限
    Audit {
        #[command(flatten)]
        filter: FilterArgs,
    },
    /// 测量 GET_STATUS 控制传输回传
    Rtt {
        #[command(flatten)]
        filter: FilterArgs,
        /// 计入统计的样本数
        #[arg(long, default_value_t = 100)]
        count: u32,
        /// 丢弃的预热样本数
        #[arg(long, default_value_t = 20)]
        warmup: u32,
        /// 单次传输超时
        #[arg(long, default_value_t = 200)]
        timeout_ms: u64,
        /// 在 JSON 里保留每个样本
        #[arg(long)]
        samples: bool,
    },
    /// 统计 V4L2 驱动交出的帧字节
    Stream {
        #[command(flatten)]
        filter: FilterArgs,
        /// 例如 /dev/video0。可重复。
        #[arg(long)]
        device: Vec<String>,
        #[arg(long, default_value_t = 5.0)]
        seconds: f64,
        #[arg(long, default_value_t = 30)]
        warmup_frames: u32,
        #[arg(long)]
        width: Option<u32>,
        #[arg(long)]
        height: Option<u32>,
        /// 4 字符像素格式，不足补空格，例如 YUYV 或 "Z16 "
        #[arg(long)]
        fourcc: Option<String>,
        #[arg(long)]
        fps: Option<u32>,
        #[arg(long)]
        min_fps: Option<f64>,
        #[arg(long)]
        expect_frame_bytes: Option<u64>,
        /// 未压缩预设。只有一路视频且只给一个预设时，用它填写帧率和帧字节判定。
        #[arg(long)]
        preset: Vec<String>,
    },
    /// 计算未压缩帧预算，并对照各档链路的规范上限
    Budget {
        #[arg(long)]
        preset: Vec<String>,
        #[arg(long)]
        width: Option<u32>,
        #[arg(long)]
        height: Option<u32>,
        #[arg(long)]
        bpp: Option<u32>,
        #[arg(long)]
        fps: Option<u32>,
        /// 480、5000、10000、20000，或 high / super / superplus10 / superplus20
        #[arg(long)]
        speed: Option<String>,
    },
    /// 绕过页缓存的顺序读
    Storage {
        #[arg(long)]
        path: PathBuf,
        #[arg(long, default_value_t = 5.0)]
        seconds: f64,
        /// 块大小，KiB，必须能被 4 整除
        #[arg(long, default_value_t = 1024)]
        block_kib: u32,
        #[arg(long, default_value_t = 4)]
        warmup_mib: u32,
    },
    /// 读 Bulk IN。默认不卸载内核驱动。
    Bulk {
        #[command(flatten)]
        filter: FilterArgs,
        #[arg(long)]
        interface: u8,
        /// 端点地址，例如 0x81
        #[arg(long)]
        endpoint: String,
        #[arg(long, default_value_t = 2.0)]
        seconds: f64,
        #[arg(long, default_value_t = 64)]
        buf_kib: u32,
        #[arg(long, default_value_t = 4)]
        warmup: u32,
        #[arg(long, default_value_t = 500)]
        timeout_ms: u64,
        /// 测试前卸载该接口的内核驱动，结束时挂回
        #[arg(long)]
        detach: bool,
    },
    /// 读取 Thunderbolt 和 PCIe 已训练链路
    Links {
        /// 列出全部 PCIe 功能号，而不只是降速项
        #[arg(long)]
        verbose: bool,
    },
    /// 检查单调时钟的地板和一次睡眠
    TimingCheck,
    /// 对匹配的相机依次做描述符、回传和码流
    Run {
        #[command(flatten)]
        filter: FilterArgs,
        #[arg(long, default_value_t = 5.0)]
        seconds: f64,
        #[arg(long, default_value_t = 30)]
        warmup_frames: u32,
        #[arg(long, default_value_t = 100)]
        rtt_count: u32,
        #[arg(long, default_value_t = 20)]
        rtt_warmup: u32,
        #[arg(long)]
        preset: Vec<String>,
        /// 权限失败、预设超过上限，或码流判定失败时返回退出码 2
        #[arg(long)]
        strict: bool,
        /// 每台匹配设备都测。默认要求恰好一台。
        #[arg(long)]
        all: bool,
    },
}

#[derive(Args)]
struct FilterArgs {
    /// 厂商号，十六进制，例如 2bc5 或 0x8086
    #[arg(long)]
    vid: Option<String>,
    #[arg(long)]
    pid: Option<String>,
    #[arg(long)]
    bus: Option<String>,
    #[arg(long)]
    addr: Option<u8>,
    /// 只保留奥比中光，以及产品字符串或已知 PID 能确认的 RealSense
    #[arg(long)]
    cameras: bool,
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    match dispatch(cli) {
        Ok(code) => code,
        Err(err) => {
            eprintln!("错误: {err}");
            ExitCode::from(1)
        }
    }
}

fn dispatch(cli: Cli) -> Result<ExitCode, Error> {
    let json = cli.json;
    match cli.command {
        Command::List { filter } => {
            let report = usb_linkbench::list_devices(&filter.parse()?)?;
            usb_linkbench::emit(json, text::list(&report), &report)?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Audit { filter } => {
            let report = usb_linkbench::audit_one(&filter.parse()?)?;
            usb_linkbench::emit(json, text::audit(&report), &report)?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Rtt {
            filter,
            count,
            warmup,
            timeout_ms,
            samples,
        } => {
            let report = usb_linkbench::control_rtt(
                &filter.parse()?,
                &RttOptions {
                    count,
                    warmup,
                    timeout: Duration::from_millis(timeout_ms),
                    keep_samples: samples,
                },
            )?;
            usb_linkbench::emit(json, text::rtt(&report), &report)?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Stream {
            filter,
            device,
            seconds,
            warmup_frames,
            width,
            height,
            fourcc,
            fps,
            min_fps,
            expect_frame_bytes,
            preset,
        } => {
            let modes = load_presets(&preset)?;
            let fourcc = fourcc.as_deref().map(parse_fourcc).transpose()?;
            if matches!(
                (width, height, fourcc),
                (Some(_), _, None) | (_, Some(_), None) | (None, _, Some(_)) | (_, None, Some(_))
            ) {
                return Err(Error::Message(
                    "设置格式时 width、height、fourcc 要一起给".into(),
                ));
            }
            let mut paths = device;
            let explicit = !paths.is_empty();
            if paths.is_empty() {
                paths = usb_linkbench::video_nodes_for(&filter.parse()?)?;
            }
            let (min_fps, expect_frame_bytes) =
                apply_single_preset(min_fps, expect_frame_bytes, paths.len(), &modes);
            let report = usb_linkbench::capture(&CaptureRequest {
                paths,
                seconds,
                warmup_frames,
                width,
                height,
                fourcc,
                fps,
                min_fps,
                expect_frame_bytes,
                strict_nodes: explicit,
            })?;
            let failed = report.streams.iter().any(|stream| {
                stream.verdict.as_ref().is_some_and(|verdict| {
                    verdict.fps_ok == Some(false) || verdict.frame_bytes_ok == Some(false)
                })
            });
            usb_linkbench::emit(json, text::stream(&report), &report)?;
            Ok(if failed {
                ExitCode::from(2)
            } else {
                ExitCode::SUCCESS
            })
        }
        Command::Budget {
            preset,
            width,
            height,
            bpp,
            fps,
            speed,
        } => {
            let modes = load_presets(&preset)?;
            let mut labels: Vec<String> = modes
                .iter()
                .map(|mode| format!("{}  {}", mode.id, mode.summary))
                .collect();
            let mut payloads: Vec<u64> = modes
                .iter()
                .map(|mode| mode.payload_bytes_per_sec())
                .collect();
            if let (Some(width), Some(height), Some(bpp), Some(fps)) = (width, height, bpp, fps) {
                let bytes = u64::from(width) * u64::from(height) * u64::from(bpp) * u64::from(fps);
                labels.push(format!("custom {width}x{height}x{bpp} @{fps}fps"));
                payloads.push(bytes);
            } else if width.is_some() || height.is_some() || bpp.is_some() || fps.is_some() {
                return Err(Error::Message(
                    "自定义预算需要同时给出 --width --height --bpp --fps".into(),
                ));
            }
            if payloads.is_empty() {
                println!("{}", preset_names());
                return Ok(ExitCode::SUCCESS);
            }
            let highlight = speed.as_deref().map(parse_speed).transpose()?;
            let report = budget_report(labels, &payloads, highlight);
            usb_linkbench::emit(json, text::budget(&report), &report)?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Storage {
            path,
            seconds,
            block_kib,
            warmup_mib,
        } => {
            if block_kib == 0 || !block_kib.is_multiple_of(4) {
                return Err(Error::Message("block-kib 必须是 4 的正整数倍".into()));
            }
            let report = usb_linkbench::read_direct(&StorageOptions {
                path,
                seconds,
                block_bytes: block_kib as usize * 1024,
                warmup_bytes: u64::from(warmup_mib) * 1024 * 1024,
            })?;
            usb_linkbench::emit(json, text::storage(&report), &report)?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Bulk {
            filter,
            interface,
            endpoint,
            seconds,
            buf_kib,
            warmup,
            timeout_ms,
            detach,
        } => {
            let report = usb_linkbench::bulk_in(
                &filter.parse()?,
                &BulkOptions {
                    interface,
                    endpoint: parse_u8(&endpoint)?,
                    seconds,
                    buffer_bytes: buf_kib as usize * 1024,
                    warmup,
                    timeout: Duration::from_millis(timeout_ms),
                    detach,
                },
            )?;
            usb_linkbench::emit(json, text::bulk(&report), &report)?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Links { verbose } => {
            let report = usb_linkbench::links(verbose);
            usb_linkbench::emit(json, text::links(&report), &report)?;
            Ok(ExitCode::SUCCESS)
        }
        Command::TimingCheck => {
            let report = usb_linkbench::timing_check();
            let ok = report.sleep_ok;
            usb_linkbench::emit(json, text::timing(&report), &report)?;
            Ok(if ok {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(2)
            })
        }
        Command::Run {
            filter,
            seconds,
            warmup_frames,
            rtt_count,
            rtt_warmup,
            preset,
            strict,
            all,
        } => {
            let parsed = filter.parse()?;
            if parsed.is_unrestricted() {
                return Err(Error::Message(
                    "run 需要 --vid、--pid、--bus、--addr 或 --cameras，避免对所有 USB 设备做回传。".into(),
                ));
            }
            let report = usb_linkbench::run_camera(&RunRequest {
                filter: parsed,
                all_matches: all,
                seconds,
                warmup_frames,
                rtt_count,
                rtt_warmup,
                rtt_timeout: Duration::from_millis(200),
                presets: load_presets(&preset)?,
                strict,
            })?;
            let failed = report.checks_failed;
            usb_linkbench::emit(json, text::run(&report), &report)?;
            Ok(if failed {
                ExitCode::from(2)
            } else {
                ExitCode::SUCCESS
            })
        }
    }
}

impl FilterArgs {
    fn parse(&self) -> Result<DeviceFilter, Error> {
        Ok(DeviceFilter {
            vendor_id: self.vid.as_deref().map(parse_u16).transpose()?,
            product_id: self.pid.as_deref().map(parse_u16).transpose()?,
            bus_id: self.bus.clone(),
            address: self.addr,
            cameras_only: self.cameras,
        })
    }
}

fn load_presets(ids: &[String]) -> Result<Vec<&'static ModeBudget>, Error> {
    let mut modes = Vec::with_capacity(ids.len());
    for id in ids {
        modes.push(
            preset(id).ok_or_else(|| {
                Error::Message(format!("未知预设 {id}。可选：\n{}", preset_names()))
            })?,
        );
    }
    Ok(modes)
}

fn preset_names() -> String {
    presets()
        .iter()
        .map(|mode| format!("{}  {}", mode.id, mode.summary))
        .collect::<Vec<_>>()
        .join("\n")
}

fn apply_single_preset(
    min_fps: Option<f64>,
    expect_frame_bytes: Option<u64>,
    nodes: usize,
    modes: &[&ModeBudget],
) -> (Option<f64>, Option<u64>) {
    if nodes == 1 && modes.len() == 1 {
        let mode = modes[0];
        (
            min_fps.or(Some(f64::from(mode.fps) * 0.95)),
            expect_frame_bytes.or(Some(mode.payload_bytes_per_frame())),
        )
    } else {
        (min_fps, expect_frame_bytes)
    }
}

fn parse_u16(text: &str) -> Result<u16, Error> {
    let (raw, hex) = hex_digits(text);
    u16::from_str_radix(hex, 16)
        .map_err(|err| Error::Message(format!("无法解析十六进制数值 {raw}: {err}")))
}

fn parse_u8(text: &str) -> Result<u8, Error> {
    let (raw, hex) = hex_digits(text);
    u8::from_str_radix(hex, 16)
        .map_err(|err| Error::Message(format!("无法解析十六进制数值 {raw}: {err}")))
}

fn hex_digits(text: &str) -> (&str, &str) {
    let raw = text.trim();
    let hex = raw
        .strip_prefix("0x")
        .or_else(|| raw.strip_prefix("0X"))
        .unwrap_or(raw);
    (raw, hex)
}

fn parse_fourcc(text: &str) -> Result<[u8; 4], Error> {
    let bytes = text.as_bytes();
    if bytes.len() > 4 {
        return Err(Error::Message("fourcc 最长 4 个字符".into()));
    }
    let mut out = [b' '; 4];
    out[..bytes.len()].copy_from_slice(bytes);
    Ok(out)
}

fn parse_speed(text: &str) -> Result<LinkSpeed, Error> {
    match text.trim().to_ascii_lowercase().as_str() {
        "1.5" | "low" => Ok(LinkSpeed::Low),
        "12" | "full" => Ok(LinkSpeed::Full),
        "480" | "high" | "usb2" => Ok(LinkSpeed::High),
        "5000" | "super" | "usb3" | "ss" => Ok(LinkSpeed::Super),
        "10000" | "superplus" | "superplus10" | "ssp" => Ok(LinkSpeed::SuperPlus10),
        "20000" | "superplus20" => Ok(LinkSpeed::SuperPlus20),
        other => Err(Error::Message(format!("无法识别链路速率 {other}"))),
    }
}
