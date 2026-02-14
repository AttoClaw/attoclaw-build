#pragma once

#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

#include "attoclaw/common.hpp"

namespace attoclaw {

struct SkillInfo {
  std::string name;
  fs::path path;
  std::string source;
};

class SkillsLoader {
 public:
  explicit SkillsLoader(const fs::path& workspace, fs::path builtin_skills = fs::path("skills"))
      : workspace_(workspace), workspace_skills_(workspace / "skills"), builtin_skills_(std::move(builtin_skills)) {}

  std::vector<SkillInfo> list_skills() const {
    std::vector<SkillInfo> skills;
    std::unordered_set<std::string> seen;

    auto scan = [&](const fs::path& dir, const std::string& source) {
      if (!fs::exists(dir) || !fs::is_directory(dir)) {
        return;
      }
      for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) {
          continue;
        }
        const auto name = entry.path().filename().string();
        if (seen.contains(name)) {
          continue;
        }
        const auto skill_file = entry.path() / "SKILL.md";
        if (!fs::exists(skill_file)) {
          continue;
        }
        seen.insert(name);
        skills.push_back({name, skill_file, source});
      }
    };

    scan(workspace_skills_, "workspace");
    scan(builtin_skills_, "builtin");
    return skills;
  }

  std::string load_skill(const std::string& name) const {
    const fs::path ws = workspace_skills_ / name / "SKILL.md";
    if (fs::exists(ws)) {
      return read_text_file(ws);
    }
    const fs::path bi = builtin_skills_ / name / "SKILL.md";
    if (fs::exists(bi)) {
      return read_text_file(bi);
    }
    return "";
  }

  std::string build_skills_summary() const {
    const auto skills = list_skills();
    if (skills.empty()) {
      return "";
    }

    std::ostringstream out;
    out << "<skills>\n";
    for (const auto& s : skills) {
      out << "  <skill available=\"true\">\n";
      out << "    <name>" << s.name << "</name>\n";
      out << "    <description>" << describe(s.name) << "</description>\n";
      out << "    <location>" << s.path.string() << "</location>\n";
      out << "  </skill>\n";
    }
    out << "</skills>";
    return out.str();
  }

 private:
  std::string describe(const std::string& name) const {
    const std::string content = load_skill(name);
    if (content.empty()) {
      return name;
    }

    std::smatch m;
    std::regex re(R"(description:\s*(.+))");
    if (std::regex_search(content, m, re) && m.size() > 1) {
      return trim(m[1].str());
    }
    return name;
  }

  fs::path workspace_;
  fs::path workspace_skills_;
  fs::path builtin_skills_;
};

}  // namespace attoclaw

