// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace syncwingetlink
{
// Coarse classification of why an IPackageSource could not produce a package list.
// Callers (eventually the M6 CLI) map this to an exit code; PolicyBlocked/AccessDenied
// both correspond to "insufficient permission" territory, but which exit code that maps
// to under --source com specifically (as opposed to --source auto, which just degrades
// to FsScanSource) is an open question - see docs/adr-phase-2.md ADR-0009.
enum class PackageSourceErrorKind
{
    AppInstallerMissing,      // the winget COM server is not registered / not installed
    PolicyBlocked,            // FindPackagesResultStatus::BlockedByPolicy
    AccessDenied,             // E_ACCESSDENIED, or FindPackagesResultStatus::AccessDenied
    ServerUnavailable,        // the out-of-proc server could not be reached/is unresponsive
    CatalogError,             // ConnectResultStatus/FindPackagesResultStatus == CatalogError,
                              // or SourceAgreementsNotAccepted
    ScanFailed,               // a filesystem-source failure (currently unused: FsScanSource
                              // degrades to an empty result rather than throwing)
    Unknown,
};

class PackageSourceError : public std::runtime_error
{
public:
    PackageSourceError(PackageSourceErrorKind kind, const std::string& message,
                      std::optional<int32_t> hresult = std::nullopt)
        : std::runtime_error(message), m_kind(kind), m_hresult(hresult)
    {
    }

    [[nodiscard]] PackageSourceErrorKind kind() const noexcept
    {
        return m_kind;
    }

    // The originating HRESULT, when the failure came from a COM call. Absent for
    // status-code failures (e.g. a FindPackagesResultStatus that is not itself an
    // HRESULT) and for non-COM sources.
    [[nodiscard]] std::optional<int32_t> hresult() const noexcept
    {
        return m_hresult;
    }

private:
    PackageSourceErrorKind m_kind;
    std::optional<int32_t> m_hresult;
};

// Classifies a COM failure HRESULT into a PackageSourceErrorKind. Pure and
// winrt-independent so it can be unit tested with synthetic HRESULTs on any machine,
// without winget installed. See docs/adr-phase-2.md ADR-0009 for the HRESULT table this
// implements.
[[nodiscard]] PackageSourceErrorKind mapHresultToKind(int32_t hresult) noexcept;

// True when a winget PackageVersionMetadataField::InstallerType value denotes a portable
// package. The COM API returns this as a free-form hstring rather than the
// PackageInstallerType enum (there is no enum getter), so this is a case-insensitive
// ordinal comparison, not a value cast. Ordinal (not locale-sensitive) on purpose:
// package identifiers and installer-type strings are not guaranteed ASCII, and a
// locale-dependent fold (e.g. the Turkish dotless-i) must not change the answer.
[[nodiscard]] bool isPortableInstallerType(std::wstring_view installerType) noexcept;
} // namespace syncwingetlink
