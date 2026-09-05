CREATE EXTENSION pg_unitree_r1;

DO $$
BEGIN
    IF EXISTS (
        SELECT 1
          FROM pg_class c
          JOIN pg_namespace n ON n.oid = c.relnamespace
          CROSS JOIN LATERAL aclexplode(
              COALESCE(c.relacl, acldefault('r', c.relowner))
          ) a
         WHERE n.nspname = 'unitree_r1'
           AND c.relname = 'command'
           AND a.grantee = 0
           AND a.privilege_type = 'SELECT'
    ) THEN
        RAISE EXCEPTION 'internal command table is readable by PUBLIC';
    END IF;
    IF EXISTS (
        SELECT 1
          FROM pg_proc p
          JOIN pg_namespace n ON n.oid = p.pronamespace
          CROSS JOIN LATERAL aclexplode(
              COALESCE(p.proacl, acldefault('f', p.proowner))
          ) a
         WHERE n.nspname = 'unitree_r1'
           AND a.grantee = 0
           AND a.privilege_type = 'EXECUTE'
    ) THEN
        RAISE EXCEPTION 'robot function is executable by PUBLIC';
    END IF;
END
$$;

DO $$
DECLARE
    first_id bigint;
    repeated_id bigint;
    opened record;
    reopened record;
    first_target_id bigint;
    repeated_target_id bigint;
    start_command_id bigint;
    stop_command_id bigint;
    zeros real[] := array_fill(0::real, ARRAY[26]);
    ones real[] := array_fill(1::real, ARRAY[26]);
BEGIN
    first_id := unitree_r1.start('contract/dedupe');
    repeated_id := unitree_r1.start('contract/dedupe');
    IF first_id <> repeated_id THEN
        RAISE EXCEPTION 'request-key dedupe failed';
    END IF;
    BEGIN
        PERFORM unitree_r1.damp('contract/dedupe');
        RAISE EXCEPTION 'request key was reused for a different command';
    EXCEPTION WHEN data_exception THEN
        NULL;
    END;

    SELECT * INTO opened FROM unitree_r1.low_level_open(
        state_timeout_ms => 100,
        damping_ms => 250,
        request_key => 'contract/low-open'
    );
    IF opened.session_id IS NULL OR opened.generation IS NULL OR
       opened.command_id IS NULL THEN
        RAISE EXCEPTION 'low_level_open did not return a complete handle';
    END IF;
    SELECT * INTO reopened FROM unitree_r1.low_level_open(
        state_timeout_ms => 100,
        damping_ms => 250,
        request_key => 'contract/low-open'
    );
    IF reopened.session_id <> opened.session_id OR
       reopened.generation <> opened.generation OR
       reopened.command_id <> opened.command_id THEN
        RAISE EXCEPTION 'low_level_open dedupe returned a different handle';
    END IF;
    BEGIN
        PERFORM * FROM unitree_r1.low_level_open(
            state_timeout_ms => 200,
            damping_ms => 250,
            request_key => 'contract/low-open'
        );
        RAISE EXCEPTION 'low_level_open accepted changed retry arguments';
    EXCEPTION WHEN data_exception THEN
        NULL;
    END;

    UPDATE unitree_r1.low_level_session
       SET status = 'armed'
     WHERE id = opened.session_id;

    first_target_id := unitree_r1.low_level_set_target(
        opened.session_id, opened.generation, 1,
        zeros, zeros, zeros, zeros, zeros,
        interval '100 milliseconds'
    );
    repeated_target_id := unitree_r1.low_level_set_target(
        opened.session_id, opened.generation, 1,
        zeros, zeros, zeros, zeros, zeros,
        interval '100 milliseconds'
    );
    IF repeated_target_id <> first_target_id THEN
        RAISE EXCEPTION 'target revision dedupe failed';
    END IF;
    BEGIN
        PERFORM unitree_r1.low_level_set_target(
            opened.session_id, opened.generation, 1,
            ones, zeros, zeros, zeros, zeros,
            interval '100 milliseconds'
        );
        RAISE EXCEPTION 'target revision accepted a different payload';
    EXCEPTION WHEN data_exception THEN
        NULL;
    END;

    BEGIN
        PERFORM unitree_r1.low_level_set_target(
            opened.session_id, opened.generation + 1, 2,
            zeros, zeros, zeros, zeros, zeros,
            interval '100 milliseconds'
        );
        RAISE EXCEPTION 'stale generation was accepted';
    EXCEPTION WHEN object_not_in_prerequisite_state THEN
        NULL;
    END;

    BEGIN
        PERFORM unitree_r1.low_level_set_target(
            opened.session_id, opened.generation, 2,
            ARRAY[0::real], zeros, zeros, zeros, zeros,
            interval '100 milliseconds'
        );
        RAISE EXCEPTION 'short actuator vector was accepted';
    EXCEPTION WHEN invalid_parameter_value THEN
        NULL;
    END;

    start_command_id := unitree_r1.low_level_start(
        opened.session_id, opened.generation, 'contract/low-start'
    );
    UPDATE unitree_r1.low_level_session
       SET status = 'active'
     WHERE id = opened.session_id;
    IF unitree_r1.low_level_start(
           opened.session_id, opened.generation, 'contract/low-start'
       ) <> start_command_id THEN
        RAISE EXCEPTION 'low_level_start retry changed command id';
    END IF;

    stop_command_id := unitree_r1.low_level_stop(
        opened.session_id, opened.generation, 'contract/low-stop'
    );
    UPDATE unitree_r1.low_level_session
       SET status = 'stopped'
     WHERE id = opened.session_id;
    IF unitree_r1.low_level_stop(
           opened.session_id, opened.generation, 'contract/low-stop'
       ) <> stop_command_id THEN
        RAISE EXCEPTION 'low_level_stop retry changed command id';
    END IF;
END
$$;

SELECT kind, status FROM unitree_r1.command_status(
    (SELECT id FROM unitree_r1.command WHERE request_key = 'contract/dedupe')
);

SELECT state, healthy FROM unitree_r1.health();

DROP EXTENSION pg_unitree_r1 CASCADE;
