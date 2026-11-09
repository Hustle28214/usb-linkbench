#include "linkbench.hpp"

#include <sys/ioctl.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace linkbench {

StorageReport read_direct(const StorageOptions& options) {
    if (options.block_bytes == 0 || options.block_bytes % 4096 != 0) throw Error("块大小必须是 4096 字节的正整数倍");
    if (options.block_bytes > 16 * 1024 * 1024) throw Error("块大小最大 16 MiB");
    const int fd = ::open(options.path.c_str(), O_RDONLY | O_DIRECT);
    if (fd < 0) throw Error(std::string("打开 ") + options.path + " 失败: " + std::strerror(errno));
    std::uint64_t file_bytes = 0;
    if (ioctl(fd, 0x80081272, &file_bytes) != 0 || file_bytes == 0) {
        struct stat st{};
        if (fstat(fd, &st) == 0 && st.st_size > 0) file_bytes = static_cast<std::uint64_t>(st.st_size);
    }
    std::vector<unsigned char> buffer(options.block_bytes + 4096);
    auto* aligned = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(buffer.data()) + 4095) & ~std::uintptr_t{4095});
    auto pread_block = [&](std::uint64_t offset) -> ssize_t {
        const auto n = ::pread(fd, aligned, options.block_bytes, static_cast<off_t>(offset));
        if (n < 0) {
            const auto err = errno;
            ::close(fd);
            throw Error(std::string("读取失败: ") + std::strerror(err));
        }
        return n;
    };
    std::uint64_t offset = 0;
    std::uint64_t warmed = 0;
    while (warmed < options.warmup_bytes) {
        const auto n = pread_block(offset);
        if (n == 0) {
            if (warmed == 0 && offset == 0) {
                ::close(fd);
                throw Error("文件是空的");
            }
            offset = 0;
            continue;
        }
        warmed += static_cast<std::uint64_t>(n);
        offset += static_cast<std::uint64_t>(n);
    }
    const auto window_ns = static_cast<std::uint64_t>(options.seconds * 1e9);
    const auto started = now_ns();
    std::uint64_t bytes = 0, reads = 0, wraps = 0;
    while (elapsed_ns(started) < window_ns) {
        const auto n = pread_block(offset);
        if (n == 0) {
            offset = 0;
            ++wraps;
            if (wraps > 10000) {
                ::close(fd);
                throw Error("文件过小，顺序读在空转");
            }
            continue;
        }
        bytes += static_cast<std::uint64_t>(n);
        ++reads;
        offset += static_cast<std::uint64_t>(n);
    }
    const auto elapsed = elapsed_ns(started);
    ::close(fd);
    StorageReport report;
    report.path = options.path;
    report.direct = true;
    report.block_bytes = options.block_bytes;
    report.file_bytes = file_bytes;
    report.warmup_bytes = options.warmup_bytes;
    report.bytes = bytes;
    report.elapsed_ns = elapsed;
    report.bytes_per_sec = elapsed == 0 ? 0 : static_cast<double>(bytes) / (static_cast<double>(elapsed) / 1e9);
    report.reads = reads;
    report.wraps = wraps;
    report.timer_overhead_ns = *summarize(overhead_samples(200));
    report.notes = {"Linux O_DIRECT 顺序读。页缓存被绕开，设备自身缓存仍在。", "分母是实测墙钟。预热字节不计入。"};
    if (wraps > 0) report.notes.push_back("文件在计时窗内被从头重读 " + std::to_string(wraps) + " 次。");
    return report;
}

}  // namespace linkbench
