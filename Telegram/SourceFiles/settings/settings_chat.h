/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

namespace Window {
class Controller;
} // namespace Window

namespace Settings {

namespace Builder {
struct HighlightEntry;
using HighlightRegistry = std::vector<std::pair<QString, HighlightEntry>>;
} // namespace Builder

void SetupDataStorage(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);
void SetupAutoDownload(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);
void SetupDefaultThemes(
	not_null<Window::Controller*> window,
	not_null<Ui::VerticalLayout*> container,
	Builder::HighlightRegistry *highlights = nullptr);
void SetupSupport(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);
void SetupExport(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	Fn<void(Type)> showOther);

void PaintRoundColorButton(
	QPainter &p,
	int size,
	QBrush brush,
	float64 selected);

void SetupThemeOptions(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	Builder::HighlightRegistry *highlights = nullptr);

void SetupThemeSettings(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	Builder::HighlightRegistry *highlights = nullptr);

void SetupCloudThemes(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	Builder::HighlightRegistry *highlights = nullptr);

void SetupChatBackground(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	Builder::HighlightRegistry *highlights = nullptr);

void SetupChatListQuickAction(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);

void SetupStickersEmoji(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	Builder::HighlightRegistry *highlights = nullptr);

void SetupMessages(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	Builder::HighlightRegistry *highlights = nullptr);

void SetupArchive(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	Fn<void(Type)> showOther);

void SetupSensitiveContent(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container,
	rpl::producer<> updateTrigger,
	Builder::HighlightRegistry *highlights = nullptr);

class Chat : public Section<Chat> {
public:
	Chat(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

	void fillTopBarMenu(
		const Ui::Menu::MenuCallback &addAction) override;

private:
	void setupContent(not_null<Window::SessionController*> controller);

	const not_null<Window::SessionController*> _controller;

};

} // namespace Settings
