#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root: sudo ./install.sh" >&2
    exit 1
fi

package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary="$package_dir/antennaguardian-pilite"
example="$package_dir/config.example.json"
service="$package_dir/antennaguardian-pilite.service"

for required in "$binary" "$example" "$service"; do
    if [ ! -f "$required" ]; then
        echo "Missing package file: $required" >&2
        exit 1
    fi
done

if ! getent group antennaguardian >/dev/null 2>&1; then
    groupadd --system antennaguardian
fi
if ! getent passwd antennaguardian >/dev/null 2>&1; then
    useradd --system --gid antennaguardian --home-dir /nonexistent \
        --shell /usr/sbin/nologin antennaguardian
fi

install -m 0755 "$binary" /usr/local/bin/antennaguardian-pilite
install -d -m 0750 -o root -g antennaguardian /etc/antennaguardian-pilite
if [ ! -f /etc/antennaguardian-pilite/config.json ]; then
    install -m 0640 -o root -g antennaguardian "$example" \
        /etc/antennaguardian-pilite/config.json
    echo "Created /etc/antennaguardian-pilite/config.json"
else
    echo "Preserved existing /etc/antennaguardian-pilite/config.json"
fi
install -m 0644 "$service" /etc/systemd/system/antennaguardian-pilite.service
systemctl daemon-reload

cat <<'EOF'

Installed, but NOT enabled or started.

1. Edit /etc/antennaguardian-pilite/config.json
2. Validate it:
   sudo -u antennaguardian /usr/local/bin/antennaguardian-pilite --check-config
3. Observe without creating an interlock:
   sudo -u antennaguardian /usr/local/bin/antennaguardian-pilite --observe
4. When ready, enable protection:
   sudo systemctl enable --now antennaguardian-pilite
EOF
