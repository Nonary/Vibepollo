/**
 * @file src/platform/windows/playnite_protocol.h
 * @brief Message schema and parsing for Playnite <-> Sunshine IPC.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace platf::playnite {

  /**
   * @brief Enumerates message kinds exchanged over the Playnite IPC channel.
   */
  enum class MessageType {
    Unknown,  ///< Unknown / unsupported message.
    Categories,  ///< Message contains a list of categories.
    Games,  ///< Message contains a list of games.
    Status  ///< Message contains a status update for a specific game / session.
  };

  /**
   * @brief Playnite category description.
   */
  struct Category {
    std::string id;  ///< Category identifier.
    std::string name;  ///< Display name.
  };

  /**
   * @brief Playnite game description parsed from IPC JSON.
   *
   * Field names intentionally use snake_case for internal consistency. They are
   * populated from the original Playnite JSON keys (e.g. workingDir -> working_dir).
   */
  struct Game {
    std::string id;  ///< Unique Playnite game id.
    std::string name;  ///< Game display name.
    std::string exe;  ///< Launch executable path (may be empty).
    std::string args;  ///< Arguments passed on launch.
    std::string working_dir;  ///< Working directory (Playnite JSON key: workingDir).
    std::vector<std::string> categories;  ///< Category names attached to the game.
    uint64_t playtime_minutes = 0;  ///< Total playtime in minutes (playtimeMinutes).
    std::string last_played;  ///< Last played timestamp (ISO8601) (lastPlayed).
    std::string box_art_path;  ///< Path/URL to cover art (boxArtPath).
    std::string description;  ///< Optional description / notes.
    std::vector<std::string> tags;  ///< Tag list.
    bool installed = false;  ///< Installation state (installed / isInstalled).
  };

  /**
   * @brief Generic message container for any Playnite IPC frame.
   */
  struct Message {
    MessageType type = MessageType::Unknown;  ///< Parsed message type.
    std::vector<Category> categories;  ///< Categories payload (if type == Categories).
    std::vector<Game> games;  ///< Games payload (if type == Games).
    // Status payload (if type == Status)
    std::string status_name;  ///< Status event name (e.g. gameStarted, gameStopped).
    std::string status_game_id;  ///< Associated game id.
    std::string status_install_dir;  ///< Install directory provided by status update.
    std::string status_exe;  ///< Executable path from status update.
  };

  /**
   * @brief Parse a JSON encoded Playnite IPC message into a typed structure.
   * @param bytes UTF-8 JSON bytes (single frame, newline already stripped).
   * @return Parsed Message (type == Unknown on failure).
   */
  Message parse(std::span<const uint8_t> bytes);
}  // namespace platf::playnite
