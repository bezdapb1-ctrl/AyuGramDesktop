#include "ayu/features/quick_save/quick_save.h"

#include "base/base_file_utilities.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_download_manager.h"
#include "data/data_file_origin.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace AyuFeatures::QuickSave {
namespace {

[[nodiscard]] QString TargetFolder(not_null<Main::Session*> session) {
	auto path = Core::App().settings().downloadPath();
	if (path.isEmpty() || path == FileDialog::Tmp()) {
		path = File::DefaultDownloadPath(session);
	}
	QDir().mkpath(path);
	return path;
}

[[nodiscard]] QString MaskDangerous(const QString &name) {
#ifdef Q_OS_WIN
	const auto lower = name.trimmed().toLower();
	if (lower.endsWith(u".lnk"_q) || lower.endsWith(u".scf"_q)) {
		return name + u".download"_q;
	}
#endif // Q_OS_WIN
	return name;
}

[[nodiscard]] QString UniquePath(const QString &folder, const QString &name) {
	const auto dir = QDir(folder);
	const auto dot = name.lastIndexOf('.');
	const auto base = (dot > 0) ? name.left(dot) : name;
	const auto extension = (dot > 0) ? name.mid(dot) : QString();
	auto result = dir.absoluteFilePath(name);
	for (auto i = 2; QFileInfo::exists(result); ++i) {
		result = dir.absoluteFilePath(base + u" (%1)"_q.arg(i) + extension);
	}
	return result;
}

[[nodiscard]] QString DocumentPath(
		not_null<DocumentData*> document,
		const QString &folder,
		TimeId date) {
	const auto name = base::FileNameFromUserString(document->filename());
	if (!name.isEmpty()) {
		return UniquePath(folder, MaskDangerous(name));
	}
	const auto suffix = Core::MimeTypeForName(
		document->mimeString()).preferredSuffix();
	const auto prefix = document->isVideoFile()
		? u"video"_q
		: (document->isVoiceMessage() || document->isAudioFile())
		? u"audio"_q
		: u"file"_q;
	return filedialogDefaultName(
		prefix,
		u"."_q + (suffix.isEmpty() ? u"bin"_q : suffix),
		folder,
		false,
		date);
}

void ShowSaved(
		not_null<Window::SessionController*> controller,
		const QString &path) {
	auto text = TextWithEntities{
		tr::ayu_QuickSaveDone(tr::now)
			+ u": "_q
			+ QFileInfo(path).fileName()
			+ u" "_q
	};
	text.append(Ui::Text::Link(tr::ayu_QuickSaveShow(tr::now)));
	controller->showToast({
		.text = std::move(text),
		.filter = [=](const auto &...) {
			File::ShowInFolder(path);
			return false;
		},
	});
}

void ShowFailed(not_null<Window::SessionController*> controller) {
	controller->showToast(tr::ayu_QuickSaveFailed(tr::now));
}

} // namespace

void AddPhotoAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<PhotoData*> photo,
		HistoryItem *item,
		not_null<Window::SessionController*> controller) {
	const auto media = photo->activeMediaView();
	if (photo->isNull() || !media || !media->loaded()) {
		return;
	}
	const auto date = item ? item->date() : TimeId(0);
	menu->addAction(tr::ayu_QuickSave(tr::now), [=] {
		const auto video = !media->videoContent(Data::PhotoSize::Large).isEmpty();
		const auto path = filedialogDefaultName(
			u"photo"_q,
			video ? u".mp4"_q : u".jpg"_q,
			TargetFolder(&photo->session()),
			false,
			date);
		if (media->saveToFile(path)) {
			ShowSaved(controller, path);
		} else {
			ShowFailed(controller);
		}
	}, &st::menuIconDownload);
}

void AddDocumentAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<DocumentData*> document,
		HistoryItem *item,
		not_null<Window::SessionController*> controller) {
	if (document->isNull()
		|| document->forbidsFileSave()
		|| document->loading()) {
		return;
	}
	const auto itemId = item ? item->fullId() : FullMsgId();
	const auto date = item ? item->date() : TimeId(0);
	menu->addAction(tr::ayu_QuickSave(tr::now), [=] {
		const auto path = DocumentPath(
			document,
			TargetFolder(&document->session()),
			date);
		const auto existing = document->filepath(true);
		if (!existing.isEmpty()) {
			if (QFile::copy(existing, path)) {
				ShowSaved(controller, path);
			} else {
				ShowFailed(controller);
			}
			return;
		}
		const auto origin = itemId
			? Data::FileOrigin(itemId)
			: Data::FileOrigin();
		document->save(origin, path);
		if (document->loading()) {
			if (const auto item = document->owner().message(itemId)) {
				Core::App().downloadManager().addLoading({
					.item = item,
					.document = document,
				});
			}
			controller->showToast(tr::ayu_QuickSaveStarted(tr::now)
				+ u": "_q
				+ QFileInfo(path).fileName());
		} else if (QFileInfo::exists(path)) {
			ShowSaved(controller, path);
		} else {
			ShowFailed(controller);
		}
	}, &st::menuIconDownload);
}

} // namespace AyuFeatures::QuickSave
