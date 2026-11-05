//! Sequential read that bypasses the page cache.
//!
//! Linux uses `O_DIRECT`, macOS uses `F_NOCACHE`, and Windows uses
//! `FILE_FLAG_NO_BUFFERING`. Device-side caches remain on the path. The timed
//! window starts after the warmup read and uses the bytes actually returned.

use std::path::Path;

use serde::Serialize;

use crate::Error;
use crate::clock;
use crate::stats::{self, Summary};

#[derive(Debug, Clone)]
pub struct StorageOptions {
    pub path: std::path::PathBuf,
    pub seconds: f64,
    pub block_bytes: usize,
    pub warmup_bytes: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct StorageReport {
    pub measurement_kind: &'static str,
    pub path: String,
    pub direct: bool,
    pub block_bytes: u64,
    pub file_bytes: u64,
    pub warmup_bytes: u64,
    pub bytes: u64,
    pub elapsed_ns: u64,
    pub bytes_per_sec: f64,
    pub reads: u64,
    pub wraps: u64,
    pub timer_overhead_ns: Summary,
    pub notes: Vec<String>,
}

pub fn read_direct(options: &StorageOptions) -> Result<StorageReport, Error> {
    if options.seconds <= 0.0 {
        return Err(Error::Message("秒数必须大于 0".into()));
    }
    if options.block_bytes == 0 || !options.block_bytes.is_multiple_of(4096) {
        return Err(Error::Message("块大小必须是 4096 的正整数倍".into()));
    }
    if options.block_bytes > 16 * 1024 * 1024 {
        return Err(Error::Message("块大小最大 16 MiB".into()));
    }
    #[cfg(unix)]
    {
        unix::read_direct(options)
    }
    #[cfg(windows)]
    {
        windows::read_direct(options)
    }
}

struct ReadWindow {
    bytes: u64,
    elapsed_ns: u64,
    reads: u64,
    wraps: u64,
}

fn report(
    path: &Path,
    file_bytes: u64,
    options: &StorageOptions,
    window: ReadWindow,
    direct_note: &str,
) -> Result<StorageReport, Error> {
    if window.elapsed_ns == 0 || window.reads == 0 {
        return Err(Error::Message("没有读到数据".into()));
    }
    let mut notes = vec![
        direct_note.to_string(),
        "只统计预热之后的读。分母是这段读的实测墙钟，不是请求的秒数。".into(),
        "页缓存被绕过，设备自身的缓存仍在路径上。".into(),
    ];
    if file_bytes < 64 * 1024 * 1024 {
        notes.push("文件小于 64 MiB，结果更容易受设备缓存影响。".into());
    }
    if window.wraps > 0 {
        notes.push(format!("文件在计时窗内被从头重读 {} 次。", window.wraps));
    }
    Ok(StorageReport {
        measurement_kind: "direct_read",
        path: path.display().to_string(),
        direct: true,
        block_bytes: options.block_bytes as u64,
        file_bytes,
        warmup_bytes: options.warmup_bytes,
        bytes: window.bytes,
        elapsed_ns: window.elapsed_ns,
        bytes_per_sec: window.bytes as f64 / (window.elapsed_ns as f64 / 1e9),
        reads: window.reads,
        wraps: window.wraps,
        timer_overhead_ns: stats::summarize(&clock::overhead_samples(200))
            .expect("overhead samples"),
        notes,
    })
}

#[cfg(unix)]
mod unix {
    use std::ffi::CString;
    use std::io::Error as IoError;
    use std::os::unix::ffi::OsStrExt;

    use super::{StorageOptions, report};
    use crate::Error;
    use crate::clock;

    struct RawFd(i32);

    impl Drop for RawFd {
        fn drop(&mut self) {
            if self.0 >= 0 {
                unsafe { libc::close(self.0) };
            }
        }
    }

    struct Aligned {
        ptr: *mut u8,
        layout: std::alloc::Layout,
    }

    impl Aligned {
        fn new(size: usize) -> Result<Self, Error> {
            let layout = std::alloc::Layout::from_size_align(size, 4096)
                .map_err(|err| Error::Message(err.to_string()))?;
            let ptr = unsafe { std::alloc::alloc(layout) };
            if ptr.is_null() {
                return Err(Error::Message("分配对齐缓冲区失败".into()));
            }
            Ok(Self { ptr, layout })
        }

        fn as_mut_ptr(&mut self) -> *mut u8 {
            self.ptr
        }
    }

    impl Drop for Aligned {
        fn drop(&mut self) {
            unsafe { std::alloc::dealloc(self.ptr, self.layout) };
        }
    }

    pub fn read_direct(options: &StorageOptions) -> Result<super::StorageReport, Error> {
        let c_path = CString::new(options.path.as_os_str().as_bytes())
            .map_err(|_| Error::Message("路径包含空字节".into()))?;
        let flags = open_flags();
        let fd = unsafe { libc::open(c_path.as_ptr(), flags) };
        if fd < 0 {
            return Err(Error::Io(IoError::last_os_error()));
        }
        let fd = RawFd(fd);
        enable_direct(fd.0)?;
        let file_bytes = file_len(fd.0)?;
        let mut buffer = Aligned::new(options.block_bytes)?;
        let mut offset = 0u64;
        let mut warmed = 0u64;
        while warmed < options.warmup_bytes {
            let n = pread(fd.0, buffer.as_mut_ptr(), options.block_bytes, offset)?;
            if n == 0 {
                if warmed == 0 && offset == 0 {
                    return Err(Error::Message("文件是空的".into()));
                }
                offset = 0;
                continue;
            }
            warmed += n as u64;
            offset += n as u64;
        }

        let window_ns = (options.seconds * 1_000_000_000.0) as u64;
        let started = clock::now_ns();
        let mut bytes = 0u64;
        let mut reads = 0u64;
        let mut wraps = 0u64;
        while clock::elapsed_ns(started) < window_ns {
            let n = pread(fd.0, buffer.as_mut_ptr(), options.block_bytes, offset)?;
            if n == 0 {
                offset = 0;
                wraps += 1;
                if wraps > 10_000 {
                    return Err(Error::Message("文件过小，顺序读在空转".into()));
                }
                continue;
            }
            bytes += n as u64;
            reads += 1;
            offset += n as u64;
        }
        let elapsed_ns = clock::elapsed_ns(started);
        report(
            &options.path,
            file_bytes,
            options,
            super::ReadWindow {
                bytes,
                elapsed_ns,
                reads,
                wraps,
            },
            direct_note(),
        )
    }

    #[cfg(target_os = "linux")]
    fn open_flags() -> i32 {
        libc::O_RDONLY | libc::O_DIRECT
    }

    #[cfg(not(target_os = "linux"))]
    fn open_flags() -> i32 {
        libc::O_RDONLY
    }

    #[cfg(target_os = "linux")]
    fn enable_direct(_fd: i32) -> Result<(), Error> {
        Ok(())
    }

    #[cfg(all(unix, not(target_os = "linux")))]
    fn enable_direct(fd: i32) -> Result<(), Error> {
        // macOS F_NOCACHE
        let rc = unsafe { libc::fcntl(fd, 48, 1) };
        if rc != 0 {
            return Err(Error::Io(IoError::last_os_error()));
        }
        Ok(())
    }

    #[cfg(target_os = "linux")]
    fn direct_note() -> &'static str {
        "Linux O_DIRECT 顺序读。"
    }

    #[cfg(not(target_os = "linux"))]
    fn direct_note() -> &'static str {
        "macOS F_NOCACHE 顺序读。"
    }

    fn file_len(fd: i32) -> Result<u64, Error> {
        let mut stat = unsafe { std::mem::zeroed::<libc::stat>() };
        let rc = unsafe { libc::fstat(fd, &mut stat) };
        if rc != 0 {
            return Err(Error::Io(IoError::last_os_error()));
        }
        Ok(stat.st_size.max(0) as u64)
    }

    fn pread(fd: i32, buf: *mut u8, len: usize, offset: u64) -> Result<usize, Error> {
        if offset > i64::MAX as u64 {
            return Err(Error::Message("文件偏移超出 pread 范围".into()));
        }
        loop {
            let n = unsafe { libc::pread(fd, buf.cast(), len, offset as i64) };
            if n >= 0 {
                return Ok(n as usize);
            }
            let err = IoError::last_os_error();
            if err.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            if err.raw_os_error() == Some(22) {
                return Err(Error::Message(
                    "文件系统拒绝直接 I/O（EINVAL）。请换成本地文件系统上的普通文件，或按 4096 对齐的块设备。".into(),
                ));
            }
            return Err(Error::Io(err));
        }
    }
}

#[cfg(windows)]
mod windows {
    use std::os::windows::ffi::OsStrExt;

    use super::{StorageOptions, report};
    use crate::Error;
    use crate::clock;

    #[link(name = "kernel32")]
    extern "system" {
        fn CreateFileW(
            name: *const u16,
            access: u32,
            share: u32,
            security: *const u8,
            disposition: u32,
            flags: u32,
            template: isize,
        ) -> isize;
        fn ReadFile(
            handle: isize,
            buf: *mut u8,
            to_read: u32,
            read: *mut u32,
            overlapped: *mut u8,
        ) -> i32;
        fn SetFilePointerEx(handle: isize, distance: i64, new_pos: *mut i64, method: u32) -> i32;
        fn CloseHandle(handle: isize) -> i32;
        fn GetLastError() -> u32;
    }

    struct Handle(isize);

    impl Drop for Handle {
        fn drop(&mut self) {
            if self.0 != -1 {
                unsafe { CloseHandle(self.0) };
            }
        }
    }

    struct Aligned {
        ptr: *mut u8,
        layout: std::alloc::Layout,
    }

    impl Aligned {
        fn new(size: usize) -> Result<Self, Error> {
            let layout = std::alloc::Layout::from_size_align(size, 4096)
                .map_err(|err| Error::Message(err.to_string()))?;
            let ptr = unsafe { std::alloc::alloc(layout) };
            if ptr.is_null() {
                return Err(Error::Message("分配对齐缓冲区失败".into()));
            }
            Ok(Self { ptr, layout })
        }
    }

    impl Drop for Aligned {
        fn drop(&mut self) {
            unsafe { std::alloc::dealloc(self.ptr, self.layout) };
        }
    }

    pub fn read_direct(options: &StorageOptions) -> Result<super::StorageReport, Error> {
        let wide: Vec<u16> = options
            .path
            .as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect();
        let handle = unsafe {
            CreateFileW(
                wide.as_ptr(),
                0x8000_0000,
                1,
                std::ptr::null(),
                3,
                0x80 | 0x2000_0000,
                0,
            )
        };
        if handle == -1 {
            return Err(Error::Message(format!(
                "CreateFileW 失败，GetLastError={}",
                unsafe { GetLastError() }
            )));
        }
        let handle = Handle(handle);
        let file_bytes = options.path.metadata().map(|meta| meta.len()).unwrap_or(0);
        let mut buffer = Aligned::new(options.block_bytes)?;
        let mut warmed = 0u64;
        while warmed < options.warmup_bytes {
            let n = read_some(handle.0, buffer.ptr, options.block_bytes)?;
            if n == 0 {
                if warmed == 0 {
                    return Err(Error::Message("文件是空的".into()));
                }
                rewind(handle.0)?;
                continue;
            }
            warmed += u64::from(n);
        }
        let window_ns = (options.seconds * 1_000_000_000.0) as u64;
        let started = clock::now_ns();
        let mut bytes = 0u64;
        let mut reads = 0u64;
        let mut wraps = 0u64;
        while clock::elapsed_ns(started) < window_ns {
            let n = read_some(handle.0, buffer.ptr, options.block_bytes)?;
            if n == 0 {
                wraps += 1;
                rewind(handle.0)?;
                if wraps > 10_000 {
                    return Err(Error::Message("文件过小，顺序读在空转".into()));
                }
                continue;
            }
            bytes += u64::from(n);
            reads += 1;
        }
        report(
            &options.path,
            file_bytes,
            options,
            super::ReadWindow {
                bytes,
                elapsed_ns: clock::elapsed_ns(started),
                reads,
                wraps,
            },
            "Windows FILE_FLAG_NO_BUFFERING 顺序读。",
        )
    }

    fn read_some(handle: isize, buf: *mut u8, len: usize) -> Result<u32, Error> {
        let mut got = 0u32;
        let ok = unsafe { ReadFile(handle, buf, len as u32, &mut got, std::ptr::null_mut()) };
        if ok == 0 {
            return Err(Error::Message(format!(
                "ReadFile 失败，GetLastError={}",
                unsafe { GetLastError() }
            )));
        }
        Ok(got)
    }

    fn rewind(handle: isize) -> Result<(), Error> {
        let ok = unsafe { SetFilePointerEx(handle, 0, std::ptr::null_mut(), 0) };
        if ok == 0 {
            return Err(Error::Message(format!(
                "SetFilePointerEx 失败，GetLastError={}",
                unsafe { GetLastError() }
            )));
        }
        Ok(())
    }
}
