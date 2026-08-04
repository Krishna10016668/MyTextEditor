<p align="center">
  <h1 align="center">📝 MyTextEditor</h1>
  <p align="center">
    A lightweight, terminal-based text editor written in C.
    <br />
    Fast. Minimal. No dependencies.
  </p>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/language-C99-blue?style=for-the-badge&logo=c&logoColor=white" />
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20WSL-orange?style=for-the-badge&logo=linux&logoColor=white" />
  <img src="https://img.shields.io/badge/version-0.0.1-green?style=for-the-badge" />
  <img src="https://img.shields.io/badge/license-MIT-purple?style=for-the-badge" />
</p>

---

## ✨ Overview

**MyTextEditor** is a terminal-based text editor built from scratch in C, designed to run directly in your terminal with zero external dependencies. It operates in **raw mode**, giving you full control over every keypress, and renders the entire interface using **ANSI escape sequences** — no curses library required.

Despite being under **1,200 lines of C**, it packs a surprising number of features that make it genuinely usable for quick edits and learning how text editors work under the hood.

---

## 🎯 Features

| Feature | Description |
|---|---|
| 📄 **File I/O** | Open, edit, and save files directly from the terminal |
| 🔍 **Incremental Search** | Real-time search with forward/backward navigation (`Ctrl-F`) |
| 🎨 **Syntax Highlighting** | Built-in highlighting for **C/C++** — keywords, types, strings, numbers, and comments |
| 💬 **Comment Support** | Highlights both single-line (`//`) and multi-line (`/* */`) comments |
| ↔️ **Horizontal & Vertical Scrolling** | Smooth scrolling in both directions for files of any size |
| 📑 **Tab Rendering** | Tabs are visually expanded to configurable tab stops (default: 8 spaces) |
| 📊 **Status Bar** | Displays filename, line count, modification status, filetype, and current line number |
| 💡 **Message Bar** | Shows context-sensitive help messages and auto-clearing notifications |
| ⚠️ **Dirty File Protection** | Requires pressing `Ctrl-Q` three times to quit with unsaved changes |
| ⌨️ **Full Keyboard Navigation** | Arrow keys, Home, End, Page Up, Page Down, and line-wrapping cursor movement |

---

## 🏗️ Architecture

The editor is organized into clearly separated modules within a single source file:

```
MyTextEditor.c
│
├── Defines & Data Structures    → Constants, enums, global state (editorConfig)
├── Filetypes / Syntax Database  → HLDB entries for language-specific highlighting
├── Terminal Layer                → Raw mode, key reading, window size detection
├── Syntax Highlighting Engine   → Per-row tokenizer with multi-line comment tracking
├── Row Operations               → Insert, delete, append, and render rows
├── Editor Operations            → Character/line insertion, deletion, newline handling
├── File I/O                     → Open, serialize, and save files to disk
├── Search (Find)                → Incremental search with match highlighting
├── Append Buffer                → Batched screen writes to prevent flicker
├── Output / Rendering           → Screen drawing, status bar, message bar, scrolling
├── Input Handling               → Prompt system, cursor movement, key dispatch
└── Initialization & Main Loop   → Setup, event loop
```

### Key Design Decisions

- **Single-file architecture** — Everything lives in `MyTextEditor.c` for simplicity and portability.
- **Raw terminal mode** — Bypasses the terminal's line buffering to handle every keypress individually.
- **Append buffer pattern** — All screen updates are batched into a buffer and flushed in a single `write()` call, eliminating flicker.
- **Dual row representation** — Each line stores both raw characters (`chars`) and a rendered version (`render`) with tabs expanded, enabling accurate cursor positioning.
- **Propagating highlight state** — Multi-line comments track open/close state per row and recursively update subsequent rows when the state changes.

---

## 🚀 Getting Started

### Prerequisites

- **GCC** (or any C99-compatible compiler)
- **Linux** or **WSL** (Windows Subsystem for Linux) — the editor uses POSIX terminal APIs (`termios`, `ioctl`, `unistd.h`)
- **Make** (optional, for convenience)

### Build

```bash
# Clone the repository
git clone https://github.com/Krishna10016668/MyTextEditor.git
cd MyTextEditor

# Build using Make
make

# Or compile manually
gcc -Wall -Wextra -pedantic -std=c99 MyTextEditor.c -o MyTextEditor
```

### Run

```bash
# Open a new empty buffer
./MyTextEditor

# Open an existing file
./MyTextEditor filename.c
```

---

## ⌨️ Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl-S` | Save the current file to disk |
| `Ctrl-Q` | Quit the editor (press 3× to force quit with unsaved changes) |
| `Ctrl-F` | Open incremental search |
| `Arrow Keys` | Move cursor (with line-wrapping support) |
| `Home` | Move cursor to beginning of line |
| `End` | Move cursor to end of line |
| `Page Up` | Scroll up one full screen |
| `Page Down` | Scroll down one full screen |
| `Backspace` / `Ctrl-H` | Delete character before cursor |
| `Delete` | Delete character under cursor |
| `Enter` | Insert a new line |
| `Escape` | Cancel current operation (in prompts/search) |

### Search Navigation

When in search mode (`Ctrl-F`):

| Key | Action |
|---|---|
| `↑` / `←` | Jump to previous match |
| `↓` / `→` | Jump to next match |
| `Enter` | Confirm and stay at match |
| `Escape` | Cancel search and return to original position |

---

## 🎨 Syntax Highlighting

The editor includes a built-in syntax highlighting engine with support for:

| Token Type | Color | Examples |
|---|---|---|
| **Keywords** (control flow) | 🟡 Yellow | `if`, `while`, `for`, `return`, `switch` |
| **Types** (data types & preprocessor) | 🟢 Green | `int`, `char`, `void`, `#include`, `#define` |
| **Strings** | 🟣 Magenta | `"hello"`, `'c'` |
| **Numbers** | 🔴 Red | `42`, `3.14`, `0xFF` |
| **Comments** (single & multi-line) | 🔵 Cyan | `// comment`, `/* block */` |
| **Search matches** | 🔷 Blue | Highlighted during `Ctrl-F` search |

### Supported File Types

| Language | Extensions |
|---|---|
| C / C++ | `.c`, `.h`, `.cpp` |

> **Extending syntax support:** Add new entries to the `HLDB[]` array in the source code with the appropriate file extensions, keywords, comment delimiters, and highlight flags.

---

## 📁 Project Structure

```
MyTextEditor/
├── MyTextEditor.c       # Complete editor source code (~1,800 lines)
├── Makefile             # Build configuration (gcc, C99, strict warnings)
├── .gitignore           # Ignores compiled binary
└── README.md            # This file
```

---

## 🔧 Configuration

The editor's behavior can be customized by modifying the `#define` constants at the top of `MyTextEditor.c`:

| Constant | Default | Description |
|---|---|---|
| `MyTextEditor_Version` | `"0.0.1"` | Version string displayed in the welcome screen |
| `MyTextEditor_TAB_STOP` | `8` | Number of spaces per tab stop |
| `MyTextEditor_QUIT_TIMES` | `3` | Number of `Ctrl-Q` presses required to quit with unsaved changes |

---

## 🧠 How It Works

1. **Terminal Setup** — The editor switches the terminal from *cooked mode* (line-buffered) to *raw mode*, allowing it to read individual keypresses without waiting for Enter.

2. **Input Loop** — The main loop calls `editorRefreshScreen()` to render the current state, then `editorProcessKeypress()` to read and handle the next key.

3. **Row Management** — Each line of text is stored as an `erow` struct with both raw and rendered representations. The rendered version expands tabs to spaces for accurate on-screen positioning.

4. **Rendering** — All screen output is assembled in an append buffer (`abuf`) and flushed in a single `write()` syscall. ANSI escape sequences handle cursor positioning, colors, and screen clearing.

5. **Syntax Highlighting** — On each row update, the highlighting engine walks through the rendered text character by character, classifying each as a keyword, string, number, comment, or normal text. Multi-line comment state propagates across rows.

6. **Search** — Incremental search updates results on every keypress. The original highlight state is saved and restored to avoid permanent visual artifacts.

---

## 🤝 Contributing

Contributions are welcome! Here are some ideas for improvements:

- [ ] Add syntax highlighting for more languages (Python, JavaScript, Rust, etc.)
- [ ] Implement undo/redo functionality
- [ ] Add line numbers in the left gutter
- [ ] Support mouse input
- [ ] Add copy/paste with system clipboard
- [ ] Implement auto-indentation
- [ ] Add configurable color themes

---

## 📜 License

This project is open source and available under the [MIT License](LICENSE).

---

<p align="center">
  Made with ❤️ and raw terminal escape sequences.
</p>
