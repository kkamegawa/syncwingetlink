<!--
Thanks for your PR! Please fill in the sections below.
-->

## Summary

<!-- What & why -->

## Related TODO

<!-- Which item/milestone in docs/TODO.md -->
- 

## Changes

- 

## Design decisions

<!-- Especially anything about COM / FS fallback / alias regex rules -->

## Testing

<!-- Tests run and results; note anything untested -->
- [ ] `msbuild syncwingetlink.sln -p:Configuration=Debug -p:Platform=x64` succeeds
- [ ] `vstest.console.exe build\x64\Debug\syncwingetlink.tests.dll` passes
- [ ] Added MSTest unit tests for `[core]` changes
- [ ] ARM64 configuration builds locally (note if only cross-built, not run — CI's
      `windows-11-vs2026-arm` leg covers the native ARM64 test run separately)

## Checklist

- [ ] Commits follow Conventional Commits
- [ ] Applied clang-format & clang-tidy
- [ ] Updated both `.vcxproj` and `.vcxproj.filters` if files were added or removed
- [ ] New logic went into `syncwingetlink.core`, not the executable project
- [ ] No breaking changes, or noted below
- [ ] MIT-compatible code only (no code copied from other repositories)
- [ ] No secrets committed
- [ ] Recorded any design deviation in `docs/adr.md`

## Breaking changes

<!-- "None" if not applicable -->
None
