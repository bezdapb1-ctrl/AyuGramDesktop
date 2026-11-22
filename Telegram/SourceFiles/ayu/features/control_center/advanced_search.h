#pragma once

#include <QtCore/QDate>

namespace Main {
class Session;
} // namespace Main

namespace AyuFeatures::AdvancedSearch {

enum class MediaType {
	Any,
	Photo,
	Video,
	File,
	Voice,
	Round,
	Music,
	Gif,
	Link,
};

enum class Source {
	All,
	Deleted,
	Edited,
};

struct Query {
	QString text;
	QString in;
	QString from;
	MediaType type = MediaType::Any;
	QDate after;
	QDate before;
	Source source = Source::All;
};

struct Result {
	FullMsgId id;
	PeerId from = 0;
	TimeId date = 0;
	QString chat;
	QString author;
	QString text;
	QString kind;
	bool local = false;
};

// Text operators: from:@user in:@chat has:photo after:2026-01-01
// before:2026-02-01 deleted:true edited:true; the rest is the text.
[[nodiscard]] Query Parse(const QString &line);

void Run(
	not_null<Main::Session*> session,
	Query query,
	Fn<void(std::vector<Result> results, QString status)> done);

[[nodiscard]] QString ExportHtml(
	const std::vector<Result> &results,
	const QString &title);

[[nodiscard]] QString TypeTitle(MediaType type);

} // namespace AyuFeatures::AdvancedSearch
