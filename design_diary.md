# Design Diary

## 2026-10-04 – Project setup
- Cloned the GitHub repository into CentOS.
- Renamed the local project directory to remoteops.
- Calculated personalisation values for IT24102483.
- Created source files and documentation files.
- Next task: implement and test the basic TCP connection.

## 2026-10-06 — Implementation and validation

The project was developed incrementally, adding features through
separate commits. The Controller sends newline-terminated commands,
and the Agent keeps each authenticated connection open for further
requests. Each TCP response includes the personalised SID:3842 tag.

SYSINFO reads Linux statistics from /proc. The cpu_load field uses
the one-minute load average, while used memory is calculated from
MemTotal minus MemAvailable. LISTPROC reads process names and PIDs
from /proc and returns a bounded, comma-separated response.

EXEC accepts only DATE, UPTIME, DISKFREE, HOSTNAME and WHOAMI.
Each keyword maps to a fixed program command. Commands outside
this whitelist are rejected instead of executing user-supplied
shell text.

A separate thread handles each Controller so that one connection
does not block the others. Authentication and monitoring state
are kept within each session. A test held five authenticated
connections open together and obtained SYSINFO from all five.

For file transfer, PUT sends the header followed immediately by
the declared number of raw bytes. GET returns a header followed
by the exact file bytes. The receive loops avoid consuming bytes
belonging to the following command. Temporary files are renamed
only after the transfer completes. Filenames are restricted to
plain names, and a 10 MiB limit is applied.

UDP monitoring uses an Agent worker and a Controller receiver
thread. An atomic stop flag and thread join manage cleanup.
Monitoring ends on STOP, QUIT or detected TCP disconnect.
The log uses a mutex to prevent concurrent entries from mixing.

Validation confirmed binary and empty-file transfers, oversized
upload rejection, directory traversal rejection and rejection of
EXEC LS. Additional tests sent commands in separate pieces and
several commands together. UDP reception and cleanup after TCP
disconnect also passed.

During testing, Linux commands such as cmp and tail were initially
entered at the remoteops> prompt. These commands needed to run at
the shell prompt after leaving the Controller. This distinction
was corrected before collecting the relevant evidence.

Remaining work at this diary update: assemble the report and
screenshots, complete the reflection, and verify the submission
archive. Current limits include bounded LISTPROC output, a fixed
authentication token, unencrypted traffic and unreliable UDP
delivery.
