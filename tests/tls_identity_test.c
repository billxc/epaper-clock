// Real mbedTLS crypto, host-only NVS/MAC/RNG adapters. Never touches hardware.
// Build with tests/tls_identity_stubs and mbedTLS 3.x includes/libs.
#include <assert.h>
#include <stdio.h>
#include <sys/random.h>
#include "../main/tls_identity.c"
#include "mbedtls/md.h"

static tls_identity_t stored;
static bool exists;
static size_t saved_size;
static int writes, commits, mac_reads;
static esp_err_t open_error, read_error, set_error, commit_error;
void esp_fill_random(void *out, size_t n)
{
    unsigned char *p = out;
    while (n) { ssize_t got = getrandom(p, n, 0); assert(got > 0); p += got; n -= got; }
}
esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    assert(type == ESP_MAC_WIFI_STA);
    const uint8_t fixture[] = {0x02,0,0,0,0,1};
    memcpy(mac, fixture, 6); ++mac_reads; return ESP_OK;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    assert(!strcmp(name, "clock_tls"));
    if (open_error) return open_error;
    *h = mode; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    assert(h == NVS_READONLY && !strcmp(key, "identity"));
    if (read_error) return read_error;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= saved_size);
    memcpy(out, &stored, saved_size); *size = saved_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t size)
{
    assert(h == NVS_READWRITE && !strcmp(key, "identity") && size == sizeof(stored));
    ++writes;
    if (set_error) return set_error;
    memcpy(&stored, in, size); saved_size = size; exists = true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { assert(h == NVS_READWRITE); ++commits; return commit_error; }
void nvs_close(nvs_handle_t h) { (void)h; }
static void reset(void)
{
    memset(&stored, 0, sizeof(stored)); exists = false; saved_size = 0;
    writes = commits = mac_reads = 0;
    open_error = read_error = set_error = commit_error = persistence_error = 0;
}
static void failed(void)
{
    tls_identity_t *id = (void *)1;
    assert(tls_identity_load("192.0.2.10", &id) != ESP_OK && id == NULL);
}
static void inspect(tls_identity_t *id)
{
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    assert(mbedtls_x509_crt_parse(&cert, (unsigned char *)id->certificate, strlen(id->certificate)+1) == 0);
    assert(!cert.MBEDTLS_PRIVATE(ca_istrue) && cert.valid_from.year == 2020 && cert.valid_to.year == 2099);
    assert(cert.MBEDTLS_PRIVATE(key_usage) == MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    assert(mbedtls_x509_crt_check_extended_key_usage(&cert, MBEDTLS_OID_SERVER_AUTH,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) == 0);
    const mbedtls_x509_sequence *san = &cert.subject_alt_names;
    assert(san->buf.len == strlen("epaper-020000000001.local"));
    assert(!memcmp(san->buf.p, "epaper-020000000001.local", san->buf.len));
    san = san->next;
    const unsigned char expected_ip[] = {192,0,2,10};
    assert(san && san->buf.len == 4 && !memcmp(san->buf.p, expected_ip, 4) && !san->next);
    // Verify self-signature directly, independent of wall clock/trust policy.
    unsigned char hash[32];
    assert(mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), cert.tbs.p, cert.tbs.len, hash) == 0);
    assert(mbedtls_pk_verify(&cert.pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), cert.MBEDTLS_PRIVATE(sig).p, cert.MBEDTLS_PRIVATE(sig).len) == 0);
    mbedtls_x509_crt_free(&cert);
}
int main(void)
{
    reset(); tls_identity_t *id = NULL;
    assert(tls_identity_load("192.0.2.10", &id) == ESP_OK && id);
    inspect(id);
    tls_identity_t first = *id;
    tls_identity_free(id);
    for (int i = 0; i < 20; ++i) {
        persistence_error = 0; // Simulate reentry/reboot/OTA/deep/minute.
        assert(tls_identity_load("192.0.2.99", &id) == ESP_OK);
        assert(!memcmp(&first, id, sizeof(first))); tls_identity_free(id);
    }
    assert(writes == 1 && commits == 1 && mac_reads == 1); // DHCP does not regenerate.
    // A distinct fresh device gets a distinct key even with the fixture MAC.
    reset(); assert(tls_identity_load("192.0.2.10", &id) == ESP_OK);
    assert(strcmp(first.private_key, id->private_key)); tls_identity_free(id);
    memcpy(stored.certificate, first.certificate, sizeof(stored.certificate));
    failed(); assert(writes == 1); // Two individually valid, mismatched key/cert.
    stored.version = 9; failed(); assert(writes == 1);
    stored = first; stored.private_key[40] ^= 1; failed(); assert(writes == 1);
    stored = first; saved_size--; failed(); assert(writes == 1);
    reset(); open_error = ESP_FAIL; failed(); assert(!writes && !mac_reads);
    reset(); read_error = ESP_FAIL; failed(); assert(!writes && !mac_reads);
    reset(); set_error = ESP_FAIL; failed(); assert(writes == 1 && !commits);
    set_error = 0; failed(); assert(writes == 1); // Failure latch.
    reset(); commit_error = ESP_FAIL; failed(); assert(writes == 1 && commits == 1);
    commit_error = 0; failed(); assert(writes == 1); // Cannot use uncommitted RAM view.
    reset(); assert(tls_identity_load("bad-ip", &id) == ESP_ERR_INVALID_ARG && !id && !writes);
    mbedtls_platform_zeroize(&first, sizeof(first));
    mbedtls_platform_zeroize(&stored, sizeof(stored));
    puts("TLS identity real-crypto persistence/SAN/fault tests passed (host simulation)");
}
