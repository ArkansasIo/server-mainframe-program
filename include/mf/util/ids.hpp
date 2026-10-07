// ---------------------------------------------------------------------------
// Identifier helpers: sortable, readable ids in a mainframe-ish style.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>

namespace mf::ids
{

    // <PREFIX>-<base36 time>-<random hex>
    std::string make(std::string_view prefix, int randomBytes = 4);

    std::string job();         // JOB-...
    std::string transaction(); // TXN-...
    std::string session();     // SES-...
    std::string correlation(); // COR-...
    std::string uuid();        // RFC 4122 v4

    // Random hex string of `bytes` bytes (2 chars each).
    std::string randomHex(int bytes);

    // Cryptographically secure random bytes.
    std::string randomBytes(int count);

} // namespace mf::ids
