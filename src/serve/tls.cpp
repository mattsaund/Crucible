// SPDX-License-Identifier: MIT
//
// HTTPS for the API server. See tls.hpp.
//
// Everything here is Mbed TLS's: the key, the certificate made from it, and
// the reading of a person's own. What the listener does with them is
// cpp-httplib's (see server.cpp), on the same library.
#include "crucible/serve/tls.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>

#include <mbedtls/asn1write.h>
#include <mbedtls/build_info.h>
#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
// The one header of theirs without C linkage said for C++, in 3.6.
extern "C" {
#include <mbedtls/psa_util.h>
}

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mbedtls/threading.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "crucible/util/platform.hpp"

namespace crucible::serve::tls {
namespace {

constexpr std::time_t kDay = 24 * 60 * 60;

/// Larger than any certificate chain or key: a file past this is not one,
/// and is not read whole to find that out.
constexpr std::uintmax_t kLargestFile = std::uintmax_t{1} << 20;

/// The most names a certificate made here carries. A machine with a bridge
/// for every container could have hundreds of addresses, none of them where
/// a client finds it.
constexpr std::size_t kMostNames = 64;

#if defined(_WIN32)
// Mbed TLS's lock on Windows: a slim reader/writer lock, held exclusively.
// threading_alt.h keeps it as the one pointer it is, so that the headers do
// not need windows.h. That the two agree is checked here.
static_assert(sizeof(SRWLOCK) == sizeof(mbedtls_threading_mutex_t), "threading_alt.h no longer fits an SRWLOCK");

SRWLOCK* srw(mbedtls_threading_mutex_t* mutex) {
    return reinterpret_cast<SRWLOCK*>(mutex);
}
void lock_init(mbedtls_threading_mutex_t* mutex) {
    InitializeSRWLock(srw(mutex));
}
void lock_free(mbedtls_threading_mutex_t*) {}   // an SRWLOCK holds nothing to free
int lock_take(mbedtls_threading_mutex_t* mutex) {
    AcquireSRWLockExclusive(srw(mutex));
    return 0;
}
int lock_give(mbedtls_threading_mutex_t* mutex) {
    ReleaseSRWLockExclusive(srw(mutex));
    return 0;
}
#endif

/// A chain of certificates, freed with it.
struct Chain {
    mbedtls_x509_crt crt;
    Chain() { mbedtls_x509_crt_init(&crt); }
    ~Chain() { mbedtls_x509_crt_free(&crt); }
    Chain(const Chain&)            = delete;
    Chain& operator=(const Chain&) = delete;
};

/// A key, freed with it.
struct Key {
    mbedtls_pk_context pk;
    Key() { mbedtls_pk_init(&pk); }
    ~Key() { mbedtls_pk_free(&pk); }
    Key(const Key&)            = delete;
    Key& operator=(const Key&) = delete;
};

/// A certificate being written, freed with it.
struct Writer {
    mbedtls_x509write_cert crt;
    Writer() { mbedtls_x509write_crt_init(&crt); }
    ~Writer() { mbedtls_x509write_crt_free(&crt); }
    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;
};

const unsigned char* bytes_of(const std::string& text) {
    return reinterpret_cast<const unsigned char*>(text.c_str());
}

/// PEM is parsed as text, its closing zero counted in; anything else is DER,
/// taken as it is.
bool is_pem(const std::string& text) {
    return text.find("-----BEGIN ") != std::string::npos;
}

/// The certificates in `text` into `chain`: true when at least one was read.
bool read_chain(const std::string& text, Chain& chain) {
    if (text.empty()) {
        return false;
    }
    const int ret = mbedtls_x509_crt_parse(&chain.crt, bytes_of(text), is_pem(text) ? text.size() + 1 : text.size());
    return ret >= 0 && chain.crt.raw.len > 0;
}

/// The private key in `text` into `key`: Mbed TLS's code for why not, or 0.
int read_key(const std::string& text, Key& key) {
    if (text.empty()) {
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    }
    return mbedtls_pk_parse_key(&key.pk, bytes_of(text), is_pem(text) ? text.size() + 1 : text.size(), nullptr, 0,
                                mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
}

bool read_file(const std::filesystem::path& file, std::string& out) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size > kLargestFile) {
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// --- dates -------------------------------------------------------------------

/// Days since 1970-01-01 of a day in the Gregorian calendar, and back: the
/// arithmetic of Howard Hinnant's chrono-compatible algorithms, since a
/// certificate's dates are UTC and gmtime and timegm are not the same on
/// every system.
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era  = (year >= 0 ? year : year - 399) / 400;
    const auto         yoe  = static_cast<unsigned>(year - era * 400);
    const unsigned     doy  = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned     doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto         doe = static_cast<unsigned>(days - era * 146097);
    const unsigned     yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned     doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned     mp  = (5 * doy + 2) / 153;
    day   = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year  = static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
}

/// `when` as a certificate's validity is written: YYYYMMDDHHMMSS, in UTC.
std::string validity_text(std::time_t when) {
    const std::int64_t seconds = static_cast<std::int64_t>(when);
    std::int64_t       days    = seconds / kDay;
    std::int64_t       rest    = seconds % kDay;
    if (rest < 0) {
        rest += kDay;
        --days;
    }
    std::int64_t year  = 0;
    unsigned     month = 0;
    unsigned     day   = 0;
    civil_from_days(days, year, month, day);
    char text[32];
    std::snprintf(text, sizeof(text), "%04lld%02u%02u%02lld%02lld%02lld", static_cast<long long>(year), month, day,
                  static_cast<long long>(rest / 3600), static_cast<long long>(rest % 3600 / 60),
                  static_cast<long long>(rest % 60));
    return text;
}

std::time_t seconds_of(const mbedtls_x509_time& at) {
    if (at.mon < 1 || at.mon > 12 || at.day < 1 || at.day > 31) {
        return 0;
    }
    const std::int64_t days = days_from_civil(at.year, static_cast<unsigned>(at.mon), static_cast<unsigned>(at.day));
    return static_cast<std::time_t>(days * kDay + at.hour * 3600 + at.min * 60 + at.sec);
}

// --- names -------------------------------------------------------------------

/// An address as text: dotted for IPv4, and IPv6 as RFC 5952 writes it,
/// lower case with the longest run of zero groups as "::".
std::string ip_text(const unsigned char* bytes, std::size_t size) {
    char piece[16];
    if (size == 4) {
        std::snprintf(piece, sizeof(piece), "%u.%u.%u.%u", unsigned{bytes[0]}, unsigned{bytes[1]}, unsigned{bytes[2]},
                      unsigned{bytes[3]});
        return piece;
    }
    if (size != 16) {
        return {};
    }
    std::array<unsigned, 8> groups{};
    for (std::size_t i = 0; i < groups.size(); ++i) {
        groups[i] = (unsigned{bytes[2 * i]} << 8) | unsigned{bytes[2 * i + 1]};
    }
    std::size_t gap = groups.size();   // where "::" goes, or nowhere
    std::size_t gap_length = 0;
    for (std::size_t i = 0; i < groups.size();) {
        if (groups[i] != 0) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < groups.size() && groups[end] == 0) {
            ++end;
        }
        if (end - i >= 2 && end - i > gap_length) {
            gap        = i;
            gap_length = end - i;
        }
        i = end;
    }
    std::string out;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (i == gap) {
            out += "::";
            i += gap_length - 1;
            continue;
        }
        if (!out.empty() && out.back() != ':') {
            out += ':';
        }
        std::snprintf(piece, sizeof(piece), "%x", groups[i]);
        out += piece;
    }
    return out;
}

/// `name` in lower case when a certificate may carry it: letters, digits,
/// hyphens and dots. Empty when not, which a name with a space or an
/// underscore in it is.
std::string dns_name(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto letter = static_cast<unsigned char>(c);
        if (std::isalnum(letter) == 0 && c != '-' && c != '.') {
            return {};
        }
        out += static_cast<char>(std::tolower(letter));
    }
    if (out.empty() || out.size() > 253 || out.front() == '.' || out.back() == '.' || out.front() == '-') {
        return {};
    }
    return out;
}

/// An address a client could reach this machine at, as its bytes, or false
/// for one it could not: the unspecified address, an IPv6 link-local one,
/// which is reached only with the interface named, and the loopback, which
/// this_machine() names itself.
bool reachable(const sockaddr* address, std::array<unsigned char, 16>& bytes, std::size_t& size) {
    if (address == nullptr) {
        return false;
    }
    // Copied out rather than cast, since a pointer to a sockaddr promises
    // only a sockaddr's alignment, which is less than the longer kinds need.
    if (address->sa_family == AF_INET) {
        sockaddr_in four{};
        std::memcpy(&four, address, sizeof(four));
        std::memcpy(bytes.data(), &four.sin_addr, 4);
        size = 4;
        return bytes[0] != 0 && bytes[0] != 127;
    }
    if (address->sa_family == AF_INET6) {
        sockaddr_in6 six{};
        std::memcpy(&six, address, sizeof(six));
        std::memcpy(bytes.data(), &six.sin6_addr, 16);
        size = 16;
        const bool link_local = bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
        const bool multicast  = bytes[0] == 0xff;
        const bool low        = std::all_of(bytes.begin(), bytes.begin() + 10, [](unsigned char b) { return b == 0; });
        // ::, ::1, and IPv4 written as IPv6, which is named as IPv4 already.
        return !link_local && !multicast && !low;
    }
    return false;
}

/// Every address this machine has on a network that is up.
std::vector<std::string> addresses() {
    std::vector<std::string> out;
    std::array<unsigned char, 16> bytes{};
    std::size_t                   size = 0;
#if defined(_WIN32)
    ULONG                      length = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG                      result = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; ++tries) {
        buffer.resize(length);
        result = GetAdaptersAddresses(AF_UNSPEC,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &length);
    }
    if (result == NO_ERROR) {
        for (const auto* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) {
                continue;
            }
            for (const auto* one = adapter->FirstUnicastAddress; one != nullptr; one = one->Next) {
                if (reachable(one->Address.lpSockaddr, bytes, size)) {
                    out.push_back(ip_text(bytes.data(), size));
                }
            }
        }
    }
#else
    ifaddrs* all = nullptr;
    if (getifaddrs(&all) == 0) {
        for (const ifaddrs* one = all; one != nullptr; one = one->ifa_next) {
            if ((one->ifa_flags & IFF_UP) != 0 && reachable(one->ifa_addr, bytes, size)) {
                out.push_back(ip_text(bytes.data(), size));
            }
        }
        freeifaddrs(all);
    }
#endif
    return out;
}

/// This machine's own name, as the system has it: never looked up.
std::vector<std::string> host_names() {
    std::vector<std::string> out;
#if defined(_WIN32)
    for (const COMPUTER_NAME_FORMAT format : {ComputerNameDnsHostname, ComputerNameDnsFullyQualified}) {
        char  name[256] = {};
        DWORD size      = sizeof(name);
        if (GetComputerNameExA(format, name, &size) != 0) {
            out.emplace_back(name, size);
        }
    }
#else
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) == 0) {
        out.emplace_back(name);
    }
#endif
    return out;
}

void add_once(std::vector<std::string>& list, const std::string& value) {
    if (!value.empty() && std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

/// The identity a listener is given, filled in from `pem`.
void fill(Identity& out, const Pem& pem, const std::filesystem::path& file, bool own_one) {
    out.pem         = pem;
    out.file        = file;
    out.fingerprint = fingerprint(pem.certificate);
    out.names       = names_in(pem.certificate);
    out.own         = own_one;
}

}  // namespace

bool ready() {
    // Once, and before any other call into Mbed TLS: on Windows its locks
    // are this file's, and are handed over first, since the PSA half takes
    // them as it starts.
    static const bool started = [] {
#if defined(_WIN32)
        mbedtls_threading_set_alt(lock_init, lock_free, lock_take, lock_give);
#endif
        return psa_crypto_init() == PSA_SUCCESS;
    }();
    return started;
}

Names this_machine() {
    Names out;
    add_once(out.dns, "localhost");
    for (const std::string& raw : host_names()) {
        const std::string name = dns_name(raw);
        add_once(out.dns, name);
        // Where the network answers for the machine by name itself, mDNS on a
        // Mac or under Avahi, it is the name with .local after it.
        if (!name.empty() && name.find('.') == std::string::npos) {
            add_once(out.dns, name + ".local");
        }
    }
    add_once(out.ip, "127.0.0.1");
    add_once(out.ip, "::1");
    for (const std::string& address : addresses()) {
        add_once(out.ip, address);
    }
    return out;
}

bool make(const Names& names, int days, Pem& out, std::string& error) {
    if (!ready()) {
        error = "HTTPS could not start";
        return false;
    }
    Key key;
    if (mbedtls_pk_setup(&key.pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0
        || mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key.pk), mbedtls_psa_get_random,
                               MBEDTLS_PSA_RANDOM_STATE) != 0) {
        error = "Could not make a key";
        return false;
    }

    // The names, as Mbed TLS takes them: a list, each node pointing into
    // the text or the bytes it names, which outlive it here.
    std::vector<std::array<unsigned char, 16>> ip_bytes;
    std::vector<std::pair<int, std::pair<const unsigned char*, std::size_t>>> entries;
    for (const std::string& name : names.dns) {
        if (entries.size() < kMostNames && !name.empty()) {
            entries.push_back({MBEDTLS_X509_SAN_DNS_NAME, {bytes_of(name), name.size()}});
        }
    }
    ip_bytes.reserve(names.ip.size());
    for (const std::string& address : names.ip) {
        std::array<unsigned char, 16> bytes{};
        const std::size_t size = mbedtls_x509_crt_parse_cn_inet_pton(address.c_str(), bytes.data());
        if (entries.size() < kMostNames && (size == 4 || size == 16)) {
            ip_bytes.push_back(bytes);
            entries.push_back({MBEDTLS_X509_SAN_IP_ADDRESS, {ip_bytes.back().data(), size}});
        }
    }
    std::vector<mbedtls_x509_san_list> list(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        list[i]                                = mbedtls_x509_san_list{};
        list[i].node.type                      = entries[i].first;
        list[i].node.san.unstructured_name.p   = const_cast<unsigned char*>(entries[i].second.first);
        list[i].node.san.unstructured_name.len = entries[i].second.second;
        list[i].next                           = i + 1 < entries.size() ? &list[i + 1] : nullptr;
    }

    // A serial no other certificate has: random, and positive, as RFC 5280
    // asks of one.
    std::array<unsigned char, 16> serial{};
    if (mbedtls_psa_get_random(MBEDTLS_PSA_RANDOM_STATE, serial.data(), serial.size()) != 0) {
        error = "Could not make a certificate";
        return false;
    }
    serial[0] = static_cast<unsigned char>((serial[0] & 0x7f) | 0x40);

    // Named for the machine, so that two of them trusted on one client are
    // told apart in its list: its name up to the first dot.
    std::string subject = "CN=Crucible";
    for (const std::string& name : names.dns) {
        if (name != "localhost") {
            subject += " on " + name.substr(0, name.find('.'));
            break;
        }
    }

    // What a client asks of a server's certificate and no more: for TLS
    // servers (the extended key usage Apple's systems insist on), not a
    // certificate authority, and with identifiers for the key, which a
    // strict client looks for. No key usage, which an older OpenSSL would
    // hold against a certificate that signs itself.
    unsigned char         server_auth[] = MBEDTLS_OID_SERVER_AUTH;
    mbedtls_asn1_sequence usage{};
    usage.buf.tag = MBEDTLS_ASN1_OID;
    usage.buf.p   = server_auth;
    usage.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);

    const std::time_t now = std::time(nullptr);
    Writer            writer;
    mbedtls_x509write_crt_set_version(&writer.crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&writer.crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&writer.crt, &key.pk);
    mbedtls_x509write_crt_set_issuer_key(&writer.crt, &key.pk);
    // A day early, for a client whose clock is behind this one's.
    const std::string from  = validity_text(now - kDay);
    const std::string until = validity_text(now + static_cast<std::time_t>(days) * kDay);
    if (mbedtls_x509write_crt_set_serial_raw(&writer.crt, serial.data(), serial.size()) != 0
        || mbedtls_x509write_crt_set_subject_name(&writer.crt, subject.c_str()) != 0
        || mbedtls_x509write_crt_set_issuer_name(&writer.crt, subject.c_str()) != 0
        || mbedtls_x509write_crt_set_validity(&writer.crt, from.c_str(), until.c_str()) != 0
        || mbedtls_x509write_crt_set_basic_constraints(&writer.crt, 0, -1) != 0
        || mbedtls_x509write_crt_set_subject_key_identifier(&writer.crt) != 0
        || mbedtls_x509write_crt_set_authority_key_identifier(&writer.crt) != 0
        || mbedtls_x509write_crt_set_ext_key_usage(&writer.crt, &usage) != 0
        || (!list.empty() && mbedtls_x509write_crt_set_subject_alternative_name(&writer.crt, list.data()) != 0)) {
        error = "Could not make a certificate";
        return false;
    }

    std::vector<unsigned char> text(16 * 1024);
    if (mbedtls_x509write_crt_pem(&writer.crt, text.data(), text.size(), mbedtls_psa_get_random,
                                  MBEDTLS_PSA_RANDOM_STATE) != 0) {
        error = "Could not make a certificate";
        return false;
    }
    Pem made;
    made.certificate = reinterpret_cast<const char*>(text.data());
    std::fill(text.begin(), text.end(), 0);
    if (mbedtls_pk_write_key_pem(&key.pk, text.data(), text.size()) != 0) {
        error = "Could not make a key";
        return false;
    }
    made.key = reinterpret_cast<const char*>(text.data());
    std::fill(text.begin(), text.end(), 0);   // a key's copy left in memory is one more
    out = std::move(made);
    return true;
}

std::string prepare(const Pem& raw, Pem& out) {
    if (!ready()) {
        return "HTTPS could not start";
    }
    Chain chain;
    if (!read_chain(raw.certificate, chain)) {
        return "Not a certificate";
    }
    Key       key;
    const int read = read_key(raw.key, key);
    if (read == MBEDTLS_ERR_PK_PASSWORD_REQUIRED || read == MBEDTLS_ERR_PK_PASSWORD_MISMATCH) {
        return "The key has a password";
    }
    if (read != 0) {
        return "Not a key";
    }
    if (mbedtls_pk_check_pair(&chain.crt.pk, &key.pk, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE) != 0) {
        return "The key does not match the certificate";
    }
    if (mbedtls_x509_time_is_past(&chain.crt.valid_to) != 0) {
        return "The certificate has expired";
    }

    Pem written;
    for (const mbedtls_x509_crt* one = &chain.crt; one != nullptr && one->raw.len > 0; one = one->next) {
        std::vector<unsigned char> text(one->raw.len * 2 + 128);
        std::size_t                length = 0;
        if (mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n", one->raw.p,
                                     one->raw.len, text.data(), text.size(), &length) != 0
            || length == 0) {
            return "Not a certificate";
        }
        written.certificate.append(reinterpret_cast<const char*>(text.data()), length - 1);   // less its zero
    }
    std::vector<unsigned char> text(16 * 1024);
    if (mbedtls_pk_write_key_pem(&key.pk, text.data(), text.size()) != 0) {
        return "Not a key";
    }
    written.key = reinterpret_cast<const char*>(text.data());
    std::fill(text.begin(), text.end(), 0);
    out = std::move(written);
    return {};
}

std::string fingerprint(const std::string& certificate) {
    if (!ready()) {
        return {};
    }
    Chain chain;
    if (!read_chain(certificate, chain)) {
        return {};
    }
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256(chain.crt.raw.p, chain.crt.raw.len, digest.data(), 0) != 0) {
        return {};
    }
    std::string out;
    char        pair[4];
    for (const unsigned char byte : digest) {
        std::snprintf(pair, sizeof(pair), "%02X", unsigned{byte});
        if (!out.empty()) {
            out += ':';
        }
        out += pair;
    }
    return out;
}

std::time_t expires(const std::string& certificate) {
    if (!ready()) {
        return 0;
    }
    Chain chain;
    if (!read_chain(certificate, chain)) {
        return 0;
    }
    return seconds_of(chain.crt.valid_to);
}

std::vector<std::string> names_in(const std::string& certificate) {
    std::vector<std::string> out;
    if (!ready()) {
        return out;
    }
    Chain chain;
    if (!read_chain(certificate, chain)) {
        return out;
    }
    for (const mbedtls_x509_sequence* one = &chain.crt.subject_alt_names; one != nullptr && one->buf.p != nullptr;
         one = one->next) {
        mbedtls_x509_subject_alternative_name name{};
        if (mbedtls_x509_parse_subject_alt_name(&one->buf, &name) != 0) {
            continue;   // a kind of name Mbed TLS does not read, which a client would not match either
        }
        const mbedtls_x509_buf& value = name.san.unstructured_name;
        if (name.type == MBEDTLS_X509_SAN_DNS_NAME) {
            out.emplace_back(reinterpret_cast<const char*>(value.p), value.len);
        } else if (name.type == MBEDTLS_X509_SAN_IP_ADDRESS) {
            add_once(out, ip_text(value.p, value.len));
        }
        mbedtls_x509_free_subject_alt_name(&name);
    }
    return out;
}

bool own(const std::filesystem::path& dir, bool renew, Identity& out, std::string& error) {
    const std::filesystem::path certificate_file = dir / "certificate.pem";
    const std::filesystem::path key_file         = dir / "key.pem";
    if (!renew) {
        Pem raw;
        Pem kept;
        if (read_file(certificate_file, raw.certificate) && read_file(key_file, raw.key) && prepare(raw, kept).empty()
            && expires(kept.certificate) - std::time(nullptr) > static_cast<std::time_t>(kRenewDays) * kDay) {
            fill(out, kept, certificate_file, true);
            return true;
        }
    }
    Pem made;
    if (!make(this_machine(), kOwnDays, made, error)) {
        return false;
    }
    // A folder only this user can open, before the key goes in it. Where the
    // file system keeps no such thing, as on Windows, the folder is inside
    // the user's own already.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    std::string why;
    if (!util::write_file_atomically(key_file, made.key, why)
        || !util::write_file_atomically(certificate_file, made.certificate, why)) {
        error = "Could not keep the certificate: " + why;
        return false;
    }
    std::filesystem::permissions(key_file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    fill(out, made, certificate_file, true);
    return true;
}

bool load(const std::filesystem::path& certificate_file, const std::filesystem::path& key_file,
          const std::filesystem::path& own_dir, Identity& out, std::string& error) {
    if (!ready()) {
        error = "HTTPS could not start";
        return false;
    }
    if (certificate_file.empty()) {
        return own(own_dir, false, out, error);
    }
    Pem raw;
    if (!read_file(certificate_file, raw.certificate)) {
        error = "Cannot read the certificate";
        return false;
    }
    if (key_file.empty()) {
        raw.key = raw.certificate;   // the key after the certificate, in one file
    } else if (!read_file(key_file, raw.key)) {
        error = "Cannot read the key";
        return false;
    }
    Pem prepared;
    if (std::string why = prepare(raw, prepared); !why.empty()) {
        error = std::move(why);
        return false;
    }
    fill(out, prepared, certificate_file, false);
    return true;
}

}  // namespace crucible::serve::tls
