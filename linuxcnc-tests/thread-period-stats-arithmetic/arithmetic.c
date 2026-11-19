#include <assert.h>
#include <string.h>

#include "hal_period_stats.h"

#define V(name) (stats.value[HAL_PERIOD_STAT_##name])

int main(void)
{
    hal_period_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    V(PERIOD_TARGET) = 1000000;

    assert(!hal_period_stats_record_start(&stats, 1000000));
    assert(V(SAMPLES) == 0);

    assert(hal_period_stats_record_start(&stats, 2100000));
    hal_period_stats_record_execution(&stats, 80000);
    assert(V(PERIOD_LAST) == 1100000);
    assert(V(JITTER) == 100000);
    assert(V(PERIOD_MIN) == 1100000);
    assert(V(PERIOD_MAX) == 1100000);
    assert(V(OUTLIER_Q) == 0);
    assert(V(MISSED) == 0);

    assert(hal_period_stats_record_start(&stats, 3700000));
    hal_period_stats_record_execution(&stats, 120000);
    assert(V(SAMPLES) == 2);
    assert(V(PERIOD_AVG) == 1350000);
    assert(V(JITTER_AVG) == 350000);
    assert(V(JITTER_MAX) == 600000);
    assert(V(OUTLIER_Q) == 1);
    assert(V(OUTLIER_H) == 1);
    assert(V(OUTLIER_AT) == 2);
    assert(V(EXEC_AVG) == 100000);
    assert(V(EXEC_MAX) == 120000);

    assert(hal_period_stats_record_start(&stats, 6800000));
    hal_period_stats_record_execution(&stats, 40000);
    assert(V(SAMPLES) == 3);
    assert(V(PERIOD_LAST) == 3100000);
    assert(V(PERIOD_MIN) == 1100000);
    assert(V(PERIOD_MAX) == 3100000);
    assert(V(JITTER) == 2100000);
    assert(V(PERIOD_AVG) == 1933333);
    assert(V(JITTER_AVG) == 933333);
    assert(V(JITTER_MAX) == 2100000);
    assert(V(OUTLIER_Q) == 2);
    assert(V(OUTLIER_H) == 2);
    assert(V(OUTLIER_AT) == 3);
    assert(V(MISSED) == 1);
    assert(V(EXEC_AVG) == 80000);
    assert(V(EXEC_MAX) == 120000);

    /* A shorter interval lowers the minimum; a quarter-period outlier that
       is below half a period counts only as a quarter outlier. */
    assert(hal_period_stats_record_start(&stats, 7500000));
    hal_period_stats_record_execution(&stats, 40000);
    assert(V(PERIOD_MIN) == 700000);
    assert(V(JITTER) == 300000);
    assert(V(OUTLIER_Q) == 3);
    assert(V(OUTLIER_H) == 2);
    assert(V(OUTLIER_AT) == 4);

    /* A reset clears everything except the target, and the next start only
       begins a new interval. */
    hal_period_stats_clear(&stats);
    assert(V(SAMPLES) == 0);
    assert(V(PERIOD_MAX) == 0);
    assert(V(PERIOD_MIN) == 0);
    assert(V(JITTER_MAX) == 0);
    assert(V(EXEC_MAX) == 0);
    assert(V(OUTLIER_Q) == 0);
    assert(V(MISSED) == 0);
    assert(V(PERIOD_TARGET) == 1000000);
    assert(!hal_period_stats_record_start(&stats, 7800000));
    assert(V(SAMPLES) == 0);

    assert(hal_period_stats_record_start(&stats, 8800000));
    hal_period_stats_record_execution(&stats, 60000);
    assert(V(SAMPLES) == 1);
    assert(V(PERIOD_AVG) == 1000000);
    assert(V(JITTER_AVG) == 0);
    assert(V(EXEC_AVG) == 60000);

    /* Values are 64-bit: counts and times beyond 32 bits are kept. */
    V(SAMPLES) = 4294967295LL;
    assert(hal_period_stats_record_start(&stats, 9800000));
    hal_period_stats_record_execution(&stats, 200000);
    assert(V(SAMPLES) == 4294967296LL);
    assert(V(EXEC_MAX) == 200000);

    assert(hal_period_stats_record_start(&stats, 9800000 + 5000000000LL));
    assert(V(PERIOD_LAST) == 5000000000LL);
    assert(V(PERIOD_MAX) == 5000000000LL);

    /* Counters saturate instead of wrapping. */
    V(SAMPLES) = RTAPI_SINT_MAX;
    V(MISSED) = RTAPI_SINT_MAX;
    assert(hal_period_stats_record_start(&stats, 9800000 + 15000000000LL));
    assert(V(SAMPLES) == RTAPI_SINT_MAX);
    assert(V(MISSED) == RTAPI_SINT_MAX);

    /* Time going backwards is not an interval. */
    assert(!hal_period_stats_record_start(&stats, 1000000));

    return 0;
}
