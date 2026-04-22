# Container Internals Simulator

A from-scratch Linux container runtime built in C, implementing the full lifecycle that real container engines like Docker and containerd use under the hood: namespaces, cgroups, pivot_root, veth networking, image registry, scheduling, monitoring, and more.

Built for learning and demonstration — every subsystem is written explicitly so the internals are visible and inspectable.

---

## Features

| Module | What it does |
|--------|-------------|
| **Namespaces** | PID, UTS, Mount, Net isolation via `clone(2)` |
| **Filesystem** | `pivot_root` + `/proc` mount, chroot fallback |
| **Resources** | CPU / memory / process limits via `RLIMIT_*` and cgroups |
| **Networking** | Linux bridge (`csbr0`), veth pairs, per-container IPs, port publishing |
| **Image registry** | Build, tag, inspect, and run from named images |
| **Scheduler** | Background time-slicing with configurable slice duration |
| **Monitor** | Live `/proc/<pid>` stats: CPU%, RSS, I/O, uptime |
| **Security** | Capabilities (`cap_set_proc`), seccomp BPF, read-only rootfs |
| **Events** | Ring-buffer event bus with follow mode and type filtering |
| **Metrics** | Counters + latency; Prometheus exposition format |
| **Orchestrator** | Multi-container spec (JSON), dependency ordering, health checks, restart policies |
| **Alerts** | CPU% / RSS threshold alerts with firing/resolved lifecycle |
| **Web dashboard** | Live REST API + browser dashboard (containers, charts, event stream) |

---

## Requirements

- **Linux** or **WSL2** (Ubuntu 22.04+ recommended)
- `gcc`, `make`
- `sudo` (namespaces + cgroups require root)
- `libcap-dev` for capability management

```bash
# Ubuntu / Debian
sudo apt-get install gcc make libcap-dev
```

---

## Quick start

```bash
git clone https://github.com/your-username/ContainerSimulator.git
cd ContainerSimulator

# Build the simulator and workload binaries
make

# Bootstrap minimal rootfs environments for tests and demos
make setup          # or: bash scripts/setup-rootfs.sh

# Run (root required for namespace + cgroup syscalls)
sudo ./container-sim
```

---

## Usage

The simulator is an interactive CLI. Type `help` inside it for the full command list.

### Container lifecycle

```
run   myapp myhost ./rootfs/test-basic /bin/sh -c "echo hello"
runbg myapp myhost ./rootfs/test-basic /bin/sleep 60
list
inspect container-0001
logs -f container-0001
stop container-0001
delete container-0001
```

### Resource limits

```
run --cpu 5 --mem 128 --pids 20 myapp myhost ./rootfs/test-basic /bin/sh
```

### Exec into a running container

```
exec container-0001 /bin/sh
```

### Networking

```
net init                          # create bridge (requires root)
run -p 8080:80 myapp myhost ./rootfs/test-basic /bin/sh
net ls
```

### Image registry

```
image build myimage:latest ./rootfs/test-basic
image ls
run myapp myhost myimage:latest /bin/sh
```

### Scheduler

```
sched on
sched slice 200
sched status
```

### Alerts

```
alert set container-0001 cpu 80
alert set container-0001 mem 256
alert ls
alert rm container-0001
```

### Metrics

```
metrics
metrics --prometheus
```

### Orchestrator

```
orch validate specs/demo.json
orch run specs/demo.json
orch status
orch down
```

### Web dashboard

```
web 8080
```

Then open `http://localhost:8080` in your browser. The dashboard auto-refreshes every 2 seconds.

---

## Testing

```bash
# Run the full test suite (requires sudo for isolation tests)
make test

# Or run an individual suite
sudo bash tests/test_basic.sh
sudo bash tests/test_isolation.sh
sudo bash tests/test_resources.sh
sudo bash tests/test_scheduler.sh
sudo bash tests/test_monitoring.sh
sudo bash tests/test_network.sh
sudo bash tests/test_security.sh
sudo bash tests/test_events.sh
sudo bash tests/test_orchestrator.sh
```

---

## Project structure

```
src/                  C source — one file per module
  main.c              CLI entry point + command dispatch
  container.c         Lifecycle: create, start, stop, delete, exec, pause
  namespace.c         clone(2) with CLONE_NEWPID|UTS|MNT|NET flags
  filesystem.c        pivot_root, /proc mount, chroot fallback
  resource.c          RLIMIT_* + cgroup v1/v2 enforcement
  network.c           veth pairs, bridge attachment, IP assignment
  bridge.c            Linux bridge (csbr0) + port forwarding rules
  image.c             Image registry: build, tag, inspect, rm
  scheduler.c         Time-slice scheduler (background thread)
  monitor.c           /proc/<pid> stat + io reader
  security.c          cap_set_proc, seccomp BPF, /proc masking
  eventbus.c          Ring-buffer event bus
  metrics.c           Counters + Prometheus format
  orchestrator.c      Multi-container spec runner
  alert.c             Threshold alert system
  webserver.c         Embedded HTTP/1.0 server + REST API
  workload_*.c        Test workload programs (CPU, mem, fork, net)

web/                  Dashboard frontend (served by webserver.c)
  index.html
  style.css
  app.js

tests/                Shell-based integration test suites
specs/                Example orchestrator JSON specs
scripts/
  setup-rootfs.sh     Bootstrap minimal rootfs for tests
```

---

## Install system-wide

```bash
sudo make install           # installs to /usr/local/bin
sudo make install PREFIX=/opt/container-sim
sudo make uninstall
```

---

## License

MIT
