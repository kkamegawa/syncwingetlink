// SPDX-License-Identifier: MIT

#pragma once

namespace syncwingetlink
{
// RAII wrapper around CoInitializeEx(COINIT_MULTITHREADED). Tolerates
// RPC_E_CHANGED_MODE (the calling thread already initialized a different concurrency
// model) rather than throwing, since a static library must not assume it owns the
// process's apartment - a host process (eventually main.cpp) or a test runner may already
// have one. winrt::init_apartment() was deliberately not used here: it throws on exactly
// the RPC_E_CHANGED_MODE condition this type needs to tolerate.
//
// Apartment state is a process-wide concern, not something that should be scoped to a
// single IPackageSource. WingetComSource currently owns one only because main.cpp does
// not exist yet (M6); once it does, main.cpp should construct a single ComApartment for
// the process lifetime and WingetComSource should stop owning its own. See
// docs/adr-phase-2.md ADR-0009.
class ComApartment
{
public:
    ComApartment();
    ~ComApartment();

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment(ComApartment&&) = delete;
    ComApartment& operator=(ComApartment&&) = delete;

private:
    bool m_owned;
};
} // namespace syncwingetlink
