#!/bin/sh
# Remove o coletor do pistar-cyd-panel do Pi-Star.  Uso: sudo ./uninstall.sh
[ "$(id -u)" -eq 0 ] || { echo "rode com sudo: sudo ./uninstall.sh"; exit 1; }
was_ro=0
grep -qE '^\S+ / \S+ ro[ ,]' /proc/mounts && was_ro=1
[ $was_ro -eq 1 ] && mount -o remount,rw /
systemctl disable --now pistar-cyd-panel.service 2>/dev/null
rm -f /etc/systemd/system/pistar-cyd-panel.service /var/www/dashboard/cyd
rm -rf /usr/local/lib/pistar-cyd-panel /usr/local/share/pistar-cyd-panel
systemctl daemon-reload
sync
[ $was_ro -eq 1 ] && mount -o remount,ro /
echo "pistar-cyd-panel removido."
