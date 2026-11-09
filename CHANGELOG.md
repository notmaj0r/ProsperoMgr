# Prospero Manager — Changelog

Versions older than the current release are collapsed below — click a version to expand it.

## v1.2

### Added
- **Media page:** lists the PS5's screenshots and clips grouped into albums by game, with All / Screenshots / Videos filters and day headings. Viewer with previous/next, playback where the browser supports it, and download. Several items can be selected for download. Thumbnails are generated on the console and cached.
- **Media delete:** screenshots and clips can be deleted from the viewer or from a multi-selection, with a confirmation.
- **Media, leftovers cleanup:** captures that the console still lists but whose image no longer exists are counted in a notice with a Clean up button, which removes their gallery rows and remaining files.
- **Media, game names:** albums use the game name stored in the console's gallery database, so captures of uninstalled games keep their own named album.
- **Media albums:** Albums tab with one cover per game, item count and size. Captures of the Home screen are grouped in their own album, and captures of games no longer installed in one shared album. Select all, and Download as ZIP (one archive instead of one file per capture). Thumbnails enlarged to 560 px. Clips show a frame on computers and phones.
- **Home library: Games and Apps tabs.** Games and apps are listed apart. Apps holds installed apps and homebrew (Netflix, YouTube, Spotify, CheatRunner, ShadowMount+, the Homebrew Launcher and others), told apart by the `app.db` fields `is3rdPartyVideoApp` and `categoryType`. Streaming apps no longer appear under Games, system entries stay hidden and the platform filter applies to Games only. Prospero Manager's own tile is listed too.
- **Play time chip on game covers:** a dark chip with a clock and the time played ("33 min", "2 h", "5 h 20 min"), in the language of the interface, like the console's home screen. Titles never played show no chip.
- **Home game cards:** the game size is shown next to the version, on one line with icons.
- **Console notifications in the system language:** the toasts shown on the PS5 (autoloader, save backups, installs, updates, startup) follow the console's language, in 16 languages. System languages without a translation use the closest one, or English. The log stays in English.
- **Safe removal of USB drives:** a Safely remove button on each USB and extended USB drive in Files asks the console's own USB service to eject it, after a confirmation, refuses while a file operation is running, and confirms when the drive is no longer mounted. `GET /api/storage/usb` lists what the service reports.
- **Saves, delete:** the backup before deleting a save is optional, with a Back up before deleting checkbox in the confirmation (off by default).
- **Files, compact rows:** a rows button in the toolbar switches between compact and comfortable rows. Compact rows are about 50 px tall, show the date only (the time stays in the tooltip) and hide the permissions column. Comfortable rows are the default, and the console's browser always uses them. The choice is remembered.
- **Autoloader steps:** the remove button is discreet (red on hover), the reorder arrows appear on hover or focus with a mouse and stay visible on touch screens, and the rows are shorter.
- **Files, game folders:** any folder with a game's `sce_sys/param.json` (whatever its name) and `.exfat` images (read from inside the image, whatever the file is named) show the game's name and cover, with the ID and folder name below. Search matches the game name.
- **Autoloader run view:** timeline with one step per payload (version, size, origin, live state, duration), a headline with the countdown or percentage, and a progress line.
- **PKG install:** package segments are sent over HTTP instead of the WebSocket, which now carries control messages only. Reaches up to 111 MB/s (was ~75–90 MB/s).
- **PKG install log:** includes the average and peak queue of segments waiting to be sent.
- **PKG upload window:** extra segments sent on a seek request count against the in-flight window, keeping it at its configured size.
- **Files:** the selection bar stays visible while scrolling.
- **Processes page:** the list refreshes by itself every few seconds without blinking, has a fixed header, an All / Payloads filter, a "Payload" tag on homebrew processes and a "showing N of M" count.
- **Logs:** following the log stops when you scroll up and resumes at the bottom, and the footer shows how many lines are listed.
- **Dialogs are larger in the console's browser.**
- **Spacing in the console's browser:** if its browser does not support flex gaps, Prospero now detects it and spaces rows with margins instead.
- **Loading placeholders instead of "Loading…" text** in the PKG, archive, folder-picker and payload-release dialogs, and in Logs, Profile and the ShadowMount folders list.
- **Cleaner "Close game" button** on Home, in the same red style as the other stop buttons.
- **Save data list reorganized:** each game keeps everything on the left (name, platform, ID, files, size, save names) with "Back up now" and a menu on the right. The user and account ID are shown once above the list instead of on every row, and an account warning only appears when a game's saves belong to different accounts.
- **Save Backups is tidier:** auto-backup is one row with its switch, a "Back up all" button with a progress bar, and the destination drive in view (the sleep delay and drive list sit under Settings, open by themselves when a drive has a problem). Each save's main action is "Back up now"; ZIP download and delete moved into a menu, as did download and delete for backups. Lists load with placeholders instead of a text line.
- **Install PKG is simpler:** the three ways to add a package are now tabs, with one line explaining the selected one, and the file chooser is a single compact row.
- **The Autoloader page shows the run while it is open:** it switches to the Autoloader tab when a run starts (once per run). Prospero does not open the browser by itself.
- **Uploads:** progress is kept when changing page, listed in Background activity on every page with a cancel button, and shown again in Files.
- **Right-click menus:** right-clicking a payload card, a save, a save backup or a custom repository opens the same menu as its three-dot button, as in Files.
- **Back and Forward on every page:** the Media tabs and open album and the Install PKG method tabs are history steps (Back leaves an album before it leaves the page), and Back closes the language picker and the notifications panel.
- **Files, Back and Forward:** every opened folder is a history step; Back and Forward (mouse side buttons, the O button on the console) move between visited folders, and Back leaves the page after the first one. `#/ftp?path=/data` opens that folder.
- **Interface transitions:** thumbnails and game covers fade in, error/warning banners and the Media selection bar ease in, filter tabs compress on press. Disabled on the console's browser and with reduced motion.
- **Game covers are shown whole.** Home cards are now vertical, with the full square cover on top, and the grid fits as many columns as the screen allows.
- **Bigger targets in the console's browser:** buttons, menu items, tabs, filter buttons and file rows are taller there, since the cursor is steered with a stick.
- **Press the PS button to abort the Autoloader** during its countdown. The countdown is at least 5 seconds, even if the delay is set lower, and the notification at the start says how long it lasts.
- **Back also undoes a mode:** it clears a selection in Files and leaves the favorites mode in Payloads, before it leaves the page.
- **Backspace goes up one folder** in Files (Alt+Up too). Delete is now the only key that deletes.
- **Each page names the browser tab**, and opens scrolled to its top.
- **Download decrypted** for executables (`.self`, `.sprx`, `.prx`, `eboot.bin`) in the file menu: a SELF comes back as a plain ELF. It goes through the bundled ftpsrv, so launch ftpsrv from Payloads first.
- **Settings → Hardware:** CPU and GPU clocks, CPU load, memory and video memory use, and the memory and fabric clocks. Readings the console does not report are left out.
- **Back button closes dialogs and menus** instead of leaving the page, including the O button on the console.
- **Right-click a file or folder** to open its actions where the pointer is.
- **Finished file jobs clear themselves after 5 seconds.** Settings → General has a "Clear after" delay (never, 1 s, 5 s, 10 s, 30 s, 1 min or 5 min) to change that for completed copy, move, zip, extract and delete jobs on the Files page. Jobs that failed or need attention stay.
- **Package review groups by game** and installs in a safe order: base game, then updates (oldest first), then DLC.
- **Cover art** in the package review and queue, read from the package itself, and on the console's own install card together with the game name.
- **Package review warns before the upload** when the game is already on the console, when an update is not newer than the installed version, or when an update has no base game.
- **Folder permissions** now apply to everything inside the folder.
- **Hungarian** translation.
- **Payload Manager libraries:** pointing the Payloads directory at a Payload Manager folder now finds its payloads, which sit one per subfolder (`name/name.elf`).
- **Payload Manager downloads:** payloads installed by Payload Manager (`/data/pldmgr/payloads`, including `.bin` ones such as etaHEN) are listed automatically, without changing the Payloads directory, and marked "From Payload Manager". Rename and delete work on them in place. If a payload has the same name in both folders, the newer file is shown.
- **Power menu** in the header: standby, reboot or shut down. Schedule any of them after a delay or when all jobs finish.
- **Automatic updates** from GitHub releases, with a manual option in Settings → About.
- **Autoloader page** in the left menu, and "Add to Autoloader" on each payload card.
- **Home overview:** temperature, memory, disk space, uptime, running payloads, and a QR code to open Prospero on your phone.
- **Temperature history chart** in Settings → Thermal.
- **Activity panel controls:** pause the queue, reorder or cancel jobs, and "Shut down when finished".
- **PKG install:** review step with type, version, size and a free-space check, a live progress card, retry, cancel, and recent installs.
- **PKG upload from your PC or phone** is faster (up to ~100 MB/s at the start), with no repeated transfers.
- **Payloads page:** summary tiles and a live upload card. Name conflicts offer Replace, Skip or Keep both.
- **File manager:** file-type icons, size and date filters, bookmarks for any folder (kept when renamed), undo for deletes, and faster uploads of many small files.
- **Close game** button on Home for the running game.
- **Uninstall games** from the Home library with the delete button on each game.
- **RAR:** one password for a batch extraction.
- **Save backups:** copy existing backups to a USB drive or any folder.
- **Saves tab shows the names of the saves inside each game and the account ID that owns them**, read from the metadata the console keeps beside the save. Nothing is mounted or changed.
- **Save backups to several USB drives:** pick more than one drive. Every backup (manual, back up all, and after closing a game) is written to each one, and each drive shows its own result. A missing, full or read-only drive is reported, never replaced by internal storage.
- **More payloads** in Repositories: OnionHEN, offact, elf-arsenal, ps5-pkg-manager, ps5upload, ps5-web-file-manager, ps5-fan-control, zftpd, fan_target, ps5-hwinfo.

### Changed
- **The Autoloader page waits for its data.** It shows a loading state until the settings and the payload list have both arrived, and an error with Retry if they cannot be loaded, instead of showing empty defaults that a late answer could overwrite.
- **Adding a step is easier.** Payloads you can add are listed with their version and where they came from, and a search appears when there are many.
- **The Autoloader sequence is clearer.** On wide screens the sequence and the steps you can add sit side by side. A payload that would conflict (a second KStuff, or KStuff with etaHEN) cannot be added and says why, and each warning has a button that takes you to the step. Waits are entered in seconds, and while it runs each step shows Running, Done or Error.
- **Game and save icons are cached by the browser** (one hour) and streamed from disk instead of being read into memory and refetched on every visit. In the console's browser, the file job check and the startup check of the Autoloader poll three times less often, and selecting a Media tile no longer re-renders the whole grid.
- **Lighter on the PS5 browser.** The shell no longer redraws every few seconds because of the uptime counter, an idle Files page and an idle Logs page stop redrawing on each poll, the Payloads page ignores a poll that finds the same processes, and the Logs page polls less often on the console.
- **File deletion:** files are removed by six threads in parallel, and each delete writes its timing to the log.
- **Deleting in the file manager is immediate.** The delete is sent as soon as you confirm, with no 8 second wait and no Undo, so the space frees up at once and a reload can no longer cancel it.
- **Faster uploads and big downloads in the file manager.** An upload now writes to the disk on its own thread while the next part arrives from the network. A file of 256 MB or more is read ahead of the network when downloaded. Files of 128 MB to 512 MB are also sent in larger parts, with fewer pauses between them.
- **The Payloads tab is now a simple launcher.** A grid of large buttons: one tap launches a payload, and a payload that is running shows a red Stop button. Each button shows its version, size and where it came from (included, uploaded, repository or USB). Favorites come first, and the star button switches to picking them. Upload, From PS5 and Autoloader sit in the header, and each payload has its own menu (rename, Autoloader, disable, remove) that is always there. Filters, counters and sorting are gone, and search only appears with many payloads.
- **The Install PKG options are clearer.** "From your PC or phone" is now separate from "On this PS5" (a USB drive plugged into the PS5, or its internal storage), each with a plain description. In the PS5's own browser, "On this PS5" comes first and is selected by default, and the PC option explains why it does not apply.
- **Install notifications show the game name** instead of a generic "Install queued", also for browser uploads and links.
- **A package refused for lack of space** now says the console is out of free space.
- **The file manager sorts by type by default.** Your own choice is still remembered.
- **Temperature history** is easier to read: current, peak (with the time it happened), average and lowest for CPU and SoC, a 5 min / 15 min / 1 h range, labelled warm and hot limits, and a readout when you hover the chart.
- **Faster on the PS5 browser:** pages load on demand, files are cached, long lists load in pages, and heavy effects are off.
- **Visual polish** across cards, buttons and the Home page, plus visible controller focus on the PS5.

### Fixed
- **File names that are not valid UTF-8** (from a FAT drive or an old ZIP) can be opened, downloaded and deleted, instead of failing because the name no longer matched.
- **The Home games list is no longer empty** while the console is writing to its database (during an install); it waits briefly instead.
- **Sidebar:** the logo is no longer squeezed when the sidebar is collapsed.
- **Apps, web-based homebrew:** apps whose tile is a web address (CheatRunner, ShadowMount+, the Homebrew Launcher, the browser tile, Slopkit) are opened in the console's browser from the Home page. They were refused as apps with `0x80b21401`.
- **Apps opened from the system's own address** (Debug Settings): they cannot be started from outside, so Prospero sends a notification to the console with an Open button that the shell itself handles.
- **Home library:** the column check on `app.db` compared pointers instead of names and always failed, so playtime and last played were never read and apps could not be listed. Both work now.
- **Console browser:** the eject button of USB drives, the clear button of search fields, and the close buttons of toasts and of the support panel stay in their corner instead of dropping into the layout.
- **Interface:** the sidebar keeps the current page in view on short screens, the Disabled tag on payload cards is no longer cut off, and the Home shortcut titles fit on one line.
- **Dialogs:** the default Cancel, Confirm, Save and Working… labels of confirmation and input dialogs follow the interface language.
- **Media delete:** a capture is now removed from the console's gallery database (`av_content.db`, backed up first, last 5 copies kept in `media_db_backups`) and then its image or clip, `.meta`, `.dat` and thumbnail are removed. Before, only the image was removed and the console kept listing the capture. If the database is unavailable or cannot be backed up, nothing is deleted.
- **Payloads:** the whole card launches the payload, not only its upper part.
- **Phone:** payload names in Autoloader steps are no longer truncated (step buttons moved to a second line); the Media viewer fills the screen, with icon-only Download and Delete; the Save Backups description is no longer squeezed into a narrow column.
- **Phone:** the More sheet opens again after selecting a page from it (its closing state was kept across openings).
- **Phone:** the Autoloader page no longer scrolls sideways (grid column width), the Files and Repositories buttons fit the screen, the Home Firmware / IP block no longer breaks, the install-PKG chooser stacks, and the header hides the version pill so titles are not cut.
- **Settings:** removed a stray line above the Danger zone; Technical details now sits in its own card.
- **Console browser:** removed a stray horizontal scrollbar on the file list.
- **Fewer on-screen toasts during the Autoloader:** while the Autoloader page is open, Prospero skips its "Auto-Loading (N) Payloads" and "Loaded all" toasts, since the page already shows them. The countdown notice with the PS button and the abort notice always show. Toasts from other payloads are theirs and stay.
- **The support panel is readable on the console:** it is wider, the text is larger, and it scrolls when the screen is short, so the whole supporters list can be reached.
- **The console's D-pad moves the browser cursor again.** Prospero no longer takes the arrow keys for its own highlight in Files, menus, the language picker, Settings tabs and the command palette, and clicked controls no longer keep a blue ring behind.
- **Canceling an upload no longer blocks the folder.** A request from the canceled upload that arrives late is refused, so deleting or moving the folder works right away.
- **Uploading many small files is faster:** files under 32 MB are written straight to their destination instead of through a staging file and a rename, uploads reuse one connection instead of opening a new one per file, and the log file is no longer reopened for every line.
- **Bookmarking the page on a computer now keeps its icon.** The icon files can be cached by the browser, and `/favicon.ico` now answers with the Prospero icon.
- **Controller navigation on the PS5 browser.** Checkboxes and radio buttons now show where the controller is, selects and cards highlight on focus, and a style rule that the console's browser dropped is split so it applies. Small icon buttons get a larger invisible target for the cursor.
- **Older PS5 browsers:** plain buttons keep their transparent look even where the browser cannot read the new CSS selector, and notification times no longer fail where relative time formatting is missing.
- **On-screen keyboard** no longer capitalizes or auto-corrects paths, names and links.
- **Hungarian flag** now uses the official red and green.
- **Extracting several ZIP files from the same folder** no longer fails with "that path overlaps a queued file operation". They are queued and extracted one after another.
- **Prospero listed itself** as a running payload in the Payloads tab. It no longer does.
- **Emoji and symbols on the PS5 browser** (the heart, star and flag in the texts, plus the check, cross and retry marks) are now drawn as small images instead of text characters, which showed as empty boxes.
- **Payload updates** no longer offer an older release as an update (for example v1.13.1 over an installed v1.15-beta1). Only a newer version is suggested.
- **Store packages (PS5 retail) were not recognised** in the package review, so they had no name, type or cover art. The LIH and FIH package formats are now read, in the browser and on the console. Base, update and DLC are told apart the same way the console's own package manager does, and the cover is found anywhere in the file.
- **Install refused with 0x80b2116f** (or 0x80b2100d / 0x80b2100e) is now retried twice (after 2 s and 5 s) with a fresh service and stream address, without cover art on the retries. The message also explains both possible causes.
- **"Install queued" notification** shows the game's cover art, read from the package.
- **Cover art on the console's install card** stays available after the install is queued, which is when the console fetches it.
- **"Connection was interrupted"** no longer shows after a live package upload that was fully delivered. The install is reported as started instead.
- **"An upload is already in progress"** no longer blocks the next package after an install. A previous upload the console has already read in full is released when a new one starts.
- **Updating a payload replaces the old file** instead of saving it as "name (1).elf", so it stays in the Autoloader.
- **A scheduled shutdown no longer cuts off a package install.** "When jobs finish" now also waits until the console reports the install as finished or failed, including installs from a file, a link or a live upload.
- **Account IDs were shown and typed in reverse byte order**, so an ID copied from Garlic SaveMgr or a save was written backwards and the save was rejected. IDs now use the same byte order as Garlic SaveMgr and must have 16 hex digits. Existing accounts and account backups keep working; the same ID now just reads differently.
- **Save backup file names** used UTC time, so they showed the wrong hour. They now use the console's local time, and old backups are pruned by date instead of by name.
- **Automatic save backup** now shows a console notification when it starts and when it ends, saying which drives got the copy and why any other failed (for example, a USB drive that was unplugged).
- **A very large PKG upload could stall for good** when the console asked again for a part the console had already refused.
- **Installing a game the console already has** now explains the refusal and what to do, instead of a bare error code.
- **Restoring a save backup** from a user that is not on the console (after a factory reset, for example) now says so instead of reporting success.
- **Dropping files on the file manager** no longer also hands them to the PKG page, which showed an error for files that are not packages.
- **Storage tiles on Home** open the file manager at that drive instead of the last folder you used.
- **Dropped folders** keep their structure when uploaded.
- **Folder and multi-file uploads** show one overall progress bar instead of one per file.
- **A package the console rejected showed "Connection interrupted"** instead of the console's own reason.
- **The PKG page threw "can't access lexical declaration"** when the upload connection had already closed, which cancelled the install.
- **Faster folder uploads:** folders are created in one request, and small files are flushed to disk once at the end instead of one by one.
- **A DLC or small package rejected as a stream** can now be installed from USB or storage.

<details>
<summary><b>v1.1</b></summary>

### Fixed
- **The web UI could load to a blank white screen.** The PS5's browser doesn't support a CSS feature Tailwind v4 relied on for all its styling.
- **The web UI failed to load, or rendered with broken spacing, on 4.xx firmware.** Its older browser engine choked on a newer JavaScript syntax and lacked flexbox spacing support.
- **Language flags in the language picker showed as blank boxes** instead of an actual flag — replaced with bundled SVG icons.
- **A saved GitHub token that expired or was revoked broke every github.com repository check.** It now falls back to an anonymous request instead of failing outright.
- **Deleting the temp folder from Files broke the next payload download or Home tile install** until a restart.
- **Restoring a PS5 save backup failed with "missing its sealed key".** Only PS4 saves have a key, so PS5 backups are no longer checked for one.
- **A folder the console refuses to open** now says so, instead of a bare "Permission denied".
- **Restoring a PS4 save backup gave a "corrupted save data" error.** The backup was missing the sealed key.
- **An exported save could not be imported back.**
- **Backing up, deleting and restoring a save took tens of seconds.**
- **Exporting a large save could run the console out of memory.**
- **Save errors showed in English in every language.**
- **PKG installs failed with `0x80b2116f`** once Prospero's tile was installed.
- **A rejected package blocked every later install until the console rebooted.**
- **PKG uploads from a desktop browser were slowed by the page itself.** Progress now updates a few times per second instead of on every segment, and files are read with the faster browser call.
- **PKG installs read the package one connection at a time.** The installer's parallel reads now run side by side, so one slow read no longer stalls the others.
- **Large PKG uploads sent many segments twice,** which halved the speed. A segment the console is already receiving is no longer sent again.
- **Large PKG uploads wasted bandwidth on resends.** The upload now follows the installer's real position, so it no longer floods a full cache.
- **Extracting an archive over an existing copy failed only at the end,** after extracting everything and deleting it again. It now stops right away with the name of the clashing file.
- **Large PKG installs now finish cleanly** instead of waiting in the log after the game is installed.
- **Uploading a folder with subfolders failed with "another file operation is in progress".**
- **Large uploads from a phone froze** at around 65 MB and locked file operations. A stalled chunk now retries by itself, and a failed upload cleans up after itself.
- **Uploads could not be canceled.** The transfer bar now has a cancel button that stops the upload and removes its partial file.
- **"That path overlaps an active upload"** blocked deleting files for 30 minutes after an interrupted upload. It now clears after 2 minutes, or right away on cancel.
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

</details>

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
