#!/bin/sh
set -eu
python3 ../runtime-availability/make-fixtures.py
for scenario in ${AVAILABILITY_REGRESSION_CASES:-fault wheel jointwheel tail amp power index jogstop joghome slowtask atspeed extra sequence}; do
    ini=test.ini
    case "$scenario" in
        fault) ini=fault.ini ;;
        tail) ini=tail.ini ;;
        power) ini=xzc.ini ;;
        joghome) ini=joghome.ini ;;
        slowtask) ini=slowtask.ini ;;
        atspeed) ini=atspeed.ini ;;
        extra)
            # Unsupported ownership must fail configuration, before Machine On.
            if linuxcnc -r extra.ini >extra.log 2>&1; then
                cat extra.log
                echo 'extra-joint configuration unexpectedly accepted' >&2
                exit 1
            fi
            cat extra.log
            grep -q 'JOINT_2: availability configuration rejected' extra.log
            echo 'availability regression extra ok'
            continue ;;
        sequence)
            if linuxcnc -r sequence.ini >sequence.log 2>&1; then
                cat sequence.log
                echo 'availability HOME_SEQUENCE configuration unexpectedly accepted' >&2
                exit 1
            fi
            cat sequence.log
            grep -q 'JOINT_1: HOME_SEQUENCE cannot be used with AVAILABILITY_CONTROL' sequence.log
            echo 'availability regression sequence ok'
            continue ;;
    esac
    AVAILABILITY_REGRESSION_CASE=$scenario linuxcnc -r "$ini"
done
