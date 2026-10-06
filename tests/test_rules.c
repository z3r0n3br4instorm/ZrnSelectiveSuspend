// SPDX-License-Identifier: GPL-2.0-only
/* Unit tests for who may keep a device open across a power-off. */
#include <stdio.h>
#include <string.h>

#include "zssd.h"

struct zssd_config zssd_cfg = { .stop_services = "nvidia-persistenced, other.service" };
static int failures;

static void expect(const char *what, struct holder_facts f, enum holder_verdict want)
{
    const char *why;
    enum holder_verdict got = holder_verdict(&f, &why);

    if (got != want) {
        fprintf(stderr, "FAIL %s: verdict %d, expected %d\n", what, got, want);
        failures++;
    }
    if ((got == HV_BLOCK) != (why[0] != '\0')) {
        fprintf(stderr, "FAIL %s: a refusal needs a reason, and only a refusal\n", what);
        failures++;
    }
}

int main(void)
{
    expect("migratable application", (struct holder_facts){ .registered = true, .migratable = true }, HV_MIGRATE);
    expect("non-migratable application", (struct holder_facts){ .registered = true }, HV_BLOCK);
    expect("outsider, detach or idle timer", (struct holder_facts){ 0 }, HV_BLOCK);
    expect("outsider, detach, wake support", (struct holder_facts){ .wake_support = true }, HV_BLOCK);
    expect("outsider, requested power-off in place", (struct holder_facts){ .freezable = true }, HV_FREEZE);
    expect("display server is never frozen", (struct holder_facts){ .display_server = true, .freezable = true }, HV_BLOCK);
    expect("non-migratable layer application waits frozen", (struct holder_facts){ .registered = true, .freezable = true }, HV_FREEZE);
    expect("listed service is stopped, not frozen", (struct holder_facts){ .listed_service = true, .freezable = true }, HV_STOP);
    expect("listed service", (struct holder_facts){ .listed_service = true }, HV_STOP);
    expect("display server, wake support", (struct holder_facts){ .display_server = true, .wake_support = true }, HV_ALLOW);
    expect("display server, stock driver", (struct holder_facts){ .display_server = true }, HV_BLOCK);
    expect("display server, stock driver, on the console",
           (struct holder_facts){ .display_server = true, .console = true }, HV_ALLOW);

    /* Process names are cut to fifteen characters by the kernel. */
    if (!service_listed("nvidia-persiste") || !service_listed("other.service") || service_listed("Xorg") ||
        service_listed("nvidia") || service_listed("")) {
        fprintf(stderr, "FAIL service_listed\n");
        failures++;
    }
    if (failures)
        return 1;
    puts("rules: ok");
    return 0;
}
