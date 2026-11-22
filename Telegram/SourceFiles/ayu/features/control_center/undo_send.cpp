#include "ayu/features/control_center/undo_send.h"

#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "chat_helpers/message_field.h"
#include "data/data_document.h"
#include "data/data_groups.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_basic.h"
#include "styles/style_chat.h"

#include <map>
#include <set>

namespace AyuFeatures::UndoSend {
namespace {

constexpr auto kRepaintEach = crl::time(200);

struct Pending {
	not_null<History*> history;
	Fn<void()> send;
	crl::time started = 0;
	crl::time deadline = 0;
	std::unique_ptr<base::Timer> timer;
	QRect badge;
};

struct State {
	std::map<FullMsgId, Pending> pending;
	std::set<not_null<Main::Session*>> watched;
	base::Timer repaint;
	rpl::event_stream<Restore> restores;
	std::vector<std::unique_ptr<base::Timer>> finished;
	bool bypass = false;
};

State &Instance() {
	// Leaked on purpose: no timers may be destroyed after the app is gone.
	static const auto result = new State();
	return *result;
}

void RepaintAll() {
	auto &state = Instance();
	for (const auto &[id, entry] : state.pending) {
		if (const auto item = entry.history->owner().message(id)) {
			entry.history->owner().requestItemRepaint(item);
		}
	}
	if (state.pending.empty()) {
		state.repaint.cancel();
	}
}

[[nodiscard]] bool Delayable(not_null<HistoryItem*> item) {
	if (item->isService()
		|| item->viaBot()
		|| item->Has<HistoryMessageForwarded>()) {
		return false;
	}
	const auto media = item->media();
	if (!media || media->webpage()) {
		return true;
	} else if (media->photo()) {
		return true;
	} else if (const auto document = media->document()) {
		// Stickers and GIFs go out right away, as asked.
		return !document->sticker() && !document->isAnimation();
	}
	return false;
}

void Watch(not_null<Main::Session*> session) {
	auto &state = Instance();
	if (!state.watched.emplace(session).second) {
		return;
	}
	session->lifetime().add([=] {
		auto &state = Instance();
		state.watched.erase(session);
		for (auto i = state.pending.begin(); i != state.pending.end();) {
			if (&i->second.history->session() == session) {
				i = state.pending.erase(i);
			} else {
				++i;
			}
		}
	});
}

void Fire(FullMsgId id) {
	auto &state = Instance();
	const auto i = state.pending.find(id);
	if (i == end(state.pending)) {
		return;
	}
	const auto history = i->second.history;
	const auto send = std::move(i->second.send);
	// The timer that called us must outlive this call.
	state.finished.push_back(std::move(i->second.timer));
	state.pending.erase(i);
	crl::on_main([] { Instance().finished.clear(); });
	if (const auto item = history->owner().message(id)) {
		history->owner().requestItemRepaint(item);
	}
	state.bypass = true;
	send();
	state.bypass = false;
}

bool Cancel(FullMsgId id) {
	auto &state = Instance();
	const auto i = state.pending.find(id);
	if (i == end(state.pending)) {
		return false;
	}
	const auto history = i->second.history;
	state.pending.erase(i);

	const auto item = history->owner().message(id);
	if (!item) {
		return true;
	}
	auto items = std::vector<not_null<HistoryItem*>>{ item };
	if (const auto group = history->owner().groups().find(item)) {
		items = { begin(group->items), end(group->items) };
	}
	auto text = TextWithTags();
	for (const auto &one : items) {
		auto prepared = PrepareEditText(one);
		if (!prepared.text.isEmpty()) {
			text = std::move(prepared);
		}
	}
	for (const auto &one : items) {
		one->destroy();
	}
	if (!text.text.isEmpty()) {
		state.restores.fire({ history, std::move(text) });
	}
	return true;
}

} // namespace

bool Intercept(
		not_null<History*> history,
		uint64 randomId,
		Fn<void()> send) {
	auto &state = Instance();
	if (state.bypass) {
		return false;
	}
	const auto seconds = AyuSettings::getInstance().undoSendDelay();
	if (seconds <= 0) {
		return false;
	}
	const auto id = history->owner().messageIdByRandomId(randomId);
	const auto item = id ? history->owner().message(id) : nullptr;
	if (!item || !Delayable(item)) {
		return false;
	}
	Watch(&history->session());
	const auto now = crl::now();
	auto timer = std::make_unique<base::Timer>([=] { Fire(id); });
	timer->callOnce(seconds * crl::time(1000));
	state.pending.insert_or_assign(id, Pending{
		.history = history,
		.send = std::move(send),
		.started = now,
		.deadline = now + seconds * crl::time(1000),
		.timer = std::move(timer),
	});
	if (!state.repaint.isActive()) {
		state.repaint.setCallback(RepaintAll);
		state.repaint.callEach(kRepaintEach);
	}
	history->owner().requestItemRepaint(item);
	return true;
}

bool IsPending(not_null<const HistoryItem*> item) {
	const auto &pending = Instance().pending;
	return !pending.empty() && pending.contains(item->fullId());
}

void PaintBadge(
		QPainter &p,
		not_null<const HistoryView::Element*> view,
		int top) {
	auto &pending = Instance().pending;
	const auto i = pending.find(view->data()->fullId());
	if (i == end(pending)) {
		return;
	}
	const auto left = std::max(
		crl::time(0),
		i->second.deadline - crl::now());
	const auto seconds = int((left + 999) / 1000);
	const auto text = u"Отменить · "_q + QString::number(seconds);
	const auto &font = st::normalFont;
	const auto &padding = st::ayuUndoBadgePadding;
	const auto width = padding.left() + font->width(text) + padding.right();
	const auto height = padding.top() + font->height + padding.bottom();
	const auto inner = view->innerGeometry();
	auto x = inner.x() - width - st::ayuUndoBadgeSkip;
	if (x < st::ayuUndoBadgeSkip) {
		x = inner.x() + st::ayuUndoBadgeSkip;
	}
	const auto y = inner.y() + inner.height() - height;
	const auto rect = QRect(x, y, width, height);

	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::msgServiceBg);
	p.drawRoundedRect(rect, height / 2., height / 2.);
	p.setPen(st::msgServiceFg);
	p.setFont(font);
	p.drawText(rect.marginsRemoved(padding), Qt::AlignCenter, text);

	i->second.badge = rect.translated(0, top);
}

bool HandleClick(not_null<History*> history, QPoint point) {
	auto &pending = Instance().pending;
	for (const auto &[id, entry] : pending) {
		if (entry.history == history && entry.badge.contains(point)) {
			const auto copy = id;
			return Cancel(copy);
		}
	}
	return false;
}

bool CancelLast(not_null<History*> history) {
	auto &pending = Instance().pending;
	auto latest = std::optional<FullMsgId>();
	auto latestStarted = crl::time(0);
	for (const auto &[id, entry] : pending) {
		if (entry.history == history && entry.started >= latestStarted) {
			latest = id;
			latestStarted = entry.started;
		}
	}
	return latest && Cancel(*latest);
}

rpl::producer<Restore> Restores() {
	return Instance().restores.events();
}

} // namespace AyuFeatures::UndoSend
