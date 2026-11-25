#include "http_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app_display.h"
#include "esp_http_server.h"
#include "esp_log.h"

#define TAG "httpdbg"

static httpd_handle_t s_server;

/* 读取整数型查询参数，缺省返回 -1。 */
static int query_int(httpd_req_t *req, const char *key)
{
    const size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len <= 1) return -1;

    char *buf = malloc(len);
    if (buf == NULL) return -1;

    int value = -1;
    if (httpd_req_get_url_query_str(req, buf, len) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, key, param, sizeof(param)) == ESP_OK) {
            value = atoi(param);
        }
    }
    free(buf);
    return value;
}

static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    /* 调试命令不能被缓存：否则浏览器会回退到旧的页面/结果。 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, text);
}

/* 所有按钮都走 fetch，不发导航：否则浏览器会跳到 /page 的纯文本结果页，
   用户只能手动返回才能继继操作。 */
static const char k_html[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>WiTime 调试</title><style>"
    "body{font-family:sans-serif;margin:20px;background:#101418;color:#e6edf3}"
    "button{display:block;width:100%;padding:18px;margin:12px 0;background:#2a303a;"
    "color:#fff;border:0;border-radius:10px;font-size:18px}"
    "button:active{background:#3d4652}"
    "#st{margin-top:16px;font-size:15px;color:#8fd694;min-height:22px}"
    "#vol button{display:inline-block;width:12%;margin:0 1%;font-size:15px;padding:12px 0}"
    "#voice button{display:inline-block;width:30%;margin:1.5% 1%;font-size:13px;padding:10px 0}"
    ".h{font-size:12px;color:#8b949e;margin-top:14px;line-height:1.6}"
    "</style></head><body><h3>WiTime 页面调试</h3>"
    "<button onclick=\"cmd('/page?n=0')\">主页面</button>"
    "<button onclick=\"cmd('/page?n=1')\">闹钟页</button>"
    "<button onclick=\"cmd('/page?n=2')\">语音页</button>"
    "<button onclick=\"cmd('/alarm?on=1')\">闹钟 开</button>"
    "<button onclick=\"cmd('/alarm?on=0')\">闹钟 关</button>"
    "<div id=\"vol\"></div>"
    "<div id=\"voice\"></div>"
    "<div id=\"st\">就绪</div>"
    "<div class=\"h\">也可直接访问 /page?n=0..2 、 /alarm?on=1 、 /alarm?vol=1..7 、 /voice?s=0..5</div>"
    "<script>"
    "function cmd(u){"
    "fetch(u,{cache:'no-store'})"
    ".then(function(r){return r.text()})"
    ".then(function(t){document.getElementById('st').textContent=t.trim()})"
    ".catch(function(e){document.getElementById('st').textContent='失败: '+e});"
    "}"
    "var vbox=document.getElementById('vol');"
    "for(var i=1;i<=7;i++){(function(n){"
    "var b=document.createElement('button');b.textContent=n;"
    "b.onclick=function(){cmd('/alarm?vol='+n)};vbox.appendChild(b);"
    "})(i);}"
    "var names=['idle','awake','listen','thinking','speaking','muted','unknown'];"
    "var obox=document.getElementById('voice');"
    "for(var k=0;k<names.length;k++){(function(n){"
    "var b=document.createElement('button');b.textContent=names[n];"
    "b.onclick=function(){cmd('/voice?s='+n)};obox.appendChild(b);"
    "})(k);}"
    "</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, k_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t page_handler(httpd_req_t *req)
{
    static const char *const k_page_reply[] = {"page=main\n", "page=alarm\n", "page=voice\n"};
    const int page = query_int(req, "n");
    if (page < 0 || page >= (int)(sizeof(k_page_reply) / sizeof(k_page_reply[0]))) {
        return send_text(req, "usage: /page?n=0|1|2\n");
    }

    app_display_set_page(page);
    ESP_LOGI(TAG, "page <- %d", page);
    return send_text(req, k_page_reply[page]);
}

static esp_err_t alarm_handler(httpd_req_t *req)
{
    const int on = query_int(req, "on");
    const int vol = query_int(req, "vol");
    if (on < 0 && vol < 0) {
        return send_text(req, "usage: /alarm?on=0|1 or /alarm?vol=1..7\n");
    }

    char reply[32];
    reply[0] = '\0';
    if (on >= 0) {
        app_display_set_alarm_enabled(on != 0);
        ESP_LOGI(TAG, "alarm <- %s", on != 0 ? "on" : "off");
        snprintf(reply, sizeof(reply), "alarm=%s", on != 0 ? "on" : "off");
    }
    if (vol >= 0) {
        app_display_set_alarm_volume((uint8_t)vol);
        ESP_LOGI(TAG, "alarm volume <- %d", vol);
        const size_t used = strlen(reply);
        snprintf(reply + used, sizeof(reply) - used, "%svol=%d", used ? " " : "", vol);
    }
    strncat(reply, "\n", sizeof(reply) - strlen(reply) - 1U);
    return send_text(req, reply);
}

static esp_err_t voice_handler(httpd_req_t *req)
{
    const int state = query_int(req, "s");
    if (state < 0) return send_text(req, "usage: /voice?s=0..5\n");

    app_display_set_voice_state(state);
    ESP_LOGI(TAG, "voice state <- %d", state);

    /* 24 字节："voice=" + 最长 11 位整数 + "\n" 仍有富余，避免 format-truncation 误报。 */
    char reply[24];
    snprintf(reply, sizeof(reply), "voice=%d\n", state);
    return send_text(req, reply);
}

esp_err_t http_debug_start(void)
{
    if (s_server != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.stack_size = 4096;
    /* 不复用连接：每个请求完毕就关，避免调试页面残留的长连接把槽位占满。 */
    config.keep_alive_enable = false;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        /* Wi-Fi 尚未就绪时这里会失败，属预期，不阻断启动流程。 */
        ESP_LOGW(TAG, "http start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    const httpd_uri_t page_uri = {
        .uri = "/page", .method = HTTP_GET, .handler = page_handler, .user_ctx = NULL};
    const httpd_uri_t alarm_uri = {
        .uri = "/alarm", .method = HTTP_GET, .handler = alarm_handler, .user_ctx = NULL};
    const httpd_uri_t voice_uri = {
        .uri = "/voice", .method = HTTP_GET, .handler = voice_handler, .user_ctx = NULL};

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &page_uri);
    httpd_register_uri_handler(s_server, &alarm_uri);
    httpd_register_uri_handler(s_server, &voice_uri);

    ESP_LOGI(TAG, "debug http ready: http://<device-ip>/");
    return ESP_OK;
}

void http_debug_stop(void)
{
    if (s_server == NULL) return;
    httpd_stop(s_server);
    s_server = NULL;
}
