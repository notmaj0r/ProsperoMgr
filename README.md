<p align="center">
  <img src="ProsperoMgr.png" alt="Prospero Manager" width="240"/>
</p>

<h1 align="center">Prospero Manager</h1>

<p align="center">
  <b>A Web-Based All-In-One homebrew manager for your PlayStation 5!</b>
</p>

<p align="center">
  <a href="https://github.com/notmaj0r/ProsperoMgr/releases/latest"><img src="https://img.shields.io/github/v/release/notmaj0r/ProsperoMgr?label=Release&logo=github&color=blue" alt="Latest release"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPLv3"/></a>
  <a href="https://discord.gg/E4g6fEqp46"><img src="https://img.shields.io/badge/Discord-Join%20our%20Server-5865F2?logo=discord&logoColor=white" alt="Join our Discord Server"/></a>
  <a href="https://ko-fi.com/maj0r"><img src="https://img.shields.io/badge/Ko--fi-Support-FF5E5B?logo=kofi&logoColor=white" alt="Support on Ko-fi"/></a>
</p>


---

**Prospero Manager** runs as a single ELF on an already-jailbroken PS5 and serves its complete interface over HTTP.

No desktop client is required. Send `ProsperoMgr.elf` to the console, open the PS5 IP address in a browser, and manage the system from a desktop, tablet, or smartphone.

Prospero brings the most common PS5 homebrew-management workflows together in one interface:

- Browse and manage PS5 storage;
- Upload and download large files;
- Create, inspect, download, and extract ZIP archives — extract RAR archives too, with password-protected ZIP and RAR support;
- Preview images without downloading them;
- Manage and launch payloads;
- Browse community payload repositories and track new releases;
- Install PKG files, including several at once queued in sequence;
- Back up and restore save data;
- Inspect running processes;
- Configure payload autoload sequences;
- Monitor temperatures and cooling configuration;
- Manage selected profile/account functionality;
- Inspect logs and background activity.

> **One payload. One dashboard. No companion application required.**

<!--
Screenshots can be added here later, for example:

## 📸 Screenshots

<p align="center">
  <img src="docs/screenshots/home.png" width="48%" />
  <img src="docs/screenshots/files.png" width="48%" />
</p>
-->

---

## ✨ Features

- **Native file manager** — browse internal, USB, and extended storage; upload, download, rename, copy, move, delete, multi-select, keyboard navigation, and run long operations as background jobs.
- **High-speed transfers** — adaptive resumable uploads, large streamed downloads, PS5-specific socket/file-transfer tuning, and browser-friendly transfer handling.
- **Archive tools** — streamed and cancelable ZIP creation/extraction, password-protected ZIP and RAR extraction, live per-file progress, multiple compression modes, archive inspection, temporary **Download as ZIP**, conflict handling with replace/skip/keep-both, and ZIP Slip/path-traversal protection.
- **Image preview** — view PNG, JPG, GIF, BMP, and WEBP files without downloading them.
- **Payload library** — upload and manage local payloads, favorites, enable/disable state, launch/stop actions, and separate visibility for payload-like processes started outside Prospero.
- **Payload Repositories** — curated community projects grouped by developer, latest-release checks, custom repositories, and direct links to the real upstream GitHub/Gitea project pages.
- **PKG installer** — install `.pkg` files from PS5 storage, USB/extended storage, a direct URL, or streamed straight from your PC/phone with no temporary copy on the console; queue several packages to install one after another.
- **Save data** — manual per-title backups, downloadable archives, restore flows, and an optional automatic per-title backup watcher.
- **Payload Autoloader** — configurable boot sequence with autosave, delays, retries, notifications, and a same-boot guard that prevents duplicate automatic launches after redeploying `ProsperoMgr.elf`.
- **Process manager** — inspect running processes and stop supported processes when needed.
- **Thermal controls** — live CPU/SoC/M.2 telemetry and fan-threshold controls when the required environment is available.
- **Accounts (Experimental)** — selected offline account operations with explicit confirmation and automatic backup safeguards.
- **Profile tools** — change the foreground user's display name and apply a custom avatar.
- **16 Languages** — complete UI translation with locale-aware formatting and RTL support for Arabic and Persian.
- **Responsive interface** — designed for desktop, tablets, and smartphones.
- **Command palette** — quick keyboard navigation with `Ctrl/Cmd + K`.
- **Background activity** — persistent progress for transfers, archives, PKG operations, and Autoloader jobs.
- **Native PS5 notifications** — startup, Autoloader, and job-completion feedback directly on the console.

---

## 🚀 Getting Started

### Requirements

Prospero Manager is intended for a PS5 that already has a compatible jailbreak/homebrew environment running.

You will need:

- An already-jailbroken PlayStation 5;
- A compatible ELF/payload loader;
- A computer, phone, or tablet on the same local network as the console.

### Run Prospero Manager

Send:

```text
ProsperoMgr.elf
```

to the PS5 using your preferred ELF loader.

Then open the dashboard from another device on the same network:

```text
http://<PS5-IP>:7070
```

Example:

```text
http://192.168.1.100:7070
```

Prospero serves the complete frontend directly from the ELF.

No separate web server or desktop companion app is required.

---

## 📁 File Manager

The File Manager is designed to behave like a full system file manager rather than a simple file browser.

It supports:

- Internal PS5 storage;
- USB storage;
- Extended storage where available;
- File and folder navigation;
- Multi-selection;
- Select-all and bulk actions;
- Upload;
- Browser download;
- Copy;
- Move;
- Rename;
- Delete;
- ZIP creation;
- ZIP and RAR extraction, including password-protected archives;
- Image preview;
- Download as ZIP;
- Background file jobs;
- Progress, speed, and cancellation for long-running operations;
- Keyboard navigation — arrow keys, Enter, F2 to rename, Delete to remove;
- Responsive layouts for desktop and mobile devices.

Large uploads use resumable chunking so a long transfer does not need to restart from zero after a recoverable interruption.

Large downloads are streamed rather than loading the whole file into backend memory.

---

## 📦 ZIP & RAR Archive Tools

Prospero includes a streamed archive engine built around `miniz` for ZIP, and the UnRAR library for RAR extraction.

Archive operations are background jobs and do not require the whole archive to be loaded into memory.

### Create ZIP

Archives can be created from:

- One file;
- One folder;
- Multiple files;
- Multiple folders;
- Mixed selections.

Available compression modes include:

- **Smart** — chooses a practical strategy based on the selected data;
- **Store** — no compression, prioritizing maximum archive-creation speed;
- **Fast** — fast Deflate compression;
- **Balanced** — more compression at a higher CPU cost.

### Extract ZIP / RAR

Prospero can:

- Inspect archive metadata before extraction;
- Extract into a dedicated folder or selected destination;
- Prompt for a password when a ZIP or RAR archive is encrypted (ZIP: traditional ZIP encryption and WinZip AES-128/192/256);
- Report per-file progress;
- Preserve empty directories;
- Cancel a large extraction;
- Handle destination conflicts — replace, skip, keep both, or cancel;
- Optionally delete the source archive after a successful extraction.

RAR archives can only be extracted, not created — that's an UnRAR license restriction, not a Prospero limitation.

Archive paths are validated before writing to disk to prevent ZIP Slip/path-traversal entries from escaping the selected destination.

### Open Archive

ZIP and RAR files can both be inspected without extracting the whole archive first.

The archive inspector can show the archive structure and metadata and then hand off to the normal extraction workflow.

### Download as ZIP

**Create ZIP** and **Download as ZIP** are intentionally different operations.

- **Create ZIP** creates a persistent archive in the filesystem.
- **Download as ZIP** creates a temporary Prospero-managed archive for the browser download flow and cleans it up afterward.

---

## 🚀 Payloads

The Payloads tab is the local payload library.

It supports:

- Drag-and-drop upload;
- Importing payloads from the PS5 filesystem;
- Search and filters;
- Favorites;
- Enable/disable state;
- Bundled and user-added payloads;
- Launch;
- Stop when a matching process is running;
- Background launch feedback.

Payload-like processes that are already running but are not registered in Prospero can be shown separately so they are not confused with locally-managed payload files.

The payloads directory itself is configurable from `Settings → General → Payloads directory` — point it at a folder already used by another homebrew manager instead of keeping a separate copy of every payload. Takes effect after restarting `ProsperoMgr.elf`.

Subdirectories are scanned too, so Payload Manager's `/data/pldmgr/payloads/<name>/<name>.elf` layout can be used directly without importing or copying files. Payloads are still identified by filename: a file in the root takes precedence over nested copies with the same filename; otherwise the first path in lexicographic order is used. Renaming or deleting a shared payload changes the original file used by the other manager.

Prospero intentionally keeps the primary action hierarchy simple:

- **Launch** when a payload is available;
- **Stop** when it is running;
- **Enable** when it is disabled;
- Secondary management actions remain contextual.

---

## 🌐 Payload Repositories

The **Payload Repositories** tab is a discovery and release-monitoring directory.

It does **not** download or install payloads from the catalog.

Prospero ships with a curated list of PS5 homebrew projects and groups them by developer.

For supported GitHub/Gitea repositories, Prospero checks the upstream releases API and displays the newest non-draft, non-prerelease tag when available.

Each entry provides an **Open** action that takes you directly to the project's real upstream repository.

Users can also add custom repositories they want Prospero to keep watching. Custom entries participate in the same developer grouping and version-checking flow.

> Payload Repositories is deliberately browse/open-only. Prospero does not mirror or rehost community payloads.

The Payloads system may still use separate direct-URL functionality for an explicitly supplied payload asset URL; that is independent from the curated Payload Repositories catalog.

Outbound repository requests use the project's PS5-native HTTP transport.

---

## 📦 PKG Installer

Prospero can install `.pkg` files using multiple workflows.

Supported sources include:

- A PKG already present on internal, USB, or extended PS5 storage;
- A direct URL, streamed without downloading the whole package first;
- Directly from a PC or phone on the same network — streamed straight into RAM, with no temporary copy written to the console first.

Select several PKGs at once, from any mix of sources, and they queue and install one after another automatically.

Large PKG transfer/staging work is kept separate from the final PS5 installation stage.

The backend uses a private worker for AppInst-related work so long-running installation operations do not unnecessarily block the main web-server process.

---

## 💾 Save Backups

The Saves interface provides per-title save-management workflows.

Features include:

- Manually create a backup;
- Download a backup archive;
- Restore an existing backup;
- Inspect backup history;
- Optional automatic per-title backup watching.

Automatic backups use a settle-window approach so Prospero does not immediately capture save data while it may still be changing.

Restore operations are treated as destructive actions and should be used carefully.

---

## ⚡ Payload Autoloader

Prospero includes a complete dashboard-based Payload Autoloader for automatically launching your preferred payload sequence when Prospero starts.

Open:

```text
Settings → Autoloader
```

From the dashboard you can:

- Enable or disable the Autoloader;
- Configure the startup delay;
- Add payloads to the sequence;
- Add delays between payload launches;
- Reorder sequence steps;
- Remove payloads or delays;
- Configure retry and failure behavior;
- Control bundled-payload autostart behavior;
- Monitor the current Autoloader status;
- Run the complete sequence manually whenever needed.

All Autoloader changes are saved automatically from the dashboard, so there is no separate Save button to manage.

### Same-Boot Session Guard

Prospero tracks the console's actual boot session using `kern.boottime`.

If `ProsperoMgr.elf` is redeployed during the same PS5 power-on session, the new Prospero instance can avoid automatically launching payloads that were already loaded earlier in that boot.

When the sequence is already satisfied, Prospero can report:

```text
ProsperoMgr: Payloads already loaded!
```

This guard resets naturally after a real PS5 reboot.

The user can always bypass the automatic guard manually with:

```text
Run sequence now
```

from Settings → Autoloader.

---

## ⚙️ Process Manager

Prospero can inspect processes running on the PS5 and present them in a dedicated advanced interface.

The process manager exposes technical information such as process IDs while keeping destructive actions separate from normal browsing.

Supported stop operations use the platform's process signaling path.

Use process termination carefully.

---

## 🌡️ Thermal

The Thermal interface can display live system temperature information such as:

- CPU;
- SoC;
- M.2/storage telemetry where available.

It also exposes fan-threshold configuration when the required kernel/homebrew environment is available.

Thermal functionality depends on the console environment and required supporting payload functionality.

---

## 👤 Profile & Accounts

### Profile

Prospero can work with the foreground PS5 user's profile, including:

- Display-name changes;
- Custom avatar application.

### Accounts — Experimental

Selected account functionality is exposed under an explicitly experimental interface.

Operations that modify account-related state are gated behind deliberate confirmation and backup safeguards.

Treat experimental account features carefully.

---

## 🌍 Languages

Prospero Manager supports a fully localized interface in:

- 🇺🇸 **English**
- 🇸🇦 **Arabic**
- 🇨🇳 **Chinese Simplified**
- 🇫🇷 **French**
- 🇩🇪 **German**
- 🇮🇹 **Italian**
- 🇯🇵 **Japanese**
- 🇰🇷 **Korean**
- 🇮🇷 **Persian**
- 🇵🇱 **Polish**
- 🇧🇷 **Brazilian Portuguese**
- 🇷🇺 **Russian**
- 🇪🇸 **Spanish**
- 🇹🇭 **Thai**
- 🇹🇷 **Turkish**
- 🇺🇦 **Ukrainian**

English is the default language.

Arabic and Persian use RTL layouts, while technical values such as:

- File paths;
- URLs;
- Process IDs;
- Title IDs;
- Versions;
- Logs;

remain readable in their natural LTR form.

The interface also uses locale-aware:

- Pluralization;
- Dates;
- Times;
- Numbers.

---

## 🔄 Updating Prospero Manager

When updating Prospero during the same console session, send the new:

```text
ProsperoMgr.elf
```

using the normal deployment method.

Prospero's same-boot Autoloader guard is designed so redeploying the manager does not automatically duplicate payload launches that were already completed during the current PS5 boot.

If you intentionally want to replay the sequence afterward, use:

```text
Settings → Autoloader → Run sequence now
```

Persistent Prospero data lives under its `/data/ProsperoMgr/` storage area.

---

## ⚠️ Disclaimer

Prospero Manager is a homebrew management application intended for **already-jailbroken PlayStation 5 consoles**.

It is not intended to provide or replace the jailbreak/homebrew environment required to run it.

Prospero exposes advanced operations involving:

- Console storage;
- Files and directories;
- Payload processes;
- PKG installation;
- Save backups/restores;
- Account-related data;
- Thermal configuration.

Some operations can modify or delete data.

Use advanced and experimental features carefully and keep backups of important files and save data.

Use Prospero Manager at your own risk.

---

## 🛠️ Building

Prospero Manager is built with the **PS5 Payload SDK**.

Set your SDK path:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

Build the project:

```sh
./build.sh
```

Output:

```text
build/ProsperoMgr.elf
```

To send the ELF to your PS5:

```sh
python3 source/tools/send_payload.py --host <ps5-ip>
```

Useful build options:

```sh
./build.sh --clean
./build.sh --skip-frontend
```

---

## 📁 Repository Layout

```text
.
├── build/          Build output
├── pkg-source/     PS5 home-screen tile sources
├── source/
│   ├── backend/    C++20 PS5 backend
│   ├── frontend/   React + Vite + Tailwind UI
│   ├── helpers/    Internal helper/worker code
│   ├── payloads/   Bundled payloads
│   └── tools/      Build and deployment tools
├── Makefile
└── build.sh
```

The frontend is built and embedded directly into `ProsperoMgr.elf`.

---

## ❤️ Support

Prospero Manager is free and open-source.

If you find it useful, consider supporting further development.

<a href="https://ko-fi.com/maj0r"><img src="https://img.shields.io/badge/Ko--fi-Support-FF6433?style=for-the-badge&logo=kofi&logoColor=white" alt="Support ProsperoMgr on Ko-fi"/></a>
<a href="https://buymeacoffee.com/maj0r"><img src="https://img.shields.io/badge/Buy%20Me%20a%20Coffee-Support-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black" alt="Support ProsperoMgr on Buy Me a Coffee"/></a>

**PIX - Brazilian method**

**LivePix**: https://livepix.gg/notmaj0r

**Donate via Crypto**

- **BTC**  
  `bc1ppfcad9ytyzmmzq3nfatfcag7q38rqcpsvxfqusq0uzv4gh82pa4syeknlk`

- **BCH (Bitcoin Cash)**  
  `qz2h97c2pyexn3fwhveu33tn5xkd3qlfn5796zhtcq`

- **USDC (Solana)**  
  `BNpezXSQWuUR9HWGjzQUS7AeEBCwiY2ZGxArWNwYXKKV`

- **USDT (TON)**  
  `UQA18f4cVS0778fL_nIssdN1GIHRYR7dQ3UnNgjPwgYvIQi_`

---

## 🤝 Credits & References

- **[Ps5-Payload-Dev](https://github.com/ps5-payload-dev)** — for the [SDK](https://github.com/ps5-payload-dev/sdk) Prospero is built with; [elfldr](https://github.com/ps5-payload-dev/elfldr), the loader every payload launch streams through; the bundled [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) and [klogsrv](https://github.com/ps5-payload-dev/klogsrv) payloads (shipped unmodified); and the [offact](https://github.com/ps5-payload-dev/offact) project, used as a reference.
- **[Sonic-Iso](https://git.etawen.dev/soniciso)** — for the [elf-arsenal](https://git.etawen.dev/soniciso/elf-arsenal) project, used as a reference.
- **VoidWhisper** — for the VoidShell project, used as a reference.
- **[PLK](https://github.com/itsPLK)** — for [PS5-Lapy-JB-Daemon](https://github.com/itsPLK/PS5-Lapy-JB-Daemon), [ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager), and [ps5-pkg-manager](https://github.com/itsPLK/ps5-pkg-manager), used as a reference.
- **[Earthonion](https://git.etawen.dev/earthonion)** — for the [np-fake-signin](https://git.etawen.dev/earthonion/np-fake-signin) and [garlic-savemgr](https://git.etawen.dev/earthonion/garlic-savemgr) projects, used as a reference. 
- **[phantomptr](https://github.com/phantomptr)** — for [ps5upload](https://github.com/phantomptr/ps5upload), used as a PS5 transfer and integration reference.
- **[owendswang](https://github.com/owendswang)** — for [PS5 Web File Manager](https://github.com/owendswang/ps5-web-file-manager) and [ps5-fan-control](https://github.com/owendswang/ps5-fan-control), used as file-management and fan-control references.
- **[seregonwar](https://github.com/seregonwar)** — for [zftpd](https://github.com/seregonwar/zftpd), used as a PS5 networking and transfer reference.
- **[drakmor](https://github.com/drakmor)** — for [fan_target](https://github.com/drakmor/fan_target) and [ps5-hwinfo](https://github.com/drakmor/ps5-hwinfo), used as fan-control safety and hardware-telemetry references.
- **[Libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/)** — the HTTP server used by the backend.
- **[Alexander Roshal / RARLab](https://www.rarlab.com/)** — for [UnRAR](https://www.rarlab.com/rar_add.htm), the library RAR extraction is built on. See `source/backend/third_party/unrar/license.txt` for its license terms.
- Everyone testing, reporting issues, sharing logs, and helping improve the project.

---

## 📜 License

Prospero Manager is licensed under the **GNU General Public License v3.0**.

See the `LICENSE` file for the full license text.

```text
GNU General Public License v3.0
```

---

<p align="center">
  <b>Prospero Manager</b><br/>
  One dashboard for your jailbroken PS5.
</p>
