#include <stdio.h>
/* S5b 夹具：动态链接 musl 的最小程序。
 * 同时引用 libm.so 中的符号，强制生成对 libm.so 的 DT_NEEDED，用于验证多对象装载。 */
extern int sf_libm_marker(void);
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("MUSL DYNAMIC OK %d\n", sf_libm_marker());
    return 0;
}
