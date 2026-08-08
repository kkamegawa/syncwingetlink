// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/PackageSourceError.h>

#include <Windows.h>
#include <winerror.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
TEST_CLASS(PackageSourceErrorTests)
{
public:
    TEST_METHOD(errorCarriesKindMessageAndHresult)
    {
        const PackageSourceError error(PackageSourceErrorKind::AccessDenied, "denied",
                                       E_ACCESSDENIED);

        Assert::IsTrue(error.kind() == PackageSourceErrorKind::AccessDenied);
        Assert::AreEqual(std::string("denied"), std::string(error.what()));
        Assert::IsTrue(error.hresult().has_value());
        Assert::AreEqual(static_cast<int32_t>(E_ACCESSDENIED), *error.hresult());
    }

    TEST_METHOD(hresultIsOptionalWhenNotSupplied)
    {
        const PackageSourceError error(PackageSourceErrorKind::ScanFailed, "no hresult here");

        Assert::IsFalse(error.hresult().has_value());
    }

    TEST_METHOD(missingAppInstallerHresultsMapToAppInstallerMissing)
    {
        Assert::IsTrue(mapHresultToKind(REGDB_E_CLASSNOTREG) ==
                       PackageSourceErrorKind::AppInstallerMissing);
        Assert::IsTrue(mapHresultToKind(CO_E_SERVER_EXEC_FAILURE) ==
                       PackageSourceErrorKind::AppInstallerMissing);
        Assert::IsTrue(mapHresultToKind(CLASS_E_CLASSNOTAVAILABLE) ==
                       PackageSourceErrorKind::AppInstallerMissing);
    }

    TEST_METHOD(accessDeniedHresultMapsToAccessDenied)
    {
        Assert::IsTrue(mapHresultToKind(E_ACCESSDENIED) == PackageSourceErrorKind::AccessDenied);
    }

    TEST_METHOD(serverUnavailableHresultsMapToServerUnavailable)
    {
        // RPC_S_SERVER_UNAVAILABLE is a Win32 error code (1722); a COM call surfaces it
        // wrapped as HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) (0x800706BA), which is
        // the form mapHresultToKind must recognize (docs/adr-phase-9.md ADR-0039).
        Assert::IsTrue(mapHresultToKind(HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)) ==
                       PackageSourceErrorKind::ServerUnavailable);
        Assert::IsTrue(mapHresultToKind(RPC_E_DISCONNECTED) ==
                       PackageSourceErrorKind::ServerUnavailable);
        Assert::IsTrue(mapHresultToKind(RPC_E_SERVER_DIED) ==
                       PackageSourceErrorKind::ServerUnavailable);
    }

    TEST_METHOD(rawServerUnavailableWin32CodeDoesNotMatchAsAnHresult)
    {
        // Regression guard for the bug ADR-0039 fixed: the raw Win32 error code (not
        // HRESULT_FROM_WIN32-wrapped) must not accidentally match any case and must fall
        // through to Unknown.
        Assert::IsTrue(mapHresultToKind(RPC_S_SERVER_UNAVAILABLE) ==
                       PackageSourceErrorKind::Unknown);
    }

    TEST_METHOD(appmodelErrorNoPackageMapsToPackageIdentityRequired)
    {
        // docs/adr-phase-9.md ADR-0039 / issue #143: PackageManager activation can fail
        // with this HRESULT even when the winget COM server is registered and winget
        // itself works, because the typed WinRT interface activation is rejected for an
        // unpackaged out-of-proc caller.
        Assert::IsTrue(mapHresultToKind(HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE)) ==
                       PackageSourceErrorKind::PackageIdentityRequired);
    }

    TEST_METHOD(unrecognizedHresultMapsToUnknown)
    {
        Assert::IsTrue(mapHresultToKind(E_FAIL) == PackageSourceErrorKind::Unknown);
        Assert::IsTrue(mapHresultToKind(E_INVALIDARG) == PackageSourceErrorKind::Unknown);
    }

    TEST_METHOD(portableInstallerTypeIsRecognizedCaseInsensitively)
    {
        Assert::IsTrue(isPortableInstallerType(L"portable"));
        Assert::IsTrue(isPortableInstallerType(L"Portable"));
        Assert::IsTrue(isPortableInstallerType(L"PORTABLE"));
    }

    TEST_METHOD(nonPortableInstallerTypesAreRejected)
    {
        Assert::IsFalse(isPortableInstallerType(L"msi"));
        Assert::IsFalse(isPortableInstallerType(L"exe"));
        Assert::IsFalse(isPortableInstallerType(L""));
        Assert::IsFalse(isPortableInstallerType(L"portables"));
        Assert::IsFalse(isPortableInstallerType(L"portabl"));
    }
};
} // namespace syncwingetlink::tests
