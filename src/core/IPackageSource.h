// SPDX-License-Identifier: MIT

#pragma once

#include "Model.h"

#include <vector>

namespace syncwingetlink
{
// WingetComSource (COM API) and FsScanSource (filesystem scan) implement this so the
// rest of core/ stays source-agnostic; see docs/PLAN.md sections 5 and 6.
class IPackageSource
{
public:
    virtual ~IPackageSource() = default;

    // Throws on unrecoverable enumeration failure (e.g. COM activation failure, denied
    // filesystem access) rather than returning a partial result silently.
    [[nodiscard]] virtual std::vector<InstalledPackage> enumeratePackages() = 0;
};
} // namespace syncwingetlink
