// SPDX-License-Identifier: MIT

#include "ComApartment.h"

#include "PackageSourceError.h"

// WIN32_LEAN_AND_MEAN (set project-wide in props/syncwingetlink.common.props) excludes
// <ole2.h> from <Windows.h>, which is where CoInitializeEx/CoUninitialize live.
#include <Windows.h>
#include <combaseapi.h>

namespace syncwingetlink
{
ComApartment::ComApartment() : m_owned(false)
{
    const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (result == RPC_E_CHANGED_MODE)
    {
        // A different concurrency model is already active on this thread. Reuse it
        // rather than failing construction; we have nothing to uninitialize later.
        m_owned = false;
    }
    else if (FAILED(result))
    {
        throw PackageSourceError(PackageSourceErrorKind::Unknown, "CoInitializeEx failed",
                                 result);
    }
    else
    {
        // S_OK or S_FALSE (already initialized on this thread with the same model) both
        // require a balancing CoUninitialize.
        m_owned = true;
    }
}

ComApartment::~ComApartment()
{
    if (m_owned)
    {
        ::CoUninitialize();
    }
}
} // namespace syncwingetlink
