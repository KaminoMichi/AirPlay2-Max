#!/system/bin/sh
# SPDX-License-Identifier: MIT
set -eu
umask 077
R=/data/local/tmp/ya-airplay
P=/data/local/tmp/ya-airplay.previous
F=/data/local/tmp/ya-airplay.failed
HOOK=/data/adb/service.d/ya-airplay.sh
PACKAGE=ya.station.airplay
hook_created=0
[ "$(id -u)" = 0 ] || { echo 'Нужен существующий root; установщик его не получает.' >&2; exit 77; }
[ "$(getprop ro.product.device)" = yandexstation_2 ] || exit 65
[ "$(getprop ro.build.version.incremental)" = 5000000921 ] || exit 65
[ "$(getprop ro.build.version.sdk)" = 28 ] || exit 65
[ "$(getprop ro.product.cpu.abi)" = arm64-v8a ] || exit 65
[ "$(getenforce)" = Permissive ] || { echo 'SELinux Enforcing: эта конфигурация не проверена; никаких изменений SELinux.' >&2; exit 65; }
owned() {
 [ -d "$1" ] && [ ! -L "$1" ] && [ "$(stat -c %u "$1")" = 0 ] &&
 [ -f "$1/PRODUCT" ] && [ ! -L "$1/PRODUCT" ] && [ "$(cat "$1/PRODUCT")" = YA_AIRPLAY_PUBLIC_V1 ]
}
hook_owned() {
 [ -f "$HOOK" ] && [ ! -L "$HOOK" ] && [ "$(stat -c %u "$HOOK")" = 0 ] &&
 [ "$(head -n 2 "$HOOK" | tail -n 1)" = '# YA_AIRPLAY_PUBLIC_V1' ]
}
check_hook() {
 [ -d /data/adb/service.d ] && [ ! -L /data/adb ] && [ ! -L /data/adb/service.d ] || { echo 'Нет поддерживаемого /data root boot hook. Hybrid Root rootd сам по себе такого hook не предоставляет.' >&2; return 69; }
 [ "$(stat -c %u /data/adb/service.d)" = 0 ] || return 77
 # Refuse a writable directory: an unprivileged owner must not replace a root boot script.
 mode=$(stat -c %a /data/adb/service.d); case "$mode" in 700|750|755) ;; *) return 77;; esac
 if [ -e "$HOOK" ] || [ -L "$HOOK" ]; then hook_owned || return 65; fi
}
set_hook() {
 check_hook
 if [ ! -e "$HOOK" ]; then
  (set -C; printf '%s\n' '#!/system/bin/sh' '# YA_AIRPLAY_PUBLIC_V1' 'exec /system/bin/sh /data/local/tmp/ya-airplay/boot.sh' > "$HOOK")
  chmod 700 "$HOOK"
  hook_created=1
 fi
 printf 'enabled\n' > "$R/autostart.enabled"
 echo 'Автозапуск включён через существующий /data root boot hook; требуется испытание перезагрузки.'
}
remove_hook() {
 if [ -e "$HOOK" ] || [ -L "$HOOK" ]; then hook_owned || exit 65; rm "$HOOK"; fi
 [ ! -e "$R/autostart.enabled" ] || rm "$R/autostart.enabled"
}
stop_owned() { owned "$R" || return 65; "$R/stop.sh"; }
OP=${1:-}; AUTOSTART=${2:-off}; VOICE=${3:-off}
case "$OP" in
 status) owned "$R" || exit 65; if "$R/status.sh"; then exit 0; else rc=$?; [ "$rc" = 3 ] && exit 0; exit "$rc"; fi;;
 start) owned "$R" || exit 65; exec "$R/start.sh" 0.0.0.0;;
 stop) stop_owned; exit;;
 voice-start) owned "$R" || exit 65; exec "$R/voice-start.sh";;
 voice-stop) owned "$R" || exit 65; exec "$R/voice-stop.sh";;
 play|pause|next|previous) owned "$R" || exit 65; exec "$R/bin/ya-remote" "$OP";;
 autostart-on) owned "$R" || exit 65; set_hook; exit;;
 autostart-off) owned "$R" || exit 65; remove_hook; exit;;
 rollback)
  owned "$R" && owned "$P" || exit 65
  [ ! -e "$F" ] && [ ! -L "$F" ] || exit 65
  stop_owned
  mv "$R" "$F"; mv "$P" "$R"
  echo 'Предыдущая версия восстановлена; не запускается автоматически. Неудачная версия сохранена в ya-airplay.failed.'
  exit;;
 uninstall)
  owned "$R" || exit 65
  [ ! -e "$F" ] && [ ! -L "$F" ] || exit 65
  stop_owned
  remove_hook
  # Preserve rather than recursively erase logs/config/backups.
  [ ! -e "$F" ] && [ ! -L "$F" ] || exit 65
  mv "$R" "$F"
  if [ -f "$F/APK_CREATED" ]; then
   path=$(pm path "$PACKAGE"); case "$path" in package:/*) path=${path#package:};; *) exit 65;; esac
   value=$(sha256sum "$path"); expected=$(cat "$F/APK_SHA256")
   [ "${value%% *}" = "$expected" ] || exit 65
   pm uninstall "$PACKAGE"
  fi
  echo 'AirPlay остановлен. Его файлы сохранены в ya-airplay.failed; прежние backups сохранены.'
  exit;;
 install|update) ;;
 *) echo 'Unknown operation' >&2; exit 64;;
esac
case "$VOICE" in off|on) ;; *) exit 64;; esac
case "$AUTOSTART" in off|required) ;; *) exit 64;; esac
if [ "$AUTOSTART" = required ]; then check_hook; fi
S=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
case "$S" in /data/local/tmp/ya-airplay-stage-*) ;; *) exit 65;; esac
[ -f "$S/payload.tar" ] && [ ! -L "$S/payload.tar" ] || exit 65
VERSION=0.1.0-rc.1
if [ -e "$R" ] || [ -L "$R" ]; then
 owned "$R" || { echo 'Чужая или старая установка: автоматическая замена запрещена.' >&2; exit 65; }
 if [ "$(cat "$R/VERSION")" = "$VERSION" ]; then
  (cd "$R" && sha256sum -c MANIFEST.sha256) >/dev/null
  echo 'Та же версия уже установлена; ничего не заменено и не перезапущено.'; exit 0
 fi
 [ "$OP" = update ] || { echo 'Для предыдущей публичной версии используйте update.' >&2; exit 65; }
 [ ! -e "$P" ] && [ ! -L "$P" ] || { echo 'Предыдущая резервная копия уже существует; она не перезаписывается.' >&2; exit 65; }
fi
# A separate legacy installation is never stopped/taken over implicitly.
if [ ! -e "$R" ]; then
 if pidof ya-shairport >/dev/null 2>&1 || pidof ya-nqptp >/dev/null 2>&1 || pidof ya-nqptp-v2 >/dev/null 2>&1; then
  echo 'Уже работает другой AirPlay/NQPTP. Сначала остановите его штатной командой владельца.' >&2; exit 75
 fi
fi
NEW="$R.new"
[ ! -e "$NEW" ] && [ ! -L "$NEW" ] || exit 65
mkdir -m 700 "$NEW"
committed=0; backed=0; apk_created=0
rollback_failed_install() {
 code=$?; trap - EXIT INT TERM
 if [ "$code" != 0 ]; then
  echo "Установка остановлена (код $code); выполняется только ограниченный откат наших файлов." >&2
  if [ "$committed" = 1 ]; then
   if "$R/stop.sh"; then
    if [ ! -e "$F" ] && [ ! -L "$F" ]; then mv "$R" "$F"; fi
   else echo 'STOP: чистая остановка не подтверждена; файлы сохранены, никаких kill по PID.' >&2; exit "$code"; fi
  fi
  if [ "$backed" = 1 ] && [ ! -e "$R" ]; then mv "$P" "$R"; fi
  if [ "$hook_created" = 1 ] && hook_owned; then rm "$HOOK"; fi
  if [ "$apk_created" = 1 ]; then
   installed=$(pm path "$PACKAGE" 2>/dev/null || :)
   case "$installed" in package:/*)
    installed=${installed#package:}; value=$(sha256sum "$installed")
    if [ "${value%% *}" = "$apk_sha" ]; then pm uninstall "$PACKAGE"; else echo 'APK identity changed; preserved for manual review' >&2; fi;;
   *) echo 'APK state unclear; no guessed uninstall' >&2;; esac
  fi
  # Keep NEW/staging on pre-commit errors as evidence; no broad deletion.
 fi
 exit "$code"
}
trap rollback_failed_install EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
tar -xf "$S/payload.tar" -C "$NEW"
owned "$NEW" || exit 65
(cd "$NEW" && sha256sum -c MANIFEST.sha256) >/dev/null
chmod 700 "$NEW" "$NEW/bin" "$NEW/etc" "$NEW/lib"
mkdir -m 700 "$NEW/run"
for f in "$NEW/bin"/* "$NEW"/*.sh; do chmod 700 "$f"; done
APK="$NEW/lib/nsd.apk"
value=$(sha256sum "$APK"); apk_sha=${value%% *}
existing=$(pm path "$PACKAGE" 2>/dev/null || :)
if [ -n "$existing" ]; then
 case "$existing" in package:/*) path=${existing#package:};; *) exit 65;; esac
 case "$path" in *'
'*) echo 'Split APK not supported' >&2; exit 65;; esac
 value=$(sha256sum "$path")
 [ "${value%% *}" = "$apk_sha" ] || { echo 'Другой NSD APK/подпись. Автоматическое удаление или замена запрещены.' >&2; exit 65; }
else
 pm install "$APK"
 apk_created=1; printf 'installed by YA AirPlay\n' > "$NEW/APK_CREATED"
fi
printf '%s\n' "$apk_sha" > "$NEW/APK_SHA256"
if [ -e "$R" ]; then
 stop_owned
 # Keep existing user settings and package ownership record.
 cp -p "$R/etc/shairport.conf.in" "$NEW/etc/shairport.conf.in"
 if [ -f "$R/APK_CREATED" ]; then cp -p "$R/APK_CREATED" "$NEW/APK_CREATED"; fi
 mv "$R" "$P"; backed=1
fi
if [ "$VOICE" = on ]; then printf "enabled\n" > "$NEW/voice.enabled"; fi
mv "$NEW" "$R"; committed=1
"$R/start.sh" 0.0.0.0
if [ "$AUTOSTART" = required ]; then set_hook; fi
"$R/bin/ya-supervisor" status
trap - EXIT INT TERM
echo 'YA AirPlay установлен и запущен. Выберите YA Station Custom в AirPlay.'
if [ "$AUTOSTART" = off ]; then echo 'Автозапуск не включён; после перезагрузки потребуется start.'; fi
