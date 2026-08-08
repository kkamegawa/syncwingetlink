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
// Callers map this to an exit code via cli::exitCodeFor() (src/cli/Dispatch.cpp): every
// kind here - including PolicyBlocked/AccessDenied, which read like "insufficient
// permission" - maps to ExitCode::PackageEnumerationFailed (4).
// ExitCode::InsufficientPermission (2) is reserved for SymlinkService's
// Developer-Mode/privilege failures, a different subsystem; a package-source error never
// produces it, under --source com or otherwise. See docs/com-api.md "Failure and
// fallback".
enum class PackageSourceErrorKind
{
    AppInstallerMissing,      // the winget COM server is not registered / not installed
    PolicyBlocked,            // FindPackagesResultStatus::BlockedByPolicy
    AccessDenied,             // E_ACCESSDENIED, or FindPackagesResultStatus::AccessDenied
    ServerUnavailable,        // the out-of-proc server could not be reached/is unresponsive
    CatalogError,             // ConnectResultStatus/FindPackagesResultStatus == CatalogError,
                              // or SourceAgreementsNotAccepted
    ScanFailed,               // a filesystem-source failure: FsScanSource could not read the
                              // Packages directory (denied access, I/O error). A merely
                              // absent directory is an empty result, not this.
    PackageIdentityRequired,  // HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE): the server is
                              // registered, but it rejected typed WinRT interface activation
                              // from this unpackaged caller. See docs/adr-phase-9.md
                              // ADR-0039 and issue #143 - distinct from AppInstallerMissing,
                              // which means the server was never found at all.
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
// without winget installed. See docs/adr-phase-2.md ADR-0009 for the original HRESULT
// table this implements, and docs/adr-phase-9.md ADR-0039 for the
// APPMODEL_ERROR_NO_PACKAGE case and the RPC_S_SERVER_UNAVAILABLE fix added later.
[[nodiscard]] PackageSourceErrorKind mapHresultToKind(int32_t hresult) noexcept;

// True when a winget PackageVersionMetadataField::InstallerType value denotes a portable
// package. The COM API returns this as a free-form hstring rather than the
// PackageInstallerType enum (there is no enum getter), so this is a case-insensitive
// ordinal comparison, not a value cast. Ordinal (not locale-sensitive) on purpose:
// package identifiers and installer-type strings are not guaranteed ASCII, and a
// locale-dependent fold (e.g. the Turkish dotless-i) must not change the answer.
[[nodiscard]] bool isPortableInstallerType(std::wstring_view installerType) noexcept;
} // namespace syncwingetlink
