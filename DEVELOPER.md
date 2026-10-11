# HidHide Developer Guide

## Programmatic integration

Applications that need to communicate with *HidHide* at runtime can do so through a WDM control device that the driver
exposes under the symbolic link `\\.\HidHide` (DOS device name). The same device can alternatively be opened by the
interface GUID `{0C320FF7-BD9B-42B6-BDAF-49FEB9C91649}` via `SetupDiGetClassDevs` or `CM_Get_Device_Interface_List`.

Once opened, the device accepts a set of I/O control codes constructed with `CTL_CODE(32769, N, METHOD_BUFFERED,
FILE_READ_DATA)`. Function numbers 2048 through 2055 correspond to the get and set operations for the whitelist,
blacklist, active state, and whitelist-inverse flag respectively. List payloads are exchanged as `MULTI_SZ` buffers —
a sequence of null-terminated wide-character strings with an additional null terminator to close the list — and the
buffer size supplied to `DeviceIoControl` must be an even number of bytes and include all terminators. Device instance
paths should be in the format returned by `SetupDiGetDeviceInstanceId`, for example
`HID\VID_054C&PID_09CC&MI_03\7&...`.

Functions 2056 and 2057 expose the *session blacklist*, which is intended for feeder applications — programs that
exclusively own one or more physical devices and re-expose them as virtual controllers. Unlike the persistent blacklist
managed through functions 2050 and 2051, session blacklist entries live only in kernel memory and are never written to
the registry, leaving the user's permanent *HidHide* configuration untouched. Each entry is owned by the process that
issued the `ADD_SESSION_BLACKLIST` request. When that process exits for any reason — clean shutdown, crash, or forced
termination — the driver automatically removes all of its entries within its process-notify callback registered via
`PsSetCreateProcessNotifyRoutine`, so no cleanup code is required in the application. On a clean exit one may optionally issue `CLR_SESSION_BLACKLIST` to release the
devices immediately, but this is not required. This approach is preferable to temporarily mutating the persistent
blacklist because it requires no rollback logic and is inherently safe against unexpected termination.

## Application profiles

Functions 2058 through 2062 expose *application profiles*. A profile is a named configuration that binds one or more
applications (by full image name) to a set of devices that should be hidden **while one of those applications is
running**. This lets a user keep different device-hiding configurations per game or tool and have the right one applied
automatically, without toggling anything by hand.

Each profile is stored as a registry subkey, `HKLM\SYSTEM\CurrentControlSet\Services\HidHide\Parameters\Profiles\<Name>`,
holding two `REG_MULTI_SZ` values — `Applications` and `BlacklistedDeviceInstancePaths`. The profile name is therefore
limited to the characters allowed in a registry key name (notably no backslash) and to 128 characters.

Auto-detection is performed by the kernel driver. It already tracks process creation and the mapping between a process
id and its executable image (via `PsSetLoadImageNotifyRoutine`). When a process whose image is bound to a profile
starts, that profile becomes *active*: its `BlacklistedDeviceInstancePaths` replaces the persistent blacklist as the set
of devices evaluated for the hide decision. Activations are stacked and the most recently started profile wins
(last-started-wins); when that process exits the previous profile in the stack is restored, and when the stack is empty
the persistent blacklist applies again. The switch is a runtime-only override — the persistent blacklist and whitelist
are never modified, and nothing is written to the registry as a result of automatic activation. Because the hide
decision is evaluated when an application opens a device (`IRP_MJ_CREATE`), an activation or deactivation only affects
handles opened afterwards.

The subscription/unsubscription to a profile is tied to the lifetime of the process: when the process exits — cleanly,
by crash, or by being force-terminated — the driver removes its activation through the same process-notify callback
used by the session blacklist, so no cleanup code is required in the application.

The profile payloads are exchanged as `MULTI_SZ` buffers following the same rules as the whitelist and blacklist, with a
convention for tagging the entries of a profile: the first string is the profile name, and every subsequent string is
prefixed with `A:` (a bound application full image name) or `D:` (a device instance path). For example a `GET_PROFILE`
response for a profile named `Game` might read: `Game`, `A:C:\Games\x.exe`, `D:HID\VID_054C&PID_09CC\7&...`.

| Function | Operation | Payload |
|---|---|---|
| `GET_PROFILES` | Enumerate the names of all stored profiles | out: `MULTI_SZ` of profile names |
| `GET_PROFILE` | Read a single profile | in: `MULTI_SZ` with one string = profile name; out: `MULTI_SZ` as described above |
| `SET_PROFILE` | Create or replace a profile | in: `MULTI_SZ` as described above (name first); no output |
| `DEL_PROFILE` | Delete a profile | in: `MULTI_SZ` with one string = profile name; no output |
| `GET_ACTIVE_PROFILE` | Report the currently active profile | out: `MULTI_SZ` holding the profile name, or empty when no profile is active |

`GET_PROFILE` returns `STATUS_OBJECT_NAME_NOT_FOUND` (surfaced to a user-mode caller as `ERROR_FILE_NOT_FOUND`) when the
named profile doesn't exist, and `SET_PROFILE` rejects a name that is empty, longer than 128 characters, or that
contains a backslash. Deleting a profile that is currently active immediately deactivates it.
