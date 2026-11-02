/* Acceptance tests h) and i) on the host, against the OS's own code (userland/shell/security.c, fx/caps.c):
 *   i) turning warnings off needs the phrase typed exactly AND the wait; afterwards every app's permissions are
 *      still kept and listed, and the gate still lists what an app can do;
 *   h) nothing but the Security page's own button turns them off: no other code in the shell calls it, no action
 *      or tag names it, and Settings is never opened onto that page by anything but a click there.
 *   tests/security_ui_test.sh */
#include "../userland/shell/appgate.h"
#include "../userland/shell/fx/caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    char dir[] = "/tmp/security-test-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    security_set_dir(dir);

    /* an app was allowed some things, and refused one */
    fx_grants g = {0};
    fx_grants_set(&g, "read:~/Notes", true);
    fx_grants_set(&g, "write:~/Notes", true);
    fx_grants_set(&g, "internet", false);
    CHECK(security_save_grants("/root/Apps/Notes.facet", &g), "grants saved");

    /* i) the phrase and the wait, both */
    unsigned long long t0 = 1000000;
    CHECK(security_warnings_on(), "on to start with");
    CHECK(!security_turn_off("I understand", t0, t0 + 20000), "a partial phrase is refused");
    CHECK(!security_turn_off("i understand this puts my computer at risk", t0, t0 + 20000), "the phrase in other case is refused");
    CHECK(!security_turn_off(SECURITY_PHRASE, t0, t0 + 4000), "the right phrase before the wait is refused");
    CHECK(!security_turn_off(SECURITY_PHRASE, t0, t0 + SECURITY_WAIT_MS - 1), "one millisecond early is refused");
    CHECK(security_warnings_on(), "still on after refusals");
    CHECK(security_turn_off(SECURITY_PHRASE, t0, t0 + SECURITY_WAIT_MS), "the phrase after the wait turns them off");
    CHECK(!security_warnings_on(), "off now");
    /* ...and the permission list is still there */
    char apps[8][300];
    int n = security_list_apps(apps, 8);
    CHECK(n == 1 && !strcmp(apps[0], "/root/Apps/Notes.facet"), "the app is still listed (%d)", n);
    fx_grants back;
    CHECK(security_load_grants("/root/Apps/Notes.facet", &back) && back.n == 3, "its permissions are still kept");
    CHECK(fx_grants_get(&back, "internet") == 0 && fx_grants_get(&back, "read:~/Notes") == 1, "and still enforced as decided");
    /* a grants file is never used for another app's path */
    fx_grants other;
    CHECK(!security_load_grants("/root/Apps/Other.facet", &other) && other.n == 0, "another app gets nothing of these");
    /* one app's warnings, the narrower choice */
    security_turn_on();
    security_set_app_quiet("/root/Apps/Notes.facet", true);
    CHECK(security_warnings_on() && security_app_quiet("/root/Apps/Notes.facet") && !security_app_quiet("/root/Apps/Other.facet"),
          "quietening one app leaves the others warned");
    printf(fails ? "%d failed\n" : "i) passed: the phrase and the wait are both needed; permissions stay listed and kept\n", fails);

    /* h) only the Security page's own button can turn them off: the source says so */
    int h = system("cd \"$(dirname \"$0\")\" 2>/dev/null; "
                   "c=$(grep -rln 'security_turn_off' userland/shell --include=*.c | sort | tr '\\n' ' '); "
                   "[ \"$c\" = 'userland/shell/appgate.c userland/shell/security.c ' ] || { echo \"  turn-off called from: $c\"; exit 1; }; "
                   "grep -q 'main_modal(off_draw' userland/shell/appgate.c || exit 1; "
                   "n=$(grep -c 'main_modal(off_draw' userland/shell/appgate.c); [ \"$n\" = 1 ] || exit 1; "
                   "! grep -rn '\"security\\|warnings off\\|security.conf' userland/shell/fm userland/shell/fx userland/shell/actions.c userland/shell/agent.c userland/shell/desktop.c || exit 1; "
                   "grep -q 'strcasecmp(title, \"Security\")' userland/shell/settings.c || exit 1; "
                   "grep -q 'if (!strcmp(current, \"Security\"))' userland/shell/settings.c || exit 1");
    CHECK(h == 0, "h) only the Security page's button reaches the turn-off; no action, tag or design names it; Settings never opens onto it");
    if (h == 0)
        printf("h) passed (in the code): only a click on Settings > Security's own button turns warnings off\n");
    return fails ? 1 : 0;
}
