/* Preserve the independent suite and expose only failpoint reachability. */
#define review_calloc original_review_calloc
#define main original_review_main
#include "../review-monitor/test_independent.c"
#undef main
#undef review_calloc
static unsigned calloc_injected_hits;
void *review_calloc(size_t n, size_t size)
{
    if (fail_calloc == 1) ++calloc_injected_hits;
    return original_review_calloc(n, size);
}
int main(int argc, char **argv)
{
    int result = original_review_main(argc, argv);
    printf("PHASE1_CALLOC injected_hits=%u\n", calloc_injected_hits);
    /* Policy-only entry points deliberately do not invoke the OOM scenario. */
    if (argc == 1 && calloc_injected_hits == 0) return 1;
    return result;
}
