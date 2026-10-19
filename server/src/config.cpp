/*
 * config.cpp -- 配置解析：命令行 + INI/JSON 文件 + 语义校验。
 *
 * 平台中立。校验阶段复用 shared/core 的 miclink_check_sample_rate_chain()，
 * 保证"驱动绝不做重采样"这条红线在服务端启动前就被拦住。
 */
#include "config.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "miclink/miclink_jitter.h"
#include "miclink/miclink_sample_rate.h"

namespace miclink {
namespace {

constexpr uint32_t kMinPort = 1u;
constexpr uint32_t kMaxPort = 65535u;
constexpr uint32_t kMaxUdpBuf = 8u * 1024u * 1024u;

std::string trim(const std::string &s)
{
    size_t begin = 0u;
    size_t end = s.size();
    while ((begin < end) && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while ((end > begin) && std::isspace(static_cast<unsigned char>(s[end - 1u]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string to_lower(std::string s)
{
    for (char &c : s) {
        if ((c >= 'A') && (c <= 'Z')) {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c == '-') {
            c = '_';
        }
    }
    return s;
}

bool parse_bool(const std::string &raw, bool *out)
{
    std::string v = to_lower(trim(raw));
    if (v.empty() || (v == "1") || (v == "true") || (v == "yes") || (v == "on")) {
        *out = true;
        return true;
    }
    if ((v == "0") || (v == "false") || (v == "no") || (v == "off")) {
        *out = false;
        return true;
    }
    return false;
}

bool parse_uint(const std::string &raw, uint64_t *out)
{
    std::string v = trim(raw);
    if (v.empty()) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
    if ((end == v.c_str()) || (errno == ERANGE)) {
        return false;
    }
    *out = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_float(const std::string &raw, float *out)
{
    std::string v = trim(raw);
    if (v.empty()) {
        return false;
    }
    char *end = nullptr;
    double parsed = std::strtod(v.c_str(), &end);
    if (end == v.c_str()) {
        return false;
    }
    *out = static_cast<float>(parsed);
    return true;
}

/** 单键赋值：key 已小写化。value 为空串表示"布尔开关置真"。 */
bool apply_key(const std::string &key, const std::string &value, ServerConfig *cfg,
               std::string *err)
{
    const std::string k = to_lower(key);
    const bool has_value = !value.empty();
    uint64_t num = 0u;

    auto need_uint = [&](uint64_t *dst) -> bool {
        if (!parse_uint(value, &num)) {
            if (err != nullptr) {
                *err = "配置项 '" + k + "' 需要非负整数，实际为 '" + value + "'";
            }
            return false;
        }
        *dst = num;
        return true;
    };

    if ((k == "port") || (k == "listen_port")) {
        /* 0 合法：由系统分配端口（测试与多实例场景）。 */
        if (!need_uint(&num) || (num > kMaxPort)) {
            if (err != nullptr) {
                *err = "listen_port 必须在 0..65535 之间（0 = 由系统分配）";
            }
            return false;
        }
        cfg->listen_port = static_cast<uint16_t>(num);
        return true;
    }
    if ((k == "bind") || (k == "bind_ip") || (k == "bind_ipv4")) {
        /* 只接受点分十进制；0 或 "0.0.0.0" 表示通配。 */
        unsigned o[4] = {0u, 0u, 0u, 0u};
        if (value.empty() || (value == "0") || (value == "0.0.0.0") || (value == "*")) {
            cfg->bind_ipv4 = 0u;
            return true;
        }
        if (std::sscanf(value.c_str(), "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) != 4) {
            if (err != nullptr) {
                *err = "配置项 'bind' 需要 IPv4 点分十进制，实际为 '" + value + "'";
            }
            return false;
        }
        if ((o[0] > 255u) || (o[1] > 255u) || (o[2] > 255u) || (o[3] > 255u)) {
            if (err != nullptr) {
                *err = "配置项 'bind' 字节越界：'" + value + "'";
            }
            return false;
        }
        cfg->bind_ipv4 = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
        return true;
    }
    if ((k == "udp_recv_buf") || (k == "udp_recv_buf_bytes") || (k == "rcvbuf")) {
        if (!need_uint(&num) || (num == 0u) || (num > kMaxUdpBuf)) {
            if (err != nullptr) {
                *err = "配置项 '" + k + "' 需要 1..8388608 之间的字节数";
            }
            return false;
        }
        cfg->udp_recv_buf_bytes = static_cast<uint32_t>(num);
        return true;
    }
    if ((k == "client_timeout") || (k == "client_timeout_ms")) {
        return need_uint(&num) && ((cfg->client_timeout_ms = static_cast<uint32_t>(num)), true);
    }
    if ((k == "rate") || (k == "endpoint_rate") || (k == "endpoint_sample_rate") ||
        (k == "sample_rate")) {
        return need_uint(&num) && ((cfg->endpoint_sample_rate = static_cast<uint32_t>(num)), true);
    }
    if ((k == "channels") || (k == "endpoint_channels")) {
        return need_uint(&num) && (num >= 1u) && (num <= 2u) &&
               ((cfg->endpoint_channels = static_cast<uint32_t>(num)), true);
    }
    if ((k == "bits") || (k == "endpoint_bits") || (k == "endpoint_bits_per_sample")) {
        return need_uint(&num) && ((cfg->endpoint_bits_per_sample = static_cast<uint32_t>(num)), true);
    }
    if ((k == "opus_rate") || (k == "expected_opus_rate") || (k == "capture_rate")) {
        return need_uint(&num) && ((cfg->expected_opus_rate = static_cast<uint32_t>(num)), true);
    }
    if ((k == "opus_channels") || (k == "expected_channels")) {
        return need_uint(&num) && (num >= 1u) && (num <= 2u) &&
               ((cfg->expected_channels = static_cast<uint32_t>(num)), true);
    }
    if ((k == "frame_ms") || (k == "frame")) {
        return need_uint(&num) && ((cfg->frame_ms = static_cast<uint32_t>(num)), true);
    }
    if ((k == "jitter_min") || (k == "jitter_min_ms")) {
        return need_uint(&num) && ((cfg->jitter_min_ms = static_cast<uint32_t>(num)), true);
    }
    if ((k == "jitter_max") || (k == "jitter_max_ms")) {
        return need_uint(&num) && ((cfg->jitter_max_ms = static_cast<uint32_t>(num)), true);
    }
    if ((k == "jitter_target") || (k == "jitter_target_ms")) {
        return need_uint(&num) && ((cfg->jitter_target_ms = static_cast<uint32_t>(num)), true);
    }
    if ((k == "jitter_capacity") || (k == "jitter_capacity_frames")) {
        return need_uint(&num) && ((cfg->jitter_capacity_frames = static_cast<uint32_t>(num)), true);
    }
    if ((k == "ring_capacity") || (k == "ring_capacity_frames")) {
        return need_uint(&num) && ((cfg->ring_capacity_frames = static_cast<uint32_t>(num)), true);
    }
    if ((k == "max_write_frames") || (k == "max_write")) {
        return need_uint(&num) && ((cfg->max_write_frames = static_cast<uint32_t>(num)), true);
    }
    if ((k == "backend") || (k == "device_backend")) {
        std::string v = to_lower(trim(value));
        if ((v != "auto") && (v != "ioctl") && (v != "fake")) {
            if (err != nullptr) {
                *err = "配置项 'backend' 只能是 auto/ioctl/fake，实际为 '" + value + "'";
            }
            return false;
        }
        cfg->device_backend = v;
        return true;
    }
    if ((k == "device_path") || (k == "device")) {
        cfg->device_path = trim(value);
        return true;
    }
    if ((k == "fake_capacity") || (k == "fake_capacity_frames")) {
        return need_uint(&num) && ((cfg->fake_capacity_frames = static_cast<uint32_t>(num)), true);
    }
    if ((k == "gain_db") || (k == "gain")) {
        float v = 0.0f;
        if (!parse_float(value, &v)) {
            if (err != nullptr) {
                *err = "配置项 'gain_db' 需要浮点数";
            }
            return false;
        }
        cfg->gain_db = v;
        return true;
    }
    if (k == "declick") {
        bool v = true;
        if (!has_value || !parse_bool(value, &v)) {
            if (err != nullptr) {
                *err = "配置项 'declick' 需要布尔值";
            }
            return false;
        }
        cfg->declick = v;
        return true;
    }
    if (k == "resampler") {
        std::string v = to_lower(trim(value));
        if ((v == "auto") || v.empty()) {
            cfg->resampler = ResamplerBackend::kAuto;
        } else if (v == "soxr") {
            cfg->resampler = ResamplerBackend::kSoxr;
        } else if ((v == "speex") || (v == "speexdsp")) {
            cfg->resampler = ResamplerBackend::kSpeex;
        } else if ((v == "none") || (v == "off")) {
            cfg->resampler = ResamplerBackend::kNone;
        } else {
            if (err != nullptr) {
                *err = "配置项 'resampler' 只能是 auto/soxr/speex/none，实际为 '" + value + "'";
            }
            return false;
        }
        return true;
    }
    if ((k == "resampler_quality") || (k == "quality")) {
        return need_uint(&num) && ((cfg->resampler_quality = static_cast<uint32_t>(num)), true);
    }
    if ((k == "log_level") || (k == "loglevel")) {
        std::string v = to_lower(trim(value));
        if (v.empty() || (v == "0") || (v == "error")) {
            cfg->log_level = LogLevelValue::kError;
        } else if ((v == "1") || (v == "warn") || (v == "warning")) {
            cfg->log_level = LogLevelValue::kWarn;
        } else if ((v == "2") || (v == "info")) {
            cfg->log_level = LogLevelValue::kInfo;
        } else if ((v == "3") || (v == "verbose") || (v == "debug")) {
            cfg->log_level = LogLevelValue::kVerbose;
        } else {
            if (err != nullptr) {
                *err = "配置项 'log_level' 只能是 error/warn/info/verbose";
            }
            return false;
        }
        return true;
    }
    if (k == "log_timestamp") {
        bool v = true;
        if (!has_value || !parse_bool(value, &v)) {
            if (err != nullptr) {
                *err = "配置项 'log_timestamp' 需要布尔值";
            }
            return false;
        }
        cfg->log_timestamp = v;
        return true;
    }
    if ((k == "level_log") || (k == "enable_level_log")) {
        bool v = true;
        if (!has_value || !parse_bool(value, &v)) {
            if (err != nullptr) {
                *err = "配置项 'level_log' 需要布尔值";
            }
            return false;
        }
        cfg->enable_level_log = v;
        return true;
    }
    if ((k == "status_interval") || (k == "status_interval_ms")) {
        return need_uint(&num) && ((cfg->status_interval_ms = static_cast<uint32_t>(num)), true);
    }
    if (k == "discovery") {
        bool v = true;
        if (!has_value || !parse_bool(value, &v)) {
            if (err != nullptr) {
                *err = "配置项 'discovery' 需要布尔值";
            }
            return false;
        }
        cfg->discovery_enabled = v;
        return true;
    }
    if (k == "discovery_port") {
        if (!need_uint(&num) || (num < kMinPort) || (num > kMaxPort)) {
            if (err != nullptr) {
                *err = "配置项 'discovery_port' 端口越界";
            }
            return false;
        }
        cfg->discovery_port = static_cast<uint16_t>(num);
        return true;
    }
    if (k == "discovery_name") {
        cfg->discovery_name = trim(value);
        return true;
    }
    if (k == "discovery_interval_ms") {
        return need_uint(&num) && ((cfg->discovery_interval_ms = static_cast<uint32_t>(num)), true);
    }
    if (k == "dry_run") {
        bool v = true;
        if (!has_value || !parse_bool(value, &v)) {
            if (err != nullptr) {
                *err = "配置项 'dry_run' 需要布尔值";
            }
            return false;
        }
        cfg->dry_run = v;
        return true;
    }

    if (err != nullptr) {
        *err = "未知配置项 '" + k + "'";
    }
    return false;
}

/** 解析一行 INI（忽略 # 与 ; 注释）。 */
bool apply_ini_line(const std::string &line, ServerConfig *cfg, std::string *err)
{
    std::string content = line;
    size_t comment = content.find('#');
    if (comment != std::string::npos) {
        content = content.substr(0u, comment);
    }
    comment = content.find(';');
    if (comment != std::string::npos) {
        content = content.substr(0u, comment);
    }
    content = trim(content);
    if (content.empty()) {
        return true;
    }
    if ((content.front() == '[') && (content.back() == ']')) {
        return true; /* 节名，本配置扁平，忽略 */
    }
    size_t eq = content.find('=');
    if (eq == std::string::npos) {
        if (err != nullptr) {
            *err = "INI 行缺少 '='：" + line;
        }
        return false;
    }
    return apply_key(trim(content.substr(0u, eq)), trim(content.substr(eq + 1u)), cfg, err);
}

/** 解析扁平 JSON（只支持一层 "key": value，value 为字符串/数字/布尔）。 */
bool apply_flat_json(const std::string &text, ServerConfig *cfg, std::string *err)
{
    size_t pos = text.find('{');
    if (pos == std::string::npos) {
        if (err != nullptr) {
            *err = "JSON 配置缺少 '{'";
        }
        return false;
    }
    size_t i = pos + 1u;
    const size_t n = text.size();

    auto skip_space = [&]() {
        while ((i < n) && (std::isspace(static_cast<unsigned char>(text[i])) || (text[i] == ','))) {
            ++i;
        }
    };

    while (i < n) {
        skip_space();
        if ((i >= n) || (text[i] == '}')) {
            break;
        }
        if (text[i] != '"') {
            if (err != nullptr) {
                *err = std::string("JSON 键必须以 '\"' 开头，位置 ") + std::to_string(i);
            }
            return false;
        }
        ++i;
        std::string key;
        while ((i < n) && (text[i] != '"')) {
            key.push_back(text[i]);
            ++i;
        }
        if (i >= n) {
            if (err != nullptr) {
                *err = "JSON 键未闭合";
            }
            return false;
        }
        ++i;
        skip_space();
        if ((i >= n) || (text[i] != ':')) {
            if (err != nullptr) {
                *err = "JSON 缺少 ':'，键 " + key;
            }
            return false;
        }
        ++i;
        skip_space();
        std::string value;
        if ((i < n) && (text[i] == '"')) {
            ++i;
            while ((i < n) && (text[i] != '"')) {
                if ((text[i] == '\\') && (i + 1u < n)) {
                    ++i;
                }
                value.push_back(text[i]);
                ++i;
            }
            if (i >= n) {
                if (err != nullptr) {
                    *err = "JSON 字符串值未闭合，键 " + key;
                }
                return false;
            }
            ++i;
        } else {
            size_t start = i;
            while ((i < n) && (text[i] != ',') && (text[i] != '}') &&
                   !std::isspace(static_cast<unsigned char>(text[i]))) {
                ++i;
            }
            value = text.substr(start, i - start);
        }
        if (!apply_key(key, value, cfg, err)) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char *config_version_string()
{
    return "miclinkserver 1.0.0 (protocol v1, ioctl v1)";
}

bool config_load_file(const std::string &path, ServerConfig *out, std::string *err)
{
    if ((out == nullptr) || path.empty()) {
        if (err != nullptr) {
            *err = "配置文件路径为空";
        }
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (err != nullptr) {
            *err = "无法打开配置文件：" + path;
        }
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    /* 以第一个非空白字符判定格式：'{' -> JSON，否则按 INI 逐行解析。 */
    size_t first = 0u;
    while ((first < text.size()) && std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    if ((first < text.size()) && (text[first] == '{')) {
        return apply_flat_json(text, out, err);
    }

    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && (line.back() == '\r')) {
            line.pop_back();
        }
        if (!apply_ini_line(line, out, err)) {
            return false;
        }
    }
    return true;
}

bool config_parse(int argc, char **argv, ServerConfig *out, std::string *err)
{
    if (out == nullptr) {
        return false;
    }

    /* 先扫一遍 --config，文件里的值先落地，再被命令行覆盖。 */
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        std::string arg = argv[i];
        std::string value;
        bool has_inline = false;
        size_t eq = arg.find('=');
        if ((eq != std::string::npos) && (arg.compare(0, 2, "--") == 0)) {
            value = arg.substr(eq + 1u);
            arg = arg.substr(0u, eq);
            has_inline = true;
        }
        if ((arg == "--config") || (arg == "-c")) {
            std::string path = has_inline ? value
                                          : (((i + 1) < argc) ? std::string(argv[i + 1]) : std::string());
            if (path.empty()) {
                if (err != nullptr) {
                    *err = "--config 缺少文件路径";
                }
                return false;
            }
            out->config_file = path;
            if (!config_load_file(path, out, err)) {
                return false;
            }
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        std::string arg = argv[i];
        if ((arg.compare(0, 2, "--") != 0) && (arg.compare(0, 1, "-") != 0)) {
            if (err != nullptr) {
                *err = "多余的位置参数 '" + arg + "'（用法见 --help）";
            }
            return false;
        }

        std::string inline_value;
        bool has_inline = false;
        size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            inline_value = arg.substr(eq + 1u);
            arg = arg.substr(0u, eq);
            has_inline = true;
        }

        if ((arg == "--help") || (arg == "-h") || (arg == "/?")) {
            out->show_help = true;
            return true;
        }
        if ((arg == "--version") || (arg == "-V")) {
            out->show_version = true;
            return true;
        }
        if ((arg == "--verbose") || (arg == "-v")) {
            out->log_level = LogLevelValue::kVerbose;
            continue;
        }
        if ((arg == "--quiet") || (arg == "-q")) {
            out->log_level = LogLevelValue::kWarn;
            continue;
        }
        if (arg == "--dry-run") {
            out->dry_run = true;
            continue;
        }
        if (arg == "--level-log") {
            out->enable_level_log = true;
            continue;
        }
        if (arg == "--no-declick") {
            out->declick = false;
            continue;
        }
        if (arg == "--discovery") {
            out->discovery_enabled = true;
            continue;
        }
        if ((arg == "--config") || (arg == "-c")) {
            if (!has_inline) {
                ++i; /* 已在上一轮处理，这里只跳过取值 */
            }
            continue;
        }

        std::string key = arg;
        if (key.compare(0, 2, "--") == 0) {
            key = key.substr(2u);
        } else if (key.compare(0, 1, "-") == 0) {
            key = key.substr(1u);
        }

        std::string value;
        if (has_inline) {
            value = inline_value;
        } else if (((i + 1) < argc) && (argv[i + 1] != nullptr) &&
                   (std::string(argv[i + 1]).compare(0, 1, "-") != 0)) {
            value = argv[++i];
        } else {
            value = "true"; /* 形如 --declick 的开关 */
        }

        if (!apply_key(key, value, out, err)) {
            return false;
        }
    }
    return true;
}

bool config_validate(const ServerConfig &cfg, std::string *err)
{
    auto fail = [&](const std::string &msg) -> bool {
        if (err != nullptr) {
            *err = msg;
        }
        return false;
    };

    /* 0 是合法值：由系统分配端口（测试与"多实例"场景使用）。 */
    if (cfg.listen_port > kMaxPort) {
        return fail("listen_port 必须在 0..65535 之间（0 = 由系统分配）");
    }
    if ((cfg.udp_recv_buf_bytes == 0u) || (cfg.udp_recv_buf_bytes > kMaxUdpBuf)) {
        return fail("udp_recv_buf_bytes 必须在 1..8388608 之间");
    }
    if ((cfg.jitter_min_ms == 0u) || (cfg.jitter_max_ms == 0u) ||
        (cfg.jitter_min_ms > cfg.jitter_max_ms)) {
        return fail("jitter_min_ms 必须 <= jitter_max_ms 且均大于 0");
    }
    if (cfg.jitter_max_ms > MICLINK_JB_HARD_MAX_DELAY) {
        return fail("jitter_max_ms 超过硬上限 " + std::to_string(MICLINK_JB_HARD_MAX_DELAY) + "ms");
    }
    if ((cfg.frame_ms != 10u) && (cfg.frame_ms != 20u) && (cfg.frame_ms != 5u) &&
        (cfg.frame_ms != 40u)) {
        return fail("frame_ms 必须是 5/10/20/40 之一（Opus 合法帧长）");
    }
    if ((cfg.endpoint_bits_per_sample != 16u)) {
        return fail("内核驱动端点只支持 16-bit，endpoint_bits_per_sample 必须为 16");
    }
    if ((cfg.endpoint_channels != 1u)) {
        return fail("内核驱动端点只支持单声道，endpoint_channels 必须为 1");
    }

    /* 采样率链路：端点率必须等于解码率（驱动绝不做重采样）；采集率允许不同。 */
    const char *message = nullptr;
    miclink_sr_result r = miclink_check_sample_rate_chain(
        cfg.expected_opus_rate, cfg.endpoint_sample_rate, cfg.endpoint_sample_rate,
        static_cast<uint16_t>(cfg.expected_channels), &message);
    if (miclink_sr_is_fatal(r)) {
        return fail(std::string("采样率链路校验失败：") + ((message != nullptr) ? message : "未知原因"));
    }
    if (r == MICLINK_SR_CAPTURE_MISMATCH) {
        /* 非致命：服务端会重采样，仅提示。 */
        (void)message;
    }
    if ((cfg.resampler == ResamplerBackend::kNone) &&
        (cfg.expected_opus_rate != cfg.endpoint_sample_rate)) {
        return fail("resampler=none 但采集率 != 端点率：请改用 soxr/speex 或对齐采样率");
    }
    if ((cfg.max_write_frames == 0u) || (cfg.max_write_frames > 65536u)) {
        return fail("max_write_frames 必须在 1..65536 之间");
    }
    if (cfg.ring_capacity_frames < 64u) {
        return fail("ring_capacity_frames 过小（至少 64）");
    }
    return true;
}

std::string config_to_string(const ServerConfig &cfg)
{
    const char *resampler_name = "auto";
    switch (cfg.resampler) {
        case ResamplerBackend::kAuto:
            resampler_name = "auto";
            break;
        case ResamplerBackend::kSoxr:
            resampler_name = "soxr";
            break;
        case ResamplerBackend::kSpeex:
            resampler_name = "speex";
            break;
        case ResamplerBackend::kNone:
            resampler_name = "none";
            break;
    }
    const char *level_name = "info";
    switch (cfg.log_level) {
        case LogLevelValue::kError:
            level_name = "error";
            break;
        case LogLevelValue::kWarn:
            level_name = "warn";
            break;
        case LogLevelValue::kInfo:
            level_name = "info";
            break;
        case LogLevelValue::kVerbose:
            level_name = "verbose";
            break;
    }

    char buf[2048];
    std::snprintf(buf, sizeof(buf),
                  "监听端口        : %u\n"
                  "绑定地址        : %u.%u.%u.%u\n"
                  "UDP 接收缓冲    : %u 字节（SO_RCVBUF）\n"
                  "客户端超时      : %u ms\n"
                  "端点格式        : %u Hz / %u ch / %u bit\n"
                  "期望 Opus 率    : %u Hz / %u ch\n"
                  "帧长            : %u ms\n"
                  "抖动缓冲        : 目标 %u ms，区间 [%u, %u] ms\n"
                  "环形缓冲        : %u 帧；单次写入上限 %u 帧\n"
                  "设备后端        : %s（路径 %s）\n"
                  "重采样          : %s，质量 %u\n"
                  "增益            : %.2f dB；去咔哒 %s\n"
                  "日志级别        : %s；电平日志 %s\n"
                  "发现服务        : %s（端口 %u）\n"
                  "配置文件        : %s",
                  static_cast<unsigned>(cfg.listen_port), (cfg.bind_ipv4 >> 24) & 0xFFu,
                  (cfg.bind_ipv4 >> 16) & 0xFFu, (cfg.bind_ipv4 >> 8) & 0xFFu, cfg.bind_ipv4 & 0xFFu,
                  cfg.udp_recv_buf_bytes, cfg.client_timeout_ms, cfg.endpoint_sample_rate,
                  cfg.endpoint_channels, cfg.endpoint_bits_per_sample, cfg.expected_opus_rate,
                  cfg.expected_channels, cfg.frame_ms, cfg.jitter_target_ms, cfg.jitter_min_ms,
                  cfg.jitter_max_ms, cfg.ring_capacity_frames, cfg.max_write_frames,
                  cfg.device_backend.c_str(), cfg.device_path.c_str(), resampler_name,
                  cfg.resampler_quality, static_cast<double>(cfg.gain_db),
                  cfg.declick ? "开" : "关", level_name, cfg.enable_level_log ? "开" : "关",
                  cfg.discovery_enabled ? "开" : "关", static_cast<unsigned>(cfg.discovery_port),
                  cfg.config_file.empty() ? "(无)" : cfg.config_file.c_str());
    return std::string(buf);
}

void config_print_usage(const char *program)
{
    const char *prog = (program != nullptr) ? program : "miclinkserver";
    std::fprintf(
        stderr,
        "%s\n"
        "用法: %s [选项]\n"
        "\n"
        "网络:\n"
        "  --port <1..65535>           UDP 监听端口（默认 5004）\n"
        "  --bind <a.b.c.d>            绑定地址（默认通配）\n"
        "  --udp-recv-buf <bytes>      SO_RCVBUF 大小（默认 262144，抗抖动突发）\n"
        "  --client-timeout-ms <ms>    超过该时间没收到包视为客户端断开（默认 3000）\n"
        "\n"
        "音频:\n"
        "  --rate <44100|48000>        驱动端点采样率（默认 48000）\n"
        "  --opus-rate <16000|48000>   期望的手机侧 Opus 采样率（默认 48000）\n"
        "  --channels <1>              端点声道（驱动只支持 1）\n"
        "  --frame-ms <5|10|20|40>     每包帧长（默认 20）\n"
        "  --jitter-min-ms <ms>        抖动缓冲目标延迟下限（默认 50）\n"
        "  --jitter-max-ms <ms>        抖动缓冲目标延迟上限（默认 100，硬上限 120）\n"
        "  --jitter-target-ms <ms>     初始目标延迟（默认取下限）\n"
        "  --resampler <auto|soxr|speex|none>  重采样后端（默认 auto）\n"
        "  --resampler-quality <n>     重采样质量档位（默认 4）\n"
        "  --gain-db <dB>              输出增益（默认 0）\n"
        "  --no-declick                关闭丢包后的淡入去咔哒\n"
        "\n"
        "设备:\n"
        "  --backend <auto|ioctl|fake> 设备后端（默认 auto：Windows 走 IOCTL，否则 Fake）\n"
        "  --device-path <path>        控制设备路径（默认 \\\\.\\MicLinkControl）\n"
        "  --ring-capacity-frames <n>  用户态环形缓冲容量（默认 8192）\n"
        "  --max-write-frames <n>      单次 WRITE_PCM 最大帧数（默认 1024）\n"
        "\n"
        "日志与发现:\n"
        "  --log-level <error|warn|info|verbose>   日志级别（默认 info）\n"
        "  --verbose / -v              等价于 --log-level verbose\n"
        "  --quiet / -q                等价于 --log-level warn\n"
        "  --level-log                 打印 RMS/peak 电平\n"
        "  --discovery                 开启本地网段广播发现服务（默认端口 5005）\n"
        "  --discovery-port <port>     发现服务端口\n"
        "\n"
        "其它:\n"
        "  --config <file>             加载 INI 或 JSON 配置文件\n"
        "  --dry-run                   只解析并打印配置，不启动\n"
        "  --help / -h                 打印本帮助\n"
        "  --version / -V              打印版本\n"
        "\n"
        "配置文件（INI 示例，键名与长选项等价，下划线/连字符均可）:\n"
        "  port = 5004\n"
        "  rate = 48000\n"
        "  opus-rate = 48000\n"
        "  jitter-min-ms = 50\n"
        "  jitter-max-ms = 100\n"
        "  resampler = soxr\n",
        config_version_string(), prog);
}

}  // namespace miclink
