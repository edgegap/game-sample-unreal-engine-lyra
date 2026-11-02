# Runtime image for the packaged Lyra Linux dedicated server on Edgegap.
# Package first into Packaged/LinuxServer (see README, "Build the Linux server"), then:
#   docker build --platform linux/amd64 --build-arg PROJECT=LyraStarterGame -t registry.edgegap.com/<registry-project>/lyra:<tag> .
# PROJECT is the .uproject name: LyraStarterGame for a project created from Fab, Lyra in Epic's GitHub repo.
FROM ubuntu:22.04

ARG PROJECT=LyraStarterGame
ENV PROJECT=${PROJECT}

ARG DEBIAN_FRONTEND=noninteractive
# ca-certificates: HTTPS for EOS and the server's calls to Edgegap.
RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Unreal refuses to start as root.
RUN useradd -m -u 1000 server
WORKDIR /app
COPY --chown=server:server Packaged/LinuxServer /app/
RUN chmod +x /app/${PROJECT}/Binaries/Linux/LyraServer
USER server

# Documentation only: the port Edgegap exposes is the one on the app version.
EXPOSE 7777/udp

# exec replaces the shell, so the server is PID 1 and gets the SIGTERM Edgegap sends when the deployment stops
# (the generated LyraServer.sh doesn't exec). -stdout -FullStdOutLogOutput: otherwise Edgegap logs stay empty.
# No map: the server starts on the front end, then Lyra's dedicated server flow hosts the playlist
# picked by the matchmaker profile (or -Playlist=<UserFacingExperience>).
ENTRYPOINT ["/bin/sh", "-c", "exec /app/$PROJECT/Binaries/Linux/LyraServer $PROJECT -log -port=7777 -stdout -FullStdOutLogOutput \"$@\"", "--"]
