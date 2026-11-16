#!/bin/sh
# Garante o link do dashboard para os dados em memoria.
# O sistema de arquivos do Pi-Star fica somente leitura: se precisar criar o link,
# libera a escrita, cria e volta como estava.
LINK=/var/www/dashboard/cyd
TARGET=/run/pistar-cyd-panel
[ "$(readlink "$LINK" 2>/dev/null)" = "$TARGET" ] && exit 0

was_ro=0
grep -qE '^\S+ / \S+ ro[ ,]' /proc/mounts && was_ro=1
[ $was_ro -eq 1 ] && mount -o remount,rw /
ln -sfn "$TARGET" "$LINK"
sync
[ $was_ro -eq 1 ] && mount -o remount,ro /
exit 0
