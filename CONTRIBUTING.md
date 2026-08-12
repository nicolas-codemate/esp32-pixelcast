# Contributing to ESP32-PixelCast

Thank you for wanting to contribute to ESP32-PixelCast!

## How to Contribute

### Reporting a Bug

1. Check that the bug hasn't already been reported in [Issues](https://github.com/nicolas-codemate/esp32-pixelcast/issues)
2. Create a new issue with:
   - Clear problem description
   - Steps to reproduce
   - Expected vs actual behavior
   - Firmware version
   - Hardware used (Trinity, DevKit, etc.)
   - Serial logs if available

### Proposing a Feature

1. Open an issue with the `enhancement` tag
2. Describe the feature and its usefulness
3. Propose an implementation if possible

### Submitting Code

1. Fork the repository
2. Create a branch (`git checkout -b feature/my-feature`)
3. Commit your changes (`git commit -m 'Add: my feature'`)
4. Push the branch (`git push origin feature/my-feature`)
5. Open a Pull Request

## Code Standards

### C++ Style

- Indentation: 4 spaces
- Braces: Allman style
- Naming:
  - Variables: `camelCase`
  - Constants: `UPPER_SNAKE_CASE`
  - Classes: `PascalCase`
  - Functions: `camelCase`

### Commits

Format: `Type: Short description`

Types:
- `Add:` New feature
- `Fix:` Bug fix
- `Update:` Update to existing feature
- `Refactor:` Refactoring without functional change
- `Docs:` Documentation
- `Style:` Formatting, code style
- `Test:` Tests

### Documentation

- Comment public functions
- Update README if necessary
- Document new APIs

## Tests

Before submitting:

1. Build for all targets (`pio run`)
2. Test on real hardware if possible
3. Check memory usage

## Versioning

The firmware follows semantic versioning. The version is printed on the boot screen and
returned by `/api/stats`, so a wrong number is visible to users on the panel itself.

One number covers the firmware and the API specs that describe it, so a client can compare
the `version` it reads from `/api/stats` against the specs it vendored and know whether the
two match. It lives in four files that must always agree:

- `platformio.ini` - the `VERSION_MAJOR`, `VERSION_MINOR`, `VERSION_PATCH` and
  `VERSION_STRING` build flags. Authoritative: they override the header.
- `include/config.h` - the same four macros as `#ifndef` fallbacks.
- `docs/api/openapi.yaml` - `info.version`.
- `docs/api/asyncapi.yaml` - `info.version`.

When to bump:

- `PATCH` - bug fix with no change to the API or the display contract, or a correction to
  what the specs say about behaviour the firmware already had
- `MINOR` - backward-compatible addition: new optional API field, new app type, new endpoint
- `MAJOR` - breaking change to the REST/MQTT contract or to the stored settings format

A specs-only change bumps the version too, and ships as a release whose binaries are
identical to the previous one - the release carries the corrected contract. The cost of one
number is that a firmware-only fix also moves the specs' version without the contract having
changed; read the release notes, not the number, to know what moved.

Bump in the same pull request as the change that warrants it. A pull request that adds an
API field and leaves the version untouched is incomplete.

## Releases

1. Make sure the four files carry the new version, and merge to `main`
2. `git tag vX.Y.Z && git push origin vX.Y.Z`
3. `.github/workflows/release.yml` checks that the tag, `VERSION_STRING`, the
   `MAJOR.MINOR.PATCH` triple, the `include/config.h` fallback and both specs'
   `info.version` all agree, then builds `trinity`, `devkit` and `esp32s3`

The workflow refuses to publish on any mismatch. Do not work around it by moving the tag:
correct the version in the four files and tag again.

### What ships

Only `pixelcast-trinity-<version>.bin` is attached to the release. `devkit` and `esp32s3`
are built to catch compilation breakage, not shipped: no hardware is available to test them,
and publishing an untested binary claims a support level the project does not have. Add a
target to the release step once someone can verify it on the matching board.

## Questions?

Open a [Discussion](https://github.com//esp32-pixelcast/discussions) for any questions.

Thank you!
