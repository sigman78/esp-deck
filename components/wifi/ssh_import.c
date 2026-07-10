/*
 * ssh_import — device implementation.
 *
 * Temporary WPA2 SoftAP + esp_http_server. Serves a one-page form; a POST
 * stores an optional private key blob and appends a connection profile to
 * profiles.ini. See ssh_import.h for the security rationale (WPA2 passphrase
 * doubles as the proof-of-possession, so the pasted key rides an encrypted
 * link and plain HTTP is acceptable).
 */

#include "ssh_import.h"
#include "wifi_manager.h"
#include "storage.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "qrcode.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "ssh_import";

static volatile int s_state = SSH_IMPORT_ST_IDLE;
static char s_service[32] = "";
static char s_pop[16]     = "";
static char s_url[24]     = "";
static char s_last[32]    = "";
static char s_err[64]     = "";
static volatile int s_count = 0;

static httpd_handle_t  s_httpd    = NULL;
static esp_netif_t    *s_ap_netif = NULL;
static SemaphoreHandle_t s_lock   = NULL;   /* serialize POST handling */

/* Bit-packed QR module matrix (RAM-frugal), same scheme as wifi_provision. */
#define QR_MAX 45
static uint8_t s_qr_mod[(QR_MAX * QR_MAX + 7) / 8];
static int     s_qr_size = 0;

static inline void qr_setbit(int i) { s_qr_mod[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline bool qr_getbit(int i) { return (s_qr_mod[i >> 3] >> (i & 7)) & 1u; }

static void qr_display_cb(esp_qrcode_handle_t qr)
{
    int sz = esp_qrcode_get_size(qr);
    if (sz <= 0 || sz > QR_MAX) { s_qr_size = 0; return; }
    memset(s_qr_mod, 0, sizeof(s_qr_mod));
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++)
            if (esp_qrcode_get_module(qr, x, y)) qr_setbit(y * QR_MAX + x);
    s_qr_size = sz;
}

static void generate_qr(const char *text)
{
    s_qr_size = 0;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func       = qr_display_cb;
    cfg.max_qrcode_version = 6;
    cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_LOW;
    esp_err_t e = esp_qrcode_generate(&cfg, text);
    if (e != ESP_OK) ESP_LOGW(TAG, "QR generate failed: %s", esp_err_to_name(e));
}

int  ssh_import_qr_size(void) { return s_qr_size; }
bool ssh_import_qr_module(int x, int y)
{
    if (x < 0 || y < 0 || x >= s_qr_size || y >= s_qr_size) return false;
    return qr_getbit(y * QR_MAX + x);
}

/* ------------------------------------------------------------- HTML form */

/* Split at the hidden PoP value so we never printf %-laden CSS. */
static const char FORM_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Cyberdeck - add SSH profile</title><style>"
    "body{background:#0b0f0b;color:#5f5;font-family:ui-monospace,Menlo,Consolas,monospace;"
    "margin:0;padding:16px;font-size:16px}"
    ".c{max-width:520px;margin:0 auto}"
    "h1{font-size:18px;color:#8f8;border-bottom:1px solid #2a2;padding-bottom:8px}"
    "label{display:block;margin:12px 0 4px;color:#9c9}"
    "input,textarea,select{width:100%;box-sizing:border-box;background:#111;color:#5f5;"
    "border:1px solid #2a2;border-radius:6px;padding:10px;font:inherit}"
    "textarea{height:110px;white-space:pre;overflow:auto}"
    ".row{display:flex;gap:10px}.row>div{flex:1}"
    ".seg{display:flex;gap:8px;margin-top:4px}"
    ".seg label{flex:1;margin:0;text-align:center;border:1px solid #2a2;border-radius:6px;"
    "padding:10px;cursor:pointer}"
    ".seg input{display:none}.seg input:checked+span{color:#0b0f0b}"
    ".seg label:has(input:checked){background:#5f5;color:#0b0f0b}"
    "button{width:100%;margin-top:18px;background:#5f5;color:#0b0f0b;border:0;border-radius:6px;"
    "padding:14px;font:inherit;font-weight:bold;cursor:pointer}"
    ".hint{color:#686;font-size:13px;margin-top:4px}"
    ".hide{display:none}"
    "</style></head><body><div class=c>"
    "<h1>&#9654; ADD SSH PROFILE</h1>"
    "<form method=post action=/save accept-charset=utf-8>"
    "<input type=hidden name=pop value=\"";

static const char FORM_TAIL[] =
    "\">"
    "<label>Profile name</label><input name=name maxlength=31 required "
    "placeholder=\"e.g. homelab\">"
    "<div class=row><div><label>Host</label>"
    "<input name=host maxlength=63 required placeholder=\"10.0.0.5 or host\"></div>"
    "<div style=flex:0.4><label>Port</label>"
    "<input name=port type=number value=22 min=1 max=65535></div></div>"
    "<label>Username</label><input name=user maxlength=31 required placeholder=root>"
    "<label>Authentication</label>"
    "<div class=seg>"
    "<label><input type=radio name=auth value=password checked onclick=am()><span>Password</span></label>"
    "<label><input type=radio name=auth value=key onclick=am()><span>Private key</span></label>"
    "</div>"
    "<div id=pw><label>Password</label><input name=password type=password></div>"
    "<div id=kb class=hide>"
    "<label>Private key (PEM)</label>"
    "<textarea name=key placeholder=\"-----BEGIN OPENSSH PRIVATE KEY-----\"></textarea>"
    "<input type=file onchange=lf(this,'key') class=hint>"
    "<label>Public key <span class=hint>(needed for ed25519/ecdsa)</span></label>"
    "<textarea name=pubkey placeholder=\"ssh-ed25519 AAAA...\" style=height:60px></textarea>"
    "<input type=file onchange=lf(this,'pubkey') class=hint>"
    "<label>Key passphrase <span class=hint>(if encrypted)</span></label>"
    "<input name=passphrase type=password>"
    "</div>"
    "<button type=submit>SAVE TO DECK</button>"
    "</form></div><script>"
    "function am(){var k=document.querySelector('[name=auth]:checked').value=='key';"
    "document.getElementById('kb').className=k?'':'hide';"
    "document.getElementById('pw').className=k?'hide':''}"
    "function lf(i,n){var f=i.files[0];if(!f)return;var r=new FileReader();"
    "r.onload=function(){document.querySelector('[name='+n+']').value=r.result};"
    "r.readAsText(f)}"
    "</script></body></html>";

static esp_err_t ok_page(httpd_req_t *req, const char *msg, const char *sub)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<style>body{background:#0b0f0b;color:#5f5;font-family:ui-monospace,monospace;"
        "text-align:center;padding:48px 16px}a{color:#8f8}h1{color:#8f8}"
        ".s{color:#686}</style><h1>");
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, "</h1><p class=s>");
    httpd_resp_sendstr_chunk(req, sub);
    httpd_resp_sendstr_chunk(req, "</p><p><a href=/>&#8592; add another</a></p>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------- form-urlencoded parse */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL-decode src[..slen) into dst (NUL-terminated, capped at dcap). */
static void url_decode(const char *src, int slen, char *dst, int dcap)
{
    int o = 0;
    for (int i = 0; i < slen && o < dcap - 1; i++) {
        char c = src[i];
        if (c == '+') {
            dst[o++] = ' ';
        } else if (c == '%' && i + 2 < slen) {
            int h = hexval(src[i + 1]), l = hexval(src[i + 2]);
            if (h >= 0 && l >= 0) { dst[o++] = (char)((h << 4) | l); i += 2; }
            else dst[o++] = c;
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
}

/* Extract field @p name from a form-urlencoded @p body into @p out.
 * Returns the decoded length, or -1 if the field is absent. */
static int form_field(const char *body, const char *name, char *out, int cap)
{
    int nlen = (int)strlen(name);
    const char *p = body;
    while (p && *p) {
        /* p sits at the start of a "key=value" pair */
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            const char *e = strchr(v, '&');
            int vlen = e ? (int)(e - v) : (int)strlen(v);
            url_decode(v, vlen, out, cap);
            return (int)strlen(out);
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    out[0] = '\0';
    return -1;
}

/* Fold a profile name into a filesystem-safe key id (alnum/-/_ only). */
static void sanitize_key_id(const char *name, char *out, int cap)
{
    int o = 0;
    for (int i = 0; name[i] && o < cap - 1; i++) {
        char c = name[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_') out[o++] = c;
        else if (c == ' ')                                     out[o++] = '_';
    }
    out[o] = '\0';
}

static void set_err(const char *msg)
{
    snprintf(s_err, sizeof(s_err), "%s", msg);
    ESP_LOGW(TAG, "reject: %s", msg);
}

/* Write keys/<id>.pub directly (storage_set_key only handles .pem). */
static void write_pubkey(const char *key_id, const char *pub, int len)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/keys/%s.pub",
             storage_platform_mount_point(), key_id);
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "pubkey open failed"); return; }
    fwrite(pub, 1, (size_t)len, f);
    fclose(f);
}

/* Append (or replace by name) @p pf in profiles.ini. Mirrors the on-device
 * editor's capacity convention (cyberdeck_app.c): store at most PROFILE_MAX. */
#define IMPORT_PROFILE_MAX 8
static esp_err_t append_profile(const conn_profile_t *pf)
{
    conn_profile_t list[IMPORT_PROFILE_MAX + 1];
    int n = 0;
    storage_load_profiles(list, &n, IMPORT_PROFILE_MAX);

    int slot = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].name, pf->name) == 0) { slot = i; break; }

    if (slot < 0) {
        if (n >= IMPORT_PROFILE_MAX) return ESP_ERR_NO_MEM;   /* list full */
        slot = n++;
    }
    list[slot] = *pf;                                         /* replace or add */
    return storage_save_profiles(list, n);
}

static esp_err_t handle_save(httpd_req_t *req, char *body)
{
    /* Field buffers. Keep host/user/name bounded to conn_profile_t sizes. */
    char pop[16], name[40], host[80], user[40], port[8], auth[16];
    char password[128], passphrase[128];

    form_field(body, "pop", pop, sizeof(pop));
    if (strcmp(pop, s_pop) != 0) {
        set_err("bad proof code");
        httpd_resp_set_status(req, "403 Forbidden");
        return ok_page(req, "&#10007; Rejected", "Proof code did not match.");
    }

    form_field(body, "name", name, sizeof(name));
    form_field(body, "host", host, sizeof(host));
    form_field(body, "user", user, sizeof(user));
    form_field(body, "port", port, sizeof(port));
    form_field(body, "auth", auth, sizeof(auth));
    form_field(body, "password",   password,   sizeof(password));
    form_field(body, "passphrase", passphrase, sizeof(passphrase));

    /* Validate. */
    if (!name[0] || !host[0] || !user[0]) {
        set_err("name, host and user are required");
        httpd_resp_set_status(req, "400 Bad Request");
        return ok_page(req, "&#10007; Missing fields",
                       "Name, host and username are required.");
    }
    if (strpbrk(name, "[]")) {           /* INI section-header safety */
        set_err("name cannot contain [ or ]");
        httpd_resp_set_status(req, "400 Bad Request");
        return ok_page(req, "&#10007; Bad name", "Name cannot contain [ or ].");
    }
    int portn = port[0] ? atoi(port) : 22;
    if (portn < 1 || portn > 65535) {
        set_err("port out of range");
        httpd_resp_set_status(req, "400 Bad Request");
        return ok_page(req, "&#10007; Bad port", "Port must be 1-65535.");
    }

    conn_profile_t pf = { 0 };
    snprintf(pf.name, sizeof(pf.name), "%s", name);
    snprintf(pf.host, sizeof(pf.host), "%s", host);
    snprintf(pf.user, sizeof(pf.user), "%s", user);
    pf.port = (uint16_t)portn;

    bool want_key = (strcmp(auth, "key") == 0);
    if (want_key) {
        /* Key PEM lives at the end of the body (largest field); parse it
         * from the raw body so we don't need a huge stack buffer. */
        char *key    = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        char *pubkey = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!key || !pubkey) {
            free(key); free(pubkey);
            set_err("out of memory for key");
            httpd_resp_set_status(req, "500 Internal Server Error");
            return ok_page(req, "&#10007; Out of memory", "Try a smaller key.");
        }
        form_field(body, "key",    key,    8192);
        form_field(body, "pubkey", pubkey, 2048);

        if (!strstr(key, "PRIVATE KEY")) {
            free(key); free(pubkey);
            set_err("not a PEM private key");
            httpd_resp_set_status(req, "400 Bad Request");
            return ok_page(req, "&#10007; Bad key",
                           "Paste a PEM private key (BEGIN ... PRIVATE KEY).");
        }

        char key_id[32];
        sanitize_key_id(name, key_id, sizeof(key_id));
        if (!key_id[0]) {
            free(key); free(pubkey);
            set_err("name has no usable characters");
            httpd_resp_set_status(req, "400 Bad Request");
            return ok_page(req, "&#10007; Bad name",
                           "Name needs a letter or digit.");
        }

        esp_err_t e = storage_set_key(key_id, key, strlen(key));
        if (e == ESP_OK && pubkey[0]) write_pubkey(key_id, pubkey, (int)strlen(pubkey));
        free(key); free(pubkey);
        if (e != ESP_OK) {
            set_err("failed to store key");
            httpd_resp_set_status(req, "500 Internal Server Error");
            return ok_page(req, "&#10007; Store failed", "Could not write the key.");
        }
        pf.auth = STORAGE_AUTH_KEY;
        snprintf(pf.key_id, sizeof(pf.key_id), "%s", key_id);
        /* password field doubles as the key passphrase (see cyberdeck_app). */
        snprintf(pf.password, sizeof(pf.password), "%s", passphrase);
    } else {
        pf.auth = STORAGE_AUTH_PASSWORD;
        snprintf(pf.password, sizeof(pf.password), "%s", password);
    }

    esp_err_t ae = append_profile(&pf);
    if (ae == ESP_ERR_NO_MEM) {
        set_err("profile list full");
        httpd_resp_set_status(req, "507 Insufficient Storage");
        return ok_page(req, "&#10007; List full",
                       "The deck already holds the maximum profiles.");
    }
    if (ae != ESP_OK) {
        set_err("failed to save profile");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return ok_page(req, "&#10007; Save failed", "Could not write profiles.");
    }

    s_err[0] = '\0';
    snprintf(s_last, sizeof(s_last), "%s", pf.name);
    s_count++;
    ESP_LOGI(TAG, "imported profile '%s' (%s)", pf.name,
             want_key ? "key" : "password");

    char sub[80];
    snprintf(sub, sizeof(sub), "%s @ %s:%d saved to the deck.", pf.user, pf.host, portn);
    return ok_page(req, "&#10003; Saved", sub);
}

/* ------------------------------------------------------------- HTTP handlers */

static esp_err_t get_form(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, FORM_HEAD);
    httpd_resp_sendstr_chunk(req, s_pop);
    httpd_resp_sendstr_chunk(req, FORM_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t post_save(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 16384) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return ok_page(req, "&#10007; Too large", "Payload exceeds 16 KB.");
    }

    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return ok_page(req, "&#10007; Out of memory", "The deck is low on RAM.");
    }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    esp_err_t e = ESP_FAIL;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
        e = handle_save(req, body);
        xSemaphoreGive(s_lock);
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
        e = ok_page(req, "&#10007; Busy", "Another save is in progress.");
    }
    free(body);
    return e;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.stack_size       = 6144;

    esp_err_t e = httpd_start(&s_httpd, &cfg);
    if (e != ESP_OK) { s_httpd = NULL; return e; }

    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    httpd_uri_t form = { .uri = "/*",    .method = HTTP_GET,  .handler = get_form };
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &form);
    return ESP_OK;
}

/* -------------------------------------------------------------- lifecycle */

esp_err_t ssh_import_start(void)
{
    if (s_state != SSH_IMPORT_ST_IDLE) return ESP_ERR_INVALID_STATE;

    if (wifi_manager_init() != ESP_OK) { s_state = SSH_IMPORT_ST_ERROR; return ESP_FAIL; }
    wifi_manager_disconnect();          /* park STA retry loop for the session */

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_lock)     s_lock     = xSemaphoreCreateMutex();

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_service, sizeof(s_service), "DECK-SETUP-%02X%02X", mac[4], mac[5]);
    snprintf(s_pop,     sizeof(s_pop),     "%02X%02X%02X%02X",
             mac[0], mac[3], mac[4], mac[5]);   /* 8 hex chars = valid WPA2 key */
    snprintf(s_url,     sizeof(s_url),     "http://192.168.4.1");
    s_last[0] = s_err[0] = '\0';
    s_count = 0;

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_service);
    ap.ap.ssid_len       = (uint8_t)strlen(s_service);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_pop);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 1;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_AP);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (e == ESP_OK) {
        esp_err_t se = esp_wifi_start();   /* no-op+OK if the driver is already up */
        if (se != ESP_OK && se != ESP_ERR_WIFI_NOT_INIT)
            ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(se));
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP up failed: %s", esp_err_to_name(e));
        ssh_import_stop();
        s_state = SSH_IMPORT_ST_ERROR;
        return e;
    }

    if (start_httpd() != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        ssh_import_stop();
        s_state = SSH_IMPORT_ST_ERROR;
        return ESP_FAIL;
    }

    /* Standard WiFi-join QR: scanning it connects the phone to the AP. */
    char qr[96];
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", s_service, s_pop);
    generate_qr(qr);

    s_state = SSH_IMPORT_ST_ACTIVE;
    ESP_LOGI(TAG, "import AP up: join '%s' (key '%s'), open %s",
             s_service, s_pop, s_url);
    return ESP_OK;
}

void ssh_import_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }

    esp_wifi_set_mode(WIFI_MODE_STA);      /* drop the AP radio, back to STA */
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_set_default_netif(sta);

    s_state   = SSH_IMPORT_ST_IDLE;
    s_qr_size = 0;
    s_service[0] = s_pop[0] = s_url[0] = '\0';
    ESP_LOGI(TAG, "import AP stopped");
}

int         ssh_import_state(void)        { return s_state; }
const char *ssh_import_service_name(void) { return s_service; }
const char *ssh_import_pop(void)          { return s_pop; }
const char *ssh_import_url(void)          { return s_url; }
const char *ssh_import_last(void)         { return s_last; }
const char *ssh_import_err(void)          { return s_err; }
int         ssh_import_count(void)        { return s_count; }
