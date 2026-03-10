# Observe Command Integration Tests
# Tests the OBSERVE functionality for various commands

start_server {tags {"observe"}} {
    test "OBSERVE GET - Basic functionality" {
        r SELECT 0
        # Set initial value
        r SET basic_k "initial_value"

        # Start observing the key
        set observe_id [r OBSERVE GET basic_k]

        # Verify initial observe response format
        assert_equal [lindex $observe_id 0] "observe"
        assert_equal [lindex $observe_id 1] "fingerprint"
        set fingerprint [lindex $observe_id 2]
        assert_equal [lindex $observe_id 3] "result"
        assert_equal [lindex $observe_id 4] "initial_value"

        # Modify the key in another client
        set rd2 [valkey_deferring_client]
        $rd2 SELECT 0
        $rd2 read
        $rd2 SET basic_k "new_value"
        $rd2 read

        # Should receive notification
        set notification [r read]
        assert_equal [lindex $notification 0] "observe"
        assert_equal [lindex $notification 1] "fingerprint"
        assert_equal [lindex $notification 2] $fingerprint
        assert_equal [lindex $notification 3] "result"
        assert_equal [lindex $notification 4] "new_value"

        # Clean up: exit observe mode
        r UNOBSERVE $fingerprint

        $rd2 close
    }

    test "OBSERVE GET - Multiple clients observing same key" {
        r SELECT 0
        r SET multi_k "value1"

        # Two clients observe the same key
        set rd1 [valkey_deferring_client]
        set rd2 [valkey_deferring_client]

        $rd1 SELECT 0
        $rd1 read
        $rd2 SELECT 0
        $rd2 read

        $rd1 OBSERVE GET multi_k
        set observe1 [$rd1 read]
        set fingerprint1 [lindex $observe1 2]

        $rd2 OBSERVE GET multi_k
        set observe2 [$rd2 read]
        set fingerprint2 [lindex $observe2 2]

        # Same command should have same fingerprint
        assert_equal $fingerprint1 $fingerprint2

        # Modify the key
        r SET multi_k "value2"

        # Both clients should receive notification
        set notif1 [$rd1 read]
        set notif2 [$rd2 read]

        assert_equal [lindex $notif1 4] "value2"
        assert_equal [lindex $notif2 4] "value2"

        $rd1 UNOBSERVE [lindex $notif1 2]
        $rd1 read
        $rd1 close

        $rd2 UNOBSERVE [lindex $notif2 2]
        $rd2 read
        $rd2 close
    }

    test "OBSERVE GET - Different keys have different fingerprints" {
        r SELECT 0
        r SET diff_k1 "value1"
        r SET diff_k2 "value2"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        $rd OBSERVE GET diff_k1
        set observe1 [$rd read]
        set fingerprint1 [lindex $observe1 2]

        $rd OBSERVE GET diff_k2
        set observe2 [$rd read]
        set fingerprint2 [lindex $observe2 2]

        # Different keys should have different fingerprints
        assert {$fingerprint1 ne $fingerprint2}

        $rd UNOBSERVE $fingerprint1 $fingerprint2
        $rd read
        $rd close
    }

    test "OBSERVE ZRANGE - Basic functionality" {
        r SELECT 0
        r ZADD zrng_zset 1 "one" 2 "two" 3 "three"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE ZRANGE zrng_zset 0 -1
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        set initial_result [lindex $observe 4]
        assert_equal $initial_result {one two three}

        # Modify the sorted set
        r ZADD zrng_zset 4 "four"

        # Should receive notification
        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        set new_result [lindex $notif 4]
        assert_equal $new_result {one two three four}

        $rd UNOBSERVE [lindex $notif 2]
        $rd read
        $rd close
    }

    test "OBSERVE ZRANGE - With WITHSCORES" {
        r SELECT 0
        r DEL zws_zset
        r ZADD zws_zset 1 "one" 2 "two"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE ZRANGE zws_zset 0 -1 WITHSCORES
        set observe [$rd read]

        set initial_result [lindex $observe 4]
        assert_equal $initial_result {one 1 two 2}

        # Modify scores
        r ZADD zws_zset 5 "one"

        set notif [$rd read]
        set new_result [lindex $notif 4]
        assert_equal $new_result {two 2 one 5}

        $rd UNOBSERVE [lindex $notif 2]
        $rd read
        $rd close
    }

    test "OBSERVE - Unsubscribe on client disconnect" {
        r SELECT 0
        r SET unsub_k "value1"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET unsub_k
        $rd read

        set info1 [r CLIENT LIST]
        assert_match "*observing=1*" $info1

        $rd close

        after 100
        set info2 [r CLIENT LIST]
        assert_no_match "*observing=1*" $info2
    }

    test "OBSERVE - Database switch isolation" {
        r SELECT 0
        r SET dbsw_k "db0_value"

        r SELECT 1
        r SET dbsw_k "db1_value"

        # Observe key in DB 0
        r SELECT 0
        set rd0 [valkey_deferring_client]
        $rd0 SELECT 0
        $rd0 read
        $rd0 OBSERVE GET dbsw_k
        set k0 [$rd0 read]
        assert_equal [lindex $k0 4] "db0_value"

        # Observe same key in DB 1
        set rd1 [valkey_deferring_client]
        $rd1 SELECT 1
        $rd1 read
        $rd1 OBSERVE GET dbsw_k
        set observe1 [$rd1 read]
        assert_equal [lindex $observe1 4] "db1_value"

        # Modify key in DB 0
        r SELECT 0
        r SET dbsw_k "db0_new"

        # Only DB 0 observe should get notification
        set notif0 [$rd0 read]
        assert_equal [lindex $notif0 4] "db0_new"

        # DB 1 observe should not receive anything
        assert_equal [$rd1 read_timeout 100] {}

        # UNOBSERVE before closing to ensure synchronous cleanup on the server
        $rd0 UNOBSERVE [lindex $k0 2]
        $rd0 read
        $rd0 close

        $rd1 UNOBSERVE [lindex $observe1 2]
        $rd1 read
        $rd1 close

        # Clean up cross-database state
        r SELECT 1
        r DEL dbsw_k
        # Restore r to the default test db (9 in non-singledb mode, 0 in singledb)
        if {$::singledb} {
            r SELECT 0
        } else {
            r SELECT 9
        }
    }

    test "OBSERVE - Debounce mechanism" {
        r SELECT 0
        r SET dbnc_k "value1"

        after 100

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET dbnc_k
        $rd read

        # Make multiple rapid changes
        r SET dbnc_k "value2"
        r SET dbnc_k "value3"
        r SET dbnc_k "value4"

        # With debounce, all rapid changes coalesce into one notification
        set notif [$rd read]
        assert_equal [lindex $notif 4] "value4"
        # Confirm no additional notifications were buffered
        assert_equal [$rd read_timeout 100] {}

        $rd UNOBSERVE [lindex $notif 2]
        $rd read
        $rd close
    }

    test "OBSERVE - Memory cleanup" {
        r SELECT 0
        set clients {}

        for {set i 0} {$i < 100} {incr i} {
            r SET memc_k$i "value$i"
            set rd [valkey_deferring_client]
            $rd SELECT 0
            $rd read
            $rd OBSERVE GET memc_k$i
            set obs [$rd read]
            lappend clients [list $rd [lindex $obs 2]]
        }

        # Close all clients (UNOBSERVE first for synchronous server cleanup)
        foreach pair $clients {
            set rd [lindex $pair 0]
            set fp [lindex $pair 1]
            $rd UNOBSERVE $fp
            $rd read
            $rd close
        }

        after 100
        set info [r INFO memory]

        for {set i 0} {$i < 100} {incr i} {
            r SET memc_k$i "newvalue$i"
        }

    }

    test "OBSERVE - Expired key notification" {
        r SELECT 0
        r SET exp_k "value" EX 1

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        $rd OBSERVE GET exp_k
        set observe [$rd read]
        assert_equal [lindex $observe 0] "observe"
        assert_equal [lindex $observe 4] "value"

        # Wait for key to expire
        after 1100

        # Access expired key to trigger deletion
        r GET exp_k

        # Should receive notification with empty result
        assert_equal [$rd read_timeout 100] {}

        $rd UNOBSERVE [lindex $observe 2]
        $rd read
        $rd close
    }

    test "OBSERVE - Fingerprint consistency" {
        r SELECT 0
        # Test that same command always generates same fingerprint
        set fingerprints {}

        for {set i 0} {$i < 10} {incr i} {
            set rd [valkey_deferring_client]
            $rd SELECT 0
            $rd read
            $rd OBSERVE GET fpc_k
            set observe [$rd read]
            lappend fingerprints [lindex $observe 2]
            $rd UNOBSERVE [lindex $observe 2]
            $rd read
            $rd close
        }

        # All fingerprints should be identical
        set first [lindex $fingerprints 0]
        foreach fp $fingerprints {
            assert_equal $fp $first
        }
    }

    test "OBSERVE - Complex scenario with multiple operations" {
        r SELECT 0

        # Setup initial data
        r SET cx_str "string"
        r LPUSH cx_list "item1" "item2"
        r SADD cx_set "member1" "member2"
        r ZADD cx_zset 1 "z1" 2 "z2"

        set rd1 [valkey_deferring_client]
        set rd2 [valkey_deferring_client]

        $rd1 SELECT 0
        $rd1 read
        $rd2 SELECT 0
        $rd2 read

        $rd1 OBSERVE GET cx_str
        $rd1 read

        $rd2 OBSERVE ZRANGE cx_zset 0 -1
        $rd2 read

        # Perform various operations
        r SET cx_str "modified"
        r ZADD cx_zset 3 "z3"
        r LPUSH cx_list "item3"
        r SADD cx_set "member3"

        # Check notifications
        set notif1 [$rd1 read]
        assert_equal [lindex $notif1 4] "modified"

        set notif2 [$rd2 read]
        assert_equal [lindex $notif2 4] {z1 z2 z3}

        $rd1 UNOBSERVE [lindex $notif1 2]
        $rd1 read
        $rd1 close

        $rd2 UNOBSERVE [lindex $notif2 2]
        $rd2 read
        $rd2 close
    }

    test "OBSERVE - Command restrictions in RESP2" {
        r SELECT 0
        r SET rstr_k1 "value1"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        # Issue OBSERVE GET to enter observing mode
        $rd OBSERVE GET rstr_k1
        set observe [$rd read]
        assert_equal [lindex $observe 0] "observe"

        $rd SET rstr_k2 "value2"
        assert_error "*Can't execute 'set'*" {$rd read}

        # PING should still work (whitelisted)
        $rd PING
        set pong [$rd read]
        assert_equal $pong "PONG"

        # QUIT should work (whitelisted)
        $rd QUIT
        $rd close
    }

    test "OBSERVE - Whitelisted commands work in observing mode" {
        r SELECT 0
        r SET wl_k1 "value1"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET wl_k1
        $rd read

        # Test PING
        $rd PING
        assert_equal [$rd read] "PONG"

        # Test another OBSERVE command (should work)
        r SET wl_k2 "value2"
        $rd OBSERVE GET wl_k2
        set observe2 [$rd read]
        assert_equal [lindex $observe2 0] "observe"

        # Test RESET (whitelisted)
        $rd RESET
        set reset_result [$rd read]
        assert_equal $reset_result "RESET"

        $rd close
        r DEL wl_k2
    }

    test "UNOBSERVE - Multiple fingerprints" {
        r SELECT 0
        r SET mfp_k1 "value1"
        r SET mfp_k2 "value2"
        r SET mfp_k3 "value3"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        # Subscribe to multiple observe queries
        $rd OBSERVE GET mfp_k1
        set fp1 [lindex [$rd read] 2]

        $rd OBSERVE GET mfp_k2
        set fp2 [lindex [$rd read] 2]

        $rd OBSERVE GET mfp_k3
        set fp3 [lindex [$rd read] 2]

        # Unobserve two fingerprints at once
        $rd UNOBSERVE $fp1 $fp2
        set count [$rd read]
        assert_equal $count 2

        # Still in observe mode because one subscription remains
        $rd SET mfp_k4 "value4"
        assert_error "*Can't execute 'set'*" {$rd read}

        # Unobserve last fingerprint
        $rd UNOBSERVE $fp3
        set count2 [$rd read]
        assert_equal $count2 1

        # Now regular commands should work
        $rd SET mfp_k4 "value4"
        set result2 [$rd read]
        assert_equal $result2 "OK"

        $rd close
    }

    test "UNOBSERVE - Invalid fingerprint" {
        r SELECT 0
        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        # Try to unobserve non-existent fingerprint
        $rd UNOBSERVE "invalidfingerprint123"
        set count [$rd read]
        assert_equal $count 0

        $rd close
    }

    test "OBSERVE - Observing clients skip timeout" {
        r SELECT 0
        set orig_timeout [lindex [r CONFIG GET timeout] 1]
        # Set a short timeout
        r CONFIG SET timeout 1

        r SET to_k "value1"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET to_k
        set obs_timeout [$rd read]

        # Restore timeout so r is not disconnected during the sleep.
        # rd has already been exposed to the 1-second timeout window while
        # observing — if observing clients weren't exempt it would be gone.
        r CONFIG SET timeout $orig_timeout

        # Wait long enough to confirm rd survived the timeout window
        after 2000

        # Client should still be connected (observing clients skip timeout)
        $rd PING
        set pong [$rd read]
        assert_equal $pong "PONG"

        $rd UNOBSERVE [lindex $obs_timeout 2]
        $rd read
        $rd close
    }

    test "OBSERVE - Events received on same connection" {
        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        $rd SET ev_k "initial"
        $rd read

        $rd OBSERVE GET ev_k
        set initial [$rd read]

        assert_equal [lindex $initial 4] "initial"

        $rd PING
        assert_equal [$rd read] "PONG"

        $rd close
    }

    test "OBSERVE connection elevation - no separate connection" {
        r SELECT 0
        # Verify that OBSERVE uses connection elevation (like pubsub)
        # not a separate connection

        r SET elev_k "value"

        # Get initial client count
        set clients_before [llength [split [r CLIENT LIST] "\n"]]

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET elev_k
        set obs_elev [$rd read]

        # Should not create additional connections
        set clients_after [llength [split [r CLIENT LIST] "\n"]]

        # Should have same number of clients (no separate observe connection)
        # Note: +1 for the rd client itself
        assert_equal [expr {$clients_after - $clients_before}] 1

        $rd UNOBSERVE [lindex $obs_elev 2]
        $rd read
        $rd close
    }

    test "OBSERVE GET - Non-existent key returns nil result" {
        r SELECT 0
        r DEL nil_k

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET nil_k
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        assert_equal [lindex $observe 1] "fingerprint"
        assert_equal [lindex $observe 3] "result"
        # Result for non-existent key must be nil/empty
        assert_equal [lindex $observe 4] {}

        $rd UNOBSERVE [lindex $observe 2]
        $rd read
        $rd close
    }

    test "OBSERVE GET - No notification when observed key is deleted" {
        r SELECT 0
        r SET delno_k "value"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET delno_k
        set observe [$rd read]
        set fingerprint [lindex $observe 2]

        # Delete the key — executeObserveCommand returns early when key
        # doesn't exist, so the observer should NOT receive a notification
        r DEL delno_k
        assert_equal [$rd read_timeout 200] {}

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE GET - Fingerprint is a 16-character lowercase hex string" {
        r SELECT 0
        r SET fphex_k "value"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET fphex_k
        set observe [$rd read]
        set fingerprint [lindex $observe 2]

        assert_equal [string length $fingerprint] 16
        assert_match {[0-9a-f]*} $fingerprint

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE GET - Re-subscribing same fingerprint is idempotent" {
        r SELECT 0
        r SET rsub_k "value"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        # Subscribe twice to the same key
        $rd OBSERVE GET rsub_k
        set obs1 [$rd read]
        set fp1 [lindex $obs1 2]

        $rd OBSERVE GET rsub_k
        set obs2 [$rd read]
        set fp2 [lindex $obs2 2]

        # Same fingerprint
        assert_equal $fp1 $fp2

        # Trigger one update
        r SET rsub_k "updated"

        # Should receive exactly one notification (not two)
        set notif [$rd read]
        assert_equal [lindex $notif 4] "updated"
        assert_equal [$rd read_timeout 100] {}

        $rd UNOBSERVE $fp1
        $rd read
        $rd close
    }

    test "OBSERVE GET - INCR and INCRBY trigger observer" {
        r SELECT 0
        r SET incr_k 10

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET incr_k
        set observe [$rd read]
        assert_equal [lindex $observe 4] "10"

        r INCR incr_k
        set notif [$rd read]
        assert_equal [lindex $notif 4] "11"

        r INCRBY incr_k 5
        set notif2 [$rd read]
        assert_equal [lindex $notif2 4] "16"

        $rd UNOBSERVE [lindex $observe 2]
        $rd read
        $rd close
    }

    test "OBSERVE GET - RENAME: old-key observer sees no updates after rename" {
        r SELECT 0
        r SET ren_old_k "value"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET ren_old_k
        set observe [$rd read]
        set fingerprint [lindex $observe 2]

        # Rename ren_old_k → ren_new_k; write to ren_new_k afterward
        r RENAME ren_old_k ren_new_k
        r SET ren_new_k "afterrename"

        # Observer was watching ren_old_k, not ren_new_k — no notification expected
        assert_equal [$rd read_timeout 200] {}

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close

        r DEL ren_new_k
    }

    test "UNOBSERVE - Returns 0 when client has no subscriptions" {
        r SELECT 0
        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        $rd UNOBSERVE "nonexistentfingerprint"
        set count [$rd read]
        assert_equal $count 0

        $rd close
    }

    test "OBSERVE - observing_clients counter increments and decrements correctly" {
        r SELECT 0
        r SET occ_k "value"

        set info_before [r INFO clients]
        regexp {observing_clients:(\d+)} $info_before -> before_count

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GET occ_k
        set obs [$rd read]

        set info_after [r INFO clients]
        regexp {observing_clients:(\d+)} $info_after -> after_count
        assert_equal $after_count [expr {$before_count + 1}]

        $rd UNOBSERVE [lindex $obs 2]
        $rd read

        set info_final [r INFO clients]
        regexp {observing_clients:(\d+)} $info_final -> final_count
        assert_equal $final_count $before_count

        $rd close
    }

    test "OBSERVE - RESP3 mode allows all commands on observing connection" {
        r SELECT 0
        r SET r3_k1 "value"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read

        # Switch to RESP3
        $rd HELLO 3
        $rd read

        # Enter observing mode
        $rd OBSERVE GET r3_k1
        $rd read

        # In RESP3, regular write commands must not be blocked
        $rd SET r3_k2 "value2"
        set result [$rd read]
        assert_equal $result "OK"

        $rd HELLO 2
        $rd read
        $rd close

        r DEL r3_k2
    }

    test "OBSERVE GET - Independent observers for different keys do not cross-notify" {
        r SELECT 0
        r SET ind_k1 "value1"
        r SET ind_k2 "value2"

        set rd1 [valkey_deferring_client]
        set rd2 [valkey_deferring_client]

        $rd1 SELECT 0
        $rd1 read
        $rd2 SELECT 0
        $rd2 read

        $rd1 OBSERVE GET ind_k1
        set obs1 [$rd1 read]

        $rd2 OBSERVE GET ind_k2
        set obs2 [$rd2 read]

        # Modify only ind_k1
        r SET ind_k1 "changed"

        # rd1 should get a notification
        set notif1 [$rd1 read]
        assert_equal [lindex $notif1 4] "changed"

        # rd2 should NOT get a notification
        assert_equal [$rd2 read_timeout 500] {}

        $rd1 UNOBSERVE [lindex $obs1 2]
        $rd1 read
        $rd1 close

        $rd2 UNOBSERVE [lindex $obs2 2]
        $rd2 read
        $rd2 close
    }

    test "OBSERVE ZRANGE - No notification when key is deleted" {
        r SELECT 0
        r ZADD zdno_zset 1 "one" 2 "two"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE ZRANGE zdno_zset 0 -1
        set observe [$rd read]

        r DEL zdno_zset
        assert_equal [$rd read_timeout 200] {}

        $rd UNOBSERVE [lindex $observe 2]
        $rd read
        $rd close
    }

    test "OBSERVE STRLEN - Basic functionality" {
        r SELECT 0
        r SET strlen_k "hello"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE STRLEN strlen_k
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        assert_equal [lindex $observe 4] 5

        r SET strlen_k "hello world"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert_equal [lindex $notif 4] 11

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE HGET - Basic functionality" {
        r SELECT 0
        r HSET hget_h field1 "initial"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE HGET hget_h field1
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        assert_equal [lindex $observe 4] "initial"

        r HSET hget_h field1 "updated"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert_equal [lindex $notif 4] "updated"

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE HGETALL - Reflects all hash field changes" {
        r SELECT 0
        r DEL hga_h
        r HSET hga_h f1 "v1" f2 "v2"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE HGETALL hga_h
        set observe [$rd read]

        set fingerprint [lindex $observe 2]
        set initial [lindex $observe 4]
        assert_equal $initial {f1 v1 f2 v2}

        r HSET hga_h f3 "v3"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert_equal [lindex $notif 4] {f1 v1 f2 v2 f3 v3}

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE LLEN - Basic functionality" {
        r SELECT 0
        r DEL llen_l
        r RPUSH llen_l "a" "b" "c"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE LLEN llen_l
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        assert_equal [lindex $observe 4] 3

        r RPUSH llen_l "d"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert_equal [lindex $notif 4] 4

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE SCARD - Basic functionality" {
        r SELECT 0
        r DEL scard_s
        r SADD scard_s "a" "b" "c"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE SCARD scard_s
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        assert_equal [lindex $observe 4] 3

        r SADD scard_s "d"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert_equal [lindex $notif 4] 4

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE ZSCORE - Basic functionality" {
        r SELECT 0
        r DEL zscore_z
        r ZADD zscore_z 1.5 "member"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE ZSCORE zscore_z member
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        assert_equal [lindex $observe 4] 1.5

        r ZADD zscore_z 9.0 "member"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert_equal [lindex $notif 4] 9

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE BITCOUNT - Basic functionality" {
        r SELECT 0
        r SET bc_k "\xff"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE BITCOUNT bc_k
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        assert_equal [lindex $observe 4] 8

        r SET bc_k "\x0f"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert_equal [lindex $notif 4] 4

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE PFCOUNT - Basic functionality" {
        r SELECT 0
        r DEL pf_k
        r PFADD pf_k "a" "b" "c"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE PFCOUNT pf_k
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        assert {[lindex $observe 4] >= 3}

        r PFADD pf_k "d" "e" "f" "g" "h"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert {[lindex $notif 4] > [lindex $observe 4]}

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE XLEN - Basic functionality" {
        r SELECT 0
        r DEL xlen_s
        r XADD xlen_s "*" field1 value1

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE XLEN xlen_s
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        assert_equal [lindex $observe 4] 1

        r XADD xlen_s "*" field2 value2

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert_equal [lindex $notif 4] 2

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE GEODIST - Basic functionality" {
        r SELECT 0
        r DEL geo_k
        r GEOADD geo_k 13.361389 38.115556 "Palermo"
        r GEOADD geo_k 15.087269 37.502669 "Catania"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE GEODIST geo_k Palermo Catania
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        set initial_dist [lindex $observe 4]
        assert {$initial_dist > 0}

        r GEOADD geo_k 2.349014 48.864716 "Paris"
        r GEOADD geo_k 15.500000 37.000000 "Catania"

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        assert {[lindex $notif 4] != $initial_dist}

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE TTL - Reflects expiry changes" {
        r SELECT 0
        r SET ttl_k "value" EX 100

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        $rd OBSERVE TTL ttl_k
        set observe [$rd read]

        assert_equal [lindex $observe 0] "observe"
        set fingerprint [lindex $observe 2]
        set initial_ttl [lindex $observe 4]
        assert {$initial_ttl > 0 && $initial_ttl <= 100}

        r EXPIRE ttl_k 500

        set notif [$rd read]
        assert_equal [lindex $notif 2] $fingerprint
        set new_ttl [lindex $notif 4]
        assert {$new_ttl > 100 && $new_ttl <= 500}

        $rd UNOBSERVE $fingerprint
        $rd read
        $rd close
    }

    test "OBSERVE GET - Multiple writes emit one notification each" {
        r SELECT 0
        r SET mw_k "v1"

        set rd [valkey_deferring_client]
        $rd SELECT 0
        $rd read
        # Disable debounce for this test
        set prev_debounce [r CONFIG GET observe-debounce-period-ms]
        r CONFIG SET observe-debounce-period-ms 0

        $rd OBSERVE GET mw_k
        $rd read

        r SET mw_k "v2"
        set n1 [$rd read]
        assert_equal [lindex $n1 4] "v2"

        r SET mw_k "v3"
        set n2 [$rd read]
        assert_equal [lindex $n2 4] "v3"

        # Restore debounce setting
        r CONFIG SET observe-debounce-period-ms [lindex $prev_debounce 1]

        $rd UNOBSERVE [lindex $n1 2]
        $rd read
        $rd close
    }
}
