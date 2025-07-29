#include <stdlib.h>

#include "fio.h"
#include "steadystate.h"

bool steadystate_enabled = false;
unsigned int ss_check_interval = 1000;

/*
 * Helper function to determine active metric type from state flags
 */
static enum ss_metric_type get_active_metric_type(struct steadystate_data *ss)
{
	if (ss->state & FIO_SS_IOPS)
		return SS_METRIC_IOPS;
	else if (ss->state & FIO_SS_BW)
		return SS_METRIC_BW;
	else
		return SS_METRIC_LAT;
}

/*
 * Helper function to get metric value based on state flags
 */
static uint64_t get_metric_value(struct steadystate_data *ss, int index)
{
	enum ss_metric_type type = get_active_metric_type(ss);

	switch (type) {
	case SS_METRIC_IOPS:
		return ss->iops_data[index];
	case SS_METRIC_BW:
		return ss->bw_data[index];
	case SS_METRIC_LAT:
		return ss->lat_data[index];
	default:
		return 0;
	}
}

/*
 * Helper function to get current metric value based on state flags
 */
static uint64_t get_current_metric(struct steadystate_data *ss, uint64_t iops, uint64_t bw, uint64_t lat)
{
	enum ss_metric_type type = get_active_metric_type(ss);

	switch (type) {
	case SS_METRIC_IOPS:
		return iops;
	case SS_METRIC_BW:
		return bw;
	case SS_METRIC_LAT:
		return lat;
	default:
		return 0;
	}
}

/*
 * Helper function for circular buffer index calculation
 */
static inline int ss_buffer_index(struct steadystate_data *ss, int offset, int intervals)
{
	return (ss->head + offset) % intervals;
}

/*
 * Helper function to increment circular buffer index
 */
static inline int ss_buffer_next(int index, int intervals)
{
	return (index + 1) % intervals;
}

/*
 * Helper function to get the active metric tracker
 */
static struct ss_metric_tracker *get_active_tracker(struct steadystate_data *ss)
{
	enum ss_metric_type type = get_active_metric_type(ss);
	return &ss->trackers[type];
}

void steadystate_free(struct thread_data *td)
{
	free(td->ss.iops_data);
	free(td->ss.bw_data);
	free(td->ss.lat_data);
	td->ss.iops_data = NULL;
	td->ss.bw_data = NULL;
	td->ss.lat_data = NULL;
}

static void steadystate_alloc(struct thread_data *td)
{
	int intervals = td->ss.dur / (ss_check_interval / 1000L);

	td->ss.bw_data = calloc(intervals, sizeof(uint64_t));
	td->ss.iops_data = calloc(intervals, sizeof(uint64_t));
	td->ss.lat_data = calloc(intervals, sizeof(uint64_t));

	td->ss.state |= FIO_SS_DATA;
}

void steadystate_setup(void)
{
	struct thread_data *prev_td;
	int prev_groupid;

	if (!steadystate_enabled)
		return;

	/*
	 * if group reporting is enabled, identify the last td
	 * for each group and use it for storing steady state
	 * data
	 */
	prev_groupid = -1;
	prev_td = NULL;
	for_each_td(td) {
		if (!td->ss.dur)
			continue;

		if (!td->o.group_reporting) {
			steadystate_alloc(td);
			continue;
		}

		if (prev_groupid != td->groupid) {
			if (prev_td)
				steadystate_alloc(prev_td);
			prev_groupid = td->groupid;
		}
		prev_td = td;
	} end_for_each();

	if (prev_td && prev_td->o.group_reporting)
		steadystate_alloc(prev_td);
}

static bool steadystate_slope(uint64_t iops, uint64_t bw, uint64_t lat,
			      struct thread_data *td)
{
	int i, j;
	double result;
	struct steadystate_data *ss = &td->ss;
	uint64_t new_val;
	int intervals = ss->dur / (ss_check_interval / 1000L);

	ss->bw_data[ss->tail] = bw;
	ss->iops_data[ss->tail] = iops;
	ss->lat_data[ss->tail] = lat;

	new_val = get_current_metric(ss, iops, bw, lat);
	struct ss_metric_tracker *tracker = get_active_tracker(ss);

	if (ss->state & FIO_SS_BUFFER_FULL || ss->tail - ss->head == intervals - 1) {
		if (!(ss->state & FIO_SS_BUFFER_FULL)) {
			/* first time through */
			for (i = 0, ss->sum_y = 0; i < intervals; i++) {
				ss->sum_y += get_metric_value(ss, i);
				j = ss_buffer_index(ss, i, intervals);
				ss->sum_xy += i * get_metric_value(ss, j);
			}
			/* Update tracker too */
			tracker->sum_y = ss->sum_y;
			tracker->sum_xy = ss->sum_xy;
			ss->state |= FIO_SS_BUFFER_FULL;
		} else {		/* easy to update the sums */
			ss->sum_y -= ss->oldest_y;
			ss->sum_y += new_val;
			ss->sum_xy = ss->sum_xy - ss->sum_y + intervals * new_val;
			/* Update tracker too */
			tracker->sum_y = ss->sum_y;
			tracker->sum_xy = ss->sum_xy;
		}

		ss->oldest_y = get_metric_value(ss, ss->head);
		tracker->oldest_y = ss->oldest_y;

		/*
		 * calculate slope as (sum_xy - sum_x * sum_y / n) / (sum_(x^2)
		 * - (sum_x)^2 / n) This code assumes that all x values are
		 * equally spaced when they are often off by a few milliseconds.
		 * This assumption greatly simplifies the calculations.
		 */
		ss->slope = (ss->sum_xy - (double) ss->sum_x * ss->sum_y / intervals) /
				(ss->sum_x_sq - (double) ss->sum_x * ss->sum_x / intervals);
		if (ss->state & FIO_SS_PCT)
			ss->criterion = 100.0 * ss->slope / (ss->sum_y / intervals);
		else
			ss->criterion = ss->slope;

		/* Update tracker too */
		tracker->slope = ss->slope;
		tracker->criterion = ss->criterion;

		dprint(FD_STEADYSTATE, "sum_y: %llu, sum_xy: %llu, slope: %f, "
					"criterion: %f, limit: %f\n",
					(unsigned long long) ss->sum_y,
					(unsigned long long) ss->sum_xy,
					ss->slope, ss->criterion, ss->limit);

		result = ss->criterion * (ss->criterion < 0.0 ? -1.0 : 1.0);
		if (result < ss->limit)
			return true;
	}

	ss->tail = ss_buffer_next(ss->tail, intervals);
	if (ss->tail <= ss->head)
		ss->head = ss_buffer_next(ss->head, intervals);

	return false;
}

static bool steadystate_deviation(uint64_t iops, uint64_t bw, uint64_t lat,
				  struct thread_data *td)
{
	int i;
	double diff;
	double mean;

	struct steadystate_data *ss = &td->ss;
	int intervals = ss->dur / (ss_check_interval / 1000L);

	ss->bw_data[ss->tail] = bw;
	ss->iops_data[ss->tail] = iops;
	ss->lat_data[ss->tail] = lat;

	struct ss_metric_tracker *tracker = get_active_tracker(ss);

	if (ss->state & FIO_SS_BUFFER_FULL || ss->tail - ss->head == intervals  - 1) {
		if (!(ss->state & FIO_SS_BUFFER_FULL)) {
			/* first time through */
			for (i = 0, ss->sum_y = 0; i < intervals; i++) {
				ss->sum_y += get_metric_value(ss, i);
			}
			tracker->sum_y = ss->sum_y;
			ss->state |= FIO_SS_BUFFER_FULL;
		} else {		/* easy to update the sum */
			ss->sum_y -= ss->oldest_y;
			ss->sum_y += get_metric_value(ss, ss->tail);
			tracker->sum_y = ss->sum_y;
		}

		ss->oldest_y = get_metric_value(ss, ss->head);
		tracker->oldest_y = ss->oldest_y;

		mean = (double) ss->sum_y / intervals;
		ss->deviation = 0.0;

		for (i = 0; i < intervals; i++) {
			diff = get_metric_value(ss, i) - mean;
			ss->deviation = max(ss->deviation, diff * (diff < 0.0 ? -1.0 : 1.0));
		}

		if (ss->state & FIO_SS_PCT)
			ss->criterion = 100.0 * ss->deviation / mean;
		else
			ss->criterion = ss->deviation;

		/* Update tracker too */
		tracker->deviation = ss->deviation;
		tracker->criterion = ss->criterion;

		dprint(FD_STEADYSTATE, "intervals: %d, sum_y: %llu, mean: %f, max diff: %f, "
					"objective: %f, limit: %f\n",
					intervals,
					(unsigned long long) ss->sum_y, mean,
					ss->deviation, ss->criterion, ss->limit);

		if (ss->criterion < ss->limit)
			return true;
	}

	ss->tail = ss_buffer_next(ss->tail, intervals);
	if (ss->tail == ss->head)
		ss->head = ss_buffer_next(ss->head, intervals);

	return false;
}

int steadystate_check(void)
{
	int  ddir, prev_groupid, group_ramp_time_over = 0;
	unsigned long rate_time;
	struct timespec now;
	uint64_t group_bw = 0, group_iops = 0;
	uint64_t group_lat_sum = 0;
	uint64_t group_lat_samples = 0;
	uint64_t td_iops, td_bytes;
	uint64_t group_lat;
	bool ret;

	prev_groupid = -1;
	for_each_td(td) {
		const bool needs_lock = td_async_processing(td);
		struct steadystate_data *ss = &td->ss;
		uint64_t td_lat_sum = 0;
		uint64_t td_lat_samples = 0;

		if (!ss->dur || td->runstate <= TD_SETTING_UP ||
		    td->runstate >= TD_EXITED || !ss->state ||
		    ss->state & FIO_SS_ATTAINED)
			continue;

		td_iops = 0;
		td_bytes = 0;
		if (!td->o.group_reporting ||
		    (td->o.group_reporting && td->groupid != prev_groupid)) {
			group_bw = 0;
			group_iops = 0;
			group_lat_sum = 0;
			group_lat_samples = 0;
			group_ramp_time_over = 0;
		}
		prev_groupid = td->groupid;

		fio_gettime(&now, NULL);
		if (ss->ramp_time && !(ss->state & FIO_SS_RAMP_OVER)) {
			/*
			 * Begin recording data one check interval after ss->ramp_time
			 * has elapsed
			 */
			if (utime_since(&td->epoch, &now) >= (ss->ramp_time + ss_check_interval * 1000L))
				ss->state |= FIO_SS_RAMP_OVER;
		}

		if (needs_lock)
			__td_io_u_lock(td);

		for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
			td_iops += td->io_blocks[ddir];
			td_bytes += td->io_bytes[ddir];
			/* Use double for intermediate calculation to avoid overflow */
			if (td->ts.clat_stat[ddir].samples) {
				double lat_contribution = td->ts.clat_stat[ddir].mean.u.f *
							 (double)td->ts.clat_stat[ddir].samples;
				td_lat_sum += (uint64_t)lat_contribution;
			}
			td_lat_samples += td->ts.clat_stat[ddir].samples;
		}

		if (needs_lock)
			__td_io_u_unlock(td);

		rate_time = mtime_since(&ss->prev_time, &now);
		memcpy(&ss->prev_time, &now, sizeof(now));

		if (ss->state & FIO_SS_RAMP_OVER) {
			group_bw += rate_time * (td_bytes - ss->prev_bytes) /
				(ss_check_interval * ss_check_interval / 1000L);
			group_iops += rate_time * (td_iops - ss->prev_iops) /
				(ss_check_interval * ss_check_interval / 1000L);
			group_lat_sum += td_lat_sum - ss->prev_lat_sum;
			group_lat_samples += td_lat_samples - ss->prev_lat_samples;
			++group_ramp_time_over;
		}
		ss->prev_iops = td_iops;
		ss->prev_bytes = td_bytes;
		ss->prev_lat_sum = td_lat_sum;
		ss->prev_lat_samples = td_lat_samples;

		if (td->o.group_reporting && !(ss->state & FIO_SS_DATA))
			continue;

		/*
		 * Don't begin checking criterion until ss->ramp_time is over
		 * for at least one thread in group
		 */
		if (!group_ramp_time_over)
			continue;

		dprint(FD_STEADYSTATE, "steadystate_check() thread: %d, "
					"groupid: %u, rate_msec: %ld, "
					"iops: %llu, bw: %llu, head: %d, tail: %d\n",
					__td_index, td->groupid, rate_time,
					(unsigned long long) group_iops,
					(unsigned long long) group_bw,
					ss->head, ss->tail);

		group_lat = 0;
		if (group_lat_samples && group_lat_sum) {
			/* Avoid precision loss by keeping calculation in double until final conversion */
			double mean_lat = (double)group_lat_sum / (double)group_lat_samples;
			group_lat = (uint64_t)mean_lat;
		}

		if (ss->check_both) {
			/* SNIA compliance: check both deviation and slope */
			bool dev_met = false, slope_met = false;
			double orig_limit = ss->limit;

			/* Check deviation with its own criterion */
			ss->limit = ss->deviation_criterion;
			dev_met = steadystate_deviation(group_iops, group_bw, group_lat, td);

			/* Check slope with its own criterion */
			ss->limit = ss->slope_criterion;
			slope_met = steadystate_slope(group_iops, group_bw, group_lat, td);

			/* Restore original limit */
			ss->limit = orig_limit;

			/* Both criteria must be met */
			ret = dev_met && slope_met;
		} else if (ss->state & FIO_SS_SLOPE) {
			ret = steadystate_slope(group_iops, group_bw, group_lat, td);
		} else {
			ret = steadystate_deviation(group_iops, group_bw, group_lat, td);
		}

		if (ret) {
			if (td->o.group_reporting) {
				for_each_td(td2) {
					if (td2->groupid == td->groupid) {
						td2->ss.state |= FIO_SS_ATTAINED;
						fio_mark_td_terminate(td2);
					}
				} end_for_each();
			} else {
				ss->state |= FIO_SS_ATTAINED;
				fio_mark_td_terminate(td);
			}
		}
	} end_for_each();
	return 0;
}

int td_steadystate_init(struct thread_data *td)
{
	struct steadystate_data *ss = &td->ss;
	struct thread_options *o = &td->o;
	int intervals;

	memset(ss, 0, sizeof(*ss));

	/* Initialize per-metric trackers */
	for (int i = 0; i < SS_METRIC_NR; i++) {
		ss->trackers[i].sum_y = 0;
		ss->trackers[i].sum_xy = 0;
		ss->trackers[i].oldest_y = 0;
		ss->trackers[i].slope = 0.0;
		ss->trackers[i].deviation = 0.0;
		ss->trackers[i].criterion = 0.0;
		ss->trackers[i].attained = false;
	}

	if (o->ss_dur) {
		steadystate_enabled = true;
		o->ss_dur /= 1000000L;

		/* put all steady state info in one place */
		ss->dur = o->ss_dur;
		ss->limit = o->ss_limit.u.f;
		ss->ramp_time = o->ss_ramp_time;
		ss_check_interval = o->ss_check_interval / 1000L;

		ss->state = o->ss_state;
		if (!td->ss.ramp_time)
			ss->state |= FIO_SS_RAMP_OVER;

		intervals = ss->dur / (ss_check_interval / 1000L);
		ss->sum_x = intervals * (intervals - 1) / 2;
		ss->sum_x_sq = (intervals - 1) * (intervals) * (2*intervals - 1) / 6;
	}

	/* make sure that ss options are consistent within reporting group */
	for_each_td(td2) {
		if (td2->groupid == td->groupid) {
			struct steadystate_data *ss2 = &td2->ss;

			if (ss2->dur != ss->dur ||
			    ss2->limit != ss->limit ||
			    ss2->ramp_time != ss->ramp_time ||
			    ss2->state != ss->state ||
			    ss2->sum_x != ss->sum_x ||
			    ss2->sum_x_sq != ss->sum_x_sq) {
				td_verror(td, EINVAL, "job rejected: steadystate options must be consistent within reporting groups");
				return 1;
			}
		}
	} end_for_each();

	return 0;
}

static uint64_t steadystate_data_mean(uint64_t *data, int ss_dur)
{
	int i;
	uint64_t sum;
	int intervals = ss_dur / (ss_check_interval / 1000L);

	if (!ss_dur)
		return 0;

	for (i = 0, sum = 0; i < intervals; i++)
		sum += data[i];

	return sum / intervals;
}

uint64_t steadystate_bw_mean(struct thread_stat *ts)
{
	return steadystate_data_mean(ts->ss_bw_data, ts->ss_dur);
}

uint64_t steadystate_iops_mean(struct thread_stat *ts)
{
	return steadystate_data_mean(ts->ss_iops_data, ts->ss_dur);
}

uint64_t steadystate_lat_mean(struct thread_stat *ts)
{
	return steadystate_data_mean(ts->ss_lat_data, ts->ss_dur);
}

