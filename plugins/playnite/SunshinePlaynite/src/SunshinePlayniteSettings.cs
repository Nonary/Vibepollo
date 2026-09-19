using System.Collections.Generic;
using System.Reflection;
using Playnite.SDK;
using Playnite.SDK.Data;

namespace SunshinePlaynite
{
    public sealed class SunshinePlayniteSettings : ObservableObject, ISettings
    {
        private readonly SunshinePlaynitePlugin plugin;
        private SunshinePlayniteSettings editingSnapshot;
        private bool connectorEnabled = true;
        private bool notifyLibraryChanges = true;
        private bool enableDebugLogging;

        public SunshinePlayniteSettings()
        {
        }

        internal SunshinePlayniteSettings(SunshinePlaynitePlugin plugin)
        {
            this.plugin = plugin;
            var saved = plugin.LoadPluginSettings<SunshinePlayniteSettings>();
            if (saved != null) CopyFrom(saved);
        }

        public bool ConnectorEnabled
        {
            get { return connectorEnabled; }
            set { SetValue(ref connectorEnabled, value); }
        }

        public bool NotifyLibraryChanges
        {
            get { return notifyLibraryChanges; }
            set { SetValue(ref notifyLibraryChanges, value); }
        }

        public bool EnableDebugLogging
        {
            get { return enableDebugLogging; }
            set { SetValue(ref enableDebugLogging, value); }
        }

        [DontSerialize]
        public string SettingsTitle
        {
            get { return "Sunshine Playnite Connector - v" + GetPluginVersion(); }
        }

        public void BeginEdit()
        {
            editingSnapshot = new SunshinePlayniteSettings();
            editingSnapshot.CopyFrom(this);
        }

        public void CancelEdit()
        {
            if (editingSnapshot != null) CopyFrom(editingSnapshot);
            editingSnapshot = null;
        }

        public void EndEdit()
        {
            editingSnapshot = null;
            if (plugin == null) return;
            plugin.SavePluginSettings(this);
            plugin.ApplySettings();
        }

        public bool VerifySettings(out List<string> errors)
        {
            errors = new List<string>();
            return true;
        }

        private void CopyFrom(SunshinePlayniteSettings source)
        {
            ConnectorEnabled = source.ConnectorEnabled;
            NotifyLibraryChanges = source.NotifyLibraryChanges;
            EnableDebugLogging = source.EnableDebugLogging;
        }

        private static string GetPluginVersion()
        {
            var assembly = typeof(SunshinePlaynitePlugin).Assembly;
            var attribute = assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>();
            if (attribute != null && !string.IsNullOrWhiteSpace(attribute.InformationalVersion))
            {
                var version = attribute.InformationalVersion;
                var metadataIndex = version.IndexOf('+');
                return metadataIndex >= 0 ? version.Substring(0, metadataIndex) : version;
            }

            var assemblyVersion = assembly.GetName().Version;
            return assemblyVersion == null ? "unknown" : assemblyVersion.ToString(3);
        }
    }
}
