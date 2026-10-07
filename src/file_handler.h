/**
 * @file file_handler.h
 * @brief Declarations for file handling functions.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

/**
 * @brief Responsible for file handling functions.
 */
namespace file_handler {
  enum class replace_result_e {
    success,
    access_denied,
    error,
  };

  class filesystem_t {
  public:
    virtual ~filesystem_t() = default;
    virtual bool exists(const std::filesystem::path &path) const = 0;
    virtual bool create_directories(const std::filesystem::path &path) = 0;
    virtual std::string read(const std::filesystem::path &path) const = 0;
    virtual bool write(const std::filesystem::path &path, std::string_view contents) = 0;
    virtual replace_result_e replace(const std::filesystem::path &temporary,
                                     const std::filesystem::path &target) = 0;
    virtual std::optional<bool> read_only(const std::filesystem::path &path) const = 0;
    virtual bool set_read_only(const std::filesystem::path &path, bool read_only) = 0;
    virtual void remove(const std::filesystem::path &path) = 0;
    virtual std::filesystem::path temporary_path(const std::filesystem::path &target) = 0;
  };

  class handler_t {
  public:
    using diagnostic_t = std::function<void(std::string_view)>;

    explicit handler_t(filesystem_t &filesystem, diagnostic_t diagnostic = {});
    std::string get_parent_directory(const std::string &path) const;
    bool make_directory(const std::string &path);
    std::string read_file(const std::filesystem::path &path) const;
    int write_file(const std::filesystem::path &path, std::string_view contents);

  private:
    filesystem_t &_filesystem;
    diagnostic_t _diagnostic;
    void report(std::string_view message) const;
  };

  /**
   * @brief Get the parent directory of a file or directory.
   * @param path The path of the file or directory.
   * @return The parent directory.
   * @examples
   * std::string parent_dir = get_parent_directory("path/to/file");
   * @examples_end
   */
  std::string get_parent_directory(const std::string &path);

  /**
   * @brief Make a directory.
   * @param path The path of the directory.
   * @return `true` on success, `false` on failure.
   * @examples
   * bool dir_created = make_directory("path/to/directory");
   * @examples_end
   */
  bool make_directory(const std::string &path);

  /**
   * @brief Read a file to string.
   * @param path The path of the file.
   * @return The contents of the file.
   * @examples
   * std::string contents = read_file("path/to/file");
   * @examples_end
   */
  std::string read_file(const char *path);

  /**
   * @brief Writes a file.
   * @param path The path of the file.
   * @param contents The contents to write.
   * @return ``0`` on success, ``-1`` on failure.
   * @examples
   * int write_status = write_file("path/to/file", "file contents");
   * @examples_end
   */
  int write_file(const char *path, const std::string_view &contents);
}  // namespace file_handler
