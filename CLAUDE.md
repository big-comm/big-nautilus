# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a fork of GNOME Files (Nautilus), the default file manager for GNOME desktop. The codebase is written in C and uses GTK4/Libadwaita for the UI. This fork includes an embedded terminal feature using VTE (Virtual Terminal Emulator).

**Key Dependencies (Nautilus 50):**
- GTK4 >= 4.20.0
- Libadwaita >= 1.8.alpha
- GLib >= 2.84.0
- VTE >= 0.78.0 (for terminal widget, vte-2.91-gtk4)
- glycin >= 2 (image loader)
- blueprint-compiler >= 0.19.0 (build-time, for .blp UIs)
- TinySPARQL 3.0 (for search and metadata)

**Runtime Dependencies:**
- Bubblewrap (for sandboxing)
- LocalSearch (for fast search, metadata extraction, starred files, batch renaming)
- xdg-user-dirs-gtk (for default bookmarks and localization)

## Build System

This project uses **Meson** as its build system.

### Common Build Commands

```bash
# Initial setup (create build directory)
meson setup builddir

# Build with development profile
meson setup builddir -Dprofile=Devel

# Compile
meson compile -C builddir

# Install
meson install -C builddir

# Run tests (headless by default)
meson test -C builddir

# Run all tests including display tests
meson setup builddir -Dtests=all
meson test -C builddir

# Run a single test
meson test -C builddir test-name

# Reconfigure build options
meson configure builddir -Doption=value

# Clean build
rm -rf builddir
```

### Build Options (meson_options.txt)

- `profile`: Build with specified application ID (use 'Devel' for development)
- `extensions`: Build stock extensions (default: true)
- `docs`: Build documentation for extension library (default: false)
- `tests`: Build tests - 'none', 'headless', or 'all' (default: 'headless')
- `cloudproviders`: Enable cloudproviders support (default: true)
- `selinux`: Enable SELinux context support (default: false)
- `packagekit`: Enable finding file type handlers using PackageKit (default: true)

## Code Architecture

### Core Components

**NautilusApplication** (`src/nautilus-application.c`)
- Main application class (extends AdwApplication)
- Manages windows, DBus, bookmarks, undo manager, tag manager
- Entry point for the application lifecycle

**NautilusWindow** and **NautilusWindowSlot** (`src/nautilus-window-slot.c`, `src/nautilus-window-slot.h`)
- Window contains one or more slots (tabs)
- Each slot manages a single location/view
- Handles navigation state (back/forward history, current location)
- Recently modified to include terminal widget integration (see `nautilus_window_slot_toggle_terminal`)

**NautilusFilesView** (`src/nautilus-files-view.c`)
- Base view component that renders files
- Subclasses include NautilusListView and NautilusGridView
- Delegates to view-specific implementations (NautilusListBase)

**NautilusFile** (`src/nautilus-file.c`)
- Core file model representing a single file/directory
- Abstracts file metadata, icons, permissions, etc.
- Async operations for file info retrieval

**NautilusDirectory** (`src/nautilus-directory.c`)
- Represents a directory and its contents
- Monitors file changes and emits signals (files-added, files-changed, done-loading)
- Subclasses handle different directory types: NautilusVfsDirectory, NautilusSearchDirectory, NautilusStarredDirectory, NautilusNetworkDirectory

### View Architecture

The application supports multiple view modes through a hierarchical architecture:

1. **NautilusFilesView**: Top-level view manager
2. **NautilusListBase**: Abstract base for list/grid views
3. **NautilusListView** / **NautilusGridView**: Concrete implementations
4. Each view uses specialized cell widgets (NautilusNameCell, NautilusStarCell, NautilusLabelCell, etc.)

### Directory Types

- **NautilusVfsDirectory**: Standard filesystem directories
- **NautilusSearchDirectory**: Virtual directory for search results
- **NautilusStarredDirectory**: Virtual directory for starred files
- **NautilusNetworkDirectory**: Network locations and remote servers

### Search Architecture

Multiple search engines work together:
- **NautilusSearchEngineLocalsearch**: Uses Tracker/LocalSearch for indexed search
- **NautilusSearchEngineSimple**: Fallback directory traversal search
- **NautilusSearchEngineRecent**: Recent files search
- **NautilusSearchEngineModel**: Searches current model

### Extension System

Nautilus supports extensions via `libnautilus-extension`:
- **Column Providers**: Add custom columns to list view
- **Menu Providers**: Add context menu items
- **Info Providers**: Add custom file information
- **Properties Model Providers**: Add custom properties

Extensions are loaded from `$libdir/nautilus/extensions-4/`.

### Terminal Widget Integration (Custom Feature)

This fork includes an embedded terminal feature:
- **NautilusTerminalWidget** (`src/nautilus-terminal-widget.c`, `src/nautilus-terminal-widget.h`)
- Uses VTE library for terminal emulation
- Supports bidirectional sync between file manager and terminal
- SSH auto-connect for SFTP locations
- Toggle via `nautilus_window_slot_toggle_terminal()`

**Terminal Sync Modes:**
- `NAUTILUS_TERMINAL_SYNC_NONE`: No synchronization
- `NAUTILUS_TERMINAL_SYNC_FM_TO_TERM`: File manager navigation changes terminal directory
- `NAUTILUS_TERMINAL_SYNC_TERM_TO_FM`: Terminal `cd` changes file manager location
- `NAUTILUS_TERMINAL_SYNC_BOTH`: Bidirectional sync

### File Operations

File operations are asynchronous and progress-tracked:
- Located in `src/nautilus-file-operations.c`
- Support undo/redo via NautilusFileUndoManager
- Progress reported via NautilusProgressInfo

### DBus Integration

- **NautilusDBusManager**: Main DBus interface
- **NautilusFreedesktopDBus**: FreeDesktop.org interfaces
- **NautilusPortal**: File chooser portal implementation
- **NautilusShellSearchProvider**: GNOME Shell search integration

## Testing

Tests are located in `test/` directory:
- `test/automated/displayless/`: Tests that don't require display
- `test/automated/display/`: Tests requiring display/GTK
- `test/file-torture.py`: File system torture test

Run specific test categories:
```bash
# Only displayless tests
meson setup builddir -Dtests=headless
meson test -C builddir

# All tests including display tests
meson setup builddir -Dtests=all
meson test -C builddir
```

## Development Workflow

### File Chooser Development

Testing the file chooser portal requires special DBus commands (see CONTRIBUTING.md):

```bash
# Ensure development version is running first
# Open file chooser
gdbus call --session --dest org.gnome.Nautilus.Devel \
  --object-path /org/freedesktop/portal/desktop \
  --method org.freedesktop.impl.portal.FileChooser.OpenFile \
  '/org/gnome/Nautilus.Devel' 'org.gnome.Nautilus.Devel' '' 'Open a File' \
  "{'directory': <false>, 'multiple': <false>}"
```

### Git Workflow

Commit messages should follow GNOME conventions:
```
tag: Short explanation of the commit

Longer explanation explaining exactly what's changed and why, whether
any external or private interfaces changed, what issue were fixed (with
issue number if applicable) and so forth. Be concise but not too brief.

Closes #1234
```

## Important Files

- `src/nautilus-application.c`: Application entry point and lifecycle
- `src/nautilus-window-slot.c`: Tab/slot management and navigation
- `src/nautilus-files-view.c`: View rendering and file display
- `src/nautilus-file.c`: File model and metadata
- `src/nautilus-directory.c`: Directory model and monitoring
- `src/nautilus-file-operations.c`: Copy, move, delete operations
- `src/nautilus-terminal-widget.c`: Embedded terminal implementation (custom)
- `data/org.gnome.nautilus.gschema.xml`: GSettings schema
- `libnautilus-extension/`: Extension API headers

## GSettings Schema

Configuration is managed via GSettings:
- Schema ID: `org.gnome.nautilus`
- Schema file: `data/org.gnome.nautilus.gschema.xml`
- Access via: `g_settings_new("org.gnome.nautilus.preferences")`

## Translation

Translations are managed via gettext:
- POT file generation: Automatic via meson
- Translation files: `po/` directory
- Add new translatable files to `po/POTFILES.in`

## Flatpak Development Build

The development flatpak manifest is located at:
- `build-aux/flatpak/org.gnome.Nautilus.json`

Build and run flatpak:
```bash
flatpak-builder --user --install --force-clean builddir build-aux/flatpak/org.gnome.Nautilus.json
flatpak run org.gnome.Nautilus.Devel
```

## Code Style

- C11 standard
- Warning level 2
- Strict compiler warnings (see meson.build for full list)
- Use GLib/GTK conventions
- Deprecation warnings currently disabled (see meson.build line 73)

## Resources

- [Extension API Documentation](https://gnome.pages.gitlab.gnome.org/nautilus/)
- [Python Extensions](https://gnome.pages.gitlab.gnome.org/nautilus-python/)
- [GNOME Discourse](https://discourse.gnome.org/tags/nautilus)
- [Issue Tracker](https://gitlab.gnome.org/GNOME/nautilus/issues)
