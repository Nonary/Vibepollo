# Steam Big Picture session cleanup

On the Linux machine host, quitting the Steam Big Picture application session
from Moonlight stops Steam games started during that session, then closes Big
Picture. Disconnecting, sleeping the client, or losing the network leaves the
game running for reconnection. Steam itself stays running.

Cleanup sends the identified game processes SIGTERM, waits up to five seconds
in total, then sends SIGKILL to those still running with the same process
identity. Games already running when Big Picture opened are excluded, including
new processes they create later. Exit the game normally first when you need to
save progress.

To leave games running even when quitting the Big Picture session, create
`~/.config/vibepollo/steam-big-picture.json` for the desktop user with:

```json
{"close-games": false}
```

This preference is read when a new Big Picture session opens. Remove the file
or set `close-games` to `true` to restore cleanup.

The existing default application's open and undo commands require no changes.
The broker passes these fixed actions to the unprivileged desktop Steam helper.
It records process IDs and Steam game IDs under the user's runtime directory;
the machine host never reads that state or receives additional capabilities.
The cleanup baseline is consumed once when the undo command runs. Missing,
invalid, or incomplete state leaves games alone and still closes Big Picture.

Identification uses readable `SteamAppId` process environment entries. Games
or launchers that remove that identifier cannot be cleaned up reliably and are
left alone. Any newly started Steam game in the same desktop session qualifies,
whether started from Big Picture or another Steam window. This behavior applies
to the Linux machine-host broker path, not Windows or macOS.
