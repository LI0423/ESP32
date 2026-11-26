#include "provisioning.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "admin_cert.h"
#include "companion_model.h"
#include "companion_progress.h"
#include "config_store.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "network.h"
#include "ota.h"

#define TAG "companion_web"
#define HTTP_BODY_MAX 2048
#define HTTP_BODY_TIMEOUT_US 5000000

static httpd_handle_t s_server;

static const char HTML_PAGE[] =
"<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>DeskPal Setup</title><style>"
":root{color-scheme:dark;--bg:#0a1024;--panel:#131d38;--edge:#2a3f72;"
"--text:#e8f4ff;--muted:#9fb1c9;--accent:#4de8ff;--ok:#5ef2b0;--warn:#ffc96b}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);"
"font:15px system-ui,-apple-system,sans-serif}main{max-width:620px;margin:auto;padding:24px 16px 48px}"
"h1{font-size:26px;margin:0 0 6px}h2{font-size:18px;margin:0 0 16px}"
".sub{color:var(--muted);margin-bottom:20px}.card{background:var(--panel);"
"border:1px solid var(--edge);border-radius:16px;padding:20px;margin:16px 0}"
"label{display:block;color:var(--muted);font-size:13px;margin:12px 0 6px}"
"input{width:100%;background:#0c142b;color:var(--text);border:1px solid var(--edge);"
"border-radius:10px;padding:11px;font:inherit}input[type=range]{padding:0}"
".row{display:flex;align-items:center;gap:10px;flex-wrap:wrap}.check{display:flex;gap:8px;"
"align-items:center;color:var(--muted);margin-top:12px}.check input{width:auto}"
"button{border:0;border-radius:10px;padding:11px 16px;background:var(--accent);"
"color:#07111f;font-weight:700;cursor:pointer;margin:4px 6px 4px 0}"
"button.secondary{background:#263a63;color:var(--text)}pre{white-space:pre-wrap;"
"word-break:break-word;background:#091022;border:1px solid var(--edge);border-radius:10px;"
"padding:12px;max-height:260px;overflow:auto;font-size:12px}"
"#message{min-height:24px;color:var(--warn)}</style></head><body><main>"
"<h1>DeskPal <span class=\"pill\">SETUP</span></h1>"
"<div class=\"sub\">首次连接 DeskPal-Setup（默认密码 deskpal1），打开 https://192.168.4.1（自签名证书，需手动信任）</div>"
"<div class=\"card\"><h2>当前状态</h2><pre id=\"preview\">{}</pre></div>"
"<div class=\"card\"><h2>网络与显示</h2>"
"<label for=\"admin_token\">管理令牌（首次从串口日志获取）</label>"
"<input id=\"admin_token\" type=\"password\" maxlength=\"64\" autocomplete=\"off\">"
"<label for=\"wifi_ssid\">Wi-Fi SSID</label><input id=\"wifi_ssid\" maxlength=\"32\" autocomplete=\"off\">"
"<label for=\"wifi_password\">Wi-Fi 密码（留空保持已保存密码）</label>"
"<input id=\"wifi_password\" type=\"password\" maxlength=\"64\" autocomplete=\"new-password\">"
"<label class=\"check\"><input id=\"clear_wifi_password\" type=\"checkbox\"> 明确清除密码</label>"
"<label for=\"brightness\">亮度 <span id=\"brightness_value\">75</span>%</label>"
"<input id=\"brightness\" type=\"range\" min=\"5\" max=\"100\" value=\"75\">"
"<label for=\"daily_goal\">今日目标（轮，1–99）</label>"
"<input id=\"daily_goal\" type=\"number\" min=\"1\" max=\"99\" step=\"1\" value=\"4\">"
"<label for=\"ota_url\">固件 URL</label><input id=\"ota_url\" maxlength=\"256\" "
"placeholder=\"https://192.168.1.20/desktop_companion.bin\" autocomplete=\"off\"></div>"
"<div class=\"card\"><h2>计时</h2><div class=\"row\">"
"<button id=\"focus_button\">开始新一轮</button>"
"<button id=\"standby_button\" class=\"secondary\">今晚先待命</button></div>"
"<div class=\"sub\">开始新一轮会立即进入全新专注；夜间手动开始会保持活跃直到次日 08:00 或再次待命。"
"今晚先待命会保持到下一个 08:00（跨午夜不解除）。</div></div>"
"<div class=\"card\"><h2>操作</h2><div class=\"row\">"
"<button id=\"save_button\">保存</button>"
"<button id=\"restart_button\" class=\"secondary\">重启</button>"
"<button id=\"ota_button\">OTA 升级</button>"
"<button id=\"ota_cancel_button\" class=\"secondary\">取消 OTA</button></div><div id=\"message\"></div></div>"
"</main><script>const $=id=>document.getElementById(id);"
"function adminHeaders(){const token=sessionStorage.getItem('deskpal_token')||'';"
"const headers={'Content-Type':'application/json'};if(token)headers.Authorization='Bearer '+token;return headers}"
"async function api(path,options={}){const headers={...adminHeaders(),...(options.headers||{})};"
"const r=await fetch(path,{...options,headers});"
"let data={};try{data=await r.json()}catch(e){}if(!r.ok)throw new Error(data.error||('HTTP '+r.status));return data}"
"async function loadConfig(){const c=await api('/api/config');$('wifi_ssid').value=c.wifi_ssid||'';"
"$('brightness').value=c.brightness;$('brightness_value').textContent=c.brightness;"
"$('daily_goal').value=c.daily_goal||4;"
"$('ota_url').value=c.ota_url||''}"
"async function refresh(){try{const s=await api('/api/status');$('preview').textContent=JSON.stringify(s,null,2)}"
"catch(e){$('preview').textContent=e.message}}"
"$('brightness').oninput=()=>{$('brightness_value').textContent=$('brightness').value};"
"$('admin_token').value=sessionStorage.getItem('deskpal_token')||'';"
"$('admin_token').oninput=()=>sessionStorage.setItem('deskpal_token',$('admin_token').value.trim());"
"$('save_button').onclick=async()=>{try{await api('/api/config',{method:'POST',body:JSON.stringify({"
"wifi_ssid:$('wifi_ssid').value,wifi_password:$('wifi_password').value,"
"clear_wifi_password:$('clear_wifi_password').checked,brightness:Number($('brightness').value),"
"daily_goal:Number($('daily_goal').value),"
"ota_url:$('ota_url').value,reboot:false})});$('message').textContent='已保存';"
"$('wifi_password').value='';await loadConfig();await refresh()}catch(e){$('message').textContent=e.message}};"
"$('focus_button').onclick=async()=>{try{await api('/api/timer/focus',{method:'POST',body:'{}'});"
"$('message').textContent='已开始新一轮专注';await refresh()}catch(e){$('message').textContent=e.message}};"
"$('standby_button').onclick=async()=>{try{await api('/api/timer/standby',{method:'POST',body:'{}'});"
"$('message').textContent='已进入待命，直到下一个 08:00';await refresh()}catch(e){$('message').textContent=e.message}};"
"$('restart_button').onclick=async()=>{try{await api('/api/restart',{method:'POST',body:'{}'});"
"$('message').textContent='正在重启…'}catch(e){$('message').textContent=e.message}};"
"$('ota_button').onclick=async()=>{try{const url=$('ota_url').value.trim();if(!url)throw new Error('请填写固件 URL');"
"const r=await api('/api/ota',{method:'POST',body:JSON.stringify({url})});"
"$('message').textContent=r.message||'OTA 已开始';await refresh()}catch(e){$('message').textContent=e.message}};"
"$('ota_cancel_button').onclick=async()=>{try{await api('/api/ota/cancel',{method:'POST',body:'{}'});"
"$('message').textContent='已请求取消 OTA';await refresh()}catch(e){$('message').textContent=e.message}};"
"loadConfig();refresh();setInterval(refresh,2000);</script></body></html>";

static void security_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'");
}

static esp_err_t send_json(httpd_req_t *request, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
        return ESP_FAIL;
    }
    security_headers(request);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *request, int status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root) cJSON_AddStringToObject(root, "error", message);
    httpd_resp_set_status(request, status == 400 ? "400 Bad Request" :
                                  status == 401 ? "401 Unauthorized" :
                                  status == 404 ? "404 Not Found" :
                                  status == 405 ? "405 Method Not Allowed" :
                                  status == 408 ? "408 Request Timeout" :
                                  status == 411 ? "411 Length Required" :
                                  status == 413 ? "413 Payload Too Large" :
                                  status == 415 ? "415 Unsupported Media Type" :
                                  "500 Internal Server Error");
    return send_json(request, root);
}

static bool json_depth_ok(const char *json, size_t len)
{
    int depth = 0;
    bool in_string = false, escaped = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)json[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') {
            if (++depth > 8) return false;
        } else if (c == '}' || c == ']') {
            if (--depth < 0) return false;
        }
    }
    return depth == 0 && !in_string && !escaped;
}

static esp_err_t reject_oversized_body(httpd_req_t *request)
{
    if (httpd_req_get_hdr_value_len(request, "Transfer-Encoding") == 0 &&
        request->content_len <= HTTP_BODY_MAX) return ESP_OK;
    esp_err_t result = send_error(request,
        httpd_req_get_hdr_value_len(request, "Transfer-Encoding") != 0 ? 411 : 413,
        httpd_req_get_hdr_value_len(request, "Transfer-Encoding") != 0 ?
            "Transfer-Encoding is not supported" : "JSON body too large");
    int sockfd = httpd_req_to_sockfd(request);
    if (sockfd >= 0) httpd_sess_trigger_close(s_server, sockfd);
    return result;
}

static bool token_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la ^ lb);
    for (size_t i = 0; i < la || i < lb; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= ca ^ cb;
    }
    return diff == 0;
}

static esp_err_t require_admin(httpd_req_t *request)
{
    companion_config_t config;
    if (companion_config_get(&config) != ESP_OK || config.admin_token[0] == '\0')
        return send_error(request, 401, "admin token required");
    size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    char header[COMPANION_ADMIN_TOKEN_MAX + 16];
    if (length < 7 || length >= sizeof(header) ||
        httpd_req_get_hdr_value_str(request, "Authorization", header, sizeof(header)) != ESP_OK ||
        strncmp(header, "Bearer ", 7) != 0 ||
        !token_equal(header + 7, config.admin_token)) {
        httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer");
        return send_error(request, 401, "invalid admin token");
    }
    return ESP_OK;
}

static cJSON *read_json_body(httpd_req_t *request)
{
    if (httpd_req_get_hdr_value_len(request, "Transfer-Encoding") != 0) {
        send_error(request, 411, "Transfer-Encoding is not supported");
        return NULL;
    }
    size_t length = request->content_len;
    if (length == 0) {
        send_error(request, 411, "JSON body required");
        return NULL;
    }
    if (length > HTTP_BODY_MAX) {
        send_error(request, 413, "JSON body too large");
        return NULL;
    }
    size_t type_len = httpd_req_get_hdr_value_len(request, "Content-Type");
    char content_type[64];
    if (type_len == 0 || type_len >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16) != 0) {
        send_error(request, 415, "Content-Type must be application/json");
        return NULL;
    }
    char *body = malloc(length + 1);
    if (!body) {
        send_error(request, 500, "out of memory");
        return NULL;
    }
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + HTTP_BODY_TIMEOUT_US;
    while (received < length) {
        if (esp_timer_get_time() >= deadline) {
            free(body);
            send_error(request, 408, "request body timeout");
            return NULL;
        }
        int count = httpd_req_recv(request, body + received, length - received);
        if (count <= 0) {
            free(body);
            send_error(request, 400, "incomplete request body");
            return NULL;
        }
        received += (size_t)count;
    }
    body[length] = '\0';
    if (memchr(body, '\0', length) || !json_depth_ok(body, length)) {
        free(body);
        send_error(request, 400, "invalid JSON");
        return NULL;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(body, length + 1, &end, true);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        free(body);
        send_error(request, 400, "JSON object required");
        return NULL;
    }
    while (end && *end && isspace((unsigned char)*end)) end++;
    if (!end || *end != '\0') {
        cJSON_Delete(root);
        free(body);
        send_error(request, 400, "invalid JSON");
        return NULL;
    }
    free(body);
    return root;
}

static bool copy_field_string(const cJSON *root, const char *key, char *out,
                              size_t capacity, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) return !required;
    if (!cJSON_IsString(item) || !item->valuestring ||
        strnlen(item->valuestring, capacity) >= capacity) return false;
    strlcpy(out, item->valuestring, capacity);
    return true;
}

static bool json_int_range(const cJSON *item, int minimum, int maximum, int *out)
{
    if (!item || !cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum ||
        item->valuedouble != (double)(int)item->valuedouble) return false;
    *out = (int)item->valuedouble;
    return true;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    security_headers(request);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get_handler(httpd_req_t *request)
{
    companion_config_t config;
    if (companion_config_get(&config) != ESP_OK) return send_error(request, 500, "config unavailable");
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_error(request, 500, "out of memory");
    cJSON_AddStringToObject(root, "wifi_ssid", config.wifi_ssid);
    cJSON_AddBoolToObject(root, "wifi_password_set", config.wifi_password[0] != '\0');
    cJSON_AddStringToObject(root, "ota_url", config.ota_url);
    cJSON_AddBoolToObject(root, "admin_token_set", config.admin_token[0] != '\0');
    cJSON_AddNumberToObject(root, "brightness", config.brightness);
    cJSON_AddNumberToObject(root, "daily_goal", config.daily_goal);
    return send_json(request, root);
}

static const char *wifi_mode_name(wifi_mode_t mode)
{
    if (mode == WIFI_MODE_AP) return "AP";
    if (mode == WIFI_MODE_APSTA) return "AP+STA";
    if (mode == WIFI_MODE_STA) return "STA";
    return "off";
}

static const char *ota_state_name(pad_ota_state_t state)
{
    static const char *const names[] = {"idle", "queued", "downloading", "validating",
                                         "ready", "rebooting", "failed"};
    return state >= PAD_OTA_IDLE && state <= PAD_OTA_FAILED ? names[state] : "unknown";
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    companion_state_t state;
    companion_model_snapshot(&state);
    pad_ota_status_t ota;
    pad_ota_get_status(&ota);
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    const char *if_key = mode == WIFI_MODE_AP ? "WIFI_AP_DEF" : "WIFI_STA_DEF";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    esp_netif_ip_info_t ip_info = {0};
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr == 0)
        memset(&ip_info, 0, sizeof(ip_info));
    char ip[16] = "-";
    if (netif && ip_info.ip.addr) esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    int rssi = 0;
    if (companion_network_is_connected()) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    cJSON *root = cJSON_CreateObject();
    cJSON *timer = cJSON_AddObjectToObject(root, "timer");
    cJSON *ota_json = cJSON_AddObjectToObject(root, "ota");
    if (!root || !timer || !ota_json) {
        cJSON_Delete(root);
        return send_error(request, 500, "out of memory");
    }
    cJSON_AddBoolToObject(root, "connected", companion_network_is_connected());
    cJSON_AddStringToObject(root, "wifi_mode", wifi_mode_name(mode));
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddBoolToObject(root, "time_valid", state.time_valid);
    cJSON_AddStringToObject(root, "local_time", state.clock);
    cJSON_AddStringToObject(root, "local_date", state.date);
    cJSON_AddStringToObject(root, "firmware", app ? app->version : "unknown");
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddStringToObject(timer, "phase", companion_phase_name(state.phase));
    cJSON_AddNumberToObject(timer, "remaining_seconds", state.remaining_seconds);
    cJSON_AddNumberToObject(timer, "total_seconds", state.total_seconds);
    cJSON_AddNumberToObject(timer, "completed_sessions", state.completed_sessions);
    cJSON_AddNumberToObject(timer, "daily_goal", state.daily_goal);
    cJSON_AddNumberToObject(timer, "streak", state.streak);
    cJSON_AddBoolToObject(timer, "standby", state.standby);
    cJSON_AddBoolToObject(timer, "work_hours_override", state.work_hours_override);
    cJSON_AddStringToObject(ota_json, "state", ota_state_name(ota.state));
    cJSON_AddNumberToObject(ota_json, "state_code", ota.state);
    cJSON_AddNumberToObject(ota_json, "bytes_written", ota.bytes_written);
    cJSON_AddNumberToObject(ota_json, "total_bytes", ota.total_bytes);
    cJSON_AddNumberToObject(ota_json, "error", ota.error);
    cJSON_AddBoolToObject(ota_json, "rollback_supported", ota.rollback_supported);
    cJSON_AddStringToObject(ota_json, "running_partition", running ? running->label : "unknown");
    return send_json(request, root);
}

static esp_err_t config_post_handler(httpd_req_t *request)
{
    esp_err_t preflight = reject_oversized_body(request);
    if (preflight != ESP_OK) return preflight;
    esp_err_t auth = require_admin(request);
    if (auth != ESP_OK) return auth;
    cJSON *root = read_json_body(request);
    if (!root) return ESP_OK;
    companion_config_t next = {0};
    bool valid = companion_config_get(&next) == ESP_OK &&
                 copy_field_string(root, "wifi_ssid", next.wifi_ssid, sizeof(next.wifi_ssid), false) &&
                 copy_field_string(root, "ota_url", next.ota_url, sizeof(next.ota_url), false);
    int brightness = next.brightness;
    const cJSON *brightness_item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (brightness_item && !json_int_range(brightness_item, 5, 100, &brightness)) valid = false;
    next.brightness = (uint8_t)brightness;
    int daily_goal = next.daily_goal;
    const cJSON *goal_item = cJSON_GetObjectItemCaseSensitive(root, "daily_goal");
    if (goal_item && !json_int_range(goal_item, (int)COMPANION_DAILY_GOAL_MIN,
                                     (int)COMPANION_DAILY_GOAL_MAX, &daily_goal))
        valid = false;
    next.daily_goal = (uint8_t)daily_goal;

    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
    if (password && (!cJSON_IsString(password) || !password->valuestring)) valid = false;
    else if (password && password->valuestring[0] &&
             !copy_field_string(root, "wifi_password", next.wifi_password,
                                sizeof(next.wifi_password), false)) valid = false;
    const cJSON *clear_password = cJSON_GetObjectItemCaseSensitive(root, "clear_wifi_password");
    if (clear_password && !cJSON_IsBool(clear_password)) valid = false;
    if (cJSON_IsBool(clear_password) && cJSON_IsTrue(clear_password)) {
        if (password && cJSON_IsString(password) && password->valuestring && password->valuestring[0])
            valid = false;
        else next.wifi_password[0] = '\0';
    }
    const cJSON *reboot = cJSON_GetObjectItemCaseSensitive(root, "reboot");
    bool reboot_after_save = false;
    if (reboot) {
        if (!cJSON_IsBool(reboot)) valid = false;
        else reboot_after_save = cJSON_IsTrue(reboot);
    }
    if (valid) valid = companion_config_validate(&next) == ESP_OK;
    esp_err_t result = valid ? companion_network_apply_config(&next) : ESP_ERR_INVALID_ARG;
    if (result == ESP_OK) companion_model_set_daily_goal(next.daily_goal);
    cJSON_Delete(root);
    if (result != ESP_OK) return send_error(request, result == ESP_ERR_INVALID_ARG ? 400 : 500,
                                            result == ESP_ERR_INVALID_ARG ? "invalid settings" : "failed to save settings");
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "saved", true);
    cJSON_AddBoolToObject(response, "connected", companion_network_is_connected());
    cJSON_AddBoolToObject(response, "rebooting", reboot_after_save);
    esp_err_t send_result = send_json(request, response);
    if (reboot_after_save && send_result == ESP_OK) companion_network_request_restart();
    return send_result;
}

static esp_err_t ota_post_handler(httpd_req_t *request)
{
    esp_err_t preflight = reject_oversized_body(request);
    if (preflight != ESP_OK) return preflight;
    esp_err_t auth = require_admin(request);
    if (auth != ESP_OK) return auth;
    cJSON *root = read_json_body(request);
    if (!root) return ESP_OK;
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(url) && url->valuestring) result = pad_ota_start(url->valuestring);
    cJSON_Delete(root);
    if (result != ESP_OK) return send_error(request, result == ESP_ERR_INVALID_ARG ? 400 : 409,
                                            result == ESP_ERR_INVALID_ARG ? "invalid URL" : "OTA is busy or unsupported");
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    cJSON_AddStringToObject(response, "message", "OTA accepted");
    return send_json(request, response);
}

static esp_err_t ota_cancel_handler(httpd_req_t *request)
{
    esp_err_t preflight = reject_oversized_body(request);
    if (preflight != ESP_OK) return preflight;
    esp_err_t auth = require_admin(request);
    if (auth != ESP_OK) return auth;
    esp_err_t result = pad_ota_cancel();
    if (result != ESP_OK) return send_error(request, 409, "OTA is not active");
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    return send_json(request, response);
}

static esp_err_t restart_post_handler(httpd_req_t *request)
{
    esp_err_t preflight = reject_oversized_body(request);
    if (preflight != ESP_OK) return preflight;
    esp_err_t auth = require_admin(request);
    if (auth != ESP_OK) return auth;
    companion_network_request_restart();
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    return send_json(request, response);
}

static esp_err_t timer_focus_post_handler(httpd_req_t *request)
{
    esp_err_t preflight = reject_oversized_body(request);
    if (preflight != ESP_OK) return preflight;
    esp_err_t auth = require_admin(request);
    if (auth != ESP_OK) return auth;
    /* Body is optional; accept empty or {}. */
    if (request->content_len > 0) {
        cJSON *root = read_json_body(request);
        if (!root) return ESP_OK;
        cJSON_Delete(root);
    }
    companion_model_start_fresh_focus();
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    cJSON_AddStringToObject(response, "phase", "focus");
    return send_json(request, response);
}

static esp_err_t timer_standby_post_handler(httpd_req_t *request)
{
    esp_err_t preflight = reject_oversized_body(request);
    if (preflight != ESP_OK) return preflight;
    esp_err_t auth = require_admin(request);
    if (auth != ESP_OK) return auth;
    if (request->content_len > 0) {
        cJSON *root = read_json_body(request);
        if (!root) return ESP_OK;
        cJSON_Delete(root);
    }
    companion_model_standby_until_morning();
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    cJSON_AddStringToObject(response, "phase", "standby");
    return send_json(request, response);
}

void companion_provisioning_start(void)
{
    if (s_server) return;
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.httpd.stack_size = 12288;
    config.httpd.recv_wait_timeout = 2;
    config.httpd.send_wait_timeout = 5;
    config.httpd.max_open_sockets = 1;
    config.httpd.max_uri_handlers = 11;
    config.servercert = admin_server_cert;
    config.servercert_len = sizeof(admin_server_cert) - 1;
    config.prvtkey_pem = admin_server_key;
    config.prvtkey_len = sizeof(admin_server_key) - 1;
    if (httpd_ssl_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS server start failed");
        return;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
    httpd_uri_t config_get = {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler};
    httpd_uri_t config_post = {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler};
    httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t ota = {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler};
    httpd_uri_t ota_cancel = {.uri = "/api/ota/cancel", .method = HTTP_POST, .handler = ota_cancel_handler};
    httpd_uri_t restart = {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post_handler};
    httpd_uri_t focus = {.uri = "/api/timer/focus", .method = HTTP_POST, .handler = timer_focus_post_handler};
    httpd_uri_t standby = {.uri = "/api/timer/standby", .method = HTTP_POST, .handler = timer_standby_post_handler};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &config_get);
    httpd_register_uri_handler(s_server, &config_post);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &ota);
    httpd_register_uri_handler(s_server, &ota_cancel);
    httpd_register_uri_handler(s_server, &restart);
    httpd_register_uri_handler(s_server, &focus);
    httpd_register_uri_handler(s_server, &standby);
    ESP_LOGI(TAG, "settings HTTPS server started");
}
