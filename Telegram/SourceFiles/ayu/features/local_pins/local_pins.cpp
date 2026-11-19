#include "ayu/features/local_pins/local_pins.h"

#include "ayu/ayu_settings.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "main/main_session.h"

#include <map>

namespace AyuFeatures::LocalPins {
namespace {

using Storage = std::map<uint64, std::vector<uint64>>;

// "self:peer,peer;self:peer" - every account keeps its own list.
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

[[nodiscard]] uint64 SelfKey(not_null<const Data::Session*> owner) {
	return owner->session().userPeerId().value;
}

[[nodiscard]] std::vector<uint64> List(
		not_null<const Data::Session*> owner) {
	const auto storage = Parse(
		AyuSettings::getInstance().localPinnedChats());
	const auto i = storage.find(SelfKey(owner));
	return (i != end(storage)) ? i->second : std::vector<uint64>();
}

void Write(
		not_null<const Data::Session*> owner,
		std::vector<uint64> list) {
	auto storage = Parse(AyuSettings::getInstance().localPinnedChats());
	storage[SelfKey(owner)] = std::move(list);
	AyuSettings::getInstance().setLocalPinnedChats(Serialize(storage));
}

[[nodiscard]] bool InMainList(not_null<History*> history) {
	const auto owner = &history->owner();
	return history->folderKnown()
		&& !history->folder()
		&& (owner->chatsListFor(history) == owner->chatsList(nullptr));
}

} // namespace

bool CanPinLocally(not_null<History*> history) {
	return InMainList(history);
}

bool IsLocal(not_null<History*> history) {
	return ranges::contains(
		List(&history->owner()),
		history->peer->id.value);
}

void Toggle(not_null<History*> history, bool pinned) {
	const auto owner = &history->owner();
	const auto id = history->peer->id.value;
	auto list = List(owner);
	list.erase(ranges::remove(list, id), end(list));
	if (pinned) {
		list.push_back(id);
	}
	Write(owner, std::move(list));
	if (InMainList(history)) {
		owner->setChatPinned(history, FilterId(), pinned);
	}
}

void Reapply(not_null<Data::Session*> owner, bool serverListApplied) {
	const auto list = List(owner);
	if (list.empty()) {
		return;
	}
	auto kept = std::vector<uint64>();
	auto added = false;
	for (const auto id : list) {
		const auto history = owner->historyLoaded(PeerId(id));
		if (!history || !InMainList(history)) {
			kept.push_back(id);
			continue;
		}
		if (history->isPinnedDialog(FilterId())) {
			// Pinned by the server list now, so it is not a local pin.
			if (!serverListApplied) {
				kept.push_back(id);
			}
			continue;
		}
		kept.push_back(id);
		owner->setPinnedFromEntryList(history, true);
		added = true;
	}
	if (kept.size() != list.size()) {
		Write(owner, std::move(kept));
	}
	if (added) {
		owner->notifyPinnedDialogsOrderUpdated();
	}
}

int CountPinned(not_null<const Data::Session*> owner) {
	const auto list = List(owner);
	if (list.empty()) {
		return 0;
	}
	auto result = 0;
	for (const auto &key : owner->pinnedChatsOrder(
			static_cast<Data::Folder*>(nullptr))) {
		if (const auto history = key.history()) {
			if (ranges::contains(list, history->peer->id.value)) {
				++result;
			}
		}
	}
	return result;
}

} // namespace AyuFeatures::LocalPins
