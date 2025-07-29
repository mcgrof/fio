/*
 * Mock test for multi-metric steady state detection
 *
 * This test validates that steady state detection works correctly when
 * tracking multiple metrics (IOPS, BW, LAT) simultaneously.
 *
 * Test scenarios:
 * 1. All metrics reach steady state at the same time
 * 2. Metrics reach steady state at different times
 * 3. One metric fails to reach steady state
 * 4. Edge cases with buffer wraparound
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "../lib/tap.h"

/* Mock structures matching FIO's implementation */
enum ss_metric_type {
	SS_METRIC_IOPS = 0,
	SS_METRIC_BW,
	SS_METRIC_LAT,
	SS_METRIC_NR
};

struct ss_metric_tracker {
	uint64_t sum_y;
	uint64_t sum_xy;
	uint64_t oldest_y;
	double slope;
	double deviation;
	double criterion;
	bool attained;
};

struct mock_steadystate {
	struct ss_metric_tracker trackers[SS_METRIC_NR];
	uint32_t active_metrics;
	double limit;
	int intervals;
	bool all_attained;
};

/* Mock active metric flags */
#define FIO_SS_ACTIVE_IOPS	(1 << SS_METRIC_IOPS)
#define FIO_SS_ACTIVE_BW	(1 << SS_METRIC_BW)
#define FIO_SS_ACTIVE_LAT	(1 << SS_METRIC_LAT)

/* Helper to calculate deviation for a data set */
static double calculate_deviation(uint64_t *data, int count)
{
	double sum = 0;
	double mean;
	double max_dev = 0;
	int i;

	for (i = 0; i < count; i++)
		sum += data[i];

	mean = sum / count;

	for (i = 0; i < count; i++) {
		double diff = fabs(data[i] - mean);
		if (diff > max_dev)
			max_dev = diff;
	}

	return max_dev;
}

/* Helper to check if all active metrics are attained */
static bool check_all_metrics_attained(struct mock_steadystate *ss)
{
	int type;

	for (type = 0; type < SS_METRIC_NR; type++) {
		uint32_t flag = 1 << type;
		if ((ss->active_metrics & flag) && !ss->trackers[type].attained)
			return false;
	}

	return true;
}

/* Test 1: All metrics reach steady state together */
static void test_all_metrics_steady_together(void)
{
	tap_diag("Testing all metrics reaching steady state together");

	struct mock_steadystate ss = {
		.active_metrics = FIO_SS_ACTIVE_IOPS | FIO_SS_ACTIVE_BW | FIO_SS_ACTIVE_LAT,
		.limit = 5.0,	/* 5% deviation limit */
		.intervals = 10
	};

	/* Simulate stable data for all metrics */
	uint64_t stable_iops[] = {1000, 1010, 995, 1005, 998, 1002, 999, 1001, 1003, 997};
	uint64_t stable_bw[] = {10485760, 10500000, 10470000, 10490000, 10480000,
				10485000, 10487000, 10483000, 10486000, 10484000};
	uint64_t stable_lat[] = {50000, 50100, 49900, 50050, 49950, 50020, 49980, 50010, 49990, 50000};

	/* Calculate deviations */
	double iops_dev = calculate_deviation(stable_iops, ss.intervals);
	double bw_dev = calculate_deviation(stable_bw, ss.intervals);
	double lat_dev = calculate_deviation(stable_lat, ss.intervals);

	/* Convert to percentage */
	double iops_mean = 1000.0;
	double bw_mean = 10485760.0;
	double lat_mean = 50000.0;

	ss.trackers[SS_METRIC_IOPS].criterion = 100.0 * iops_dev / iops_mean;
	ss.trackers[SS_METRIC_BW].criterion = 100.0 * bw_dev / bw_mean;
	ss.trackers[SS_METRIC_LAT].criterion = 100.0 * lat_dev / lat_mean;

	/* Check if each metric meets the limit */
	ss.trackers[SS_METRIC_IOPS].attained = ss.trackers[SS_METRIC_IOPS].criterion < ss.limit;
	ss.trackers[SS_METRIC_BW].attained = ss.trackers[SS_METRIC_BW].criterion < ss.limit;
	ss.trackers[SS_METRIC_LAT].attained = ss.trackers[SS_METRIC_LAT].criterion < ss.limit;

	/* All should be attained */
	tap_ok(ss.trackers[SS_METRIC_IOPS].attained,
	       "IOPS steady (deviation: %.2f%% < %.2f%%)",
	       ss.trackers[SS_METRIC_IOPS].criterion, ss.limit);
	tap_ok(ss.trackers[SS_METRIC_BW].attained,
	       "BW steady (deviation: %.2f%% < %.2f%%)",
	       ss.trackers[SS_METRIC_BW].criterion, ss.limit);
	tap_ok(ss.trackers[SS_METRIC_LAT].attained,
	       "LAT steady (deviation: %.2f%% < %.2f%%)",
	       ss.trackers[SS_METRIC_LAT].criterion, ss.limit);

	ss.all_attained = check_all_metrics_attained(&ss);
	tap_ok(ss.all_attained, "All metrics reached steady state together");
}

/* Test 2: Metrics reach steady state at different times */
static void test_metrics_steady_different_times(void)
{
	tap_diag("Testing metrics reaching steady state at different times");

	struct mock_steadystate ss = {
		.active_metrics = FIO_SS_ACTIVE_IOPS | FIO_SS_ACTIVE_BW,
		.limit = 3.0,	/* 3% deviation limit */
		.intervals = 10
	};

	/* IOPS becomes stable early */
	uint64_t iops_data[] = {1000, 1001, 999, 1000, 1002, 998, 1001, 999, 1000, 1001};

	/* BW stabilizes later */
	uint64_t bw_data[] = {8000000, 9000000, 10000000, 10500000, 10480000,
			      10490000, 10485000, 10487000, 10483000, 10486000};

	/* Check at interval 5 (first half) */
	double iops_dev_5 = calculate_deviation(iops_data, 5);
	double bw_dev_5 = calculate_deviation(bw_data, 5);

	double iops_crit_5 = 100.0 * iops_dev_5 / 1000.0;
	double bw_crit_5 = 100.0 * bw_dev_5 / 9000000.0;

	tap_ok(iops_crit_5 < ss.limit,
	       "IOPS steady early (deviation: %.2f%% < %.2f%%)", iops_crit_5, ss.limit);
	tap_ok(bw_crit_5 > ss.limit,
	       "BW not steady early (deviation: %.2f%% > %.2f%%)", bw_crit_5, ss.limit);

	/* Check at interval 10 (full data) */
	double iops_dev_10 = calculate_deviation(iops_data, 10);
	double bw_dev_10 = calculate_deviation(bw_data + 5, 5);  /* Last 5 values */

	double iops_crit_10 = 100.0 * iops_dev_10 / 1000.0;
	double bw_crit_10 = 100.0 * bw_dev_10 / 10485000.0;

	tap_ok(iops_crit_10 < ss.limit,
	       "IOPS still steady (deviation: %.2f%% < %.2f%%)", iops_crit_10, ss.limit);
	tap_ok(bw_crit_10 < ss.limit,
	       "BW steady later (deviation: %.2f%% < %.2f%%)", bw_crit_10, ss.limit);
}

/* Test 3: One metric fails to reach steady state */
static void test_one_metric_fails(void)
{
	tap_diag("Testing one metric failing to reach steady state");

	struct mock_steadystate ss = {
		.active_metrics = FIO_SS_ACTIVE_IOPS | FIO_SS_ACTIVE_BW | FIO_SS_ACTIVE_LAT,
		.limit = 2.0,	/* 2% deviation limit */
		.intervals = 10
	};

	/* IOPS and BW are stable */
	uint64_t stable_iops[] = {1000, 1005, 995, 1000, 998, 1002, 999, 1001, 997, 1003};
	uint64_t stable_bw[] = {10485760, 10500000, 10470000, 10490000, 10480000,
				10485000, 10487000, 10483000, 10486000, 10484000};

	/* LAT is unstable with spikes */
	uint64_t unstable_lat[] = {50000, 51000, 49000, 75000, 48000, 52000, 80000, 47000, 53000, 49000};

	/* Calculate criteria */
	ss.trackers[SS_METRIC_IOPS].criterion = 100.0 * calculate_deviation(stable_iops, ss.intervals) / 1000.0;
	ss.trackers[SS_METRIC_BW].criterion = 100.0 * calculate_deviation(stable_bw, ss.intervals) / 10485760.0;
	ss.trackers[SS_METRIC_LAT].criterion = 100.0 * calculate_deviation(unstable_lat, ss.intervals) / 55000.0;

	/* Check attainment */
	ss.trackers[SS_METRIC_IOPS].attained = ss.trackers[SS_METRIC_IOPS].criterion < ss.limit;
	ss.trackers[SS_METRIC_BW].attained = ss.trackers[SS_METRIC_BW].criterion < ss.limit;
	ss.trackers[SS_METRIC_LAT].attained = ss.trackers[SS_METRIC_LAT].criterion < ss.limit;

	tap_ok(ss.trackers[SS_METRIC_IOPS].attained, "IOPS steady");
	tap_ok(ss.trackers[SS_METRIC_BW].attained, "BW steady");
	tap_ok(!ss.trackers[SS_METRIC_LAT].attained,
	       "LAT not steady (deviation: %.2f%% > %.2f%%)",
	       ss.trackers[SS_METRIC_LAT].criterion, ss.limit);

	ss.all_attained = check_all_metrics_attained(&ss);
	tap_ok(!ss.all_attained, "Overall steady state NOT attained due to LAT");
}

/* Test 4: Multi-metric with only two metrics active */
static void test_two_metrics_active(void)
{
	tap_diag("Testing with only two metrics active");

	struct mock_steadystate ss = {
		.active_metrics = FIO_SS_ACTIVE_IOPS | FIO_SS_ACTIVE_LAT,  /* No BW */
		.limit = 4.0,
		.intervals = 8
	};

	uint64_t iops[] = {5000, 5050, 4950, 5025, 4975, 5010, 4990, 5000};
	uint64_t lat[] = {20000, 20100, 19900, 20050, 19950, 20025, 19975, 20000};

	ss.trackers[SS_METRIC_IOPS].criterion = 100.0 * calculate_deviation(iops, ss.intervals) / 5000.0;
	ss.trackers[SS_METRIC_LAT].criterion = 100.0 * calculate_deviation(lat, ss.intervals) / 20000.0;

	ss.trackers[SS_METRIC_IOPS].attained = ss.trackers[SS_METRIC_IOPS].criterion < ss.limit;
	ss.trackers[SS_METRIC_LAT].attained = ss.trackers[SS_METRIC_LAT].criterion < ss.limit;

	/* BW should not affect the result since it's not active */
	ss.trackers[SS_METRIC_BW].attained = false;

	tap_ok(ss.trackers[SS_METRIC_IOPS].attained, "IOPS steady");
	tap_ok(ss.trackers[SS_METRIC_LAT].attained, "LAT steady");

	ss.all_attained = check_all_metrics_attained(&ss);
	tap_ok(ss.all_attained, "Both active metrics reached steady state");
}

/* Test 5: Buffer wraparound handling */
static void test_buffer_wraparound(void)
{
	tap_diag("Testing buffer wraparound with multi-metric");

	/* Simulate a longer test with wraparound */
	int buffer_size = 10;
	int total_samples = 25;
	uint64_t iops_buffer[10] = {0};
	uint64_t bw_buffer[10] = {0};
	int head = 0;
	int tail = 0;

	/* Fill buffer with increasing stability */
	for (int i = 0; i < total_samples; i++) {
		/* Early samples are unstable */
		if (i < 10) {
			iops_buffer[tail] = 1000 + (rand() % 200 - 100);
			bw_buffer[tail] = 10000000 + (rand() % 2000000 - 1000000);
		} else {
			/* Later samples stabilize */
			iops_buffer[tail] = 1000 + (rand() % 20 - 10);
			bw_buffer[tail] = 10485760 + (rand() % 20000 - 10000);
		}

		tail = (tail + 1) % buffer_size;
		if (tail == head)
			head = (head + 1) % buffer_size;
	}

	/* Calculate deviation on the wrapped buffer */
	uint64_t recent_iops[10];
	uint64_t recent_bw[10];
	for (int i = 0; i < buffer_size; i++) {
		int idx = (head + i) % buffer_size;
		recent_iops[i] = iops_buffer[idx];
		recent_bw[i] = bw_buffer[idx];
	}

	double iops_dev = calculate_deviation(recent_iops, buffer_size);
	double bw_dev = calculate_deviation(recent_bw, buffer_size);

	tap_ok(iops_dev < 20.0, "IOPS stabilized after wraparound (deviation: %.2f)", iops_dev);
	tap_ok(bw_dev < 20000.0, "BW stabilized after wraparound (deviation: %.2f)", bw_dev);

	/* Check percentage criteria */
	double iops_pct = 100.0 * iops_dev / 1000.0;
	double bw_pct = 100.0 * bw_dev / 10485760.0;

	tap_ok(iops_pct < 3.0, "IOPS percentage criterion met (%.2f%% < 3%%)", iops_pct);
	tap_ok(bw_pct < 3.0, "BW percentage criterion met (%.2f%% < 3%%)", bw_pct);
}

int main(void)
{
	tap_init();
	tap_plan(19);

	tap_diag("=== FIO Multi-Metric Steady State Mock Test ===");
	tap_diag("Testing simultaneous tracking of multiple metrics");

	test_all_metrics_steady_together();
	test_metrics_steady_different_times();
	test_one_metric_fails();
	test_two_metrics_active();
	test_buffer_wraparound();

	return tap_done();
}
