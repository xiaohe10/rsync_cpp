void rs_emit_delta_header(rs_job_t *);
void rs_emit_literal_cmd(rs_job_t *, int len);
void rs_emit_end_cmd(rs_job_t *);
void rs_emit_copy_cmd(rs_job_t *job, rs_long_t where, rs_long_t len);
