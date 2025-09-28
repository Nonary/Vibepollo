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
$script:SunshineLaunchedGameIds = $null
$slVar = $null
try { $slVar = Get-Variable -Name 'SunshineLaunchedGameIds' -Scope Global -ErrorAction SilentlyContinue } catch { $slVar = $null }
if (-not $slVar -or -not ($slVar.Value -is [System.Collections.Concurrent.ConcurrentDictionary[string,bool]])) {
  $global:SunshineLaunchedGameIds = New-Object 'System.Collections.Concurrent.ConcurrentDictionary[string,bool]'
}
$script:SunshineLaunchedGameIds = $global:SunshineLaunchedGameIds
$script:PendingLauncherGameIds = $null
$plVar = $null
try { $plVar = Get-Variable -Name 'PendingLauncherGameIds' -Scope Global -ErrorAction SilentlyContinue } catch { $plVar = $null }
if (-not $plVar -or -not ($plVar.Value -is [System.Collections.Concurrent.ConcurrentDictionary[string,bool]])) {
  $global:PendingLauncherGameIds = New-Object 'System.Collections.Concurrent.ConcurrentDictionary[string,bool]'
}
$script:PendingLauncherGameIds = $global:PendingLauncherGameIds
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

# Build a pipe security descriptor that allows Sunshine (SYSTEM) and the current user.
function New-PipeSecurity {
  try {
    $pipeSec = New-Object System.IO.Pipes.PipeSecurity

    $systemSid = New-Object System.Security.Principal.SecurityIdentifier([System.Security.Principal.WellKnownSidType]::LocalSystemSid, $null)
    $interactiveSid = New-Object System.Security.Principal.SecurityIdentifier([System.Security.Principal.WellKnownSidType]::InteractiveSid, $null)
    $worldSid = New-Object System.Security.Principal.SecurityIdentifier([System.Security.Principal.WellKnownSidType]::WorldSid, $null)
    try {
      $userSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
    } catch { $userSid = $null }

    $full = [System.IO.Pipes.PipeAccessRights]::FullControl
    $rw = [System.IO.Pipes.PipeAccessRights]::ReadWrite
    $allow = [System.Security.AccessControl.AccessControlType]::Allow

    $pipeSec.AddAccessRule((New-Object System.IO.Pipes.PipeAccessRule($systemSid, $full, $allow))) | Out-Null
    $pipeSec.AddAccessRule((New-Object System.IO.Pipes.PipeAccessRule($interactiveSid, $rw, $allow))) | Out-Null
    $pipeSec.AddAccessRule((New-Object System.IO.Pipes.PipeAccessRule($worldSid, $rw, $allow))) | Out-Null
    if ($userSid) {
      $pipeSec.AddAccessRule((New-Object System.IO.Pipes.PipeAccessRule($userSid, $full, $allow))) | Out-Null
    }
    return $pipeSec
  } catch {
    Write-Log "PipeSecurity: failed to build descriptor; defaulting" -Level 'WARN'
    return $null
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

function Close-SunshineConnection {
  param([string]$Reason = '')
  try {
    if ($Reason) { Write-Log ("CloseSunConn: reason={0}" -f $Reason) -Level 'DEBUG' } else { Write-Log "CloseSunConn: closing" -Level 'DEBUG' }
  } catch {}
  $conn = $global:SunConn
  $global:SunConn = $null
  try { [OutboxPump]::Stop() } catch {}
  if ($conn) {
    try { if ($conn.Reader) { $conn.Reader.Dispose() } } catch {}
    try { if ($conn.Writer) { $conn.Writer.Dispose() } } catch {}
    try { if ($conn.Stream) { $conn.Stream.Dispose() } } catch {}
  }
  try { [ShutdownBridge]::CloseSunStream() } catch {}
}

function Initialize-SunshineConnection {
  param($Stream, $Reader, $Writer, $Hello)
  try {
    Close-SunshineConnection -Reason 'replace'
    $info = @{
      Stream = $Stream
      Reader = $Reader
      Writer = $Writer
      Hello  = $Hello
    }
    $global:SunConn = $info
    try { [ShutdownBridge]::SetSunStream($Stream) } catch {}
    Write-Log "PipeServer: Sunshine core connection ready"
  }
  catch {
    Write-Log "PipeServer: failed to initialize Sunshine connection: $($_.Exception.Message)"
    try { if ($Reader) { $Reader.Dispose() } } catch {}
    try { if ($Writer) { $Writer.Dispose() } } catch {}
    try { if ($Stream) { $Stream.Dispose() } } catch {}
  }
}

function Initialize-LauncherConnection {
  param([string]$ConnectionId, $Stream, $Reader, $Writer, $Hello)
  if (-not $ConnectionId) { $ConnectionId = ([Guid]::NewGuid().ToString('N')) }
  $connInfo = @{
    Stream = $Stream
    Reader = $Reader
    Writer = $Writer
    Hello  = $Hello
    Guid   = $ConnectionId
    Pid    = $null
    GameId = $null
  }
  try {
    if ($Hello) {
      try { $connInfo.Pid = [int]$Hello.pid } catch {}
      try { $connInfo.GameId = [string]$Hello.gameId } catch {}
    }
  } catch {}
  try {
    $connInfo.Outbox = New-Object 'System.Collections.Concurrent.BlockingCollection[string]'
    if (-not $connInfo.Writer) {
      $connInfo.Writer = New-Object System.IO.StreamWriter($Stream, [System.Text.Encoding]::UTF8, 8192, $true)
      $connInfo.Writer.AutoFlush = $true
    }
    $connInfo.Pump = New-Object PerConnPump($connInfo.Writer, $connInfo.Outbox)
    $connInfo.Pump.Start()
  } catch {
    Write-Log "PipeServer: failed to start launcher outbox pump: $($_.Exception.Message)"
  }
  try {
    if (-not $global:LauncherConns.TryAdd($ConnectionId, $connInfo)) {
      $global:LauncherConns[$ConnectionId] = $connInfo
    }
  } catch {
    Write-Log "PipeServer: failed to register launcher connection: $($_.Exception.Message)"
  }
  try {
    $modulePath = try { Join-Path $PSScriptRoot 'SunshinePlaynite.psm1' } catch { $null }
    $rs = [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
    $rs.ApartmentState = [System.Threading.ApartmentState]::MTA
    $rs.ThreadOptions = [System.Management.Automation.Runspaces.PSThreadOptions]::UseNewThread
    $rs.Open()
    if ($PlayniteApi) { $rs.SessionStateProxy.SetVariable('PlayniteApi', $PlayniteApi) }
    if ($Outbox) { $rs.SessionStateProxy.SetVariable('Outbox', $Outbox) }
    try { $rs.SessionStateProxy.SetVariable('LauncherConns', $global:LauncherConns) } catch {}
    try { if ($global:SunshineLaunchedGameIds) { $rs.SessionStateProxy.SetVariable('SunshineLaunchedGameIds', $global:SunshineLaunchedGameIds) } } catch {}
    try { if ($global:PendingLauncherGameIds) { $rs.SessionStateProxy.SetVariable('PendingLauncherGameIds', $global:PendingLauncherGameIds) } } catch {}
    if ($PSScriptRoot) { $rs.SessionStateProxy.SetVariable('PSScriptRoot', $PSScriptRoot) }
    $rs.SessionStateProxy.SetVariable('Cts', $script:Cts)
    $rs.SessionStateProxy.SetVariable('ConnGuid', $ConnectionId)
    $rs.SessionStateProxy.SetVariable('Conn', $connInfo)
    $ps = [System.Management.Automation.PowerShell]::Create()
    $ps.Runspace = $rs
    if ($modulePath) { $ps.AddScript("Import-Module -Force '$modulePath'") | Out-Null }
    $ps.AddScript('$global:LauncherConns = $LauncherConns') | Out-Null
    $ps.AddScript('$global:SunshineLaunchedGameIds = $SunshineLaunchedGameIds') | Out-Null
    $ps.AddScript('$global:PendingLauncherGameIds = $PendingLauncherGameIds') | Out-Null
    $ps.AddScript('Start-LauncherConnReader -Guid $ConnGuid -Conn $Conn -Token $Cts.Token') | Out-Null
    $handle = $ps.BeginInvoke()
    $connInfo.Runspace = $rs
    $connInfo.PowerShell = $ps
    $connInfo.Handle = $handle
  } catch {
    Write-Log "PipeServer: failed to start launcher reader: $($_.Exception.Message)"
  }
  try { Sync-LauncherConnectionActiveGame -Guid $ConnectionId -Conn $connInfo } catch { Write-Log ("PipeServer: sync failed for {0}: {1}" -f $ConnectionId, $_.Exception.Message) }
  try { Flush-PendingLauncherStatuses -Targets @($ConnectionId) } catch { Write-Log ("PipeServer: pending flush failed for {0}: {1}" -f $ConnectionId, $_.Exception.Message) }
  try {
    Write-Log ("PipeServer: launcher connection registered id={0} pid={1}" -f $ConnectionId, $connInfo.Pid)
  } catch {}
  return $ConnectionId
}

function Wait-ForPipeAck {
  param($Control)
  $ack = New-Object byte[] 1
  $read = $Control.Read($ack, 0, 1)
  if ($read -ne 1 -or $ack[0] -ne 0x02) {
    throw "Handshake ACK missing"
  }
}

function Write-HandShakeMessage {
  param($Control, [string]$PipeName)
  $chars = New-Object char[] 40
  $nameChars = ($PipeName + [char]0).ToCharArray()
  [System.Array]::Copy($nameChars, 0, $chars, 0, [Math]::Min($nameChars.Length, $chars.Length))
  $buffer = New-Object byte[] (80)
  [System.Text.Encoding]::Unicode.GetBytes($chars, 0, $chars.Length, $buffer, 0) | Out-Null
  $Control.Write($buffer, 0, $buffer.Length)
  $Control.Flush()
}

function Start-PipeServerLoop {
  param([System.Threading.CancellationToken]$Token = $script:Cts.Token)
  try { if (-not $script:LogPath) { Initialize-Logging } } catch {}
  Write-Log "PipeServer: starting"
  $pipeSecurity = New-PipeSecurity
  while (-not ($Token -and $Token.IsCancellationRequested)) {
    $control = $null
    $data = $null
    try {
      $control = if ($pipeSecurity) {
        New-Object System.IO.Pipes.NamedPipeServerStream('Sunshine.PlayniteExtension', [System.IO.Pipes.PipeDirection]::InOut, 1, [System.IO.Pipes.PipeTransmissionMode]::Byte, [System.IO.Pipes.PipeOptions]::Asynchronous, 65536, 65536, $pipeSecurity)
      } else {
        New-Object System.IO.Pipes.NamedPipeServerStream('Sunshine.PlayniteExtension', [System.IO.Pipes.PipeDirection]::InOut, 1, [System.IO.Pipes.PipeTransmissionMode]::Byte, [System.IO.Pipes.PipeOptions]::Asynchronous, 65536, 65536)
      }
      if ($Token -and $Token.IsCancellationRequested) { break }
      $waitTask = $control.WaitForConnectionAsync()
      while (-not $waitTask.Wait(200)) {
        if ($Token -and $Token.IsCancellationRequested) { throw [System.OperationCanceledException]::new() }
      }
      if (-not $control.IsConnected) { continue }
      $pipeName = ([Guid]::NewGuid().ToString('B'))
      $data = if ($pipeSecurity) {
        New-Object System.IO.Pipes.NamedPipeServerStream($pipeName, [System.IO.Pipes.PipeDirection]::InOut, 1, [System.IO.Pipes.PipeTransmissionMode]::Byte, [System.IO.Pipes.PipeOptions]::Asynchronous, 65536, 65536, $pipeSecurity)
      } else {
        New-Object System.IO.Pipes.NamedPipeServerStream($pipeName, [System.IO.Pipes.PipeDirection]::InOut, 1, [System.IO.Pipes.PipeTransmissionMode]::Byte, [System.IO.Pipes.PipeOptions]::Asynchronous, 65536, 65536)
      }
      Write-HandShakeMessage -Control $control -PipeName $pipeName
      Wait-ForPipeAck -Control $control
      try { $control.Disconnect() } catch {}
      try { $control.Dispose() } catch {}
      $control = $null

      $waitData = $data.WaitForConnectionAsync()
      while (-not $waitData.Wait(200)) {
        if ($Token -and $Token.IsCancellationRequested) { throw [System.OperationCanceledException]::new() }
      }
      if (-not $data.IsConnected) { throw "Data pipe failed to connect" }

      $reader = New-Object System.IO.StreamReader($data, [System.Text.Encoding]::UTF8, $false, 8192, $true)
      $writer = New-Object System.IO.StreamWriter($data, [System.Text.Encoding]::UTF8, 8192, $true)
      $writer.AutoFlush = $true

      $helloLine = $null
      try { $helloLine = $reader.ReadLine() } catch {}
      if ($null -eq $helloLine) {
        throw "No hello received"
      }
      $hello = $null
      try { $hello = $helloLine | ConvertFrom-Json -ErrorAction Stop } catch { $hello = $null }
      $role = ''
      try { if ($hello -and $hello.role) { $role = [string]$hello.role } } catch { $role = '' }
      if (-not $role) {
        if (-not $global:SunConn) { $role = 'sunshine' } else { $role = 'launcher' }
      }
      if ($role -eq 'sunshine') {
        Initialize-SunshineConnection -Stream $data -Reader $reader -Writer $writer -Hello $hello
        Write-Log "PipeServer: Sunshine connection accepted" -Level 'DEBUG'
        $data = $null
      } else {
        $connId = Initialize-LauncherConnection -ConnectionId $pipeName -Stream $data -Reader $reader -Writer $writer -Hello $hello
        Write-Log ("PipeServer: launcher connection accepted id={0}" -f $connId) -Level 'DEBUG'
        $data = $null
      }
    }
    catch [System.OperationCanceledException] {
      break
    }
    catch {
      Write-Log "PipeServer: connection failed: $($_.Exception.Message)"
      if ($data) {
        try { $data.Dispose() } catch {}
      }
    }
    finally {
      if ($control) { try { $control.Dispose() } catch {} }
      if ($data) { try { $data.Dispose() } catch {} }
    }
  }
  Write-Log "PipeServer: exiting"
}

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
          Register-SunshineLaunchedGame -Id $obj.id
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
    $conn = $global:SunConn
    if (-not $conn -or -not $conn.Stream -or -not $conn.Stream.CanRead) {
      if ($Token -and $Token.IsCancellationRequested) { break }
      Start-Sleep -Milliseconds 300
      continue
    }
    Write-Log "Connector loop: activating Sunshine connection"
    try { [OutboxPump]::Start($conn.Writer, $global:Outbox); Write-DebugLog "Connector loop: OutboxPump started" } catch { Write-Log "Failed to start OutboxPump: $($_.Exception.Message)" }
    Send-InitialSnapshot
    $active = $true
    while (-not ($Token -and $Token.IsCancellationRequested) -and $active) {
      $line = $null
      try { $line = $conn.Reader.ReadLine() }
      catch {
        if ($Token -and $Token.IsCancellationRequested) {
          $active = $false
          break
        }
        Write-Log "Reader exception: $($_.Exception.Message)"
        $active = $false
        break
      }
      if ($null -eq $line) {
        Write-Log "Reader: EOF from Sunshine core"
        $active = $false
        break
      }
      try {
        $obj = $line | ConvertFrom-Json -ErrorAction Stop
        if ($obj.type -eq 'command' -and $obj.command -eq 'launch' -and $obj.id) {
          try {
            Register-SunshineLaunchedGame -Id $obj.id
            [UIBridge]::StartGameByGuidStringOnUIThread([string]$obj.id)
            Write-Log "Dispatched launch to UI thread via UIBridge.StartGameByGuidStringOnUIThread"
          } catch { Write-Log "Failed to dispatch launch: $($_.Exception.Message)" }
        } elseif ($obj.type -eq 'command' -and $obj.command -eq 'stop') {
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
    if ($global:SunConn -eq $conn) {
      Close-SunshineConnection -Reason 'connector-loop'
    }
  }
  Write-Log "Connector loop: exiting (stopping=$([bool]($Token -and $Token.IsCancellationRequested)))"
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
      try { if ($global:SunshineLaunchedGameIds) { $rs.SessionStateProxy.SetVariable('SunshineLaunchedGameIds', $global:SunshineLaunchedGameIds) } } catch {}
      try { if ($global:PendingLauncherGameIds) { $rs.SessionStateProxy.SetVariable('PendingLauncherGameIds', $global:PendingLauncherGameIds) } } catch {}
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
      $ps.AddScript('$global:SunshineLaunchedGameIds = $SunshineLaunchedGameIds') | Out-Null
      $ps.AddScript('$global:PendingLauncherGameIds = $PendingLauncherGameIds') | Out-Null
      $ps.AddScript('Start-ConnectorLoop -Token $Cts.Token') | Out-Null
      $handle = $ps.BeginInvoke()
      $script:Bg = @{ Runspace = $rs; PowerShell = $ps; Handle = $handle }
      Write-Log "OnApplicationStarted: background runspace started"
      try { Write-DebugLog ("OnApplicationStarted: RS1={0}" -f $rs.InstanceId) } catch {}

      # Start pipe server in a separate runspace
      $rs2 = [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
      $rs2.ApartmentState = [System.Threading.ApartmentState]::MTA
      $rs2.ThreadOptions = [System.Management.Automation.Runspaces.PSThreadOptions]::UseNewThread
      $rs2.Open()
      if ($PlayniteApi) { $rs2.SessionStateProxy.SetVariable('PlayniteApi', $PlayniteApi) }
      if ($Outbox) { $rs2.SessionStateProxy.SetVariable('Outbox', $Outbox) }
      # Inject the existing shared LauncherConns object so this runspace uses the same instance
      try { $rs2.SessionStateProxy.SetVariable('LauncherConns', $global:LauncherConns) } catch {}
      try { if ($global:SunshineLaunchedGameIds) { $rs2.SessionStateProxy.SetVariable('SunshineLaunchedGameIds', $global:SunshineLaunchedGameIds) } } catch {}
      try { if ($global:PendingLauncherGameIds) { $rs2.SessionStateProxy.SetVariable('PendingLauncherGameIds', $global:PendingLauncherGameIds) } } catch {}
      if ($PSScriptRoot) { $rs2.SessionStateProxy.SetVariable('PSScriptRoot', $PSScriptRoot) }
      $rs2.SessionStateProxy.SetVariable('Cts', $script:Cts)
      $ps2 = [System.Management.Automation.PowerShell]::Create()
      $ps2.Runspace = $rs2
      if ($modulePath) { $ps2.AddScript("Import-Module -Force '$modulePath'") | Out-Null }
      # After module import (which initializes its own table), rebind to the injected shared instance
      $ps2.AddScript('$global:LauncherConns = $LauncherConns') | Out-Null
      $ps2.AddScript('$global:SunshineLaunchedGameIds = $SunshineLaunchedGameIds') | Out-Null
      $ps2.AddScript('$global:PendingLauncherGameIds = $PendingLauncherGameIds') | Out-Null
      $ps2.AddScript('Start-PipeServerLoop -Token $Cts.Token') | Out-Null
      $handle2 = $ps2.BeginInvoke()
      $script:Bg2 = @{ Runspace = $rs2; PowerShell = $ps2; Handle = $handle2 }
      Write-Log "OnApplicationStarted: pipe server runspace started"
      try { Write-DebugLog ("OnApplicationStarted: RS2={0}" -f $rs2.InstanceId) } catch {}
      try { Register-GameCollectionEvents } catch { Write-Log "OnApplicationStarted: event registration failed: $($_.Exception.Message)" }
      try { Ensure-SnapshotDebounceTimer } catch {}
      try { Send-RunningGamesStatusSnapshot } catch { Write-Log ("OnApplicationStarted: running snapshot failed: {0}" -f $_.Exception.Message) }
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

function Build-StatusPayload {
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
  $status = @{ name = $Name; id = $Game.Id.ToString(); installDir = $instDir; exe = (Get-GameActionInfo -Game $Game).exe }
  return @{ type = 'status'; status = $status } | ConvertTo-Json -Depth 5 -Compress
}

function Send-PayloadToLauncherConnections {
  param(
    [Parameter(Mandatory)][string]$Payload,
    [string[]]$Targets,
    [string]$Context = 'Status broadcast',
    [switch]$ReturnCount
  )
  try {
    $keys = @()
    if ($Targets -and $Targets.Count -gt 0) {
      foreach ($t in $Targets) { if ($t) { $keys += $t } }
    } else {
      try { $keys = @($global:LauncherConns.Keys) } catch { $keys = @() }
    }
    $count = 0
    try { $count = $keys.Count } catch {}
    Write-Log ("{0}: enqueue to {1} launcher connection(s)" -f $Context, $count)
    $success = 0
    foreach ($guid in $keys) {
      $conn = $null
      try { $conn = $global:LauncherConns[$guid] } catch {}
      if (-not $conn) { continue }
      try {
        if ($conn.Outbox) {
          $null = $conn.Outbox.Add($Payload)
          $success++
          Write-Log ("{0}: queued for {1}" -f $Context, $guid)
        }
        elseif ($conn.Writer) {
          $conn.Writer.WriteLine($Payload)
          $conn.Writer.Flush()
          $success++
          Write-Log ("{0}: wrote directly for {1}" -f $Context, $guid)
        }
        else {
          Write-Log ("{0}: missing conn/outbox for {1}" -f $Context, $guid)
        }
      } catch {
        Write-Log ("{0}: enqueue failed for {1}: {2}" -f $Context, $guid, $_.Exception.Message)
      }
    }
    if ($ReturnCount) { return $success }
  }
  catch { Write-Log ("{0}: unexpected failure: {1}" -f $Context, $_.Exception.Message) }
}

function Send-StatusMessage {
  param([string]$Name, [object]$Game, [switch]$ReturnLauncherCount)
  $payload = $null
  try { $payload = Build-StatusPayload -Name $Name -Game $Game }
  catch {
    Write-Log ("Status build failed: {0}" -f $_.Exception.Message)
    return
  }
  Send-JsonMessage -Json $payload
  if ($ReturnLauncherCount) {
    return (Send-PayloadToLauncherConnections -Payload $payload -ReturnCount)
  } else {
    Send-PayloadToLauncherConnections -Payload $payload | Out-Null
  }
}

function Get-PlayniteGameById {
  param([object]$Id)
  try {
    $norm = Normalize-GameId -Id $Id
    if (-not $norm) { return $null }
    if (-not $PlayniteApi) { return $null }
    $db = $null
    try { $db = $PlayniteApi.Database } catch {}
    if (-not $db) { return $null }
    $games = $null
    try { $games = $db.Games } catch {}
    if (-not $games) { return $null }
    try {
      $guid = [guid]$norm
      try {
        $found = $games.Get($guid)
        if ($found) { return $found }
      } catch {}
    } catch {}
    foreach ($g in $games) {
      try {
        $gid = $g.Id
        if ($gid -and ((Normalize-GameId -Id $gid) -eq $norm)) { return $g }
      } catch {}
    }
  } catch {}
  return $null
}

function Test-GameIsRunning {
  param([object]$Game)
  try {
    if (-not $Game) { return $false }
    try {
      $prop = $Game.PSObject.Properties['IsRunning']
      if ($prop -and ($prop.Value -ne $null)) { return [bool]$prop.Value }
    } catch {}
    try {
      $type = $Game.GetType()
      if ($type) {
        $p = $type.GetProperty('IsRunning')
        if ($p) {
          $val = $p.GetValue($Game, $null)
          if ($val -ne $null) { return [bool]$val }
        }
      }
    } catch {}
    try {
      $stateProp = $Game.PSObject.Properties['PlayState']
      if ($stateProp -and $stateProp.Value) {
        $state = $stateProp.Value.ToString()
        if ($state -and ($state -eq 'Playing')) { return $true }
      }
    } catch {}
  } catch {}
  return $false
}

function Get-RunningGames {
  $result = @()
  try {
    if (-not $PlayniteApi) { return $result }
    $db = $null
    try { $db = $PlayniteApi.Database } catch {}
    if (-not $db) { return $result }
    $games = $null
    try { $games = $db.Games } catch {}
    if (-not $games) { return $result }
    foreach ($g in $games) {
      try {
        if (Test-GameIsRunning -Game $g) { $result += $g }
      } catch {}
    }
  } catch { Write-Log ("RunningGames: enumeration failed: {0}" -f $_.Exception.Message) }
  return $result
}

function Send-RunningGamesStatusSnapshot {
  try {
    $running = Get-RunningGames
    if (-not $running -or $running.Count -eq 0) { return }
    foreach ($game in $running) {
      if (-not $game) { continue }
      $gameId = $null
      try { $gameId = $game.Id } catch {}
      if (-not $gameId) { continue }
      if (-not (Test-SunshineLaunchedGame -Id $gameId)) {
        $queued = 0
        try { $queued = Send-StatusMessage -Name 'gameStarted' -Game $game -ReturnLauncherCount }
        catch {
          Write-Log ("RunningSnapshot: send failed for {0}: {1}" -f (Normalize-GameId -Id $gameId), $_.Exception.Message)
          $queued = 0
        }
        if ($queued -gt 0) {
          try { Remove-PendingLauncherGame -Id $gameId } catch {}
          Write-Log ("RunningSnapshot: delivered gameStarted for {0} (connections={1})" -f (Normalize-GameId -Id $gameId), $queued) -Level 'DEBUG'
        }
        else {
          try { Add-PendingLauncherGame -Id $gameId } catch {}
          Write-Log ("RunningSnapshot: pending delivery for {0}" -f (Normalize-GameId -Id $gameId)) -Level 'DEBUG'
        }
        try { Register-SunshineLaunchedGame -Id $gameId } catch {}
      }
    }
  } catch { Write-Log ("RunningSnapshot: error: {0}" -f $_.Exception.Message) }
}

function Sync-LauncherConnectionActiveGame {
  param([string]$Guid, [hashtable]$Conn)
  try {
    if (-not $Guid) { return }
    $targets = @($Guid)
    $candidates = @()
    $targetId = ''
    try { $targetId = [string]$Conn.GameId } catch {}
    if ($targetId) {
      $game = Get-PlayniteGameById -Id $targetId
      if ($game -and (Test-GameIsRunning -Game $game)) { $candidates = @($game) }
    }
    if (-not $candidates -or $candidates.Count -eq 0) {
      $candidates = Get-RunningGames
    }
    foreach ($game in $candidates) {
      if (-not $game) { continue }
      $payload = $null
      try { $payload = Build-StatusPayload -Name 'gameStarted' -Game $game } catch { continue }
      $gid = ''
      try { $gid = [string]$game.Id } catch {}
      if ($gid -and (-not (Test-SunshineLaunchedGame -Id $gid))) {
        Register-SunshineLaunchedGame -Id $gid
      }
      $queued = 0
      try {
        $queued = Send-PayloadToLauncherConnections -Payload $payload -Targets $targets -Context ("Connection sync {0}" -f $Guid) -ReturnCount
      } catch { $queued = 0 }
      if ($queued -gt 0) {
        try { Remove-PendingLauncherGame -Id $gid } catch {}
        Write-Log ("Connection sync {0}: delivered gameStarted for {1}" -f $Guid, (Normalize-GameId -Id $gid)) -Level 'DEBUG'
      } else {
        try { Add-PendingLauncherGame -Id $gid } catch {}
        Write-Log ("Connection sync {0}: pending delivery for {1}" -f $Guid, (Normalize-GameId -Id $gid)) -Level 'DEBUG'
      }
    }
  } catch { Write-Log ("Connection sync {0}: error: {1}" -f $Guid, $_.Exception.Message) }
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

function Normalize-GameId {
  param([object]$Id)
  try {
    if ($null -eq $Id) { return '' }
    $str = [string]$Id
    if ([string]::IsNullOrWhiteSpace($str)) { return '' }
    $trimmed = $str.Trim().Trim('{','}')
    if ([string]::IsNullOrWhiteSpace($trimmed)) { return '' }
    return $trimmed.ToLowerInvariant()
  } catch { return '' }
}

function Add-PendingLauncherGame {
  param([object]$Id)
  try {
    $norm = Normalize-GameId -Id $Id
    if (-not $norm) { return }
    if (-not $script:PendingLauncherGameIds) { return }
    $script:PendingLauncherGameIds[$norm] = $true
    Write-DebugLog ("PendingLaunch: queued {0}" -f $norm)
  } catch { Write-Log "PendingLaunch: add failed: $($_.Exception.Message)" }
}

function Remove-PendingLauncherGame {
  param([object]$Id)
  try {
    $norm = Normalize-GameId -Id $Id
    if (-not $norm) { return $false }
    if (-not $script:PendingLauncherGameIds) { return $false }
    $dummy = $false
    if ($script:PendingLauncherGameIds.TryRemove($norm, [ref]$dummy)) {
      Write-DebugLog ("PendingLaunch: removed {0}" -f $norm)
      return $true
    }
    return $false
  } catch {
    Write-Log "PendingLaunch: remove failed: $($_.Exception.Message)"
    return $false
  }
}

function Get-PendingLauncherGameIds {
  try {
    if (-not $script:PendingLauncherGameIds) { return @() }
    return @($script:PendingLauncherGameIds.Keys)
  } catch {
    Write-Log "PendingLaunch: enumerate failed: $($_.Exception.Message)"
    return @()
  }
}

function Flush-PendingLauncherStatuses {
  param([string[]]$Targets)
  $ids = Get-PendingLauncherGameIds
  if (-not $ids -or $ids.Count -eq 0) { return 0 }
  $sent = 0
  foreach ($id in $ids) {
    $game = $null
    try { $game = Get-PlayniteGameById -Id $id } catch {}
    if (-not $game) {
      try { Remove-PendingLauncherGame -Id $id } catch {}
      continue
    }
    $payload = $null
    try { $payload = Build-StatusPayload -Name 'gameStarted' -Game $game } catch {}
    if (-not $payload) { continue }
    $queued = 0
    try {
      $queued = Send-PayloadToLauncherConnections -Payload $payload -Targets $Targets -Context ("Pending flush {0}" -f $id) -ReturnCount
    } catch { $queued = 0 }
    if ($queued -gt 0) {
      $sent += $queued
      try { Register-SunshineLaunchedGame -Id $id } catch {}
      try { Remove-PendingLauncherGame -Id $id } catch {}
    }
  }
  return $sent
}

function Register-SunshineLaunchedGame {
  param([object]$Id)
  try {
    $norm = Normalize-GameId -Id $Id
    if (-not $norm) { return }
    if (-not $script:SunshineLaunchedGameIds) { return }
    $script:SunshineLaunchedGameIds[$norm] = $true
    $count = 0
    try { $count = $script:SunshineLaunchedGameIds.Count } catch {}
    Write-Log ("LaunchTrack: registered {0} (count={1})" -f $norm, $count) -Level 'DEBUG'
  } catch { Write-Log "LaunchTrack: register failed: $($_.Exception.Message)" }
}

function Test-SunshineLaunchedGame {
  param([object]$Id)
  try {
    $norm = Normalize-GameId -Id $Id
    if (-not $norm) { return $false }
    if (-not $script:SunshineLaunchedGameIds) { return $false }
    return $script:SunshineLaunchedGameIds.ContainsKey($norm)
  } catch {
    Write-Log "LaunchTrack: test failed: $($_.Exception.Message)"
    return $false
  }
}

function Remove-SunshineLaunchedGame {
  param([object]$Id)
  try {
    $norm = Normalize-GameId -Id $Id
    if (-not $norm) { return $false }
    if (-not $script:SunshineLaunchedGameIds) { return $false }
    $removed = $false
    if ($script:SunshineLaunchedGameIds.TryRemove($norm, [ref]$removed)) {
      $count = 0
      try { $count = $script:SunshineLaunchedGameIds.Count } catch {}
      Write-Log ("LaunchTrack: removed {0} (count={1})" -f $norm, $count) -Level 'DEBUG'
      return $true
    }
    Write-Log ("LaunchTrack: remove miss for {0}" -f $norm) -Level 'DEBUG'
    return $false
  } catch {
    Write-Log "LaunchTrack: remove failed: $($_.Exception.Message)"
    return $false
  }
}

function Test-LauncherConnectionForGame {
  param([object]$Id)
  try {
    $norm = Normalize-GameId -Id $Id
    if (-not $norm) { return $false }
    $keys = @()
    try { $keys = @($global:LauncherConns.Keys) } catch { $keys = @() }
    foreach ($guid in $keys) {
      $conn = $null
      try { $conn = $global:LauncherConns[$guid] } catch {}
      if (-not $conn) { continue }
      $cid = $null
      try { $cid = $conn.GameId } catch {}
      if (-not $cid) { continue }
      $cNorm = Normalize-GameId -Id $cid
      if ($cNorm -and ($cNorm -eq $norm)) { return $true }
    }
    return $false
  } catch {
    Write-Log "LaunchTrack: connection probe failed: $($_.Exception.Message)"
    return $false
  }
}

function OnGameStarted() {
  param($evnArgs)
  $game = $evnArgs.Game
  Write-Log "OnGameStarted: $($game.Name) [$($game.Id)]"
  $gameId = $null
  try { $gameId = $game.Id } catch {}
  if (-not (Test-SunshineLaunchedGame -Id $gameId)) {
    if (Test-LauncherConnectionForGame -Id $gameId) {
      Register-SunshineLaunchedGame -Id $gameId
      Write-Log "OnGameStarted: registered via launcher connection" -Level 'DEBUG'
    }
  }
  if (Test-SunshineLaunchedGame -Id $gameId) {
    Write-Log "OnGameStarted: sunshine-owned session" -Level 'DEBUG'
  }
  Send-StatusMessage -Name 'gameStarted' -Game $game
}

function OnGameStopped() {
  param($evnArgs)
  $game = $evnArgs.Game
  Write-Log "OnGameStopped: $($game.Name) [$($game.Id)]"
  $gameId = $null
  try { $gameId = $game.Id } catch {}
  if (-not (Remove-SunshineLaunchedGame -Id $gameId)) {
    $norm = Normalize-GameId -Id $gameId
    if (-not $norm) { $norm = 'untracked' }
    Write-DebugLog ("OnGameStopped: skipping status for {0}" -f $norm)
    return
  }
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
