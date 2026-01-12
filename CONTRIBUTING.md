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

## Questions?

Open a [Discussion](https://github.com//esp32-pixelcast/discussions) for any questions.

Thank you!
