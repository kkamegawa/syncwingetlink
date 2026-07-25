// SPDX-License-Identifier: MIT

#include "PackageSourceError.h"

#include <Windows.h>

#include <winerror.h>

namespace syncwingetlink
{
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
    case RPC_S_SERVER_UNAVAILABLE:
    case RPC_E_DISCONNECTED:
    case RPC_E_SERVER_DIED:
        return PackageSourceErrorKind::ServerUnavailable;
    default:
        return PackageSourceErrorKind::Unknown;
    }
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
