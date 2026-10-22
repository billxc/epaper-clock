// Host policy regression of the real ota.c with SDK/NVS/hash primitives mocked.
// Does NOT validate mbedTLS cryptography, flash hardware or reboot/rollback.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/ota.c"
_Static_assert(PREFIX_SIZE == 288, "ESP app prefix layout");
_Static_assert(sizeof(resume_record_t) == 92 && offsetof(resume_record_t, password) == 73,
               "OTA v1 recovery layout must remain compatible");

static const esp_partition_t running = {.address=0x20000,.size=0x780000,.subtype=16};
static const esp_partition_t inactive = {.address=0x7a0000,.size=0x780000,.subtype=17};
static esp_app_desc_t current = {.magic_word=ESP_APP_DESC_MAGIC_WORD,.version="current",.project_name="epaper-clock"};
static unsigned writes, aborts, ends, records, boots, frees, hash_failure;
static bool sdk_failure;
static char events[64];
static void event(char c) {size_t n=strlen(events); assert(n+1<sizeof(events));events[n]=c;events[n+1]=0;}
const esp_partition_t *esp_ota_get_running_partition(void) {return &running;}
const esp_partition_t *esp_ota_get_next_update_partition(const void *p) {(void)p;return &inactive;}
const esp_app_desc_t *esp_app_get_description(void) {return &current;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s) {(void)p;*s=0;return ESP_OK;}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void) {return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) {return ESP_OK;}
esp_err_t esp_partition_get_sha256(const esp_partition_t *p,uint8_t *h) {(void)p;memset(h,42,32);return ESP_OK;}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t o,void *b,size_t n) {
    (void)p;(void)o;esp_image_header_t h={.magic=0xe9,.chip_id=9};assert(n==sizeof(h));memcpy(b,&h,n);return ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t n,esp_ota_handle_t *h) {(void)p;(void)n;*h=1;event('W');return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *b,size_t n) {(void)h;(void)b;writes+=(unsigned)n;return ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t h) {(void)h;aborts++;event('A');return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t h) {(void)h;ends++;event('E');return sdk_failure?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) {assert(p==&inactive);boots++;event('B');return ESP_OK;}
int64_t esp_timer_get_time(void) {return 0;}
esp_err_t nvs_open(const char *n,int m,nvs_handle_t *h) {(void)n;(void)m;*h=1;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *b,size_t n) {
    (void)h;assert(!strcmp(k,"resume"));assert(n==sizeof(resume_record_t));
    const resume_record_t *r=b;assert(r->magic==RECORD_MAGIC);assert(!strcmp(r->password,"0123456789abcdef"));records++;event('R');return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *b,size_t *n) {(void)h;(void)k;(void)b;(void)n;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_commit(nvs_handle_t h) {(void)h;return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *k) {(void)h;(void)k;return ESP_OK;}
void nvs_close(nvs_handle_t h) {(void)h;}
esp_err_t lan_password_get(char out[17],const char *legacy) {memcpy(out,legacy,17);return ESP_OK;}
void mbedtls_sha256_init(mbedtls_sha256_context *c) {memset(c,0,sizeof(*c));}
void mbedtls_sha256_free(mbedtls_sha256_context *c) {memset(c,0,sizeof(*c));frees++;}
int mbedtls_sha256_starts(mbedtls_sha256_context *c,int mode) {(void)c;assert(mode==0);return hash_failure==1?-1:0;}
int mbedtls_sha256_update(mbedtls_sha256_context *c,const unsigned char *b,size_t n) {
    if(hash_failure==2)return -1;
    for(size_t i=0;i<n;i++)c->sum[c->used++%32]^=b[i];
    return 0;
}
int mbedtls_sha256_finish(mbedtls_sha256_context *c,unsigned char out[32]) {event('H');memcpy(out,c->sum,32);return hash_failure==3?-1:0;}
static void reset(void) {
    if(ota_busy())ota_abort("reset");
    writes=aborts=ends=records=boots=frees=hash_failure=0;sdk_failure=false;events[0]=0;
}
static void digest(const uint8_t *b,size_t n,char hex[65]) {
    uint8_t sum[32]={0};for(size_t i=0;i<n;i++)sum[i%32]^=b[i];
    for(size_t i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",sum[i]);
}
int main(void) {
    uint8_t app[PREFIX_SIZE+83]={0};
    esp_image_header_t header={.magic=0xe9,.chip_id=9};
    esp_app_desc_t desc={.magic_word=ESP_APP_DESC_MAGIC_WORD,.version="target",.project_name="epaper-clock"};
    memcpy(app,&header,sizeof(header));memcpy(app+sizeof(header)+sizeof(esp_image_segment_header_t),&desc,sizeof(desc));
    for(size_t i=PREFIX_SIZE;i<sizeof(app);i++)app[i]=(uint8_t)i;
    char hex[65];digest(app,sizeof(app),hex);
    reset();assert(ota_begin(sizeof(app),"0123456789abcdef",hex)==ESP_OK);
    assert(ota_write(app,13)==ESP_OK);assert(writes==0);
    assert(ota_write(app+13,sizeof(app)-13)==ESP_OK);assert(writes==sizeof(app));
    assert(ota_finish()==ESP_OK);assert(!strcmp(events,"WHERB"));assert(frees==1&&records==1&&boots==1);
    reset();hex[0]=hex[0]=='0'?'1':'0';
    assert(ota_begin(sizeof(app),"0123456789abcdef",hex)==ESP_OK);
    assert(ota_write(app,sizeof(app))==ESP_OK);assert(ota_finish()!=ESP_OK);
    assert(!strcmp(events,"WHA"));assert(aborts==1&&ends==0&&records==0&&boots==0&&frees==1&&!ota_busy());
    ota_status_t s;ota_get_status(&s);assert(strstr(s.message,"SHA256 mismatch"));
    digest(app,sizeof(app),hex);
    reset();sdk_failure=true;
    assert(ota_begin(sizeof(app),"0123456789abcdef",hex)==ESP_OK);assert(ota_write(app,sizeof(app))==ESP_OK);
    assert(ota_finish()!=ESP_OK);assert(ends==1&&records==0&&boots==0); // SDK validation retained.
    reset();assert(ota_begin(sizeof(app),"0123456789abcdef",NULL)==ESP_OK);
    assert(ota_write(app,sizeof(app))==ESP_OK);assert(ota_finish()==ESP_OK);assert(!strcmp(events,"WERB")); // Legacy manual.
    reset();assert(ota_begin(sizeof(app),"0123456789abcdef","bad")!=ESP_OK);assert(writes==0&&boots==0);
    reset();hex[0]='g';assert(ota_begin(sizeof(app),"0123456789abcdef",hex)!=ESP_OK);
    assert(writes==0&&boots==0);digest(app,sizeof(app),hex);
    reset();assert(ota_begin(inactive.size+1,"0123456789abcdef",hex)!=ESP_OK);assert(writes==0&&boots==0);
    for(unsigned field=0;field<3;field++) {
        reset();uint8_t bad[sizeof(app)];memcpy(bad,app,sizeof(app));
        bad[field==0?0:field==1?12:80]^=1;
        assert(ota_begin(sizeof(app),"0123456789abcdef",NULL)==ESP_OK);
        assert(ota_write(bad,sizeof(bad))!=ESP_OK);
        assert(writes==0&&ends==0&&records==0&&boots==0);
    }
    for(unsigned failure=1;failure<=3;failure++) {
        reset();hash_failure=failure;
        esp_err_t err=ota_begin(sizeof(app),"0123456789abcdef",hex);
        if(err==ESP_OK)err=ota_write(app,sizeof(app));
        if(err==ESP_OK)err=ota_finish();
        assert(err!=ESP_OK&&ends==0&&records==0&&boots==0&&frees==1&&!ota_busy());
    }
    reset();assert(ota_begin(sizeof(app),"0123456789abcdef",hex)==ESP_OK);
    assert(ota_write(app,PREFIX_SIZE)==ESP_OK);assert(ota_finish()!=ESP_OK);
    assert(aborts==1&&ends==0&&records==0&&boots==0&&frees==1);
    puts("OTA hash policy passed (mock primitives; no hardware)");
    return 0;
}
