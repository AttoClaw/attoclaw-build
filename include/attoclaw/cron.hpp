#pragma once

#include <atomic>
#include <array>
#include <cctype>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "attoclaw/common.hpp"

namespace attoclaw {

struct CronSchedule {
  std::string kind{"every"};  // at | every | cron
  int64_t at_ms{0};
  int64_t every_ms{0};
  std::string expr;
};

struct CronPayload {
  std::string kind{"agent_turn"};
  std::string message;
  bool deliver{false};
  std::string channel;
  std::string to;
};

struct CronJobState {
  int64_t next_run_at_ms{0};
  int64_t last_run_at_ms{0};
  std::string last_status;
  std::string last_error;
};

struct CronJob {
  std::string id;
  std::string name;
  bool enabled{true};
  CronSchedule schedule{};
  CronPayload payload{};
  CronJobState state{};
  int64_t created_at_ms{0};
  int64_t updated_at_ms{0};
  bool delete_after_run{false};
};

class CronService {
 public:
  using OnJob = std::function<std::optional<std::string>(const CronJob&)>;

  explicit CronService(fs::path store_path, OnJob on_job = nullptr)
      : store_path_(std::move(store_path)), on_job_(std::move(on_job)) {
    load_store();
  }

  ~CronService() { stop(); }

  void set_on_job(OnJob cb) { on_job_ = std::move(cb); }

  void start() {
    if (running_.exchange(true)) {
      return;
    }
    recompute_next_runs();
    save_store();
    worker_ = std::thread([this]() { run_loop(); });
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::vector<CronJob> list_jobs(bool include_disabled = false) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<CronJob> jobs;
    for (const auto& j : jobs_) {
      if (include_disabled || j.enabled) {
        jobs.push_back(j);
      }
    }
    std::sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) {
      return a.state.next_run_at_ms < b.state.next_run_at_ms;
    });
    return jobs;
  }

  CronJob add_job(const std::string& name, const CronSchedule& schedule, const std::string& message,
                  bool deliver = false, const std::string& channel = "", const std::string& to = "",
                  bool delete_after_run = false) {
    std::lock_guard<std::mutex> lock(mu_);

    CronJob j;
    j.id = random_id(8);
    j.name = name;
    j.enabled = true;
    j.schedule = schedule;
    j.payload = CronPayload{"agent_turn", message, deliver, channel, to};
    j.created_at_ms = now_ms();
    j.updated_at_ms = j.created_at_ms;
    j.delete_after_run = delete_after_run;
    j.state.next_run_at_ms = compute_next_run_ms(schedule, now_ms());

    jobs_.push_back(j);
    save_store();
    cv_.notify_all();
    return j;
  }

  bool remove_job(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto old_size = jobs_.size();
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const CronJob& j) { return j.id == id; }),
                jobs_.end());
    const bool removed = jobs_.size() != old_size;
    if (removed) {
      save_store();
      cv_.notify_all();
    }
    return removed;
  }

  std::optional<CronJob> enable_job(const std::string& id, bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& j : jobs_) {
      if (j.id == id) {
        j.enabled = enabled;
        j.updated_at_ms = now_ms();
        j.state.next_run_at_ms = enabled ? compute_next_run_ms(j.schedule, now_ms()) : 0;
        save_store();
        cv_.notify_all();
        return j;
      }
    }
    return std::nullopt;
  }

  bool run_job_now(const std::string& id, bool force = false) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& j : jobs_) {
      if (j.id == id) {
        if (!force && !j.enabled) {
          return false;
        }
        execute_job(j);
        save_store();
        cv_.notify_all();
        return true;
      }
    }
    return false;
  }

  json status() {
    std::lock_guard<std::mutex> lock(mu_);
    int64_t next_wake = 0;
    for (const auto& j : jobs_) {
      if (!j.enabled || j.state.next_run_at_ms <= 0) {
        continue;
      }
      if (next_wake == 0 || j.state.next_run_at_ms < next_wake) {
        next_wake = j.state.next_run_at_ms;
      }
    }
    return json{{"enabled", running_.load()}, {"jobs", jobs_.size()}, {"next_wake_at_ms", next_wake}};
  }

 private:
  int64_t compute_next_run_ms(const CronSchedule& s, int64_t now) const {
    if (s.kind == "at") {
      return s.at_ms > now ? s.at_ms : 0;
    }
    if (s.kind == "every") {
      return s.every_ms > 0 ? now + s.every_ms : 0;
    }
    if (s.kind == "cron") {
      return compute_next_cron_run_ms(s.expr, now);
    }
    return 0;
  }

  struct CronSpec {
    std::array<bool, 60> minutes{};
    std::array<bool, 24> hours{};
    std::array<bool, 32> month_days{};
    std::array<bool, 13> months{};
    std::array<bool, 8> week_days{};  // 0-7 (0 and 7 are Sunday)
    bool dom_any{false};
    bool dow_any{false};
    bool valid{false};
  };

  static std::tm localtime_safe(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
  }

  static bool parse_int(const std::string& s, int& out) {
    if (s.empty()) {
      return false;
    }
    for (char c : s) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        return false;
      }
    }
    try {
      out = std::stoi(s);
      return true;
    } catch (...) {
      return false;
    }
  }

  template <std::size_t N>
  static bool parse_cron_field(const std::string& token, int min_v, int max_v, std::array<bool, N>& out,
                               bool* is_any = nullptr, bool allow_weekday_7 = false) {
    out.fill(false);
    if (is_any) {
      *is_any = false;
    }

    auto mark = [&](int v) {
      if (allow_weekday_7 && v == 7) {
        out[0] = true;
        out[7] = true;
      } else {
        out[static_cast<std::size_t>(v)] = true;
      }
    };

    std::istringstream ss(token);
    std::string part;
    bool saw_any = false;

    while (std::getline(ss, part, ',')) {
      part = trim(part);
      if (part.empty()) {
        return false;
      }

      int step = 1;
      std::string base = part;
      const auto slash = part.find('/');
      if (slash != std::string::npos) {
        base = part.substr(0, slash);
        int parsed_step = 0;
        if (!parse_int(part.substr(slash + 1), parsed_step) || parsed_step <= 0) {
          return false;
        }
        step = parsed_step;
      }

      int start = min_v;
      int end = max_v;
      if (base == "*" || base.empty()) {
        saw_any = true;
      } else {
        const auto dash = base.find('-');
        if (dash != std::string::npos) {
          int a = 0;
          int b = 0;
          if (!parse_int(base.substr(0, dash), a) || !parse_int(base.substr(dash + 1), b)) {
            return false;
          }
          start = a;
          end = b;
        } else {
          int one = 0;
          if (!parse_int(base, one)) {
            return false;
          }
          start = one;
          end = one;
        }
      }

      if (start > end) {
        return false;
      }

      for (int v = start; v <= end; v += step) {
        if (v < min_v || v > max_v) {
          return false;
        }
        mark(v);
      }
    }

    if (is_any) {
      *is_any = saw_any;
    }

    for (bool b : out) {
      if (b) {
        return true;
      }
    }
    return false;
  }

  static CronSpec parse_cron_expr(const std::string& expr) {
    CronSpec spec;
    std::istringstream ss(expr);
    std::vector<std::string> fields;
    std::string tok;
    while (ss >> tok) {
      fields.push_back(tok);
    }
    if (fields.size() != 5) {
      return spec;
    }

    bool ok = true;
    ok = ok && parse_cron_field(fields[0], 0, 59, spec.minutes);
    ok = ok && parse_cron_field(fields[1], 0, 23, spec.hours);
    ok = ok && parse_cron_field(fields[2], 1, 31, spec.month_days, &spec.dom_any);
    ok = ok && parse_cron_field(fields[3], 1, 12, spec.months);
    ok = ok && parse_cron_field(fields[4], 0, 7, spec.week_days, &spec.dow_any, true);
    spec.valid = ok;
    return spec;
  }

  static bool cron_match(const CronSpec& spec, const std::tm& tm) {
    const bool minute_ok = spec.minutes[static_cast<std::size_t>(tm.tm_min)];
    const bool hour_ok = spec.hours[static_cast<std::size_t>(tm.tm_hour)];
    const bool month_ok = spec.months[static_cast<std::size_t>(tm.tm_mon + 1)];
    const bool dom_ok = spec.month_days[static_cast<std::size_t>(tm.tm_mday)];
    const bool dow_ok = spec.week_days[static_cast<std::size_t>(tm.tm_wday)];

    if (!(minute_ok && hour_ok && month_ok)) {
      return false;
    }

    if (spec.dom_any && spec.dow_any) {
      return true;
    }
    if (spec.dom_any) {
      return dow_ok;
    }
    if (spec.dow_any) {
      return dom_ok;
    }
    return dom_ok || dow_ok;
  }

  static int64_t compute_next_cron_run_ms(const std::string& expr, int64_t now_ms_val) {
    const CronSpec spec = parse_cron_expr(expr);
    if (!spec.valid) {
      return 0;
    }

    std::time_t t = static_cast<std::time_t>((now_ms_val / 1000) + (60 - ((now_ms_val / 1000) % 60)));
    constexpr int kMaxMinuteLookahead = 60 * 24 * 366 * 2;  // 2 years.

    for (int i = 0; i < kMaxMinuteLookahead; ++i, t += 60) {
      const std::tm tm = localtime_safe(t);
      if (cron_match(spec, tm)) {
        return static_cast<int64_t>(t) * 1000;
      }
    }
    return 0;
  }

  void run_loop() {
    while (running_.load()) {
      int64_t next_wake = 0;
      {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& j : jobs_) {
          if (!j.enabled || j.state.next_run_at_ms <= 0) {
            continue;
          }
          if (next_wake == 0 || j.state.next_run_at_ms < next_wake) {
            next_wake = j.state.next_run_at_ms;
          }
        }
      }

      if (next_wake == 0) {
        std::unique_lock<std::mutex> lock(wait_mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(500));
        continue;
      }

      const int64_t now = now_ms();
      if (now < next_wake) {
        std::unique_lock<std::mutex> lock(wait_mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(next_wake - now));
        continue;
      }

      std::lock_guard<std::mutex> lock(mu_);
      for (auto& j : jobs_) {
        if (j.enabled && j.state.next_run_at_ms > 0 && now_ms() >= j.state.next_run_at_ms) {
          execute_job(j);
        }
      }

      jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                 [](const CronJob& j) {
                                   return j.schedule.kind == "at" && j.delete_after_run &&
                                          j.state.last_status == "ok";
                                 }),
                  jobs_.end());

      save_store();
    }
  }

  void execute_job(CronJob& job) {
    const int64_t start = now_ms();
    try {
      if (on_job_) {
        (void)on_job_(job);
      }
      job.state.last_status = "ok";
      job.state.last_error.clear();
    } catch (const std::exception& e) {
      job.state.last_status = "error";
      job.state.last_error = e.what();
    }

    job.state.last_run_at_ms = start;
    job.updated_at_ms = now_ms();

    if (job.schedule.kind == "at") {
      if (!job.delete_after_run) {
        job.enabled = false;
        job.state.next_run_at_ms = 0;
      }
    } else {
      job.state.next_run_at_ms = compute_next_run_ms(job.schedule, now_ms());
    }
  }

  void load_store() {
    std::lock_guard<std::mutex> lock(mu_);
    jobs_.clear();

    const std::string raw = read_text_file(store_path_);
    if (raw.empty()) {
      return;
    }

    try {
      const json root = json::parse(raw);
      if (!root.contains("jobs") || !root["jobs"].is_array()) {
        return;
      }

      for (const auto& x : root["jobs"]) {
        CronJob j;
        j.id = x.value("id", random_id(8));
        j.name = x.value("name", "job");
        j.enabled = x.value("enabled", true);

        if (x.contains("schedule") && x["schedule"].is_object()) {
          const auto& s = x["schedule"];
          j.schedule.kind = s.value("kind", "every");
          j.schedule.at_ms = s.value("atMs", 0LL);
          j.schedule.every_ms = s.value("everyMs", 0LL);
          j.schedule.expr = s.value("expr", "");
        }
        if (x.contains("payload") && x["payload"].is_object()) {
          const auto& p = x["payload"];
          j.payload.kind = p.value("kind", "agent_turn");
          j.payload.message = p.value("message", "");
          j.payload.deliver = p.value("deliver", false);
          j.payload.channel = p.value("channel", "");
          j.payload.to = p.value("to", "");
        }
        if (x.contains("state") && x["state"].is_object()) {
          const auto& st = x["state"];
          j.state.next_run_at_ms = st.value("nextRunAtMs", 0LL);
          j.state.last_run_at_ms = st.value("lastRunAtMs", 0LL);
          j.state.last_status = st.value("lastStatus", "");
          j.state.last_error = st.value("lastError", "");
        }

        j.created_at_ms = x.value("createdAtMs", now_ms());
        j.updated_at_ms = x.value("updatedAtMs", j.created_at_ms);
        j.delete_after_run = x.value("deleteAfterRun", false);

        jobs_.push_back(std::move(j));
      }

    } catch (const std::exception& e) {
      Logger::log(Logger::Level::kWarn, std::string("Failed to load cron store: ") + e.what());
    }
  }

  void save_store() const {
    json root;
    root["version"] = 1;
    root["jobs"] = json::array();

    for (const auto& j : jobs_) {
      root["jobs"].push_back({
          {"id", j.id},
          {"name", j.name},
          {"enabled", j.enabled},
          {"schedule",
           {{"kind", j.schedule.kind}, {"atMs", j.schedule.at_ms}, {"everyMs", j.schedule.every_ms}, {"expr", j.schedule.expr}}},
          {"payload",
           {{"kind", j.payload.kind},
            {"message", j.payload.message},
            {"deliver", j.payload.deliver},
            {"channel", j.payload.channel},
            {"to", j.payload.to}}},
          {"state",
           {{"nextRunAtMs", j.state.next_run_at_ms},
            {"lastRunAtMs", j.state.last_run_at_ms},
            {"lastStatus", j.state.last_status},
            {"lastError", j.state.last_error}}},
          {"createdAtMs", j.created_at_ms},
          {"updatedAtMs", j.updated_at_ms},
          {"deleteAfterRun", j.delete_after_run},
      });
    }

    write_text_file(store_path_, root.dump(2));
  }

  void recompute_next_runs() {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = now_ms();
    for (auto& j : jobs_) {
      if (j.enabled) {
        j.state.next_run_at_ms = compute_next_run_ms(j.schedule, now);
      }
    }
  }

  fs::path store_path_;
  OnJob on_job_;

  std::atomic<bool> running_{false};
  std::thread worker_;

  mutable std::mutex mu_;
  std::vector<CronJob> jobs_;

  std::mutex wait_mu_;
  std::condition_variable cv_;
};

}  // namespace attoclaw

