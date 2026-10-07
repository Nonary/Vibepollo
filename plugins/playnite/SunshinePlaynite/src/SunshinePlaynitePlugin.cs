using System;
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

        public override void Dispose()
        {
            connector.Dispose();
            base.Dispose();
        }
    }
}
