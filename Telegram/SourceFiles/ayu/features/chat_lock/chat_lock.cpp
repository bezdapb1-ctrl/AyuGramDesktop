#include "ayu/features/chat_lock/chat_lock.h"

#include "ayu/ayu_settings.h"
#include "base/random.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QCryptographicHash>

#include <map>

namespace AyuFeatures::ChatLock {
namespace {

constexpr auto kUnlockedFor = crl::time(5 * 60 * 1000);
constexpr auto kMinPinLength = 4;

using Storage = std::map<uint64, std::vector<uint64>>;

std::map<uint64, crl::time> UnlockedTill;

[[nodiscard]] Storage Parse(const QString &raw) {
	auto result = Storage();
	for (const auto &account : raw.split(';', Qt::SkipEmptyParts)) {
		const auto parts = account.split(':');
		if (parts.size() != 2) {
			continue;
		}
		const auto self = parts[0].toULongLong();
		if (!self) {
			continue;
		}
		auto &list = result[self];
		for (const auto &peer : parts[1].split(',', Qt::SkipEmptyParts)) {
			if (const auto id = peer.toULongLong()) {
				list.push_back(id);
			}
		}
	}
	return result;
}

[[nodiscard]] QString Serialize(const Storage &storage) {
	auto accounts = QStringList();
	for (const auto &[self, list] : storage) {
		if (list.empty()) {
			continue;
		}
		auto peers = QStringList();
		for (const auto id : list) {
			peers.push_back(QString::number(id));
		}
		accounts.push_back(QString::number(self) + ':' + peers.join(','));
	}
	return accounts.join(';');
}

[[nodiscard]] uint64 SelfKey(not_null<const PeerData*> peer) {
	return peer->session().userPeerId().value;
}

[[nodiscard]] std::vector<uint64> List(not_null<const PeerData*> peer) {
	const auto storage = Parse(AyuSettings::getInstance().lockedChats());
	const auto i = storage.find(SelfKey(peer));
	return (i != end(storage)) ? i->second : std::vector<uint64>();
}

void Write(not_null<const PeerData*> peer, std::vector<uint64> list) {
	auto storage = Parse(AyuSettings::getInstance().lockedChats());
	storage[SelfKey(peer)] = std::move(list);
	AyuSettings::getInstance().setLockedChats(Serialize(storage));
}

[[nodiscard]] QString HashPin(const QString &salt, const QString &pin) {
	return QString::fromLatin1(QCryptographicHash::hash(
		(salt + pin).toUtf8(),
		QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool HasPin() {
	return !AyuSettings::getInstance().chatLockPin().isEmpty();
}

[[nodiscard]] bool CheckPin(const QString &pin) {
	const auto &stored = AyuSettings::getInstance().chatLockPin();
	const auto colon = stored.indexOf(':');
	return (colon > 0)
		&& (HashPin(stored.left(colon), pin) == stored.mid(colon + 1));
}

void SetPin(const QString &pin) {
	const auto salt = QString::number(base::RandomValue<uint64>(), 16);
	AyuSettings::getInstance().setChatLockPin(salt + ':' + HashPin(salt, pin));
}

void SetLocked(not_null<PeerData*> peer, bool locked) {
	auto list = List(peer);
	list.erase(ranges::remove(list, peer->id.value), end(list));
	if (locked) {
		list.push_back(peer->id.value);
	} else {
		UnlockedTill.erase(peer->id.value);
	}
	Write(peer, std::move(list));
	if (const auto history = peer->owner().historyLoaded(peer.get())) {
		history->updateChatListEntry();
	}
}

void ShowPinBox(
		not_null<Window::SessionController*> controller,
		bool create,
		Fn<void()> done) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(create
			? tr::ayu_ChatLockSetPinTitle()
			: tr::ayu_ChatLockEnterPinTitle());
		const auto pin = box->addRow(object_ptr<Ui::PasswordInput>(
			box,
			st::defaultInputField,
			create
				? tr::ayu_ChatLockNewPin()
				: tr::ayu_ChatLockPinPlaceholder()));
		const auto repeat = create
			? box->addRow(object_ptr<Ui::PasswordInput>(
				box,
				st::defaultInputField,
				tr::ayu_ChatLockRepeatPin()))
			: nullptr;
		box->setFocusCallback([=] { pin->setFocusFast(); });

		const auto submit = [=] {
			const auto value = pin->getLastText();
			if (create) {
				if (value.size() < kMinPinLength) {
					pin->showError();
					controller->showToast(
						tr::ayu_ChatLockPinTooShort(tr::now));
					return;
				} else if (repeat->getLastText() != value) {
					repeat->showError();
					controller->showToast(
						tr::ayu_ChatLockPinMismatch(tr::now));
					return;
				}
				SetPin(value);
			} else if (!CheckPin(value)) {
				pin->showError();
				return;
			}
			const auto callback = done;
			box->closeBox();
			if (callback) {
				callback();
			}
		};
		QObject::connect(
			pin,
			&Ui::MaskedInputField::submitted,
			box.get(),
			[=] {
				if (repeat) {
					repeat->setFocusFast();
				} else {
					submit();
				}
			});
		if (repeat) {
			QObject::connect(
				repeat,
				&Ui::MaskedInputField::submitted,
				box.get(),
				submit);
		}
		box->addButton(
			create ? tr::lng_settings_save() : tr::ayu_ChatLockUnlock(),
			submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace

bool IsLockedChat(not_null<const PeerData*> peer) {
	if (AyuSettings::getInstance().lockedChats().isEmpty()) {
		return false;
	}
	return ranges::contains(List(peer), peer->id.value);
}

bool NeedsUnlock(not_null<const PeerData*> peer) {
	if (!IsLockedChat(peer) || !HasPin()) {
		return false;
	}
	const auto i = UnlockedTill.find(peer->id.value);
	return (i == end(UnlockedTill)) || (i->second < crl::now());
}

void RequestUnlock(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		Fn<void()> done) {
	const auto id = peer->id.value;
	ShowPinBox(controller, false, [=] {
		UnlockedTill[id] = crl::now() + kUnlockedFor;
		if (done) {
			done();
		}
	});
}

void AddMenuAction(
		PeerData *peer,
		not_null<Window::SessionController*> controller,
		const Ui::Menu::MenuCallback &addAction) {
	if (!peer || peer->isSelf()) {
		return;
	}
	const auto locked = IsLockedChat(peer);
	addAction({
		.text = (locked
			? tr::ayu_ChatLockRemove(tr::now)
			: tr::ayu_ChatLockAdd(tr::now)),
		.handler = [=] {
			const auto apply = [=](bool value) {
				SetLocked(peer, value);
				controller->showToast(value
					? tr::ayu_ChatLockAdded(tr::now)
					: tr::ayu_ChatLockRemoved(tr::now));
			};
			if (locked) {
				ShowPinBox(controller, false, [=] { apply(false); });
			} else if (!HasPin()) {
				ShowPinBox(controller, true, [=] { apply(true); });
			} else {
				apply(true);
			}
		},
		.icon = &st::menuIconLock,
	});
}

} // namespace AyuFeatures::ChatLock
