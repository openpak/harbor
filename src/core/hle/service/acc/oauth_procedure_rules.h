// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The rules of the Nintendo Account procedures (oauth_procedure.cpp) as pure functions: which
// command each stage allows, how a suspend and a resume move the stage, and where GetRequest's two
// pointer outputs go. Header only, no kernel or service code, so check/oauth_check.cpp tests the
// state machine and the layouts without an emulator.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace Service::Account::ProcedureRules {

// The module's procedure stages (+0x18): fresh, suspended, resumed, response applied.
enum class Stage { Fresh, Suspended, Resumed, Applied };

// Session kinds in the module's store (0x0860eea0): linkage 0, registration 3.
enum class Kind : int { Linkage = 0, Registration = 3 };

struct Machine {
    Kind kind = Kind::Registration;
    Stage stage = Stage::Fresh;
    /// ApplyResponse took a callback (its work may still run, or may have failed): a second
    /// response is refused, as is a new request.
    bool has_code = false;
    /// Registration 110/220/221/230 created the user.
    bool registered = false;
};

/// GetRequest / GetRequestWithTheme (0x085a1880): no code yet, stage 0.
constexpr bool CanGetRequest(const Machine& m) {
    return !m.has_code && m.stage == Stage::Fresh;
}

/// PrepareAsync's work (0x085a1e40) only checks the stage. Linkage answers 2124-0011 instead.
constexpr bool PrepareSucceeds(const Machine& m) {
    return m.stage == Stage::Fresh;
}

/// ApplyResponseAsync (3) and ApplyResponseForUserCreationAsync (200): a fresh procedure, or for a
/// linkage also a resumed one (the Login applet ran between Suspend and IAdministrator 252).
constexpr bool CanApply(const Machine& m) {
    if (m.has_code) {
        return false;
    }
    return m.stage == Stage::Fresh || (m.kind == Kind::Linkage && m.stage == Stage::Resumed);
}

/// Suspend (10). A registration suspends from stage 0 only; a linkage before any response, fresh
/// or resumed.
constexpr bool CanSuspend(const Machine& m) {
    if (m.kind == Kind::Registration) {
        return m.stage == Stage::Fresh;
    }
    return !m.has_code && (m.stage == Stage::Fresh || m.stage == Stage::Resumed);
}

/// What Suspend leaves behind on the object the guest still holds.
constexpr Stage AfterSuspend(const Machine& m) {
    return m.kind == Kind::Linkage ? Stage::Suspended : m.stage;
}

/// A procedure made by acc:su 212/214 or IAdministrator 252 from a suspended session.
constexpr Stage AfterResume(Kind kind) {
    return kind == Kind::Linkage ? Stage::Resumed : Stage::Fresh;
}

/// 100/101/102/103/210: only once the response is applied.
constexpr bool CanReadApplied(const Machine& m) {
    return m.stage == Stage::Applied;
}

/// 110/220/221/230: once, after the response is applied.
constexpr bool CanRegister(const Machine& m) {
    return m.kind == Kind::Registration && m.stage == Stage::Applied && !m.registered;
}

/// The work of an ApplyResponseAsync finished.
constexpr Stage AfterApply(const Machine& m, bool succeeded) {
    return succeeded ? Stage::Applied : m.stage;
}

// GetRequest's outputs: buffer<RequestUrl, 0x1A, 0x1000> and buffer<CallbackUri, 0x1A, 0x100>.
inline constexpr std::size_t RequestUrlSize = 0x1000;
inline constexpr std::size_t CallbackUriSize = 0x100;

enum class RequestLayout {
    /// Neither shape below: the procedure answers InvalidBuffer and keeps its stage.
    Refused,
    /// One receive-list entry per output (the shape Eden's other 0x1A outputs arrive in).
    TwoEntries,
    /// A single receive-list entry big enough for both, written back to back (unknown contract).
    OneEntry,
};

/// Where GetRequest writes, from the receive-list entry sizes the request carried.
inline RequestLayout PlanRequest(const std::vector<std::size_t>& receive_sizes) {
    if (receive_sizes.size() >= 2 && receive_sizes[0] > 0 && receive_sizes[1] > 0) {
        return RequestLayout::TwoEntries;
    }
    if (receive_sizes.size() == 1 && receive_sizes[0] >= RequestUrlSize + CallbackUriSize) {
        return RequestLayout::OneEntry;
    }
    return RequestLayout::Refused;
}

/// A fixed-size string output: `min(entry, capacity)` bytes, the text cut to leave a NUL, the
/// rest zero.
inline std::vector<std::uint8_t> FixedString(std::string_view text, std::size_t capacity,
                                             std::size_t entry) {
    std::vector<std::uint8_t> out(std::min(entry, capacity));
    if (!out.empty()) {
        std::memcpy(out.data(), text.data(), std::min(text.size(), out.size() - 1));
    }
    return out;
}

} // namespace Service::Account::ProcedureRules
