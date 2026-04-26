#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../src/ml/llm.h"
#include "../../src/ml/autocomplete.h"

#if defined(__APPLE__)
#include <sys/qos.h>

extern volatile qos_class_t g_llm_load_qos;
extern volatile qos_class_t g_ac_worker_initial_qos;

static void test_llm_thread_user_interactive(void)
{
    LLM *l = llm_create("fake_model");
    assert(NULL != l);
    llm_wait_ready(l);

    /* load_worker must have run at USER_INTERACTIVE (P-cores). */
    assert(QOS_CLASS_USER_INTERACTIVE == g_llm_load_qos);

    llm_destroy(l);
}

static void test_autocomplete_worker_starts_background(void)
{
    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, NULL);
    assert(NULL != ac);

    /* Give the worker thread time to start and record its initial QoS. */
    usleep(50000);

    /* Worker must start at BACKGROUND (escalates only during LLM inference). */
    assert(QOS_CLASS_BACKGROUND == g_ac_worker_initial_qos);

    autocomplete_destroy(ac);
}

#endif /* __APPLE__ */

int main(void)
{
#if defined(__APPLE__)
    test_llm_thread_user_interactive();
    test_autocomplete_worker_starts_background();
#endif
    printf("test_thread_qos: all passed\n");
    return 0;
}
