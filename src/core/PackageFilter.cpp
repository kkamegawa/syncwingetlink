// SPDX-License-Identifier: MIT

#include "PackageFilter.h"

#include <Windows.h>

#include <algorithm>
#include <utility>

namespace syncwingetlink
{
namespace
{
[[nodiscard]] bool equalsOrdinalIgnoreCase(wchar_t left, wchar_t right) noexcept
{
    if (left == right)
    {
        return true;
    }

    return ::CompareStringOrdinal(&left, 1, &right, 1, TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool matchesAnyPattern(const std::vector<std::wstring>& patterns,
                                     std::wstring_view packageId,
                                     std::wstring_view executableFileName)
{
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::wstring& pattern) {
        return matchesGlob(pattern, packageId) || matchesGlob(pattern, executableFileName);
    });
}
} // namespace

bool matchesGlob(std::wstring_view pattern, std::wstring_view value) noexcept
{
    // Iterative backtracking rather than recursion: patternIndex/valueIndex walk forward,
    // and starIndex remembers the most recent '*' so a dead end can resume by letting that
    // '*' swallow one more character. This is O(pattern x value) worst case with no stack
    // growth, so a pathological pattern cannot overflow the stack.
    std::size_t patternIndex = 0;
    std::size_t valueIndex = 0;
    std::size_t starIndex = std::wstring_view::npos;
    std::size_t valueIndexAtStar = 0;

    while (valueIndex < value.size())
    {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == L'?' ||
             equalsOrdinalIgnoreCase(pattern[patternIndex], value[valueIndex])))
        {
            ++patternIndex;
            ++valueIndex;
        }
        else if (patternIndex < pattern.size() && pattern[patternIndex] == L'*')
        {
            starIndex = patternIndex;
            valueIndexAtStar = valueIndex;
            ++patternIndex;
        }
        else if (starIndex != std::wstring_view::npos)
        {
            patternIndex = starIndex + 1;
            ++valueIndexAtStar;
            valueIndex = valueIndexAtStar;
        }
        else
        {
            return false;
        }
    }

    // Trailing '*'s are the only pattern characters allowed to match nothing.
    while (patternIndex < pattern.size() && pattern[patternIndex] == L'*')
    {
        ++patternIndex;
    }

    return patternIndex == pattern.size();
}

PackageFilter::PackageFilter(std::vector<std::wstring> includePatterns,
                             std::vector<std::wstring> excludePatterns)
    : m_includePatterns(std::move(includePatterns)),
      m_excludePatterns(std::move(excludePatterns))
{
}

bool PackageFilter::includesExecutable(std::wstring_view packageId,
                                       std::wstring_view executableFileName) const
{
    // Exclude is checked first and is final: a user who writes both --include and
    // --exclude expects the exclusion to carve out of the inclusion, not to be overridden
    // by it.
    if (matchesAnyPattern(m_excludePatterns, packageId, executableFileName))
    {
        return false;
    }

    // No --include at all means "everything", not "nothing".
    if (m_includePatterns.empty())
    {
        return true;
    }

    return matchesAnyPattern(m_includePatterns, packageId, executableFileName);
}

std::vector<InstalledPackage> PackageFilter::apply(std::vector<InstalledPackage> packages) const
{
    if (isEmpty())
    {
        return packages;
    }

    std::vector<InstalledPackage> filtered;
    filtered.reserve(packages.size());

    for (InstalledPackage& package : packages)
    {
        std::vector<PackageExe> executables;
        executables.reserve(package.executables.size());

        for (PackageExe& executable : package.executables)
        {
            // filename() returns a temporary path; its native() reference stays valid for
            // the duration of the full expression, which covers the call.
            if (includesExecutable(package.id, executable.path.filename().native()))
            {
                executables.push_back(std::move(executable));
            }
        }

        if (executables.empty())
        {
            // Nothing left to link for this package. Dropping it keeps the caller from
            // reporting a package that has no work attached to it.
            continue;
        }

        package.executables = std::move(executables);
        filtered.push_back(std::move(package));
    }

    return filtered;
}

bool PackageFilter::isEmpty() const noexcept
{
    return m_includePatterns.empty() && m_excludePatterns.empty();
}
} // namespace syncwingetlink
