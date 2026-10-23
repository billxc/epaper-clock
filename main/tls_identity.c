#include "tls_identity.h"
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_random.h"
#include "esp_mac.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "mbedtls/platform_util.h"

// No key/certificate logging. No shared key, wall clock or factory-reset hook.
// Latch write failures: NVS RAM can expose a value whose commit failed.
static esp_err_t persistence_error;
static int random_bytes(void *ctx, unsigned char *out, size_t size)
{
    (void)ctx;
    esp_fill_random(out, size); // Caller has started Wi-Fi, supplying entropy.
    return 0;
}
void tls_identity_free(tls_identity_t *identity)
{
    if (identity) {
        mbedtls_platform_zeroize(identity, sizeof(*identity));
        free(identity);
    }
}
static bool valid(tls_identity_t *id)
{
    if (id->version != 1 || !memchr(id->certificate, 0, sizeof(id->certificate)) ||
        !memchr(id->private_key, 0, sizeof(id->private_key))) return false;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);
    // Deliberately no date/trust verification against an unset device RTC.
    int rc = mbedtls_x509_crt_parse(&cert, (unsigned char *)id->certificate,
                                   strlen(id->certificate) + 1);
    if (!rc) rc = mbedtls_pk_parse_key(&key, (unsigned char *)id->private_key,
        strlen(id->private_key) + 1, NULL, 0, random_bytes, NULL);
    if (!rc) rc = mbedtls_pk_check_pair(&cert.pk, &key, random_bytes, NULL);
    bool ok = rc == 0 && mbedtls_pk_get_type(&key) == MBEDTLS_PK_ECKEY &&
        mbedtls_pk_get_bitlen(&key) == 256 && !cert.MBEDTLS_PRIVATE(ca_istrue);
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&cert);
    return ok;
}
static esp_err_t generate(const char *ip, tls_identity_t *id)
{
    struct in_addr address;
    if (!ip || inet_pton(AF_INET, ip, &address) != 1) return ESP_ERR_INVALID_ARG;
    uint8_t mac[6], serial[16];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) return err;
    char dns[32], subject[48];
    snprintf(dns, sizeof(dns), "epaper-%02x%02x%02x%02x%02x%02x.local",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(subject, sizeof(subject), "CN=%s", dns);
    // DER GeneralNames: SEQUENCE { dNSName [2], iPAddress [7] }.
    unsigned char san[64] = {0x30, 0, 0x82, 0};
    size_t n = strlen(dns);
    san[1] = (unsigned char)(n + 8);
    san[3] = (unsigned char)n;
    memcpy(san + 4, dns, n);
    san[n + 4] = 0x87; san[n + 5] = 4;
    memcpy(san + n + 6, &address.s_addr, 4);
    // DER ExtendedKeyUsage { id-kp-serverAuth }.
    const unsigned char eku[] = {0x30,0x0a,0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x01};
    esp_fill_random(serial, sizeof(serial));
    serial[0] = (serial[0] & 0x7f) | 1;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    int rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (!rc) rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
        mbedtls_pk_ec(key), random_bytes, NULL);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if (!rc) rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    if (!rc) rc = mbedtls_x509write_crt_set_subject_name(&crt, subject);
    if (!rc) rc = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
    // Long-lived LAN self-signed TEST identity, not public CA trust. Generation
    // never waits for RTC/NTP. Browser still checks its own time and SAN.
    if (!rc) rc = mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20991231235959");
    if (!rc) rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if (!rc) rc = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (!rc) rc = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, n + 10);
    if (!rc) rc = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_EXTENDED_KEY_USAGE,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, eku, sizeof(eku));
    if (!rc) rc = mbedtls_pk_write_key_pem(&key, (unsigned char *)id->private_key, sizeof(id->private_key));
    if (!rc) rc = mbedtls_x509write_crt_pem(&crt, (unsigned char *)id->certificate,
        sizeof(id->certificate), random_bytes, NULL);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    if (rc) return ESP_FAIL;
    id->version = 1;
    return valid(id) ? ESP_OK : ESP_FAIL;
}
esp_err_t tls_identity_load(const char *ip, tls_identity_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (persistence_error) return persistence_error;
    tls_identity_t *id = calloc(1, sizeof(*id));
    if (!id) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("clock_tls", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(*id);
        err = nvs_get_blob(handle, "identity", id, &size);
        nvs_close(handle);
        if (err == ESP_OK && (size != sizeof(*id) || !valid(id))) err = ESP_ERR_INVALID_STATE;
    }
    // Only absent data permits first creation; corruption is never overwritten.
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = generate(ip, id);
        if (err == ESP_OK) {
            err = nvs_open("clock_tls", NVS_READWRITE, &handle);
            if (err == ESP_OK) {
                err = nvs_set_blob(handle, "identity", id, sizeof(*id));
                if (err == ESP_OK) err = nvs_commit(handle);
                nvs_close(handle);
            }
            if (err != ESP_OK) persistence_error = err;
        }
    }
    if (err == ESP_OK) *out = id;
    else tls_identity_free(id);
    return err;
}
