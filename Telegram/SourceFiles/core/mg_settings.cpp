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

// Each setting is a scalar persisted through the upstream typed pref store
// (Core::Settings::readPref / writePref), with a dedicated event_stream so the
// UI can react to changes live. Toggles default to false (opt-in fork
// features).
#define MG_SETTING(Type, Name, Key, Default) \
namespace { \
[[nodiscard]] rpl::event_stream<Type> &Name##Stream() { \
	static auto result = rpl::event_stream<Type>(); \
	return result; \
} \
} /* namespace */ \
Type Name() { \
	return Core::App().settings().readPref<Type>(Key, Default); \
} \
void Set##Name(Type value) { \
	Core::App().settings().writePref<Type>(Key, value); \
	Core::App().saveSettingsDelayed(); \
	Name##Stream().fire_copy(value); \
} \
rpl::producer<Type> Name##Value() { \
	return Name##Stream().events_starting_with(Name()); \
}

#define MG_BOOL_SETTING(Name, Key) MG_SETTING(bool, Name, Key, false)

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

// A FilterId; 0 means "open the account's default folder" (upstream).
MG_SETTING(int, LaunchFolder, "mg-launch-folder", 0)

#undef MG_BOOL_SETTING
#undef MG_SETTING

} // namespace MG
