/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

namespace MG {

[[nodiscard]] bool ShowPeerId();
void SetShowPeerId(bool value);
[[nodiscard]] rpl::producer<bool> ShowPeerIdValue();

[[nodiscard]] bool HideStories();
void SetHideStories(bool value);
[[nodiscard]] rpl::producer<bool> HideStoriesValue();

[[nodiscard]] bool DeleteForAllDefault();
void SetDeleteForAllDefault(bool value);
[[nodiscard]] rpl::producer<bool> DeleteForAllDefaultValue();

[[nodiscard]] bool MessageDetails();
void SetMessageDetails(bool value);
[[nodiscard]] rpl::producer<bool> MessageDetailsValue();

[[nodiscard]] bool ShowCharCounter();
void SetShowCharCounter(bool value);
[[nodiscard]] rpl::producer<bool> ShowCharCounterValue();

[[nodiscard]] bool HideAllChats();
void SetHideAllChats(bool value);
[[nodiscard]] rpl::producer<bool> HideAllChatsValue();

[[nodiscard]] bool DisableGlobalSearch();
void SetDisableGlobalSearch(bool value);
[[nodiscard]] rpl::producer<bool> DisableGlobalSearchValue();

[[nodiscard]] bool DisableLinkPreviews();
void SetDisableLinkPreviews(bool value);
[[nodiscard]] rpl::producer<bool> DisableLinkPreviewsValue();

[[nodiscard]] bool DisableAiEditor();
void SetDisableAiEditor(bool value);
[[nodiscard]] rpl::producer<bool> DisableAiEditorValue();

[[nodiscard]] bool DisableAiSummaries();
void SetDisableAiSummaries(bool value);
[[nodiscard]] rpl::producer<bool> DisableAiSummariesValue();

[[nodiscard]] bool OpenLinksInBrowser();
void SetOpenLinksInBrowser(bool value);
[[nodiscard]] rpl::producer<bool> OpenLinksInBrowserValue();

[[nodiscard]] bool ReduceTracking();
void SetReduceTracking(bool value);
[[nodiscard]] rpl::producer<bool> ReduceTrackingValue();

// The lifetime to ask for when creating a temporary (perfect forward secrecy)
// key. One hour while "Reduce network tracking" is on, the upstream 24 hours
// otherwise, and the steps in between once the server has refused the shorter
// one.
[[nodiscard]] TimeId TemporaryKeyExpiresIn();
[[nodiscard]] rpl::producer<TimeId> TemporaryKeyExpiresInValue();

// Moves one step up the lifetime ladder after the server rejected a bind, and
// reports whether there was a step left. False means the reduced lifetime is
// not what the server is objecting to, so the caller must run its own
// failure handling.
bool StepUpTemporaryKeyExpiresIn();

// Puts the ladder back on its shortest step, for when the option is switched
// on again after the server pushed it up.
void ResetTemporaryKeyLadder();

[[nodiscard]] bool StripTracking();
void SetStripTracking(bool value);
[[nodiscard]] rpl::producer<bool> StripTrackingValue();

[[nodiscard]] bool ConfirmInternalLinks();
void SetConfirmInternalLinks(bool value);
[[nodiscard]] rpl::producer<bool> ConfirmInternalLinksValue();

[[nodiscard]] bool KeepDraftsLocal();
void SetKeepDraftsLocal(bool value);
[[nodiscard]] rpl::producer<bool> KeepDraftsLocalValue();

[[nodiscard]] bool HidePremiumPromo();
void SetHidePremiumPromo(bool value);
[[nodiscard]] rpl::producer<bool> HidePremiumPromoValue();

[[nodiscard]] bool LockOnHide();
void SetLockOnHide(bool value);
[[nodiscard]] rpl::producer<bool> LockOnHideValue();

[[nodiscard]] bool AllRecentStickers();
void SetAllRecentStickers(bool value);
[[nodiscard]] rpl::producer<bool> AllRecentStickersValue();

// FilterId of the folder to open on launch; 0 = the account's default folder.
[[nodiscard]] int LaunchFolder();
void SetLaunchFolder(int value);
[[nodiscard]] rpl::producer<int> LaunchFolderValue();

} // namespace MG
