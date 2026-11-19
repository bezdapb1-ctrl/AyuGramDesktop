// Quick save: one menu click puts a photo, video or file straight into the
// downloads folder, without the "Save as" dialog.
#pragma once

class PhotoData;
class DocumentData;
class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace AyuFeatures::QuickSave {

void AddPhotoAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<PhotoData*> photo,
	HistoryItem *item,
	not_null<Window::SessionController*> controller);

void AddDocumentAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<DocumentData*> document,
	HistoryItem *item,
	not_null<Window::SessionController*> controller);

} // namespace AyuFeatures::QuickSave
