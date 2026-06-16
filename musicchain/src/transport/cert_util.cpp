#include "cert_util.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace mc::transport {

CertFiles make_self_signed_files() {
    CertFiles out{};

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!pctx) return out;
    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_group_name(pctx, "P-256") <= 0) {
        EVP_PKEY_CTX_free(pctx); return out;
    }
    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0) { EVP_PKEY_CTX_free(pctx); return out; }
    EVP_PKEY_CTX_free(pctx);

    X509* x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60 * 24 * 365 * 5);
    X509_set_pubkey(x, pkey);

    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                (const uint8_t*)"musicchain", -1, -1, 0);
    X509_set_issuer_name(x, name);
    if (X509_sign(x, pkey, EVP_sha256()) == 0) {
        X509_free(x); EVP_PKEY_free(pkey); return out;
    }

#ifdef _WIN32
    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    out.cert_path = std::string(tmpdir) + "musicchain_" + std::to_string(GetCurrentProcessId()) + ".crt";
    out.key_path  = std::string(tmpdir) + "musicchain_" + std::to_string(GetCurrentProcessId()) + ".key";
#else
    out.cert_path = "/tmp/musicchain_" + std::to_string(getpid()) + ".crt";
    out.key_path  = "/tmp/musicchain_" + std::to_string(getpid()) + ".key";
#endif

    BIO* cb = BIO_new_file(out.cert_path.c_str(), "wb");
    if (!cb) { X509_free(x); EVP_PKEY_free(pkey); return out; }
    PEM_write_bio_X509(cb, x);
    BIO_free(cb);

    BIO* kb = BIO_new_file(out.key_path.c_str(), "wb");
    if (!kb) {
        X509_free(x); EVP_PKEY_free(pkey);
        std::remove(out.cert_path.c_str());
        return out;
    }
    PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BIO_free(kb);

    X509_free(x);
    EVP_PKEY_free(pkey);
    out.ok = true;
    return out;
}

} // namespace mc::transport
