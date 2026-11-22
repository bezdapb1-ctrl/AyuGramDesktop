#include "ayu/features/control_center/link_check.h"

#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "base/call_delayed.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "main/main_session.h"
#include "ui/basic_click_handlers.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace AyuFeatures::LinkCheck {
namespace {

constexpr auto kPreviewRetries = 3;
constexpr auto kPreviewRetryDelay = crl::time(1500);

bool Bypass = false;

const auto kLoggers = {
	"grabify.link", "grabify.org", "grabify.icu", "iplogger.org",
	"iplogger.com", "iplogger.ru", "iplogger.co", "iplogger.info",
	"2no.co", "yip.su", "iplis.ru", "ps3cfw.com", "blasze.com",
	"blasze.tk", "bmwforum.co", "leancoding.co", "quickmessage.us",
	"spottyfly.com", "stopify.co", "curiouscat.club", "catsnthings.fun",
	"catsnthings.com", "gyazo.nl", "ipgraber.ru", "ipgrabber.ru",
	"ezstat.ru", "fuglekos.com", "partpicker.shop", "sportshub.bar",
	"locations.quest", "lovebird.guru", "trulove.guru", "dateing.club",
	"shrekis.life", "headshot.monster", "gaming-at-my.best",
	"progaming.monster", "yourmy.monster", "imageshare.best",
	"screenshot.best", "gamingfun.me", "crabrave.pw", "joinmy.site",
	"iplogger.cn", "wl.gl", "ed.tc", "bc.ax", "maper.info",
};

const auto kShorteners = {
	"bit.ly", "tinyurl.com", "t.co", "goo.gl", "ow.ly", "is.gd",
	"cutt.ly", "rebrand.ly", "shorturl.at", "clck.ru", "vk.cc",
	"tiny.cc", "rb.gy", "s.id", "u.to", "qps.ru", "bit.do", "lnkd.in",
	"buff.ly", "adf.ly", "shorte.st", "v.gd", "t.ly", "short.io",
	"surl.li", "goo.su", "inlnk.ru", "shrtco.de",
};

struct Brand {
	const char *word = nullptr;
	const char *title = nullptr;
	std::vector<const char*> official;
};

const std::vector<Brand> &Brands() {
	static const auto result = std::vector<Brand>{
		{ "steam", "Steam", { "steampowered.com", "steamcommunity.com",
			"steamstatic.com", "steamgames.com", "steam-chat.com",
			"steamdeck.com", "steamserver.net", "steamcontent.com" } },
		{ "telegram", "Telegram", { "telegram.org", "t.me",
			"telegram.me", "telegra.ph", "telesco.pe" } },
		{ "discord", "Discord", { "discord.com", "discord.gg",
			"discordapp.com", "discordapp.net", "discord.media" } },
		{ "faceit", "FACEIT", { "faceit.com" } },
		{ "funpay", "FunPay", { "funpay.com" } },
		{ "paypal", "PayPal", { "paypal.com", "paypal.me" } },
		{ "roblox", "Roblox", { "roblox.com" } },
		{ "epicgames", "Epic Games", { "epicgames.com" } },
		{ "binance", "Binance", { "binance.com" } },
		{ "fragment", "Fragment", { "fragment.com" } },
		{ "tonkeeper", "Tonkeeper", { "tonkeeper.com" } },
		{ "instagram", "Instagram", { "instagram.com" } },
		{ "tiktok", "TikTok", { "tiktok.com" } },
		{ "youtube", "YouTube", { "youtube.com", "youtu.be" } },
		{ "github", "GitHub", { "github.com", "github.io",
			"githubusercontent.com" } },
		{ "twitch", "Twitch", { "twitch.tv" } },
		{ "spotify", "Spotify", { "spotify.com" } },
		{ "google", "Google", { "google.com", "google.ru",
			"google.com.ua", "googleusercontent.com", "googleapis.com" } },
	};
	return result;
}

const auto kDefaultTrusted = {
	"youtube.com", "youtu.be", "google.com", "github.com",
	"wikipedia.org", "telegram.org", "x.com", "twitter.com", "twitch.tv",
	"steampowered.com", "steamcommunity.com", "discord.com", "reddit.com",
	"vk.com", "spotify.com", "soundcloud.com", "instagram.com",
	"tiktok.com", "yandex.ru", "genius.com",
};

[[nodiscard]] bool Matches(const QString &host, const QString &domain) {
	return (host == domain) || host.endsWith(u"."_q + domain);
}

template <typename List>
[[nodiscard]] bool MatchesAny(const QString &host, const List &list) {
	for (const auto domain : list) {
		if (Matches(host, QString::fromLatin1(domain))) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool LooksLikeIp(const QString &host) {
	if (host.startsWith('[') || host.contains(':')) {
		return true;
	}
	for (const auto ch : host) {
		if (!ch.isDigit() && ch != '.') {
			return false;
		}
	}
	return !host.isEmpty();
}

[[nodiscard]] bool MixedScripts(const QString &host) {
	auto latin = false;
	auto cyrillic = false;
	for (const auto ch : host) {
		const auto code = ch.unicode();
		if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')) {
			latin = true;
		} else if (code >= 0x0400 && code <= 0x04FF) {
			cyrillic = true;
		}
	}
	return latin && cyrillic;
}

void OpenAnyway(const QString &url, const QVariant &context) {
	Bypass = true;
	UrlClickHandler::Open(url, context);
	Bypass = false;
}

void RequestPreview(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		not_null<Ui::FlatLabel*> label,
		const QString &url,
		int retriesLeft) {
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	session->api().request(MTPmessages_GetWebPagePreview(
		MTP_flags(0),
		MTP_string(url),
		MTPVector<MTPMessageEntity>()
	)).done([=](const MTPmessages_WebPagePreview &result) {
		if (!weak) {
			return;
		}
		auto lines = QStringList();
		auto pending = false;
		result.data().vmedia().match([&](const MTPDmessageMediaWebPage &media) {
			media.vwebpage().match([&](const MTPDwebPage &page) {
				const auto site = qs(page.vsite_name().value_or_empty());
				const auto title = qs(page.vtitle().value_or_empty());
				const auto about = qs(page.vdescription().value_or_empty());
				const auto shown = qs(page.vdisplay_url());
				if (!site.isEmpty()) {
					lines.push_back(u"Сайт: "_q + site);
				}
				if (!title.isEmpty()) {
					lines.push_back(u"Заголовок: "_q + title);
				}
				if (!about.isEmpty()) {
					lines.push_back(about.left(300));
				}
				if (!shown.isEmpty()) {
					lines.push_back(u"Конечный адрес: "_q + shown);
				}
			}, [&](const MTPDwebPagePending &) {
				pending = true;
			}, [&](const auto &) {
			});
		}, [&](const auto &) {
		});
		if (pending && retriesLeft > 0) {
			base::call_delayed(kPreviewRetryDelay, weak.data(), [=] {
				RequestPreview(box, session, label, url, retriesLeft - 1);
			});
			return;
		}
		label->setText(lines.isEmpty()
			? u"Telegram не смог собрать превью этой страницы."_q
			: lines.join(QChar(10)));
	}).fail([=] {
		if (weak) {
			label->setText(u"Превью недоступно."_q);
		}
	}).send();
}

void ShowCheckBox(
		not_null<Window::SessionController*> controller,
		const QString &url,
		const QVariant &context) {
	const auto analysis = Analyze(url);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Проверка ссылки"_q));
		box->setWidth(st::boxWideWidth);

		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Сайт: "_q + analysis.shownHost,
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			url,
			st::boxDividerLabel));

		auto warnings = analysis.warnings;
		if (warnings.isEmpty()) {
			warnings.push_back(u"✓ Явных признаков опасности не найдено."_q);
		}
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			warnings.join(QChar(10)),
			st::boxLabel));

		const auto preview = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Загружаю превью через сервер Telegram, сайт твой IP не увидит…"_q,
			st::boxDividerLabel));
		RequestPreview(
			box,
			&controller->session(),
			preview,
			url,
			kPreviewRetries);

		const auto trust = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Доверять сайту "_q + analysis.host + u" и открывать сразу"_q,
			false,
			st::defaultBoxCheckbox));

		box->addButton(rpl::single(u"Открыть"_q), [=] {
			if (trust->checked() && !analysis.host.isEmpty()) {
				auto list = TrustedHosts();
				if (!list.contains(analysis.host)) {
					list.push_back(analysis.host);
					SetTrustedHosts(list);
				}
			}
			box->closeBox();
			OpenAnyway(url, context);
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
		box->addLeftButton(rpl::single(u"Скопировать"_q), [=] {
			QGuiApplication::clipboard()->setText(url);
			controller->showToast(u"Ссылка скопирована"_q);
		});
	}));
}

} // namespace

Analysis Analyze(const QString &url) {
	auto result = Analysis();
	const auto parsed = QUrl::fromUserInput(url);
	const auto host = parsed.host().toLower();
	const auto unicode = QUrl::fromAce(host.toLatin1());
	result.host = host;
	result.shownHost = unicode.isEmpty() ? host : unicode;

	const auto danger = [&](const QString &text) {
		result.warnings.push_back(u"⛔ "_q + text);
		result.danger = true;
	};
	const auto warn = [&](const QString &text) {
		result.warnings.push_back(u"⚠ "_q + text);
	};

	if (MatchesAny(host, kLoggers)) {
		danger(u"Известный IP-логгер: откроешь — владелец узнает твой IP, "
			"город и устройство."_q);
	}
	if (!parsed.userName().isEmpty()) {
		danger(u"В адресе спрятан «@»: настоящий сайт — "_q + host + u"."_q);
	}
	if (LooksLikeIp(host)) {
		danger(u"Вместо названия сайта — голый IP-адрес."_q);
	}
	if (host.contains(u"xn--"_q)) {
		danger(u"Адрес записан спецсимволами (punycode): "_q
			+ result.shownHost
			+ u" — так маскируют поддельные сайты."_q);
	}
	if (MixedScripts(result.shownHost)) {
		danger(u"В адресе смешаны русские и латинские буквы."_q);
	}
	for (const auto &brand : Brands()) {
		const auto word = QString::fromLatin1(brand.word);
		if (host.contains(word) && !MatchesAny(host, brand.official)) {
			danger(u"Похоже на подделку под «"_q
				+ QString::fromLatin1(brand.title)
				+ u"», но это не официальный сайт."_q);
			break;
		}
	}
	if (MatchesAny(host, kShorteners)) {
		warn(u"Сокращённая ссылка: настоящий адрес скрыт. Превью ниже "
			"показывает, куда она ведёт."_q);
	}
	if (parsed.scheme().toLower() == u"http"_q) {
		warn(u"Соединение без шифрования (http)."_q);
	}
	if (parsed.port() > 0 && parsed.port() != 80 && parsed.port() != 443) {
		warn(u"Нестандартный порт "_q + QString::number(parsed.port()) + u"."_q);
	}
	if (host.count('.') >= 4) {
		warn(u"Очень длинный адрес с кучей поддоменов."_q);
	}
	return result;
}

QStringList DefaultTrustedHosts() {
	auto result = QStringList();
	for (const auto host : kDefaultTrusted) {
		result.push_back(QString::fromLatin1(host));
	}
	return result;
}

QStringList TrustedHosts() {
	const auto &raw = AyuSettings::getInstance().trustedLinkHosts();
	if (raw.isEmpty()) {
		return DefaultTrustedHosts();
	} else if (raw == u"-"_q) {
		return {};
	}
	return raw.split(QChar(10), Qt::SkipEmptyParts);
}

void SetTrustedHosts(const QStringList &hosts) {
	auto clean = QStringList();
	for (const auto &host : hosts) {
		const auto trimmed = host.trimmed().toLower();
		if (!trimmed.isEmpty() && !clean.contains(trimmed)) {
			clean.push_back(trimmed);
		}
	}
	AyuSettings::getInstance().setTrustedLinkHosts(clean.isEmpty()
		? u"-"_q
		: clean.join(QChar(10)));
}

bool IsTrusted(const QString &host) {
	for (const auto &domain : TrustedHosts()) {
		if (Matches(host, domain)) {
			return true;
		}
	}
	return false;
}

bool Intercept(const QString &url, const QVariant &context) {
	if (Bypass || !AyuSettings::getInstance().linkCheckEnabled()) {
		return false;
	}
	const auto parsed = QUrl::fromUserInput(url);
	const auto scheme = parsed.scheme().toLower();
	if (scheme != u"http"_q && scheme != u"https"_q) {
		return false;
	}
	const auto host = parsed.host().toLower();
	if (host.isEmpty() || IsTrusted(host)) {
		return false;
	}
	auto controller = context.value<ClickHandlerContext>().sessionWindow.get();
	if (!controller) {
		if (const auto window = Core::App().activeWindow()) {
			controller = window->sessionController();
		}
	}
	if (!controller) {
		return false;
	}
	ShowCheckBox(controller, url, context);
	return true;
}

} // namespace AyuFeatures::LinkCheck
