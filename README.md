# OpenMediaVault Intel GPU Statistics Plugin

[![Build and Release OMV Package](https://github.com/zukirenx/openmediavault-intelgpustats/actions/workflows/build-package.yml/badge.svg?branch=main)](https://github.com/zukirenx/openmediavault-intelgpustats/actions/workflows/build-package.yml)
![OMV Support](https://img.shields.io/badge/OMV-7%20%7C%208-green)
![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)

An [OpenMediaVault](https://www.openmediavault.org/) plugin for live Intel GPU
metrics and GPU utilization history in the dashboard. This plugin is adapted
from [openmediavault-nvidiastats](https://github.com/routmoute/openmediavault-nvidiastats),
which declares GPL-3.0 in its README.

This project is licensed under **GNU GPL-3.0-only**; see [LICENSE](LICENSE).

## Features

### Live metrics

- GPU Render and Video engine utilization, GT frequency, temperature, and
   GPU/package power where the hardware exposes them.
- A compact **Intel GPU** dashboard widget refreshes live readings every five
   seconds.

### Historical graph

- **Intel GPU Usage** shows utilization history with OMV's hour, day, week,
   month, and year views.
- Collectd samples the GPU's RC6 residency counter every 10 seconds. The graph
   estimates utilization from time outside the GPU idle state, so it is useful
   for trends but is not equivalent to per-engine utilization from
   `intel_gpu_top`.

### OpenMediaVault integration

- Native dashboard widgets, collectd data collection, and OMV RRD graphing.
- The collector runs as the unprivileged `nobody` user and reads sysfs.

The live summary uses `intel_gpu_top` when it is available. Temperature and
frequency use Linux sysfs; CPU package temperature is used as a proxy on systems
without a dedicated GPU sensor.

## Requirements

- OpenMediaVault 7 or 8. Tested on OpenMediaVault 8.5.1.
- An Intel GPU exposed through Linux DRM/sysfs (typically the `i915` driver).
- `intel-gpu-tools` is recommended for live engine utilization and power data.
- The RRD usage graph uses sysfs and does not require `intel-gpu-tools`.
- Collectd and OMV's RRD graphing components, installed by OMV.

## Build

From the repository root, build a `.deb` with:

```sh
bash ./build
```

This produces `openmediavault-intelgpustats_1.0.0.deb` in the repository root.
For a full Debian source build, install `debhelper` and `devscripts`, then run
`debuild -us -uc`.

## Install

```sh
sudo apt install ./openmediavault-intelgpustats_1.0.0.deb
```

The package installs the RPC service, dashboard widgets, collectd exec
collector, and RRD graph plugin. It triggers an OMV workbench rebuild and
restarts collectd. Add **Intel GPU** and **Intel GPU Usage** from the dashboard
widget picker. RRD history starts accumulating after installation, so longer
periods fill in over time.

To enable the richer live metrics, install `intel-gpu-tools` separately:

```sh
sudo apt install intel-gpu-tools
```

## Uninstall

```sh
sudo apt remove openmediavault-intelgpustats
```

## Troubleshooting

- Check that an Intel DRM device is present: `cat /sys/class/drm/card*/device/vendor`
   should include `0x8086`.
- Check collection: `systemctl status collectd` and
   `ls /var/lib/rrdcached/db/localhost/exec-intelgpu/`.
- If live engine or power readings are unavailable, install `intel-gpu-tools`
   and check `intel_gpu_top` as root.
