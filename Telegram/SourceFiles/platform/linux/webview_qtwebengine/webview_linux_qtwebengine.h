// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "webview/platform/linux/webview_linux.h"
#include "base/basic_types.h"

#include <QtCore/QObject>
#include <QtCore/QString>

namespace Webview::QtWebEngine {

// WHY: `window.external.invoke` over QWebChannel needs moc, and AUTOMOC is
// enabled on lib_webview only for this backend, so the Q_OBJECT lives in a header.
class Bridge final : public QObject {
	Q_OBJECT
public:
	Bridge(QObject *parent, Fn<void(const QString&)> handler);

public Q_SLOTS:
	void invoke(const QString &message);

private:
	Fn<void(const QString&)> _handler;

};

[[nodiscard]] Available Availability();
[[nodiscard]] std::unique_ptr<Interface> CreateInstance(Config config);
[[nodiscard]] bool SeparateStorageIdSupported();

} // namespace Webview::QtWebEngine
