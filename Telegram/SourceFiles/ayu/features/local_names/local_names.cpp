#include "ayu/features/local_names/local_names.h"

#include "ayu/ayu_settings.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <map>

namespace AyuFeatures::LocalNames {
namespace {

constexpr auto kMaxLength = 64;

std::map<uint64, QString> Cache;
QString CacheSource;
bool CacheReady = false;

// Real names of renamed peers, to show them and to restore them.
std::map<uint64, QString> Originals;

[[nodiscard]] const std::map<uint64, QString> &Names() {
	const auto &raw = AyuSettings::getInstance().localNames();
	if (!CacheReady || raw != CacheSource) {
		CacheReady = true;
		CacheSource = raw;
		Cache.clear();
		const auto object = QJsonDocument::fromJson(raw.toUtf8()).object();
		for (auto i = object.begin(); i != object.end(); ++i) {
			const auto id = i.key().toULongLong();
			const auto name = i.value().toString().trimmed();
			if (id && !name.isEmpty()) {
				Cache.emplace(id, name);
			}
		}
	}
	return Cache;
}

[[nodiscard]] QString Lookup(PeerId id) {
	const auto &names = Names();
	const auto i = names.find(id.value);
	return (i != end(names)) ? i->second : QString();
}

void Store(PeerId id, const QString &name) {
	auto names = Names();
	if (name.isEmpty()) {
		names.erase(id.value);
	} else {
		names[id.value] = name;
	}
	auto object = QJsonObject();
	for (const auto &[key, value] : names) {
		object.insert(QString::number(key), value);
	}
	AyuSettings::getInstance().setLocalNames(QString::fromUtf8(
		QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

[[nodiscard]] QString RealName(not_null<PeerData*> peer) {
	const auto i = Originals.find(peer->id.value);
	return (i != end(Originals)) ? i->second : peer->name();
}

void Refresh(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		user->setName(
			user->firstName,
			user->lastName,
			user->nameOrPhone,
			user->editableUsername());
	} else if (const auto channel = peer->asChannel()) {
		channel->setName(RealName(peer), channel->editableUsername());
	} else if (const auto chat = peer->asChat()) {
		chat->setName(RealName(peer));
	}
}

void ShowBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto current = Lookup(peer->id);
	const auto real = RealName(peer);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_LocalNameTitle());
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::ayu_LocalNamePlaceholder(),
			current));
		field->setMaxLength(kMaxLength);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_LocalNameAbout(tr::now) + u"\n"_q + real,
			st::boxDividerLabel));
		box->setFocusCallback([=] { field->setFocusFast(); });

		const auto apply = [=](const QString &name) {
			Store(peer->id, name);
			Refresh(peer);
			box->closeBox();
		};
		const auto save = [=] { apply(field->getLastText().trimmed()); };
		field->submits() | rpl::on_next(save, field->lifetime());
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		if (!current.isEmpty()) {
			box->addLeftButton(tr::ayu_LocalNameReset(), [=] {
				apply(QString());
			});
		}
	}));
}

} // namespace

QString Apply(not_null<PeerData*> peer, const QString &name) {
	const auto local = Lookup(peer->id);
	if (local.isEmpty()) {
		Originals.erase(peer->id.value);
		return name;
	}
	Originals[peer->id.value] = name;
	return local;
}

void AddMenuAction(
		PeerData *peer,
		not_null<Window::SessionController*> controller,
		const Ui::Menu::MenuCallback &addAction) {
	if (!peer || peer->isSelf() || peer->isRepliesChat()) {
		return;
	}
	const auto renamed = !Lookup(peer->id).isEmpty();
	addAction({
		.text = (renamed
			? tr::ayu_LocalNameEdit(tr::now)
			: tr::ayu_LocalNameAdd(tr::now)),
		.handler = [=] { ShowBox(controller, peer); },
		.icon = &st::menuIconEdit,
	});
}

} // namespace AyuFeatures::LocalNames
