#include "ayu/features/control_center/control_center.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/control_center/advanced_search.h"
#include "ayu/features/control_center/ghost_rules.h"
#include "ayu/features/control_center/link_check.h"
#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QFile>
#include <QtCore/QUrl>
#include <QtGui/QCursor>

#include <array>

namespace AyuFeatures::ControlCenter {
namespace {

constexpr auto kMaxShownResults = 200;

struct State {
	not_null<Window::SessionController*> controller;
	Ui::VerticalLayout *inner = nullptr;
	Tab tab = Tab::Ghost;
	Fn<void()> rebuild;
	Fn<void()> rebuildNow;
	bool rebuildQueued = false;
	base::unique_qptr<Ui::PopupMenu> menu;
	AdvancedSearch::MediaType searchType = AdvancedSearch::MediaType::Any;
	std::vector<AdvancedSearch::Result> results;
	QString searchStatus;
	QString searchLine;
	QString searchChat;
	QString searchAuthor;
	QString searchAfter;
	QString searchBefore;
	int searchSource = 0;
	bool searching = false;
};

not_null<Ui::FlatLabel*> AddLabel(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		const style::FlatLabel &st = st::boxLabel) {
	return container->add(
		object_ptr<Ui::FlatLabel>(container, text, st),
		st::boxRowPadding);
}

[[nodiscard]] not_null<Ui::SettingsButton*> AddButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	return container->add(object_ptr<Ui::SettingsButton>(
		container,
		rpl::single(text),
		st::settingsButtonNoIcon));
}

[[nodiscard]] not_null<Ui::InputField*> AddField(
		not_null<Ui::VerticalLayout*> container,
		const QString &placeholder,
		const QString &value) {
	return container->add(
		object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			rpl::single(placeholder),
			value),
		st::boxRowPadding);
}

[[nodiscard]] QString ProfileSummary(const GhostRules::Profile &profile) {
	auto hidden = QStringList();
	if (profile.hideRead) {
		hidden.push_back(u"прочтение"_q);
	}
	if (profile.hideTyping) {
		hidden.push_back(u"«печатает»"_q);
	}
	if (profile.hideActions) {
		hidden.push_back(u"«записывает / отправляет»"_q);
	}
	return hidden.isEmpty()
		? u"ничего не скрывает"_q
		: (u"скрывает: "_q + hidden.join(u", "_q));
}

void EditProfileBox(
		not_null<Window::SessionController*> controller,
		std::optional<GhostRules::Profile> existing) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(existing
			? u"Профиль невидимки"_q
			: u"Новый профиль"_q));
		const auto name = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Название"_q),
			existing ? existing->name : QString()));
		const auto check = [&](const QString &text, bool value) {
			return box->addRow(object_ptr<Ui::Checkbox>(
				box,
				text,
				value,
				st::defaultBoxCheckbox));
		};
		const auto read = check(
			u"Скрывать прочтение"_q,
			existing ? existing->hideRead : true);
		const auto typing = check(
			u"Скрывать «печатает…»"_q,
			existing ? existing->hideTyping : true);
		const auto actions = check(
			u"Скрывать «записывает / отправляет»"_q,
			existing ? existing->hideActions : true);
		box->setFocusCallback([=] { name->setFocusFast(); });
		const auto save = [=] {
			const auto title = name->getLastText().trimmed();
			if (title.isEmpty()) {
				name->showError();
				return;
			}
			GhostRules::SaveCustomProfile({
				.id = existing ? existing->id : QString(),
				.name = title,
				.hideRead = read->checked(),
				.hideTyping = typing->checked(),
				.hideActions = actions->checked(),
			});
			box->closeBox();
		};
		name->submits() | rpl::on_next(save, name->lifetime());
		box->addButton(rpl::single(u"Сохранить"_q), save);
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
		if (existing) {
			const auto id = existing->id;
			box->addLeftButton(rpl::single(u"Удалить"_q), [=] {
				GhostRules::RemoveCustomProfile(id);
				box->closeBox();
			});
		}
	}));
}

void BuildGhost(
		not_null<State*> state,
		not_null<Ui::VerticalLayout*> container) {
	const auto controller = state->controller;
	const auto session = &controller->session();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Профили"_q));
	for (const auto &profile : GhostRules::Profiles()) {
		const auto button = AddButton(
			container,
			profile.name + u"  —  "_q + ProfileSummary(profile));
		if (profile.builtin) {
			button->setAttribute(Qt::WA_TransparentForMouseEvents);
		} else {
			button->setClickedCallback([=] {
				EditProfileBox(controller, profile);
			});
		}
	}
	AddButton(container, u"+  Новый профиль"_q)->setClickedCallback([=] {
		EditProfileBox(controller, std::nullopt);
	});

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Чаты с особым режимом"_q));
	const auto rules = GhostRules::Rules(session);
	if (rules.empty()) {
		AddLabel(
			container,
			u"Пока нет. Нажми на призрака в шапке любого чата, чтобы "
			"выбрать для него режим."_q,
			st::boxDividerLabel);
	}
	for (const auto &[peerId, profileId] : rules) {
		const auto peer = session->data().peerLoaded(peerId);
		const auto name = peer
			? peer->name()
			: (u"Чат "_q + QString::number(peerId.value));
		const auto button = AddButton(
			container,
			name + u"  —  "_q + GhostRules::ProfileTitle(profileId));
		button->setClickedCallback([=] {
			if (!peer) {
				return;
			}
			state->menu = base::make_unique_q<Ui::PopupMenu>(
				button,
				st::defaultPopupMenu);
			GhostRules::FillMenu(state->menu.get(), controller, peer);
			state->menu->addAction(u"Открыть чат"_q, [=] {
				controller->showPeerHistory(peer);
			});
			state->menu->popup(QCursor::pos());
		});
	}
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Режим чата важнее общих настроек невидимки. Статус "
			"«в сети» Telegram показывает всем сразу, поэтому он "
			"настраивается только глобально."_q));
}

void OpenResult(
		not_null<State*> state,
		const AdvancedSearch::Result &result,
		Fn<void()> close) {
	const auto controller = state->controller;
	if (!result.local && result.id.peer) {
		close();
		controller->showPeerHistory(
			result.id.peer,
			Window::SectionShow::Way::ClearStack,
			result.id.msg);
		return;
	}
	const auto copy = result;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(copy.kind.isEmpty()
			? u"Сообщение"_q
			: copy.kind));
		box->setWidth(st::boxWideWidth);
		const auto when = base::unixtime::parse(copy.date).toString(
			u"dd.MM.yyyy HH:mm"_q);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			copy.chat
				+ (copy.author.isEmpty() ? QString() : (u" · "_q + copy.author))
				+ u" · "_q
				+ when,
			st::boxDividerLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			copy.text,
			st::boxLabel));
		if (copy.id.peer) {
			box->addLeftButton(rpl::single(u"Открыть чат"_q), [=] {
				box->closeBox();
				close();
				controller->showPeerHistory(
					copy.id.peer,
					Window::SectionShow::Way::ClearStack,
					copy.id.msg);
			});
		}
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

void AddResultRow(
		not_null<State*> state,
		not_null<Ui::VerticalLayout*> container,
		const AdvancedSearch::Result &result,
		Fn<void()> close) {
	const auto row = container->add(object_ptr<Ui::AbstractButton>(container));
	row->resize(row->width(), st::ayuSearchRowHeight);
	const auto copy = result;
	row->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(row);
		if (row->isOver()) {
			p.fillRect(row->rect(), st::windowBgOver);
		}
		const auto &padding = st::ayuSearchRowPadding;
		const auto width = row->width() - padding.left() - padding.right();
		const auto when = base::unixtime::parse(copy.date).toString(
			u"dd.MM.yyyy HH:mm"_q);
		auto header = copy.chat;
		if (!copy.author.isEmpty() && copy.author != copy.chat) {
			header += u" · "_q + copy.author;
		}
		header += u" · "_q + when;
		auto left = padding.left();
		if (!copy.kind.isEmpty()) {
			const auto badge = u"["_q + copy.kind + u"] "_q;
			p.setFont(st::semiboldFont);
			p.setPen(st::attentionButtonFg);
			p.drawText(left, padding.top() + st::semiboldFont->ascent, badge);
			left += st::semiboldFont->width(badge);
		}
		p.setFont(st::semiboldFont);
		p.setPen(st::windowActiveTextFg);
		p.drawText(
			left,
			padding.top() + st::semiboldFont->ascent,
			st::semiboldFont->elided(header, width - (left - padding.left())));
		p.setFont(st::normalFont);
		p.setPen(st::windowFg);
		const auto line = copy.text.simplified();
		p.drawText(
			padding.left(),
			row->height() - padding.bottom() - st::normalFont->descent,
			st::normalFont->elided(line, width));
	}, row->lifetime());
	row->setClickedCallback([=] { OpenResult(state, copy, close); });
}

void BuildSearch(
		not_null<State*> state,
		not_null<Ui::VerticalLayout*> container,
		Fn<void()> close) {
	using namespace AdvancedSearch;
	const auto controller = state->controller;

	Ui::AddSkip(container);
	const auto line = AddField(
		container,
		u"Что ищем (текст и операторы)"_q,
		state->searchLine);
	AddLabel(
		container,
		u"Операторы: from:@user  in:@чат  has:photo/video/file/voice/round/"
		"music/gif/link  after:2026-01-01  before:2026-02-01  "
		"deleted:true  edited:true"_q,
		st::boxDividerLabel);
	const auto chat = AddField(
		container,
		u"Чат: @username или название (необязательно)"_q,
		state->searchChat);
	const auto author = AddField(
		container,
		u"Автор: @username или имя (необязательно)"_q,
		state->searchAuthor);
	const auto after = AddField(
		container,
		u"После даты: ГГГГ-ММ-ДД"_q,
		state->searchAfter);
	const auto before = AddField(
		container,
		u"До даты: ГГГГ-ММ-ДД"_q,
		state->searchBefore);

	const auto typeButton = AddButton(
		container,
		u"Тип вложения: "_q + TypeTitle(state->searchType));
	typeButton->setClickedCallback([=] {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			typeButton,
			st::defaultPopupMenu);
		for (const auto type : {
				MediaType::Any, MediaType::Photo, MediaType::Video,
				MediaType::File, MediaType::Voice, MediaType::Round,
				MediaType::Music, MediaType::Gif, MediaType::Link }) {
			state->menu->addAction(TypeTitle(type), [=] {
				state->searchType = type;
				state->rebuild();
			});
		}
		state->menu->popup(QCursor::pos());
	});

	const auto source = std::make_shared<Ui::RadiobuttonGroup>(
		state->searchSource);
	const auto sources = std::array<QString, 3>{ {
		u"Все сообщения (сервер + сохранённые)"_q,
		u"Только удалённые"_q,
		u"Только старые версии правок"_q,
	} };
	for (auto i = 0; i != int(sources.size()); ++i) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				source,
				i,
				sources[i],
				st::defaultBoxCheckbox),
			st::boxRowPadding);
	}

	const auto remember = [=] {
		state->searchLine = line->getLastText();
		state->searchChat = chat->getLastText();
		state->searchAuthor = author->getLastText();
		state->searchAfter = after->getLastText();
		state->searchBefore = before->getLastText();
		state->searchSource = source->current();
	};
	source->setChangedCallback([=](int) { remember(); });

	const auto run = [=] {
		if (state->searching) {
			return;
		}
		remember();
		auto query = Parse(state->searchLine);
		if (!state->searchChat.trimmed().isEmpty()) {
			query.in = state->searchChat.trimmed();
		}
		if (!state->searchAuthor.trimmed().isEmpty()) {
			query.from = state->searchAuthor.trimmed();
		}
		if (state->searchType != MediaType::Any) {
			query.type = state->searchType;
		}
		if (state->searchSource == 1) {
			query.source = Source::Deleted;
		} else if (state->searchSource == 2) {
			query.source = Source::Edited;
		}
		const auto parseDate = [](const QString &value) {
			return QDate::fromString(value.trimmed(), Qt::ISODate);
		};
		if (const auto date = parseDate(state->searchAfter); date.isValid()) {
			query.after = date;
		}
		if (const auto date = parseDate(state->searchBefore); date.isValid()) {
			query.before = date;
		}
		state->searching = true;
		state->searchStatus = u"Ищу…"_q;
		state->rebuild();
		Run(
			&controller->session(),
			query,
			crl::guard(state->inner, [=](
					std::vector<Result> results,
					QString status) {
				state->searching = false;
				state->results = std::move(results);
				state->searchStatus = status;
				state->rebuild();
			}));
	};
	line->submits() | rpl::on_next(run, line->lifetime());
	AddButton(container, u"🔍  Искать"_q)->setClickedCallback(run);

	if (!state->searchStatus.isEmpty()) {
		Ui::AddSkip(container);
		AddLabel(container, state->searchStatus, st::boxDividerLabel);
	}
	if (!state->results.empty()) {
		const auto exportButton = AddButton(
			container,
			u"💾  Экспорт в HTML"_q);
		exportButton->setClickedCallback([=] {
			const auto results = state->results;
			FileDialog::GetWritePath(
				exportButton.get(),
				u"Экспорт результатов поиска"_q,
				u"HTML (*.html)"_q,
				u"ayugram_search.html"_q,
				[=](QString &&path) {
					QFile file(path);
					if (file.open(QIODevice::WriteOnly)) {
						file.write(ExportHtml(
							results,
							u"Поиск AyuGram"_q).toUtf8());
						controller->showToast(u"Сохранено"_q);
					} else {
						controller->showToast(u"Не удалось сохранить"_q);
					}
				});
		});
		Ui::AddSkip(container);
		auto shown = 0;
		for (const auto &result : state->results) {
			if (++shown > kMaxShownResults) {
				AddLabel(
					container,
					u"Показаны первые 200, в HTML попадут все."_q,
					st::boxDividerLabel);
				break;
			}
			AddResultRow(state, container, result, close);
		}
	}
	Ui::AddSkip(container);
}

void BuildLinks(
		not_null<State*> state,
		not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	const auto enabled = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			u"Проверять сторонние ссылки перед открытием"_q,
			AyuSettings::getInstance().linkCheckEnabled(),
			st::defaultBoxCheckbox),
		st::boxRowPadding);
	enabled->checkedChanges() | rpl::on_next([=](bool checked) {
		AyuSettings::getInstance().setLinkCheckEnabled(checked);
	}, enabled->lifetime());

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(u"Доверенные сайты (открываются сразу)"_q));
	const auto hosts = LinkCheck::TrustedHosts();
	if (hosts.isEmpty()) {
		AddLabel(container, u"Список пуст."_q, st::boxDividerLabel);
	}
	for (const auto &host : hosts) {
		AddButton(container, host + u"   ✕"_q)->setClickedCallback([=] {
			auto list = LinkCheck::TrustedHosts();
			list.removeAll(host);
			LinkCheck::SetTrustedHosts(list);
			state->rebuild();
		});
	}
	const auto field = AddField(
		container,
		u"Добавить сайт, например example.com"_q,
		QString());
	const auto add = [=] {
		auto host = field->getLastText().trimmed().toLower();
		if (host.startsWith(u"http://"_q) || host.startsWith(u"https://"_q)) {
			host = QUrl(host).host();
		}
		if (host.isEmpty()) {
			field->showError();
			return;
		}
		auto list = LinkCheck::TrustedHosts();
		list.push_back(host);
		LinkCheck::SetTrustedHosts(list);
		state->rebuild();
	};
	field->submits() | rpl::on_next(add, field->lifetime());
	AddButton(container, u"+  Добавить"_q)->setClickedCallback(add);
	AddButton(container, u"Вернуть список по умолчанию"_q)->setClickedCallback([=] {
		AyuSettings::getInstance().setTrustedLinkHosts(QString());
		state->rebuild();
	});
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Перед открытием показываются настоящий домен, "
			"предупреждения про IP-логгеры, сокращатели и поддельные "
			"адреса, а превью страницы загружает сервер Telegram, "
			"поэтому сайт не видит твой IP."_q));
}

void BuildSending(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Отмена отправки"_q));
	const auto values = std::array<int, 5>{ { 0, 1, 3, 5, 10 } };
	auto current = 0;
	for (auto i = 0; i != int(values.size()); ++i) {
		if (values[i] == AyuSettings::getInstance().undoSendDelay()) {
			current = i;
		}
	}
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(current);
	for (auto i = 0; i != int(values.size()); ++i) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				group,
				i,
				(values[i]
					? (u"Задержка "_q + QString::number(values[i]) + u" с"_q)
					: u"Выключено"_q),
				st::defaultBoxCheckbox),
			st::boxRowPadding);
	}
	group->setChangedCallback([=](int index) {
		AyuSettings::getInstance().setUndoSendDelay(values[index]);
	});
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Текст, фото, видео, файлы, голосовые и кружки уходят "
			"не сразу: пока сообщение полупрозрачное, его можно отменить "
			"кнопкой рядом с ним или Ctrl+Z, и текст вернётся в поле "
			"ввода. Стикеры и GIF отправляются сразу."_q));
}

} // namespace

void Show(not_null<Window::SessionController*> controller, Tab tab) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setNoContentMargin(true);
		const auto state = box->lifetime().make_state<State>(State{
			.controller = controller,
			.tab = tab,
		});
		const auto tabs = box->setPinnedToTopContent(
			object_ptr<Ui::SettingsSlider>(
				box.get(),
				st::defaultTabsSlider));
		tabs->resizeToWidth(st::boxWideWidth);
		tabs->move(0, 0);
		tabs->setRippleTopRoundRadius(st::boxRadius);
		tabs->setSections({
			u"Невидимка"_q,
			u"Поиск"_q,
			u"Ссылки"_q,
			u"Отправка"_q,
		});
		tabs->setActiveSectionFast(int(tab));

		state->inner = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto close = crl::guard(box, [=] { box->closeBox(); });
		state->rebuildNow = [=] {
			const auto inner = state->inner;
			inner->clear();
			switch (state->tab) {
			case Tab::Ghost: BuildGhost(state, inner); break;
			case Tab::Search: BuildSearch(state, inner, close); break;
			case Tab::Links: BuildLinks(state, inner); break;
			case Tab::Sending: BuildSending(inner); break;
			}
			inner->resizeToWidth(st::boxWideWidth);
		};
		// Buttons ask for a rebuild from their own click handlers.
		state->rebuild = [=] {
			if (state->rebuildQueued) {
				return;
			}
			state->rebuildQueued = true;
			crl::on_main(state->inner, [=] {
				state->rebuildQueued = false;
				state->rebuildNow();
			});
		};
		tabs->sectionActivated(
		) | rpl::on_next([=](int index) {
			state->tab = Tab(index);
			state->rebuild();
		}, box->lifetime());
		GhostRules::Changes(
		) | rpl::on_next([=] {
			if (state->tab == Tab::Ghost) {
				state->rebuild();
			}
		}, box->lifetime());
		state->rebuildNow();

		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

} // namespace AyuFeatures::ControlCenter
