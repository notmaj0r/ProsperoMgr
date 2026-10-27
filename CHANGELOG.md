# Prospero Manager — Changelog

Versions older than the current release are collapsed below — click a version to expand it.

## Unreleased

### Added
- **Extract password-protected `.zip` archives.** Extract and Open Archive now ask for the password when a ZIP is encrypted, the same way they already did for `.rar`. Supports the traditional ZIP encryption and WinZip AES-128/192/256.
- **A password field in the multi-archive Extract dialog**, so a batch of protected archives can be extracted with one password.
- **Show/hide toggle on archive password fields.**

## v1.1

### Fixed
- **The web UI could load to a blank white screen.** The PS5's browser doesn't support a CSS feature Tailwind v4 relied on for all its styling.
- **The web UI failed to load, or rendered with broken spacing, on 4.xx firmware.** Its older browser engine choked on a newer JavaScript syntax and lacked flexbox spacing support.
- **Language flags in the language picker showed as blank boxes** instead of an actual flag — replaced with bundled SVG icons.
- **A saved GitHub token that expired or was revoked broke every github.com repository check.** It now falls back to an anonymous request instead of failing outright.
- **Deleting the temp folder from Files broke the next payload download or Home tile install** until a restart.
- **Restoring a PS4 save backup gave a "corrupted save data" error.** The backup was missing the sealed key.
- **An exported save could not be imported back.**
- **Backing up, deleting and restoring a save took tens of seconds.**
- **Exporting a large save could run the console out of memory.**
- **Save errors showed in English in every language.**
- **PKG installs failed with `0x80b2116f`** once Prospero's tile was installed.
- **A rejected package blocked every later install until the console rebooted.**
- **A rejected install now says the download was kept**, and points at Debug Settings → Game → Package Installer.
- **A file download could close an unrelated file** when the console ran low on memory.
- **Copy and move jobs showed an untranslated label** in the Files job list.
- **PKG installs and payload downloads ignored a local DNS override** like nanoDNS.
- **A payload download overwrote an installed payload with the same filename.**
- **Enabling or disabling a payload could silently revert.**
- **A payload launch could report success when the PS5 rejected it.**
- **A password-protected `.rar` said the archive was corrupt** instead of asking for the password.
- **The file list didn't refresh right after a fast job finished** (a quick rename, delete, or small copy).
- **Extracting an archive and choosing "Cancel" on a naming conflict silently overwrote the file instead.**

### Added
- **Extract `.rar` archives, including password-protected ones**, the same way `.zip` already works.
- **Preview images (PNG, JPG, GIF, BMP, WEBP) without downloading them.**
- **Keyboard navigation in Files** — arrow keys to move, Enter to open, F2 to rename, Delete to remove.
- **Upload conflicts can now be skipped or kept as a copy**, not just replaced.
- **Install multiple PKGs at once, from your PC/phone or PS5 storage.** They queue and install one after another automatically.
- **Install packages straight from USB and extended storage.** Nothing is copied to internal storage first.
- **Install from a URL without downloading the package first.** Needs a server that supports range requests.
- **Packages show their own title while installing**, not just a content ID.
- **Import a save** from an exported zip or a backup.
- **Delete save data** for a title — it's backed up first, so it can be restored.
- **Restore and Back up all run in the background** and show in Background activity.
- **Restore can skip its safety backup** — faster, but then it can't be undone.
- **Configurable payloads directory** (Settings → General). Takes effect after restarting `ProsperoMgr.elf`.

### Changed
- **PC/phone PKG installs now stream straight into RAM instead of a temp file** — faster, no double storage. Keep the browser tab open until the install finishes.
- **Restoring a save is undoable and all-or-nothing.** It refuses while the game is running.
- **Background activity no longer labels an unknown job "Creating ZIP from".**
- **Removed the in-process fallback installer.**

<details>
<summary><b>v1.0</b></summary>

Initial release.

- Native file manager for internal, USB, and extended storage — upload, download, rename, copy, move, delete, multi-select, background jobs
- Resumable uploads and streamed downloads tuned for the PS5's transfer path
- Streamed ZIP engine (create, extract, inspect, download-as-ZIP) with ZIP Slip protection
- Local payload library — import, favorites, enable/disable, launch/stop, separate view for unmanaged payload-like processes
- Payload Repositories tab — curated community projects grouped by developer, upstream release checks, custom repositories
- PKG installer — from PS5 storage or a direct URL
- Save data backups — manual and automatic per-title backups, restore, backup history
- Payload Autoloader — configurable boot sequence with delays, retries, notifications, and a same-boot guard
- Process manager — inspect and stop running processes
- Thermal panel — live CPU/SoC/M.2 telemetry and fan-threshold controls
- Profile tools and experimental account operations
- 16 languages with locale-aware formatting and RTL support
- Command palette (`Ctrl/Cmd + K`) and native PS5 notifications

</details>
