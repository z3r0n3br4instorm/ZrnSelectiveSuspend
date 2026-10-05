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
| `detach`, `attach`, `resume` | root, members of the admin group (`--group`, default `zss`), and the user the daemon runs as unless `--no-owner-access` is given |

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

### `outcome`
The answer to `migrate`, `restore` or `resume`. Exactly one is sent per request.

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `id` | int | the request's `id` |
| `result` | string | `migrated`, `parked` or `failed` |
| `target` | string | where the application now runs, when migrated |
| `error` | string | `ZSSFailedResumeNoDRM` when a resume found no suitable GPU |
| `reason` | string | explanation, e.g. the capability the target lacks |

## Daemon to application

### `welcome`
| Field | Type | Meaning |
| :--- | :--- | :--- |
| `detached` | string | comma-separated addresses of GPUs that are currently detached; the layer does not load their drivers |

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
`safe-to-remove`, `attaching`), `dry_run` (applications were moved away by a
dry run), `backend`, `removal_supported`.

`client`: `gpu`, `pid`, `name`, `class` (`migratable`, `non-migratable`,
`display-server`), `reason`, and for registered applications `devices`,
`parked`, `away` (it started on this GPU and is running elsewhere).

### `detach`
| Field | Type | Meaning |
| :--- | :--- | :--- |
| `gpu` | string | GPU to detach |
| `to` | string | optional target for its applications; chosen automatically when absent |

Zero or more `blocker` messages may precede the `result`.

`blocker`: `pid`, `name`, `class`, `reason`. When any blocker counts, the
result is `ok: false` with `error: "ZSSDetachBlocked"`. Under the dry-run
backend, processes outside the layer are listed with a class ending in
`ignored in dry run` and do not block.

### `attach`
| Field | Type | Meaning |
| :--- | :--- | :--- |
| `gpu` | string | GPU to attach |

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
