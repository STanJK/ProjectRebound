// IntegrityProof: SHA256(embedded_PEM + nonce).
// Identical to ToolBox's util::integrity::prove() — both sides
// bake the same signer.pem and compute the same hash.
//
// Uses Windows BCrypt for SHA-256 (no external deps).

#include "IntegrityProof.h"
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "bcrypt.lib")

namespace IntegrityProof
{

// ---- Embedded PEM — must match res/signer.pem ----

extern const char PUBKEY_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDHDCCAgSgAwIBAgIQcGqQwkirKYJPpnlKYZJLjzANBgkqhkiG9w0BAQsFADAmMSQwIgYDVQQD\n"
    "DBtQcm9qZWN0UmVib3VuZCBDb2RlIFNpZ25pbmcwHhcNMjYwNzMwMDEwMTIyWhcNMzYwNzMwMDEx\n"
    "MTIyWjAmMSQwIgYDVQQDDBtQcm9qZWN0UmVib3VuZCBDb2RlIFNpZ25pbmcwggEiMA0GCSqGSIb3\n"
    "DQEBAQUAA4IBDwAwggEKAoIBAQCmYfzhC7Yc32zTYrPlL5Vv4RmDZt0FwdqL8Y3numsIFcKDr+fJ\n"
    "d5kwMsGAydZopVKAbbb5GuUcxMgZvXqjidtP4WvO6Nc1bbvbhgom/SgNb++oh3jG0flpflA4l7Qi\n"
    "+CVH2d9oCvFAgNzHtuoOM66Ji83SXj9h4+TZWXOPsNQi1DmC3iOei1nPt/sZjWfx56tpnawomg1H\n"
    "vztPApBAdDs5972c/KMm778cWeJcg4ZOMOt5fOTo3Da9UPhs3z79Umj6mex/cIaCLsc+/YzcJ4u1\n"
    "bxgyjifpdml3a1UdxYlV0jV8rl0jnJE2vt/21CdFdiY4poWd63RIhsotmPlSsulVAgMBAAGjRjBE\n"
    "MA4GA1UdDwEB/wQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDAzAdBgNVHQ4EFgQUVrakO15t+F5E\n"
    "Mvv5vhatrThQU/gwDQYJKoZIhvcNAQELBQADggEBAH3yg4zILptX0citH5TSkslgUyBapdyvbAWX\n"
    "EDu87ekttppmmiE0XhIBkIkqccgrl+LabD6Ay+gB95omYhmqEMlZTYMy8OUEpaFbLza/YcCghuP9\n"
    "3bjNzYmYHSDgEWg4I121euOJ4KSe9Sfj8Sb7Nxndtha3GUmm2S5nzlazGsLi4mF6+LfhOhj0HJSu\n"
    "MOkM38CUkYpmB1o3y6SwlhSM6H5eUxcD+X2OKqh7G0xN6XSU9Tv1g3A61OJBhx0G1U2q3OMr6fMe\n"
    "RBAS0nf8PAa1Te1VNk/nizlVYVxMHHa/vrWNqj+V1abOUIUohVc9WNJsgFKeMdEYM/mepCRZ2XYL\n"
    "iSE=\n"
    "-----END CERTIFICATE-----\n";

// ---- BCrypt SHA-256 ----

static std::vector<unsigned char> Sha256(const unsigned char* data, size_t len)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;

    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);

    DWORD hashSize = 0, resultLen = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashSize, sizeof(hashSize), &resultLen, 0);

    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);

    std::vector<unsigned char> pemBytes(PUBKEY_PEM, PUBKEY_PEM + sizeof(PUBKEY_PEM) - 1);
    BCryptHashData(hHash, pemBytes.data(), (ULONG)pemBytes.size(), 0);
    BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0);

    std::vector<unsigned char> hash(hashSize);
    BCryptFinishHash(hHash, hash.data(), hashSize, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return hash;
}

std::string Compute(const std::string_view nonce)
{
    auto hash = Sha256(reinterpret_cast<const unsigned char*>(nonce.data()), nonce.size());

    std::ostringstream oss;
    for (unsigned char byte : hash)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;

    return oss.str();
}

} // namespace IntegrityProof
