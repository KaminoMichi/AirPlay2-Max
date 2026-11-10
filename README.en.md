# YA AirPlay 2

AirPlay 2 for the Yandex Station Max with Zigbee, **YNDX-00052**, firmware **5000000921**, Android 9/API28/ARM64. It uses Android's existing audio framework and installs under `/data`.

**0.1.0-rc.1 is a release candidate.** The owner confirmed normal playback and commands with the installed SF1 + VP2 baseline. The new installation package and reboot lifecycle still need device testing. Other hardware revisions and firmware builds are rejected.

Features include iPhone playback, Android hardware volume and reverse volume synchronization, protected Play/Pause commands, and an optional native SDK event bridge for “Alice, pause”. Mac, Next/Previous, semantic voice discrimination and offline recognition are not claimed as verified.

Download the install archive and SHA256SUMS from the same release. Verify the checksum, extract it, connect through your existing ADB access, then run:

```sh
python3 install.py --model YNDX-00052 --root rootd --hybrid-root-version 1.2.4 --voice on
```

Python 3.12+ and Android Platform Tools are required on your computer. Existing root is required; this installer does not acquire root, modify firmware or change SELinux. Review the displayed operation and type `YES`.

Autostart requires an **existing** `/data/adb/service.d` root hook and the separate `--autostart required` option. That hook was not found on the tested Hybrid Root setup, so autostart is not claimed for it. Use the documented `start` command after reboot.

[Russian installation guide](INSTALL.md) · [Uninstall/rollback](UNINSTALL.md) · [Build](BUILD.md) · [Licenses](THIRD_PARTY_NOTICES.md)

The source repository contains one current version. Install binaries, corresponding dependency sources and a static relink kit are separate release assets. Original adapters are MIT-licensed; upstream components retain their licenses, including GPL-2.0-only for the separate NQPTP executable.
