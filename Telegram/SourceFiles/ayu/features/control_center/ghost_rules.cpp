#include "ayu/features/control_center/ghost_rules.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/control_center/control_center.h"
#include "base/random.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <map>

namespace AyuFeatures::GhostRules {
namespace {

using RulesMap = std::map<uint64, std::map<uint64, QString>>;

rpl::event_stream<> &ChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

[[nodiscard]] std::vector<Profile> Builtin() {
	return {
		{ u"full"_q, u"Полный призрак"_q, true, true, true, true },
		{ u"notyping"_q, u"Без набора текста"_q, false, true, true, true },
		{ u"noread"_q, u"Без прочтения"_q, true, false, false, true },
		{ u"normal"_q, u"Обычный (всё видно)"_q, false, false, false, true },
	};
}

[[nodiscard]] std::vector<Profile> ParseCustom(const QString &raw) {
	auto result = std::vector<Profile>();
	const auto array = QJsonDocument::fromJson(raw.toUtf8()).array();
	for (const auto &value : array) {
		const auto object = value.toObject();
		auto profile = Profile{
			.id = object.value(u"id"_q).toString(),
			.name = object.value(u"name"_q).toString(),
			.hideRead = object.value(u"read"_q).toBool(),
			.hideTyping = object.value(u"typing"_q).toBool(),
			.hideActions = object.value(u"actions"_q).toBool(),
		};
		if (!profile.id.isEmpty() && !profile.name.isEmpty()) {
			result.push_back(std::move(profile));
		}
	}
	return result;
}

void WriteCustom(const std::vector<Profile> &list) {
	auto array = QJsonArray();
	for (const auto &profile : list) {
		array.push_back(QJsonObject{
			{ u"id"_q, profile.id },
			{ u"name"_q, profile.name },
			{ u"read"_q, profile.hideRead },
			{ u"typing"_q, profile.hideTyping },
			{ u"actions"_q, profile.hideActions },
		});
	}
	AyuSettings::getInstance().setGhostProfiles(QString::fromUtf8(
		QJsonDocument(array).toJson(QJsonDocument::Compact)));
	ChangesStream().fire({});
}

// The rules are read on every read/typing request, so keep them parsed.
const RulesMap &CachedRules() {
	static auto raw = QString();
	static auto parsed = RulesMap();
	const auto &now = AyuSettings::getInstance().ghostChatRules();
	if (now != raw) {
		raw = now;
		parsed.clear();
		const auto object = QJsonDocument::fromJson(now.toUtf8()).object();
		for (auto i = object.begin(); i != object.end(); ++i) {
			auto &chats = parsed[i.key().toULongLong()];
			const auto inner = i.value().toObject();
			for (auto j = inner.begin(); j != inner.end(); ++j) {
				chats[j.key().toULongLong()] = j.value().toString();
			}
		}
	}
	return parsed;
}

void WriteRules(const RulesMap &rules) {
	auto object = QJsonObject();
	for (const auto &[self, chats] : rules) {
		if (chats.empty()) {
			continue;
		}
		auto inner = QJsonObject();
		for (const auto &[peer, profile] : chats) {
			inner.insert(QString::number(peer), profile);
		}
		object.insert(QString::number(self), inner);
	}
	AyuSettings::getInstance().setGhostChatRules(QString::fromUtf8(
		QJsonDocument(object).toJson(QJsonDocument::Compact)));
	ChangesStream().fire({});
}

[[nodiscard]] uint64 SelfKey(not_null<const PeerData*> peer) {
	return peer->session().userPeerId().value;
}

} // namespace

std::vector<Profile> Profiles() {
	auto result = Builtin();
	for (auto &profile : ParseCustom(AyuSettings::getInstance().ghostProfiles())) {
		result.push_back(std::move(profile));
	}
	return result;
}

std::optional<Profile> FindProfile(const QString &id) {
	for (auto &profile : Profiles()) {
		if (profile.id == id) {
			return profile;
		}
	}
	return std::nullopt;
}

void SaveCustomProfile(const Profile &profile) {
	auto list = ParseCustom(AyuSettings::getInstance().ghostProfiles());
	auto copy = profile;
	if (copy.id.isEmpty()) {
		copy.id = u"c"_q + QString::number(base::RandomValue<uint32>(), 16);
	}
	const auto i = ranges::find(list, copy.id, &Profile::id);
	if (i != end(list)) {
		*i = copy;
	} else {
		list.push_back(copy);
	}
	WriteCustom(list);
}

void RemoveCustomProfile(const QString &id) {
	auto list = ParseCustom(AyuSettings::getInstance().ghostProfiles());
	list.erase(ranges::remove(list, id, &Profile::id), end(list));
	WriteCustom(list);

	// Chats that used the removed profile fall back to the global settings.
	auto rules = CachedRules();
	for (auto &[self, chats] : rules) {
		for (auto i = chats.begin(); i != chats.end();) {
			i = (i->second == id) ? chats.erase(i) : std::next(i);
		}
	}
	WriteRules(rules);
}

QString ProfileIdFor(not_null<const PeerData*> peer) {
	const auto &rules = CachedRules();
	const auto i = rules.find(SelfKey(peer));
	if (i == end(rules)) {
		return QString();
	}
	const auto j = i->second.find(peer->id.value);
	return (j != end(i->second)) ? j->second : QString();
}

std::optional<Profile> ProfileFor(not_null<const PeerData*> peer) {
	const auto id = ProfileIdFor(peer);
	return id.isEmpty() ? std::nullopt : FindProfile(id);
}

void SetProfile(not_null<PeerData*> peer, const QString &profileId) {
	auto rules = CachedRules();
	auto &chats = rules[SelfKey(peer)];
	if (profileId.isEmpty()) {
		chats.erase(peer->id.value);
	} else {
		chats[peer->id.value] = profileId;
	}
	WriteRules(rules);
}

std::vector<std::pair<PeerId, QString>> Rules(
		not_null<Main::Session*> session) {
	auto result = std::vector<std::pair<PeerId, QString>>();
	const auto &rules = CachedRules();
	const auto i = rules.find(session->userPeerId().value);
	if (i != end(rules)) {
		for (const auto &[peer, profile] : i->second) {
			result.emplace_back(PeerId(peer), profile);
		}
	}
	return result;
}

rpl::producer<> Changes() {
	return ChangesStream().events();
}

bool SendRead(not_null<const PeerData*> peer) {
	if (const auto profile = ProfileFor(peer)) {
		return !profile->hideRead;
	}
	return AyuSettings::ghost(&peer->session()).sendReadMessages();
}

bool SendTyping(not_null<const PeerData*> peer) {
	if (const auto profile = ProfileFor(peer)) {
		return !profile->hideTyping;
	}
	return AyuSettings::ghost(&peer->session()).sendUploadProgress();
}

bool SendActions(not_null<const PeerData*> peer) {
	if (const auto profile = ProfileFor(peer)) {
		return !profile->hideActions;
	}
	return AyuSettings::ghost(&peer->session()).sendUploadProgress();
}

bool ReadAfterReply(not_null<const PeerData*> peer) {
	if (const auto profile = ProfileFor(peer)) {
		return profile->hideRead;
	}
	const auto &ghost = AyuSettings::ghost(&peer->session());
	return !ghost.sendReadMessages() && ghost.markReadAfterAction();
}

QString ProfileTitle(const QString &id) {
	if (const auto profile = FindProfile(id)) {
		return profile->name;
	}
	return u"Общие настройки"_q;
}

void FillMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto current = ProfileIdFor(peer);
	for (const auto &profile : Profiles()) {
		const auto id = profile.id;
		menu->addAction(
			(current == id ? u"✓  "_q : u"     "_q) + profile.name,
			[=] { SetProfile(peer, id); });
	}
	menu->addSeparator();
	menu->addAction(
		(current.isEmpty() ? u"✓  "_q : u"     "_q) + u"Общие настройки AyuGram"_q,
		[=] { SetProfile(peer, QString()); });
	menu->addAction(u"Профили и правила…"_q, [=] {
		ControlCenter::Show(controller, ControlCenter::Tab::Ghost);
	});
}

} // namespace AyuFeatures::GhostRules
