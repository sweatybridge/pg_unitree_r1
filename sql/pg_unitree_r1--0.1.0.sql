CREATE SCHEMA unitree_r1;

CREATE TABLE unitree_r1.low_level_session (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    generation bigint GENERATED ALWAYS AS IDENTITY UNIQUE NOT NULL,
    status text NOT NULL DEFAULT 'preparing'
        CHECK (status IN ('preparing', 'armed', 'active', 'stopping', 'stopped', 'faulted')),
    state_timeout_ms integer NOT NULL DEFAULT 100
        CHECK (state_timeout_ms BETWEEN 20 AND 1000),
    damping_ms integer NOT NULL DEFAULT 250
        CHECK (damping_ms BETWEEN 20 AND 5000),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    armed_at timestamptz,
    started_at timestamptz,
    stopped_at timestamptz,
    last_target_id bigint,
    last_target_revision bigint NOT NULL DEFAULT 0,
    fault_code text,
    fault_detail text
);

CREATE TABLE unitree_r1.command (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    request_key text UNIQUE,
    kind text NOT NULL CHECK (kind IN (
        'start', 'stand_up', 'damp', 'zero_torque', 'stop_move',
        'move', 'set_velocity', 'set_speed_mode',
        'low_prepare', 'low_start', 'low_stop'
    )),
    status text NOT NULL DEFAULT 'queued'
        CHECK (status IN ('queued', 'running', 'succeeded', 'failed', 'rejected', 'ambiguous')),
    session_id bigint REFERENCES unitree_r1.low_level_session(id),
    vx real,
    vy real,
    yaw real,
    duration_s real,
    integer_arg integer,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    started_at timestamptz,
    completed_at timestamptz,
    attempt integer NOT NULL DEFAULT 0,
    sdk_code integer,
    result text,
    error_code text,
    error_detail text
);

CREATE INDEX command_claim_idx
    ON unitree_r1.command (id) WHERE status = 'queued';

CREATE TABLE unitree_r1.low_level_target (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    session_id bigint NOT NULL REFERENCES unitree_r1.low_level_session(id),
    generation bigint NOT NULL,
    revision bigint NOT NULL CHECK (revision > 0),
    q real[] NOT NULL CHECK (cardinality(q) = 26),
    dq real[] NOT NULL CHECK (cardinality(dq) = 26),
    kp real[] NOT NULL CHECK (cardinality(kp) = 26),
    kd real[] NOT NULL CHECK (cardinality(kd) = 26),
    tau real[] NOT NULL CHECK (cardinality(tau) = 26),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    valid_until timestamptz NOT NULL,
    status text NOT NULL DEFAULT 'queued'
        CHECK (status IN ('queued', 'accepted', 'rejected', 'expired')),
    error_code text,
    UNIQUE (session_id, generation, revision)
);

CREATE INDEX low_level_target_pending_idx
    ON unitree_r1.low_level_target (session_id, id)
    WHERE status = 'queued';

CREATE TABLE unitree_r1.gateway_status (
    singleton boolean PRIMARY KEY DEFAULT true CHECK (singleton),
    worker_pid integer,
    state text NOT NULL DEFAULT 'offline'
        CHECK (state IN ('offline', 'starting', 'ready', 'high_level',
                         'low_level_armed', 'low_level_active', 'faulted')),
    network_interface text,
    started_at timestamptz,
    heartbeat_at timestamptz,
    last_state_at timestamptz,
    active_session_id bigint,
    active_generation bigint,
    control_rate_hz integer NOT NULL DEFAULT 500,
    control_ticks bigint NOT NULL DEFAULT 0,
    deadline_misses bigint NOT NULL DEFAULT 0,
    crc_errors bigint NOT NULL DEFAULT 0,
    publish_failures bigint NOT NULL DEFAULT 0,
    last_error_code text,
    last_error_detail text,
    last_error_at timestamptz
);

INSERT INTO unitree_r1.gateway_status(singleton) VALUES (true);

CREATE TABLE unitree_r1.telemetry_latest (
    singleton boolean PRIMARY KEY DEFAULT true CHECK (singleton),
    observed_at timestamptz NOT NULL,
    session_id bigint,
    generation bigint,
    robot_tick bigint,
    mode_machine integer,
    q real[] NOT NULL CHECK (cardinality(q) = 26),
    dq real[] NOT NULL CHECK (cardinality(dq) = 26)
);

CREATE FUNCTION unitree_r1._enqueue(
    _kind text,
    _request_key text DEFAULT NULL,
    _session_id bigint DEFAULT NULL,
    _vx real DEFAULT NULL,
    _vy real DEFAULT NULL,
    _yaw real DEFAULT NULL,
    _duration_s real DEFAULT NULL,
    _integer_arg integer DEFAULT NULL
) RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    command_id bigint;
    same_request boolean;
BEGIN
    INSERT INTO unitree_r1.command(
        request_key, kind, session_id, vx, vy, yaw, duration_s, integer_arg
    ) VALUES (
        _request_key, _kind, _session_id, _vx, _vy, _yaw, _duration_s, _integer_arg
    )
    ON CONFLICT (request_key) DO NOTHING
    RETURNING id INTO command_id;

    IF command_id IS NULL THEN
        SELECT c.id,
               c.kind = _kind
               AND c.session_id IS NOT DISTINCT FROM _session_id
               AND c.vx IS NOT DISTINCT FROM _vx
               AND c.vy IS NOT DISTINCT FROM _vy
               AND c.yaw IS NOT DISTINCT FROM _yaw
               AND c.duration_s IS NOT DISTINCT FROM _duration_s
               AND c.integer_arg IS NOT DISTINCT FROM _integer_arg
          INTO command_id, same_request
          FROM unitree_r1.command c
         WHERE c.request_key = _request_key;
        IF NOT same_request THEN
            RAISE EXCEPTION 'request_key already names a different command'
                USING ERRCODE = '22000';
        END IF;
    END IF;
    RETURN command_id;
END
$$;

CREATE FUNCTION unitree_r1.start(request_key text DEFAULT NULL)
RETURNS bigint LANGUAGE sql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$ SELECT unitree_r1._enqueue('start', request_key) $$;

CREATE FUNCTION unitree_r1.stand_up(request_key text DEFAULT NULL)
RETURNS bigint LANGUAGE sql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$ SELECT unitree_r1._enqueue('stand_up', request_key) $$;

CREATE FUNCTION unitree_r1.damp(request_key text DEFAULT NULL)
RETURNS bigint LANGUAGE sql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$ SELECT unitree_r1._enqueue('damp', request_key) $$;

CREATE FUNCTION unitree_r1.zero_torque(request_key text DEFAULT NULL)
RETURNS bigint LANGUAGE sql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$ SELECT unitree_r1._enqueue('zero_torque', request_key) $$;

CREATE FUNCTION unitree_r1.stop_move(request_key text DEFAULT NULL)
RETURNS bigint LANGUAGE sql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$ SELECT unitree_r1._enqueue('stop_move', request_key) $$;

CREATE FUNCTION unitree_r1.move(
    vx real,
    vy real,
    yaw real,
    duration_s real DEFAULT 1.0,
    request_key text DEFAULT NULL
) RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    IF vx IS NULL OR vy IS NULL OR yaw IS NULL OR duration_s IS NULL OR
       vx < -3 OR vx > 3 OR vy < -2 OR vy > 2 OR yaw < -4 OR yaw > 4 OR
       duration_s <= 0 OR duration_s > 864000 THEN
        RAISE EXCEPTION 'move arguments exceed the extension safety envelope'
            USING ERRCODE = '22023';
    END IF;
    RETURN unitree_r1._enqueue(
        'move', request_key, NULL, vx, vy, yaw, duration_s, NULL
    );
END
$$;

CREATE FUNCTION unitree_r1.set_velocity(
    vx real,
    vy real,
    yaw real,
    duration_s real DEFAULT 1.0,
    request_key text DEFAULT NULL
) RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    IF vx IS NULL OR vy IS NULL OR yaw IS NULL OR duration_s IS NULL OR
       vx < -3 OR vx > 3 OR vy < -2 OR vy > 2 OR yaw < -4 OR yaw > 4 OR
       duration_s <= 0 OR duration_s > 864000 THEN
        RAISE EXCEPTION 'velocity arguments exceed the extension safety envelope'
            USING ERRCODE = '22023';
    END IF;
    RETURN unitree_r1._enqueue(
        'set_velocity', request_key, NULL, vx, vy, yaw, duration_s, NULL
    );
END
$$;

CREATE FUNCTION unitree_r1.set_speed_mode(
    speed_mode integer,
    request_key text DEFAULT NULL
) RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    IF speed_mode IS NULL OR speed_mode NOT BETWEEN 0 AND 3 THEN
        RAISE EXCEPTION 'speed_mode must be between 0 and 3'
            USING ERRCODE = '22023';
    END IF;
    RETURN unitree_r1._enqueue(
        'set_speed_mode', request_key, NULL, NULL, NULL, NULL, NULL, speed_mode
    );
END
$$;

CREATE FUNCTION unitree_r1.low_level_open(
    state_timeout_ms integer DEFAULT 100,
    damping_ms integer DEFAULT 250,
    request_key text DEFAULT NULL
) RETURNS TABLE(session_id bigint, generation bigint, command_id bigint)
LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    same_request boolean;
BEGIN
    IF state_timeout_ms IS NULL OR state_timeout_ms NOT BETWEEN 20 AND 1000 OR
       damping_ms IS NULL OR damping_ms NOT BETWEEN 20 AND 5000 THEN
        RAISE EXCEPTION 'invalid low-level watchdog configuration'
            USING ERRCODE = '22023';
    END IF;
    IF low_level_open.request_key IS NOT NULL THEN
        SELECT c.session_id, s.generation, c.id,
               c.kind = 'low_prepare'
               AND s.state_timeout_ms = low_level_open.state_timeout_ms
               AND s.damping_ms = low_level_open.damping_ms
          INTO session_id, generation, command_id, same_request
          FROM unitree_r1.command c
          LEFT JOIN unitree_r1.low_level_session s ON s.id = c.session_id
         WHERE c.request_key = low_level_open.request_key;
        IF FOUND THEN
            IF same_request IS DISTINCT FROM true THEN
                RAISE EXCEPTION 'request_key already names a different low-level session'
                    USING ERRCODE = '22000';
            END IF;
            RETURN NEXT;
            RETURN;
        END IF;
    END IF;

    INSERT INTO unitree_r1.low_level_session(
        state_timeout_ms, damping_ms
    ) VALUES (
        state_timeout_ms, damping_ms
    )
    RETURNING id, low_level_session.generation
        INTO session_id, generation;

    command_id := unitree_r1._enqueue(
        'low_prepare', request_key, session_id
    );
    RETURN NEXT;
END
$$;

CREATE FUNCTION unitree_r1.low_level_start(
    session_id bigint,
    generation bigint,
    request_key text DEFAULT NULL
) RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    existing_id bigint;
    same_request boolean;
BEGIN
    IF low_level_start.request_key IS NOT NULL THEN
        SELECT c.id,
               c.kind = 'low_start'
               AND c.session_id = low_level_start.session_id
          INTO existing_id, same_request
          FROM unitree_r1.command c
         WHERE c.request_key = low_level_start.request_key;
        IF FOUND THEN
            IF same_request IS DISTINCT FROM true THEN
                RAISE EXCEPTION 'request_key already names a different command'
                    USING ERRCODE = '22000';
            END IF;
            RETURN existing_id;
        END IF;
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM unitree_r1.low_level_session s
        WHERE s.id = low_level_start.session_id
          AND s.generation = low_level_start.generation
          AND s.status = 'armed'
    ) THEN
        RAISE EXCEPTION 'low-level session is not armed or generation is stale'
            USING ERRCODE = '55000';
    END IF;
    RETURN unitree_r1._enqueue(
        'low_start', request_key, low_level_start.session_id
    );
END
$$;

CREATE FUNCTION unitree_r1.low_level_stop(
    session_id bigint,
    generation bigint,
    request_key text DEFAULT NULL
) RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    existing_id bigint;
    same_request boolean;
BEGIN
    IF low_level_stop.request_key IS NOT NULL THEN
        SELECT c.id,
               c.kind = 'low_stop'
               AND c.session_id = low_level_stop.session_id
          INTO existing_id, same_request
          FROM unitree_r1.command c
         WHERE c.request_key = low_level_stop.request_key;
        IF FOUND THEN
            IF same_request IS DISTINCT FROM true THEN
                RAISE EXCEPTION 'request_key already names a different command'
                    USING ERRCODE = '22000';
            END IF;
            RETURN existing_id;
        END IF;
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM unitree_r1.low_level_session s
        WHERE s.id = low_level_stop.session_id
          AND s.generation = low_level_stop.generation
          AND s.status IN ('armed', 'active', 'faulted')
    ) THEN
        RAISE EXCEPTION 'low-level session is not stoppable or generation is stale'
            USING ERRCODE = '55000';
    END IF;
    RETURN unitree_r1._enqueue(
        'low_stop', request_key, low_level_stop.session_id
    );
END
$$;

CREATE FUNCTION unitree_r1.low_level_set_target(
    session_id bigint,
    generation bigint,
    revision bigint,
    q real[],
    dq real[],
    kp real[],
    kd real[],
    tau real[],
    valid_for interval DEFAULT interval '100 milliseconds'
) RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    target_id bigint;
    same_payload boolean;
BEGIN
    IF q IS NULL OR dq IS NULL OR kp IS NULL OR kd IS NULL OR tau IS NULL OR
       array_ndims(q) <> 1 OR array_ndims(dq) <> 1 OR
       array_ndims(kp) <> 1 OR array_ndims(kd) <> 1 OR
       array_ndims(tau) <> 1 OR
       cardinality(q) <> 26 OR cardinality(dq) <> 26 OR
       cardinality(kp) <> 26 OR cardinality(kd) <> 26 OR
       cardinality(tau) <> 26 THEN
        RAISE EXCEPTION 'each low-level vector must contain exactly 26 values'
            USING ERRCODE = '22023';
    END IF;
    IF revision <= 0 THEN
        RAISE EXCEPTION 'revision must be positive' USING ERRCODE = '22023';
    END IF;
    IF valid_for IS NULL OR valid_for < interval '10 milliseconds' OR
       valid_for > interval '1 second' THEN
        RAISE EXCEPTION 'valid_for must be between 10 milliseconds and 1 second'
            USING ERRCODE = '22023';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM unitree_r1.low_level_session s
        WHERE s.id = low_level_set_target.session_id
          AND s.generation = low_level_set_target.generation
          AND s.status IN ('armed', 'active')
    ) THEN
        RAISE EXCEPTION 'low-level session is not accepting targets or generation is stale'
            USING ERRCODE = '55000';
    END IF;

    INSERT INTO unitree_r1.low_level_target(
        session_id, generation, revision, q, dq, kp, kd, tau, valid_until
    ) VALUES (
        low_level_set_target.session_id,
        low_level_set_target.generation,
        low_level_set_target.revision,
        low_level_set_target.q,
        low_level_set_target.dq,
        low_level_set_target.kp,
        low_level_set_target.kd,
        low_level_set_target.tau,
        clock_timestamp() + low_level_set_target.valid_for
    )
    ON CONFLICT ON CONSTRAINT low_level_target_session_id_generation_revision_key
    DO NOTHING
    RETURNING id INTO target_id;

    IF target_id IS NULL THEN
        SELECT t.id,
               t.q = low_level_set_target.q
               AND t.dq = low_level_set_target.dq
               AND t.kp = low_level_set_target.kp
               AND t.kd = low_level_set_target.kd
               AND t.tau = low_level_set_target.tau
          INTO target_id, same_payload
          FROM unitree_r1.low_level_target t
         WHERE t.session_id = low_level_set_target.session_id
           AND t.generation = low_level_set_target.generation
           AND t.revision = low_level_set_target.revision;
        IF NOT same_payload THEN
            RAISE EXCEPTION 'revision already exists with a different payload'
                USING ERRCODE = '22000';
        END IF;
    END IF;
    RETURN target_id;
END
$$;

CREATE FUNCTION unitree_r1.command_status(command_id bigint)
RETURNS TABLE(
    id bigint,
    kind text,
    status text,
    sdk_code integer,
    result text,
    error_code text,
    error_detail text,
    created_at timestamptz,
    started_at timestamptz,
    completed_at timestamptz
) LANGUAGE sql STABLE SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT c.id, c.kind, c.status, c.sdk_code, c.result,
           c.error_code, c.error_detail, c.created_at, c.started_at, c.completed_at
      FROM unitree_r1.command c
     WHERE c.id = command_id
$$;

CREATE FUNCTION unitree_r1.low_level_session_status(session_id bigint)
RETURNS TABLE(
    id bigint,
    generation bigint,
    status text,
    created_at timestamptz,
    armed_at timestamptz,
    started_at timestamptz,
    stopped_at timestamptz,
    last_target_revision bigint,
    fault_code text,
    fault_detail text
) LANGUAGE sql STABLE SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT s.id, s.generation, s.status, s.created_at, s.armed_at,
           s.started_at, s.stopped_at, s.last_target_revision,
           s.fault_code, s.fault_detail
      FROM unitree_r1.low_level_session s
     WHERE s.id = session_id
$$;

CREATE FUNCTION unitree_r1.low_level_target_status(target_id bigint)
RETURNS TABLE(
    id bigint,
    session_id bigint,
    generation bigint,
    revision bigint,
    status text,
    created_at timestamptz,
    valid_until timestamptz,
    error_code text
) LANGUAGE sql STABLE SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT t.id, t.session_id, t.generation, t.revision, t.status,
           t.created_at, t.valid_until, t.error_code
      FROM unitree_r1.low_level_target t
     WHERE t.id = target_id
$$;

CREATE FUNCTION unitree_r1.telemetry()
RETURNS TABLE(
    observed_at timestamptz,
    session_id bigint,
    generation bigint,
    robot_tick bigint,
    mode_machine integer,
    q real[],
    dq real[]
) LANGUAGE sql STABLE SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT t.observed_at, t.session_id, t.generation, t.robot_tick,
           t.mode_machine, t.q, t.dq
      FROM unitree_r1.telemetry_latest t
     WHERE t.singleton
$$;

CREATE FUNCTION unitree_r1.health()
RETURNS TABLE(
    state text,
    healthy boolean,
    worker_pid integer,
    heartbeat_at timestamptz,
    heartbeat_age interval,
    last_state_at timestamptz,
    active_session_id bigint,
    active_generation bigint,
    control_ticks bigint,
    deadline_misses bigint,
    crc_errors bigint,
    publish_failures bigint,
    last_error_code text,
    last_error_detail text
) LANGUAGE sql STABLE SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT g.state,
           g.state NOT IN ('offline', 'faulted')
             AND g.heartbeat_at > clock_timestamp() - interval '5 seconds',
           g.worker_pid,
           g.heartbeat_at,
           clock_timestamp() - g.heartbeat_at,
           g.last_state_at,
           g.active_session_id,
           g.active_generation,
           g.control_ticks,
           g.deadline_misses,
           g.crc_errors,
           g.publish_failures,
           g.last_error_code,
           g.last_error_detail
      FROM unitree_r1.gateway_status g
     WHERE g.singleton
$$;

REVOKE ALL ON ALL TABLES IN SCHEMA unitree_r1 FROM PUBLIC;
REVOKE ALL ON ALL SEQUENCES IN SCHEMA unitree_r1 FROM PUBLIC;
REVOKE ALL ON ALL FUNCTIONS IN SCHEMA unitree_r1 FROM PUBLIC;
