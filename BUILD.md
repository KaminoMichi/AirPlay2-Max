# Сборка из исходников

Одна исходная версия соответствует `0.1.0-rc.1`. Исходное состояние для переноса — испытанная владельцем SF1 + VP2. Изменены рабочие пути, упаковка и lifecycle; аудиоалгоритм и рабочий вариант AP2 Remote payload сохранены. Новый ELF поэтому имеет собственную контрольную сумму и требует отдельного испытания на Android.

## Среда

Сборка выполняется на Linux x86_64; проверяемая среда — Ubuntu 24.04, Android NDK r30, JDK с поддержкой `javac --release 8`, Android SDK Build Tools 30.0.3 (`dx`, `aapt`, `apksigner`) и platform `android-23/android.jar` как compile stub. Целевой runtime — Android 9/API28/ARM64.

Для зависимостей нужны C/C++ toolchain, Python 3.12+, Perl, make, autoconf, automake, libtool, pkg-config, gettext/autopoint и texinfo. Системные пакеты и SDK устанавливает разработчик отдельно. Скрипты проекта не запускают `curl | sh`, не устанавливают root и не выполняют ADB при сборке.

## Полная пересборка зависимостей

Скачайте архив соответствующих исходников зависимостей из того же релиза и проверьте SHA256SUMS. В новом рабочем каталоге:

```sh
python3 tools/build-deps.py \
  --source-archive /path/to/ya-airplay-0.1.0-rc.1-third-party-sources.tar.gz \
  --ndk /path/to/android-ndk-r30 \
  --out /path/to/new-dependency-build
```

Флаг `--resume` допускается только для собственного прежнего каталога с совпадающими входами. Уже собранные библиотеки повторно используются после проверки рецепта и SHA каждого статического архива. Другой каталог или изменённые входы не принимаются.

Версии, commit IDs и configure-параметры закреплены в `tools/dependencies.json`. FFmpeg включает AAC, ALAC и PCM16; GPL/nonfree/version3, ненужные устройства и сеть выключены. Никаких libav* из host `/usr/lib` в Android ELF не подключается. Для libgpg-error явно включён совместимый `gpg-error-config`; для git-export libplist создаётся `.tarball-version` из закреплённой версии.

## Native-компоненты

```sh
python3 tools/build.py \
  --deps /path/to/new-dependency-build \
  --ndk /path/to/android-ndk-r30
```

Получаются receiver, NQPTP, supervisor, AP2 remote CLI, голосовой мост, проверка состояния загрузки и атомарный publisher. Все исходные единицы компилируются заново, обязательные символы проверяются линкером; неподдерживаемые импорты Android pthread cancellation отвергаются. Используется реальная общая реализация cooperative cancellation, а не пустые заглушки.

Основной процесс — Shairport Sync 5.6.2; NQPTP 1.2.9 остаётся отдельным процессом с smi11. Для нового рабочего пути shared memory размещается в приватном `run/shm-nqptp`. Чистка после новой загрузки не использует старые PID и не исправляет автоматически сбой той же загрузки.

## Java и APK

```sh
python3 tools/build-java.py \
  --sdk /path/to/android-sdk \
  --signed-nsd /path/to/unchanged-release-nsd.apk \
  --reference-volume /path/to/unchanged-release-volume-bridge.jar
```

Проверенная NSD APK сохраняется с прежней подписью. Скрипт заново компилирует исходники и требует точного совпадения `classes.dex` и AndroidManifest с APK. DEX Volume Bridge тоже собирается заново и сверяется с рабочим JAR. После совпадения исходный JAR сохраняется целиком, включая ZIP-метаданные. Оба reference-файла можно извлечь из payload.tar установочного релиза; приватные ключи для этого не нужны.

Приватный ключ подписи NSD не включён и не запрашивается. Этот скрипт не может создать новую подпись прежнего издателя. Для изменения APK в будущем потребуется отдельный процесс подписи внешним keystore и безопасная миграция; произвольная новая подпись не выдаётся за совместимое обновление.

## Проверки и упаковка

```sh
python3 tests/test_host.py
python3 tests/test_install.py
python3 tests/test_voice.py
python3 tools/package.py
```

Проверки используют только собственные Linux-фикстуры. Они не подтверждают Android Audio HAL, реальную перезагрузку или исполнение команды iPhone. Текущий перечень подтверждений и открытых пунктов — `TEST_RESULTS.md`.

Сборочные отчёты и бинарники появляются в `build/`; они не входят в историю исходного Git-репозитория. `tools/package.py` формирует установочный архив и SHA. Он не выполняет установку и публикацию.

## Перелинковка статических библиотек

Распакуйте relink kit из релиза. Для повторной линковки receiver с библиотеками из него либо с совместимой пересобранной заменой:

```sh
python3 tools/relink.py \
  --kit /path/to/extracted/relink \
  --ndk /path/to/android-ndk-r30 \
  --output /path/to/new-ya-shairport
```

Вы можете заменить статические архивы в собственной копии kit и выполнить линковку заново. Исходники зависимостей и лицензии распространяются вместе с kit. Побайтовая воспроизводимость проверяется для закреплённого набора статических архивов; новая подпись APK и другой toolchain не обещаются побайтово идентичными.
