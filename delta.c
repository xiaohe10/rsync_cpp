#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "librsync.h"
#include "emit.h"
#include "stream.h"
#include "util.h"
#include "sumset.h"
#include "job.h"
#include "trace.h"
#include "rollsum.h"

/**
 * 2002-06-26: Donovan Baarda
 *
 * The following is based entirely on pysync. It is much cleaner than the
 * previous incarnation of this code. It is slightly complicated because in
 * this case the output can block, so the main delta loop needs to stop
 * when this happens.
 *
 * In pysync a 'last' attribute is used to hold the last miss or match for
 * extending if possible. In this code, basis_len and scoop_pos are used
 * instead of 'last'. When basis_len > 0, last is a match. When basis_len =
 * 0 and scoop_pos is > 0, last is a miss. When both are 0, last is None
 * (ie, nothing).
 *
 * Pysync is also slightly different in that a 'flush' method is available
 * to force output of accumulated data. This 'flush' is use to finalise
 * delta calculation. In librsync input is terminated with an eof flag on
 * the input stream. I have structured this code similar to pysync with a
 * seperate flush function that is used when eof is reached. This allows
 * for a flush style API if one is ever needed. Note that flush in pysync
 * can be used for more than just terminating delta calculation, so a flush
 * based API can in some ways be more flexible...
 *
 * The input data is first scanned, then processed. Scanning identifies
 * input data as misses or matches, and emits the instruction stream.
 * Processing the data consumes it off the input scoop and outputs the
 * processed miss data into the tube.
 *
 * The scoop contains all data yet to be processed. The scoop_pos is an
 * index into the scoop that indicates the point scanned to. As data is
 * scanned, scoop_pos is incremented. As data is processed, it is removed
 * from the scoop and scoop_pos adjusted. Everything gets complicated
 * because the tube can block. When the tube is blocked, no data can be
 * processed.
 *
 */

/* used by rdiff, but now redundant */
int rs_roll_paranoia = 0;

static rs_result rs_delta_s_scan(rs_job_t *job);
static rs_result rs_delta_s_flush(rs_job_t *job);
static rs_result rs_delta_s_end(rs_job_t *job);
static inline void rs_getinput(rs_job_t *job);
static inline int rs_findmatch(rs_job_t *job, rs_long_t *match_pos, size_t *match_len);
static inline rs_result rs_appendmatch(rs_job_t *job, rs_long_t match_pos, size_t match_len);
static inline rs_result rs_appendmiss(rs_job_t *job, size_t miss_len);
static inline rs_result rs_appendflush(rs_job_t *job);
static inline rs_result rs_processmatch(rs_job_t *job);
static inline rs_result rs_processmiss(rs_job_t *job);

/**
 * \brief Get a block of data if possible, and see if it matches.
 *
 * On each call, we try to process all of the input data available on the
 * scoop and input buffer. */
static rs_result rs_delta_s_scan(rs_job_t *job)
{
    const size_t   block_len = job->signature->block_len;
    rs_long_t      match_pos;
    size_t         match_len;
    rs_result      result;
    Rollsum        test;

    rs_job_check(job);
    /* read the input into the scoop */
    rs_getinput(job);
    /* output any pending output from the tube */
    result=rs_tube_catchup(job);
    /* while output is not blocked and there is a block of data */
    while ((result==RS_DONE) &&
           ((job->scoop_pos + block_len) < job->scoop_avail)) {
        /* check if this block matches */
        if (rs_findmatch(job,&match_pos,&match_len)) {
            /* append the match and reset the weak_sum */
            result=rs_appendmatch(job,match_pos,match_len);
            RollsumInit(&job->weak_sum);
        } else {
            /* rotate the weak_sum and append the miss byte */
            RollsumRotate(&job->weak_sum, job->scoop_next[job->scoop_pos],
                          job->scoop_next[job->scoop_pos + block_len]);
            result=rs_appendmiss(job,1);
            if (rs_roll_paranoia) {
                RollsumInit(&test);
                RollsumUpdate(&test, job->scoop_next + job->scoop_pos, block_len);
                if (RollsumDigest(&test) != RollsumDigest(&job->weak_sum)) {
                    rs_fatal("mismatch between rolled sum "FMT_WEAKSUM" and check "FMT_WEAKSUM"",
                             RollsumDigest(&job->weak_sum), RollsumDigest(&test));
                }

            }
        }
    }
    /* if we completed OK */
    if (result==RS_DONE) {
        /* if we reached eof, we can flush the last fragment */
        if (job->stream->eof_in) {
            job->statefn=rs_delta_s_flush;
            return RS_RUNNING;
        } else {
            /* we are blocked waiting for more data */
            return RS_BLOCKED;
        }
    }
    return result;
}


static rs_result rs_delta_s_flush(rs_job_t *job)
{
    rs_long_t      match_pos;
    size_t         match_len;
    rs_result      result;

    rs_job_check(job);
    /* read the input into the scoop */
    rs_getinput(job);
    /* output any pending output */
    result=rs_tube_catchup(job);
    /* while output is not blocked and there is any remaining data */
    while ((result==RS_DONE) && (job->scoop_pos < job->scoop_avail)) {
        /* check if this block matches */
        if (rs_findmatch(job,&match_pos,&match_len)) {
            /* append the match and reset the weak_sum */
            result=rs_appendmatch(job,match_pos,match_len);
            RollsumInit(&job->weak_sum);
        } else {
            /* rollout from weak_sum and append the miss byte */
            RollsumRollout(&job->weak_sum,job->scoop_next[job->scoop_pos]);
            rs_trace("block reduced to "FMT_SIZE"", job->weak_sum.count);
            result=rs_appendmiss(job,1);
        }
    }
    /* if we are not blocked, flush and set end statefn. */
    if (result==RS_DONE) {
        result=rs_appendflush(job);
        job->statefn=rs_delta_s_end;
    }
    if (result==RS_DONE) {
        return RS_RUNNING;
    }
    return result;
}


static rs_result rs_delta_s_end(rs_job_t *job)
{
    rs_emit_end_cmd(job);
    return RS_DONE;
}


static inline void rs_getinput(rs_job_t *job) {
    size_t len;

    len=rs_scoop_total_avail(job);
    if (job->scoop_avail < len) {
        rs_scoop_input(job,len);
    }
}


/**
 * find a match at scoop_pos, returning the match_pos and match_len.
 * Note that this will calculate weak_sum if required. It will also
 * determine the match_len.
 *
 * Note that this routine could be modified to do xdelta style matches that
 * would extend matches past block boundaries by matching backwards and
 * forwards beyond the block boundaries. Extending backwards would require
 * decrementing scoop_pos as appropriate.
 */
static inline int rs_findmatch(rs_job_t *job, rs_long_t *match_pos, size_t *match_len) {
    const size_t block_len = job->signature->block_len;

    /* calculate the weak_sum if we don't have one */
    if (job->weak_sum.count == 0) {
        /* set match_len to min(block_len, scan_avail) */
        *match_len=job->scoop_avail - job->scoop_pos;
        if (*match_len > block_len) {
            *match_len = block_len;
        }
        /* Update the weak_sum */
        RollsumUpdate(&job->weak_sum,job->scoop_next+job->scoop_pos,*match_len);
        rs_trace("calculate weak sum from scratch length "FMT_SIZE"",job->weak_sum.count);
    } else {
        /* set the match_len to the weak_sum count */
        *match_len=job->weak_sum.count;
    }
    *match_pos = rs_signature_find_match(job->signature,
					 RollsumDigest(&job->weak_sum),
					 job->scoop_next+job->scoop_pos,
					 *match_len);
    return *match_pos != -1;
}


/**
 * Append a match at match_pos of length match_len to the delta, extending
 * a previous match if possible, or flushing any previous miss/match. */
static inline rs_result rs_appendmatch(rs_job_t *job, rs_long_t match_pos, size_t match_len)
{
    rs_result result=RS_DONE;

    /* if last was a match that can be extended, extend it */
    if (job->basis_len && (job->basis_pos + job->basis_len) == match_pos) {
        job->basis_len+=match_len;
    } else {
        /* else appendflush the last value */
        result=rs_appendflush(job);
        /* make this the new match value */
        job->basis_pos=match_pos;
        job->basis_len=match_len;
    }
    /* increment scoop_pos to point at next unscanned data */
    job->scoop_pos+=match_len;
    /* we can only process from the scoop if output is not blocked */
    if (result==RS_DONE) {
        /* process the match data off the scoop*/
        result=rs_processmatch(job);
    }
    return result;
}


/**
 * Append a miss of length miss_len to the delta, extending a previous miss
 * if possible, or flushing any previous match.
 *
 * This also breaks misses up into 4*block_len segments to avoid accumulating
 * too much in memory. */
static inline rs_result rs_appendmiss(rs_job_t *job, size_t miss_len)
{
    const size_t   max_miss = 32768;  /* For 0.01% 3 command bytes overhead. */
    rs_result result=RS_DONE;

    /* If last was a match, or max_miss misses, appendflush it. */
    if (job->basis_len || (job->scoop_pos >= max_miss)) {
        result=rs_appendflush(job);
    }
    /* increment scoop_pos */
    job->scoop_pos+=miss_len;
    return result;
}


/**
 * Flush any accumulating hit or miss, appending it to the delta.
 */
static inline rs_result rs_appendflush(rs_job_t *job)
{
    /* if last is a match, emit it and reset last by resetting basis_len */
    if (job->basis_len) {
        rs_trace("matched "FMT_LONG" bytes at "FMT_LONG"!", job->basis_len, job->basis_pos);
        rs_emit_copy_cmd(job, job->basis_pos, job->basis_len);
        job->basis_len=0;
        return rs_processmatch(job);
    /* else if last is a miss, emit and process it*/
    } else if (job->scoop_pos) {
        rs_trace("got "FMT_SIZE" bytes of literal data", job->scoop_pos);
        rs_emit_literal_cmd(job, job->scoop_pos);
        return rs_processmiss(job);
    }
    /* otherwise, nothing to flush so we are done */
    return RS_DONE;
}


/**
 * The scoop contains match data at scoop_next of length scoop_pos. This
 * function processes that match data, returning RS_DONE if it completes,
 * or RS_BLOCKED if it gets blocked. After it completes scoop_pos is reset
 * to still point at the next unscanned data.
 *
 * This function currently just removes data from the scoop and adjusts
 * scoop_pos appropriately. In the future this could be used for something
 * like context compressing of miss data. Note that it also calls
 * rs_tube_catchup to output any pending output. */
static inline rs_result rs_processmatch(rs_job_t *job)
{
    job->scoop_avail-=job->scoop_pos;
    job->scoop_next+=job->scoop_pos;
    job->scoop_pos=0;
    return rs_tube_catchup(job);
}

/**
 * The scoop contains miss data at scoop_next of length scoop_pos. This
 * function processes that miss data, returning RS_DONE if it completes, or
 * RS_BLOCKED if it gets blocked. After it completes scoop_pos is reset to
 * still point at the next unscanned data.
 *
 * This function uses rs_tube_copy to queue copying from the scoop into
 * output. and uses rs_tube_catchup to do the copying. This automaticly
 * removes data from the scoop, but this can block. While rs_tube_catchup
 * is blocked, scoop_pos does not point at legit data, so scanning can also
 * not proceed.
 *
 * In the future this could do compression of miss data before outputing
 * it. */
static inline rs_result rs_processmiss(rs_job_t *job)
{
    rs_tube_copy(job, job->scoop_pos);
    job->scoop_pos=0;
    return rs_tube_catchup(job);
}


/**
 * \brief State function that does a slack delta containing only
 * literal data to recreate the input.
 */
static rs_result rs_delta_s_slack(rs_job_t *job)
{
    rs_buffers_t * const stream = job->stream;
    size_t avail = stream->avail_in;

    if (avail) {
        rs_trace("emit slack delta for "FMT_SIZE" available bytes", avail);
        rs_emit_literal_cmd(job, avail);
        rs_tube_copy(job, avail);
        return RS_RUNNING;
    } else if (rs_job_input_is_ending(job)) {
        job->statefn = rs_delta_s_end;
        return RS_RUNNING;
    }
    return RS_BLOCKED;
}


/**
 * State function for writing out the header of the encoding job.
 */
static rs_result rs_delta_s_header(rs_job_t *job)
{
    rs_emit_delta_header(job);
    if (job->signature) {
        job->statefn = rs_delta_s_scan;
    } else {
        rs_trace("no signature provided for delta, using slack deltas");
        job->statefn = rs_delta_s_slack;
    }
    return RS_RUNNING;
}


rs_job_t *rs_delta_begin(rs_signature_t *sig)
{
    rs_job_t *job;

    job = rs_job_new("delta", rs_delta_s_header);
    /* Caller can pass NULL sig or empty sig for "slack deltas". */
    if (sig && sig->count > 0) {
        rs_signature_check(sig);
        /* Caller must have called rs_build_hash_table() by now. */
        assert(sig->hashtable);
        job->signature = sig;
        RollsumInit(&job->weak_sum);
    }
    return job;
}
