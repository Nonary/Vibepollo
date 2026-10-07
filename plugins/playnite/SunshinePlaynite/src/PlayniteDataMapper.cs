using System;
using System.Collections.Generic;
using System.Linq;
using Playnite.SDK;
using Playnite.SDK.Models;
using Playnite.SDK.Plugins;

namespace SunshinePlaynite
{
    internal sealed class PlayniteDataMapper
    {
        private readonly IPlayniteAPI api;
        private readonly Func<object, string> serialize;

        public PlayniteDataMapper(IPlayniteAPI api, Func<object, string> serialize)
        {
            this.api = api;
            this.serialize = serialize;
        }

        public PlayniteSnapshot BuildSnapshot()
        {
            var categories = api.Database.Categories.Select(x => new Dictionary<string, object>
            {
                ["id"] = x.Id.ToString(), ["name"] = x.Name
            }).ToList();
            var categoryNames = api.Database.Categories.ToDictionary(x => x.Id, x => x.Name);
            var pluginNames = new Dictionary<Guid, string>();
            foreach (var plugin in api.Addons.Plugins.OfType<LibraryPlugin>()) pluginNames[plugin.Id] = plugin.Name;
            var plugins = pluginNames.OrderBy(x => x.Value).Select(x => new Dictionary<string, object>
            {
                ["id"] = x.Key.ToString(), ["name"] = x.Value
            }).ToList();
            var games = api.Database.Games.Select(game => BuildGame(game, categoryNames, pluginNames)).ToList();
            return new PlayniteSnapshot(categories, plugins, games);
        }

        public string BuildStatus(string name, Game game)
        {
            var action = GetAction(game);
            var installDirectory = game.InstallDirectory ?? string.Empty;
            if (action.Source != null && action.Source.Type.ToString().IndexOf("Emulator", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                var emulator = api.Database.Emulators.Get(action.Source.EmulatorId);
                if (emulator != null && !string.IsNullOrEmpty(emulator.InstallDir)) installDirectory = emulator.InstallDir;
            }
            return serialize(new Dictionary<string, object>
            {
                ["type"] = "status",
                ["status"] = new Dictionary<string, object>
                {
                    ["name"] = name, ["id"] = game.Id.ToString(), ["installDir"] = installDirectory, ["exe"] = action.Path
                }
            });
        }

        public bool SetCover(Guid gameId, string sourcePath)
        {
            var game = api.Database.Games.Get(gameId);
            if (game == null) return false;
            var old = game.CoverImage;
            var imported = api.Database.AddFile(sourcePath, gameId);
            try
            {
                game.CoverImage = imported;
                api.Database.Games.Update(game);
                return true;
            }
            catch
            {
                game.CoverImage = old;
                try { api.Database.RemoveFile(imported); } catch { }
                throw;
            }
        }

        private Dictionary<string, object> BuildGame(Game game, IDictionary<Guid, string> categories, IDictionary<Guid, string> plugins)
        {
            var action = GetAction(game);
            var categoryList = (game.CategoryIds ?? new List<Guid>()).Where(categories.ContainsKey).Select(x => categories[x]).ToArray();
            string pluginName;
            plugins.TryGetValue(game.PluginId, out pluginName);
            return new Dictionary<string, object>
            {
                ["id"] = game.Id.ToString(), ["name"] = game.Name,
                ["exe"] = action.Path, ["args"] = action.Arguments, ["workingDir"] = action.WorkingDirectory,
                ["installDir"] = game.InstallDirectory ?? string.Empty, ["categories"] = categoryList,
                ["pluginId"] = game.PluginId == Guid.Empty ? string.Empty : game.PluginId.ToString(),
                ["pluginName"] = pluginName ?? string.Empty, ["playtimeMinutes"] = (int)(game.Playtime / 60),
                ["lastPlayed"] = game.LastActivity.HasValue ? game.LastActivity.Value.ToString("o") : string.Empty,
                ["boxArtPath"] = FullPath(game.CoverImage), ["iconPath"] = FullPath(game.Icon),
                ["installed"] = game.IsInstalled, ["tags"] = new string[0]
            };
        }

        private ActionInfo GetAction(Game game)
        {
            var action = game.GameActions == null ? null : game.GameActions.FirstOrDefault(x => x.IsPlayAction) ?? game.GameActions.FirstOrDefault();
            return new ActionInfo
            {
                Path = action == null ? string.Empty : action.Path ?? string.Empty,
                Arguments = action == null ? string.Empty : action.Arguments ?? string.Empty,
                WorkingDirectory = action != null && !string.IsNullOrEmpty(action.WorkingDir) ? action.WorkingDir : game.InstallDirectory ?? string.Empty,
                Source = action
            };
        }

        private string FullPath(string path)
        {
            if (string.IsNullOrEmpty(path)) return string.Empty;
            try { return api.Database.GetFullFilePath(path); } catch { return string.Empty; }
        }

        private sealed class ActionInfo
        {
            public string Path, Arguments, WorkingDirectory;
            public GameAction Source;
        }
    }

    internal sealed class PlayniteSnapshot
    {
        public PlayniteSnapshot(List<Dictionary<string, object>> categories, List<Dictionary<string, object>> plugins, List<Dictionary<string, object>> games)
        {
            Categories = categories;
            Plugins = plugins;
            Games = games;
        }

        public List<Dictionary<string, object>> Categories { get; private set; }
        public List<Dictionary<string, object>> Plugins { get; private set; }
        public List<Dictionary<string, object>> Games { get; private set; }
    }
}
