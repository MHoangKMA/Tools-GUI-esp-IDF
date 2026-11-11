#include "wifi_portal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <strings.h>
#include <utility>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#define TAG "WifiPortal"

static constexpr const char* kProbeUrl = "http://connectivitycheck.gstatic.com/generate_204";
static constexpr const char* kUserAgent =
    "Mozilla/5.0 (Linux; Android 10; ESP32) AppleWebKit/537.36 (KHTML, like Gecko) Mobile Safari/537.36";
static constexpr size_t kMaxBody = 16 * 1024;
static constexpr int kProbeTimeoutMs = 3000;
static constexpr int kPageTimeoutMs = 8000;
static constexpr int kMaxHops = 6;
static constexpr int kLoginAttempts = 3;

// ==================== HTTP helper ====================

struct HttpResult {
    int status = 0;  // 0 = transport failure
    std::string body;
    std::string location;
    std::vector<std::string> set_cookies;
};

static esp_err_t HttpEvent(esp_http_client_event_t* evt) {
    auto* result = static_cast<HttpResult*>(evt->user_data);
    if (result == nullptr) {
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "Location") == 0) {
            result->location = evt->header_value;
        } else if (strcasecmp(evt->header_key, "Set-Cookie") == 0) {
            result->set_cookies.push_back(evt->header_value);
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (result->body.size() < kMaxBody && evt->data != nullptr) {
            size_t room = kMaxBody - result->body.size();
            result->body.append(static_cast<const char*>(evt->data),
                                std::min(room, static_cast<size_t>(evt->data_len)));
        }
    }
    return ESP_OK;
}

class CookieJar {
public:
    void Absorb(const std::vector<std::string>& set_cookies) {
        for (const auto& line : set_cookies) {
            std::string pair = line.substr(0, line.find(';'));
            auto eq = pair.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string name = pair.substr(0, eq);
            auto it = std::find_if(cookies_.begin(), cookies_.end(),
                                   [&](const std::pair<std::string, std::string>& c) { return c.first == name; });
            if (it != cookies_.end()) {
                it->second = pair.substr(eq + 1);
            } else {
                cookies_.emplace_back(name, pair.substr(eq + 1));
            }
        }
    }
    std::string Header() const {
        std::string header;
        for (const auto& c : cookies_) {
            if (!header.empty()) {
                header += "; ";
            }
            header += c.first + "=" + c.second;
        }
        return header;
    }

private:
    std::vector<std::pair<std::string, std::string>> cookies_;
};

static HttpResult Request(const std::string& url, bool post, const std::string& post_body,
                          CookieJar& jar, int timeout_ms) {
    HttpResult result;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = timeout_ms;
    config.disable_auto_redirect = true;  // we need to see every hop and its cookies
    config.event_handler = HttpEvent;
    config.user_data = &result;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_agent = kUserAgent;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return result;
    }
    std::string cookie = jar.Header();
    if (!cookie.empty()) {
        esp_http_client_set_header(client, "Cookie", cookie.c_str());
    }
    if (post) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, post_body.c_str(), post_body.size());
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        result.status = esp_http_client_get_status_code(client);
        jar.Absorb(result.set_cookies);
    } else {
        ESP_LOGW(TAG, "HTTP %s %s failed: %s", post ? "POST" : "GET", url.c_str(), esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return result;
}

static bool IsRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// ==================== URL / HTML helpers ====================

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static std::string Resolve(const std::string& base, const std::string& ref) {
    if (ref.empty()) {
        return base;
    }
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) {
        return ref;
    }
    auto scheme_end = base.find("://");
    if (scheme_end == std::string::npos) {
        return ref;
    }
    if (ref.rfind("//", 0) == 0) {
        return base.substr(0, scheme_end + 1) + ref;
    }
    auto path_start = base.find('/', scheme_end + 3);
    std::string origin = path_start == std::string::npos ? base : base.substr(0, path_start);
    if (ref[0] == '/') {
        return origin + ref;
    }
    std::string path = path_start == std::string::npos ? "/" : base.substr(path_start);
    path = path.substr(0, path.find_first_of("?#"));
    path = path.substr(0, path.rfind('/') + 1);
    return origin + path + ref;
}

static std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string DecodeEntities(std::string s) {
    const std::pair<const char*, const char*> table[] = {
        {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}};
    for (const auto& [from, to] : table) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, strlen(from), to);
            pos += strlen(to);
        }
    }
    return s;
}

// Reads attribute `attr` from a single tag ("<input type=text name=u>").
static std::string GetAttr(const std::string& tag, const char* attr) {
    std::string lower = ToLower(tag);
    size_t len = strlen(attr);
    size_t pos = 0;
    while ((pos = lower.find(attr, pos)) != std::string::npos) {
        bool boundary = pos > 0 && std::isspace(static_cast<unsigned char>(lower[pos - 1]));
        size_t e = pos + len;
        while (e < lower.size() && std::isspace(static_cast<unsigned char>(lower[e]))) {
            e++;
        }
        if (boundary && e < lower.size() && lower[e] == '=') {
            e++;
            while (e < lower.size() && std::isspace(static_cast<unsigned char>(lower[e]))) {
                e++;
            }
            if (e >= tag.size()) {
                return "";
            }
            if (tag[e] == '"' || tag[e] == '\'') {
                char quote = tag[e];
                size_t end = tag.find(quote, e + 1);
                return DecodeEntities(tag.substr(e + 1, end == std::string::npos ? std::string::npos : end - e - 1));
            }
            size_t end = tag.find_first_of(" \t\r\n>", e);
            return DecodeEntities(tag.substr(e, end == std::string::npos ? std::string::npos : end - e));
        }
        pos += len;
    }
    return "";
}

static bool Contains(const std::string& haystack_lower, std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (haystack_lower.find(n) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct LoginForm {
    bool found = false;
    bool post = true;
    std::string action;
    std::vector<std::pair<std::string, std::string>> fields;
    bool has_password_field = false;
};

// Picks the form holding a password field (else the first form) and fills it.
static LoginForm BuildForm(const std::string& html, const std::string& username,
                           const std::string& password) {
    LoginForm form;
    std::string lower = ToLower(html);

    // Locate candidate forms
    std::vector<std::pair<size_t, size_t>> forms;
    size_t pos = 0;
    while ((pos = lower.find("<form", pos)) != std::string::npos) {
        size_t end = lower.find("</form", pos);
        end = end == std::string::npos ? lower.size() : end;
        forms.emplace_back(pos, end);
        pos = end;
    }
    if (forms.empty()) {
        return form;
    }
    auto chosen = forms[0];
    for (const auto& f : forms) {
        if (lower.substr(f.first, f.second - f.first).find("type=\"password\"") != std::string::npos ||
            lower.substr(f.first, f.second - f.first).find("type='password'") != std::string::npos ||
            lower.substr(f.first, f.second - f.first).find("type=password") != std::string::npos) {
            chosen = f;
            break;
        }
    }

    std::string block = html.substr(chosen.first, chosen.second - chosen.first);
    std::string block_lower = lower.substr(chosen.first, chosen.second - chosen.first);

    size_t form_tag_end = block.find('>');
    std::string form_tag = block.substr(0, form_tag_end == std::string::npos ? block.size() : form_tag_end + 1);
    form.found = true;
    form.action = GetAttr(form_tag, "action");
    form.post = ToLower(GetAttr(form_tag, "method")) != "get";

    bool username_assigned = false;
    bool submit_added = false;
    size_t p = 0;
    while ((p = block_lower.find("<input", p)) != std::string::npos) {
        size_t end = block.find('>', p);
        std::string tag = block.substr(p, end == std::string::npos ? std::string::npos : end - p + 1);
        p = end == std::string::npos ? block.size() : end;

        std::string name = GetAttr(tag, "name");
        std::string type = ToLower(GetAttr(tag, "type"));
        std::string value = GetAttr(tag, "value");
        std::string name_lower = ToLower(name);
        if (name.empty() && type != "submit") {
            continue;
        }

        if (type == "password") {
            form.fields.emplace_back(name, password);
            form.has_password_field = true;
        } else if (type == "hidden") {
            form.fields.emplace_back(name, value);
        } else if (type == "checkbox") {
            bool consent = Contains(name_lower, {"accept", "agree", "term", "tos", "policy", "consent", "conditions"});
            if (consent || ToLower(tag).find("checked") != std::string::npos) {
                form.fields.emplace_back(name, value.empty() ? "on" : value);
            }
        } else if (type == "submit" || type == "image") {
            if (!submit_added && !name.empty()) {
                form.fields.emplace_back(name, value);
                submit_added = true;
            }
        } else if (type == "button" || type == "reset" || type == "radio" || type == "file") {
            continue;
        } else {  // text, email, tel, number or missing type
            if (!username_assigned && !username.empty()) {
                form.fields.emplace_back(name, username);
                username_assigned = true;
            } else {
                form.fields.emplace_back(name, value);
            }
        }
    }
    return form;
}

// Looks for a client-side redirect: <meta http-equiv=refresh> or window.location = '...'.
static std::string FindScriptRedirect(const std::string& html) {
    std::string lower = ToLower(html);
    size_t pos = lower.find("http-equiv=\"refresh\"");
    if (pos == std::string::npos) {
        pos = lower.find("http-equiv='refresh'");
    }
    if (pos != std::string::npos) {
        size_t tag_start = lower.rfind('<', pos);
        size_t tag_end = html.find('>', pos);
        if (tag_start != std::string::npos && tag_end != std::string::npos) {
            std::string content = GetAttr(html.substr(tag_start, tag_end - tag_start + 1), "content");
            auto url_pos = ToLower(content).find("url=");
            if (url_pos != std::string::npos) {
                std::string url = content.substr(url_pos + 4);
                if (!url.empty() && (url[0] == '\'' || url[0] == '"')) {
                    url = url.substr(1);
                }
                while (!url.empty() && (url.back() == '\'' || url.back() == '"' || url.back() == ' ')) {
                    url.pop_back();
                }
                return url;
            }
        }
    }
    for (const char* key : {"window.location.href", "window.location.replace(", "window.location=", "window.location =",
                            "location.href=", "location.href ="}) {
        pos = lower.find(key);
        if (pos == std::string::npos) {
            continue;
        }
        size_t q = html.find_first_of("'\"", pos + strlen(key));
        if (q == std::string::npos) {
            continue;
        }
        size_t q_end = html.find(html[q], q + 1);
        if (q_end != std::string::npos) {
            return html.substr(q + 1, q_end - q - 1);
        }
    }
    return "";
}

// ==================== Probe / login ====================

struct ProbeResult {
    enum Kind { kOnline, kPortal, kUnreachable } kind = kUnreachable;
    std::string url;  // where the login flow starts
};

static ProbeResult Probe() {
    CookieJar jar;
    HttpResult res = Request(kProbeUrl, false, "", jar, kProbeTimeoutMs);
    ProbeResult probe;
    if (res.status == 204 && res.body.empty()) {
        probe.kind = ProbeResult::kOnline;
    } else if (IsRedirect(res.status) && !res.location.empty()) {
        probe.kind = ProbeResult::kPortal;
        probe.url = Resolve(kProbeUrl, res.location);
    } else if (res.status == 200 && !res.body.empty()) {
        probe.kind = ProbeResult::kPortal;
        probe.url = kProbeUrl;  // portal answered in place of the real server
    }
    return probe;
}

// Walks redirects to the login page, fills and submits the form. True when a form was
// submitted; the caller re-probes to see whether it worked.
static bool SubmitPortalLogin(const std::string& start_url, const std::string& username,
                              const std::string& password, const std::atomic<bool>* cancel) {
    CookieJar jar;
    std::string url = start_url;
    for (int hop = 0; hop < kMaxHops; hop++) {
        if (cancel && cancel->load()) {
            return false;
        }
        HttpResult page = Request(url, false, "", jar, kPageTimeoutMs);
        if (IsRedirect(page.status) && !page.location.empty()) {
            url = Resolve(url, page.location);
            continue;
        }
        if (page.status != 200) {
            ESP_LOGW(TAG, "Portal page %s returned status %d", url.c_str(), page.status);
            return false;
        }

        LoginForm form = BuildForm(page.body, username, password);
        if (!form.found || (form.has_password_field && password.empty())) {
            std::string next = FindScriptRedirect(page.body);
            if (!next.empty() && !form.found) {
                url = Resolve(url, next);
                continue;
            }
            ESP_LOGW(TAG, "%s", !form.found ? "No login form on the portal page (JavaScript-only portal?)"
                                           : "Portal asks for a password but none is saved");
            return false;
        }

        std::string body;
        for (const auto& [name, value] : form.fields) {
            if (!body.empty()) {
                body += '&';
            }
            body += UrlEncode(name) + "=" + UrlEncode(value);
        }
        std::string target = Resolve(url, form.action);
        ESP_LOGI(TAG, "Submitting portal form (%s) to %s, %d fields",
                 form.post ? "POST" : "GET", target.c_str(), static_cast<int>(form.fields.size()));
        if (!form.post) {
            target += (target.find('?') == std::string::npos ? "?" : "&") + body;
        }
        HttpResult reply = Request(target, form.post, body, jar, kPageTimeoutMs);

        // Follow the redirect chain after the login so the portal can whitelist us
        for (int i = 0; i < kMaxHops && IsRedirect(reply.status) && !reply.location.empty(); i++) {
            target = Resolve(target, reply.location);
            reply = Request(target, false, "", jar, kPageTimeoutMs);
        }
        return reply.status != 0;
    }
    return false;
}

PortalResult WifiPortal::EnsureOnline(const std::string& username, const std::string& password,
                                      const std::atomic<bool>* cancel) {
    ProbeResult probe = Probe();
    if (probe.kind == ProbeResult::kOnline) {
        ESP_LOGI(TAG, "Internet reachable, no captive portal");
        return PortalResult::Online;
    }
    if (probe.kind == ProbeResult::kUnreachable) {
        ESP_LOGW(TAG, "Connectivity probe did not complete; assuming a normal network");
        return PortalResult::Unreachable;
    }

    ESP_LOGW(TAG, "Captive portal detected: %s", probe.url.c_str());
    for (int attempt = 1; attempt <= kLoginAttempts; attempt++) {
        if (cancel && cancel->load()) {
            return PortalResult::Failed;
        }
        if (SubmitPortalLogin(probe.url, username, password, cancel)) {
            vTaskDelay(pdMS_TO_TICKS(1500));  // let the gateway open the firewall
            if (Probe().kind == ProbeResult::kOnline) {
                ESP_LOGI(TAG, "Captive portal passed (attempt %d)", attempt);
                return PortalResult::LoggedIn;
            }
        }
        ESP_LOGW(TAG, "Portal login attempt %d/%d did not open the internet", attempt, kLoginAttempts);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return PortalResult::Failed;
}
