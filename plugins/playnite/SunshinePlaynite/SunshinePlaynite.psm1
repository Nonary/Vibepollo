<#
  Sunshine Playnite Connector (PowerShell Script Extension)
  - Connects to Sunshine over named pipes (control + data via anonymous handshake)
  - Sends categories and game metadata as JSON messages
  - Reconnects on failure and runs in background while Playnite is open

  Requires: Playnite script extension context (access to $PlayniteApi)
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$global:SunConn = $null

# Logger state
$script:LogPath = $null
$script:HostLogger = $null
$script:LogLevel = $null  # DEBUG | INFO | WARN | ERROR (case-insensitive)
$script:Bg = $null
# Cooperative shutdown (shared across all runspaces)
if (-not (Get-Variable -Name 'Cts' -Scope Script -ErrorAction SilentlyContinue)) {
  $script:Cts = New-Object System.Threading.CancellationTokenSource
}
function Test-Stopping {
  try { return ($script:Cts -and $script:Cts.IsCancellationRequested) } catch { return $false }
}
# Thread-safe map for launcher connections shared across runspaces
try {
  $lcVar = Get-Variable -Name 'LauncherConns' -Scope Global -ErrorAction SilentlyContinue
} catch { $lcVar = $null }
if (-not $lcVar -or -not ($lcVar.Value -is [System.Collections.Concurrent.ConcurrentDictionary[string,object]])) {
  $global:LauncherConns = New-Object 'System.Collections.Concurrent.ConcurrentDictionary[string,object]'
}
$script:Outbox = $null
$script:GameEventSubs = @()
$script:GameEventsRegistered = $false
$script:SnapshotTimer = $null
$script:SnapshotTimerSub = $null
$script:SnapshotIntervalMs = 3000

function Ensure-SnapshotDebounceTimer {
  try {
    if (-not $script:SnapshotTimer) {
      $t = New-Object System.Timers.Timer
      $t.Interval = [double]$script:SnapshotIntervalMs
      $t.AutoReset = $false
      $script:SnapshotTimer = $t
      try {
        $script:SnapshotTimerSub = Register-ObjectEvent -InputObject $t -EventName Elapsed -Action {
          try { Write-Log "SnapshotDebounce: firing Send-InitialSnapshot"; Send-InitialSnapshot } catch { Write-Log "SnapshotDebounce: error: $($_.Exception.Message)" }
        } -ErrorAction SilentlyContinue
      } catch { Write-Log "Ensure-SnapshotDebounceTimer: failed to register timer event: $($_.Exception.Message)" }
      Write-Log "Ensure-SnapshotDebounceTimer: timer created"
    }
  } catch { Write-Log "Ensure-SnapshotDebounceTimer: error: $($_.Exception.Message)" }
}

function Kick-SnapshotDebounce {
  try {
    Ensure-SnapshotDebounceTimer
    if ($script:SnapshotTimer) {
      try { $script:SnapshotTimer.Stop() } catch {}
      try { $script:SnapshotTimer.Interval = [double]$script:SnapshotIntervalMs } catch {}
      try { $script:SnapshotTimer.Start() } catch {}
      Write-Log "SnapshotDebounce: kicked"
    }
  } catch { Write-Log "Kick-SnapshotDebounce: error: $($_.Exception.Message)" }
}

function Resolve-LogPath {
  try {
    $appData = $env:APPDATA
    if ($appData) {
      $dir = Join-Path $appData 'Sunshine'
      try { if (-not (Test-Path $dir)) { [void](New-Item -ItemType Directory -Path $dir -Force) } } catch {}
      return (Join-Path $dir 'sunshine_playnite.log')
    }
  } catch {}
  try {
    if ($PSScriptRoot) {
      return (Join-Path $PSScriptRoot 'sunshine_playnite.log')
    }
  } catch {}
  return (Join-Path $env:TEMP 'sunshine_playnite.log')
}

function Initialize-Logging {
  try {
    if (-not $script:LogPath) { $script:LogPath = Resolve-LogPath }
    # Initialize log level from env, default to DEBUG (slightly excessive, but controlled)
    if (-not $script:LogLevel) {
      $lvl = $env:SUNSHINE_PLAYNITE_LOGLEVEL
      if ([string]::IsNullOrWhiteSpace($lvl)) { $lvl = 'DEBUG' }
      $script:LogLevel = ($lvl.Trim().ToUpperInvariant())
    }
    if (Get-Variable -Name __logger -Scope Global -ErrorAction SilentlyContinue) { $script:HostLogger = $__logger } else { $script:HostLogger = $null }
    # Basic log rotation (~1 MB)
    if (Test-Path $script:LogPath) {
      $len = (Get-Item $script:LogPath).Length
      if ($len -gt 1MB) {
        Remove-Item -Path $script:LogPath -ErrorAction SilentlyContinue
      }
    }
    $ts = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
    $hdr = @(
      "[$ts] === Sunshine Playnite Connector starting ===",
      "Process=$PID User=$env:USERNAME Session=$([Environment]::UserInteractive) PSVersion=$($PSVersionTable.PSVersion)",
      "LogLevel=$script:LogLevel LogPath=$script:LogPath PSScriptRoot=$PSScriptRoot"
    ) -join [Environment]::NewLine
    try {
      [System.IO.File]::WriteAllText($script:LogPath, $hdr + [Environment]::NewLine, [System.Text.Encoding]::UTF8)
    }
    catch {
      # Fallback to Out-File if static write fails
      $hdr | Out-File -FilePath $script:LogPath -Encoding utf8
    }
    if ($script:HostLogger) { $script:HostLogger.Info("SunshinePlaynite: logging initialized at $script:LogPath (level=$script:LogLevel)") }
  }
  catch {}
}

function Get-LogLevelRank {
  param([string]$Level)
  try {
    $useLevel = $Level
    if (-not $useLevel) { $useLevel = 'INFO' }
    switch ($useLevel.ToUpperInvariant()) {
      'DEBUG' { return 0 }
      'INFO'  { return 1 }
      'WARN'  { return 2 }
      'ERROR' { return 3 }
      default { return 1 }
    }
  } catch { return 1 }
}

function Should-Log {
  param([string]$Level)
  try {
    $curLevel = $script:LogLevel; if (-not $curLevel) { $curLevel = 'INFO' }
    $reqLevel = $Level; if (-not $reqLevel) { $reqLevel = 'INFO' }
    $cur = Get-LogLevelRank -Level $curLevel
    $req = Get-LogLevelRank -Level $reqLevel
    return ($req -ge $cur)
  } catch { return $true }
}

function Write-Log {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory=$true, Position=0)] [string]$Message,
    [ValidateSet('DEBUG','INFO','WARN','ERROR')] [string]$Level = 'INFO'
  )
  try {
    # Auto-elevate level for common error/warning words if caller didn't set explicit severity
    $effLevel = $Level
    try {
      if ($effLevel -eq 'INFO') {
        $lm = $Message
        if ($lm) {
          $lm = $lm.ToLowerInvariant()
          if ($lm -match '(?:^|\b)(error|exception)(?:\b|:)') { $effLevel = 'ERROR' }
          elseif ($lm -match '(?:\b)(failed|failure|timeout|crash(?:ed)?)(?:\b|:)') { $effLevel = 'WARN' }
        }
      }
    } catch {}
    if (-not (Should-Log -Level $effLevel)) { return }
    $ts = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
    $tid = try { [System.Threading.Thread]::CurrentThread.ManagedThreadId } catch { 0 }
    $rsId = 'NA'
    try {
      $drs = [System.Management.Automation.Runspaces.Runspace]::DefaultRunspace
      if ($drs) { $rsId = ($drs.InstanceId.ToString()) }
    } catch {}
    if ($rsId -and $rsId.Length -gt 8) { $rsId = $rsId.Substring(0,8) }

    # Extract tag from conventional "Tag: message" prefix if present
    $tag = ''
    try {
      if ($Message -match '^(?<tag>[A-Za-z0-9 \[\]#_-]{2,32}):\s') { $tag = $matches['tag'] }
    } catch {}

    $prefix = "[$ts] [$effLevel] [T#$tid RS=$rsId]"
    if ($tag) { $prefix = "$prefix [$tag]" }
    $line = "$prefix $Message"

    if (-not $script:LogPath) {
      try { $script:LogPath = Resolve-LogPath } catch {}
    }
    if ($script:LogPath) {
      try {
        [System.IO.File]::AppendAllText($script:LogPath, $line + [Environment]::NewLine, [System.Text.Encoding]::UTF8)
      }
      catch {
        Add-Content -Path $script:LogPath -Value $line -Encoding utf8
      }
    }
    if ($script:HostLogger) {
      try {
        $hl = $script:HostLogger
        switch ($effLevel) {
          'DEBUG' { if ($hl.PSObject.Methods['Debug']) { $hl.Debug("SunshinePlaynite: $Message") } else { $hl.Info("SunshinePlaynite: $Message") } }
          'INFO'  { $hl.Info("SunshinePlaynite: $Message") }
          'WARN'  { if ($hl.PSObject.Methods['Warn']) { $hl.Warn("SunshinePlaynite: $Message") } elseif ($hl.PSObject.Methods['Warning']) { $hl.Warning("SunshinePlaynite: $Message") } else { $hl.Info("SunshinePlaynite: $Message") } }
          'ERROR' { if ($hl.PSObject.Methods['Error']) { $hl.Error("SunshinePlaynite: $Message") } else { $hl.Info("SunshinePlaynite: $Message") } }
          default { $hl.Info("SunshinePlaynite: $Message") }
        }
      } catch { }
    }
  }
  catch {}
}

function Write-DebugLog {
  param([string]$Message)
  Write-Log -Message $Message -Level 'DEBUG'
}

## Removed: P/Invoke probe (WaitNamedPipe). Use NamedPipeClientStream.Connect(timeout).

# UI bridge: static C# helper to run Playnite API calls on the UI thread without requiring a PS runspace there
try {
  if (-not ([System.Management.Automation.PSTypeName]'UIBridge').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections;
using System.Reflection;
using System.Windows.Threading;

public static class UIBridge
{
    public static Dispatcher Dispatcher;
    public static object Api;

    public static void Init(Dispatcher d, object api)
    {
        Dispatcher = d;
        Api = api;
    }

    public static void Invoke(Action action)
    {
        if (action == null) return;
        var d = Dispatcher;
        if (d != null) { d.BeginInvoke(action); }
        else { action(); }
    }

    public static void InvokeWithApi(Action<object> action)
    {
        if (action == null) return;
        var api = Api;
        if (api == null) return;
        var d = Dispatcher;
        if (d != null) { d.BeginInvoke(new Action(() => action(api))); }
        else { action(api); }
    }

    public static void StartGameByGuidString(string guidStr)
    {
        if (string.IsNullOrWhiteSpace(guidStr)) return;
        Guid gid; if (!Guid.TryParse(guidStr, out gid)) return;
        var api = Api; if (api == null) return;
        try
        {
            var m = api.GetType().GetMethod("StartGame", new Type[] { typeof(Guid) });
            if (m != null) { m.Invoke(api, new object[] { gid }); return; }
        }
        catch { }
        try
        {
            var dbProp = api.GetType().GetProperty("Database");
            var db = dbProp != null ? dbProp.GetValue(api) : null; if (db == null) return;
            var gamesProp = db.GetType().GetProperty("Games");
            var games = gamesProp != null ? gamesProp.GetValue(db) as IEnumerable : null; if (games == null) return;
            object found = null;
            foreach (var g in games)
            {
                try
                {
                    var idProp = g.GetType().GetProperty("Id");
                    if (idProp != null)
                    {
                        var idVal = idProp.GetValue(g, null);
                        if (idVal is Guid)
                        {
                            var gg = (Guid)idVal;
                            if (gg.Equals(gid)) { found = g; break; }
                        }
                    }
                }
                catch { }
            }
            if (found != null)
            {
                var m2 = api.GetType().GetMethod("StartGame", new Type[] { found.GetType() });
                if (m2 != null) m2.Invoke(api, new object[] { found });
            }
        }
        catch { }
    }

    public static void StartGameByGuidStringOnUIThread(string guidStr)
    {
        var d = Dispatcher;
        if (d != null) { d.BeginInvoke(new Action(() => StartGameByGuidString(guidStr))); }
        else { StartGameByGuidString(guidStr); }
    }
}
"@ -ReferencedAssemblies @('WindowsBase')
    Write-Log "Loaded UIBridge"
  }
}
catch { Write-Log "Failed to load UIBridge: $($_.Exception.Message)" }

## Single outbox queue + minimal writer pump
try {
  if (-not ([System.Management.Automation.PSTypeName]'OutboxPump').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Threading;
using System.Collections.Concurrent;

public static class OutboxPump
{
    static CancellationTokenSource _cts;
    static Thread _thread;

    public static void Start(TextWriter writer, BlockingCollection<string> outbox)
    {
        Stop();
        _cts = new CancellationTokenSource();
        _thread = new Thread(() =>
        {
            try
            {
                while (!_cts.IsCancellationRequested)
                {
                    string line;
                    if (!outbox.TryTake(out line, 500)) { continue; }
                    try { writer.WriteLine(line); writer.Flush(); }
                    catch { /* swallow transient write errors; outer loop handles reconnect */ }
                }
            }
            catch { }
        });
        _thread.IsBackground = true;
        _thread.Start();
    }

    public static void Stop()
    {
        try { if (_cts != null) _cts.Cancel(); } catch { }
        try { if (_thread != null && _thread.IsAlive) _thread.Join(500); } catch { }
        _cts = null; _thread = null;
    }
}
"@
    Write-Log "Loaded OutboxPump"
  }
}
catch { Write-Log "Failed to load OutboxPump: $($_.Exception.Message)" }

# Per-connection writer pump to avoid UI-thread blocking on launcher pipe writes
try {
  if (-not ([System.Management.Automation.PSTypeName]'PerConnPump').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Threading;
using System.Collections.Concurrent;

public sealed class PerConnPump : IDisposable
{
    readonly TextWriter _writer;
    readonly BlockingCollection<string> _q;
    readonly Thread _t;
    readonly CancellationTokenSource _cts = new CancellationTokenSource();

    public PerConnPump(TextWriter writer, BlockingCollection<string> queue)
    {
        if (writer == null) { throw new ArgumentNullException("writer"); }
        if (queue == null) { throw new ArgumentNullException("queue"); }
        _writer = writer;
        _q = queue;
        _t = new Thread(Run);
        _t.IsBackground = true;
        try { _t.Name = "PerConnPump"; } catch { }
    }

    void Run()
    {
        try
        {
            while (!_cts.IsCancellationRequested)
            {
                string line;
                if (!_q.TryTake(out line, 500)) continue;
                try { _writer.WriteLine(line); _writer.Flush(); }
                catch { }
            }
        }
        catch { }
    }

    public void Start()
    {
        _t.Start();
    }

    public void Dispose()
    {
        try { _cts.Cancel(); } catch {}
        try { if (_t.IsAlive) _t.Join(500); } catch {}
    }
}
"@
    Write-Log "Loaded PerConnPump"
  }
}
catch { Write-Log "Failed to load PerConnPump: $($_.Exception.Message)" }

# Cross-runspace shutdown bridge for the connector data pipe
try {
  if (-not ([System.Management.Automation.PSTypeName]'ShutdownBridge').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.IO.Pipes;

public static class ShutdownBridge
{
    static object _gate = new object();
    static NamedPipeClientStream _sun;

    public static void SetSunStream(NamedPipeClientStream s)
    {
        lock (_gate) { _sun = s; }
    }

    public static void CloseSunStream()
    {
        NamedPipeClientStream s = null;
        lock (_gate)
        {
            s = _sun;
            _sun = null;
        }
        try { if (s != null) s.Dispose(); } catch { }
    }
}
"@
    Write-Log "Loaded ShutdownBridge"
  }
}
catch { Write-Log "Failed to load ShutdownBridge: $($_.Exception.Message)" }

# Outbox shared between UI and background runspaces (global singleton)
try {
  $haveGlobal = $null
  try { $haveGlobal = Get-Variable -Name 'Outbox' -Scope Global -ErrorAction SilentlyContinue } catch {}
  if (-not $haveGlobal -or -not ($global:Outbox -is [System.Collections.Concurrent.BlockingCollection[string]])) {
    $global:Outbox = New-Object 'System.Collections.Concurrent.BlockingCollection[string]'
    Write-Log "Outbox initialized (global singleton)"
  } else {
    Write-Log "Outbox bound to existing global singleton"
  }
  $script:Outbox = $global:Outbox
  $Outbox = $script:Outbox
}
catch {
  $global:Outbox = New-Object 'System.Collections.Concurrent.BlockingCollection[string]'
  $script:Outbox = $global:Outbox
  $Outbox = $script:Outbox
  Write-Log "Outbox reinitialized due to error: $($_.Exception.Message)"
}

function Get-FullPipeName {
  param([string]$Name)
  if ($Name -like "\\\\.\\pipe\\*") { return $Name }
  return "\\\\.\\pipe\\$Name"
}

## Removed Test-PipeAvailable (pre-probe not needed)

function Connect-SunshinePipe {
  param(
    [string]$PublicName = 'sunshine_playnite_connector',
    [int]$TimeoutMs = 5000,
    [System.Threading.CancellationToken]$Token = $null
  )

  # Mandatory anonymous handshake protocol:
  # 1) Connect to control pipe ($PublicName)
  # 2) Read 80-byte wchar[40] data-pipe name (AnonConnectMsg)
  # 3) Send single-byte ACK (0x02)
  # 4) Disconnect control pipe
  # 5) Connect to returned data pipe name and build UTF-8 line reader/writer

  $control = $null
  $data = $null
  $reader = $null
  $writer = $null
  $ACK = 0x02

  try {
    if ($Token -and $Token.IsCancellationRequested) { throw [System.OperationCanceledException]::new() }
    Write-Log "Handshake: connecting control pipe '$PublicName' (timeout=${TimeoutMs}ms)"
    $control = New-Object System.IO.Pipes.NamedPipeClientStream('.', $PublicName, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
    $control.Connect([Math]::Max(1000, [int]$TimeoutMs))
    Write-Log "Handshake: control connected (IsConnected=$($control.IsConnected))"

    # Read fixed-size AnonConnectMsg (wchar[40] => 80 bytes) with a simple synchronous read
    $expected = 80
    $buf = New-Object byte[] $expected
    $off = 0
    try { $control.ReadTimeout = 3000 } catch { Write-Log "Handshake: control stream does not support ReadTimeout; proceeding" }
    while ($off -lt $expected) {
      if ($Token -and $Token.IsCancellationRequested) { throw [System.OperationCanceledException]::new() }
      $n = $control.Read($buf, $off, ($expected - $off))
      if ($n -le 0) { throw "Control pipe closed during handshake ($off/$expected)" }
      $off += $n
    }
    # Decode wchar[40] to .NET string; trim trailing nulls
    $pipeName = [System.Text.Encoding]::Unicode.GetString($buf)
    $pipeName = $pipeName.Trim([char]0)
    if (-not $pipeName) { throw "Handshake received empty data pipe name" }
    Write-Log "Handshake: received data pipe name '$pipeName'"

    # Send ACK (single byte 0x02)
    $ackBuf = [byte[]]@([byte]$ACK)
    $control.Write($ackBuf, 0, 1)
    $control.Flush()
    Write-Log "Handshake: ACK sent"

    # Close control and connect to data pipe
    try { $control.Dispose() } catch {}
    $control = $null

    $data = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
    if ($Token -and $Token.IsCancellationRequested) { throw [System.OperationCanceledException]::new() }
    $data.Connect(3000)
    if ($PublicName -eq 'sunshine_playnite_connector') {
      try { [ShutdownBridge]::SetSunStream($data) } catch {}
    }
    try { $data.ReadTimeout = 5000 } catch {}
    try { $data.WriteTimeout = 5000 } catch {}
    $writer = New-Object System.IO.StreamWriter($data, [System.Text.Encoding]::UTF8, 8192, $true)
    $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader($data, [System.Text.Encoding]::UTF8, $false, 8192, $true)
    Write-Log "Handshake: data pipe connected (IsConnected=$($data.IsConnected))"
    return @{ Stream = $data; Writer = $writer; Reader = $reader }
  }
  catch {
    if ($data -and $PublicName -eq 'sunshine_playnite_connector') { try { [ShutdownBridge]::CloseSunStream() } catch {} }
    if ($reader) { try { $reader.Dispose() } catch {} }
    if ($writer) { try { $writer.Dispose() } catch {} }
    if ($data) { try { $data.Dispose() } catch {} }
    if ($control) { try { $control.Dispose() } catch {} }
    $ex = $_.Exception
    Write-Log "Handshake connect failed: $($ex.GetType().FullName): $($ex.Message) HResult=$([String]::Format('0x{0:X8}', $ex.HResult))"
    throw
  }
}

function Send-JsonMessage {
  param(
    [Parameter(Mandatory)] [string]$Json,
    [switch]$AllowConnectIfMissing
  )
  # Single strategy: enqueue to the shared outbox; background writer will flush to pipe
  try { $null = $Outbox.Add($Json); Write-Log ("Queued line of {0} chars" -f $Json.Length) }
  catch { Write-Log "Failed to enqueue message: $($_.Exception.Message)" }
}

# Discover running launcher helper processes and extract their public GUID and game id
function Get-LauncherProcesses {
  # Tolerate Playnite.Launcher / playnite-launcher / playnite_launcher / ... (+ optional suffix)
  $nameRegex = '(?i)^playnite[\._-]?launcher(?:.*)?\.exe$'
  $found = New-Object System.Collections.ArrayList

  function Add-Candidate {
    param($pid, $name, $cmdline)
    if (-not $name -or ($name -notmatch $nameRegex)) { return }
    if (-not $cmdline) {
      Write-Log "LauncherProbe: PID=$pid Name=$name has empty/blocked CommandLine"
      return
    }

    $pub = $null; $gid = $null

    # Accept -/-- or /, allow space or '=' between key and value
    if ($cmdline -match '(?i)(?:^|\s)[-/]{1,2}public[-_]?guid(?:\s+|=)\{?([0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12})\}?') {
      $pub = $matches[1]
    }
    if ($cmdline -match '(?i)(?:^|\s)[-/]{1,2}game[-_]?id(?:\s+|=)\{?([0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12})\}?') {
      $gid = $matches[1]
    }

    if ($pub) {
      [void]$found.Add(@{ Pid = [int]$pid; PublicGuid = $pub; GameId = $gid })
      $gidOut = $gid; if (-not $gidOut) { $gidOut = '' }
      Write-DebugLog ("LauncherProbe: candidate pid={0} pguid={1} game={2}" -f $pid, $pub, $gidOut)
    }
  }

  # 1) CIM/WMI broad LIKE filter
  try {
    $q = "SELECT ProcessId, Name, CommandLine FROM Win32_Process WHERE Name LIKE 'Playnite%Launcher%.exe'"
    $cim = Get-CimInstance -Query $q -ErrorAction SilentlyContinue
    foreach ($p in ($cim | Where-Object { $_.Name -match $nameRegex })) {
      Add-Candidate -pid $p.ProcessId -name $p.Name -cmdline $p.CommandLine
    }
  }
  catch { Write-Log "LauncherProbe: CIM LIKE query failed: $($_.Exception.Message)" }

  # 2) Fallback: Get-Process then query each PID’s CommandLine via CIM/WMI
  try {
    foreach ($gp in (Get-Process -Name 'playnite*' -ErrorAction SilentlyContinue)) {
      $pid = [int]$gp.Id
      $name = $gp.Name
      $cmd = $null
      try {
        $c = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $pid) -ErrorAction SilentlyContinue
        if (-not $c) { $c = Get-WmiObject Win32_Process -Filter ("ProcessId={0}" -f $pid) -ErrorAction SilentlyContinue }
        if ($c) { $cmd = [string]$c.CommandLine }
      }
      catch {}
      Add-Candidate -pid $pid -name $name -cmdline $cmd
    }
  }
  catch { Write-Log "LauncherProbe: Get-Process pass failed: $($_.Exception.Message)" }

  # Marker-based discovery removed: do not scan %APPDATA%\Sunshine\playnite_launcher for JSON markers

  if ($found.Count -gt 0) {
    Write-Log ("LauncherProbe: total candidates={0}" -f $found.Count)
    try {
      $samples = ($found | Select-Object -First 3)
      $sdesc = ($samples | ForEach-Object { "pid=$($_.Pid) guid=$($_.PublicGuid)" }) -join '; '
      if ($sdesc) { Write-DebugLog ("LauncherProbe: sample: {0}" -f $sdesc) }
    } catch {}
  }
  return , $found  # ensure array even if single
}


# Ensure connections to discovered launchers; start reader threads to handle commands
function Ensure-LauncherConnections {
  $found = Get-LauncherProcesses
  foreach ($it in $found) {
    $guid = [string]$it.PublicGuid
    # Ensure braces to match the server-side control pipe naming
    $pipeGuid = if ($guid -match '^\{[0-9A-Fa-f\-]{36}\}$') { $guid } else { '{' + $guid + '}' }
    $guid = $pipeGuid
    try {
      $exists = $false
      try { $exists = $global:LauncherConns.ContainsKey($guid) } catch { $exists = $false }
      if ($exists) { continue }
    } catch {}
    try {
      $pipe = "sunshine_playnite_ctl_$guid"
      Write-Log "LauncherWatcher: connecting to pipe '$pipe' for pid=$($it.Pid)"
      # Pass cancellation token so Connect can be interrupted on shutdown
      $conn = Connect-SunshinePipe -PublicName $pipe -TimeoutMs 10000 -Token $Cts.Token
      if (-not $conn) { continue }
      $connInfo = @{ Stream = $conn.Stream; Writer = $conn.Writer; Reader = $conn.Reader; Pid = $it.Pid; GameId = $it.GameId }
      $gOut = $it.GameId; if (-not $gOut) { $gOut = '' }
      Write-DebugLog ("LauncherWatcher: new connection candidate guid={0} pid={1} game={2}" -f $guid, $it.Pid, $gOut)
      # Attach a per-connection outbox + pump to avoid UI-thread blocking
      try {
        $connInfo.Outbox = New-Object 'System.Collections.Concurrent.BlockingCollection[string]'
        if (-not $connInfo.Writer) {
          $connInfo.Writer = New-Object System.IO.StreamWriter($conn.Stream, [System.Text.Encoding]::UTF8, 8192, $true)
          $connInfo.Writer.AutoFlush = $true
        }
        $connInfo.Pump = New-Object PerConnPump($connInfo.Writer, $connInfo.Outbox)
        $connInfo.Pump.Start()
        Write-DebugLog ("LauncherWatcher: per-connection pump started for {0}" -f $guid)
      } catch { Write-Log "LauncherWatcher: failed to start per-connection pump: $($_.Exception.Message)" }
      try {
        if (-not $global:LauncherConns.TryAdd($guid, $connInfo)) {
          Write-Log "LauncherWatcher: connection already present for $guid; skipping add"
          continue
        }
      } catch { $global:LauncherConns[$guid] = $connInfo }

      # Send a tiny sanity status so the launcher log confirms data-pipe reads (enqueue; background will write)
      try {
        $hello = @{ type = 'status'; status = @{ name = 'hello'; id = [string]$it.GameId } } | ConvertTo-Json -Compress
        if ($connInfo.Outbox) { $null = $connInfo.Outbox.Add($hello) }
        Write-Log "LauncherWatcher: enqueued hello probe to ${guid}"
      } catch { Write-Log ("LauncherWatcher: hello probe enqueue failed for {0}: {1}" -f $guid, $_.Exception.Message) }

      # Start a dedicated runspace for this connection reader to avoid no-runspace errors
      $rs = [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
      $rs.ApartmentState = [System.Threading.ApartmentState]::MTA
      $rs.ThreadOptions = [System.Management.Automation.Runspaces.PSThreadOptions]::UseNewThread
      $rs.Open()
      if ($PlayniteApi) { $rs.SessionStateProxy.SetVariable('PlayniteApi', $PlayniteApi) }
      if ($Outbox) { $rs.SessionStateProxy.SetVariable('Outbox', $Outbox) }
      try { if ($global:LauncherConns) { $rs.SessionStateProxy.SetVariable('LauncherConns', $global:LauncherConns) } } catch {}
      if ($PSScriptRoot) { $rs.SessionStateProxy.SetVariable('PSScriptRoot', $PSScriptRoot) }
      $rs.SessionStateProxy.SetVariable('Cts', $Cts)
      $rs.SessionStateProxy.SetVariable('ConnGuid', $guid)
      $rs.SessionStateProxy.SetVariable('Conn', $connInfo)
      $ps = [System.Management.Automation.PowerShell]::Create()
      $ps.Runspace = $rs
      $modulePath = try { Join-Path $PSScriptRoot 'SunshinePlaynite.psm1' } catch { $null }
      if ($modulePath) { $ps.AddScript("Import-Module -Force '$modulePath'") | Out-Null }
      # Ensure the runspace's $global:LauncherConns points to the shared table passed in
      $ps.AddScript('$global:LauncherConns = $LauncherConns') | Out-Null
      $ps.AddScript('Start-LauncherConnReader -Guid $ConnGuid -Conn $Conn -Token $Cts.Token') | Out-Null
      $h = $ps.BeginInvoke()
      $connInfo.Runspace = $rs; $connInfo.PowerShell = $ps; $connInfo.Handle = $h
      Write-Log "LauncherWatcher: connected to $pipe"
    }
    catch { Write-Log "LauncherWatcher: failed to connect to ${guid}: $($_.Exception.Message)" }
  }
}

# Reader loop for a single launcher connection; runs inside its own PowerShell runspace
function Start-LauncherConnReader {
  param([Parameter(Mandatory)][string]$Guid,
        [Parameter(Mandatory)][hashtable]$Conn,
        [System.Threading.CancellationToken]$Token = $script:Cts.Token)
  try {
    while ($Conn -and $Conn.Reader -and $Conn.Stream -and $Conn.Stream.CanRead -and -not ($Token -and $Token.IsCancellationRequested)) {
      $line = $null
      try { $line = $Conn.Reader.ReadLine() } catch { if ($Token -and $Token.IsCancellationRequested) { break } else { break } }
      if ($null -eq $line) { break }
      Write-DebugLog ("LauncherConn[{0}]: received line len={1}" -f $Guid, $line.Length)
      try {
        $obj = $line | ConvertFrom-Json -ErrorAction Stop
        if ($obj.type -eq 'command' -and $obj.command -eq 'launch' -and $obj.id) {
          [UIBridge]::StartGameByGuidStringOnUIThread([string]$obj.id)
          Write-Log "LauncherConn[$Guid]: launch dispatched for $($obj.id)"
        }
        elseif ($obj.type -and $obj.command) {
          Write-DebugLog ("LauncherConn[{0}]: unhandled command type={1} cmd={2}" -f $Guid, [string]$obj.type, [string]$obj.command)
        }
      } catch {
        if ($Token -and $Token.IsCancellationRequested) { break }
        Write-Log "LauncherConn[$Guid]: parse failure: $($_.Exception.Message)"
      }
    }
  } catch {
    if (-not ($Token -and $Token.IsCancellationRequested)) {
      Write-Log "LauncherConn[$Guid]: reader crashed: $($_.Exception.Message)"
    }
  }
  finally {
    try { if ($Conn.Reader) { $Conn.Reader.Dispose() } } catch {}
    try { if ($Conn.Writer) { $Conn.Writer.Dispose() } } catch {}
    try { if ($Conn.Stream) { $Conn.Stream.Dispose() } } catch {}
    try { if ($Conn.Pump) { $Conn.Pump.Dispose() } } catch {}
    try { $tmp = $null; [void]$global:LauncherConns.TryRemove($Guid, [ref]$tmp) } catch {}
    Write-Log "LauncherConn[$Guid]: disconnected"
  }
}

function Get-PlayniteCategories {
  # Be defensive: Database or Categories can be null (no categories created yet)
  if (-not $PlayniteApi) { return @() }
  $cats = @()
  try {
    $db = $null
    try { $db = $PlayniteApi.Database } catch {}
    if (-not $db) { return @() }
    $src = $null
    try { $src = $db.Categories } catch {}
    if ($src) {
      foreach ($c in $src) {
        try { $cats += @{ id = $c.Id.ToString(); name = $c.Name } } catch {}
      }
    }
  } catch {}
  Write-Log "Collected $($cats.Count) categories"
  try {
    $s = ($cats | Select-Object -First 3)
    $names = ($s | ForEach-Object { $_.name }) -join ', '
    if ($names) { Write-DebugLog ("Categories sample: {0}" -f $names) }
  } catch {}
  return $cats
}

function Get-CategoryNamesMap {
  $map = @{}
  try {
    $db = $null
    try { $db = $PlayniteApi.Database } catch {}
    if (-not $db) { return $map }
    $src = $null
    try { $src = $db.Categories } catch {}
    if (-not $src) { return $map }
    foreach ($c in $src) {
      try { $map[$c.Id] = $c.Name } catch {}
    }
  } catch {}
  return $map
}

function Get-GameActionInfo {
  param([object]$Game)
  # Best-effort extraction of primary play action
  $exe = ''
  $args = ''
  $workDir = ''
  try {
    $actions = $Game.GameActions
    if ($actions -and $actions.Count -gt 0) {
      $play = $actions | Where-Object { $_.IsPlayAction } | Select-Object -First 1
      if (-not $play) { $play = $actions[0] }
      if ($play) {
        if ($play.Path) { $exe = $play.Path }
        if ($play.Arguments) { $args = $play.Arguments }
        if ($play.WorkingDir) { $workDir = $play.WorkingDir }
      }
    }
  }
  catch {}
  if (-not $workDir -and $Game.InstallDirectory) { $workDir = $Game.InstallDirectory }
  return @{ exe = $exe; args = $args; workingDir = $workDir }
}

function Get-BoxArtPath {
  param([object]$Game)
  try {
    if ($Game.CoverImage) {
      return $PlayniteApi.Database.GetFullFilePath($Game.CoverImage)
    }
  }
  catch {}
  return ''
}

function Get-PlayniteGames {
  if (-not $PlayniteApi) { return @() }
  $catMap = Get-CategoryNamesMap
  $games = @()
  foreach ($g in $PlayniteApi.Database.Games) {
    $act = Get-GameActionInfo -Game $g
    $catNames = @()
    if ($g.CategoryIds) { foreach ($cid in $g.CategoryIds) { if ($catMap.ContainsKey($cid)) { $catNames += $catMap[$cid] } } }
    $playtimeMin = 0
    try { if ($g.Playtime) { $playtimeMin = [int]([double]$g.Playtime / 60.0) } } catch {}
    $lastPlayed = ''
    try { if ($g.LastActivity) { $lastPlayed = ([DateTime]$g.LastActivity).ToString('o') } } catch {}
    $boxArt = Get-BoxArtPath -Game $g
    # Determine installed state explicitly; fallback to InstallDirectory when IsInstalled is unavailable
    $installed = $false
    try {
      if ($null -ne $g.IsInstalled) { $installed = [bool]$g.IsInstalled }
      elseif ($g.InstallDirectory) { $installed = $true }
    } catch {}
    $games += @{
      id              = $g.Id.ToString()
      name            = $g.Name
      exe             = $act.exe
      args            = $act.args
      workingDir      = $act.workingDir
      categories      = $catNames
      playtimeMinutes = $playtimeMin
      lastPlayed      = $lastPlayed
      boxArtPath      = $boxArt
      installed       = $installed
      tags            = @() # TODO: fill from $g.TagIds if needed
    }
  }
  Write-Log "Collected $($games.Count) games"
  try {
    $s = ($games | Select-Object -First 3)
    $names = ($s | ForEach-Object { $_.name }) -join ', '
    if ($names) { Write-DebugLog ("Games sample: {0}" -f $names) }
  } catch {}
  return $games
}

function Send-InitialSnapshot {
  Write-Log "Building initial snapshot"
  # Force array to avoid null Count() semantics if provider returns $null
  $categories = @(Get-PlayniteCategories)
  $json = @{ type = 'categories'; payload = $categories } | ConvertTo-Json -Depth 6 -Compress
  Send-JsonMessage -Json $json -AllowConnectIfMissing
  $catCount = 0; try { $catCount = [int]$categories.Count } catch { $catCount = 0 }
  Write-Log ("Sent categories snapshot ({0})" -f $catCount)

  $games = Get-PlayniteGames
  # Send in batches to avoid overly large messages
  $batchSize = 100
  Write-DebugLog ("Initial snapshot: totalGames={0} batchSize={1}" -f $games.Count, $batchSize)
  for ($i = 0; $i -lt $games.Count; $i += $batchSize) {
    $chunk = $games[$i..([Math]::Min($i + $batchSize - 1, $games.Count - 1))]
    $jsonG = @{ type = 'games'; payload = $chunk } | ConvertTo-Json -Depth 8 -Compress
    Send-JsonMessage -Json $jsonG -AllowConnectIfMissing
    $batchIndex = [int]([double]$i / $batchSize) + 1
    $chunkCount = $chunk.Count
    Write-Log ("Sent games batch {0} with {1} items" -f $batchIndex, $chunkCount)
  }
  $gamesCount = $games.Count
  Write-Log ("Initial snapshot completed: categories={0} games={1}" -f $catCount, $gamesCount)
}

function Start-ConnectorLoop {
  param([System.Threading.CancellationToken]$Token = $script:Cts.Token)
  try { if (-not $script:LogPath) { Initialize-Logging } } catch {}
  Write-Log "Connector loop: starting"
  while (-not ($Token -and $Token.IsCancellationRequested)) {
    try {
      if (-not $global:SunConn -or -not $global:SunConn.Stream -or -not $global:SunConn.Stream.CanWrite) {
        if ($Token -and $Token.IsCancellationRequested) { break }
        Write-Log "Connector loop: attempting connection"
        # Keep timeouts small so shutdown isn't held by Connect()
        $global:SunConn = Connect-SunshinePipe -TimeoutMs 2000 -Token $Token
        Write-Log "Connector loop: connected"
        # Start writer pump bound to this connection
        try { [OutboxPump]::Start($global:SunConn.Writer, $global:Outbox); Write-DebugLog "Connector loop: OutboxPump started" } catch { Write-Log "Failed to start OutboxPump: $($_.Exception.Message)" }
        # Send initial snapshot on (re)connect
        Send-InitialSnapshot
      }
      else {
        Write-Log "Connector loop: using existing connection"
      }
      # Reader loop: handle simple line-delimited command messages from Sunshine
      while ($global:SunConn -and $global:SunConn.Stream -and $global:SunConn.Stream.CanRead -and -not ($Token -and $Token.IsCancellationRequested)) {
        try {
          $line = $global:SunConn.Reader.ReadLine()
        }
        catch {
          if ($Token -and $Token.IsCancellationRequested) { break }
          Write-Log "Reader exception: $($_.Exception.Message)"
          break
        }
        if ($null -eq $line) { Write-Log "Reader: EOF from server"; break }
        $lineLen = $line.Length
        Write-Log ("Received line ({0} chars)" -f $lineLen)
        try {
          $obj = $line | ConvertFrom-Json -ErrorAction Stop
          if ($obj.type -eq 'command' -and $obj.command -eq 'launch' -and $obj.id) {
            try {
              [UIBridge]::StartGameByGuidStringOnUIThread([string]$obj.id)
              Write-Log "Dispatched launch to UI thread via UIBridge.StartGameByGuidStringOnUIThread"
            } catch { Write-Log "Failed to dispatch launch: $($_.Exception.Message)" }
          } elseif ($obj.type -eq 'command' -and $obj.command -eq 'stop') {
            # New: forward a synthetic gameStopped status to the matching launcher connection(s)
            try {
              $targetId = ''
              try { if ($obj.id) { $targetId = [string]$obj.id } } catch {}
              Send-StopSignalToLauncher -GameId $targetId
              Write-Log ("Forwarded stop signal to launcher(s) for id='{0}'" -f $targetId)
            } catch { Write-Log ("Failed to forward stop signal: {0}" -f $_.Exception.Message) }
          } else {
            try { Write-DebugLog ("Connector loop: unhandled message type={0} cmd={1}" -f ([string]$obj.type), ([string]$obj.command)) } catch {}
          }
        }
        catch {
          if ($Token -and $Token.IsCancellationRequested) { break }
          Write-Log "Failed to parse/handle line: $($_.Exception.Message)"
        }
      }
    }
    catch {
      if ($Token -and $Token.IsCancellationRequested) { break }
      Write-Log "Connector loop: connection attempt failed: $($_.Exception.Message)"
    }
    try { [ShutdownBridge]::CloseSunStream() } catch {}
    $global:SunConn = $null
    try { [OutboxPump]::Stop() } catch {}
    if ($Token -and $Token.WaitHandle.WaitOne(300)) { break }  # was Start-Sleep -Seconds 3; now cancellable short waits
  }
  Write-Log "Connector loop: exiting (stopping=$([bool]($Token -and $Token.IsCancellationRequested)))"
}

function Start-LauncherWatcherLoop {
  param([System.Threading.CancellationToken]$Token = $script:Cts.Token)
  try { if (-not $script:LogPath) { Initialize-Logging } } catch {}
  Write-Log "LauncherWatcher: started"
  while (-not ($Token -and $Token.IsCancellationRequested)) {
    try { Ensure-LauncherConnections } catch { if ($Token -and $Token.IsCancellationRequested) { break } else { Write-Log "LauncherWatcher: error: $($_.Exception.Message)" } }
    if ($Token -and $Token.WaitHandle.WaitOne(1500)) { break }
  }
  Write-Log "LauncherWatcher: exiting"
}

# Synchronous single-shot connection probe to ensure logging works even before background loop
# Removed legacy Try-ConnectOnce path; background loop owns connection + snapshot

function OnApplicationStarted() {
  try {
    Initialize-Logging
    Write-Log "OnApplicationStarted invoked"
      if (-not $script:Cts) { $script:Cts = New-Object System.Threading.CancellationTokenSource }
      # Initialize UI bridge with Playnite's Dispatcher and API
      try {
        $dispatcher = $null
      try {
        $app = [System.Windows.Application]::Current
        if ($app -and $app.Dispatcher) { $dispatcher = $app.Dispatcher }
      }
      catch {}
      if (-not $dispatcher) {
        try { $dispatcher = [System.Windows.Threading.Dispatcher]::CurrentDispatcher } catch {}
      }
      [UIBridge]::Init($dispatcher, $PlayniteApi)
      $hasDisp = ([bool][UIBridge]::Dispatcher)
      $hasApi = ([bool][UIBridge]::Api)
      Write-Log ("UIBridge initialized: Dispatcher={0} Api={1}" -f $hasDisp, $hasApi)
    }
    catch { Write-Log "Failed to initialize UIBridge: $($_.Exception.Message)" }
    # Start background connector in a dedicated PowerShell runspace for proper function/variable scope
    try {
      $modulePath = try { Join-Path $PSScriptRoot 'SunshinePlaynite.psm1' } catch { $null }
      $rs = [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
      $rs.ApartmentState = [System.Threading.ApartmentState]::MTA
      $rs.ThreadOptions = [System.Management.Automation.Runspaces.PSThreadOptions]::UseNewThread
      $rs.Open()
      if ($PlayniteApi) { $rs.SessionStateProxy.SetVariable('PlayniteApi', $PlayniteApi) }
      if ($Outbox) { $rs.SessionStateProxy.SetVariable('Outbox', $Outbox) }
      if ($PSScriptRoot) { $rs.SessionStateProxy.SetVariable('PSScriptRoot', $PSScriptRoot) }
      $rs.SessionStateProxy.SetVariable('Cts', $script:Cts)
      # Ensure connector runspace can access the shared launcher connections table
      try { $rs.SessionStateProxy.SetVariable('LauncherConns', $global:LauncherConns) } catch {}
      # No need to pass UI objects; UIBridge holds static references accessible across runspaces
      $ps = [System.Management.Automation.PowerShell]::Create()
      $ps.Runspace = $rs
      if ($modulePath) { $ps.AddScript("Import-Module -Force '$modulePath'") | Out-Null }
      # Rebind this runspace's $global:LauncherConns to the injected shared instance
      $ps.AddScript('$global:LauncherConns = $LauncherConns') | Out-Null
      $ps.AddScript('Start-ConnectorLoop -Token $Cts.Token') | Out-Null
      $handle = $ps.BeginInvoke()
      $script:Bg = @{ Runspace = $rs; PowerShell = $ps; Handle = $handle }
      Write-Log "OnApplicationStarted: background runspace started"
      try { Write-DebugLog ("OnApplicationStarted: RS1={0}" -f $rs.InstanceId) } catch {}

      # Start launcher watcher in a separate runspace
      $rs2 = [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
      $rs2.ApartmentState = [System.Threading.ApartmentState]::MTA
      $rs2.ThreadOptions = [System.Management.Automation.Runspaces.PSThreadOptions]::UseNewThread
      $rs2.Open()
      if ($PlayniteApi) { $rs2.SessionStateProxy.SetVariable('PlayniteApi', $PlayniteApi) }
      if ($Outbox) { $rs2.SessionStateProxy.SetVariable('Outbox', $Outbox) }
      # Inject the existing shared LauncherConns object so this runspace uses the same instance
      try { $rs2.SessionStateProxy.SetVariable('LauncherConns', $global:LauncherConns) } catch {}
      if ($PSScriptRoot) { $rs2.SessionStateProxy.SetVariable('PSScriptRoot', $PSScriptRoot) }
      $rs2.SessionStateProxy.SetVariable('Cts', $script:Cts)
      $ps2 = [System.Management.Automation.PowerShell]::Create()
      $ps2.Runspace = $rs2
      if ($modulePath) { $ps2.AddScript("Import-Module -Force '$modulePath'") | Out-Null }
      # After module import (which initializes its own table), rebind to the injected shared instance
      $ps2.AddScript('$global:LauncherConns = $LauncherConns') | Out-Null
      $ps2.AddScript('Start-LauncherWatcherLoop -Token $Cts.Token') | Out-Null
      $handle2 = $ps2.BeginInvoke()
      $script:Bg2 = @{ Runspace = $rs2; PowerShell = $ps2; Handle = $handle2 }
      Write-Log "OnApplicationStarted: launcher watcher runspace started"
      try { Write-DebugLog ("OnApplicationStarted: RS2={0}" -f $rs2.InstanceId) } catch {}
      try { Register-GameCollectionEvents } catch { Write-Log "OnApplicationStarted: event registration failed: $($_.Exception.Message)" }
      try { Ensure-SnapshotDebounceTimer } catch {}
    }
    catch {
      Write-Log "OnApplicationStarted: failed to start background runspace: $($_.Exception.Message)"
    }
  }
  catch {
    # Fallback: run synchronously once
    Write-Log "OnApplicationStarted: background failed, running connector synchronously"
    Start-ConnectorLoop
  }
}

## Removed Process-CmdQueue (no longer used)

function Send-StatusMessage {
  param([string]$Name, [object]$Game)
  $instDir = ''
  try { if ($Game.InstallDirectory) { $instDir = $Game.InstallDirectory } } catch {}
  try {
    $actions = $null
    try { $actions = $Game.GameActions } catch {}
    if ($actions -and $actions.Count -gt 0) {
      $play = $actions | Where-Object { $_.IsPlayAction } | Select-Object -First 1
      if (-not $play) { $play = $actions[0] }
      if ($play) {
        $isEmu = $false
        try {
          $t = $play.Type
          if ($t) {
            $tStr = try { $t.ToString() } catch { [string]$t }
            if ($tStr -and ($tStr -match 'Emulator')) { $isEmu = $true }
          }
        } catch {}
        $emuIdStr = ''
        try { $emuIdStr = [string]$play.EmulatorId } catch {}
        if (-not $isEmu -and $emuIdStr -and ($emuIdStr -notmatch '(?i)^0{8}-0{4}-0{4}-0{4}-0{12}$')) { $isEmu = $true }
        if ($isEmu -and $PlayniteApi) {
          try {
            $db = $null
            try { $db = $PlayniteApi.Database } catch {}
            if ($db -and $db.Emulators -and $emuIdStr) {
              foreach ($emu in $db.Emulators) {
                try {
                  $eid = [string]$emu.Id
                  if ($eid -and ($eid.ToLowerInvariant() -eq $emuIdStr.ToLowerInvariant())) {
                    $edir = ''
                    try { $edir = $emu.InstallDir } catch {}
                    if ($edir) { $instDir = $edir }
                    break
                  }
                } catch {}
              }
            }
          } catch {}
        }
      }
    }
  } catch {}
  $payload = @{ type = 'status'; status = @{ name = $Name; id = $Game.Id.ToString(); installDir = $instDir; exe = (Get-GameActionInfo -Game $Game).exe } } | ConvertTo-Json -Depth 5 -Compress
  Send-JsonMessage -Json $payload
  try {
    $keys = @()
    try { $keys = @($global:LauncherConns.Keys) } catch { $keys = @() }
    $count = $keys.Count
    Write-Log ("Status broadcast: enqueue to {0} launcher connection(s)" -f $count)
    foreach ($guid in $keys) {
      $conn = $null
      try { $conn = $global:LauncherConns[$guid] } catch {}
      try {
        if ($conn -and $conn.Outbox) {
          $null = $conn.Outbox.Add($payload)
          Write-Log ("Status broadcast: queued for {0}" -f $guid)
        } else {
          Write-Log ("Status broadcast: missing conn/outbox for {0}" -f $guid)
        }
      } catch {
        Write-Log ("Status broadcast: enqueue failed for {0}: {1}" -f $guid, $_.Exception.Message)
      }
    }
  }
  catch { Write-Log ("Status broadcast: unexpected failure: {0}" -f $_.Exception.Message) }
}

# Send a synthetic 'gameStopped' status to launcher connection(s) to trigger graceful staging.
function Send-StopSignalToLauncher {
  param([string]$GameId)
  try {
    $idStr = if ($GameId) { $GameId } else { '' }
    $payload = @{ type = 'status'; status = @{ name = 'gameStopped'; id = $idStr } } | ConvertTo-Json -Depth 4 -Compress
  } catch {
    Write-Log "StopSignal: failed to build JSON payload"
    return
  }
  try {
    $keys = @()
    try { $keys = @($global:LauncherConns.Keys) } catch { $keys = @() }
    $selected = New-Object System.Collections.ArrayList
    $norm = { param($s) if (-not $s) { return '' } ($s -replace '[{}]', '').ToLowerInvariant() }
    if ($GameId) {
      foreach ($guid in $keys) {
        try {
          $conn = $global:LauncherConns[$guid]
          if (-not $conn) { continue }
          $cid = ''
          try { $cid = [string]$conn.GameId } catch {}
          if (-not $cid) { continue }
          if ((& $norm $cid) -eq (& $norm $GameId)) { [void]$selected.Add($guid) }
        } catch {}
      }
      if ($selected.Count -eq 0) {
        Write-Log ("StopSignal: no matching LauncherConns for id='{0}', broadcasting to all ({1})" -f $GameId, $keys.Count)
        foreach ($guid in $keys) { [void]$selected.Add($guid) }
      }
    } else {
      foreach ($guid in $keys) { [void]$selected.Add($guid) }
    }
    Write-Log ("StopSignal: targeting {0} connection(s)" -f $selected.Count)
    foreach ($guid in $selected) {
      $conn = $null
      try { $conn = $global:LauncherConns[$guid] } catch {}
      if (-not $conn) { continue }
      try {
        if ($conn.Outbox) { $null = $conn.Outbox.Add($payload); Write-Log ("StopSignal: queued for {0}" -f $guid) }
        elseif ($conn.Writer) { $conn.Writer.WriteLine($payload); $conn.Writer.Flush(); Write-Log ("StopSignal: wrote directly for {0}" -f $guid) }
        else { Write-Log ("StopSignal: no outbox/writer for {0}" -f $guid) }
      } catch {
        Write-Log ("StopSignal: enqueue/write failed for {0}: {1}" -f $guid, $_.Exception.Message)
      }
    }
  } catch {
    Write-Log ("StopSignal: unexpected failure: {0}" -f $_.Exception.Message)
  }
}

function OnGameStarted() {
  param($evnArgs)
  $game = $evnArgs.Game
  Write-Log "OnGameStarted: $($game.Name) [$($game.Id)]"
  Send-StatusMessage -Name 'gameStarted' -Game $game
}

function OnGameStopped() {
  param($evnArgs)
  $game = $evnArgs.Game
  Write-Log "OnGameStopped: $($game.Name) [$($game.Id)]"
  Send-StatusMessage -Name 'gameStopped' -Game $game
}

function Register-GameCollectionEvents {
  try {
    if (-not $PlayniteApi) { Write-Log "Register-GameCollectionEvents: no PlayniteApi"; return }
    $db = $null
    try { $db = $PlayniteApi.Database } catch {}
    if (-not $db) { Write-Log "Register-GameCollectionEvents: no Database"; return }
    $games = $null
    try { $games = $db.Games } catch {}
    if (-not $games) { Write-Log "Register-GameCollectionEvents: no Games collection"; return }
    if ($script:GameEventsRegistered) { Write-Log "Register-GameCollectionEvents: already registered"; return }
    $modulePath = try { Join-Path $PSScriptRoot 'SunshinePlaynite.psm1' } catch { $null }
    $msg = @{ Module = $modulePath; Outbox = $global:Outbox }
    try {
      $sub1 = Register-ObjectEvent -InputObject $games -EventName 'ItemCollectionChanged' -Action {
        try {
          $ob = $null
          try { $ob = $Event.MessageData.Outbox } catch {}
          if ($ob) { $script:Outbox = $ob; $global:Outbox = $ob }
        } catch {}
        try { Write-Log "ItemCollectionChanged: debounce snapshot"; Kick-SnapshotDebounce } catch {}
      } -MessageData $msg -ErrorAction SilentlyContinue
      if ($sub1) { $script:GameEventSubs += ,$sub1 }
    } catch { Write-Log "Register-GameCollectionEvents: ItemCollectionChanged registration failed: $($_.Exception.Message)" }
    try {
      $sub2 = Register-ObjectEvent -InputObject $games -EventName 'ItemUpdated' -Action {
        try {
          $ob = $null
          try { $ob = $Event.MessageData.Outbox } catch {}
          if ($ob) { $script:Outbox = $ob; $global:Outbox = $ob }
        } catch {}
        try { Write-Log "ItemUpdated: debounce snapshot"; Kick-SnapshotDebounce } catch {}
      } -MessageData $msg -ErrorAction SilentlyContinue
      if ($sub2) { $script:GameEventSubs += ,$sub2 }
    } catch { Write-Log "Register-GameCollectionEvents: ItemUpdated registration failed: $($_.Exception.Message)" }
    $script:GameEventsRegistered = ($script:GameEventSubs.Count -gt 0)
    Write-Log ("Register-GameCollectionEvents: registered={0} subs={1}" -f $script:GameEventsRegistered, $script:GameEventSubs.Count)
  } catch { Write-Log "Register-GameCollectionEvents: error: $($_.Exception.Message)" }
}

function Unregister-GameCollectionEvents {
  try {
    foreach ($sub in @($script:GameEventSubs)) {
      try { if ($sub -and $sub.SubscriptionId) { Unregister-Event -SubscriptionId $sub.SubscriptionId -ErrorAction SilentlyContinue } } catch {}
      try { if ($sub -and $sub.Id) { Remove-Job -Id $sub.Id -Force -ErrorAction SilentlyContinue } } catch {}
    }
    $script:GameEventSubs = @()
    $script:GameEventsRegistered = $false
    Write-Log "Unregister-GameCollectionEvents: done"
  } catch { Write-Log "Unregister-GameCollectionEvents: error: $($_.Exception.Message)" }
}

# Helper: bounded, non-blocking runspace/PS teardown
function Close-RunspaceFast {
  param(
    [Parameter(Mandatory=$true)][hashtable]$Bag,
    [int]$TimeoutMs = 500
  )
  try {
    $ps = $null; $rs = $null
    try { $ps = $Bag.PowerShell } catch {}
    try { $rs = $Bag.Runspace } catch {}
    $done = New-Object System.Threading.ManualResetEventSlim($false)
    $state = [PSCustomObject]@{ PS = $ps; RS = $rs; Done = $done }
    [System.Threading.ThreadPool]::QueueUserWorkItem({ param($s)
      try { if ($s.PS) { $s.PS.Stop() } } catch {}
      try { if ($s.PS) { $s.PS.Dispose() } } catch {}
      try { if ($s.RS) { $s.RS.Close() } } catch {}
      try { if ($s.RS) { $s.RS.Dispose() } } catch {}
      try { $s.Done.Set() } catch {}
    }, $state) | Out-Null
    if (-not $done.Wait($TimeoutMs)) {
      Write-Log "Close-RunspaceFast: timeout after $TimeoutMs ms; abandoning cleanup."
    }
  } catch {
    Write-Log "Close-RunspaceFast: error: $($_.Exception.Message)"
  }
}

# Optional: clean up on exit (cooperative cancellation)
function OnApplicationStopped() {
  try {
    Write-Log "OnApplicationStopped: begin shutdown"
    try { Write-DebugLog ("OnApplicationStopped: connCount(before)={0}" -f ($global:LauncherConns.Count)) } catch {}
    # 1) Signal all loops to exit ASAP
    try { if ($script:Cts) { $script:Cts.Cancel() } } catch {}
    # Actively break the connector runspace's blocking ReadLine()
    try { [ShutdownBridge]::CloseSunStream() } catch {}

    # 2) Proactively close streams to break any ReadLine() blocking
    try {
      if ($global:SunConn -and $global:SunConn.Stream) { $global:SunConn.Stream.Dispose() }
    } catch {}
    $global:SunConn = $null

    # 3) Stop pumps
    try { [OutboxPump]::Stop() } catch {}

    # 4) Tear down per-launcher connections (this also unblocks their readers)
    try {
      foreach ($kv in $global:LauncherConns.GetEnumerator()) {
        $c = $kv.Value
        try { if ($c.Reader)     { $c.Reader.Dispose() } } catch {}
        try { if ($c.Writer)     { $c.Writer.Dispose() } } catch {}
        try { if ($c.Stream)     { $c.Stream.Dispose() } } catch {}
        try { if ($c.Pump)       { $c.Pump.Dispose() } } catch {}
        try { Close-RunspaceFast -Bag $c -TimeoutMs 300 } catch {}
      }
    } catch {}
    try { $global:LauncherConns = New-Object 'System.Collections.Concurrent.ConcurrentDictionary[string,object]' } catch { $global:LauncherConns = @{} }
    try { Write-DebugLog ("OnApplicationStopped: connCount(after)={0}" -f ($global:LauncherConns.Count)) } catch {}

    # 5) Shut down the two background runspaces
    try {
      try { Unregister-GameCollectionEvents } catch {}
      try {
        if ($script:SnapshotTimerSub) {
          try { Unregister-Event -SourceIdentifier $script:SnapshotTimerSub.SourceIdentifier -ErrorAction SilentlyContinue } catch {}
          $script:SnapshotTimerSub = $null
        }
        if ($script:SnapshotTimer) {
          try { $script:SnapshotTimer.Stop() } catch {}
          try { $script:SnapshotTimer.Dispose() } catch {}
          $script:SnapshotTimer = $null
        }
      } catch {}
      if ($script:Bg)  { Close-RunspaceFast -Bag $script:Bg  -TimeoutMs 500; $script:Bg  = $null }
      if ($script:Bg2) { Close-RunspaceFast -Bag $script:Bg2 -TimeoutMs 500; $script:Bg2 = $null }
    } catch {}

    Write-Log "OnApplicationStopped: connector stopped"
  }
  catch {
    try { Write-Log "OnApplicationStopped: error during shutdown: $($_.Exception.Message)" } catch {}
  }
}
