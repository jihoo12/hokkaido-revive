# AGENTS

Instructions for AI agents working on this repository.

## Core Principles

1. **Always update docs** when changing functionality
2. **Always update versions** when releasing (use the automated tool)
3. **Keep READMEs accurate** — they're the first thing users see
4. **Test your changes** before committing

## Components

### hokkaido (C++ compiler)
- **Location**: Root directory (`default.nix`, `CMakeLists.txt`)
- **Language**: C++
- **Version**: `default.nix` → `version = "..."`
- **Build**: `cmake .. && make`
- **Outputs**: `hokkaido` compiler, `hok-lsp` language server

### hok-lsp (Language Server)
- **Location**: `src-cpp/lsp/`
- **Language**: C++
- **Version**: Same as hokkaido (bundled in same build)
- **Build**: Built automatically with hokkaido
- **Features**: Diagnostics, hover, completion, go-to-definition, find references

### otaru (Package Manager & Build Tool)
- **Location**: `otaru/`
- **Language**: Rust
- **Version**: `otaru/Cargo.toml` + `otaru/default.nix`
- **Build**: `cargo build --manifest-path otaru/Cargo.toml`
- **Features**: Project scaffolding, dependency management, build orchestration, C/C++ builds, WASM compilation, web app scaffolding (`--web`), dev server, DOM library bundling, npm dependency support

### sapporo (Web App Toolkit)
- **Location**: `sapporo/` (DOM bindings `.hk` + `.js`)
- **Library**: `sapporo/sapporo/sapporo.hk` and `sapporo/sapporo.js` are embedded in otaru via `include_bytes!` and bundled into web projects at scaffold time.

### std (Standard Library)
- **Location**: `std/`
- **Language**: .hk (Hokkaido)
- **Version**: N/A (ships with hokkaido/otaru)
- **Usage**: `import "std"` in .hk files

## Version Management

### Single Source of Truth: `versions.toml`

All versions live in `versions.toml`. Never edit version numbers directly in other files.

```bash
./scripts/bump-version.sh --show              # View current versions
./scripts/bump-version.sh --set otaru 0.8.0   # Bump a component
./scripts/bump-version.sh --apply             # Re-apply versions.toml to all files
./scripts/bump-version.sh --commit            # Apply + git commit
```

### Version Table

| Component | Version | Files Updated |
|-----------|---------|---------------|
| hokkaido | 0.22.0 | `default.nix` |
| otaru | 0.8.1 | `otaru/Cargo.toml`, `otaru/default.nix`, `otaru/Cargo.lock` |

### When to Bump Versions

- **Bug fix**: Patch bump (0.7.1 → 0.7.2)
- **New feature**: Minor bump (0.7.1 → 0.8.0)
- **Breaking change**: Major bump (0.7.1 → 1.0.0)

## Documentation Updates

### Always Update These Files

When you change functionality, check and update:

1. **README.md** — Project overview, quick start, features
2. **Component READMEs** — `otaru/README.md`, `src-cpp/lsp/README.md`
3. **`sapporo/docs/*.md`** — API docs, examples, tutorials
4. **`docs/*.md`** — User-facing documentation
5. **`AGENTS.md`** — This file (version table, workflows)

### Documentation Checklist

- [ ] Compiler changes → Update `README.md` (hokkaido section)
- [ ] LSP changes → Update `src-cpp/lsp/README.md`
- [ ] Otaru changes → Update `otaru/README.md`
- [ ] Sapporo API changes → Update `sapporo/docs/api.md`
- [ ] Sapporo examples → Update `sapporo/docs/examples.md`
- [ ] Sapporo architecture → Update `sapporo/docs/docs.md`
- [ ] CLI changes → Update relevant component README
- [ ] Version bump → Run `./scripts/bump-version.sh --commit`

## Common Workflows

### Adding a New Feature

1. Implement the feature
2. Add tests if applicable
3. Update relevant docs (API, examples, README)
4. Run `./scripts/bump-version.sh --apply` if version bump needed
5. Commit with descriptive message

### Fixing a Bug

1. Fix the bug
2. Add regression test if applicable
3. Update docs if behavior changed
4. Commit with descriptive message

### Releasing a New Version

1. Ensure all changes are committed
2. Run `./scripts/bump-version.sh --set <component> <version>`
3. Run `./scripts/bump-version.sh --commit`
4. Push: `git push`
5. Create release tag: `git tag v<version>`
6. Push tag: `git push --tags`

## Project Structure

```
hokkaido/
├── default.nix          # Hokkaido compiler (C++)
├── std/                 # Standard library
├── otaru/               # Package manager + build tool (Rust)
├── sapporo/             # Web app library (.hk + .js, embedded in otaru)
├── src-cpp/lsp/         # Language server (C++)
├── test/                # Compiler tests
│   ├── phases/          # Compiler phase progression tests
│   ├── features/        # Individual feature tests
│   ├── examples/        # Demo/example programs
│   ├── packages/        # Package/module system tests
│   └── run.sh           # Test runner script
├── test-app/            # Sapporo web app test project
├── testnix/             # Nix build test project
├── install.sh           # Official installer
├── versions.toml        # Single source of truth for versions
├── scripts/             # Automation scripts
└── AGENTS.md            # This file
```

## Tools

- **nix**: Use when specific tool is not available
- **cmake**: C++ build system
- **./scripts/bump-version.sh**: Version management

## Code Style

- **C++**: Follow LLVM coding standards
- **Rust**: Follow standard rustfmt conventions
- **Shell**: Use `#!/usr/bin/env bash`, `set -euo pipefail`
- **Nix**: Follow nixpkgs conventions
- **Comments**: Minimal, only when necessary

## Testing

- Run `cargo test` for Rust components
- Run `cmake .. && make && ctest` for C++ components
- Run `./test/run.sh` to run all compiler tests (or `./test/run.sh <category>` for phases/features/examples/packages)
- Test CLI tools manually after changes
- Verify examples work before committing
