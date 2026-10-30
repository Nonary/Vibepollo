using System;
using System.Collections.Generic;
using System.IO;
using System.Windows.Controls;
using Playnite.SDK;
using Playnite.SDK.Events;
using Playnite.SDK.Plugins;

namespace SunshinePlaynite
{
    public sealed class SunshinePlaynitePlugin : GenericPlugin
    {
        private readonly ConnectorService connector;
        private readonly SunshinePlayniteSettings settings;
        private bool applicationStarted;

        public override Guid Id { get; } = Guid.Parse("E9B40F2D-8EED-4B5C-9149-D780E2F1268D");

        public SunshinePlaynitePlugin(IPlayniteAPI api) : base(api)
        {
            Properties = new GenericPluginProperties { HasSettings = true };
            settings = new SunshinePlayniteSettings(this);
            connector = new ConnectorService(api, LogManager.GetLogger(), settings.EnableDebugLogging);
            connector.ApplySettings(settings.NotifyLibraryChanges, settings.EnableDebugLogging);
            WarnAboutLegacyConnectors(api);
        }

        public override ISettings GetSettings(bool firstRunSettings)
        {
            return settings;
        }

        public override UserControl GetSettingsView(bool firstRunSettings)
        {
            return new SunshinePlayniteSettingsView { DataContext = settings };
        }

        public override void OnApplicationStarted(OnApplicationStartedEventArgs args)
        {
            applicationStarted = true;
            ApplySettings();
        }

        public override void OnApplicationStopped(OnApplicationStoppedEventArgs args)
        {
            applicationStarted = false;
            connector.Stop();
        }

        public override void OnGameStarting(OnGameStartingEventArgs args)
        {
            if (!connector.GameStarting(args.Game)) args.CancelStartup = true;
        }

        public override void OnGameStarted(OnGameStartedEventArgs args)
        {
            connector.GameStarted(args.Game);
        }

        public override void OnGameStopped(OnGameStoppedEventArgs args)
        {
            connector.GameStopped(args.Game);
        }

        public override void OnLibraryUpdated(OnLibraryUpdatedEventArgs args)
        {
            if (settings.NotifyLibraryChanges) connector.QueueSnapshot();
        }

        internal void ApplySettings()
        {
            connector.ApplySettings(settings.NotifyLibraryChanges, settings.EnableDebugLogging);
            if (!applicationStarted) return;

            if (settings.ConnectorEnabled)
            {
                connector.Start();
                if (settings.NotifyLibraryChanges) connector.QueueSnapshot();
            }
            else
            {
                connector.Stop();
            }
        }

        // The pre-0.5 script connector serves the same pipe name. When both are
        // loaded only one can own it, so name the duplicate in the logs.
        private void WarnAboutLegacyConnectors(IPlayniteAPI api)
        {
            var roots = new List<string>();
            try { roots.Add(Path.Combine(api.Paths.ApplicationPath, "Extensions")); } catch { }
            try { roots.Add(Path.Combine(api.Paths.ConfigurationPath, "Extensions")); } catch { }
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var root in roots)
            {
                try
                {
                    if (!System.IO.Directory.Exists(root)) continue;
                    foreach (var directory in System.IO.Directory.GetDirectories(root))
                    {
                        var module = Path.Combine(directory, "SunshinePlaynite.psm1");
                        if (!File.Exists(module) || !seen.Add(Path.GetFullPath(module))) continue;
                        connector.Warn("A legacy Sunshine Playnite script connector is installed at " + directory +
                            ". It competes with this plugin for the Sunshine pipe; delete that folder and restart Playnite.");
                    }
                }
                catch { }
            }
        }

        public override void Dispose()
        {
            connector.Dispose();
            base.Dispose();
        }
    }
}
