// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/PackageSourceFactory.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
// The selection logic is exercised entirely against these fakes. No test constructs a real
// WingetComSource: out-of-process COM activation for the winget CLSID is environment
// dependent and can take the whole test host down with it - see docs/adr-phase-2.md
// ADR-0009.
class FakeSource final : public IPackageSource
{
public:
    explicit FakeSource(std::wstring packageId) : m_packageId(std::move(packageId))
    {
    }

    [[nodiscard]] std::vector<InstalledPackage> enumeratePackages() override
    {
        if (m_packageId.empty())
        {
            return {};
        }

        InstalledPackage package;
        package.id = m_packageId;
        package.name = m_packageId;
        package.executables.push_back(PackageExe{std::filesystem::path(L"tool.exe"), std::nullopt});
        return {package};
    }

private:
    std::wstring m_packageId;
};

// Constructs fine, fails at query time - the shape of a COM source that connected but
// whose FindPackages call then failed.
class ThrowingSource final : public IPackageSource
{
public:
    explicit ThrowingSource(PackageSourceErrorKind kind) : m_kind(kind)
    {
    }

    [[nodiscard]] std::vector<InstalledPackage> enumeratePackages() override
    {
        throw PackageSourceError(m_kind, "enumeration failed");
    }

private:
    PackageSourceErrorKind m_kind;
};

[[nodiscard]] PackageSourceFactoryFn makeFake(std::wstring packageId)
{
    return [packageId = std::move(packageId)] { return std::make_unique<FakeSource>(packageId); };
}

[[nodiscard]] PackageSourceFactoryFn failingToConstruct(PackageSourceErrorKind kind)
{
    return [kind]() -> std::unique_ptr<IPackageSource> {
        throw PackageSourceError(kind, "activation failed");
    };
}

[[nodiscard]] PackageSourceFactoryFn failingToEnumerate(PackageSourceErrorKind kind)
{
    return [kind] { return std::make_unique<ThrowingSource>(kind); };
}

[[nodiscard]] std::wstring onlyPackageId(const std::vector<InstalledPackage>& packages)
{
    Assert::AreEqual(static_cast<std::size_t>(1), packages.size());
    return packages.front().id;
}
} // namespace

TEST_CLASS(AutoPackageSourceTests)
{
public:
    TEST_METHOD(comIsUsedWhenItSucceeds)
    {
        AutoPackageSource source(makeFake(L"from-com"), makeFake(L"from-fs"));

        Assert::IsTrue(onlyPackageId(source.enumeratePackages()) == L"from-com");
        Assert::IsTrue(source.resolvedSource() == PackageSource::Com);
        Assert::IsFalse(source.degradationKind().has_value());
    }

    TEST_METHOD(resolvedSourceIsAutoUntilEnumerationHappens)
    {
        const AutoPackageSource source(makeFake(L"from-com"), makeFake(L"from-fs"));

        Assert::IsTrue(source.resolvedSource() == PackageSource::Auto);
        Assert::IsFalse(source.degradationKind().has_value());
    }

    TEST_METHOD(aComConstructionFailureDegradesToTheFilesystem)
    {
        AutoPackageSource source(failingToConstruct(PackageSourceErrorKind::AppInstallerMissing),
                                 makeFake(L"from-fs"));

        Assert::IsTrue(onlyPackageId(source.enumeratePackages()) == L"from-fs");
        Assert::IsTrue(source.resolvedSource() == PackageSource::FileSystem);
        Assert::IsTrue(source.degradationKind() == PackageSourceErrorKind::AppInstallerMissing);
    }

    TEST_METHOD(aComEnumerationFailureAlsoDegradesToTheFilesystem)
    {
        // Activation and catalog connect happen in WingetComSource's constructor, but the
        // FindPackages query can still fail afterwards. Both must degrade under --source auto.
        AutoPackageSource source(failingToEnumerate(PackageSourceErrorKind::CatalogError),
                                 makeFake(L"from-fs"));

        Assert::IsTrue(onlyPackageId(source.enumeratePackages()) == L"from-fs");
        Assert::IsTrue(source.resolvedSource() == PackageSource::FileSystem);
        Assert::IsTrue(source.degradationKind() == PackageSourceErrorKind::CatalogError);
    }

    TEST_METHOD(theDegradeHandlerReceivesTheOriginalError)
    {
        std::optional<PackageSourceErrorKind> observed;
        std::string message;

        AutoPackageSource source(failingToConstruct(PackageSourceErrorKind::PolicyBlocked),
                                 makeFake(L"from-fs"),
                                 [&](const PackageSourceError& error) {
                                     observed = error.kind();
                                     message = error.what();
                                 });

        const std::vector<InstalledPackage> packages = source.enumeratePackages();

        Assert::IsTrue(onlyPackageId(packages) == L"from-fs");
        Assert::IsTrue(observed == PackageSourceErrorKind::PolicyBlocked);
        Assert::AreEqual(std::string("activation failed"), message);
    }

    TEST_METHOD(theDegradeHandlerIsNotCalledWhenComWorks)
    {
        bool degraded = false;

        AutoPackageSource source(makeFake(L"from-com"), makeFake(L"from-fs"),
                                 [&](const PackageSourceError&) { degraded = true; });

        const std::vector<InstalledPackage> packages = source.enumeratePackages();

        Assert::IsTrue(onlyPackageId(packages) == L"from-com");
        Assert::IsFalse(degraded);
    }

    TEST_METHOD(anEmptyComResultIsNotAFailure)
    {
        // A machine with no portable packages installed enumerates zero of them. Falling
        // back to the filesystem here would be second-guessing a source that worked.
        bool degraded = false;

        AutoPackageSource source(makeFake(L""), makeFake(L"from-fs"),
                                 [&](const PackageSourceError&) { degraded = true; });

        Assert::IsTrue(source.enumeratePackages().empty());
        Assert::IsTrue(source.resolvedSource() == PackageSource::Com);
        Assert::IsFalse(degraded);
    }

    TEST_METHOD(anExceptionThatIsNotAPackageSourceErrorPropagates)
    {
        // Only "COM is unavailable" degrades. A programming error must not be silently
        // converted into a filesystem scan.
        AutoPackageSource source([]() -> std::unique_ptr<IPackageSource> {
                                     throw std::runtime_error("not a package source error");
                                 },
                                 makeFake(L"from-fs"));

        Assert::ExpectException<std::runtime_error>(
            [&] { const std::vector<InstalledPackage> ignored = source.enumeratePackages(); });
    }
};

TEST_CLASS(CreatePackageSourceTests)
{
public:
    TEST_METHOD(autoReturnsASourceThatCanDegrade)
    {
        const std::unique_ptr<IPackageSource> source = createPackageSource(
            PackageSource::Auto, failingToConstruct(PackageSourceErrorKind::AppInstallerMissing),
            makeFake(L"from-fs"));

        Assert::IsNotNull(source.get());
        Assert::IsTrue(onlyPackageId(source->enumeratePackages()) == L"from-fs");
    }

    TEST_METHOD(explicitComDoesNotDegrade)
    {
        // The user named COM, so its failure is theirs to see; the M6 CLI maps the kind
        // onto an exit code rather than quietly scanning the filesystem instead.
        bool filesystemWasBuilt = false;

        Assert::ExpectException<PackageSourceError>([&] {
            const std::unique_ptr<IPackageSource> ignored = createPackageSource(
                PackageSource::Com,
                failingToConstruct(PackageSourceErrorKind::AppInstallerMissing),
                [&]() -> std::unique_ptr<IPackageSource> {
                    filesystemWasBuilt = true;
                    return std::make_unique<FakeSource>(L"from-fs");
                });
        });

        Assert::IsFalse(filesystemWasBuilt);
    }

    TEST_METHOD(explicitComUsesTheComSourceWhenItWorks)
    {
        const std::unique_ptr<IPackageSource> source =
            createPackageSource(PackageSource::Com, makeFake(L"from-com"), makeFake(L"from-fs"));

        Assert::IsTrue(onlyPackageId(source->enumeratePackages()) == L"from-com");
    }

    TEST_METHOD(explicitFileSystemNeverTouchesCom)
    {
        bool comWasBuilt = false;

        const std::unique_ptr<IPackageSource> source =
            createPackageSource(PackageSource::FileSystem,
                                [&]() -> std::unique_ptr<IPackageSource> {
                                    comWasBuilt = true;
                                    return std::make_unique<FakeSource>(L"from-com");
                                },
                                makeFake(L"from-fs"));

        Assert::IsTrue(onlyPackageId(source->enumeratePackages()) == L"from-fs");
        Assert::IsFalse(comWasBuilt);
    }

    TEST_METHOD(aFactoryThatYieldsNothingIsReportedNotDereferenced)
    {
        Assert::ExpectException<PackageSourceError>([] {
            const std::unique_ptr<IPackageSource> ignored =
                createPackageSource(PackageSource::Com,
                                    [] { return std::unique_ptr<IPackageSource>{}; },
                                    [] { return std::unique_ptr<IPackageSource>{}; });
        });
    }
};
} // namespace syncwingetlink::tests
