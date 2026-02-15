#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <cstdio>

#include <curl/curl.h>

#include "attoclaw/common.hpp"

namespace attoclaw {

struct HttpResponse {
  long status{0};
  std::string body;
  std::string final_url;
  std::string error;
  std::map<std::string, std::string> headers{};
};

struct MultipartField {
  std::string name;
  std::string value;
};

class HttpClient {
 public:
  HttpClient() {
    ensure_global_init();
    easy_ = curl_easy_init();
  }

  ~HttpClient() {
    if (easy_) {
      curl_easy_cleanup(easy_);
      easy_ = nullptr;
    }
  }

  HttpResponse get(const std::string& url, const std::map<std::string, std::string>& headers = {},
                   int timeout_s = 30, bool follow_redirects = true, long max_redirects = 5) {
    return request("GET", url, "", headers, timeout_s, follow_redirects, max_redirects);
  }

  HttpResponse post(const std::string& url, const std::string& body,
                    const std::map<std::string, std::string>& headers = {}, int timeout_s = 60,
                    bool follow_redirects = true, long max_redirects = 5) {
    return request("POST", url, body, headers, timeout_s, follow_redirects, max_redirects);
  }

  // Server-sent events / chunked streaming.
  //
  // on_line is called for each complete line received (without trailing '\n').
  // Return false from on_line to abort the transfer early.
  HttpResponse post_stream_lines(const std::string& url, const std::string& body,
                                 const std::map<std::string, std::string>& headers,
                                 const std::function<bool(const std::string&)>& on_line, int timeout_s = 120,
                                 bool follow_redirects = true, long max_redirects = 5) {
    return request_stream_lines("POST", url, body, headers, on_line, timeout_s, follow_redirects, max_redirects);
  }

  HttpResponse post_multipart_file(const std::string& url, const std::map<std::string, std::string>& headers,
                                   const std::vector<MultipartField>& fields, const std::string& file_field_name,
                                   const fs::path& file_path, const std::string& content_type = "",
                                   int timeout_s = 300, bool follow_redirects = true, long max_redirects = 5) {
    CURL* curl = ensure_easy();
    if (!curl) {
      return HttpResponse{0, "", "", "curl init failed"};
    }

    curl_easy_reset(curl);
    std::string response_body;
    std::map<std::string, std::string> response_headers;
    struct curl_slist* header_list = nullptr;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    apply_common_options(curl, timeout_s, follow_redirects, max_redirects);

    curl_mime* mime = curl_mime_init(curl);
    if (!mime) {
      return HttpResponse{0, "", "", "curl mime init failed"};
    }

    for (const auto& f : fields) {
      curl_mimepart* part = curl_mime_addpart(mime);
      curl_mime_name(part, f.name.c_str());
      curl_mime_data(part, f.value.c_str(), static_cast<size_t>(f.value.size()));
    }

    curl_mimepart* file_part = curl_mime_addpart(mime);
    curl_mime_name(file_part, file_field_name.c_str());
    curl_mime_filedata(file_part, file_path.string().c_str());
    if (!content_type.empty()) {
      curl_mime_type(file_part, content_type.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

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
    out.headers = std::move(response_headers);

    if (header_list) {
      curl_slist_free_all(header_list);
    }
    curl_mime_free(mime);
    return out;
  }

  HttpResponse download_to_file(const std::string& url, const std::map<std::string, std::string>& headers,
                                const fs::path& out_path, int timeout_s = 120, bool follow_redirects = true,
                                long max_redirects = 5) {
    CURL* curl = ensure_easy();
    if (!curl) {
      return HttpResponse{0, "", "", "curl init failed"};
    }

    curl_easy_reset(curl);
    std::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    FILE* fp = std::fopen(out_path.string().c_str(), "wb");
    if (!fp) {
      curl_easy_cleanup(curl);
      return HttpResponse{0, "", "", "failed to open output file"};
    }

    struct curl_slist* header_list = nullptr;
    std::map<std::string, std::string> response_headers;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    apply_common_options(curl, timeout_s, follow_redirects, max_redirects);

    for (const auto& [k, v] : headers) {
      const std::string line = k + ": " + v;
      header_list = curl_slist_append(header_list, line.c_str());
    }
    if (header_list) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    const CURLcode rc = curl_easy_perform(curl);
    std::fclose(fp);

    HttpResponse out;
    if (rc != CURLE_OK) {
      out.error = curl_easy_strerror(rc);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
    char* final_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
    out.final_url = final_url ? std::string(final_url) : url;
    out.headers = std::move(response_headers);

    if (header_list) {
      curl_slist_free_all(header_list);
    }

    if (!out.error.empty() || out.status < 200 || out.status >= 300) {
      std::error_code rm_ec;
      fs::remove(out_path, rm_ec);
    }
    return out;
  }

 private:
  static std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }

  static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const auto n = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, n);
    return n;
  }

  static size_t header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const auto n = size * nmemb;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    if (!headers || !ptr || n == 0) {
      return n;
    }

    std::string line(ptr, n);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }

    const auto p = line.find(':');
    if (p == std::string::npos) {
      return n;
    }

    std::string key = trim(line.substr(0, p));
    std::string val = trim(line.substr(p + 1));
    if (key.empty()) {
      return n;
    }
    key = to_lower_ascii(key);
    (*headers)[key] = val;
    return n;
  }

  static size_t write_file_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    FILE* fp = static_cast<FILE*>(userdata);
    if (!fp) {
      return 0;
    }
    return std::fwrite(ptr, size, nmemb, fp);
  }

  struct StreamLineState {
    std::string buffer;
    std::function<bool(const std::string&)> on_line;
    bool aborted{false};
  };

  static size_t stream_lines_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const auto n = size * nmemb;
    auto* st = static_cast<StreamLineState*>(userdata);
    if (!st || st->aborted) {
      return 0;
    }

    st->buffer.append(ptr, n);
    while (true) {
      const auto pos = st->buffer.find('\n');
      if (pos == std::string::npos) {
        break;
      }
      std::string line = st->buffer.substr(0, pos);
      st->buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (st->on_line) {
        const bool keep_going = st->on_line(line);
        if (!keep_going) {
          st->aborted = true;
          return 0;  // abort transfer
        }
      }
    }
    return n;
  }

  static void ensure_global_init() {
    static std::once_flag flag;
    std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  }

  CURL* easy_{nullptr};

  CURL* ensure_easy() {
    if (!easy_) {
      easy_ = curl_easy_init();
    }
    return easy_;
  }

  void apply_common_options(CURL* curl, int timeout_s, bool follow_redirects, long max_redirects) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (std::min)(10, (std::max)(1, timeout_s / 3)));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_redirects);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "attoclaw/0.1");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip/br
  }

  HttpResponse request(const std::string& method, const std::string& url, const std::string& body,
                       const std::map<std::string, std::string>& headers, int timeout_s,
                       bool follow_redirects, long max_redirects) {
    CURL* curl = ensure_easy();
    if (!curl) {
      return HttpResponse{0, "", "", "curl init failed"};
    }

    curl_easy_reset(curl);
    std::string response_body;
    std::map<std::string, std::string> response_headers;
    struct curl_slist* header_list = nullptr;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    apply_common_options(curl, timeout_s, follow_redirects, max_redirects);

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
    out.headers = std::move(response_headers);

    if (header_list) {
      curl_slist_free_all(header_list);
    }
    return out;
  }

  HttpResponse request_stream_lines(const std::string& method, const std::string& url, const std::string& body,
                                    const std::map<std::string, std::string>& headers,
                                    const std::function<bool(const std::string&)>& on_line, int timeout_s,
                                    bool follow_redirects, long max_redirects) {
    CURL* curl = ensure_easy();
    if (!curl) {
      return HttpResponse{0, "", "", "curl init failed"};
    }

    curl_easy_reset(curl);
    struct curl_slist* header_list = nullptr;
    std::map<std::string, std::string> response_headers;
    StreamLineState state;
    state.on_line = on_line;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &stream_lines_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    apply_common_options(curl, timeout_s, follow_redirects, max_redirects);

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
    out.body = std::move(state.buffer);  // tail (usually empty)
    out.headers = std::move(response_headers);

    if (header_list) {
      curl_slist_free_all(header_list);
    }
    return out;
  }
};

}  // namespace attoclaw
