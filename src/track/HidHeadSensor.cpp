#include "track/HidHeadSensor.h"

#include "util/Log.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>
#include <numbers>
#include <string_view>

namespace track {
namespace {

// Android Head Tracker HID protocol usages (Sensors page).
constexpr USAGE kSensorPage = 0x20;
constexpr USAGE kOtherCustom = 0xE1;
constexpr USAGE kSensorDescription = 0x0308;
constexpr USAGE kReportInterval = 0x030E;
constexpr USAGE kReportingNoEvents = 0x0840, kReportingAllEvents = 0x0841;
constexpr USAGE kPowerFull = 0x0851, kPowerOff = 0x0855;
constexpr USAGE kTransportAcl = 0xF800;  // v2.0 (LE-capable) devices: pick classic ACL, as Windows has no ISO path
constexpr USAGE kRotationVector = 0x0544;  // Custom Value 1
constexpr std::string_view kMarker = "#AndroidHeadTracker#";
// Protocol: devices must support >= 50 Hz and should allow up to 100 Hz; faster samples shorten prediction.
constexpr double kTargetIntervalS = 0.010;
constexpr size_t kMaxPackedBytes = 64;

// DEVPKEY_Device_InstanceId, spelled out so no GUID-defining header is needed.
const DEVPROPKEY kInstanceIdKey = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() {
        if (valid()) CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

struct Preparsed {
    PHIDP_PREPARSED_DATA p = nullptr;
    Preparsed() = default;
    ~Preparsed() {
        if (p) HidD_FreePreparsedData(p);
    }
    Preparsed(const Preparsed&) = delete;
    Preparsed& operator=(const Preparsed&) = delete;
};

std::string win32Error(DWORD e) { return logx::hr(static_cast<long>(HRESULT_FROM_WIN32(e))); }

HANDLE openHid(const wchar_t* path, DWORD access, DWORD flags) {
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr);
}

int unitExponent(ULONG nibble) {
    const int n = static_cast<int>(nibble & 0xF);
    return n >= 8 ? n - 16 : n;
}

std::vector<HIDP_VALUE_CAPS> valueCaps(HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA ppd, USHORT count) {
    std::vector<HIDP_VALUE_CAPS> caps(count);
    if (count == 0 || HidP_GetValueCaps(type, caps.data(), &count, ppd) != HIDP_STATUS_SUCCESS) return {};
    caps.resize(count);
    return caps;
}

std::vector<HIDP_BUTTON_CAPS> buttonCaps(HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA ppd, USHORT count) {
    std::vector<HIDP_BUTTON_CAPS> caps(count);
    if (count == 0 || HidP_GetButtonCaps(type, caps.data(), &count, ppd) != HIDP_STATUS_SUCCESS) return {};
    caps.resize(count);
    return caps;
}

USAGE usageOf(const HIDP_VALUE_CAPS& c) { return c.IsRange ? c.Range.UsageMin : c.NotRange.Usage; }

// 1: description carries the Android head tracker marker; 0: another custom sensor; -1: no feature report readable.
int describesHeadTracker(HANDLE h, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps) {
    if (caps.FeatureReportByteLength == 0) return -1;
    const auto features = valueCaps(HidP_Feature, ppd, caps.NumberFeatureValueCaps);
    std::vector<UCHAR> ids;
    for (const auto& c : features)
        if (std::find(ids.begin(), ids.end(), c.ReportID) == ids.end()) ids.push_back(c.ReportID);
    std::vector<char> report(caps.FeatureReportByteLength);
    bool readAny = false;
    for (UCHAR id : ids) {
        std::fill(report.begin(), report.end(), char(0));
        report[0] = static_cast<char>(id);
        if (!HidD_GetFeature(h, report.data(), static_cast<ULONG>(report.size()))) continue;
        readAny = true;
        for (const auto& c : features) {
            if (c.ReportID != id || c.UsagePage != kSensorPage || usageOf(c) != kSensorDescription) continue;
            std::string text((c.ReportCount * c.BitSize + 7) / 8, '\0');
            if (c.BitSize == 8 &&
                HidP_GetUsageValueArray(HidP_Feature, kSensorPage, c.LinkCollection, kSensorDescription, text.data(),
                                        static_cast<USHORT>(text.size()), ppd, report.data(),
                                        static_cast<ULONG>(report.size())) == HIDP_STATUS_SUCCESS &&
                text.find(kMarker) != std::string::npos)
                return 1;
        }
        // Some HID parser versions omit constant fields like the description, so also scan the raw report.
        if (std::string_view(report.data(), report.size()).find(kMarker) != std::string_view::npos) return 1;
    }
    return readAny ? 0 : -1;
}

std::string devNodeString(DEVINST node, ULONG property) {
    wchar_t buf[256]{};
    ULONG size = static_cast<ULONG>(sizeof(buf) - sizeof(wchar_t));
    if (CM_Get_DevNode_Registry_PropertyW(node, property, nullptr, buf, &size, 0) != CR_SUCCESS) return {};
    return logx::narrow(buf);
}

std::wstring upper(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towupper(ch));
    return s;
}

// Bluetooth address embedded in a BTHENUM/BTHLEDEVICE service instance id ("...\9&2B0A3C1E&0&ACXXXXXXXXXX_C00000000").
std::wstring bluetoothAddress(std::wstring_view id) {
    const auto seg = id.substr(id.rfind(L'\\') + 1);
    for (size_t i = 0; i + 12 <= seg.size(); ++i) {
        if (i > 0 && seg[i - 1] != L'&') continue;
        if (i + 12 < seg.size() && seg[i + 12] != L'_') continue;
        const auto token = seg.substr(i, 12);
        if (std::all_of(token.begin(), token.end(), [](wchar_t ch) { return std::iswxdigit(ch) != 0; }))
            return upper(std::wstring(token));
    }
    return {};
}

// Name of the paired Bluetooth device node ("BTHENUM\DEV_<addr>\..." / "BTHLE\DEV_<addr>\...") for an address.
std::string bluetoothDeviceName(const wchar_t* enumerator, const std::wstring& address) {
    constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, enumerator, flags) != CR_SUCCESS || len == 0) return {};
    std::vector<wchar_t> list(len);
    if (CM_Get_Device_ID_ListW(enumerator, list.data(), len, flags) != CR_SUCCESS) return {};
    const std::wstring key = L"\\DEV_" + address;
    for (wchar_t* p = list.data(); *p; p += std::wcslen(p) + 1) {
        if (upper(p).find(key) == std::wstring::npos) continue;
        DEVINST node = 0;
        if (CM_Locate_DevNodeW(&node, p, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        std::string name = devNodeString(node, CM_DRP_FRIENDLYNAME);
        if (name.empty()) name = devNodeString(node, CM_DRP_DEVICEDESC);
        if (!name.empty()) return name;
    }
    return {};
}

// The headphones' user-visible Bluetooth name, found through the HID collection's parent service node.
std::string bluetoothName(const wchar_t* interfacePath) {
    wchar_t id[MAX_DEVICE_ID_LEN]{};
    ULONG size = static_cast<ULONG>(sizeof(id));
    DEVPROPTYPE type = 0;
    DEVINST node = 0;
    if (CM_Get_Device_Interface_PropertyW(interfacePath, &kInstanceIdKey, &type, reinterpret_cast<PBYTE>(id), &size,
                                          0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING || CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return {};
    for (int depth = 0; depth < 4 && CM_Get_Parent(&node, node, 0) == CR_SUCCESS; ++depth) {
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};
        const std::wstring parent = upper(id);
        if (!parent.starts_with(L"BTHENUM\\") && !parent.starts_with(L"BTHLEDEVICE\\")) continue;
        const std::wstring address = bluetoothAddress(parent);
        if (address.empty()) return {};
        std::string name = bluetoothDeviceName(L"BTHENUM", address);
        return name.empty() ? bluetoothDeviceName(L"BTHLE", address) : name;
    }
    return {};
}

std::string sensorName(HANDLE h, const wchar_t* path) {
    if (std::string bt = bluetoothName(path); !bt.empty()) return bt;
    wchar_t product[256]{};
    if (HidD_GetProductString(h, product, static_cast<ULONG>(sizeof(product) - sizeof(wchar_t))) && product[0])
        return logx::narrow(product);
    return "Head tracker";
}

// Protocol rotation vector (axis * angle, radians; transform from the reference frame to the head frame; head axes
// X = right ear, Y = nose, Z = top of head) -> degrees in track::Orientation convention.
Orientation toOrientation(const double rv[3]) {
    const double angle = std::sqrt(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]);
    if (angle < 1e-9) return {};
    const double kx = rv[0] / angle, ky = rv[1] / angle, kz = rv[2] / angle;
    const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
    // Rodrigues matrix M (reference -> head). Its rows are the head axes in reference coordinates; only the entries
    // the yaw-pitch-roll (Z, X', Y'') decomposition needs are formed.
    const double rightZ = t * kx * kz + s * ky;  // M02
    const double noseX = t * kx * ky + s * kz;   // M10
    const double noseY = c + t * ky * ky;        // M11
    const double noseZ = t * ky * kz - s * kx;   // M12
    const double topZ = c + t * kz * kz;         // M22
    constexpr double deg = 180.0 / std::numbers::pi;
    Orientation o;
    o.yaw = static_cast<float>(std::atan2(noseX, noseY) * deg);                        // nose towards +X = right
    o.pitch = static_cast<float>(std::asin(std::clamp(noseZ, -1.0, 1.0)) * deg);     // nose up
    o.roll = static_cast<float>(std::atan2(-rightZ, topZ) * deg);                      // right ear down
    return o;
}

}  // namespace

std::vector<HidHeadSensor::Info> HidHeadSensor::enumerate() {
    std::vector<Info> out;
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    std::vector<wchar_t> list;
    for (;;) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, &hidGuid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
                CR_SUCCESS ||
            len <= 1)
            return out;
        list.assign(len, L'\0');
        const CONFIGRET cr = CM_Get_Device_Interface_ListW(&hidGuid, nullptr, list.data(), len,
                                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_SUCCESS) break;
        if (cr != CR_BUFFER_SMALL) return out;  // CR_BUFFER_SMALL: a device arrived in between, retry
    }
    for (const wchar_t* path = list.data(); *path; path += std::wcslen(path) + 1) {
        // Query-only open first: keyboards/mice are held exclusively and must not be opened for read/write.
        Handle query(openHid(path, 0, 0));
        if (!query.valid()) continue;
        Preparsed ppd;
        HIDP_CAPS caps{};
        if (!HidD_GetPreparsedData(query.h, &ppd.p) || HidP_GetCaps(ppd.p, &caps) != HIDP_STATUS_SUCCESS ||
            caps.UsagePage != kSensorPage || caps.Usage != kOtherCustom)
            continue;
        Handle rw(openHid(path, GENERIC_READ | GENERIC_WRITE, 0));
        // An unreadable description still lists the collection; only a readable non-matching one is another sensor.
        if (describesHeadTracker(rw.valid() ? rw.h : query.h, ppd.p, caps) == 0) continue;
        out.push_back({path, sensorName(query.h, path)});
    }
    return out;
}

HidHeadSensor::~HidHeadSensor() { stop(); }

bool HidHeadSensor::start(const std::wstring& path, HeadTracker& sink, std::string* error) {
    std::lock_guard lock(mu_);
    stopLocked();
    auto fail = [&](const std::string& msg) {
        logx::error("Head sensor: " + msg);
        if (error) *error = msg;
        release();
        return false;
    };

    HANDLE h = openHid(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_FLAG_OVERLAPPED);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot open sensor " + win32Error(GetLastError()));
    handle_ = h;
    PHIDP_PREPARSED_DATA ppd = nullptr;
    if (!HidD_GetPreparsedData(h, &ppd)) return fail("cannot read HID descriptor " + win32Error(GetLastError()));
    preparsed_ = ppd;
    HIDP_CAPS caps{};
    if (HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS || caps.UsagePage != kSensorPage ||
        caps.Usage != kOtherCustom || caps.InputReportByteLength == 0 || caps.FeatureReportByteLength == 0)
        return fail("device is not an Android head tracker sensor");
    inputBytes_ = caps.InputReportByteLength;
    featureBytes_ = caps.FeatureReportByteLength;

    bool found = false;
    for (const auto& c : valueCaps(HidP_Input, ppd, caps.NumberInputValueCaps)) {
        if (c.UsagePage != kSensorPage || usageOf(c) != kRotationVector) continue;
        const size_t packed = (static_cast<size_t>(c.ReportCount) * c.BitSize + 7) / 8;
        if (c.ReportCount < 3 || c.BitSize == 0 || c.BitSize > 32 || packed > kMaxPackedBytes) break;
        rotation_.reportId = c.ReportID;
        rotation_.linkCollection = c.LinkCollection;
        rotation_.bitSize = c.BitSize;
        rotation_.packedBytes = static_cast<uint16_t>(packed);
        rotation_.isSigned = c.LogicalMin < 0;
        // HID physical value: physical range maps the logical one (identity when unspecified), times 10^exponent.
        const double unit = std::pow(10.0, unitExponent(c.UnitsExp));
        rotation_.scale = unit;
        rotation_.offset = 0.0;
        if ((c.PhysicalMin != 0 || c.PhysicalMax != 0) && c.LogicalMax != c.LogicalMin) {
            const double k = (double(c.PhysicalMax) - c.PhysicalMin) / (double(c.LogicalMax) - c.LogicalMin);
            rotation_.scale = k * unit;
            rotation_.offset = (c.PhysicalMin - c.LogicalMin * k) * unit;
        }
        found = true;
        break;
    }
    if (!found) return fail("descriptor has no usable rotation vector field");

    name_ = sensorName(h, path.c_str());
    if (!configure(true, error)) {
        release();
        return false;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return fail("CreateEvent failed " + win32Error(GetLastError()));
    sink_ = &sink;
    thread_ = std::thread(&HidHeadSensor::readerMain, this);
    logx::info("Head sensor: reading '" + name_ + "'");
    return true;
}

void HidHeadSensor::stop() {
    std::lock_guard lock(mu_);
    stopLocked();
}

void HidHeadSensor::stopLocked() {
    if (thread_.joinable()) {
        SetEvent(stopEvent_);
        thread_.join();
        // Stop the stream so the headphones stop spending battery and Bluetooth bandwidth on reports.
        configure(false, nullptr);
    }
    release();
}

void HidHeadSensor::release() {
    if (handle_) CloseHandle(handle_);
    if (stopEvent_) CloseHandle(stopEvent_);
    if (preparsed_) HidD_FreePreparsedData(static_cast<PHIDP_PREPARSED_DATA>(preparsed_));
    handle_ = stopEvent_ = preparsed_ = nullptr;
    sink_ = nullptr;
}

// Writes the host-owned properties. Reports are built from zero and sent whole (as Android's host does): reading
// them back first is unreliable on some firmware, and every writable field of interest is set explicitly.
bool HidHeadSensor::configure(bool on, std::string* error) {
    auto* ppd = static_cast<PHIDP_PREPARSED_DATA>(preparsed_);
    HIDP_CAPS caps{};
    if (HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS) return false;
    std::map<UCHAR, std::vector<char>> reports;
    auto report = [&](UCHAR id) -> std::vector<char>& {
        auto& r = reports[id];
        if (r.empty()) {
            r.assign(featureBytes_, '\0');
            r[0] = static_cast<char>(id);
        }
        return r;
    };
    auto fail = [&](const std::string& msg) {
        logx::error("Head sensor: " + msg);
        if (error) *error = msg;
        return false;
    };

    bool interval = false;
    for (const auto& c : valueCaps(HidP_Feature, ppd, caps.NumberFeatureValueCaps)) {
        if (c.UsagePage != kSensorPage || usageOf(c) != kReportInterval) continue;
        // Units are seconds scaled by the unit exponent; pick the advertised interval closest to the target.
        const bool physical = c.PhysicalMin != 0 || c.PhysicalMax != 0;
        const LONG lo = physical ? std::min(c.PhysicalMin, c.PhysicalMax) : std::min(c.LogicalMin, c.LogicalMax);
        const LONG hi = physical ? std::max(c.PhysicalMin, c.PhysicalMax) : std::max(c.LogicalMin, c.LogicalMax);
        const double unit = std::pow(10.0, unitExponent(c.UnitsExp));
        const LONG value = std::clamp(static_cast<LONG>(std::lround(kTargetIntervalS / unit)), lo, hi);
        auto& r = report(c.ReportID);
        const auto st =
            physical ? HidP_SetScaledUsageValue(HidP_Feature, kSensorPage, c.LinkCollection, kReportInterval, value, ppd,
                                                r.data(), static_cast<ULONG>(r.size()))
                     : HidP_SetUsageValue(HidP_Feature, kSensorPage, c.LinkCollection, kReportInterval,
                                          static_cast<ULONG>(value), ppd, r.data(), static_cast<ULONG>(r.size()));
        interval = st == HIDP_STATUS_SUCCESS;
        break;
    }

    // Selector usages live in named-array collections (exposed as button caps). Transport only exists on v2.0.
    const auto buttons = buttonCaps(HidP_Feature, ppd, caps.NumberFeatureButtonCaps);
    auto select = [&](USAGE usage) {
        for (const auto& b : buttons) {
            const USAGE lo = b.IsRange ? b.Range.UsageMin : b.NotRange.Usage;
            const USAGE hi = b.IsRange ? b.Range.UsageMax : b.NotRange.Usage;
            if (b.UsagePage != kSensorPage || usage < lo || usage > hi) continue;
            auto& r = report(b.ReportID);
            USAGE u = usage;
            ULONG n = 1;
            return HidP_SetUsages(HidP_Feature, kSensorPage, b.LinkCollection, &u, &n, ppd, r.data(),
                                  static_cast<ULONG>(r.size())) == HIDP_STATUS_SUCCESS;
        }
        return false;
    };
    select(kTransportAcl);
    const bool power = select(on ? kPowerFull : kPowerOff);
    const bool reporting = select(on ? kReportingAllEvents : kReportingNoEvents);
    if (!interval || !power || !reporting)
        return fail(std::string("descriptor lacks writable ") +
                    (!interval ? "report interval" : !power ? "power state" : "reporting state"));

    for (auto& [id, r] : reports)
        if (!HidD_SetFeature(static_cast<HANDLE>(handle_), r.data(), static_cast<ULONG>(r.size())))
            return fail("feature report " + std::to_string(id) + " rejected " + win32Error(GetLastError()));
    return true;
}

bool HidHeadSensor::decodeRotation(uint8_t* report, uint32_t bytes, double rv[3]) const {
    if (bytes == 0 || (rotation_.reportId != 0 && report[0] != rotation_.reportId)) return false;
    uint8_t packed[kMaxPackedBytes]{};
    if (HidP_GetUsageValueArray(HidP_Input, kSensorPage, rotation_.linkCollection, kRotationVector,
                                reinterpret_cast<PCHAR>(packed), rotation_.packedBytes,
                                static_cast<PHIDP_PREPARSED_DATA>(preparsed_), reinterpret_cast<PCHAR>(report),
                                bytes) != HIDP_STATUS_SUCCESS)
        return false;
    const unsigned bits = rotation_.bitSize;
    for (unsigned i = 0; i < 3; ++i) {
        // HID packs fields little-endian, least significant bit first.
        uint64_t raw = 0;
        for (unsigned b = 0; b < bits; ++b) {
            const unsigned bit = i * bits + b;
            raw |= uint64_t((packed[bit / 8] >> (bit % 8)) & 1u) << b;
        }
        int64_t logical = static_cast<int64_t>(raw);
        if (rotation_.isSigned && ((raw >> (bits - 1)) & 1u)) logical -= int64_t(1) << bits;
        rv[i] = double(logical) * rotation_.scale + rotation_.offset;
        if (!std::isfinite(rv[i])) return false;
    }
    return true;
}

void HidHeadSensor::readerMain() {
    const HANDLE h = static_cast<HANDLE>(handle_);
    const HANDLE io = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!io) {
        logx::error("Head sensor: CreateEvent failed " + win32Error(GetLastError()));
        return;
    }
    const HANDLE waits[2] = {static_cast<HANDLE>(stopEvent_), io};
    std::vector<uint8_t> report(inputBytes_);
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = io;
        if (!ReadFile(h, report.data(), static_cast<DWORD>(report.size()), nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            logx::error("Head sensor: read failed " + win32Error(GetLastError()));
            break;
        }
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(h, &ov);
            DWORD ignored = 0;
            GetOverlappedResult(h, &ov, &ignored, TRUE);  // the driver must release `ov` and `report` first
            break;
        }
        DWORD got = 0;
        if (!GetOverlappedResult(h, &ov, &got, FALSE)) {
            logx::error("Head sensor: read failed " + win32Error(GetLastError()));  // e.g. headphones disconnected
            break;
        }
        double rv[3];
        if (decodeRotation(report.data(), got, rv)) sink_->publish(toOrientation(rv));
    }
    CloseHandle(io);
}

}  // namespace track
