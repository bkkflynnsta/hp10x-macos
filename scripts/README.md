# scripts

Maintenance scripts for a Mac with an HP Laser 103/107/108. They're run from a clone of this repo and aren't part of the installer package: `installer/build-pkg.sh` packages only the files it names.

| Script | What it does |
|---|---|
| `hp108-check.sh [queue]` | Read-only health check of a print queue (default `HP_Laser_108a`) and the printer's USB connection. |
| `remove-108a-support.sh` | Removes the hp10x driver and HP's Samsung printer driver, for a clean reinstall. Run with `sudo`. Delete the print queues first. |

The installed driver has its own uninstaller, which removes hp10x only: `sudo /Library/Printers/hp10x/uninstall.sh`.
