#pragma once
// Native PS5 toasts. Notify() carries ProsperoMgr's own icon (sceNotificationSend rich
// toast, see notify.cpp) and falls back to plain text if that call fails.

#include <string>

namespace prospero {

void Notify(const std::string &message);

// Same as Notify(), with a PNG (a game's cover art) as the toast icon instead of our own.
void NotifyWithImage(const std::string &message, const std::string &png);

// A toast with a button that opens a system address (for example pssettings:play?function=debug_settings)
// when it is selected on the console.
void NotifyWithAction(const std::string &message, const std::string &action_name, const std::string &action_url);

// Plain text-only toast (sceKernelSendNotificationRequest) — no icon. Use for routine
// technical status lines that don't need the branded rich toast.
void NotifyDebug(const std::string &message);

} // namespace prospero
