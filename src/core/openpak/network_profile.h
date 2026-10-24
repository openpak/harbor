// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (network_profile.h).

// Which names the emulated console redirects and where they go, as OpenPak publishes it.
//
// The built-in list is a fallback, not the truth: OpenPak generates this from its own live
// routing, so a title served on a new hostname works without a new emulator build. Two parts
// matter beyond the wildcards -- an override sends one name to a different address (the NAT
// check compares what TWO addresses observe of one console, so its second probe must not
// collapse onto the first), and `never` names must reach the real internet, because a
// connection test pointed at OpenPak measures OpenPak.

#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OpenPak::NetworkProfile {

struct Service {
    std::string id;   // "act", "ecs", "nus", "ias", "ccs", "ccsu", "idbe", "boss", "tagaya", "olv"
    std::string url;  // what a client that addresses a URL (Cemu) uses for that service
};

struct Profile {
    long long version = 0;
    long long generated_at = 0;
    std::string platform;

    std::string server_address;  // a literal IPv4/IPv6 address, never a name
    int https_port = 443;
    int http_port = 80;

    std::string ca_url;        // empty means "no CA shipping in this profile"
    std::string ca_sha256;     // hex; the CA is only trusted when its digest matches
    long long ca_not_after = 0;

    std::vector<std::string> suffixes;  // ".nintendo.net" — every name under the family
    std::vector<std::string> exact;     // "gamesync.npln.nintendo.net" — one name
    std::vector<std::string> never;     // names that must reach the real internet
    // A name with an address of its own, ahead of the wildcards: (name, literal address).
    std::vector<std::pair<std::string, std::string>> overrides;

    std::vector<Service> services;
    long long recheck_after = 21600;    // seconds; a hint, not a contract
};

// Where the applied profile came from. A player's "it can't connect" is answered by logging
// this and the profile version — see Fetch().
enum class Source {
    Fetched,  // answered 200, validated, applied and cached
    Cached,   // 304, or the fetch failed and last-known-good is in use
    BuiltIn,  // nothing stored; the compiled-in default is in use
};

struct Result {
    bool ok = false;        // false only when even the built-in default could not be produced,
                            // which currently cannot happen; kept for future-proofing callers
    Source source = Source::BuiltIn;
    // The site answered for the profile in use now: a fresh copy (Fetched) or a 304 for the
    // stored one (Cached). False for no answer, an HTTP error or a refused body.
    bool confirmed = false;
    Profile profile;
    std::string error;      // why the fetch or the profile fell back, for the log
};

// ---------------------------------------------------------------------------------------------
// The ceiling (docs/signed-ceiling.md): per platform, the domain families this client may redirect
// at all. It is a file signed with an Ed25519 key that never touches the server, fetched from the
// pinned openpak.org origin at boot and cached; the compiled-in list below is only the first-boot
// fallback, used while no verified ceiling has ever been cached, and is never updated again.
// The profile chooses within the ceiling: a name outside it is dropped, individually and logged,
// never the whole profile.

// A public key a ceiling may be signed with, and its keyid (hex sha256 of the raw key).
struct PinnedKey {
    std::array<unsigned char, 32> key{};
    std::string keyid;
};
PinnedKey MakePinnedKey(std::array<unsigned char, 32> const& raw);

// The keys a ceiling is accepted under: a list, so a successor can be pinned before the current
// key is retired.
std::vector<PinnedKey> const& PinnedKeys();

// One signed destination rewrite (the ceiling's "ip_redirects"): a title that dials `from`, a
// hard-coded IPv4 address and port rather than a name, is connected to `to` instead. Only a listed
// address:port is rewritten, and only to the target the signed file names, so the box cannot widen
// it either.
struct IpRedirect {
    std::array<unsigned char, 4> from_ip{};
    unsigned short from_port = 0;
    std::array<unsigned char, 4> to_ip{};
    unsigned short to_port = 0;
    bool tcp = true;  // "proto": "tcp" (a connect) or "udp" (connect or sendto on a datagram socket)
};

struct Ceiling {
    long long version = 0;  // 0: the compiled fallback, never signed
    std::string issued;
    std::map<std::string, std::vector<std::string>> platforms;
    std::map<std::string, std::vector<IpRedirect>> ip_redirects;  // none in the compiled fallback

    // The platform's families; none for a platform the ceiling does not name.
    std::vector<std::string> const& FamiliesFor(std::string const& platform) const;
    // The platform's ip redirects; none for a platform the ceiling does not name.
    std::vector<IpRedirect> const& IpRedirectsFor(std::string const& platform) const;
};

// "a.b.c.d:port" with a dotted-quad IPv4 address (no leading zeros) and a port 1-65535.
bool ParseIpv4Endpoint(std::string const& text, std::array<unsigned char, 4>& ip,
                       unsigned short& port);

// The target of this platform's signed ip redirect for a destination (port in host order), from
// CurrentCeiling(); nothing when the destination is not listed for that protocol.
std::optional<std::pair<std::array<unsigned char, 4>, unsigned short>> IpRedirectFor(
    std::array<unsigned char, 4> const& ip, unsigned short port, bool tcp,
    std::string const& platform);

enum class CeilingSource {
    Fetched,  // fetched now, verified, not older than anything accepted before, cached
    Cached,   // the cached envelope, re-verified on load
    BuiltIn,  // nothing verified was ever cached: the compiled fallback
};
char const* CeilingSourceName(CeilingSource s);

struct CeilingResult {
    CeilingSource source = CeilingSource::BuiltIn;
    Ceiling ceiling;
    std::string error;  // why the fetched envelope (or the cache) was not used, for the log
};

// The compiled fallback: the families every earlier build carried, for every platform.
Ceiling CompiledCeiling();

// The contract's family rule: a leading dot, lower case, at least two non-empty labels of hostname
// characters. ".ea.com" yes; ".com", ".net" and "com.au" no.
bool FamilyWellFormed(std::string const& family);

// Verifies an envelope: at least one Ed25519 signature over the decoded payload bytes that is valid
// under a key in `keys` with that keyid, then parses those same bytes and applies the payload
// rules (type, positive version, every family well formed, every ip redirect well formed and
// unique per platform and protocol -- one bad entry rejects the file).
std::optional<Ceiling> VerifyCeiling(std::string const& envelope,
                                     std::vector<PinnedKey> const& keys, std::string& error);

// The boot decision, without I/O: the cached envelope (re-verified), the fetched one (verified and
// accepted only when its version is at least `highest_accepted` and the cached one's), else the
// cache, else the compiled fallback. `store` says the fetched bytes are to be cached.
struct CeilingDecision {
    CeilingResult result;
    bool store = false;
    long long highest_accepted = 0;  // what to persist afterwards
};
CeilingDecision DecideCeiling(std::optional<std::string> const& cached,
                              std::optional<std::string> const& fetched,
                              long long highest_accepted, std::vector<PinnedKey> const& keys);

// GET https://openpak.org/api/v1/network/ceiling: the pinned origin, public TLS trust only (never
// OPENPAK_API, the OpenPak CA or a URL from the profile), two seconds, one attempt. The body
// exactly as received, or nothing.
std::optional<std::string> FetchCeilingEnvelope();

// DecideCeiling against network-ceiling.json, then caches the accepted envelope bytes there
// (atomically, 0600) and the highest version accepted in network-ceiling-version, and makes the
// result the process's ceiling. `fetched` nothing means "offline": the cache or the fallback.
CeilingResult LoadCeiling(std::optional<std::string> const& fetched,
                          std::vector<PinnedKey> const& keys = PinnedKeys());

// FetchCeilingEnvelope, then LoadCeiling. Blocks up to the fetch's two seconds.
CeilingResult RefreshCeiling();

// The process's ceiling: the one LoadCeiling last produced, or -- before any -- the verified cache
// (read without the network) or the compiled fallback.
Ceiling CurrentCeiling();

// Whether one name (or a family, leading dot) is inside `families`, on a label boundary: a family
// covers its apex and everything below it. "a.nintendo.net" is inside ".nintendo.net",
// "evilnintendo.net" and "a.nintendo.net.evil.example" are not. Case-insensitive.
bool NameInFamilies(std::string const& host, std::vector<std::string> const& families);

// NameInFamilies against this platform's families in CurrentCeiling().
bool NameAllowed(std::string const& host, std::string const& platform);

// Drops every suffix, exact name and override outside `families`, each one logged (unless `log`
// is false), and returns what was dropped. `never` names stay: they only ever keep a name away
// from OpenPak.
std::vector<std::string> FilterToCeiling(Profile& profile, std::vector<std::string> const& families,
                                         bool log = true);

// The contract's digest of the effective redirect set: sha256 (hex) of the sorted
// "family:<suffix>", "exact:<name>", "override:<name>=<ip>" and "address:<ip>" entries, one per
// line. Only this platform's profile goes in, so another platform's change never changes it.
std::string EffectiveDigest(Profile const& profile);

// ---------------------------------------------------------------------------------------------
// The site the profile and the CA are fetched from: server.json's website, https only. The
// ceiling never comes from here (it is always the pinned openpak.org origin).
inline constexpr char const* DefaultSite = "https://openpak.org";
void SetSite(std::string website);
std::string Site();

// ---------------------------------------------------------------------------------------------
// The change notice (contract rule 4). Apply() records the digest of what is in use. Once a Fetch
// has run, a background thread re-checks ceiling + profile every six hours (+-10 %) and soon after
// an OpenPak sign-in, never applying what it finds: when the new digest differs from the one in
// use, the notifier runs once per new digest, on that background thread. The host tells the
// player to restart the game (or the emulator) to use the new redirects.
void SetChangeNotifier(std::function<void()> notifier);

// Starts the background re-check for `platform` (idempotent; Fetch calls it).
void StartWatching(std::string const& platform);

// The re-check's look: the ceiling refreshed (and cached), the profile fetched and, when it
// validates and its CA pins, stored with ca.pem for the next boot. Nothing is applied to this
// process. The digest of what was fetched, filtered through the ceiling, or nothing.
std::optional<std::string> FetchFreshDigest(std::string const& platform);

// Asks the running re-check to look now (after a sign-in); nothing when none is running.
void RecheckSoon();

// The digest Apply() last recorded, empty before any.
std::string DigestInUse();

// Records the digest in use, for a host that applies a profile it fetched itself (Azahar's core)
// rather than through Apply().
void SetDigestInUse(std::string const& digest);

// The notice decision on its own: true when `digest` differs from the one in use and from the last
// one already announced, and records it as announced. Exposed for hosts that check themselves.
bool ShouldAnnounce(std::string const& digest);

// The whole of §2 of the PRD. One conditional GET with the stored ETag; 200 validates, stores
// and applies; 304 applies the stored profile; anything else applies the stored profile; no
// stored profile means the built-in default. Times out in two seconds, never retries. The ceiling
// is fetched beside it (same two seconds, in parallel) and the applied profile is filtered
// through this platform's part of it. Starts the background re-check (StartWatching).
// `platform` is one of switch, wiiu, 3ds, wii, ds.
Result Fetch(std::string const& platform);

// "fetched", "cached" or "built-in" — the word the log line and a status row both want.
char const* SourceName(Source s);

// The same flow without the stored ETag (an unconditional GET).
Result Refresh(std::string const& platform);

// What Fetch decides from one answer to the profile request (status 0: no answer), without
// applying it: for the self-check.
Result DecideFetched(std::string const& platform, int status, std::string const& body,
                     std::string const& etag, std::string const& etag_sent);

// The compiled-in default, used while no profile was ever stored: the Nintendo families as
// wildcards to the console server, and (switch) the NAT check's second probe with an address of
// its own, as the Ryujinx Harbor carries them. Filtered through this platform's part of
// CurrentCeiling().
Profile BuiltIn(std::string const& platform);

// Parse and validate. Validate judges the profile's shape (platform, literal usable addresses); a
// profile it rejects is not applied at all. Which names the profile may redirect is the ceiling's
// call, made name by name after validation (FilterToCeiling), never grounds to reject it whole.
std::optional<Profile> Parse(std::string const& json, std::string& error);
std::optional<std::string> Validate(Profile const& profile, std::string const& expected_platform);

// The stored profile, network-profile.json: {"etag": <ETag>, "profile": <the body exactly as the
// server sent it>}. Re-parsed, re-validated and filtered through the ceiling on the way in, so a
// hand-edited store is only ever as trusted as a fetch.
std::optional<Profile> LoadStored(std::string const& platform);
bool StoreRaw(std::string const& body, std::string const& etag);

// The profile this process redirects with. Fetch and Refresh apply what they return; Apply
// is for a host that got its profile some other way. Thread-safe: DNS lookups read it while a
// refresh replaces it.
void Apply(Result const& applied);

// Where `host` should resolve under the applied profile, or nothing when the name is not
// OpenPak's to answer. `never` wins over everything, then `overrides`, then exact names, then
// suffixes. The profile's own server address wins over `server_address`, which is what a
// profile without one (the built-in list) resolves to. Nothing is applied yet: nothing matches.
std::optional<std::string> RedirectFor(std::string const& host, std::string const& server_address);

// Whether `host` is one of the applied profile's `never` names (it must reach the real internet).
bool IsNever(std::string const& host);

// True when a stored or fetched profile is applied, rather than the built-in list or nothing.
bool Loaded();

// Downloads profile.ca_url (https only) over public TLS and keeps it only when the SHA-256
// matches ca_sha256, written atomically to ca.pem. Nothing is downloaded when ca.pem already
// hashes to ca_sha256. A failure is a log line and false; the stored CA, or none, stays.
bool EnsureCA(Profile const& profile);

// Whether `pem` hashes to `sha256_hex` (case-insensitive): the pin check on its own.
bool CaMatchesPin(std::string const& pem, std::string const& sha256_hex);

// ---------------------------------------------------------------------------------------------
// The guest's DNS, decided here so every resolver command fails closed the same way.
enum class GuestNameKind {
    Direct,    // not OpenPak's to answer: the host resolver (a `never` name, or outside the ceiling)
    Redirect,  // answer with `address`
    Blocked,   // inside a ceiling family with no redirect: never reaches the real service
};
struct GuestName {
    GuestNameKind kind = GuestNameKind::Direct;
    std::string address;
};

// RedirectFor first; a name it does not redirect that is inside this platform's ceiling families
// (CurrentCeiling) is Blocked; a redirect with no address to go to (no profile address and no
// console server) is Blocked too. `never` names stay Direct, as RedirectFor already says.
GuestName ClassifyGuestName(std::string const& host, std::string const& platform,
                            std::string const& server_address);

} // namespace OpenPak::NetworkProfile
