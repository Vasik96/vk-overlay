// Process filter: name matching incl. the Wine "final name is set late" behaviour.
#include <sys/prctl.h>

#include <cstdio>
#include <cstdlib>

#include "process_filter.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void set_name(const char* n) { prctl(PR_SET_NAME, n, 0, 0, 0); }  // same call Wine uses

int main() {
    using vko::process_allowed;
    using vko::process_name_matches_list;

    // ---- list matching ---------------------------------------------------------------
    CHECK(process_name_matches_list("GTA5.exe", "GTA5.exe"));
    CHECK(process_name_matches_list("gta5.exe", "GTA5.EXE"));                    // case-insensitive
    CHECK(process_name_matches_list("java", "foo, java ,bar"));                  // list, spaces
    CHECK(!process_name_matches_list("javaw", "java"));                          // whole name, not prefix
    CHECK(process_name_matches_list("GeometryDash.ex", "GeometryDash.exe"));      // kernel keeps 15 chars
    CHECK(!process_name_matches_list("PlayGTAV.exe", "GTA5.exe"));
    CHECK(!process_name_matches_list("x", ""));

    ::unsetenv("VKOVERLAY_DISABLE");
    ::unsetenv("VKOVERLAY_PROCESS");

    // ---- no filter: everything except helpers --------------------------------------------
    set_name("cs2");               CHECK(process_allowed(true) && process_allowed(false));
    set_name("java");              CHECK(process_allowed(true));
    set_name("GTA5.exe");          CHECK(process_allowed(true));
    set_name("steamwebhelper");    CHECK(!process_allowed(true));
    set_name("bash");              CHECK(!process_allowed(true));
    set_name("wineserver");        CHECK(!process_allowed(true));

    // ---- filter set: the Proton case ------------------------------------------------------
    ::setenv("VKOVERLAY_PROCESS", "GTA5.exe", 1);
    set_name("wine64-preload");                                   // what a Wine process is called at load time
    CHECK(process_allowed(true));                                 // let through: final name unknown yet
    CHECK(!process_allowed(false));                               // ...but not if it is still that at the first frame
    set_name("GTA5.exe");
    CHECK(process_allowed(true) && process_allowed(false));       // the game itself
    set_name("PlayGTAV.exe");
    CHECK(!process_allowed(false));                               // the launcher is filtered at the first frame
    set_name("steam");
    CHECK(!process_allowed(true));                                // non-Wine non-match is rejected at load
    set_name("java");
    CHECK(!process_allowed(true));

    ::setenv("VKOVERLAY_DISABLE", "1", 1);
    set_name("GTA5.exe");          CHECK(!process_allowed(true));  // explicit kill switch beats everything

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
