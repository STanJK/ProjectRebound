#pragma once
#include <string>
#include <string_view>

// Embedded public key — identical to the ToolBox's signer.pem.
// Both sides compute SHA256(PEM + nonce) → guaranteed match.
namespace IntegrityProof
{
    extern const char PUBKEY_PEM[];
    std::string Compute(const std::string_view nonce);
}
