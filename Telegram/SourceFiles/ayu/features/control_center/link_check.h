#pragma once

namespace AyuFeatures::LinkCheck {

struct Analysis {
	QString host;
	QString shownHost;
	QStringList warnings;
	bool danger = false;
};

// Shows the check window instead of opening an external link right away.
[[nodiscard]] bool Intercept(const QString &url, const QVariant &context);

[[nodiscard]] Analysis Analyze(const QString &url);
[[nodiscard]] QStringList TrustedHosts();
void SetTrustedHosts(const QStringList &hosts);
[[nodiscard]] QStringList DefaultTrustedHosts();
[[nodiscard]] bool IsTrusted(const QString &host);

} // namespace AyuFeatures::LinkCheck
