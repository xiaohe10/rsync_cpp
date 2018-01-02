rs_weak_sum_t rs_calc_weak_sum(void const *buf, size_t len);

void rs_calc_md4_sum(void const *buf, size_t buf_len, rs_strong_sum_t *);
void rs_calc_blake2_sum(void const *buf, size_t buf_len, rs_strong_sum_t *);
