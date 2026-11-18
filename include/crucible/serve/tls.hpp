// SPDX-License-Identifier: MIT
//
// HTTPS for the API server: the certificate it shows a client, and the key
// that proves the server is the one the certificate names.
//
// Either a certificate the person picked, as files, or Crucible's own: made
// the first time HTTPS is turned on, for this machine's names and addresses,
// and kept, so that a client told to trust it once goes on trusting it after
// a restart. Made again, with the same key, once the machine is reached by
// an address it does not name (see own). Nobody vouches for Crucible's own
// but Crucible (it is self-signed), so a client is given the file to trust,
// or its SHA-256 to pin, and Settings, Server shows both.
//
// Mbed TLS underneath, compiled in (see cmake/CrucibleDependencies.cmake):
// the same library cpp-httplib's HTTPS is made with.
#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace crucible::serve::tls {

/// How long Crucible's own certificate is good for: under the 825 days
/// Apple's systems take from any server's certificate, one somebody trusts
/// by hand included.
constexpr int kOwnDays = 800;

/// Made again when no more than this many days of it are left, so that a
/// client is not met by one that ran out.
constexpr int kRenewDays = 30;

/// A certificate and its private key, as PEM text: the certificate first,
/// and any it is signed under after it.
struct Pem {
    std::string certificate;
    std::string key;

    bool operator==(const Pem&) const = default;
};

/// What the server listens with.
struct Identity {
    Pem                   pem;
    std::filesystem::path file;          ///< the certificate's, to give a client
    std::string           fingerprint;   ///< its SHA-256, "AB:CD:..."
    std::vector<std::string> names;      ///< the names and addresses it is for
    bool                  own = false;   ///< made by Crucible
};

/// What a certificate made here is for: the names a client may reach this
/// machine by, and its addresses as text.
struct Names {
    std::vector<std::string> dns;
    std::vector<std::string> ip;
};

/// This machine's address on its network, as a client there reaches it:
/// IPv4, or IPv6 when `ipv6`. The one a packet to the outside leaves from,
/// or where there is no route out, that of the first interface that is up.
/// Never an IPv6 temporary address (RFC 8981) where the machine has a
/// lasting one: the system keeps those for its own connections out and
/// replaces them within a day or two. Empty when there is no network, or
/// none of that kind.
std::string network_address(bool ipv6);

/// What a client reaches this machine by, which Crucible's own certificate
/// must name: localhost and the machine's own name, 127.0.0.1 and ::1, and
/// its address on each kind of network (see network_address). Not every
/// address the machine has: one for each container's bridge comes and goes,
/// and none of them is where a client finds it.
Names reached_by();

/// This machine's: what it is reached by, and the address of each network
/// it is on, less IPv6 addresses it is replacing. Asked of the machine
/// itself, never of a name server: a lookup of the machine's own name is
/// half a minute on some.
Names this_machine();

/// A self-signed certificate for `names`, good from a day ago (a clock
/// behind this one's) for `days`, and its key: the one in `key`, as PEM, or
/// a new one when that is empty, an ECDSA key on P-256, which every client
/// of the last decade takes.
bool make(const Names& names, int days, Pem& out, std::string& error, const std::string& key = {});

/// `raw`, read as a certificate and a key, PEM or DER, and written again as
/// PEM, which is what the listener takes: the certificate and those after
/// it, and the key. Why it will not serve, in a few words, or empty: not a
/// certificate, not a key, a key with a password, a key that is not the
/// certificate's, a certificate that has run out.
std::string prepare(const Pem& raw, Pem& out);

/// The first certificate's SHA-256, as a client shows one: 32 pairs of
/// capital hex digits with colons between. Empty when it cannot be read.
std::string fingerprint(const std::string& certificate);

/// When the first certificate stops being good, or 0 when it cannot be read.
std::time_t expires(const std::string& certificate);

/// The names and addresses the first certificate is for, as text.
std::vector<std::string> names_in(const std::string& certificate);

/// Whether the first certificate names each of `needed`, a name in any case.
bool names_all(const std::string& certificate, const Names& needed);

/// Whether Crucible's own certificate still serves this machine: it names
/// everything the machine is reached by, and has more than kRenewDays left.
bool serves_here(const std::string& certificate);

/// Crucible's own certificate and key, kept in `dir`: the ones there when
/// they read, belong together and still serve here (see serves_here). When
/// they no longer serve, a certificate made again for this machine as it is
/// now, with the names the old one had as well, and signed with the same
/// key: a client that pins the key, or trusts the old certificate (by its
/// subject and its key, as OpenSSL does), goes on trusting the new one. Its
/// own SHA-256 changes all the same, since the names are in what is hashed.
/// A new key with it only when there is none to keep, or when `renew` asks
/// for one. Written to `dir`, the key so that only this user can read it.
bool own(const std::filesystem::path& dir, bool renew, Identity& out, std::string& error);

/// What the server listens with: the certificate in `certificate_file`
/// and the key in `key_file`, which may be empty when the key follows the
/// certificate in its file; or Crucible's own in `own_dir` when neither is
/// named. False, with `error` saying why in a few words, when they cannot
/// be read or will not serve.
bool load(const std::filesystem::path& certificate_file, const std::filesystem::path& key_file,
          const std::filesystem::path& own_dir, Identity& out, std::string& error);

/// Mbed TLS readied for many threads and its PSA half started, once: before
/// anything else is asked of it, a listener included. False when it could
/// not be.
bool ready();

namespace detail {

/// The addresses /proc/net/if_inet6 marks temporary or deprecated, as text:
/// how Linux says which of its IPv6 addresses it is letting go. One address
/// a line, as 32 hex digits, its flags in the fifth field.
std::vector<std::string> passing_in(const std::string& if_inet6);

}  // namespace detail

}  // namespace crucible::serve::tls
