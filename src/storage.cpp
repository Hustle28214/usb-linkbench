#include "linkbench.hpp"

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace linkbench {
namespace {

namespace fs = std::filesystem;

std::string read_trim(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::string text;
    std::getline(in, text);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

std::string unescape_mount(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size()) {
            const auto a = static_cast<unsigned char>(text[i + 1]);
            const auto b = static_cast<unsigned char>(text[i + 2]);
            const auto c = static_cast<unsigned char>(text[i + 3]);
            if (a >= '0' && a <= '7' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
                out.push_back(static_cast<char>((a - '0') * 64 + (b - '0') * 8 + (c - '0')));
                i += 3;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

bool mount_covers(const std::string& mount, const std::string& path) {
    if (mount.empty() || path.empty()) return false;
    if (mount == "/") return path[0] == '/';
    if (path == mount) return true;
    return path.size() > mount.size() && path.compare(0, mount.size(), mount) == 0 && path[mount.size()] == '/';
}

}  // namespace

std::optional<std::string> mount_majmin(const std::string& mountinfo, const std::string& path) {
    std::optional<std::string> best;
    std::size_t best_len = 0;
    std::istringstream lines(mountinfo);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream row(line);
        std::vector<std::string> fields;
        std::string field;
        while (row >> field) fields.push_back(unescape_mount(field));
        std::size_t dash = fields.size();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i] == "-") {
                dash = i;
                break;
            }
        }
        if (dash < 5) continue;
        const auto& mount = fields[4];
        if (!mount_covers(mount, path)) continue;
        if (!best || mount.size() > best_len) {
            best = fields[2];
            best_len = mount.size();
        }
    }
    return best;
}

StorageLink storage_usb(const std::string& path) {
    StorageLink link;
    if (path.empty()) {
        link.detail = "未选文件";
        return link;
    }
    std::string majmin;
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
        majmin = std::to_string(major(st.st_rdev)) + ":" + std::to_string(minor(st.st_rdev));
    } else {
        std::string lookup = path;
        std::error_code ec;
        const auto canonical = fs::weakly_canonical(fs::path(path), ec);
        if (!ec) lookup = canonical.string();
        std::ifstream mountinfo("/proc/self/mountinfo");
        if (!mountinfo) {
            link.detail = "读不到挂载表";
            return link;
        }
        std::ostringstream text;
        text << mountinfo.rdbuf();
        const auto id = mount_majmin(text.str(), lookup);
        if (!id) {
            link.detail = "不在已挂载的路径上";
            return link;
        }
        majmin = *id;
    }

    std::error_code ec;
    auto current = fs::canonical(fs::path("/sys/dev/block") / majmin, ec);
    if (ec) {
        link.detail = "找不到块设备";
        return link;
    }
    while (true) {
        if (fs::is_regular_file(current / "idVendor") && fs::is_regular_file(current / "speed")) {
            const auto speed_text = read_trim(current / "speed");
            if (auto parsed = speed_from_sysfs(speed_text)) link.speed = *parsed;
            else link.speed = speed_from_mbps(std::strtod(speed_text.c_str(), nullptr));
            link.product = read_trim(current / "product");
            link.location = current.filename().string();
            link.usb = true;
            return link;
        }
        if (!current.has_parent_path() || current == current.root_path()) break;
        current = current.parent_path();
    }
    link.detail = "不是 USB 设备";
    return link;
}

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
    if (wraps > 0) report.notes.push_back("文件在计时窗内被从头重读 " + std::to_string(wraps) + " 次。");
    return report;
}

}  // namespace linkbench
