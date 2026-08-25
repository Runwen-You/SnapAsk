#include "platform/windows/CredentialStore.h"

#include <QUuid>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincred.h>

#include <limits>
#include <vector>

namespace snapask::platform::windows {
namespace {

class ScopedCredential final {
public:
    ~ScopedCredential()
    {
        if (value_) {
            if (value_->CredentialBlob != nullptr
                && value_->CredentialBlobSize > 0) {
                SecureZeroMemory(
                    value_->CredentialBlob,
                    static_cast<SIZE_T>(value_->CredentialBlobSize));
            }
            CredFree(value_);
        }
    }
    PCREDENTIALW* out() { return &value_; }
    PCREDENTIALW get() const { return value_; }

private:
    PCREDENTIALW value_ = nullptr;
};

QString systemErrorMessage(const DWORD code)
{
    switch (code) {
    case ERROR_NOT_FOUND: return QStringLiteral("未找到系统凭据");
    case ERROR_NO_SUCH_LOGON_SESSION: return QStringLiteral("当前登录会话无法访问系统凭据");
    case ERROR_BAD_USERNAME: return QStringLiteral("系统拒绝了凭据名称");
    default: return QStringLiteral("Windows Credential Manager 操作失败（%1）").arg(code);
    }
}

} // namespace

bool CredentialStore::write(
    const QString& credentialRef,
    const QString& secret,
    QString* error) const
{
    if (!isValidReference(credentialRef)) {
        if (error) *error = QStringLiteral("凭据引用无效");
        return false;
    }
    if (secret.isEmpty()) {
        if (error) *error = QStringLiteral("API Key 不能为空");
        return false;
    }

    const auto byteCount = static_cast<size_t>(secret.size()) * sizeof(wchar_t);
    if (byteCount > CRED_MAX_CREDENTIAL_BLOB_SIZE
        || byteCount > std::numeric_limits<DWORD>::max()) {
        if (error) *error = QStringLiteral("API Key 超过系统凭据长度限制");
        return false;
    }
    std::vector<wchar_t> secretBuffer(static_cast<size_t>(secret.size()) + 1U, L'\0');
    secret.toWCharArray(secretBuffer.data());

    auto mutableTarget = credentialRef.toStdWString();
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = mutableTarget.data();
    credential.CredentialBlobSize = static_cast<DWORD>(byteCount);
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(secretBuffer.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"SnapAsk API credential");

    const BOOL succeeded = CredWriteW(&credential, 0);
    const DWORD systemError = succeeded ? ERROR_SUCCESS : GetLastError();
    SecureZeroMemory(secretBuffer.data(), secretBuffer.size() * sizeof(wchar_t));
    if (!succeeded) {
        if (error) *error = systemErrorMessage(systemError);
        return false;
    }
    return true;
}

std::optional<QString> CredentialStore::read(
    const QString& credentialRef,
    QString* error) const
{
    if (!isValidReference(credentialRef)) {
        if (error) *error = QStringLiteral("凭据引用无效");
        return std::nullopt;
    }
    ScopedCredential credential;
    const auto target = credentialRef.toStdWString();
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, credential.out())) {
        if (error) *error = systemErrorMessage(GetLastError());
        return std::nullopt;
    }
    const auto* value = credential.get();
    if (value->CredentialBlobSize % sizeof(wchar_t) != 0) {
        if (error) *error = QStringLiteral("系统凭据格式无效");
        return std::nullopt;
    }
    const auto length = static_cast<qsizetype>(value->CredentialBlobSize / sizeof(wchar_t));
    return QString::fromWCharArray(
        reinterpret_cast<const wchar_t*>(value->CredentialBlob), length);
}

bool CredentialStore::contains(
    const QString& credentialRef,
    QString* error) const
{
    if (!isValidReference(credentialRef)) {
        if (error) *error = QStringLiteral("凭据引用无效");
        return false;
    }

    ScopedCredential credential;
    const auto target = credentialRef.toStdWString();
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, credential.out())) {
        return true;
    }

    const DWORD code = GetLastError();
    if (code != ERROR_NOT_FOUND && error) {
        *error = systemErrorMessage(code);
    }
    return false;
}

bool CredentialStore::remove(const QString& credentialRef, QString* error) const
{
    if (!isValidReference(credentialRef)) {
        if (error) *error = QStringLiteral("凭据引用无效");
        return false;
    }
    const auto target = credentialRef.toStdWString();
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    const auto code = GetLastError();
    if (code == ERROR_NOT_FOUND) return true;
    if (error) *error = systemErrorMessage(code);
    return false;
}

bool CredentialStore::isValidReference(const QString& credentialRef)
{
    if (!credentialRef.startsWith(QStringLiteral("SnapAsk/provider/"))) return false;
    const auto suffix = credentialRef.sliced(QStringLiteral("SnapAsk/provider/").size());
    const QUuid id(suffix);
    return !id.isNull() && suffix.size() <= 64;
}

} // namespace snapask::platform::windows
