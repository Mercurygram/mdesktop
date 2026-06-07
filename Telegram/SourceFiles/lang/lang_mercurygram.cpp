/*
This file is part of Mercurygram Desktop,
a privacy and security focused fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/lang_mercurygram.h"

#include <QtCore/QFile>

namespace Lang {
namespace {

[[nodiscard]] QByteArray ReadOverlay(const QString &id) {
	if (id.isEmpty() || id.startsWith('#')) {
		// Empty or pseudo-locale (e.g. custom packs, "#TEST_X").
		return QByteArray();
	}
	auto file = QFile(u":/langs/mercurygram/%1.strings"_q.arg(id.toLower()));
	if (!file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	return file.readAll();
}

} // namespace

QByteArray MercurygramOverlay(const QString &id, const QString &baseId) {
	auto result = ReadOverlay(id);
	if (result.isEmpty() && baseId != id) {
		result = ReadOverlay(baseId);
	}
	return result;
}

} // namespace Lang
