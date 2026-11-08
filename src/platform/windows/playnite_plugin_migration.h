/** Ownership-checked, recoverable retirement of an alternate Playnite connector. */
#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#ifdef _WIN32
  #include <Windows.h>
#endif

namespace platf::playnite::migration {
  namespace fs = std::filesystem;

  // Do not follow junctions/symlinks to another installation or user's files.
  enum class path_inspection { safe, unsafe, error };

  inline path_inspection inspect_path(const fs::path &path) {
    if (!path.is_absolute()) return path_inspection::unsafe;
    fs::path current = path.root_path();
    for (const auto &part : path.relative_path()) {
      if (part == "..") return path_inspection::unsafe;
      current /= part;
      std::error_code ec;
      const auto status = fs::symlink_status(current, ec);
      if (ec) return path_inspection::error;
      if (fs::is_symlink(status)) return path_inspection::unsafe;
#ifdef _WIN32
      const auto attributes = GetFileAttributesW(current.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) return path_inspection::error;
      if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return path_inspection::unsafe;
#endif
    }
    return path_inspection::safe;
  }

  inline bool plain_path(const fs::path &path) {
    return inspect_path(path) == path_inspection::safe;
  }

  inline std::string scalar(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    value = value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                             (value.front() == '"' && value.back() == '"'))) {
      value = value.substr(1, value.size() - 2);
    }
    return value;
  }

  // These are the manifests shipped by this integration, not a directory-name
  // heuristic. Unknown/ambiguous YAML is deliberately left alone.
  enum class manifest_inspection { unowned, owned, error };

  inline manifest_inspection read_manifest(std::istream &stream) {
    if (!stream) return manifest_inspection::error;
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(stream, line)) {
      if (line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      const auto key = scalar(line.substr(0, colon));
      if (key != "Id" && key != "Module" && key != "Type") continue;
      // Nested keys must never masquerade as the top-level identity.
      if (line.empty() || line.front() == ' ' || line.front() == '\t' || fields.count(key)) return manifest_inspection::unowned;
      fields[key] = scalar(line.substr(colon + 1));
    }
    if (stream.bad() || (stream.fail() && !stream.eof())) return manifest_inspection::error;
    const bool script = fields["Id"] == "Sunshine.Playnite.PowerShell" && fields["Type"] == "Script" &&
                        fields["Module"] == "SunshinePlaynite.psm1";
    const bool compiled = fields["Id"] == "Sunshine.Playnite.Integration" && fields["Type"] == "GenericPlugin" &&
                          (fields["Module"] == "SunshinePlaynite.dll" || fields["Module"] == "VibeshinePlaynite.dll" ||
                           fields["Module"] == "VibepolloPlaynite.dll");
    return script || compiled ? manifest_inspection::owned : manifest_inspection::unowned;
  }

  inline manifest_inspection inspect_manifest(const fs::path &manifest) {
    std::error_code ec;
    const auto status = fs::symlink_status(manifest, ec);
    if (ec) return ec == std::errc::no_such_file_or_directory ? manifest_inspection::unowned : manifest_inspection::error;
    if (status.type() == fs::file_type::not_found) return manifest_inspection::unowned;
    const auto path_state = inspect_path(manifest);
    if (path_state == path_inspection::error) return manifest_inspection::error;
    if (path_state == path_inspection::unsafe || !fs::is_regular_file(status)) return manifest_inspection::unowned;
    const auto size = fs::file_size(manifest, ec);
    if (ec) return manifest_inspection::error;
    if (size > 65536) return manifest_inspection::unowned;
    std::ifstream stream(manifest);
    return read_manifest(stream);
  }

  inline fs::path pending_marker(const fs::path &active) {
    return active / ".vibeshine-playnite-migration-pending";
  }

  inline bool migration_pending(const fs::path &active) {
    std::error_code ec;
    const auto status = fs::symlink_status(pending_marker(active), ec);
    if (ec) return ec != std::errc::no_such_file_or_directory;
    return status.type() != fs::file_type::not_found;
  }

  // A directory marker cannot redirect writes through a pre-existing symlink.
  // It remains present across crashes and failed retirement attempts.
  inline bool begin_migration(const fs::path &active, std::string &error) {
    std::error_code ec;
    if (!plain_path(active)) {
      error = "Cannot safely record pending Playnite connector migration";
      return false;
    }
    const auto marker = pending_marker(active);
    fs::create_directory(marker, ec);
    if (ec || !plain_path(marker) || !fs::is_directory(marker, ec) || ec) {
      error = "Cannot record pending Playnite connector migration" + (ec ? ": " + ec.message() : std::string());
      return false;
    }
    return true;
  }

  inline bool complete_migration(const fs::path &active, std::string &error) {
    std::error_code ec;
    fs::remove(pending_marker(active), ec);
    if (ec) {
      error = "Cannot clear pending Playnite connector migration: " + ec.message();
      return false;
    }
    return true;
  }

  // The caller supplies only roots tied to the selected registry-confirmed
  // installation. An explicit destination elsewhere must not retire either.
  inline std::optional<fs::path> alternate_root(const fs::path &destination, const fs::path &profile, const fs::path &install) {
    std::error_code ec;
    if (fs::equivalent(destination, profile, ec) && !ec) return install;
    ec.clear();
    if (fs::equivalent(destination, install, ec) && !ec) return profile;
    return std::nullopt;
  }

  // A resolved installed profile target must not silently deploy a new
  // version if discovery of its paired executable root failed.
  inline bool unresolved_installed_target(const fs::path &destination, const fs::path &installed_target, bool alternate_resolved) {
    if (alternate_resolved || installed_target.empty()) return false;
    std::error_code ec;
    return fs::equivalent(destination, installed_target, ec) && !ec;
  }

  inline bool needs_migration(bool alternate_exists, bool pending) {
    return alternate_exists || pending;
  }

  // Called only after the replacement has been verified and Playnite stopped.
  // A missing manifest is an idempotent success; unowned/linked entries are not
  // touched. A failed rename leaves the original intact and is retryable.
  inline bool disable_duplicate(const fs::path &active, const fs::path &alternate, std::string &error,
                                manifest_inspection (*inspect)(const fs::path &) = inspect_manifest,
                                path_inspection (*inspect_safety)(const fs::path &) = inspect_path) {
    std::error_code ec;
    if (!fs::exists(alternate, ec)) {
      if (!ec) return true;
      error = "Could not inspect alternate Playnite extension: " + ec.message();
      return false;
    }
    if (!fs::is_directory(alternate, ec)) {
      if (!ec) return true;
      error = "Could not inspect alternate Playnite extension: " + ec.message();
      return false;
    }
    if (fs::equivalent(active, alternate, ec) && !ec) return true;
    const auto active_safety = inspect_safety(active);
    const auto alternate_safety = inspect_safety(alternate);
    if (active_safety == path_inspection::error || alternate_safety == path_inspection::error) {
      error = "Could not inspect Playnite connector path metadata";
      return false;
    }
    if (active_safety == path_inspection::unsafe || alternate_safety == path_inspection::unsafe) return true;
    const auto manifest = alternate / "extension.yaml";
    const auto alternate_manifest = inspect(manifest);
    if (alternate_manifest == manifest_inspection::error) {
      error = "Could not read alternate Playnite connector manifest";
      return false;
    }
    if (alternate_manifest != manifest_inspection::owned) return true;
    // Never disable the old connector if the new activation manifest is absent
    // or unrecognized (including interrupted deployment).
    if (inspect(active / "extension.yaml") != manifest_inspection::owned) {
      error = "Replacement Playnite connector manifest could not be verified";
      return false;
    }
    for (unsigned int suffix = 0; suffix < 1000; ++suffix) {
      const auto backup = alternate / ("extension.yaml.vibepollo-disabled" + (suffix ? "." + std::to_string(suffix) : ""));
      ec.clear();
      const auto status = fs::symlink_status(backup, ec);
      if (ec && ec != std::errc::no_such_file_or_directory) break;
      if (fs::exists(status)) continue;
      ec.clear();
      fs::rename(manifest, backup, ec);
      if (!ec) return true;
      break;
    }
    error = "Could not disable duplicate Playnite connector in " + alternate.string() +
            (ec ? ": " + ec.message() : ": backup slots exhausted");
    return false;
  }
}
