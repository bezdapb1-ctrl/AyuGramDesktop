#include "ayu/features/chat_stats/chat_stats.h"

#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QDateTime>

#include <array>

namespace AyuFeatures::ChatStats {
namespace {

constexpr auto kMaxChats = 30;
constexpr auto kTopShown = 10;
constexpr auto kSampleLimit = 100;
constexpr auto kRequestDelay = crl::time(300);
constexpr auto kBarsHeight = 110;
constexpr auto kLabelsHeight = 18;

struct ChatResult {
	QString name;
	int total = 0;
	int mine = 0;
};

struct State {
	explicit State(not_null<Main::Session*> session)
	: api(&session->mtp()) {
	}

	MTP::Sender api;
	base::Timer timer;
	std::vector<not_null<PeerData*>> peers;
	std::vector<ChatResult> results;
	std::array<int, 24> hours = {};
	std::array<int, 7> weekdays = {};
	ChatResult current;
	int index = 0;
	int pending = 0;
	bool finished = false;
	rpl::event_stream<> updates;
};

[[nodiscard]] int CountOf(const MTPmessages_Messages &result) {
	return result.match([](const MTPDmessages_messages &data) {
		return int(data.vmessages().v.size());
	}, [](const MTPDmessages_messagesSlice &data) {
		return data.vcount().v;
	}, [](const MTPDmessages_channelMessages &data) {
		return data.vcount().v;
	}, [](const MTPDmessages_messagesNotModified &data) {
		return data.vcount().v;
	});
}

[[nodiscard]] std::vector<TimeId> DatesOf(
		const MTPmessages_Messages &result) {
	auto dates = std::vector<TimeId>();
	result.match([](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		for (const auto &message : data.vmessages().v) {
			message.match([](const MTPDmessageEmpty &) {
			}, [&](const auto &data) {
				dates.push_back(data.vdate().v);
			});
		}
	});
	return dates;
}

[[nodiscard]] std::vector<not_null<PeerData*>> CollectPeers(
		not_null<Main::Session*> session) {
	auto result = std::vector<not_null<PeerData*>>();
	const auto list = session->data().chatsList()->indexed();
	for (const auto &row : list->all()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto peer = history->peer;
		if (peer->isSelf()
			|| peer->isBroadcast()
			|| peer->isRepliesChat()
			|| peer->isVerifyCodes()
			|| peer->isServiceUser()) {
			continue;
		}
		result.push_back(peer);
		if (int(result.size()) >= kMaxChats) {
			break;
		}
	}
	return result;
}

void Step(not_null<State*> state) {
	if (state->index >= int(state->peers.size())) {
		state->finished = true;
		state->updates.fire({});
		return;
	}
	const auto peer = state->peers[state->index];
	state->current = ChatResult{ .name = peer->name() };
	state->pending = 2;
	const auto doneOne = [=] {
		if (--state->pending > 0) {
			return;
		}
		state->results.push_back(state->current);
		++state->index;
		state->updates.fire({});
		state->timer.callOnce(kRequestDelay);
	};

	state->api.request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(0), // offset_id
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(1), // limit
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		state->current.total = CountOf(result);
		doneOne();
	}).fail([=](const MTP::Error &) {
		doneOne();
	}).send();

	using Flag = MTPmessages_Search::Flag;
	state->api.request(MTPmessages_Search(
		MTP_flags(Flag::f_from_id),
		peer->input(),
		MTP_string(),
		MTP_inputPeerSelf(),
		MTPInputPeer(), // saved_peer_id
		MTPVector<MTPReaction>(), // saved_reaction
		MTP_int(0), // top_msg_id
		MTP_inputMessagesFilterEmpty(),
		MTP_int(0), // min_date
		MTP_int(0), // max_date
		MTP_int(0), // offset_id
		MTP_int(0), // add_offset
		MTP_int(kSampleLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		state->current.mine = CountOf(result);
		for (const auto date : DatesOf(result)) {
			const auto local = base::unixtime::parse(date);
			++state->hours[local.time().hour()];
			++state->weekdays[local.date().dayOfWeek() - 1];
		}
		doneOne();
	}).fail([=](const MTP::Error &) {
		doneOne();
	}).send();
}

[[nodiscard]] QString SummaryText(not_null<State*> state) {
	if (!state->finished) {
		return tr::ayu_ChatStatsProgress(
			tr::now,
			lt_done,
			QString::number(state->index),
			lt_total,
			QString::number(state->peers.size()));
	}
	auto results = state->results;
	std::stable_sort(begin(results), end(results), [](
			const ChatResult &a,
			const ChatResult &b) {
		return a.total > b.total;
	});
	auto total = 0;
	auto mine = 0;
	for (const auto &one : results) {
		total += one.total;
		mine += one.mine;
	}
	const auto percent = total ? (mine * 100 / total) : 0;
	auto lines = QStringList();
	lines.push_back(tr::ayu_ChatStatsTotal(
		tr::now,
		lt_chats,
		QString::number(results.size()),
		lt_messages,
		QString::number(total),
		lt_mine,
		QString::number(mine),
		lt_percent,
		QString::number(percent)));
	lines.push_back(QString());
	lines.push_back(tr::ayu_ChatStatsTop(tr::now));
	const auto shown = std::min(int(results.size()), kTopShown);
	for (auto i = 0; i != shown; ++i) {
		const auto &one = results[i];
		lines.push_back(u"%1. %2 — %3 (%4)"_q
			.arg(i + 1)
			.arg(one.name)
			.arg(one.total)
			.arg(tr::ayu_ChatStatsMine(
				tr::now,
				lt_mine,
				QString::number(one.mine))));
	}
	return lines.join('\n');
}

void AddBars(
		not_null<Ui::VerticalLayout*> container,
		not_null<State*> state,
		bool hours) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			(hours
				? tr::ayu_ChatStatsHours(tr::now)
				: tr::ayu_ChatStatsWeekdays(tr::now)),
			st::boxLabel),
		st::boxRowPadding);
	const auto widget = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding);
	widget->resize(
		st::boxWideWidth,
		style::ConvertScale(kBarsHeight + kLabelsHeight));
	const auto names = tr::ayu_ChatStatsDayNames(tr::now).split(',');

	widget->paintRequest() | rpl::on_next([=] {
		const auto barsHeight = style::ConvertScale(kBarsHeight);
		const auto labelsHeight = style::ConvertScale(kLabelsHeight);
		auto p = QPainter(widget);
		auto values = std::vector<int>();
		if (hours) {
			values.assign(begin(state->hours), end(state->hours));
		} else {
			values.assign(begin(state->weekdays), end(state->weekdays));
		}
		const auto maximum = std::max(
			1,
			*std::max_element(begin(values), end(values)));
		const auto count = int(values.size());
		const auto width = widget->width();
		const auto slot = double(width) / count;
		const auto bar = std::max(double(style::ConvertScale(2)), slot * 0.7);

		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgActive);
		for (auto i = 0; i != count; ++i) {
			const auto height = int(barsHeight * values[i] / maximum);
			const auto x = slot * i + (slot - bar) / 2.;
			p.drawRect(QRectF(x, barsHeight - height, bar, height));
		}

		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		for (auto i = 0; i != count; ++i) {
			const auto label = hours
				? ((i % 6 == 0) ? QString::number(i) : QString())
				: names.value(i);
			if (label.isEmpty()) {
				continue;
			}
			const auto rect = QRectF(
				slot * i,
				barsHeight,
				hours ? slot * 3 : slot,
				labelsHeight);
			p.drawText(
				rect,
				label,
				QTextOption(hours ? style::al_left : style::al_top));
		}
	}, widget->lifetime());

	state->updates.events() | rpl::on_next([=] {
		widget->update();
	}, widget->lifetime());
}

} // namespace

void ShowBox(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_ChatStatsTitle());
		box->setWidth(st::boxWideWidth);

		const auto state = box->lifetime().make_state<State>(session);
		state->peers = CollectPeers(session);
		state->timer.setCallback([=] { Step(state); });

		const auto summary = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			SummaryText(state),
			st::boxLabel));
		state->updates.events() | rpl::on_next([=] {
			summary->setText(SummaryText(state));
		}, summary->lifetime());

		const auto content = box->verticalLayout();
		AddBars(content, state, true);
		AddBars(content, state, false);

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		Step(state);
	}));
}

} // namespace AyuFeatures::ChatStats
