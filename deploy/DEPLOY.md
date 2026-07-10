# Deploying solsrv (sub-project 1)

Target: a small Debian/Ubuntu VPS. Caddy terminates HTTPS and proxies to
solsrv on loopback. solsrv never touches TLS (spec §1).

## 1. Provision (once)

As root on the fresh VPS:

```sh
apt update && apt install -y build-essential clang rsync sqlite3 \
    debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
    | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
    | tee /etc/apt/sources.list.d/caddy-stable.list
apt update && apt install -y caddy

adduser --disabled-password --gecos '' solarium
mkdir -p /home/solarium/data /home/solarium/backups
chown -R solarium:solarium /home/solarium
```

Point your domain's A/AAAA records at the VPS before starting Caddy — it
needs them to obtain certificates.

## 2. Ship + build (every deploy)

From the repo on the Mac (server sources + vendor, no build artifacts):

```sh
rsync -av --exclude '*.o' \
    build.sh sol_base.h \
    json.c json.h nid.c nid.h sha256.c sha256.h b64.c b64.h \
    srv_rand.c srv_rand.h srv_db.c srv_db.h srv_events.c srv_events.h \
    srv_auth.c srv_auth.h srv_web.c srv_web.h srv_main.c \
    vendor deploy \
    solarium@YOUR_VPS:/home/solarium/src/
```

On the VPS as `solarium`:

```sh
cd ~/src && ./build.sh server && cp solsrv ~/solsrv
```

(`clang` is installed above; `CC=gcc ./build.sh server` works too.)

## 3. First run (once)

As root:

```sh
cp /home/solarium/src/deploy/solsrv.service /etc/systemd/system/
cp /home/solarium/src/deploy/Caddyfile /etc/caddy/Caddyfile   # edit the domain first
systemctl daemon-reload
systemctl enable --now solsrv
systemctl reload caddy
```

As `solarium` — create your user and the placeholder boards:

```sh
printf 'YOUR_PASSWORD' | ~/solsrv -d ~/data/solsrv.db --adduser fran
~/solsrv -d ~/data/solsrv.db --seed
```

(Run these while the service is up or down — WAL + busy_timeout make a
second process safe.)

Visit `https://your-domain/` → sign in → the seeded board list. Done =
spec sub-project 1 exit criterion.

**Reverse-proxy note:** Caddy appends the real client IP to `X-Forwarded-For`
by default. solsrv trusts that header only when the TCP peer is loopback
(127.0.0.1 / ::1), so login throttling stays per-client IP even behind the
proxy. If you ever bind solsrv on a non-loopback interface (don't), XFF is
ignored and the TCP peer address is used as the throttle key.

## 4. Backups (once)

As `solarium`, `crontab -e`:

```
17 3 * * * /home/solarium/src/deploy/backup.sh /home/solarium/data/solsrv.db /home/solarium/backups
```

Restore = stop solsrv, copy a snapshot over `~/data/solsrv.db` (remove any
`-wal`/`-shm` sidecars), start solsrv.

## 5. Update code later

Repeat step 2, then as root: `systemctl restart solsrv`.
