#pragma once

#include <map>
#include <string>

#include <curl/curl.h>

#include "attoclaw/common.hpp"

namespace attoclaw {

struct HttpResponse {
  long status{0};
  std::string body;
  std::string final_url;
  std::string error;
};

class HttpClient {
 public:
  HttpClient() { ensure_global_init(); }

  HttpResponse get(const std::string& url, const std::map<std::string, std::string>& headers = {},
                   int timeout_s = 30, bool follow_redirects = true, long max_redirects = 5) {
    return request("GET", url, "", headers, timeout_s, follow_redirects, max_redirects);
  }

  HttpResponse post(const std::string& url, const std::string& body,
                    const std::map<std::string, std::string>& headers = {}, int timeout_s = 60,
                    bool follow_redirects = true, long max_redirects = 5) {
    return request("POST", url, body, headers, timeout_s, follow_redirects, max_redirects);
  }

 private:
  static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const auto n = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, n);
    return n;
  }

  static void ensure_global_init() {
    static std::once_flag flag;
    std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  }

  HttpResponse request(const std::string& method, const std::string& url, const std::string& body,
                       const std::map<std::string, std::string>& headers, int timeout_s,
                       bool follow_redirects, long max_redirects) {
    CURL* curl = curl_easy_init();
    if (!curl) {
      return HttpResponse{0, "", "", "curl init failed"};
    }

    std::string response_body;
    struct curl_slist* header_list = nullptr;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_redirects);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "attoclaw/0.1");

    if (method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    for (const auto& [k, v] : headers) {
      const std::string line = k + ": " + v;
      header_list = curl_slist_append(header_list, line.c_str());
    }
    if (header_list) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    const CURLcode rc = curl_easy_perform(curl);

    HttpResponse out;
    if (rc != CURLE_OK) {
      out.error = curl_easy_strerror(rc);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
    char* final_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
    out.final_url = final_url ? std::string(final_url) : url;
    out.body = std::move(response_body);

    if (header_list) {
      curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);
    return out;
  }
};

}  // namespace attoclaw


