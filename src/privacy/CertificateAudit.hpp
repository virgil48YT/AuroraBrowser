// Aurora — minimal trust-on-first-visit certificate-change alarm.
#pragma once

#include <string_view>

#include "core/IncognitoPolicy.hpp"
#include "storage/Database.hpp"

namespace aurora::privacy {

enum class CertificateObservation {
    Ignored, Recorded, Unchanged, Changed, Accepted, StorageError
};

// Synchronous, UI-thread-only, like AdblockService. The CEF caller supplies a
// Chromium-canonicalized HTTPS origin and the SHA-256 of the DER leaf cert.
class CertificateAudit {
public:
    explicit CertificateAudit(storage::Database& db) : db_(db) {}

    // Invalid/error certificates may be compared but never seed a first pin.
    // Only a deliberate user acceptance may replace a known fingerprint.
    // Private windows read existing pins but never insert or replace them.
    CertificateObservation observe(std::string_view origin,
                                   std::string_view sha256,
                                   const core::IncognitoPolicy& policy = {},
                                   bool trusted = true,
                                   bool explicit_user_accept = false);

private:
    storage::Database& db_;
};

} // namespace aurora::privacy
