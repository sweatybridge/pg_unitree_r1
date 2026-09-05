# pg_unitree_r1

[![CI](https://github.com/sweatybridge/pg_unitree_r1/actions/workflows/ci.yml/badge.svg)](https://github.com/sweatybridge/pg_unitree_r1/actions/workflows/ci.yml)

`pg_unitree_r1` embeds a Unitree R1 gateway in a PostgreSQL background worker.
The SQL API is asynchronous: transactions append commands or versioned targets,
and the worker is the only thread that calls PostgreSQL and the synchronous
Unitree client APIs. A separate native thread publishes low-level DDS commands
at 500 Hz and never calls SPI or allocates PostgreSQL objects.

The extension is intended for a dedicated robot appliance, not a shared
general-purpose database server. Low-level motor control can injure people or
damage hardware. Test without payloads, use physical restraints and an
independent emergency stop, and replace the default broad software limits with
limits validated for the exact R1 model and task before enabling torque.

## Architecture

The durable path is:

```text
SQL transaction -> command/target table -> gateway worker -> Unitree SDK/DDS
                                                  |
                                      500 Hz safety/control thread
```

High-level commands are serialized through `unitree_r1.command`. Low-level
sessions have a monotonically increasing generation, and every target has a
monotonic revision plus a short expiry. A stale generation, repeated revision,
non-finite value, value outside the safety envelope, stale robot state, or an
expired target is rejected or faults to a bounded damping sequence. A database
or worker restart marks an interrupted low-level session as faulted; it is never
automatically resumed.

## Build and install

Requirements are Linux on `x86_64` or `aarch64`, PostgreSQL server development
headers, GNU Make, a C++17 compiler, and a Unitree SDK2 checkout. Clone this
repository inside SDK2 so its default `UNITREE_ROOT=..` resolves correctly:

```sh
git clone https://github.com/unitreerobotics/unitree_sdk2.git
git clone https://github.com/sweatybridge/pg_unitree_r1.git \
  unitree_sdk2/pg_unitree_r1
cd unitree_sdk2/pg_unitree_r1
```

Then build from the extension directory:

```sh
make core-test
make -j$(nproc)
sudo make install
```

`UNITREE_ROOT` defaults to the repository root (`..`) and `UNITREE_ARCH`
defaults to `uname -m`. Installation copies the bundled Cyclone DDS libraries
beside the extension module because this Windows checkout stores the `.so.0`
symlinks as text files.

The included Docker build verifies the core, compiles the module, and installs
it into PostgreSQL 17 by default. Run these commands from the SDK2 root because
the SDK is the Docker build context:

```sh
cd ..
docker build -f pg_unitree_r1/Dockerfile -t pg-unitree-r1 .
docker build --build-arg PG_MAJOR=18 \
  -f pg_unitree_r1/Dockerfile -t pg-unitree-r1:pg18 .
```

CI pins the SDK checkout used for reproducible PostgreSQL 17 and 18 builds.

## Releases

Updating `default_version` in `pg_unitree_r1.control` on `main` triggers the
release workflow. The matching install script must exist at
`sql/pg_unitree_r1--VERSION.sql`. The workflow reruns the full PostgreSQL 17 and
18 test matrix, then publishes versioned Linux `x86_64` packages and SHA-256
checksums to a `vVERSION` GitHub release.

Each archive contains the extension module, its Cyclone DDS runtime libraries,
the control file, and the versioned SQL install script under a PostgreSQL-major
specific `usr/` tree. Extract the archive, inspect it, then install it with:

```sh
sudo cp -a pg_unitree_r1-VERSION-pgMAJOR-linux-x86_64/usr/. /usr/
```

Released extension versions are immutable. If the `vVERSION` tag already
exists, the workflow fails instead of replacing its assets.

## Configure PostgreSQL

The worker is static and must be preloaded. Add settings like these to
`postgresql.conf`, then restart PostgreSQL:

```conf
shared_preload_libraries = 'pg_unitree_r1'
pg_unitree_r1.database = 'robot'
pg_unitree_r1.network_interface = 'eth0'
pg_unitree_r1.poll_ms = 10
pg_unitree_r1.sdk_timeout_s = 5
```

`network_interface` is required. If it is empty, the worker deliberately stays
offline and reports `network_interface_required` from `unitree_r1.health()`;
it does not initialize the Unitree SDK.

Create the extension in the configured database:

```sql
CREATE EXTENSION pg_unitree_r1;
SELECT * FROM unitree_r1.health();
```

Robot functions are not executable by `PUBLIC`. Grant schema usage and only
the operations needed by a dedicated application role, for example:

```sql
GRANT USAGE ON SCHEMA unitree_r1 TO robot_operator;
GRANT EXECUTE ON FUNCTION unitree_r1.health() TO robot_operator;
GRANT EXECUTE ON FUNCTION unitree_r1.stop_move(text) TO robot_operator;
```

The extension name retains the conventional `pg_` prefix. Its SQL schema is
`unitree_r1` because PostgreSQL reserves `pg_*` schema names.

## High-level control

Every mutating function returns a durable command ID immediately. Reusing a
`request_key` with the same arguments returns the original command; reusing it
for a different operation is an error.

```sql
SELECT unitree_r1.start('mission-42/start');
SELECT unitree_r1.stand_up('mission-42/stand');
SELECT unitree_r1.move(0.25, 0, 0, 2.0, 'mission-42/move-1');
SELECT unitree_r1.stop_move('mission-42/stop');

SELECT * FROM unitree_r1.command_status(1);
```

Also exposed are `damp`, `zero_torque`, `set_velocity`, and
`set_speed_mode`. A command can finish as `succeeded`, `failed`, `rejected`, or
`ambiguous`; `ambiguous` means a restart or exception made the external side
effect uncertain, so callers should reconcile robot state before retrying.

## Low-level control

Opening a session queues release of the high-level motion service and waits for
a fresh CRC-valid `rt/lowstate` sample:

```sql
SELECT * FROM unitree_r1.low_level_open(
  state_timeout_ms => 100,
  damping_ms => 250,
  request_key => 'controller-a/open'
);
```

Keep the returned `(session_id, generation)` as a fencing token. Once its
prepare command succeeds and session status is `armed`, submit a complete R1
target. Arrays use the 26-joint order listed below.

```sql
SELECT unitree_r1.low_level_set_target(
  session_id => 7,
  generation => 7,
  revision => 1,
  q => array_fill(0::real, ARRAY[26]),
  dq => array_fill(0::real, ARRAY[26]),
  kp => array_fill(0::real, ARRAY[26]),
  kd => array_fill(3::real, ARRAY[26]),
  tau => array_fill(0::real, ARRAY[26]),
  valid_for => interval '100 milliseconds'
);

SELECT * FROM unitree_r1.low_level_target_status(1);
SELECT unitree_r1.low_level_start(7, 7, 'controller-a/start');
```

The application must continue publishing increasing revisions before each
target expires. PostgreSQL is not in the 500 Hz loop; it supplies held,
time-bounded setpoints. Stop explicitly when finished:

```sql
SELECT unitree_r1.low_level_stop(7, 7, 'controller-a/stop');
```

R1 array order:

```text
left hip pitch, roll, yaw, knee, ankle pitch, ankle roll,
right hip pitch, roll, yaw, knee, ankle pitch, ankle roll,
waist roll, waist yaw,
left shoulder pitch, roll, yaw, elbow, wrist roll,
right shoulder pitch, roll, yaw, elbow, wrist roll,
head pitch, head yaw
```

The corresponding DDS motor indices are
`0..13, 15..19, 22..26, 29, 30`, matching the repository R1 examples.

## Operations

Use these read-only functions instead of reading internal tables directly:

```sql
SELECT * FROM unitree_r1.health();
SELECT * FROM unitree_r1.command_status(123);
SELECT * FROM unitree_r1.low_level_session_status(7);
SELECT * FROM unitree_r1.low_level_target_status(91);
SELECT * FROM unitree_r1.telemetry();
```

The worker logs structured decision events (`event=...`) but never logs at the
control rate. Persistent status includes heartbeat age, active generation,
control ticks, deadline misses, CRC errors, publish failures, and the last fault.

## Current safety envelope

The testable native core rejects absolute joint position over 12.6 rad,
velocity over 30 rad/s, `kp` over 500, `kd` over 50, and feed-forward torque
over 120 in SDK units. Gains must be non-negative. These are final sanity bounds,
not per-joint operating limits. A production deployment should add model- and
joint-specific limits before commanding nonzero gains or torque.
