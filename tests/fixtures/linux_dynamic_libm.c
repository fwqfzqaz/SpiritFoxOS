/* S5b 夹具：musl 风格的最小 libm.so（SONAME libm.so）。
 * 真实 musl 把数学函数并入 libc.so；这里保留一个独立小库以验证多对象 DT_NEEDED。 */
int sf_libm_marker(void) { return 42; }
