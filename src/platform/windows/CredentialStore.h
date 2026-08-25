#pragma once

#include <QString>
#include <optional>

namespace snapask::platform::windows {

class CredentialStore final {
public:
    bool write(const QString& credentialRef, const QString& secret, QString* error = nullptr) const;
    [[nodiscard]] std::optional<QString> read(
        const QString& credentialRef,
        QString* error = nullptr) const;
    [[nodiscard]] bool contains(
        const QString& credentialRef,
        QString* error = nullptr) const;
    bool remove(const QString& credentialRef, QString* error = nullptr) const;

    static bool isValidReference(const QString& credentialRef);
};

} // namespace snapask::platform::windows
