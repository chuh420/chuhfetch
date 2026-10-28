# chuhfetch

A personal system fetch tool written in C. I wrote it for my own machine, so it ships with one built-in logo and you can't customize anything: no config, no themes, no flags.

It started as a clone of my macchina setup and prints the same layout without depending on macchina.

## How it works

- Reads system info from `/proc`, `/sys`, `/etc/os-release`, `statfs` and `sysinfo`, using only libc and syscalls. It never runs external programs.
- Looks up the GPU name in `/usr/share/hwdata/pci.ids`.
- Finds the window manager by connecting to the Wayland socket and reading the compositor's PID with `SO_PEERCRED`.
- Measures CPU load from `/proc/stat` over a 250 ms window.
- Measures GPU load over the same window from the i915 PMU `*-busy` counters via `perf_event_open`, the way btop does. With `perf_event_paranoid = 2` this needs `cap_perfmon` on the binary. Without it, the GPU line shows no percentage.

## Build

```sh
make
make install
```

`make install` copies the binary to `~/.local/bin` and runs `sudo setcap cap_perfmon+ep` on it.
