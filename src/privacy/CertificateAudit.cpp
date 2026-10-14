#include "privacy/CertificateAudit.hpp"

#include <algorithm>

namespace aurora::privacy {

CertificateObservation CertificateAudit::observe(
    std::string_view origin, std::string_view sha256,
    const core::IncognitoPolicy& policy, bool trusted, bool explicit_user_accept) {
    using Result = CertificateObservation;
    // Origins here have already been parsed by Chromium. Defend against
    // accidental path/userinfo/opaque keys and malformed fingerprint callers.
    if (!origin.starts_with("https://") || origin.size() <= 8 ||
        origin.find_first_of("/?#@", 8) != std::string_view::npos ||
        sha256.size() != 64 ||
        !std::all_of(sha256.begin(), sha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) return Result::Ignored;

    bool exists = false;
    std::string previous;
    {
        auto stmt = db_.prepare(
            "SELECT sha256 FROM certificate_audit WHERE origin = ?1;");
        if (!stmt || stmt->bind(1, origin) != storage::DbError::Ok)
            return Result::StorageError;
        const auto row = stmt->step();
        if (!row) return Result::StorageError;
        exists = *row;
        if (exists) previous = stmt->column_text(0);
    }
    if (exists && previous == sha256) return Result::Unchanged;
    if (exists && !explicit_user_accept) return Result::Changed;
    if (!exists && !trusted && !explicit_user_accept) return Result::Ignored;
    if (!policy.may_write_certificate_audit())
        return explicit_user_accept ? Result::Accepted : Result::Ignored;

    auto stmt = db_.prepare(
        "INSERT INTO certificate_audit(origin, sha256) VALUES (?1, ?2) "
        "ON CONFLICT(origin) DO UPDATE SET sha256 = excluded.sha256;");
    if (!stmt || stmt->bind(1, origin) != storage::DbError::Ok ||
        stmt->bind(2, sha256) != storage::DbError::Ok)
        return Result::StorageError;
    const auto done = stmt->step();
    if (!done || *done) return Result::StorageError;
    return explicit_user_accept ? Result::Accepted : Result::Recorded;
}

} // namespace aurora::privacy
