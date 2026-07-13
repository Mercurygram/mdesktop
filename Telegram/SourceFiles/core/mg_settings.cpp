/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mg_settings.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <rpl/event_stream.h>

namespace MG {

// Each toggle is a plain bool persisted through the upstream typed pref store
// (Core::Settings::readPref / writePref), with a dedicated event_stream so the
// UI can react to changes live. Defaults are false (opt-in fork features).
#define MG_BOOL_SETTING(Name, Key) \
namespace { \
[[nodiscard]] rpl::event_stream<bool> &Name##Stream() { \
	static auto result = rpl::event_stream<bool>(); \
	return result; \
} \
} /* namespace */ \
bool Name() { \
	return Core::App().settings().readPref<bool>(Key, false); \
} \
void Set##Name(bool value) { \
	Core::App().settings().writePref<bool>(Key, value); \
	Core::App().saveSettingsDelayed(); \
	Name##Stream().fire_copy(value); \
} \
rpl::producer<bool> Name##Value() { \
	return Name##Stream().events_starting_with(Name()); \
}

MG_BOOL_SETTING(ShowPeerId, "mg-show-peer-id")
MG_BOOL_SETTING(HideStories, "mg-hide-stories")
MG_BOOL_SETTING(DeleteForAllDefault, "mg-delete-for-all-default")
MG_BOOL_SETTING(LargePhotos, "mg-large-photos")
MG_BOOL_SETTING(MessageDetails, "mg-message-details")
MG_BOOL_SETTING(HideAllChats, "mg-hide-all-chats")
MG_BOOL_SETTING(DisableGlobalSearch, "mg-disable-global-search")
MG_BOOL_SETTING(DisableLinkPreviews, "mg-disable-link-previews")
MG_BOOL_SETTING(DisableAiEditor, "mg-disable-ai-editor")
MG_BOOL_SETTING(DisableAiSummaries, "mg-disable-ai-summaries")
MG_BOOL_SETTING(OpenLinksInBrowser, "mg-open-links-in-browser")
MG_BOOL_SETTING(HidePremiumPromo, "mg-hide-premium-promo")
MG_BOOL_SETTING(AllRecentStickers, "mg-all-recent-stickers")

#undef MG_BOOL_SETTING

} // namespace MG
