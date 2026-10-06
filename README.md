# RemoteOps — IE3090

Student ID: IT24102483
Platform: CentOS Stream 10
Language: C using BSD sockets and POSIX threads

## Personalisation

| Item | Value |
|---|---|
| Agent TCP port | 9410 |
| Authentication command | AUTH OPS-2483 |
| Response suffix | SID:3842 |
| Agent storage | ./agentfiles/IT24102483/ |
| Agent log | remoteops_IT24102483.log |
| Source files | agent_483.c, controller_483.c |
| Build file | Makefile_483 |

## Build

Run from the project directory:

```bash
mkdir -p agentfiles/IT24102483
make -f Makefile_483
```

## Run

Terminal 1:

```bash
./agent_483
```

Terminal 2:

```bash
./controller_483
```

The Controller currently connects to 127.0.0.1.
Run both programs on the same machine for this configuration.

## Commands

Authenticate before using other commands:

```text
AUTH OPS-2483
SYSINFO
LISTPROC
EXEC DATE
EXEC UPTIME
EXEC DISKFREE
EXEC HOSTNAME
EXEC WHOAMI
PUT test_483.txt
GET test_483.txt
MONITOR START 10483
MONITOR STOP
QUIT
```

Create the local upload file before starting the Controller.
Downloaded files are saved as downloaded_<filename>.

MONITOR START takes the Controller's UDP receiving port.
The Agent sends system statistics approximately every two seconds.

## Design

The Agent accepts TCP connections and creates a separate thread
for each Controller. Authentication and monitoring state belong
to the individual session.

TCP commands and responses end with a newline. Receive loops
handle commands split across multiple sends and multiple commands
sent together. Send loops handle partial sends.

PUT sends a command header followed immediately by the declared
number of raw file bytes. GET returns a header followed by the
declared number of raw file bytes. Temporary files are renamed
after a complete transfer.

UDP monitoring uses a separate Agent worker and a Controller
receiver thread. Monitoring stops on STOP, QUIT or TCP disconnect.

The Agent appends timestamped session, command and response
events to its log. A mutex protects concurrent log writes.

## Validation evidence

See validation_tests_483.txt and the screenshots included
with the report.

## Limits and implementation choices

- File transfers are limited to 10 MiB.
- Filenames contain only letters, numbers, dots, underscores
  and dashes, and cannot begin with a dot.
- SYSINFO reports the one-minute Linux load average as cpu_load.
- Used memory is calculated from MemTotal minus MemAvailable.
- LISTPROC returns entries that fit within its 8000-byte response.
- EXEC supports only the five fixed commands listed above.
- UDP delivery is not guaranteed.
- This implementation uses a fixed assignment authentication
  token and does not encrypt network traffic.
- Unexpected TCP disconnect is detected when socket I/O reports
  closure or an error; network outages can take longer to detect.

## AI assistance

AI assistance was used for explanations, code generation,
incremental patches and validation commands. Details are recorded
in prompt_log.md. Test evidence comes from actual execution on
the CentOS environment.
