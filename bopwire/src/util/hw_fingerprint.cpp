#include "hw_fingerprint.h"
#include "../crypto/hash.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <iphlpapi.h>
#  include <winioctl.h>
#elif defined(__APPLE__)
#  include <unistd.h>
#  include <sys/utsname.h>
#  include <net/if_dl.h>
#  include <ifaddrs.h>
#  include <IOKit/IOKitLib.h>
#  include <CoreFoundation/CoreFoundation.h>
#else // Linux / other POSIX
#  include <unistd.h>
#  include <sys/utsname.h>
#  include <cstdio>
#  include <dirent.h>
#  include <fstream>
#endif

namespace mc::util {
namespace {

// Fingerprint material + whether at least one genuinely per-UNIT hardware id
// (a serial/UUID that distinguishes two otherwise-identical machines and
// survives an OS reinstall) was folded in this run. `strong` gates the entropy
// tier: material with only weak sources (MAC/OS/hostname/CPU-model) must NOT
// mint a confident, collision-prone device_id — the caller routes it to the
// software tier (per-install random) instead.
struct Collected {
    std::string material;
    bool        strong = false;
};

// Weak / non-unique source (present on many identical machines).
void add(std::string& material, const char* tag, const std::string& value) {
    if (value.empty()) return;
    material += tag;
    material += '=';
    material += value;
    material += '\n';
}

// Per-UNIT source: folds in AND marks the fingerprint strong.
void add_unit(Collected& c, const char* tag, const std::string& value) {
    if (value.empty()) return;
    add(c.material, tag, value);
    c.strong = true;
}

// Reject common OEM/firmware placeholder junk that is shared across a whole
// product line (so it is NOT per-unit). Case-insensitive exact match.
std::string strip_placeholder(std::string v) {
    // trim surrounding whitespace first
    size_t a = v.find_first_not_of(" \t\r\n");
    size_t b = v.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    v = v.substr(a, b - a + 1);
    std::string lo;
    lo.reserve(v.size());
    for (char ch : v) lo.push_back(static_cast<char>(std::tolower((unsigned char)ch)));
    static const char* junk[] = {
        "none", "n/a", "na", "to be filled by o.e.m.", "to be filled by o.e.m",
        "default string", "system serial number", "not specified",
        "not applicable", "o.e.m.", "oem", "0", "00000000", "unknown",
        "system product name", "system manufacturer", "invalid",
    };
    for (const char* j : junk) if (lo == j) return {};
    return v;
}

#if defined(_WIN32)

std::string reg_string(HKEY root, const char* subkey, const char* name) {
    char buf[512];
    DWORD len = sizeof(buf);
    LSTATUS st = RegGetValueA(root, subkey, name,
                              RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                              nullptr, buf, &len);
    if (st != ERROR_SUCCESS) {
        len = sizeof(buf);
        st = RegGetValueA(root, subkey, name, RRF_RT_REG_SZ,
                          nullptr, buf, &len);
    }
    if (st != ERROR_SUCCESS || len == 0) return {};
    return std::string(buf, (len > 0 && buf[len - 1] == '\0') ? len - 1 : len);
}

std::string primary_mac() {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15000;
    std::vector<uint8_t> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size);
    }
    if (r != NO_ERROR) return {};
    std::set<std::string> macs;
    static const char* hx = "0123456789abcdef";
    for (auto* a = addrs; a; a = a->Next) {
        if (a->PhysicalAddressLength != 6) continue;
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD &&
            a->IfType != IF_TYPE_IEEE80211) continue;
        std::string m;
        bool all_zero = true;
        for (int i = 0; i < 6; ++i) {
            uint8_t b = a->PhysicalAddress[i];
            if (b) all_zero = false;
            m.push_back(hx[b >> 4]);
            m.push_back(hx[b & 0xF]);
        }
        if (!all_zero) macs.insert(m);
    }
    return macs.empty() ? std::string() : *macs.begin();
}

// ---- SMBIOS (firmware) via GetSystemFirmwareTable('RSMB') ----
// The returned buffer is a RawSMBIOSData header {u8 Used20CallingMethod; u8
// MajorVersion; u8 MinorVersion; u8 DmiRevision; u32 Length; u8 Table[Length]}.
// Structures: {u8 Type; u8 Length; u16 Handle} + formatted area (Length bytes)
// + a NUL-terminated string set ending in a double NUL.
std::vector<uint8_t> smbios_raw() {
    DWORD sz = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (sz == 0) return {};
    std::vector<uint8_t> buf(sz);
    DWORD got = GetSystemFirmwareTable('RSMB', 0, buf.data(), sz);
    if (got == 0 || got > sz) return {};
    buf.resize(got);
    return buf;
}

// Locate the first structure of `type`; returns its formatted-area pointer and
// (via out params) the string-set bounds. nullptr if not found.
const uint8_t* smbios_find(const std::vector<uint8_t>& raw, uint8_t type,
                           const uint8_t** str_begin, const uint8_t** str_end) {
    if (raw.size() < 8) return nullptr;
    uint32_t tlen = 0;
    std::memcpy(&tlen, raw.data() + 4, 4);
    const uint8_t* p   = raw.data() + 8;
    const uint8_t* end = raw.data() + raw.size();
    if (static_cast<size_t>(8) + tlen <= raw.size()) end = raw.data() + 8 + tlen;
    while (p + 4 <= end) {
        uint8_t stype = p[0];
        uint8_t slen  = p[1];
        if (slen < 4 || p + slen > end) break;
        const uint8_t* strings = p + slen;
        const uint8_t* s = strings;
        while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) ++s;
        const uint8_t* next = (s + 2 <= end) ? s + 2 : end;
        if (stype == type) {
            if (str_begin) *str_begin = strings;
            if (str_end)   *str_end   = next;
            return p;
        }
        if (stype == 127) break; // end-of-table
        p = next;
    }
    return nullptr;
}

std::string smbios_string(const uint8_t* begin, const uint8_t* end, uint8_t index) {
    if (index == 0 || !begin) return {};
    const uint8_t* s = begin;
    for (uint8_t i = 1; i < index && s < end; ++i) {
        while (s < end && *s) ++s;
        if (s < end) ++s; // skip terminating NUL
    }
    std::string out;
    while (s < end && *s) out.push_back(static_cast<char>(*s++));
    return strip_placeholder(out);
}

std::string smbios_system_uuid() {
    auto raw = smbios_raw();
    const uint8_t* sb = nullptr; const uint8_t* se = nullptr;
    const uint8_t* st = smbios_find(raw, 1 /*System Information*/, &sb, &se);
    if (!st || st[1] < 0x18) return {};       // formatted area must reach UUID+16
    const uint8_t* u = st + 0x08;             // UUID at offset 0x08, 16 bytes
    bool all00 = true, allff = true;
    for (int i = 0; i < 16; ++i) { if (u[i] != 0x00) all00 = false; if (u[i] != 0xff) allff = false; }
    if (all00 || allff) return {};
    // First three groups are little-endian (matches dmidecode / wmic csproduct).
    static const int ord[16] = {3,2,1,0, 5,4, 7,6, 8,9,10,11,12,13,14,15};
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        uint8_t b = u[ord[i]];
        s.push_back(hx[b >> 4]); s.push_back(hx[b & 0xF]);
        if (i == 3 || i == 5 || i == 7 || i == 9) s.push_back('-');
    }
    return s;
}

std::string smbios_baseboard_serial() {
    auto raw = smbios_raw();
    const uint8_t* sb = nullptr; const uint8_t* se = nullptr;
    const uint8_t* st = smbios_find(raw, 2 /*Baseboard*/, &sb, &se);
    if (!st || st[1] < 0x08) return {};
    return smbios_string(sb, se, st[0x07]); // Serial Number string index at 0x07
}

std::string disk_serial() {
    HANDLE h = CreateFileA("\\\\.\\PhysicalDrive0", 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    std::vector<uint8_t> buf(1024);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                              buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr);
    CloseHandle(h);
    if (!ok || ret < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return {};
    auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
    if (d->SerialNumberOffset == 0 || d->SerialNumberOffset >= ret) return {};
    const char* sn = reinterpret_cast<const char*>(buf.data()) + d->SerialNumberOffset;
    size_t maxlen = ret - d->SerialNumberOffset;
    std::string s(sn, ::strnlen(sn, maxlen));
    return strip_placeholder(s);
}

Collected collect() {
    Collected c;
    // per-unit (strong)
    add_unit(c, "machineguid",
             reg_string(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid"));
    add_unit(c, "system_uuid",  smbios_system_uuid());
    add_unit(c, "disk_serial",  disk_serial());
    add_unit(c, "board_serial", smbios_baseboard_serial());
    // weak
    add(c.material, "mac", primary_mac());
    add(c.material, "os_product",
        reg_string(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName"));
    add(c.material, "os_build",
        reg_string(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuildNumber"));
    add(c.material, "cpu",
        reg_string(HKEY_LOCAL_MACHINE,
                   "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString"));
    char host[256];
    DWORD hlen = sizeof(host);
    if (GetComputerNameA(host, &hlen)) add(c.material, "host", std::string(host, hlen));
    return c;
}

#elif defined(__APPLE__)

std::string primary_mac() {
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || !ifap) return {};
    std::set<std::string> macs;
    static const char* hx = "0123456789abcdef";
    for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
        auto* sdl = reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
        if (sdl->sdl_alen != 6) continue;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(LLADDR(sdl));
        std::string s; bool all_zero = true;
        for (int i = 0; i < 6; ++i) {
            if (p[i]) all_zero = false;
            s.push_back(hx[p[i] >> 4]); s.push_back(hx[p[i] & 0xF]);
        }
        if (!all_zero) macs.insert(s);
    }
    freeifaddrs(ifap);
    return macs.empty() ? std::string() : *macs.begin();
}

std::string cfstr_to_std(CFStringRef s) {
    if (!s) return {};
    char buf[256];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8))
        return std::string(buf);
    return {};
}

// IOPlatformExpertDevice property (kIOPlatformUUIDKey / kIOPlatformSerialNumberKey).
std::string ioreg_string(CFStringRef key) {
    // 0 == kIOMainPortDefault == kIOMasterPortDefault (both MACH_PORT_NULL);
    // passing 0 avoids the SDK-version rename.
    io_service_t svc = IOServiceGetMatchingService(
        0, IOServiceMatching("IOPlatformExpertDevice"));
    if (!svc) return {};
    CFTypeRef prop = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
    std::string out;
    if (prop) {
        if (CFGetTypeID(prop) == CFStringGetTypeID())
            out = cfstr_to_std(static_cast<CFStringRef>(prop));
        CFRelease(prop);
    }
    IOObjectRelease(svc);
    return strip_placeholder(out);
}

Collected collect() {
    Collected c;
    add_unit(c, "ioplatform_uuid",   ioreg_string(CFSTR(kIOPlatformUUIDKey)));
    add_unit(c, "ioplatform_serial", ioreg_string(CFSTR(kIOPlatformSerialNumberKey)));
    add(c.material, "mac", primary_mac());
    struct utsname u {};
    if (uname(&u) == 0) {
        add(c.material, "os", std::string(u.sysname) + " " + u.release);
        add(c.material, "host", u.nodename);
    }
    return c;
}

#else // Linux / generic POSIX

std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                             line.back() == ' '))
        line.pop_back();
    return line;
}

std::string primary_mac() {
    DIR* d = opendir("/sys/class/net");
    if (!d) return {};
    std::set<std::string> macs;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == ".." || name == "lo") continue;
        std::string mac = read_first_line(
            std::string("/sys/class/net/") + name + "/address");
        std::string norm; norm.reserve(12);
        for (char c : mac) if (c != ':') norm.push_back((char)tolower((unsigned char)c));
        if (norm.size() == 12 && norm != "000000000000") macs.insert(norm);
    }
    closedir(d);
    return macs.empty() ? std::string() : *macs.begin();
}

// Disk serial via /dev/disk/by-id symlink NAMES (readable without root; the
// name encodes the drive serial). Skip partitions; prefer a stable ordering.
std::string disk_by_id() {
    DIR* d = opendir("/dev/disk/by-id");
    if (!d) return {};
    std::set<std::string> ids;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (n.find("-part") != std::string::npos) continue; // partition, not the disk
        if (n.rfind("ata-", 0) == 0 || n.rfind("nvme-", 0) == 0 ||
            n.rfind("scsi-", 0) == 0 || n.rfind("wwn-", 0) == 0 ||
            n.rfind("mmc-", 0) == 0 || n.rfind("usb-", 0) == 0)
            ids.insert(n);
    }
    closedir(d);
    return ids.empty() ? std::string() : *ids.begin();
}

Collected collect() {
    Collected c;
    // machine-id (per-install); fall back to the dbus copy for containers /
    // images where /etc/machine-id was zeroed.
    std::string mid = read_first_line("/etc/machine-id");
    if (mid.empty()) mid = read_first_line("/var/lib/dbus/machine-id");
    add_unit(c, "machine_id", mid);
    // DMI serials — often root-only (empty for an unprivileged app), and full of
    // OEM placeholders, so strip_placeholder guards them.
    add_unit(c, "product_uuid",   strip_placeholder(read_first_line("/sys/class/dmi/id/product_uuid")));
    add_unit(c, "product_serial", strip_placeholder(read_first_line("/sys/class/dmi/id/product_serial")));
    add_unit(c, "board_serial",   strip_placeholder(read_first_line("/sys/class/dmi/id/board_serial")));
    add_unit(c, "disk_id",        disk_by_id());
    // weak
    add(c.material, "mac", primary_mac());
    struct utsname u {};
    if (uname(&u) == 0) {
        add(c.material, "os", std::string(u.sysname) + " " + u.release);
        add(c.material, "host", u.nodename);
    }
    return c;
}

#endif

} // namespace

DeviceFingerprint device_fingerprint_ex() {
    Collected c = collect();
    DeviceFingerprint out;
    if (c.material.empty()) return out;   // hex empty, strong=false
    Hash256 h = crypto::sha256(
        reinterpret_cast<const uint8_t*>(c.material.data()), c.material.size());
    out.hex    = crypto::to_hex(h);
    out.strong = c.strong;
    return out;
}

std::string device_fingerprint_hex() {
    return device_fingerprint_ex().hex;
}

} // namespace mc::util
