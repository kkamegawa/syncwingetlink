// SPDX-License-Identifier: MIT

#include "PackageSourceError.h"

#include <Windows.h>

#include <winerror.h>

namespace syncwingetlink
{
namespace
{
constexpr std::string_view kTroubleshootingUrl =
    "https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
}

PackageSourceErrorKind mapHresultToKind(int32_t hresult) noexcept
{
    switch (hresult)
    {
    case REGDB_E_CLASSNOTREG:
    case CO_E_SERVER_EXEC_FAILURE:
    case CLASS_E_CLASSNOTAVAILABLE:
        return PackageSourceErrorKind::AppInstallerMissing;
    case E_ACCESSDENIED:
        return PackageSourceErrorKind::AccessDenied;
    // RPC_S_SERVER_UNAVAILABLE is a Win32 error code, not an HRESULT; a COM failure
    // surfaces it wrapped as HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)
    // (0x800706BA), so the raw constant never matched here (docs/adr-phase-9.md
    // ADR-0039). RPC_E_DISCONNECTED/RPC_E_SERVER_DIED are already genuine HRESULTs.
    case HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE):
    case RPC_E_DISCONNECTED:
    case RPC_E_SERVER_DIED:
        return PackageSourceErrorKind::ServerUnavailable;
    // The server is registered and reachable, but rejected typed WinRT interface
    // activation from this unpackaged caller (docs/adr-phase-9.md ADR-0039, issue
    // #143). Distinct from AppInstallerMissing, whose HRESULTs mean the server was
    // never found at all.
    case HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE):
        return PackageSourceErrorKind::PackageIdentityRequired;
    default:
        return PackageSourceErrorKind::Unknown;
    }
}

std::string_view remediationFor(PackageSourceErrorKind kind) noexcept
{
    switch (kind)
    {
    case PackageSourceErrorKind::AppInstallerMissing:
        return "Install or repair App Installer, or re-run with --source fs. See "
               "https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
    case PackageSourceErrorKind::PolicyBlocked:
        return "A policy blocked package enumeration; check policy settings or re-run with "
               "--source fs. See "
               "https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
    case PackageSourceErrorKind::AccessDenied:
        return "Package enumeration was denied; retry with the required access or re-run "
               "with --source fs. See "
               "https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
    case PackageSourceErrorKind::ServerUnavailable:
        return "The winget COM server was unavailable; retry, or re-run with --source fs. "
               "See https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
    case PackageSourceErrorKind::CatalogError:
        return "The winget catalog was not usable; run winget list once to accept source "
               "agreements, then retry, or re-run with --source fs. See "
               "https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
    case PackageSourceErrorKind::ScanFailed:
        return "The Packages directory could not be scanned; verify it or pass "
               "--packages-dir. See "
               "https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
    case PackageSourceErrorKind::PackageIdentityRequired:
        return "This host rejected typed WinRT activation from an unpackaged process; "
               "re-run with --source fs, or use --source auto to fall back automatically. "
               "See https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
    case PackageSourceErrorKind::Unknown:
        return "Re-run with --verbose, and try --source fs to bypass COM. See "
               "https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md";
    }

    return kTroubleshootingUrl;
}

bool isPortableInstallerType(std::wstring_view installerType) noexcept
{
    constexpr std::wstring_view kPortable = L"portable";
    if (installerType.size() != kPortable.size())
    {
        return false;
    }

    // Ordinal, not locale-sensitive: CompareStringOrdinal never applies a
    // locale-dependent case fold (e.g. the Turkish dotless-i), unlike towlower/_wcsicmp.
    const int result = ::CompareStringOrdinal(
        installerType.data(), static_cast<int>(installerType.size()), kPortable.data(),
        static_cast<int>(kPortable.size()), TRUE);
    return result == CSTR_EQUAL;
}
} // namespace syncwingetlink
