#pragma once

#include <QtCore/QString>

#include <vector>

namespace Ui {
class InputField;
} // namespace Ui

namespace AyuFeatures::Templates {

struct Template {
	QString key;
	QString text;
};

[[nodiscard]] std::vector<Template> Parse(const QString &raw);

void Attach(not_null<Ui::InputField*> field);

} // namespace AyuFeatures::Templates
