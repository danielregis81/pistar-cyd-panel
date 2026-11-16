#!/bin/sh
# Instala o coletor do pistar-cyd-panel NO Pi-Star.
# Uso (no Pi-Star, dentro da pasta do projeto):  sudo ./install.sh
set -e
cd "$(dirname "$0")"

[ "$(id -u)" -eq 0 ] || { echo "rode com sudo: sudo ./install.sh"; exit 1; }
[ -f /etc/pistar-release ] || echo "aviso: /etc/pistar-release não encontrado (isto é um Pi-Star?)"
for f in pistar-cyd-panel.py countries.json pistar-cyd-panel.service ensure-link.sh; do
    [ -f "$f" ] || { echo "faltando: $f (rode dentro da pasta pistar/ do projeto)"; exit 1; }
done

was_ro=0
grep -qE '^\S+ / \S+ ro[ ,]' /proc/mounts && was_ro=1
[ $was_ro -eq 1 ] && mount -o remount,rw /
restore() {
    sync
    if [ $was_ro -eq 1 ] && ! mount -o remount,ro / 2>/dev/null; then
        echo "aviso: não consegui voltar o sistema para somente leitura agora (algum processo está usando)."
        echo "       Ele volta sozinho no próximo reboot, ou rode: sudo mount -o remount,ro /"
    fi
}
trap restore EXIT

install -d /usr/local/lib/pistar-cyd-panel /usr/local/share/pistar-cyd-panel/flags
install -m 644 pistar-cyd-panel.py countries.json /usr/local/lib/pistar-cyd-panel/
install -m 755 ensure-link.sh /usr/local/lib/pistar-cyd-panel/
cp flags/*.565 /usr/local/share/pistar-cyd-panel/flags/
chmod 644 /usr/local/share/pistar-cyd-panel/flags/*.565
install -m 644 pistar-cyd-panel.service /etc/systemd/system/
ln -sfn /run/pistar-cyd-panel /var/www/dashboard/cyd

systemctl daemon-reload
systemctl enable pistar-cyd-panel.service >/dev/null 2>&1
systemctl restart pistar-cyd-panel.service

# a primeira leitura do log pode levar alguns segundos num Pi Zero
echo "aguardando o primeiro status.json..."
i=0; code=000
while [ $i -lt 90 ]; do
    code=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1/cyd/status.json)
    [ "$code" = 200 ] && break
    sleep 2; i=$((i+2))
done

if [ "$code" = 200 ]; then
    ip=$(hostname -I | awk '{print $1}')
    echo "OK: dados em http://$ip/cyd/status.json"
    echo "Use este endereço no config.h do firmware: PISTAR_HOST \"$ip\""
else
    echo "ERRO: /cyd/status.json não respondeu. Veja: journalctl -u pistar-cyd-panel -n 30"
    exit 1
fi
