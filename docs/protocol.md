# ZSS wire protocol

`zssd` listens on a Unix stream socket, `/run/zss/zssd.sock` by default
(`--socket`, or `$ZSS_SOCKET` for clients). Every message is one JSON object
on one line, ending in `\n`. Objects are flat: values are strings, integers or
booleans. Every message has a `type`. Unknown types and unknown fields are
ignored, and a malformed line is skipped.

The daemon identifies each peer with `SO_PEERCRED`; the process ID and user
it uses are the kernel's, not anything the peer claims.

GPUs are named by PCI address, for example `0000:01:00.0`. The word
`software` names a CPU renderer, which has no address.

## Who may send what

| Message | Accepted from |
| :--- | :--- |
| `register`, `state`, `outcome`, `status`, `subscribe` | any local user |
| `detach`, `attach`, `resume`, `off`, `on` | root, members of the admin group (`--group`, default `zss`), and the user the daemon runs as unless `--no-owner-access` is given |

A refused request gets `result` with `ok: false` and `message: "permission denied"`, and nothing changes.

## Application (layer) to daemon

### `register`
Sent once when the layer starts in a process.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `pid` | int | informational; the daemon uses the kernel's value |
| `name` | string | process name |

The daemon answers with `welcome`.

### `state`
Sent whenever the process's relationship to a GPU changes.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `gpu` | string | PCI address |
| `holds` | bool | the process has that GPU's driver open |
| `devices` | int | Vulkan devices currently running on it |
| `origin` | int | Vulkan devices that started on it, wherever they run now |
| `parked` | int | of those, how many are parked |
| `migratable` | bool | false if any device on it cannot be migrated |
| `reason` | string | why not, naming the untracked feature |

### `lost`
The real driver reported that the device is lost.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `gpu` | string | PCI address of the GPU the lost device was on |

The daemon answers every application using that GPU with `evacuate`. A daemon
that does not know this message ignores it; the layer then decides for itself
after five seconds.

### `outcome`
The answer to `migrate`, `evacuate`, `restore` or `resume`. Exactly one is sent per request.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `id` | int | the request's `id` |
| `result` | string | `migrated`, `parked` or `failed` |
| `target` | string | where the application now runs, when migrated |
| `error` | string | `ZSSFailedResumeNoDRM` when a resume found no suitable GPU |
| `reason` | string | explanation, e.g. the capability the target lacks |
| `lost_contents` | int | after an `evacuate`: objects whose contents only the GPU had, now zero-filled |

## Daemon to application

### `welcome`
| Field | Type | Meaning |
| :--- | :--- | :--- |
| `detached` | string | comma-separated addresses of GPUs that are currently detached; the layer does not load their drivers |
| `software` | bool | whether this daemon may send applications to the software renderer; the layer then counts it among the GPUs whose common capabilities it reports |

### `migrate`
Leave a GPU.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `id` | int | echoed in the `outcome` |
| `from` | string | GPU to leave |
| `to` | string | GPU to move to, `software`, or empty to park |

The application moves every device on `from` to `to`. If `to` is empty,
unknown to the application, or lacks something the application uses, the
application parks instead. Either way it then lets go of `from` completely.
On `failed` nothing has moved.

### `evacuate`
A GPU is lost. Unlike `migrate`, nothing may be read back from it.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `id` | int | echoed in the `outcome` |
| `from` | string | GPU that was lost |
| `to` | string | where to rebuild: another GPU, `software`, empty to park, or `from` itself |

The application rebuilds every device on `from` from what it holds in memory.
`to` equal to `from` means the GPU is still on the bus and only a driver reset
happened: devices that reported the loss are rebuilt in place, and devices
that are healthy are left alone. A layer that does not know this message
ignores it.

### `restore`
A GPU is back.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `id` | int | echoed in the `outcome` |
| `gpu` | string | GPU that returned |

The application moves back every device that started on `gpu` and resumes
those parked for it. Devices that started elsewhere are left alone.

### `resume`
Try to get parked devices running on any suitable GPU that is present now.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `id` | int | echoed in the `outcome` |

Fails with `error: "ZSSFailedResumeNoDRM"` when there is none, and the
application stays parked.

## Control client (`zssctl`) to daemon

### `status`
Optional `gpu` limits the answer to one device. The daemon sends, per GPU, one
`gpu` message followed by one `client` message per process, then `end`.

`gpu`: `gpu`, `state` (`attached`, `detaching`, `powered-off`,
`safe-to-remove`, `attaching`, or `lost` for a device that left the bus
without a detach), `wake_support` (the driver can signal a waiting caller),
`wakes` (times woken by a request), `driver_frozen` (the driver was frozen when the device went silent, and will be resumed when it is back), `iommu` (the kernel confines the device's memory access), `served` (times a device switched off on request was powered briefly for the display server), `serving` (it is powered for that reason right now), `waiting` (programs asleep on the driver of a device that was switched off on request), `idle_wait` (seconds until an automatic
power-off; 0 when none is configured), `dry_run` (applications were moved away by a
dry run), `backend`, `removal_supported`.

`client`: `gpu`, `pid`, `name`, `class` (`migratable`, `non-migratable`,
`display-server`, or `stale` for a process outside the layer still holding a
handle to a device that is gone), `reason`, and for registered applications `devices`,
`parked`, `away` (it started on this GPU and is running elsewhere).

### `detach`
| Field | Type | Meaning |
| :--- | :--- | :--- |
| `gpu` | string | GPU to detach |
| `to` | string | optional target for its applications; chosen automatically when absent |

A lost device cannot be detached; the result says it is already gone.

Zero or more `blocker` messages may precede the `result`.

`blocker`: `pid`, `name`, `class`, `reason`. When any blocker counts, the
result is `ok: false` with `error: "ZSSDetachBlocked"`. Under the dry-run
backend, processes outside the layer are listed with a class ending in
`ignored in dry run` and do not block.

### `attach`
| Field | Type | Meaning |
| :--- | :--- | :--- |
| `gpu` | string | GPU to attach |

### `off`
Power a suspend-in-place GPU off under the running desktop.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `gpu` | string | GPU to power off |
| `to` | string | optional target for its applications |
| `console` | bool | switch the screen to a text console and report there |

`progress` messages arrive while it runs, then `result`. Blockers are reported
as for `detach`, except that a display server and the services listed in
`stop_services` do not block, and that a process which cannot be moved is
frozen until the device is powered on instead of blocking. It still blocks if it
cannot be frozen, or if the request came from it or one of its children and
`console` is not set (see the `gpu-idle-power` specification).

### `on`
| Field | Type | Meaning |
| :--- | :--- | :--- |
| `gpu` | string | GPU to power on |
| `return` | bool | also move back the applications that started on it |

Also accepted for a device in state `lost` that the kernel module manages: the
module tries to restore power, then its configuration, then resumes a driver
it had frozen. It is refused, naming them, while programs other than the
display server and the listed services still hold a device whose driver is
frozen.

### `progress`
One step of an `off` or `on`, as a line of text prefixed `[ZrnSelectiveSuspend]`.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `text` | string | the line |

### `resume`
| Field | Type | Meaning |
| :--- | :--- | :--- |
| `pid` | int | parked application to resume |

### `result`
Ends `detach`, `attach` and `resume`.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `ok` | bool | whether the request succeeded |
| `error` | string | `ZSSDetachBlocked` or `ZSSFailedResumeNoDRM`, when one applies |
| `message` | string | human-readable detail |
| `gpu`, `state`, `dry_run` | | the device's state afterwards, for detach and attach |

### `subscribe`
From then on the connection receives an `event` for every state change:
`gpu`, `old`, `new`.
