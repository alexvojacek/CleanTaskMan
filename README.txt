Clean Task Manager 0.7.0
Windows 11 x64. Run CleanTaskMan.exe from the portable ZIP or use the optional
per-user installer. No added driver, .NET runtime, or background service is
required. Upgrades and uninstall preserve the app's configuration files.

Built by Reed and Morgan with direction and testing by Alex.
The approved palette remains unchanged. The selected CPU, Storage, Network and
GPU output values are one additional DIP smaller, with regular weight. Labels,
metadata, Memory and CPU counts/uptime retain their previous sizes. Sidebar
metadata grows one DIP where it fits.

Startup
The empty interface is prepared before showing its window. After its first
visible draw and a compositor flush, native collection begins on background
workers. Dynamic fields remain blank until a valid reading exists. Graphs use
actual timestamped samples; no sample values or invented history are shown.
CPU rates use one real 100 ms startup interval. PDH rate counters are primed
before inventory discovery. Normal refreshes then follow the configured speed.
The first short measurement is naturally less averaged than later readings.
Individual Windows counters can remain blank until their source reports valid
data; no fabricated values or smoothing are used to conceal that interval.

Live measurements
- Processes and Details: native executable names, process IDs, creation-time
  identities, CPU busy-time percentages normalized over all logical processors,
  private working set where permitted, architecture, user identity when known.
  Processes groups identical executable paths within each Windows session;
  expand a row to inspect actual PIDs. Section counts describe display groups.
  Details and Performance retain individual process counts.
- CPU: overall or logical-processor utilization, dynamic Windows-reported
  processor frequency, processor
  topology/cache sizes, process/thread/handle totals, uptime, virtualization.
  Logical graphs show a small green leaf and "park" only when Windows reports
  that logical processor parked. Idle utilization alone never sets the badge.
- Memory: physical used/available RAM, installed capacity, commit, system cache,
  paged and non-paged kernel pools. Firmware reports configured memory speed,
  occupied/available slots, module capacity/type and original slot locators.
  Click or hover over the slot details to see the full module list.
  Compact slot numbers follow the firmware's module-list order; full locators
  remain available in that list.
- Disks: physical-disk counters for activity, reads/writes, average response
  time; model/type/capacity when accessible. A RAM disk may appear as a disk.
  The lower-right panel adds Capacity, Formatted, System disk, Page file and
  Type. Formatted sums each mapped Windows filesystem volume once, using
  quota-independent allocation totals (or a non-quota filesystem fallback).
  Volume GUIDs and physical extents map volumes to their actual device.
  System disk identifies the Windows-directory volume; Page file reflects
  active native pagefile enumeration. Unsupported/incomplete fields remain
  blank. Multi-disk volumes leave Formatted blank when attribution is ambiguous.
  Storage metadata refreshes on the slow background inventory cycle, never
  during drawing. SSD (SATA) is reported only with a valid no-seek-penalty result.
  Memory and Storage main graphs now use denser, size-aware grid spacing.
- Network: connected physical Ethernet/Wi-Fi adapters and byte-counter deltas.
  Filter-driver interfaces are excluded. Numeric IPv4 and IPv6 addresses are
  matched to the same adapter identity used for its counters.
- GPUs: engine utilization and adapter memory counters; supported GPU
  temperatures through the existing Windows graphics driver. Software and
  indirect display aliases are excluded; physical PCI identities and adapter
  LUIDs distinguish the devices. Installed Windows driver version is included.
  The sidebar uses three compact text lines, including the GPU model, within
  the same 64-DIP row height for every device (two DIPs added above and below).
  The device list keeps a fixed 222-DIP width so the full RTX 5070/5080 names
  fit at minimum window width. Its right edge and the adjacent graph/title
  starting positions stay fixed when dragging the window's right edge. The
  previous 222-to-230-DIP width breakpoint has been removed. Fonts, row heights
  and internal alignment are retained across the Performance views.
  Network rows show connection type, adapter alias, then throughput.
  GPU now has 3D/Copy/Video Encode/Video Decode graphs plus dedicated/shared memory histories. Default window size is unchanged; short windows scroll the GPU content. Driver date (YYYY-MM-DD) and compact PCI location are included. Unsupported readings stay blank.
  GPU and Storage bottom readouts use compact text-sized columns, with the
  metadata alongside at narrower window widths. GPU always keeps Utilization
  and Temperature together on the first row, Dedicated GPU memory on the
  second and Shared GPU memory on the third, at every window width. Short/narrow
  views retain a scroll/reflow fallback for metadata when the content cannot fit.
  Units and measurement precision are retained.
- Services: native service names, display descriptions, PID and current state.
- Users: Windows sessions and totals over accessible processes.
- Startup apps: Run registry entries and StartupApproved status when available.

Process controls and administrator access
Run as administrator is enabled by default. Windows requests permission at
launch; declining opens a standard-access session without repeating the prompt.
Settings can disable that launch request, or restart the same executable with
administrator rights. No background service or elevated helper is installed.
Some protected Windows processes remain inaccessible even as administrator.
End task and Efficiency mode operate on captured PID + creation-time identities.
Critical processes and self-termination are guarded. Ending a group acts on
its captured members, never every later process sharing its executable name.
Efficiency mode applies Idle priority plus execution EcoQoS; disabling restores
the original priority unless another program changed it in the meantime.
Right-click a Processes/Details row for End task, Efficiency mode, Open file
location or Properties. Destructive actions ask for confirmation in the app.
Per-process disk activity uses Windows process-accounted disk counters, with
two PID + creation-time snapshots and the measured interval. This fixes missed
copy activity through storage filter/cache drivers. These bytes are Windows'
process attribution, not a measurement of physical NAND writes. Pipe/network
I/O is excluded. Native private-layout use is guarded for Windows 11 x64 builds
22000 through 26300 and validated against the current process identity.
If that source is unavailable, physical disk-completion ETW remains a fallback.
TCP/UDP bytes use a private native ETW session. Administrator access is normally
required. Sources are never added together and a zero native reading does not
trigger fallback. Unavailable/lost intervals stay blank and require fresh
baselines. The app only closes the ETW session it created.

Configuration
Preferences use a readable UTF-8 CleanTaskMan.ini beside the executable.
There is no Save button. Settings changes are persisted immediately, including
Reset and the Task Manager replacement toggle. Remembered view choices (CPU
graph mode, selected device, page, sorting and group expansion) save when the
user changes them. Re-selecting an unchanged preference skips redundant writes.
Startup, sensor refresh, hover, resizing, minimizing and ordinary close never
save general preferences. Administrator restart transfers session state in RAM.

Window position, normal size/maximized state and DPI use the separate readable
CleanTaskMan.window.ini. True exit writes it only when placement differs from
that instance's restored baseline. An unchanged exit writes neither file.
A Settings save never changes that baseline or overwrites newer placement.
The two files use separate writers; one cannot replace another's pending job.
Existing geometry from a 0.5.x main INI is retained unchanged as an upgrade
fallback. New placement files are independent of preference revisions; older
sidecars retain their legacy revision check. Invalid placement is ignored.
The folder must permit writes; failures are reported. Keep both INI files with
the executable when updating. The package supplies no INI to overwrite yours.
Use Exit in the tray menu before replacing a resident executable during updates.

New configuration controls
- Real time update speed: High = 500 ms, Normal = 1000 ms, Low = 1500 ms.
  Normal is the default. Expensive hardware inventory retains its slower
  refresh schedule independently of the selected measurement interval.
- Minimize to tray when closing: a compact toggle, off by default. When on,
  Close hides the window with a tray icon. The tray menu provides Open and Exit.
  Starting the same executable again restores its resident window, including
  when the resident session is elevated; it does not start another monitor.
- All metric workers and this app's ETW session stop while hidden/minimized.
  An in-flight Windows discovery call must finish before shutdown completes.
  Restoring paints the interface first and restarts fresh measurement baselines;
  the paused gap is not included in CPU/disk/network rates or invented history.
- Reset to defaults: resets appearance, refresh speed, startup page, geometry
  and other preferences immediately. Reset also removes
  this copy's owned Task Manager redirect. Another program's redirect is kept.
- Replace Task Manager: off by default. The toggle immediately applies the change;
  administrator access is required to write the 64-bit Windows
  IFEO taskmgr.exe Debugger value. Turning it off removes
  only this copy's exact owned value, restoring the Windows launch behavior.
  Existing replacements belonging to another program are preserved.
  This Windows integration necessarily lives in the Windows registry; its
  ownership record and all app preferences stay in the single INI.
  Turn it off before moving/deleting the executable. If relocating the app,
  retain its INI so the recorded previous location can still be reverted.
  Copying an INI never activates a replacement automatically.

Development verification
Native telemetry and private, never-shown UI/lifecycle fixtures cover GPU engine
classification and aggregation, independent bounded histories, unknown values,
geometry migration, immediate Settings writes, unchanged write-free exit,
placement-only session end, isolated replacement/reset, cadence and paused
collectors. Real HKLM Task Manager replacement is not changed by those fixtures.
Native layout PNGs are exported directly from the fixture's own render buffer,
using deterministic test data. No desktop capture or visible app input is used.
They check the normal and small-window layouts, not live compositor behavior.
The user confirmed 0.5.2 fixed the search-field resize transparency and retained
normal typing/selection. This version preserves its separate Direct2D canvas
and constant-alpha-255 native EDIT. Hidden tests verify those properties.
Real tray/Explorer restart, cross-integrity restoration, UAC handoff, active
video encode/decode load and this version's visible resize remain manual checks.

Current boundaries
This is still a development milestone, not a complete Task Manager replacement.
Run new task and service/startup controls remain unconnected. The UI explains
that when such a command is invoked.
Historical App usage, metered-network/tile history, BIOS boot time, and startup
impact are not collected. Startup folders and packaged startup entries are not
part of the current basic Run inventory.
Protected/inaccessible process fields and unsupported driver measurements stay
blank. Group classification uses visible-window/system-directory information;
it does not reproduce Task Manager's full package/service application taxonomy.
CPU Speed prefers the Windows Actual Frequency counter and falls back to base
frequency multiplied by Processor Performance (including boost above 100%).
It is an aggregate Windows reading, not a per-core hardware sensor or a promise
that every logical processor runs at the same clock speed.
No universal CPU-temperature, voltage, or motherboard-fan sensing is included.
PawnIO is deferred; a future explicit "Install extended sensors" feature can
provide optional hardware monitoring without becoming a core dependency.

Shortcuts
Alt+1 through Alt+8: switch pages.
Ctrl+Tab / Ctrl+Shift+Tab: next / previous page.
Ctrl+F: search. Tab and arrow keys: navigation/selection.
Mouse wheel over the Performance device list: scroll the list.
Right-click the CPU graph: Change graph to > Logical processors / Overall.
Shift+wheel: horizontal table scrolling where needed.

Build
PowerShell: .\build.ps1
Optional: .\build.ps1 -ZigPath <path-to-zig.exe> -OutputDirectory build\native
CMakeLists.txt is provided for Visual Studio C++; this candidate was compiled
and checked with the workspace Zig toolchain, not MSVC.
Native tests: .\tests\run.ps1 (uses the workspace-local Zig toolchain).
Settings/lifecycle tests: .\tests\run-settings.ps1.
Hidden rendering fixtures: .\tests\run-ui-refinement.ps1.
Disk/parking regressions: .\tests\build-sensor-refinement.ps1, then run the
administrator-only build\sensor-qa\sensor-tests.exe. Its bounded copy tests
use the test executable's folder. Set CLEANTASKMAN_TEST_VOLUME to an additional
writable directory to run the same copy test on another volume.
Refinement tests: .\tests\build-refinement.ps1, then run
build\refinement\RefinementTests.exe. The elevated test creates bounded local
helper processes, writes a temporary disk workload and downloads a public
Microsoft Learn document for network attribution. QA helpers are not shipped.

QA-only startup options (not enabled in normal launches)
--sensor-delay-ms=60000   Hold all native discovery/collection for 60 seconds.
--page=performance       Open on Performance.
--no-admin               Suppress the startup elevation request for this run.
--exit-after-ms=10000    Close the test window after 10 seconds.
--diagnostics=<path>     Write startup events; timings begin at wWinMain entry
                         and exclude pre-entry Windows loader/launch overhead.
--benchmark-file=<path>  Record entry-to-first-visible-draw submission and close.
These diagnostics do not certify cold-boot or full click-to-display latency.
