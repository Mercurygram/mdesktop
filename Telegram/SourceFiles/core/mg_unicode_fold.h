/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Included rather than left to stdafx.h: the unit test builds this pair alone,
// without the application's precompiled header.
#include <QtCore/QString>

namespace MG {

// The plain ASCII reading of a name written with a decorated Unicode "font"
// ("𝐌𝐞𝐫𝐜𝐮𝐫𝐲", "Ｍｅｒｃｕｒｙ", "ⓜⓔⓡⓒⓤⓡⓨ", "ŋơ۷ɛƖ"), or an empty string when
// there was nothing to fold.
//
// The name indexing appends it next to the transliteration variants it already
// builds, so a plain query finds the chat while the name as it was written
// keeps matching itself. The result is meant for matching, never for display.
[[nodiscard]] QString FoldDecorated(const QString &text);

} // namespace MG
