# Lyra on Edgegap, with Epic Online Services

Host Epic's [Lyra Starter Game](https://dev.epicgames.com/documentation/en-us/unreal-engine/lyra-sample-game-in-unreal-engine) (Unreal Engine 5.8) on [Edgegap](https://edgegap.com) dedicated servers, with sessions and Epic account login on Epic Online Services (EOS).

- **Start a Game** (Online) no longer makes the player a listen server. It asks the Edgegap matchmaker for a server near the players, Edgegap starts a dedicated server, and the player joins it.
- **Quick Play** joins a running Edgegap match found through EOS, or matchmakes a new one.
- Every Edgegap server registers an **EOS session**, so Lyra's **Browse** screen lists it and other players join through EOS.
- Players sign in with their **Epic account**: EOS friends, presence and the overlay keep working.
- A server stops its Edgegap deployment once its match is empty, so idle servers don't bill.

This repository holds only Edgegap's part: a plugin, config blocks, a Dockerfile and a matchmaker config, which you add to your own copy of Lyra. Lyra's code and content come from Epic (Fab or Epic's GitHub) under the Unreal Engine EULA and are not redistributed here.

Verified on Edgegap with UE 5.8.4 (2026-11-02):
- Epic login, then Start a Game → matchmaker → dedicated server → joined with the player's EOS identity.
- Two clients queued at the same time and matched onto one server.
- The server created its EOS session with Edgegap's external port.
- The server stopped its deployment after the players left.

Not verified yet: Browse and Quick Play finding a running match from a second Epic account.

## Contents

| Path | What it is |
|---|---|
| `Plugins/EdgegapIntegration/` | The plugin, C++ only, no content assets. |
| `Plugins/EdgegapSDKBridge/` | Optional, off by default: matchmaking through the [Edgegap SDK for Unreal Engine](https://github.com/edgegap/sdk-unreal-engine). See [Optional: the Edgegap SDK](#optional-the-edgegap-sdk). |
| `Config/DefaultEngine.Edgegap.ini` | Append to your `Config/DefaultEngine.ini`: server target, EOS (always on), matchmaker settings, playlist → profile map. |
| `Config/DefaultGame.Edgegap.ini` | Append to your `Config/DefaultGame.ini`: packages `Config/UserEngine.ini`. |
| `Config/UserEngine.ini.example` | Template for your local secrets. |
| `Dockerfile`, `.dockerignore` | Runtime image for the packaged Linux server. |
| `matchmaker-config.json` | Edgegap matchmaker with profiles `controlpoint`, `elimination`, `topdown`. |

### How the plugin works

- **`UEdgegapSessionSubsystem`** replaces Lyra's `UCommonSessionSubsystem` (CommonUser only creates the base class when no subclass exists). Online `HostSession` measures latency to Edgegap's beacons, sends a matchmaker ticket for the playlist's profile, polls it, and travels to the assigned server. Browse, Join, invites and offline or LAN hosting stay CommonUser's. Lobbies and reservation beacons are off, because Edgegap servers are dedicated servers advertised as EOS sessions. A small overlay shows progress with a Cancel button.
- **`UEdgegapLyraServerSubsystem`** (dedicated servers only):
  - hosts the playlist mapped to the matchmaker profile (`MM_MATCH_PROFILE` → `-Playlist=`, read by Lyra's `ALyraGameMode::HostDedicatedServerMatch`);
  - sets the EOS session's port to `ARBITRIUM_PORT_GAMEPORT_EXTERNAL` (EOS fills in the public IP, but would advertise the internal 7777);
  - on matchmaker deployments, stops the deployment 15 s after the last player leaves, or after 5 min without players.
- **`EdgegapEOSServerConfig`** loads at `EarliestPossible`, before the online subsystem. On dedicated servers it turns off Epic account login and swaps in the server's own EOS client from `EOS_SERVER_CLIENT_ID` and `EOS_SERVER_CLIENT_SECRET`.

## Setup

### 1. Lyra and tools

- Unreal Engine **5.8** from the Epic Games Launcher.
- Lyra Starter Game for 5.8: in the launcher, add it from [Fab](https://www.fab.com/listings/93faede1-4434-47c0-85f1-bf27c0820ad0) and **Create Project** (its project name is `LyraStarterGame`). Keep the project outside OneDrive and other synced folders.
- Docker Desktop. The launcher engine can't build Server targets, so the Linux server is built in Epic's engine image `ghcr.io/epicgames/unreal-engine:dev-slim-5.8.4` (about 80 GB). It needs a GitHub account [linked to your Epic account](https://www.unrealengine.com/en-US/ue-on-github) and a token with `read:packages` (`gh auth refresh -s read:packages`, then `gh auth token | docker login ghcr.io -u <github user> --password-stdin`).

### 2. Add this repository to Lyra

1. Copy `Plugins/EdgegapIntegration` into your project's `Plugins` folder.
2. In your `.uproject`, add to `Plugins`:
   ```json
   { "Name": "OnlineSubsystemEOS", "Enabled": true },
   { "Name": "EdgegapIntegration", "Enabled": true }
   ```
3. Append `Config/DefaultEngine.Edgegap.ini` to `Config/DefaultEngine.ini`, and `Config/DefaultGame.Edgegap.ini` to `Config/DefaultGame.ini` (set `<Project>`).
4. In `Source/LyraServer.Target.cs`, make `bUseChecksInShipping = true;` conditional. Installed engines (the launcher, Epic's container images) refuse targets that change it:
   ```csharp
   if (BuildEnvironment == TargetBuildEnvironment.Unique)
   {
       bUseChecksInShipping = true;
   }
   ```
5. If you build the server with clang (the container does), make sure your Lyra copy matches the engine version. The Fab copy for 5.8 has two CommonUser files (`CommonUserBasicPresence.cpp`, `CommonSessionSubsystem.cpp`) with unreachable code that clang rejects as errors. Take them from Epic's GitHub `5.8.4-release` tag, or delete the unreachable `break;` lines and the loop that returns in its first iteration.
6. Add `Config/User*.ini` to your `.gitignore`.

### 3. EOS (developer portal)

In a product of your organization:
1. **Client policies:** a *Peer2Peer* policy for players. For the server, a *Custom* policy with **Sessions** enabled (tick each action) and **User required** off. The *TrustedServer* template doesn't save in the portal.
2. **Clients:** one per policy, e.g. *Lyra Client* and *Lyra Server*.
3. **Epic Account Services:** accept the terms, create an application, enable *Online Presence* and *Friends*, and link *Lyra Client*. Brand settings are only needed before release.
4. Put the product, sandbox and deployment IDs and the *Lyra Client* ID in the `LyraEdgegap` artifact in `Config/DefaultEngine.ini`.

### 4. Local secrets

Copy `Config/UserEngine.ini.example` to `Config/UserEngine.ini` and fill in the *Lyra Client* secret, a 64-hex-character encryption key, and the matchmaker URL and token (step 6). The matchmaker token is safe in game clients: it only creates tickets. The *Lyra Server* secret never goes in a file: it lives on the Edgegap app version.

### 5. Build the Linux server

```bash
docker volume create lyra-work
# Copy the project, with its content, into the volume (no Binaries/Intermediate/Saved/DerivedDataCache), owned by ue4.
docker run --rm -u root -v lyra-work:/work -v "<project dir>:/src:ro" ghcr.io/epicgames/unreal-engine:dev-slim-5.8.4 \
  bash -c 'mkdir -p /work/Lyra && cd /src && tar --exclude=Binaries --exclude=Intermediate --exclude=Saved --exclude=DerivedDataCache -cf - . | tar -xf - -C /work/Lyra && chown -R ue4:ue4 /work'

# Build, cook and package. The first run takes about 40 minutes.
docker run --rm -v lyra-work:/work -v lyra-ddc:/home/ue4/.local/share/UnrealEngine -v lyra-zen:/home/ue4/.config/Epic \
  ghcr.io/epicgames/unreal-engine:dev-slim-5.8.4 \
  /home/ue4/UnrealEngine/Engine/Build/BatchFiles/RunUAT.sh BuildCookRun -project=/work/Lyra/<Project>.uproject \
  -noP4 -utf8output -unattended -nodebuginfo -server -noclient -platform=Linux -serverplatform=Linux \
  -serverconfig=Development -build -cook -stage -pak -archive -archivedirectory=/work/Packaged -UbtArgs="-NoUBA"

# Copy the result next to the Dockerfile, then build and push the image.
docker run --rm -u root -v lyra-work:/work -v "$PWD/Packaged:/out" ghcr.io/epicgames/unreal-engine:dev-slim-5.8.4 cp -r /work/Packaged/LinuxServer /out/
docker build --platform linux/amd64 --build-arg PROJECT=<Project> -t registry.edgegap.com/<registry project>/lyra:<YYYYMMDD-HHMM> .
docker push registry.edgegap.com/<registry project>/lyra:<YYYYMMDD-HHMM>
```

Keep the `lyra-ddc` and `lyra-zen` volumes between builds: UE 5.8 keeps cook results in Zen under `~/.config/Epic`.

### 6. Edgegap

1. [Create an app](https://app.edgegap.com/application-management/applications/list) and a version: the image above, port **7777 UDP** named `gameport`, 1.5 vCPU / 3 GB, your Container Registry credentials, and the variables `EOS_SERVER_CLIENT_ID` and `EOS_SERVER_CLIENT_SECRET` (secret).
2. Set the app version in `matchmaker-config.json`, create the matchmaker in the dashboard (Matchmaker → Create), and copy its URL and token into `Config/UserEngine.ini`.

### 7. Play

Start Lyra (`UnrealEditor.exe <Project>.uproject -game`), press a key to sign in with your Epic account (the account portal opens in your browser), then **Play Lyra → Start a Game** with Network *Online*. Alone, you get a server after 10 s; two players queuing together share one.

- Unattended test: `-EdgegapFindMatch=DA_Convolution_ControlPoint` queues 5 s after the front end loads. It skips Lyra's press-start screen, which then stays over the match, so use it for tests only.
- Two Epic accounts on one machine: use Epic's Developer Authentication Tool with `-AUTH_TYPE=developer -AUTH_LOGIN=localhost:<port> -AUTH_PASSWORD=<credential name>`.

## Optional: the Edgegap SDK

`EdgegapIntegration` has its own small matchmaker client, so the sample needs nothing else. To use the [Edgegap SDK for Unreal Engine](https://github.com/edgegap/sdk-unreal-engine) instead, turn on the `EdgegapSDKBridge` plugin. It is off by default (`EnabledByDefault: false`), and nothing changes until you do.

With the bridge on:
- The SDK's matchmaker client does the search: beacon pings (UDP, falling back to TCP), a solo group, polling. Lyra's side stays the same: Start a Game and Quick Play, the playlist → profile map, the overlay with Cancel, travel and the rejoin after the server's map change.
- Players join with their ticket (`?edgegap_mm_ticket_id=`). On servers the matchmaker starts, the SDK's server module only lets in players whose ticket the matchmaker assigned to that server, one connection per ticket. The built-in client sends its ticket too.
- The SDK also stops the deployment when the server process exits or crashes.

Verified on Edgegap with UE 5.8.4 and SDK 0.1.0 (2026-11-03): two clients queued through the SDK, were matched onto one server in about 15 s, and joined with their tickets, including the rejoin after the server's map change. A player with no ticket, or with a ticket from another match, was kicked. Known gap: a second player reusing a ticket that is still connected was let in; this is being fixed in the SDK.

Setup:
1. Copy `Plugin/EdgegapSDK` from [sdk-unreal-engine](https://github.com/edgegap/sdk-unreal-engine) (0.1.0 or later) to your project's `Plugins/EdgegapSDK`, and `Plugins/EdgegapSDKBridge` from this repository to `Plugins/EdgegapSDKBridge`.
2. In your `.uproject`, add to `Plugins`:
   ```json
   { "Name": "EdgegapSDKBridge", "Enabled": true }
   ```
3. Rebuild the editor, and rebuild the Linux server (setup step 5) so the server gets the ticket check.

The bridge reuses `MatchmakerUrl`, `AuthToken` and `PortName` from `[/Script/EdgegapIntegration.EdgegapSessionSubsystem]`. The SDK's own settings (Project Settings → Plugins → Edgegap, `EDGEGAP_MM_URL` / `EDGEGAP_MM_TOKEN`, `-EdgegapMMUrl=` / `-EdgegapMMToken=`) win when set.

The ticket check refuses players who join through **Browse** or **Quick Play**, since EOS sessions carry no matchmaker ticket. To keep EOS joins, turn it off in `Config/DefaultGame.ini`:

```ini
[/Script/EdgegapSDK.EdgegapSettings]
bRequireMatchmakerTicket=False
```

## Configuration

`[/Script/EdgegapIntegration.EdgegapSessionSubsystem]` in `DefaultEngine.ini` / `UserEngine.ini`:

| Key | Default | Meaning |
|---|---|---|
| `bEnableMatchmaking` | `False` (`True` in the config block) | Online hosting goes through Edgegap. Off: Lyra's listen server. |
| `MatchmakerUrl`, `AuthToken` | | From the matchmaker page of the dashboard. Quote the URL. |
| `DefaultProfile` | | Profile for playlists not listed in `PlaylistProfiles`. |
| `PlaylistProfiles` | | `(Playlist="DA_Expanse_TDM",Profile="elimination")`: client and server use the same map. |
| `bSendBeaconLatencies` | `True` | Must match the profile's `latencies` rule. |
| `JoinRetryWindowSeconds`, `MaxJoinAttempts` | `60`, `5` | Rejoin when a fresh server drops early joiners (see below). |
| `bShowStatusOverlay` | `True` | Matchmaking progress overlay. |

`[/Script/EdgegapIntegration.EdgegapLyraServerSubsystem]`: `NoPlayerTimeoutSeconds` (300), `EmptyServerGraceSeconds` (15), `PortName` (`gameport`).

## Pitfalls we hit

| Symptom | Cause / fix |
|---|---|
| Players dropped with *Host closed the connection* seconds after joining | A fresh Lyra server boots on `/Engine/Maps/Entry`, then travels to the playlist's map and drops early joiners. The client rejoins its assigned server. |
| `-CustomConfig=EOS` does nothing in packaged builds | `Config/Custom/EOS` is only staged for targets with that CustomConfig, which installed engines can't build. The EOS settings go in `DefaultEngine.ini`. |
| Server waits forever on an Epic login (device code in the log) | Dedicated servers must not use Epic Account Services; `EdgegapEOSServerConfig` turns it off. |
| Server boots EOS with the player client | The online subsystem loads at PostConfigInit, before project plugins of that phase. The config module loads at `EarliestPossible`. |
| `Project contains multiple Server targets` | `DefaultServerTarget=LyraServer` (in the config block). |
| `LyraServer modifies the values of properties: [ bUseChecksInShipping ]` | Installed engine: setup step 2.4. |
| clang `error: 'break' will never be executed` in CommonUser | Setup step 2.5. |
| clang `CHAINED-SIGNAL 11` in the container | Build without UBA: `-UbtArgs="-NoUBA"`. |
| `Failed reading oplog from Zen` with `-skipcook` | Cook results live in Zen: keep the `lyra-zen` volume. |
| Deploy fails with *Unable to pull image* | Registry credentials missing on the app version. |

## License

The files in this repository are under the [Apache License 2.0](LICENSE). Lyra, Unreal Engine and their source and content are Epic Games' and are licensed under the [Unreal Engine EULA](https://www.unrealengine.com/eula).
