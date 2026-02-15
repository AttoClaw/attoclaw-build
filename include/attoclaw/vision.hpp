#pragma once

#include <optional>
#include <string>
#include <vector>

#include "attoclaw/common.hpp"

namespace attoclaw {

struct VisionFrame {
  fs::path path;
  std::string data_url;
  int64_t timestamp_ms{0};
};

inline std::string sh_single_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\"'\"'";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

inline bool command_exists_in_path(const std::string& command) {
#ifdef _WIN32
  const CommandResult r = run_command_capture("where " + command, 10);
#else
  const CommandResult r = run_command_capture("sh -lc \"command -v " + command + "\"", 10);
#endif
  return r.ok && !trim(r.output).empty();
}

inline bool is_headless_server() {
#ifdef _WIN32
  return false;
#else
  const char* display = std::getenv("DISPLAY");
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  return (!(display && *display) && !(wayland && *wayland));
#endif
}

inline bool try_install_linux_package(const std::string& package_name, int timeout_s = 180,
                                      std::string* note = nullptr) {
#ifdef _WIN32
  (void)package_name;
  (void)timeout_s;
  if (note) {
    *note = "auto install is not supported on Windows for this dependency";
  }
  return false;
#else
  struct ManagerCommand {
    std::string manager;
    std::string command;
  };

  const std::vector<ManagerCommand> managers = {
      {"pkg", "pkg install -y " + package_name},
      {"apt-get", "apt-get install -y " + package_name},
      {"apt", "apt install -y " + package_name},
      {"dnf", "dnf install -y " + package_name},
      {"yum", "yum install -y " + package_name},
      {"pacman", "pacman -Sy --noconfirm " + package_name},
      {"zypper", "zypper --non-interactive install " + package_name},
      {"apk", "apk add --no-progress " + package_name},
  };

  std::string last_error;
  for (const auto& item : managers) {
    if (!command_exists_in_path(item.manager)) {
      continue;
    }

    std::string cmd = item.command;
    if (item.manager != "pkg") {
      if (command_exists_in_path("sudo")) {
        cmd = "sudo -n " + cmd;
      }
    }

    CommandResult install = run_command_capture(cmd, timeout_s);
    if (install.ok) {
      return true;
    }
    const std::string err = trim(install.output);
    if (!err.empty()) {
      last_error = err;
    }
  }

  if (note) {
    if (!last_error.empty()) {
      *note = last_error;
    } else {
      *note = "no supported package manager found or install failed";
    }
  }
  return false;
#endif
}

inline bool ensure_vision_capture_dependencies(std::string* note = nullptr) {
#ifdef _WIN32
  return true;
#else
  if (is_headless_server()) {
    if (note) {
      *note = "vision is unavailable on headless server (DISPLAY/WAYLAND_DISPLAY not set)";
    }
    return false;
  }
  if (command_exists_in_path("grim") || command_exists_in_path("scrot")) {
    return true;
  }

  static bool install_attempted = false;
  if (!install_attempted) {
    install_attempted = true;
    std::string install_note;
    (void)try_install_linux_package("grim", 180, &install_note);
    if (!command_exists_in_path("grim")) {
      (void)try_install_linux_package("scrot", 180, &install_note);
    }
  }

  if (command_exists_in_path("grim") || command_exists_in_path("scrot")) {
    return true;
  }
  if (note) {
    *note = "no screenshot tool available (grim/scrot). Auto-install failed.";
  }
  return false;
#endif
}

inline bool has_tesseract_ocr() {
#ifdef _WIN32
  CommandResult r = run_command_capture("where tesseract", 10);
  return r.ok && !trim(r.output).empty();
#else
  CommandResult r = run_command_capture("sh -lc \"command -v tesseract\"", 10);
  return r.ok && !trim(r.output).empty();
#endif
}

inline bool ensure_tesseract_ocr(std::string* note = nullptr) {
  if (has_tesseract_ocr()) {
    return true;
  }
#ifdef _WIN32
  if (note) {
    *note = "tesseract OCR is not installed";
  }
  return false;
#else
  static bool install_attempted = false;
  if (!install_attempted) {
    install_attempted = true;
    std::string install_note;
    (void)try_install_linux_package("tesseract-ocr", 240, &install_note);
    if (!has_tesseract_ocr()) {
      (void)try_install_linux_package("tesseract", 240, &install_note);
    }
  }
  if (has_tesseract_ocr()) {
    return true;
  }
  if (note) {
    *note = "tesseract OCR is not installed and auto-install failed";
  }
  return false;
#endif
}

inline std::string extract_ocr_text(const fs::path& image_path, int timeout_s = 20) {
  const fs::path p = fs::absolute(image_path);
  if (!fs::exists(p)) {
    return "";
  }
  if (!ensure_tesseract_ocr()) {
    return "";
  }

#ifdef _WIN32
  const std::string cmd = "tesseract \"" + p.string() + "\" stdout --psm 6";
#else
  const std::string cmd = "sh -lc \"tesseract '" + p.string() + "' stdout --psm 6\"";
#endif
  CommandResult r = run_command_capture(cmd, timeout_s);
  if (!r.ok) {
    return "";
  }

  std::string out = trim(r.output);
  if (out.size() > 6000) {
    out.resize(6000);
    out += "\n... (truncated)";
  }
  return out;
}

inline std::string base64_encode_bytes(const std::vector<unsigned char>& data) {
  static constexpr char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);

  std::size_t i = 0;
  while (i + 3 <= data.size()) {
    const unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                           (static_cast<unsigned int>(data[i + 1]) << 8) |
                           static_cast<unsigned int>(data[i + 2]);
    out.push_back(tbl[(n >> 18) & 0x3F]);
    out.push_back(tbl[(n >> 12) & 0x3F]);
    out.push_back(tbl[(n >> 6) & 0x3F]);
    out.push_back(tbl[n & 0x3F]);
    i += 3;
  }

  const std::size_t rem = data.size() - i;
  if (rem == 1) {
    const unsigned int n = static_cast<unsigned int>(data[i]) << 16;
    out.push_back(tbl[(n >> 18) & 0x3F]);
    out.push_back(tbl[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const unsigned int n =
        (static_cast<unsigned int>(data[i]) << 16) | (static_cast<unsigned int>(data[i + 1]) << 8);
    out.push_back(tbl[(n >> 18) & 0x3F]);
    out.push_back(tbl[(n >> 12) & 0x3F]);
    out.push_back(tbl[(n >> 6) & 0x3F]);
    out.push_back('=');
  }

  return out;
}

inline std::vector<unsigned char> read_binary_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return {};
  }
  in.seekg(0, std::ios::end);
  const auto size = static_cast<std::size_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  std::vector<unsigned char> data(size);
  if (size > 0) {
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
  }
  return data;
}

inline std::string ps_quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\'') {
      out += "''";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

inline std::optional<VisionFrame> capture_vision_frame(int max_width = 960, int jpeg_quality = 60) {
#ifndef _WIN32
  (void)max_width;
  (void)jpeg_quality;
  return std::nullopt;
#else
  max_width = (std::max)(320, max_width);
  jpeg_quality = std::clamp(jpeg_quality, 20, 95);

  const fs::path dir = expand_user_path("~/.attoclaw") / "vision_frames";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path out = dir / ("frame_" + std::to_string(now_ms()) + ".jpg");
  const std::string out_abs = fs::absolute(out).string();

  const std::string command =
      "powershell -NoProfile -ExecutionPolicy Bypass -Command "
      "\"Add-Type -AssemblyName System.Windows.Forms; "
      "Add-Type -AssemblyName System.Drawing; "
      "$b=[System.Windows.Forms.SystemInformation]::VirtualScreen; "
      "$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height; "
      "$g=[System.Drawing.Graphics]::FromImage($bmp); "
      "$g.CopyFromScreen($b.Left,$b.Top,0,0,$bmp.Size); "
      "$outBmp=$bmp; "
      "if($bmp.Width -gt " +
      std::to_string(max_width) +
      "){"
      "$h=[int]($bmp.Height*" +
      std::to_string(max_width) +
      "/$bmp.Width); "
      "$outBmp=New-Object System.Drawing.Bitmap $bmp," +
      std::to_string(max_width) +
      ",$h;"
      "} "
      "$enc=[System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | "
      "Where-Object { $_.MimeType -eq 'image/jpeg' }; "
      "$ep=New-Object System.Drawing.Imaging.EncoderParameters 1; "
      "$ep.Param[0]=New-Object System.Drawing.Imaging.EncoderParameter("
      "[System.Drawing.Imaging.Encoder]::Quality," +
      std::to_string(jpeg_quality) +
      "); "
      "$outBmp.Save('" +
      ps_quote(out_abs) +
      "',$enc,$ep); "
      "if($outBmp -ne $bmp){$outBmp.Dispose()}; "
      "$g.Dispose(); $bmp.Dispose();\"";

  const CommandResult res = run_command_capture(command, 30);
  if (!res.ok || !fs::exists(out)) {
    return std::nullopt;
  }

  std::vector<unsigned char> bytes = read_binary_file(out);
  if (bytes.empty()) {
    return std::nullopt;
  }

  VisionFrame f;
  f.path = fs::absolute(out);
  f.timestamp_ms = now_ms();
  f.data_url = "data:image/jpeg;base64," + base64_encode_bytes(bytes);
  return f;
#endif
}

}  // namespace attoclaw
