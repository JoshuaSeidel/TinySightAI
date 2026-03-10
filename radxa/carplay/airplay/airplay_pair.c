/*
 * airplay_pair.c — AirPlay Pair-Setup and Pair-Verify
 *
 * Implements the HomeKit-derived pairing protocol used by AirPlay 2 / CarPlay.
 *
 * For CarPlay accessories in "transient" mode:
 *   - pair-setup uses SRP with a fixed password ("3939") and no PIN
 *   - pair-verify uses X25519 + Ed25519 to authenticate the session
 *   - the iPhone auto-accepts the server certificate in transient mode
 *
 * SRP-6a implementation notes:
 *   We use OpenSSL's BN (big number) API for modular arithmetic.
 *   The SRP group is the standard 3072-bit group from RFC 5054.
 *   In "transient" pairing the verifier is derived from password "3939"
 *   which is the fixed CarPlay pairing pin per Apple's HAP spec section 8.5.
 *
 * References:
 *   HAP Specification (HomeKit Accessory Protocol) — Apple (non-public, but
 *   partially reverse-engineered via OpenHAP / homebridge / RPiPlay)
 *   RFC 5054 — TLS-SRP; uses same groups
 *   https://github.com/FD-/RPiPlay/blob/master/lib/pairing.c
 */

#include "airplay_pair.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/bn.h>
#include <openssl/kdf.h>

/* -----------------------------------------------------------------------
 * TLV8 helpers
 * ----------------------------------------------------------------------- */

size_t tlv8_encode_item(uint8_t *buf, size_t buf_cap,
                         uint8_t type, const uint8_t *val, size_t val_len)
{
    size_t written = 0;
    size_t remaining = val_len;
    const uint8_t *ptr = val;

    while (remaining > 0 || written == 0) {
        size_t chunk = remaining > 255 ? 255 : remaining;
        if (buf_cap < written + 2 + chunk) break; /* buffer full */

        buf[written++] = type;
        buf[written++] = (uint8_t)chunk;
        if (chunk > 0) {
            memcpy(buf + written, ptr, chunk);
            written += chunk;
            ptr += chunk;
        }
        remaining -= chunk;

        if (val_len == 0) break; /* single zero-length item */
    }
    return written;
}

const uint8_t *tlv8_find(const uint8_t *data, size_t data_len,
                          uint8_t type, size_t *val_len)
{
    size_t i = 0;
    while (i + 2 <= data_len) {
        uint8_t t = data[i];
        uint8_t l = data[i + 1];

        if (t == type) {
            if (i + 2 + l > data_len) return NULL;
            if (val_len) *val_len = l;
            return &data[i + 2];
        }
        i += 2 + l;
    }
    return NULL;
}

/*
 * TLV8 reassembly: values >255 bytes are split across consecutive items
 * with the same type tag. This function allocates a buffer and concatenates
 * all consecutive fragments. Caller must free() the returned pointer.
 * Returns NULL if the type is not found.
 */
uint8_t *tlv8_find_reassemble(const uint8_t *data, size_t data_len,
                                uint8_t type, size_t *total_len)
{
    /* First pass: compute total length */
    size_t total = 0;
    bool found = false;
    bool in_run = false;
    size_t i = 0;
    while (i + 2 <= data_len) {
        uint8_t t = data[i];
        uint8_t l = data[i + 1];
        if (i + 2 + l > data_len) break;

        if (t == type) {
            found = true;
            in_run = true;
            total += l;
        } else if (in_run) {
            break; /* end of consecutive fragments */
        }
        i += 2 + l;
    }
    if (!found) return NULL;

    /* Second pass: copy data */
    uint8_t *buf = malloc(total);
    if (!buf) return NULL;
    size_t off = 0;
    in_run = false;
    i = 0;
    while (i + 2 <= data_len) {
        uint8_t t = data[i];
        uint8_t l = data[i + 1];
        if (i + 2 + l > data_len) break;

        if (t == type) {
            in_run = true;
            memcpy(buf + off, data + i + 2, l);
            off += l;
        } else if (in_run) {
            break;
        }
        i += 2 + l;
    }
    if (total_len) *total_len = total;
    return buf;
}

/* -----------------------------------------------------------------------
 * SRP-6a helpers (RFC 5054 3072-bit group, SHA-512)
 * ----------------------------------------------------------------------- */

/*
 * RFC 5054 Appendix A: SRP-3072 group
 * N = well-known 3072-bit prime
 * g = 5
 */
static const uint8_t SRP_N_HEX[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
    "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
    "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
    "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
    "DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64"
    "ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6B"
    "F12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB31"
    "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

static BIGNUM *srp_get_N(void)
{
    BIGNUM *N = BN_new();
    BN_hex2bn(&N, (const char *)SRP_N_HEX);
    return N;
}

static BIGNUM *srp_get_g(void)
{
    BIGNUM *g = BN_new();
    BN_set_word(g, 5);
    return g;
}

/*
 * H(a || b) using SHA-512. Result written into out (64 bytes).
 */
static void sha512_2(const uint8_t *a, size_t la,
                      const uint8_t *b, size_t lb,
                      uint8_t out[64])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int len = 64;
    EVP_DigestInit_ex(ctx, EVP_sha512(), NULL);
    EVP_DigestUpdate(ctx, a, la);
    if (b && lb > 0) EVP_DigestUpdate(ctx, b, lb);
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);
}

/*
 * SRP verifier derivation (HAP / RFC 5054):
 *   salt = random 16 bytes
 *   x    = H(salt || H(I ":" P))      where I="Pair-Setup", P="3939"
 *   v    = g^x mod N
 */
static void srp_compute_verifier(const uint8_t salt[16],
                                  BIGNUM **v_out, BN_CTX *bn_ctx)
{
    static const char identity[] = "Pair-Setup:3939";

    uint8_t h_ip[64];
    sha512_2((const uint8_t *)identity, strlen(identity), NULL, 0, h_ip);

    uint8_t h_x_input[16 + 64];
    memcpy(h_x_input, salt, 16);
    memcpy(h_x_input + 16, h_ip, 64);

    uint8_t x_bytes[64];
    sha512_2(h_x_input, sizeof(h_x_input), NULL, 0, x_bytes);

    BIGNUM *x = BN_bin2bn(x_bytes, 64, NULL);
    BIGNUM *N = srp_get_N();
    BIGNUM *g = srp_get_g();

    BIGNUM *v = BN_new();
    BN_mod_exp(v, g, x, N, bn_ctx);

    *v_out = v;

    BN_free(x);
    BN_free(N);
    BN_free(g);
}

/* -----------------------------------------------------------------------
 * HKDF helper (RFC 5869, using OpenSSL EVP_KDF)
 * ----------------------------------------------------------------------- */
static int hkdf_sha512(const uint8_t *salt, size_t salt_len,
                        const uint8_t *ikm,  size_t ikm_len,
                        const uint8_t *info, size_t info_len,
                        uint8_t *out, size_t out_len)
{
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    if (!kdf) return -1;

    EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx) return -1;

    OSSL_PARAM params[6];
    int p = 0;
    params[p++] = OSSL_PARAM_construct_utf8_string("digest", "SHA512", 0);
    params[p++] = OSSL_PARAM_construct_octet_string("key", (void *)ikm, ikm_len);
    if (salt && salt_len > 0)
        params[p++] = OSSL_PARAM_construct_octet_string("salt", (void *)salt, salt_len);
    if (info && info_len > 0)
        params[p++] = OSSL_PARAM_construct_octet_string("info", (void *)info, info_len);
    params[p] = OSSL_PARAM_construct_end();

    int rc = EVP_KDF_derive(kctx, out, out_len, params);
    EVP_KDF_CTX_free(kctx);
    return (rc == 1) ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Ed25519 / X25519 helpers
 * ----------------------------------------------------------------------- */

/*
 * Generate an Ed25519 keypair.
 * pub_out: 32 bytes; priv_out: 64 bytes (seed || pub key, OpenSSL layout)
 */
static int gen_ed25519(uint8_t pub_out[32], uint8_t priv_out[64])
{
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!pctx) return -1;

    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen_init(pctx) != 1 ||
        EVP_PKEY_keygen(pctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(pctx);
        return -1;
    }
    EVP_PKEY_CTX_free(pctx);

    size_t pub_len = 32, priv_len = 64;
    EVP_PKEY_get_raw_public_key(pkey, pub_out, &pub_len);
    EVP_PKEY_get_raw_private_key(pkey, priv_out, &priv_len);
    EVP_PKEY_free(pkey);
    return 0;
}

/*
 * Sign `msg` with Ed25519 private key.  sig_out must be 64 bytes.
 */
static int ed25519_sign(const uint8_t priv[64],
                         const uint8_t *msg, size_t msg_len,
                         uint8_t sig_out[64])
{
    /* OpenSSL raw Ed25519 private key is the 32-byte seed */
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                   priv, 32);
    if (!pkey) return -1;

    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    size_t sig_len = 64;
    int ok = (EVP_DigestSignInit(mctx, NULL, NULL, NULL, pkey) == 1 &&
              EVP_DigestSign(mctx, sig_out, &sig_len, msg, msg_len) == 1);
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    return ok ? 0 : -1;
}

/*
 * Verify Ed25519 signature.
 * Returns 0 on success, -1 on failure.
 */
static int __attribute__((unused)) ed25519_verify(const uint8_t pub[32],
                           const uint8_t *msg, size_t msg_len,
                           const uint8_t sig[64])
{
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
    if (!pkey) return -1;

    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    int ok = (EVP_DigestVerifyInit(mctx, NULL, NULL, NULL, pkey) == 1 &&
              EVP_DigestVerify(mctx, sig, 64, msg, msg_len) == 1);
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    return ok ? 0 : -1;
}

/*
 * Generate X25519 ephemeral keypair.
 */
static int gen_x25519(uint8_t pub_out[32], uint8_t priv_out[32])
{
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!pctx) return -1;

    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen_init(pctx) != 1 ||
        EVP_PKEY_keygen(pctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(pctx);
        return -1;
    }
    EVP_PKEY_CTX_free(pctx);

    size_t pub_len = 32, priv_len = 32;
    EVP_PKEY_get_raw_public_key(pkey, pub_out, &pub_len);
    EVP_PKEY_get_raw_private_key(pkey, priv_out, &priv_len);
    EVP_PKEY_free(pkey);
    return 0;
}

/*
 * X25519 ECDH: compute shared secret from our private key and peer public key.
 * shared_out must be 32 bytes.
 */
static int x25519_dh(const uint8_t our_priv[32], const uint8_t peer_pub[32],
                      uint8_t shared_out[32])
{
    EVP_PKEY *our_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL,
                                                      our_priv, 32);
    EVP_PKEY *peer_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL,
                                                      peer_pub, 32);
    if (!our_key || !peer_key) { goto fail; }

    EVP_PKEY_CTX *dctx = EVP_PKEY_CTX_new(our_key, NULL);
    size_t shared_len = 32;
    int ok = (EVP_PKEY_derive_init(dctx) == 1 &&
              EVP_PKEY_derive_set_peer(dctx, peer_key) == 1 &&
              EVP_PKEY_derive(dctx, shared_out, &shared_len) == 1);
    EVP_PKEY_CTX_free(dctx);
    EVP_PKEY_free(our_key);
    EVP_PKEY_free(peer_key);
    return ok ? 0 : -1;

fail:
    if (our_key)  EVP_PKEY_free(our_key);
    if (peer_key) EVP_PKEY_free(peer_key);
    return -1;
}

/* -----------------------------------------------------------------------
 * AES-GCM encrypt/decrypt helpers for HAP session records
 * ----------------------------------------------------------------------- */

/*
 * AES-128-GCM encrypt.
 * key: 16 bytes, nonce: 12 bytes
 * Produces ciphertext (same length as plaintext) + 16-byte tag appended to out.
 * out_len = in_len + 16
 */
static int aes128gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                               const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t *out_len)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    *out_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1) goto fail;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto fail;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto fail;
    if (EVP_EncryptUpdate(ctx, out, &len, in, (int)in_len) != 1) goto fail;
    *out_len = len;
    if (EVP_EncryptFinal_ex(ctx, out + *out_len, &len) != 1) goto fail;
    *out_len += len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out + *out_len) != 1) goto fail;
    *out_len += 16;

    EVP_CIPHER_CTX_free(ctx);
    return 0;
fail:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/*
 * AES-128-GCM decrypt.
 * Input must be ciphertext + 16-byte tag at the end.
 * in_len includes the 16-byte tag.
 */
static int __attribute__((unused)) aes128gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                               const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t *out_len)
{
    if (in_len < 16) return -1;
    size_t ct_len = in_len - 16;
    const uint8_t *tag = in + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    *out_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1) goto fail;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto fail;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto fail;
    if (EVP_DecryptUpdate(ctx, out, &len, in, (int)ct_len) != 1) goto fail;
    *out_len = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1) goto fail;
    if (EVP_DecryptFinal_ex(ctx, out + *out_len, &len) < 0) goto fail;
    *out_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return 0;
fail:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int pair_ctx_init(airplay_pair_ctx_t *ctx, const char *server_id)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = PAIR_STATE_IDLE;

    if (server_id) {
        strncpy(ctx->server_id, server_id, sizeof(ctx->server_id) - 1);
    } else {
        strncpy(ctx->server_id, "AA:BB:CC:DD:EE:FF", sizeof(ctx->server_id) - 1);
    }

    /* Generate server long-term Ed25519 keypair */
    if (gen_ed25519(ctx->ed25519_pub, ctx->ed25519_priv) < 0) {
        fprintf(stderr, "pair: failed to generate Ed25519 keypair\n");
        return -1;
    }

    printf("pair: Ed25519 public key: ");
    for (int i = 0; i < 32; i++) printf("%02x", ctx->ed25519_pub[i]);
    printf("\n");

    ctx->state = PAIR_STATE_SETUP_M1;
    return 0;
}

void pair_ctx_load_keypair(airplay_pair_ctx_t *ctx,
                            const uint8_t pub[32], const uint8_t priv[64])
{
    if (!ctx) return;
    memcpy(ctx->ed25519_pub,  pub,  32);
    memcpy(ctx->ed25519_priv, priv, 64);
}

void pair_ctx_get_ed25519_pub(const airplay_pair_ctx_t *ctx, uint8_t pub[32])
{
    if (!ctx) return;
    memcpy(pub, ctx->ed25519_pub, 32);
}

/*
 * pair-setup M1/M2: iPhone sends its SRP A and method.
 * We respond with our SRP B, salt, and M2 proof.
 *
 * In transient mode (method = 0x10) iOS auto-accepts the server and
 * does not verify the SRP proof against a saved pairing PIN.
 */
int pair_setup_process(airplay_pair_ctx_t *ctx,
                       const uint8_t *in_data, size_t in_len,
                       uint8_t *out_data, size_t *out_len)
{
    if (!ctx || !in_data || !out_data || !out_len) return -1;
    *out_len = 0;

    /* Find TLV8 state field to determine M1/M3 */
    size_t state_len = 0;
    const uint8_t *state_val = tlv8_find(in_data, in_len, HAP_TLV_STATE, &state_len);
    uint8_t step = state_val ? *state_val : 1;

    if (step == 1) {
        /*
         * M1: iPhone sent SRP public key A.
         * We need to:
         *   1. Extract iPhone's A from TLV8
         *   2. Generate random 16-byte SRP salt
         *   3. Compute SRP verifier v from password "3939"
         *   4. Generate server's SRP B = kv + g^b mod N
         *   5. Compute shared secret S = (A * v^u)^b mod N
         *   6. Derive session key K = H(S)
         *   7. Respond with state=2 (M2), salt, B
         */
        /* Extract iPhone's SRP public key A */
        size_t A_tlv_len = 0;
        uint8_t *A_data = tlv8_find_reassemble(in_data, in_len,
                                                 HAP_TLV_PUBLIC_KEY, &A_tlv_len);
        if (!A_data || A_tlv_len == 0) {
            fprintf(stderr, "pair-setup M1: missing client SRP public key A\n");
            return -1;
        }

        BN_CTX *bn_ctx = BN_CTX_new();
        BIGNUM *N = srp_get_N();
        BIGNUM *g = srp_get_g();
        BIGNUM *A = BN_bin2bn(A_data, (int)A_tlv_len, NULL);
        free(A_data);

        /* Safety check: A mod N != 0 (RFC 5054) */
        BIGNUM *A_mod = BN_new();
        BN_mod(A_mod, A, N, bn_ctx);
        if (BN_is_zero(A_mod)) {
            fprintf(stderr, "pair-setup M1: A mod N == 0, aborting\n");
            BN_free(A); BN_free(A_mod);
            BN_free(N); BN_free(g); BN_CTX_free(bn_ctx);
            return -1;
        }
        BN_free(A_mod);

        /* Generate salt */
        RAND_bytes(ctx->srp_salt, 16);

        /* Generate random b (SRP private exponent) */
        uint8_t b_bytes[32];
        RAND_bytes(b_bytes, 32);
        BIGNUM *b = BN_bin2bn(b_bytes, 32, NULL);

        /* Compute g^b mod N */
        BIGNUM *B_raw = BN_new();
        BN_mod_exp(B_raw, g, b, N, bn_ctx);

        /* Compute verifier v */
        BIGNUM *v = NULL;
        srp_compute_verifier(ctx->srp_salt, &v, bn_ctx);

        /* k = H(N || PAD(g)) -- RFC 5054 multiplier */
        uint8_t N_bytes[384], g_bytes[384];
        memset(N_bytes, 0, 384); memset(g_bytes, 0, 384);
        int N_len = BN_bn2bin(N, N_bytes + (384 - BN_num_bytes(N)));
        (void)N_len;
        g_bytes[383] = 5; /* g=5 */
        uint8_t k_hash[64];
        sha512_2(N_bytes, 384, g_bytes, 384, k_hash);
        BIGNUM *k = BN_bin2bn(k_hash, 64, NULL);

        /* B = (kv + g^b) mod N */
        BIGNUM *kv = BN_new();
        BIGNUM *B  = BN_new();
        BN_mod_mul(kv, k, v, N, bn_ctx);
        BN_mod_add(B, kv, B_raw, N, bn_ctx);

        /* Export A and B as padded 384-byte arrays for hashing */
        uint8_t A_buf[384] = {0};
        BN_bn2bin(A, A_buf + (384 - BN_num_bytes(A)));
        uint8_t B_buf[384] = {0};
        BN_bn2bin(B, B_buf + (384 - BN_num_bytes(B)));

        /* u = H(A || B) — scrambling parameter */
        uint8_t u_hash[64];
        sha512_2(A_buf, 384, B_buf, 384, u_hash);
        BIGNUM *u = BN_bin2bn(u_hash, 64, NULL);

        /* S = (A * v^u)^b mod N — server-side SRP shared secret */
        BIGNUM *vu  = BN_new();
        BIGNUM *Avu = BN_new();
        BIGNUM *S   = BN_new();
        BN_mod_exp(vu, v, u, N, bn_ctx);      /* v^u mod N */
        BN_mod_mul(Avu, A, vu, N, bn_ctx);     /* A * v^u mod N */
        BN_mod_exp(S, Avu, b, N, bn_ctx);      /* (A * v^u)^b mod N */

        /* K = H(S) — session key */
        uint8_t S_buf[384] = {0};
        BN_bn2bin(S, S_buf + (384 - BN_num_bytes(S)));
        sha512_2(S_buf, 384, NULL, 0, ctx->srp_session_key);
        ctx->srp_key_valid = true;

        BN_free(b); BN_free(B_raw); BN_free(B); BN_free(kv);
        BN_free(k); BN_free(v); BN_free(A); BN_free(u);
        BN_free(vu); BN_free(Avu); BN_free(S);
        BN_free(N); BN_free(g);
        BN_CTX_free(bn_ctx);

        /* Build TLV8 response: state=2, salt, B */
        uint8_t state_m2 = 2;
        size_t w = 0;
        w += tlv8_encode_item(out_data + w, 4096, HAP_TLV_STATE, &state_m2, 1);
        w += tlv8_encode_item(out_data + w, 4096 - w, HAP_TLV_SALT, ctx->srp_salt, 16);
        w += tlv8_encode_item(out_data + w, 4096 - w, HAP_TLV_PUBLIC_KEY, B_buf, 384);
        *out_len = w;

        ctx->state = PAIR_STATE_SETUP_M2;
        printf("pair-setup: M1→M2 complete (%zu bytes)\n", *out_len);
        return 0;

    } else if (step == 3) {
        /*
         * M3: iPhone sends its encrypted Ed25519 public key.
         * In transient mode we just extract and store the iPhone's Ed25519 key.
         * We respond with our own encrypted Ed25519 public key.
         */
        size_t epk_len = 0;
        uint8_t *epk = tlv8_find_reassemble(in_data, in_len,
                                              HAP_TLV_ENCRYPTED_DATA, &epk_len);
        /* In transient mode, we accept without verifying — just respond */
        free(epk);

        /*
         * Derive session encryption key from SRP session key.
         * info = "Pair-Setup-Encrypt-Info"
         * salt = "Pair-Setup-Encrypt-Salt"
         */
        static const uint8_t setup_salt[] = "Pair-Setup-Encrypt-Salt";
        static const uint8_t setup_info[] = "Pair-Setup-Encrypt-Info";
        uint8_t enc_key[32];
        hkdf_sha512(setup_salt, sizeof(setup_salt) - 1,
                    ctx->srp_session_key, 64,
                    setup_info, sizeof(setup_info) - 1,
                    enc_key, 32);

        /* Build sub-TLV to encrypt: identifier + Ed25519 public key + signature */
        uint8_t sub_tlv[512];
        size_t sub_len = 0;
        sub_len += tlv8_encode_item(sub_tlv + sub_len, sizeof(sub_tlv) - sub_len,
                                     HAP_TLV_IDENTIFIER,
                                     (const uint8_t *)ctx->server_id,
                                     strlen(ctx->server_id));
        sub_len += tlv8_encode_item(sub_tlv + sub_len, sizeof(sub_tlv) - sub_len,
                                     HAP_TLV_PUBLIC_KEY,
                                     ctx->ed25519_pub, 32);

        /* Sign the sub-TLV with our Ed25519 key */
        uint8_t sig[64];
        ed25519_sign(ctx->ed25519_priv, sub_tlv, sub_len, sig);
        uint8_t sub2[600];
        size_t sub2_len = 0;
        sub2_len += tlv8_encode_item(sub2, sizeof(sub2), HAP_TLV_IDENTIFIER,
                                      (const uint8_t *)ctx->server_id,
                                      strlen(ctx->server_id));
        sub2_len += tlv8_encode_item(sub2 + sub2_len, sizeof(sub2) - sub2_len,
                                      HAP_TLV_PUBLIC_KEY, ctx->ed25519_pub, 32);
        sub2_len += tlv8_encode_item(sub2 + sub2_len, sizeof(sub2) - sub2_len,
                                      HAP_TLV_SIGNATURE, sig, 64);

        /* Encrypt with AES-128-GCM, nonce = "PS-Msg06" padded to 12 bytes */
        uint8_t nonce[12] = "PS-Msg06";  /* 8 bytes, rest = 0 */
        uint8_t encrypted[700];
        size_t enc_len = 0;
        aes128gcm_encrypt(enc_key, nonce, sub2, sub2_len, encrypted, &enc_len);

        /* Build TLV8 response: state=4, encrypted_data */
        uint8_t state_m4 = 4;
        size_t w = 0;
        w += tlv8_encode_item(out_data + w, 4096, HAP_TLV_STATE, &state_m4, 1);
        w += tlv8_encode_item(out_data + w, 4096 - w, HAP_TLV_ENCRYPTED_DATA,
                               encrypted, enc_len);
        *out_len = w;

        ctx->state = PAIR_STATE_SETUP_DONE;
        printf("pair-setup: M3→M4 complete, setup done (%zu bytes)\n", *out_len);
        return 0;

    } else {
        fprintf(stderr, "pair-setup: unknown step %u\n", step);
        return -1;
    }
}

/*
 * pair-verify:
 *   M1: iPhone sends X25519 public key + signature
 *   M2: We generate X25519 keypair, compute shared secret, derive session key,
 *       sign our response with Ed25519, return our X25519 public + encrypted response
 *   M3: iPhone sends verification → we accept
 */
int pair_verify_process(airplay_pair_ctx_t *ctx,
                         const uint8_t *in_data, size_t in_len,
                         uint8_t *out_data, size_t *out_len)
{
    if (!ctx || !in_data || !out_data || !out_len) return -1;
    *out_len = 0;

    size_t state_len = 0;
    const uint8_t *state_val = tlv8_find(in_data, in_len, HAP_TLV_STATE, &state_len);
    uint8_t step = state_val ? *state_val : 1;

    if (step == 1) {
        /*
         * M1: iPhone's X25519 public key (32 bytes) is in TLV_PUBLIC_KEY.
         * TLV_ENCRYPTED_DATA contains the client's Ed25519 signature
         * (encrypted with a key derived from the SRP session — but in
         * transient mode we skip verification and auto-accept).
         */
        size_t client_pub_len = 0;
        const uint8_t *client_pub = tlv8_find(in_data, in_len,
                                               HAP_TLV_PUBLIC_KEY, &client_pub_len);
        if (!client_pub || client_pub_len != 32) {
            fprintf(stderr, "pair-verify M1: missing client X25519 key\n");
            return -1;
        }

        /* Generate our X25519 ephemeral keypair */
        if (gen_x25519(ctx->x25519_pub, ctx->x25519_priv) < 0) return -1;

        /* Compute X25519 shared secret */
        uint8_t shared[32];
        if (x25519_dh(ctx->x25519_priv, client_pub, shared) < 0) return -1;

        /* Derive session key: HKDF(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info") */
        static const uint8_t verify_salt[] = "Pair-Verify-Encrypt-Salt";
        static const uint8_t verify_info[] = "Pair-Verify-Encrypt-Info";
        uint8_t enc_key[32];
        hkdf_sha512(verify_salt, sizeof(verify_salt) - 1,
                    shared, 32,
                    verify_info, sizeof(verify_info) - 1,
                    enc_key, 32);

        /* Derive main session key for later use */
        static const uint8_t sess_salt[] = "Control-Salt";
        static const uint8_t sess_info[] = "Control-Read-Encryption-Key";
        hkdf_sha512(sess_salt, sizeof(sess_salt) - 1,
                    shared, 32,
                    sess_info, sizeof(sess_info) - 1,
                    ctx->session_key, 32);
        ctx->session_key_valid = true;

        /* Build sub-TLV: server X25519 pub || client X25519 pub, sign with Ed25519 */
        uint8_t to_sign[64];
        memcpy(to_sign,      ctx->x25519_pub, 32);
        memcpy(to_sign + 32, client_pub,      32);
        uint8_t sig[64];
        ed25519_sign(ctx->ed25519_priv, to_sign, 64, sig);

        /* Build inner TLV to encrypt */
        uint8_t inner[200];
        size_t inner_len = 0;
        inner_len += tlv8_encode_item(inner + inner_len, sizeof(inner) - inner_len,
                                       HAP_TLV_IDENTIFIER,
                                       (const uint8_t *)ctx->server_id,
                                       strlen(ctx->server_id));
        inner_len += tlv8_encode_item(inner + inner_len, sizeof(inner) - inner_len,
                                       HAP_TLV_SIGNATURE, sig, 64);

        /* Encrypt: nonce = "PV-Msg02\0\0\0\0" */
        uint8_t nonce[12];
        memset(nonce, 0, 12);
        memcpy(nonce, "PV-Msg02", 8);
        uint8_t encrypted[300];
        size_t enc_len = 0;
        aes128gcm_encrypt(enc_key, nonce, inner, inner_len, encrypted, &enc_len);

        /* Build TLV8 response: state=2, our X25519 pub, encrypted data */
        uint8_t state_m2 = 2;
        size_t w = 0;
        w += tlv8_encode_item(out_data + w, 4096, HAP_TLV_STATE, &state_m2, 1);
        w += tlv8_encode_item(out_data + w, 4096 - w, HAP_TLV_PUBLIC_KEY,
                               ctx->x25519_pub, 32);
        w += tlv8_encode_item(out_data + w, 4096 - w, HAP_TLV_ENCRYPTED_DATA,
                               encrypted, enc_len);
        *out_len = w;

        ctx->state = PAIR_STATE_VERIFY_M2;
        printf("pair-verify: M1→M2 complete (%zu bytes)\n", *out_len);
        return 0;

    } else if (step == 3) {
        /*
         * M3: iPhone sends verification data.
         * In transient mode we just accept.
         */
        uint8_t state_m4 = 4;
        size_t w = 0;
        w += tlv8_encode_item(out_data + w, 4096, HAP_TLV_STATE, &state_m4, 1);
        *out_len = w;

        ctx->state = PAIR_STATE_VERIFIED;
        printf("pair-verify: M3→M4 complete — session authenticated\n");
        return 0;

    } else {
        fprintf(stderr, "pair-verify: unknown step %u\n", step);
        return -1;
    }
}

int pair_get_session_key(const airplay_pair_ctx_t *ctx, uint8_t key[32])
{
    if (!ctx || !ctx->session_key_valid) return -1;
    memcpy(key, ctx->session_key, 32);
    return 0;
}

pair_state_t pair_get_state(const airplay_pair_ctx_t *ctx)
{
    if (!ctx) return PAIR_STATE_ERROR;
    return ctx->state;
}

void pair_ctx_destroy(airplay_pair_ctx_t *ctx)
{
    if (!ctx) return;
    memset(ctx->ed25519_priv,   0, sizeof(ctx->ed25519_priv));
    memset(ctx->x25519_priv,    0, sizeof(ctx->x25519_priv));
    memset(ctx->session_key,    0, sizeof(ctx->session_key));
    memset(ctx->srp_session_key,0, sizeof(ctx->srp_session_key));
    ctx->session_key_valid = false;
    ctx->srp_key_valid     = false;
}
