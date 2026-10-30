/*
 * test_import_map.c - import map parsing and specifier resolution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_runner.h"
#include "../src/import_map.h"

void run_import_map_tests(void);

/* Resolves spec from base and compares with want (NULL: no mapping). */
static bool resolves_to(const char *spec, const char *base, const char *want) {
    char *got = import_map_resolve(spec, base);
    bool ok = want ? (got && strcmp(got, want) == 0) : got == NULL;
    if (!ok) printf("    %s from %s: got %s, want %s\n", spec, base, got ? got : "(none)", want ? want : "(none)");
    free(got);
    return ok;
}

TEST(test_import_map_bare_and_prefix) {
    const char *page = "https://example.com/app/index.html";
    const char *map =
        "{\"imports\": {"
        "  \"react\": \"https://cdn.example/react-1.js\","
        "  \"react/jsx-runtime\": \"/assets/jsx\\u002Djs.js\","
        "  \"lib/\": \"./vendor/lib/\","
        "  \"/old/\": \"/new/\","
        "  \"bad/\": \"/no-slash\","
        "  \"num\": 3"
        "}, \"integrity\": {}}";
    import_map_reset();
    import_map_add(map, strlen(map), page);
    ASSERT_TRUE(resolves_to("react", page, "https://cdn.example/react-1.js"));
    ASSERT_TRUE(resolves_to("react/jsx-runtime", page, "https://example.com/assets/jsx-js.js"));
    ASSERT_TRUE(resolves_to("lib/a/b.js", page, "https://example.com/app/vendor/lib/a/b.js"));
    ASSERT_TRUE(resolves_to("/old/x.js", page, "https://example.com/new/x.js"));
    ASSERT_TRUE(resolves_to("../old/x.js", page, "https://example.com/new/x.js"));
    ASSERT_TRUE(resolves_to("bad/x.js", page, NULL));        /* blocked */
    ASSERT_TRUE(resolves_to("num", page, NULL));
    ASSERT_TRUE(resolves_to("vue", page, NULL));
    ASSERT_TRUE(resolves_to("./other.js", page, NULL));
    /* A second map adds keys but does not override the first's. */
    const char *more = "{\"imports\": {\"react\": \"/r2.js\", \"vue\": \"/vue.js\"}}";
    import_map_add(more, strlen(more), page);
    ASSERT_TRUE(resolves_to("react", page, "https://cdn.example/react-1.js"));
    ASSERT_TRUE(resolves_to("vue", page, "https://example.com/vue.js"));
    import_map_reset();
    ASSERT_TRUE(resolves_to("react", page, NULL));
    return true;
}

TEST(test_import_map_scopes) {
    const char *page = "https://example.com/index.html";
    const char *map =
        "{\"imports\": {\"dep\": \"/dep-v1.js\"},"
        " \"scopes\": {"
        "   \"/legacy/\": {\"dep\": \"/dep-v0.js\"},"
        "   \"/legacy/deep/\": {\"dep\": \"/dep-deep.js\"}"
        " }}";
    import_map_reset();
    import_map_add(map, strlen(map), page);
    ASSERT_TRUE(resolves_to("dep", "https://example.com/main.js", "https://example.com/dep-v1.js"));
    ASSERT_TRUE(resolves_to("dep", "https://example.com/legacy/a.js", "https://example.com/dep-v0.js"));
    ASSERT_TRUE(resolves_to("dep", "https://example.com/legacy/deep/a.js", "https://example.com/dep-deep.js"));
    /* Malformed JSON adds nothing. */
    const char *broken = "{\"imports\": {\"x\": \"/x.js\" ";
    import_map_add(broken, strlen(broken), page);
    ASSERT_TRUE(resolves_to("x", page, NULL));
    import_map_reset();
    return true;
}

void run_import_map_tests(void) {
    printf("\n--- Import Map Tests ---\n");
    RUN_TEST(test_import_map_bare_and_prefix);
    RUN_TEST(test_import_map_scopes);
}
