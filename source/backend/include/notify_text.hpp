#pragma once
// Texts of the native console notifications, in the console's system language.

#include <initializer_list>
#include <string>
#include <utility>

namespace prospero {

// Order matches the rows of notify_catalog.inc.
enum class Msg {
    kPayloadsAlreadyLoaded,
    kAutoloadCountdown,
    kAutoloadAborted,
    kConflictKstuff,
    kConflictEtahen,
    kAutoLoadingSkipping,
    kAutoLoading,
    kLoadedAll,
    kListening,
    kStartup,
    kSignedIn,
    kSaveImported,
    kSaveRestored,
    kSavesBackingUp,
    kSavesBackedUp,
    kSaveBackupFailed,
    kSavesBackedUpTo,
    kUpdating,
    kInstallQueued,
    kInstalling,
    kPkgInstalled,
    kReasonDisconnected,
    kReasonReadOnly,
    kReasonBusy,
    kReasonNoSpace,
    kReasonWriteFailed,
    kReasonVerifyFailed,
    kReasonNoSaves,
    kReasonNotUsb,
    kDriveExtUsb,
    kDriveInternal,
    kOpenTitle,
    kCount,
};

using TextArgs = std::initializer_list<std::pair<const char *, std::string>>;

// The text in the system language, with each {name} replaced by its value.
std::string NotifyText(Msg msg, TextArgs args = {});

} // namespace prospero
