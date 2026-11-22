#include "ayu/features/control_center/advanced_search.h"

#include "apiwrap.h"
#include "ayu/data/ayu_database.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

#include <QtCore/QDateTime>

namespace AyuFeatures::AdvancedSearch {
namespace {

constexpr auto kPerPage = 100;
constexpr auto kMaxPages = 3;
constexpr auto kLocalLimit = 300;

struct State {
	not_null<Main::Session*> session;
	Query query;
	PeerData *in = nullptr;
	PeerData *from = nullptr;
	std::vector<Result> results;
	Fn<void(std::vector<Result>, QString)> done;
	int pages = 0;
	QString note;
};

[[nodiscard]] QString Lower(const QString &value) {
	return value.trimmed().toLower();
}

[[nodiscard]] MediaType ParseType(const QString &value) {
	const auto v = Lower(value);
	if (v == u"photo"_q || v == u"фото"_q) {
		return MediaType::Photo;
	} else if (v == u"video"_q || v == u"видео"_q) {
		return MediaType::Video;
	} else if (v == u"file"_q || v == u"doc"_q || v == u"файл"_q) {
		return MediaType::File;
	} else if (v == u"voice"_q || v == u"голосовое"_q) {
		return MediaType::Voice;
	} else if (v == u"round"_q || v == u"кружок"_q) {
		return MediaType::Round;
	} else if (v == u"music"_q || v == u"audio"_q || v == u"музыка"_q) {
		return MediaType::Music;
	} else if (v == u"gif"_q) {
		return MediaType::Gif;
	} else if (v == u"link"_q || v == u"url"_q || v == u"ссылка"_q) {
		return MediaType::Link;
	}
	return MediaType::Any;
}

[[nodiscard]] bool IsTrue(const QString &value) {
	const auto v = Lower(value);
	return v.isEmpty() || v == u"true"_q || v == u"yes"_q || v == u"да"_q;
}

[[nodiscard]] QStringList Tokens(const QString &line) {
	auto result = QStringList();
	auto current = QString();
	auto quoted = false;
	for (const auto ch : line) {
		if (ch == '"') {
			quoted = !quoted;
		} else if (ch.isSpace() && !quoted) {
			if (!current.isEmpty()) {
				result.push_back(current);
				current.clear();
			}
		} else {
			current += ch;
		}
	}
	if (!current.isEmpty()) {
		result.push_back(current);
	}
	return result;
}

[[nodiscard]] MTPMessagesFilter Filter(MediaType type) {
	switch (type) {
	case MediaType::Photo: return MTP_inputMessagesFilterPhotos();
	case MediaType::Video: return MTP_inputMessagesFilterVideo();
	case MediaType::File: return MTP_inputMessagesFilterDocument();
	case MediaType::Voice: return MTP_inputMessagesFilterVoice();
	case MediaType::Round: return MTP_inputMessagesFilterRoundVideo();
	case MediaType::Music: return MTP_inputMessagesFilterMusic();
	case MediaType::Gif: return MTP_inputMessagesFilterGif();
	case MediaType::Link: return MTP_inputMessagesFilterUrl();
	case MediaType::Any: break;
	}
	return MTP_inputMessagesFilterEmpty();
}

[[nodiscard]] TimeId MinDate(const Query &query) {
	return query.after.isValid()
		? TimeId(query.after.startOfDay().toSecsSinceEpoch())
		: TimeId(0);
}

[[nodiscard]] TimeId MaxDate(const Query &query) {
	return query.before.isValid()
		? TimeId(query.before.endOfDay().toSecsSinceEpoch())
		: TimeId(0);
}

[[nodiscard]] QString MediaLabel(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return QString();
	} else if (media->photo()) {
		return u"[фото]"_q;
	} else if (const auto document = media->document()) {
		if (document->isVoiceMessage()) {
			return u"[голосовое]"_q;
		} else if (document->isVideoMessage()) {
			return u"[кружок]"_q;
		} else if (document->sticker()) {
			return u"[стикер]"_q;
		} else if (document->isAnimation()) {
			return u"[GIF]"_q;
		} else if (document->isAudioFile()) {
			return u"[музыка]"_q;
		} else if (document->isVideoFile()) {
			return u"[видео]"_q;
		}
		return u"[файл: "_q + document->filename() + u"]"_q;
	}
	return QString();
}

[[nodiscard]] Result FromItem(not_null<HistoryItem*> item) {
	auto text = item->originalText().text;
	const auto label = MediaLabel(item);
	if (!label.isEmpty()) {
		text = text.isEmpty() ? label : (label + u" "_q + text);
	}
	return {
		.id = item->fullId(),
		.from = item->from()->id,
		.date = item->date(),
		.chat = item->history()->peer->name(),
		.author = item->from()->name(),
		.text = text,
	};
}

[[nodiscard]] std::vector<not_null<HistoryItem*>> ItemsFromResult(
		not_null<Main::Session*> session,
		const MTPmessages_Messages &result) {
	auto &owner = session->data();
	auto list = QVector<MTPMessage>();
	result.match([&](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		owner.processUsers(data.vusers());
		owner.processChats(data.vchats());
		list = data.vmessages().v;
	});
	auto items = std::vector<not_null<HistoryItem*>>();
	for (const auto &message : list) {
		const auto peerId = PeerFromMessage(message);
		if (!owner.peerLoaded(peerId) || !DateFromMessage(message)) {
			continue;
		}
		if (const auto item = owner.addNewMessage(
				message,
				MessageFlags(),
				NewMessageType::Existing)) {
			items.push_back(item);
		}
	}
	return items;
}

void ResolvePeer(
		not_null<Main::Session*> session,
		const QString &value,
		Fn<void(PeerData*)> done) {
	const auto text = value.trimmed();
	if (text.isEmpty()) {
		done(nullptr);
		return;
	}
	const auto byUsername = text.startsWith('@');
	const auto username = byUsername ? text.mid(1) : text;
	if (const auto peer = session->data().peerByUsername(username)) {
		done(peer);
		return;
	}
	if (!byUsername) {
		const auto needle = text.toLower();
		const auto list = session->data().chatsList()->indexed();
		for (const auto &row : list->all()) {
			if (const auto history = row->history()) {
				if (history->peer->name().toLower().contains(needle)) {
					done(history->peer);
					return;
				}
			}
		}
	}
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		session->data().processUsers(result.data().vusers());
		session->data().processChats(result.data().vchats());
		done(session->data().peerLoaded(peerFromMTP(result.data().vpeer())));
	}).fail([=] {
		done(nullptr);
	}).send();
}

[[nodiscard]] QString AuthorName(
		not_null<Main::Session*> session,
		ID fromId) {
	if (const auto user = session->data().userLoaded(UserId(fromId))) {
		return user->name();
	} else if (const auto channel = session->data().channelLoaded(
			ChannelId(fromId))) {
		return channel->name();
	}
	return QString();
}

template <typename Message>
void AppendLocal(
		const std::shared_ptr<State> &state,
		const std::vector<Message> &list,
		const QString &kind) {
	const auto session = state->session;
	for (const auto &message : list) {
		const auto hasMedia = !message.mediaPath.empty()
			|| (message.documentType != 0);
		if (state->query.type != MediaType::Any && !hasMedia) {
			continue;
		}
		const auto peer = getPeerFromDialogId(message.dialogId);
		auto text = QString::fromStdString(message.text);
		if (text.isEmpty() && hasMedia) {
			text = u"[медиа]"_q;
		}
		state->results.push_back({
			.id = FullMsgId(peer ? peer->id : PeerId(), MsgId(message.messageId)),
			.date = TimeId(message.date),
			.chat = peer ? peer->name() : u"Неизвестный чат"_q,
			.author = AuthorName(session, message.fromId),
			.text = text,
			.kind = kind,
			.local = true,
		});
	}
}

void SearchLocal(const std::shared_ptr<State> &state) {
	const auto &query = state->query;
	const auto session = state->session;
	const auto userId = ID(session->userId().bare & PeerId::kChatTypeMask);
	const auto dialogId = state->in ? getDialogIdFromPeer(state->in) : ID(0);
	const auto fromId = state->from
		? ID(state->from->id.value & PeerId::kChatTypeMask)
		: ID(0);
	const auto text = query.text.toStdString();
	if (query.source != Source::Edited) {
		AppendLocal(state, AyuDatabase::searchDeletedMessages(
			userId,
			dialogId,
			fromId,
			MinDate(query),
			MaxDate(query),
			text,
			kLocalLimit), u"Удалено"_q);
	}
	if (query.source != Source::Deleted) {
		AppendLocal(state, AyuDatabase::searchEditedMessages(
			userId,
			dialogId,
			fromId,
			MinDate(query),
			MaxDate(query),
			text,
			kLocalLimit), u"Старая версия"_q);
	}
}

void Finish(const std::shared_ptr<State> &state) {
	if (!state->done) {
		return;
	}
	auto &results = state->results;
	ranges::sort(results, ranges::greater(), &Result::date);
	auto status = u"Найдено: "_q + QString::number(results.size());
	if (!state->note.isEmpty()) {
		status += u". "_q + state->note;
	}
	const auto done = base::take(state->done);
	done(std::move(results), status);
}

void SearchServerPage(
		const std::shared_ptr<State> &state,
		int32 offsetRate,
		PeerData *offsetPeer,
		MsgId offsetId) {
	const auto session = state->session;
	const auto &query = state->query;
	const auto handle = [=](const MTPmessages_Messages &result) {
		auto nextRate = int32(0);
		result.match([&](const MTPDmessages_messagesSlice &data) {
			nextRate = data.vnext_rate().value_or_empty();
		}, [&](const auto &) {
		});
		const auto items = ItemsFromResult(session, result);
		for (const auto &item : items) {
			if (state->from && !state->in && item->from().get() != state->from) {
				continue;
			}
			state->results.push_back(FromItem(item));
		}
		++state->pages;
		if (int(items.size()) >= kPerPage && state->pages < kMaxPages) {
			const auto last = items.back();
			SearchServerPage(state, nextRate, last->history()->peer, last->id);
		} else {
			Finish(state);
		}
	};
	const auto fail = [=](const MTP::Error &error) {
		state->note = (error.type() == u"SEARCH_QUERY_EMPTY"_q)
			? u"Для поиска по серверу нужен текст, тип или автор"_q
			: (u"Сервер ответил ошибкой: "_q + error.type());
		Finish(state);
	};
	if (const auto in = state->in) {
		using Flag = MTPmessages_Search::Flag;
		session->api().request(MTPmessages_Search(
			MTP_flags(state->from ? Flag::f_from_id : Flag()),
			in->input(),
			MTP_string(query.text),
			(state->from ? state->from->input() : MTP_inputPeerEmpty()),
			MTP_inputPeerEmpty(), // saved_peer_id
			MTPVector<MTPReaction>(), // saved_reaction
			MTP_int(0), // top_msg_id
			Filter(query.type),
			MTP_int(MinDate(query)),
			MTP_int(MaxDate(query)),
			MTP_int(offsetId),
			MTP_int(0), // add_offset
			MTP_int(kPerPage),
			MTP_int(0), // max_id
			MTP_int(0), // min_id
			MTP_long(0) // hash
		)).done(handle).fail(fail).send();
	} else {
		session->api().request(MTPmessages_SearchGlobal(
			MTP_flags(0),
			MTP_int(0), // folder_id
			MTPInputChannel(), // community
			MTP_string(query.text),
			Filter(query.type),
			MTP_int(MinDate(query)),
			MTP_int(MaxDate(query)),
			MTP_int(offsetRate),
			(offsetPeer ? offsetPeer->input() : MTP_inputPeerEmpty()),
			MTP_int(offsetId),
			MTP_int(kPerPage)
		)).done(handle).fail(fail).send();
	}
}

} // namespace

Query Parse(const QString &line) {
	auto result = Query();
	auto words = QStringList();
	for (const auto &token : Tokens(line)) {
		const auto colon = token.indexOf(':');
		const auto key = (colon > 0) ? Lower(token.left(colon)) : QString();
		const auto value = (colon > 0) ? token.mid(colon + 1) : QString();
		if (key == u"from"_q || key == u"от"_q) {
			result.from = value;
		} else if (key == u"in"_q || key == u"в"_q) {
			result.in = value;
		} else if (key == u"has"_q || key == u"тип"_q) {
			result.type = ParseType(value);
		} else if (key == u"after"_q || key == u"после"_q) {
			result.after = QDate::fromString(value, Qt::ISODate);
		} else if (key == u"before"_q || key == u"до"_q) {
			result.before = QDate::fromString(value, Qt::ISODate);
		} else if (key == u"deleted"_q || key == u"удалено"_q) {
			if (IsTrue(value)) {
				result.source = Source::Deleted;
			}
		} else if (key == u"edited"_q || key == u"изменено"_q) {
			if (IsTrue(value)) {
				result.source = Source::Edited;
			}
		} else {
			words.push_back(token);
		}
	}
	result.text = words.join(' ');
	return result;
}

void Run(
		not_null<Main::Session*> session,
		Query query,
		Fn<void(std::vector<Result> results, QString status)> done) {
	const auto state = std::make_shared<State>(State{
		.session = session,
		.query = std::move(query),
		.done = std::move(done),
	});
	ResolvePeer(session, state->query.in, [=](PeerData *in) {
		if (!state->query.in.trimmed().isEmpty() && !in) {
			state->note = u"Чат «"_q + state->query.in + u"» не найден"_q;
			Finish(state);
			return;
		}
		state->in = in;
		ResolvePeer(session, state->query.from, [=](PeerData *from) {
			if (!state->query.from.trimmed().isEmpty() && !from) {
				state->note = u"Автор «"_q
					+ state->query.from
					+ u"» не найден"_q;
				Finish(state);
				return;
			}
			state->from = from;
			const auto &q = state->query;
			const auto narrowed = !q.text.isEmpty() || in || from;
			if (q.source != Source::All || narrowed) {
				SearchLocal(state);
			}
			const auto serverPossible = !q.text.isEmpty()
				|| (q.type != MediaType::Any)
				|| (in && from);
			if (q.source != Source::All || !serverPossible) {
				if (q.source == Source::All && !serverPossible) {
					state->note = u"Для поиска по серверу нужен текст, "
						"тип вложения или чат вместе с автором"_q;
				}
				Finish(state);
				return;
			}
			SearchServerPage(state, 0, nullptr, MsgId(0));
		});
	});
}

QString ExportHtml(
		const std::vector<Result> &results,
		const QString &title) {
	auto html = QString();
	html += u"<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\">"_q;
	html += u"<title>"_q + title.toHtmlEscaped() + u"</title><style>"_q;
	html += u"body{background:#0e1621;color:#e9eef4;font-family:"
		"'Segoe UI',Arial,sans-serif;max-width:860px;margin:24px auto;"
		"padding:0 16px}h1{font-size:20px}.m{background:#182533;"
		"border-radius:12px;padding:10px 14px;margin:10px 0}.h{color:#6ab3f3;"
		"font-size:13px;margin-bottom:4px}.k{background:#e55c5c;color:#fff;"
		"border-radius:8px;padding:1px 8px;margin-left:6px;font-size:12px}"
		".t{white-space:pre-wrap;line-height:1.4}</style></head><body>"_q;
	html += u"<h1>"_q + title.toHtmlEscaped() + u"</h1>"_q;
	for (const auto &result : results) {
		const auto when = base::unixtime::parse(result.date).toString(
			u"dd.MM.yyyy HH:mm"_q);
		html += u"<div class=\"m\"><div class=\"h\">"_q
			+ result.chat.toHtmlEscaped();
		if (!result.author.isEmpty()) {
			html += u" · "_q + result.author.toHtmlEscaped();
		}
		html += u" · "_q + when;
		if (!result.kind.isEmpty()) {
			html += u"<span class=\"k\">"_q
				+ result.kind.toHtmlEscaped()
				+ u"</span>"_q;
		}
		html += u"</div><div class=\"t\">"_q
			+ result.text.toHtmlEscaped()
			+ u"</div></div>"_q;
	}
	html += u"</body></html>"_q;
	return html;
}

QString TypeTitle(MediaType type) {
	switch (type) {
	case MediaType::Any: return u"Любой"_q;
	case MediaType::Photo: return u"Фото"_q;
	case MediaType::Video: return u"Видео"_q;
	case MediaType::File: return u"Файлы"_q;
	case MediaType::Voice: return u"Голосовые"_q;
	case MediaType::Round: return u"Кружки"_q;
	case MediaType::Music: return u"Музыка"_q;
	case MediaType::Gif: return u"GIF"_q;
	case MediaType::Link: return u"Ссылки"_q;
	}
	return QString();
}

} // namespace AyuFeatures::AdvancedSearch
