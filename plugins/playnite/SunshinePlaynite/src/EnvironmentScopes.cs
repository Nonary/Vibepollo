using System;
using System.Collections.Generic;
using System.Linq;

namespace SunshinePlaynite
{
    internal sealed class EnvironmentScopes
    {
        private readonly object sync = new object();
        private readonly Dictionary<string, Dictionary<string, string>> scopes = new Dictionary<string, Dictionary<string, string>>();
        private readonly Dictionary<string, string> originals = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        private readonly List<string> order = new List<string>();

        public void Set(string id, IDictionary<string, string> values)
        {
            lock (sync)
            {
                Dictionary<string, string> old;
                scopes.TryGetValue(id, out old);
                var affected = new HashSet<string>(values.Keys, StringComparer.OrdinalIgnoreCase);
                if (old != null) affected.UnionWith(old.Keys);
                foreach (var key in values.Keys)
                {
                    if (!originals.ContainsKey(key)) originals[key] = Environment.GetEnvironmentVariable(key);
                }
                scopes[id] = new Dictionary<string, string>(values, StringComparer.OrdinalIgnoreCase);
                if (!order.Contains(id)) order.Add(id);
                Reconcile(affected);
            }
        }

        public void Clear(string id)
        {
            lock (sync)
            {
                Dictionary<string, string> old;
                if (!scopes.TryGetValue(id, out old)) return;
                scopes.Remove(id);
                order.Remove(id);
                Reconcile(old.Keys);
            }
        }

        public void RunTemporary(IDictionary<string, string> values, Action action)
        {
            lock (sync)
            {
                var previous = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                try
                {
                    foreach (var item in values)
                    {
                        previous[item.Key] = Environment.GetEnvironmentVariable(item.Key);
                        Environment.SetEnvironmentVariable(item.Key, item.Value);
                    }
                    action();
                }
                finally
                {
                    foreach (var item in previous) Environment.SetEnvironmentVariable(item.Key, item.Value);
                }
            }
        }

        private void Reconcile(IEnumerable<string> keys)
        {
            foreach (var key in keys.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                var found = false;
                string value = null;
                for (var index = order.Count - 1; index >= 0; index--)
                {
                    if (scopes[order[index]].TryGetValue(key, out value))
                    {
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    Environment.SetEnvironmentVariable(key, value);
                }
                else if (originals.TryGetValue(key, out value))
                {
                    Environment.SetEnvironmentVariable(key, value);
                    originals.Remove(key);
                }
            }
        }
    }
}
