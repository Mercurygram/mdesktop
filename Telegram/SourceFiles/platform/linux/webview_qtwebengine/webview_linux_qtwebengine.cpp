// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview_linux_qtwebengine.h"

#include "webview/platform/linux/webview_linux_http_server.h"
#include "webview/webview_data_stream.h"
#include "base/assertion.h"
#include "base/debug_log.h"
#include "base/flat_map.h"
#include "base/random.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"

#include <crl/crl.h>
#include <rpl/variable.h>

#include <array>
#include <format>
#include <set>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QLibraryInfo>
#include <QtCore/QMutex>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtGui/QDesktopServices>
#include <QtNetwork/QAuthenticator>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>
#include <QtWidgets/QWidget>

#include <QtWebChannel/QWebChannel>
#include <QtWebEngineCore/QWebEngineCookieStore>
#include <QtWebEngineCore/QWebEngineDownloadRequest>
#include <QtWebEngineCore/QWebEngineHistory>
#include <QtWebEngineCore/QWebEngineNewWindowRequest>
#include <QtWebEngineCore/QWebEnginePage>
#include <QtWebEngineCore/QWebEnginePermission>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWebEngineCore/QWebEngineScript>
#include <QtWebEngineCore/QWebEngineScriptCollection>
#include <QtWebEngineCore/QWebEngineSettings>
#include <QtWebEngineCore/QWebEngineUrlRequestInterceptor>
#include <QtWebEngineWidgets/QWebEngineView>

namespace Webview::QtWebEngine {
namespace {

constexpr auto kDataHost = "127.0.0.1";
constexpr auto kMaxScriptMessageBytes = 1024 * 1024;

// MainWorld, so page scripts calling window.external.invoke see the bridge.
constexpr auto kWorldId = quint32(QWebEngineScript::MainWorld);

[[nodiscard]] QString QWebChannelScript() {
	// Process-constant, read once for every Instance.
	static const QString kScript = [] {
		const auto read = [](const QString &path) {
			auto file = QFile(path);
			return file.open(QIODevice::ReadOnly)
				? QString::fromUtf8(file.readAll())
				: QString();
		};
		// Qt resource from QtWebChannel, else the on-disk copy shipped with Qt.
		if (auto result = read(u":/qtwebchannel/qwebchannel.js"_q)
				; !result.isEmpty()) {
			return result;
		}
		const auto onDisk = QLibraryInfo::path(QLibraryInfo::DataPath)
			+ u"/webchannel/qwebchannel.js"_q;
		if (auto result = read(onDisk); !result.isEmpty()) {
			return result;
		}
		LOG(("WebView Error: Could not load qwebchannel.js."));
		return QString();
	}();
	return kScript;
}

[[nodiscard]] std::string GenerateMessageToken() {
	auto bytes = std::array<std::uint8_t, 32>();
	::base::RandomFill(bytes.data(), bytes.size());
	constexpr auto kHex = "0123456789abcdef";
	auto result = std::string();
	result.reserve(bytes.size() * 2);
	for (const auto byte : bytes) {
		result.push_back(kHex[byte >> 4]);
		result.push_back(kHex[byte & 0x0F]);
	}
	return result;
}

// WHY: qt.webChannelTransport is reachable from child frames too,
// so every message carries a token only this top frame script knows.
[[nodiscard]] QString ExternalBridgeScript(const std::string &token) {
	return uR"(
(function() {
	if (window !== window.top) {
		return;
	}
	const messageToken = ')"_q + QString::fromStdString(token) + uR"(';
	let bridge = null;
	const queue = [];
	Object.defineProperty(window, 'external', {
		value: Object.freeze({
			invoke: function(s) {
				const message = messageToken + String(s);
				if (bridge) {
					bridge.invoke(message);
				} else {
					queue.push(message);
				}
			}
		}),
		configurable: false,
		writable: false
	});
	new QWebChannel(qt.webChannelTransport, function(channel) {
		bridge = channel.objects.external;
		while (queue.length) {
			bridge.invoke(queue.shift());
		}
	});
})();
)"_q;
}

// WHY: restricted content must be plain HTML, SVG or XHTML could run
// scripts before the Content-Security-Policy meta is in place.
[[nodiscard]] QString RestrictedContentTypeScript() {
	return uR"(
if (document.contentType !== 'text/html') {
	window.stop();
	document.documentElement && document.documentElement.remove();
}
)"_q;
}

[[nodiscard]] bool AllowedRestrictedUrl(
		const QUrl &url,
		const QUrl &expected) {
	return url.isValid()
		&& expected.isValid()
		&& expected.scheme() == u"https"_q
		&& expected.userInfo().isEmpty()
		&& expected.port(443) == 443
		&& (url.scheme() == u"https"_q || url.scheme() == u"wss"_q)
		&& url.host() == expected.host()
		&& url.port(443) == 443
		&& url.userInfo().isEmpty();
}

class RestrictedInterceptor final : public QWebEngineUrlRequestInterceptor {
public:
	RestrictedInterceptor(QObject *parent, QUrl origin)
	: QWebEngineUrlRequestInterceptor(parent)
	, _origin(std::move(origin)) {
	}

	void interceptRequest(QWebEngineUrlRequestInfo &info) override {
		if (!AllowedRestrictedUrl(info.requestUrl(), _origin)) {
			info.block(true);
		}
	}

private:
	const QUrl _origin;

};

// WHY: two profiles on one storage path fight over it, so share one.
// Third party cookies only below top pages of instances that asked.
struct SharedProfile {
	QWebEngineProfile *profile = nullptr;
	std::shared_ptr<QMutex> mutex;
	std::shared_ptr<std::multiset<QString>> thirdPartyHosts;
};

[[nodiscard]] SharedProfile LookupProfile(
		const QString &storageName,
		const QString &path) {
	static auto cache = ::base::flat_map<QString, SharedProfile>();
	auto &result = cache[path + '\n' + storageName];
	if (result.profile) {
		return result;
	}
	// A named profile is persistent (an unnamed one is off-the-record).
	result.profile = new QWebEngineProfile(
		storageName,
		QCoreApplication::instance());
	if (!path.isEmpty()) {
		result.profile->setPersistentStoragePath(path);
		result.profile->setCachePath(path + u"/cache"_q);
	}
	result.profile->setPersistentCookiesPolicy(
		QWebEngineProfile::ForcePersistentCookies);
	result.mutex = std::make_shared<QMutex>();
	result.thirdPartyHosts = std::make_shared<std::multiset<QString>>();
	result.profile->cookieStore()->setCookieFilter([
		mutex = result.mutex,
		hosts = result.thirdPartyHosts
	](const QWebEngineCookieStore::FilterRequest &request) {
		if (!request.thirdParty) {
			return true;
		}
		const auto lock = QMutexLocker(mutex.get());
		return hosts->contains(request.firstPartyUrl.host());
	});
	return result;
}

// WHY: input goes to QtWebEngine's internal child widgets, so filter the app
// and match the view's top-level window. No RTTI casts here: QtWebEngine ships
// QtQuick types built -fno-rtti, a dynamic_cast on them would crash.
class InteractionFilter final : public QObject {
public:
	InteractionFilter(
		QObject *parent,
		QPointer<QWidget> view,
		Fn<void()> onInteraction)
	: QObject(parent)
	, _view(view)
	, _onInteraction(std::move(onInteraction)) {
		if (const auto app = QCoreApplication::instance()) {
			app->installEventFilter(this);
		}
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		const auto type = event->type();
		if ((type == QEvent::MouseButtonPress || type == QEvent::KeyPress)
			&& _view
			&& watched->isWidgetType()
			&& static_cast<QWidget*>(watched)->window() == _view->window()
			&& _onInteraction) {
			_onInteraction();
		}
		return false;
	}

private:
	QPointer<QWidget> _view;
	Fn<void()> _onInteraction;

};

class Page;

class Instance final : public Interface, public ::base::has_weak_ptr {
public:
	Instance();
	~Instance();

	[[nodiscard]] bool create(Config config);

	void navigate(std::string url) override;
	void navigateToData(std::string id) override;
	void loadHtml(std::string html, std::string baseUrl) override;
	void reload() override;

	void init(std::string js) override;
	void initAllFrames(std::string js) override;
	void eval(std::string js) override;

	void focus() override;
	void setInteractionHandler(Fn<void()> handler) override;

	void setOpaqueBg(QColor opaqueBg) override;
	void resize(int width, int height) override;
	void setFullscreen(bool fullscreen) override;

	QWidget *widget() override;

	void refreshNavigationHistoryState() override;
	auto navigationHistoryState()
		-> rpl::producer<NavigationHistoryState> override;

	ZoomController *zoomController() override;

	[[nodiscard]] bool acceptNavigation(const QUrl &url, bool isMainFrame);
	[[nodiscard]] bool restricted() const;

private:
	class Zoom final : public ZoomController {
	public:
		explicit Zoom(QPointer<QWebEngineView> view) : _view(view) {
		}
		rpl::producer<int> zoomValue() override {
			return _value.value();
		}
		void setZoom(int value) override {
			if (_view) {
				_view->setZoomFactor(value / 100.);
			}
			_value = value;
		}
	private:
		QPointer<QWebEngineView> _view;
		rpl::variable<int> _value = 100;
	};

	void addScript(const QString &source, bool allFrames = false);
	void updateHistoryStates();
	void updateThirdPartyHost();
	void loadFinished(bool ok);
	void messageReceived(const QString &message);
	[[nodiscard]] std::string dataDomain() const;
	bool startDataServer();
	void dataRequest(
		DataResponse resolved,
		QTcpSocket *socket,
		const std::string &resourceId,
		std::int64_t requestedOffset,
		std::int64_t requestedLimit,
		bool headersWritten,
		const std::shared_ptr<HttpServer::Guard> &guard);

	std::unique_ptr<QWebEngineProfile> _restrictedProfile;
	SharedProfile _shared;
	QString _thirdPartyHost;
	base::unique_qptr<QWidget> _widget;
	QPointer<QWebEngineView> _view;
	QPointer<Page> _page;
	QPointer<QWebChannel> _channel;
	QPointer<Bridge> _bridge;

	std::optional<Zoom> _zoom;

	std::optional<HttpServer> _dataServer;
	std::string _dataPassword;
	std::string _dataRequestRedirectHost;
	quint16 _dataPort = 0;

	Fn<void(Message)> _messageHandler;
	Fn<bool(std::string, bool)> _navigationStartHandler;
	Fn<void(bool)> _navigationDoneHandler;
	Fn<DataResult(DataRequest)> _dataRequestHandler;
	Fn<void()> _externalWindowCloseHandler;
	Fn<void()> _interactionHandler;

	const std::string _messageToken = GenerateMessageToken();
	QUrl _htmlBaseUrl;
	QUrl _restrictedOrigin;

	rpl::variable<NavigationHistoryState> _navigationHistoryState;
	WindowMode _mode = WindowMode::Embedded;
	bool _allowThirdPartyCookies = false;
	bool _debug = false;

};

class Page final : public QWebEnginePage {
public:
	Page(
		QWebEngineProfile *profile,
		QObject *parent,
		Fn<DialogResult(DialogArgs)> dialog,
		not_null<Instance*> instance)
	: QWebEnginePage(profile, parent)
	, _dialog(std::move(dialog))
	, _instance(instance) {
	}

protected:
	bool acceptNavigationRequest(
			const QUrl &url,
			NavigationType type,
			bool isMainFrame) override {
		return _instance->acceptNavigation(url, isMainFrame);
	}

	QStringList chooseFiles(
			FileSelectionMode mode,
			const QStringList &oldFiles,
			const QStringList &acceptedMimeTypes) override {
		return _instance->restricted()
			? QStringList()
			: QWebEnginePage::chooseFiles(mode, oldFiles, acceptedMimeTypes);
	}

	void javaScriptAlert(
			const QUrl &securityOrigin,
			const QString &msg) override {
		if (_dialog) {
			_dialog(DialogArgs{
				.type = DialogType::Alert,
				.text = msg.toStdString(),
				.url = securityOrigin.toString().toStdString(),
			});
		}
	}

	bool javaScriptConfirm(
			const QUrl &securityOrigin,
			const QString &msg) override {
		if (!_dialog) {
			return false;
		}
		return _dialog(DialogArgs{
			.type = DialogType::Confirm,
			.text = msg.toStdString(),
			.url = securityOrigin.toString().toStdString(),
		}).accepted;
	}

	bool javaScriptPrompt(
			const QUrl &securityOrigin,
			const QString &msg,
			const QString &defaultValue,
			QString *result) override {
		if (!_dialog) {
			return false;
		}
		const auto resolved = _dialog(DialogArgs{
			.type = DialogType::Prompt,
			.value = defaultValue.toStdString(),
			.text = msg.toStdString(),
			.url = securityOrigin.toString().toStdString(),
		});
		if (resolved.accepted && result) {
			*result = QString::fromStdString(resolved.text);
		}
		return resolved.accepted;
	}

private:
	Fn<DialogResult(DialogArgs)> _dialog;
	not_null<Instance*> _instance;

};

Instance::Instance() = default;

Instance::~Instance() {
	base::take(_widget);
	// Without a view (WindowMode::Hidden) nothing else owns the page.
	delete _page.data();
	if (!_thirdPartyHost.isEmpty()) {
		const auto lock = QMutexLocker(_shared.mutex.get());
		_shared.thirdPartyHosts->erase(
			_shared.thirdPartyHosts->find(_thirdPartyHost));
	}
}

bool Instance::create(Config config) {
	if (!config.restrictedOrigin.empty()
		&& config.restrictedContentSecurityPolicy.empty()) {
		return false;
	}
	_restrictedOrigin = config.restrictedOrigin.empty()
		? QUrl()
		: QUrl(QString::fromStdString(config.restrictedOrigin));
	_mode = config.mode;
	_debug = config.debug && !restricted();
	_allowThirdPartyCookies = config.allowThirdPartyCookies && !restricted();
	_messageHandler = std::move(config.messageHandler);
	_navigationStartHandler = std::move(config.navigationStartHandler);
	_navigationDoneHandler = std::move(config.navigationDoneHandler);
	_dataRequestHandler = std::move(config.dataRequestHandler);
	_dataRequestRedirectHost = std::move(config.dataRequestRedirectHost);
	_externalWindowCloseHandler = std::move(config.externalWindowCloseHandler);

	auto profile = (QWebEngineProfile*)nullptr;
	if (restricted()) {
		// Off-the-record, no cookies, only the restricted origin reachable.
		_restrictedProfile = std::make_unique<QWebEngineProfile>();
		profile = _restrictedProfile.get();
		profile->cookieStore()->setCookieFilter([](const auto &) {
			return false;
		});
		profile->setUrlRequestInterceptor(
			new RestrictedInterceptor(profile, _restrictedOrigin));
		QObject::connect(
			profile,
			&QWebEngineProfile::downloadRequested,
			profile,
			[](QWebEngineDownloadRequest *download) { download->cancel(); });
	} else {
		// Per-token storage when isolated, a fixed shared one for legacy.
		const auto token = config.userDataToken;
		const auto isolated = !token.empty()
			&& token != LegacyStorageIdToken().toStdString();
		const auto storageName = isolated
			? QString::fromUtf8(
				QByteArray::fromRawData(token.data(), token.size()).toHex())
			: u"webview"_q;
		const auto path = config.userDataPath.empty()
			? QString()
			: (QString::fromStdString(config.userDataPath)
				+ (isolated ? ('/' + storageName) : QString()));
		_shared = LookupProfile(storageName, path);
		profile = _shared.profile;
	}

	if (_mode == WindowMode::Hidden) {
		_page = new Page(profile, nullptr, nullptr, this);
		// WHY: a view-less page is a throttled background tab; the
		// visibility sticks only once a load has started.
		_page->connect(_page, &QWebEnginePage::loadStarted, _page, [=] {
			_page->setVisible(true);
		});
	} else {
		const auto parent = (_mode == WindowMode::External)
			? nullptr
			: config.parent;
		_view = new QWebEngineView(parent);
		_page = new Page(
			profile,
			_view.data(),
			restricted() ? nullptr : std::move(config.dialogHandler),
			this);
		_view->setPage(_page);
		_view->setContextMenuPolicy(_debug
			? Qt::DefaultContextMenu
			: Qt::PreventContextMenu);
		_widget.reset(_view.data());
	}

	const auto settings = _page->settings();
	if (restricted()) {
		using Attribute = QWebEngineSettings::WebAttribute;
		for (const auto attribute : {
			Attribute::AutoLoadImages,
			Attribute::DnsPrefetchEnabled,
			Attribute::FullScreenSupportEnabled,
			Attribute::LocalStorageEnabled,
			Attribute::HyperlinkAuditingEnabled,
			Attribute::WebGLEnabled,
			Attribute::Accelerated2dCanvasEnabled,
			Attribute::JavascriptCanAccessClipboard,
			Attribute::JavascriptCanPaste,
			Attribute::JavascriptCanOpenWindows,
			Attribute::ScreenCaptureEnabled,
			Attribute::PluginsEnabled,
			Attribute::PdfViewerEnabled,
			Attribute::BackForwardCacheEnabled,
		}) {
			settings->setAttribute(attribute, false);
		}
		settings->setAttribute(Attribute::PlaybackRequiresUserGesture, true);
		settings->setAttribute(Attribute::WebRTCPublicInterfacesOnly, true);
		_page->setAudioMuted(true);
	} else {
		// Clipboard writes for "Copy" buttons; reads stay denied below.
		settings->setAttribute(
			QWebEngineSettings::JavascriptCanAccessClipboard,
			true);
		settings->setAttribute(QWebEngineSettings::JavascriptCanPaste, false);
		// Autoplay IV videos and GIFs like WebKitGTK does.
		settings->setAttribute(
			QWebEngineSettings::PlaybackRequiresUserGesture,
			false);
		if (_debug) {
			settings->setAttribute(
				QWebEngineSettings::JavascriptCanOpenWindows,
				true);
		}
	}

	_channel = new QWebChannel(_page.data());
	_bridge = new Bridge(_channel.data(), [
		weak = base::make_weak(this)
	](const QString &message) {
		if (const auto strong = weak.get()) {
			strong->messageReceived(message);
		}
	});
	_channel->registerObject(u"external"_q, _bridge.data());
	_page->setWebChannel(_channel.data(), kWorldId);

	_page->connect(_page, &QWebEnginePage::urlChanged, _page, [=] {
		updateHistoryStates();
		updateThirdPartyHost();
	});
	_page->connect(_page, &QWebEnginePage::titleChanged, _page, [=] {
		updateHistoryStates();
	});
	_page->connect(_page, &QWebEnginePage::loadFinished, _page, [=](bool ok) {
		loadFinished(ok);
	});
	_page->connect(
		_page,
		&QWebEnginePage::authenticationRequired,
		_page,
		[=](const QUrl &url, QAuthenticator *authenticator) {
			if (!restricted()
					&& _dataPort
					&& url.host() == QString::fromUtf8(kDataHost)
					&& url.port() == int(_dataPort)) {
				authenticator->setUser(QString());
				authenticator->setPassword(
					QString::fromStdString(_dataPassword));
			} else {
				*authenticator = QAuthenticator();
			}
		});
	_page->connect(
		_page,
		&QWebEnginePage::permissionRequested,
		_page,
		[=](QWebEnginePermission permission) {
			// Clipboard reads go through the embedder, gated on user input.
			using Type = QWebEnginePermission::PermissionType;
			if (restricted()
				|| permission.permissionType() == Type::ClipboardReadWrite) {
				permission.deny();
			}
		});
	_page->connect(
		_page,
		&QWebEnginePage::newWindowRequested,
		_page,
		[=](QWebEngineNewWindowRequest &request) {
			const auto url = request.requestedUrl();
			if (!restricted()
				&& _navigationStartHandler
				&& _navigationStartHandler(url.toString().toStdString(), true)) {
				QDesktopServices::openUrl(url);
			}
		});

	if (_view) {
		_zoom.emplace(_view);

		// Parented to the view, so it leaves the app filter list with it.
		new InteractionFilter(_view.data(), _view.data(), [
			weak = base::make_weak(this)
		] {
			if (const auto strong = weak.get()
					; strong && strong->_interactionHandler) {
				strong->_interactionHandler();
			}
		});
	}

	if (restricted()) {
		addScript(RestrictedContentTypeScript(), true);
	}
	addScript(QWebChannelScript());
	addScript(ExternalBridgeScript(_messageToken));

	setOpaqueBg(config.opaqueBg);

	if (_mode == WindowMode::External && !config.initialSize.isEmpty()) {
		_view->resize(config.initialSize);
	}

	return true;
}

bool Instance::restricted() const {
	return !_restrictedOrigin.isEmpty();
}

bool Instance::acceptNavigation(const QUrl &url, bool isMainFrame) {
	if (!isMainFrame) {
		return true;
	}
	// setHtml() loads a data: URL, report the base URL the document gets.
	const auto reported = (url.scheme() == u"data"_q
			&& !_htmlBaseUrl.isEmpty())
		? base::take(_htmlBaseUrl)
		: url;
	const auto string = reported.toString().toStdString();
	const auto domain = dataDomain();
	if (!domain.empty() && string.starts_with(domain)) {
		return true;
	}
	return !_navigationStartHandler || _navigationStartHandler(string, false);
}

void Instance::loadFinished(bool ok) {
	if (!ok || !restricted()) {
		if (_navigationDoneHandler) {
			_navigationDoneHandler(ok);
		}
		updateHistoryStates();
		return;
	}
	// Checked from an isolated world, the page can't fake the answer.
	_page->runJavaScript(
		u"document.contentType"_q,
		QWebEngineScript::ApplicationWorld,
		crl::guard(this, [=](const QVariant &result) {
			const auto html = (result.toString() == u"text/html"_q);
			if (!html) {
				_page->triggerAction(QWebEnginePage::Stop);
			}
			if (_navigationDoneHandler) {
				_navigationDoneHandler(html);
			}
			updateHistoryStates();
		}));
}

void Instance::messageReceived(const QString &message) {
	const auto text = message.toStdString();
	if (text.size() > kMaxScriptMessageBytes + _messageToken.size()
		|| !text.starts_with(_messageToken)
		|| !_messageHandler) {
		return;
	}
	_messageHandler(Message{
		.text = text.substr(_messageToken.size()),
		.sourceUrl = _page->url().toString().toStdString(),
	});
}

void Instance::updateThirdPartyHost() {
	if (!_allowThirdPartyCookies) {
		return;
	}
	const auto host = _page->url().host();
	if (host == _thirdPartyHost) {
		return;
	}
	const auto lock = QMutexLocker(_shared.mutex.get());
	auto &hosts = *_shared.thirdPartyHosts;
	if (!_thirdPartyHost.isEmpty()) {
		hosts.erase(hosts.find(_thirdPartyHost));
	}
	_thirdPartyHost = host;
	if (!host.isEmpty()) {
		hosts.insert(host);
	}
}

void Instance::addScript(const QString &source, bool allFrames) {
	if (source.isEmpty()) {
		return;
	}
	auto script = QWebEngineScript();
	script.setSourceCode(source);
	script.setInjectionPoint(QWebEngineScript::DocumentCreation);
	script.setWorldId(kWorldId);
	script.setRunsOnSubFrames(allFrames);
	_page->scripts().insert(script);
}

void Instance::navigate(std::string url) {
	_page->load(QUrl(QString::fromStdString(url)));
}

void Instance::navigateToData(std::string id) {
	if (!startDataServer()) {
		return;
	}
	navigate(dataDomain() + id);
}

void Instance::loadHtml(std::string html, std::string baseUrl) {
	_htmlBaseUrl = QUrl(QString::fromStdString(baseUrl));
	_page->setHtml(QString::fromStdString(html), _htmlBaseUrl);
}

void Instance::reload() {
	_page->triggerAction(QWebEnginePage::ReloadAndBypassCache);
}

void Instance::init(std::string js) {
	addScript(QString::fromStdString(js));
}

void Instance::initAllFrames(std::string js) {
	addScript(QString::fromStdString(js), true);
}

void Instance::eval(std::string js) {
	_page->runJavaScript(QString::fromStdString(js), kWorldId);
}

void Instance::focus() {
	if (_view) {
		_view->setFocus();
		_view->activateWindow();
	}
}

void Instance::setInteractionHandler(Fn<void()> handler) {
	_interactionHandler = std::move(handler);
}

void Instance::setOpaqueBg(QColor opaqueBg) {
	if (_page) {
		_page->setBackgroundColor(opaqueBg);
	}
}

void Instance::resize(int width, int height) {
	if (_widget) {
		_widget->resize(width, height);
	}
}

void Instance::setFullscreen(bool fullscreen) {
	if (_mode != WindowMode::External || !_view) {
		return;
	}
	if (fullscreen) {
		_view->showFullScreen();
	} else {
		_view->showNormal();
	}
}

QWidget *Instance::widget() {
	return _widget.get();
}

void Instance::refreshNavigationHistoryState() {
	updateHistoryStates();
}

auto Instance::navigationHistoryState()
-> rpl::producer<NavigationHistoryState> {
	return _navigationHistoryState.value();
}

ZoomController *Instance::zoomController() {
	return _zoom ? &*_zoom : nullptr;
}

void Instance::updateHistoryStates() {
	if (!_page) {
		return;
	}
	const auto history = _page->history();
	_navigationHistoryState = NavigationHistoryState{
		.url = _page->url().toString().toStdString(),
		.title = _page->title().toStdString(),
		.canGoBack = history->canGoBack(),
		.canGoForward = history->canGoForward(),
	};
}

std::string Instance::dataDomain() const {
	if (!_dataPort) {
		return std::string();
	}
	return std::format("http://{}:{}/", kDataHost, _dataPort);
}

// Basic auth keeps page content from reaching the localhost data server.
bool Instance::startDataServer() {
	if (_dataServer) {
		return true;
	}

	_dataPassword = QUuid::createUuid()
		.toString(QUuid::WithoutBraces)
		.toStdString();
	_dataServer.emplace(
		QByteArray(_dataPassword.c_str()),
		QByteArray::fromStdString(_dataRequestRedirectHost),
		[=](
				QTcpSocket *socket,
				const QByteArray &id,
				const ::base::flat_map<QByteArray, QByteArray> &headers,
				const std::shared_ptr<HttpServer::Guard> &guard) {
			if (!_dataRequestHandler) {
				return;
			}
			const auto resourceId = id.toStdString();
			auto prepared = DataRequest{
				.id = resourceId,
			};
			const auto getHeader = [&](const QByteArray &key) {
				const auto it = headers.find(key);
				return it != headers.end() ? it->second : QByteArray();
			};
			const auto rangeHeader = getHeader("Range");
			if (!rangeHeader.isEmpty()) {
				ParseRangeHeaderFor(prepared, rangeHeader.toStdString());
			}
			const auto requestedOffset = prepared.offset;
			const auto requestedLimit = prepared.limit;
			prepared.done = crl::guard(socket, [=](DataResponse resolved) {
				dataRequest(
					std::move(resolved),
					socket,
					resourceId,
					requestedOffset,
					requestedLimit,
					false,
					guard);
			});
			_dataRequestHandler(prepared);
		});

	if (!_dataServer->listen(QHostAddress::LocalHost)) {
		LOG(("WebView Error: %1").arg(_dataServer->errorString()));
		_dataServer.reset();
		return false;
	}
	_dataPort = _dataServer->serverPort();
	return true;
}

void Instance::dataRequest(
		DataResponse resolved,
		QTcpSocket *socket,
		const std::string &resourceId,
		std::int64_t requestedOffset,
		std::int64_t requestedLimit,
		bool headersWritten,
		const std::shared_ptr<HttpServer::Guard> &guard) {
	auto &stream = resolved.stream;
	if (!stream) {
		return;
	}
	const auto length = stream->size();
	Assert(length > 0);

	const auto offset = resolved.streamOffset;
	if (requestedOffset >= offset + length || offset > requestedOffset) {
		return;
	}

	auto bytes = QByteArray();
	bytes.resize(length);
	const auto read = stream->read(bytes.data(), length);
	Assert(read == length);

	const auto useOffset = (requestedOffset - offset);
	const auto useLength = (requestedLimit > 0)
		? std::min(requestedLimit, (length - useOffset))
		: (length - useOffset);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	bytes.slice(useOffset, useLength);
#else // Qt >= 6.8.0
	bytes = std::move(bytes.mid(useOffset, useLength));
#endif // Qt < 6.8.0

	const auto total = resolved.totalSize ? resolved.totalSize : length;
	const auto partial = (requestedOffset > 0) || (requestedLimit > 0);
	if (requestedLimit <= 0) {
		requestedLimit = (total - requestedOffset);
	}

	if (!headersWritten) {
		socket->write("HTTP/1.1 ");
		socket->write(partial ? "206 Partial Content\r\n" : "200 OK\r\n");

		const auto mime = QByteArray(stream->mime());
		socket->write("Content-Type: " + mime + "\r\n");
		socket->write("Accept-Ranges: bytes\r\n");
		socket->write("Cache-Control: no-store\r\n");
		socket->write("Content-Length: "
			+ QByteArray::number(requestedLimit)
			+ "\r\n");

		if (partial) {
			socket->write("Content-Range: bytes "
				+ QByteArray::number(requestedOffset)
				+ '-'
				+ QByteArray::number(requestedOffset + requestedLimit - 1)
				+ '/'
				+ QByteArray::number(total)
				+ "\r\n");
		}

		socket->write("\r\n");
		headersWritten = true;
	}

	socket->write(bytes);
	if (requestedLimit == useLength) {
		return;
	}

	requestedOffset += useLength;
	requestedLimit -= useLength;

	_dataRequestHandler({
		.id = resourceId,
		.offset = requestedOffset,
		.limit = requestedLimit,
		.done = crl::guard(socket, [=](DataResponse resolved) {
			dataRequest(
				std::move(resolved),
				socket,
				resourceId,
				requestedOffset,
				requestedLimit,
				headersWritten,
				guard);
		}),
	});
}

} // namespace

Bridge::Bridge(QObject *parent, Fn<void(const QString&)> handler)
: QObject(parent)
, _handler(std::move(handler)) {
}

void Bridge::invoke(const QString &message) {
	if (_handler) {
		_handler(message);
	}
}

Available Availability() {
	return Available{
		.customSchemeRequests = true,
		.customRangeRequests = true,
		.customReferer = true,
	};
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Supported()) {
		return nullptr;
	}
	auto result = std::make_unique<Instance>();
	if (!result->create(std::move(config))) {
		return nullptr;
	}
	return result;
}

bool SeparateStorageIdSupported() {
	return true;
}

} // namespace Webview::QtWebEngine

// Replaces lib_webview's webview_linux.cpp, which this build excludes.
namespace Webview {

Available Availability() {
	return QtWebEngine::Availability();
}

bool HiddenSupported() {
	return true;
}

bool SupportsEmbedAfterCreate() {
	return true;
}

bool SeparateStorageIdSupported() {
	return QtWebEngine::SeparateStorageIdSupported();
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	return QtWebEngine::CreateInstance(std::move(config));
}

std::string GenerateStorageToken() {
	constexpr auto kSize = 16;
	auto result = std::string(kSize, ' ');
	base::RandomFill(result.data(), result.size());
	return result;
}

void ClearStorageDataByToken(const std::string &token) {
}

// In-process backend: no-op WebKitGTK helper entry points for the launcher.
namespace WebKitGTK {

int Exec() {
	return 0;
}

void SetSocketPath(const std::string &socketPath) {
}

} // namespace WebKitGTK
} // namespace Webview
