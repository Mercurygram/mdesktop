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

[[nodiscard]] bool LargePhotos();
void SetLargePhotos(bool value);
[[nodiscard]] rpl::producer<bool> LargePhotosValue();

[[nodiscard]] bool MessageDetails();
void SetMessageDetails(bool value);
[[nodiscard]] rpl::producer<bool> MessageDetailsValue();

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

} // namespace MG
