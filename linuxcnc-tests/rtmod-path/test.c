#include "config.h"
#include "rtapi/rtmod_path.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch_file(const char *path, const char *value) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
    close(fd);
}

int main(void) {
    rtmod_path_t path = {0};
    char extras[PATH_MAX * 2], *a, *b, *colon;
    char ordered[PATH_MAX * 2], result[PATH_MAX], error[1024];
    char fa[PATH_MAX], fb[PATH_MAX], escaped[PATH_MAX], outside[PATH_MAX];
    const char *name = "rtmod_path_fixture";

    assert(rtmod_path_init(&path, NULL, error, sizeof(error)) == 0);
    assert(path.count == 1 && !strcmp(path.dirs[0], EMC2_RTLIB_DIR));
    rtmod_path_free(&path);
    assert(rtmod_path_init(&path, "", error, sizeof(error)) < 0);
    assert(rtmod_path_init(&path, "relative", error, sizeof(error)) < 0);
    assert(rtmod_path_init(&path, "/tmp::/var", error, sizeof(error)) < 0);
    assert(rtmod_path_init(&path, "/definitely/not/trusted", error, sizeof(error)) < 0);
    assert(!rtmod_name_valid("../bad") && !rtmod_name_valid("bad/name"));

    /* The standard directory: real components resolve there, and the
       protected rtapi/hal_lib lookups never leave it. */
    assert(rtmod_path_init(&path, EMC2_RTLIB_DIR "/", error, sizeof(error)) == 0);
    assert(path.count == 1 && !strcmp(path.dirs[0], EMC2_RTLIB_DIR));
    snprintf(fa, sizeof(fa), "%s/and2%s", EMC2_RTLIB_DIR, MODULE_EXT);
    assert(rtmod_path_resolve(&path, "and2", 0, result, sizeof(result), error, sizeof(error)) == 0);
    assert(!strcmp(result, fa));
    assert(rtmod_path_resolve(&path, "hal_lib", 1, result, sizeof(result), error, sizeof(error)) == 0);
    assert(rtmod_path_resolve(&path, "../and2", 0, result, sizeof(result), error, sizeof(error)) < 0);
    rtmod_path_free(&path);
    {
        char name[64];
        assert(rtmod_path_is_trusted_file(fa, name, sizeof(name)) && !strcmp(name, "and2"));
    }

    strcpy(extras, EMC2_EXTRA_RTLIB_DIRS);
    colon = strchr(extras, ':');
    if (!colon) {
        /* Ordered lookup needs two trusted extra directories
           (--with-extra-rtlib-dirs=DIR_A:DIR_B); this build has fewer. */
        fputs("rtmod-path: ordered lookup checks need two extra rtlib directories; not run\n", stderr);
        puts("rtmod-path: all tests passed");
        return 0;
    }
    *colon = 0; a = extras; b = colon + 1;
    assert(*a && *b && !strchr(b, ':'));
    mkdir(a, 0700); mkdir(b, 0700);
    snprintf(fa, sizeof(fa), "%s/%s%s", a, name, MODULE_EXT);
    snprintf(fb, sizeof(fb), "%s/%s%s", b, name, MODULE_EXT);
    touch_file(fa, "first-broken-copy"); touch_file(fb, "second-copy");

    snprintf(ordered, sizeof(ordered), "%s:%s:%s", a, a, b);
    assert(rtmod_path_init(&path, ordered, error, sizeof(error)) == 0 && path.count == 2);
    assert(rtmod_path_resolve(&path, name, 0, result, sizeof(result), error, sizeof(error)) == 0);
    assert(!strcmp(result, fa)); /* selection is final; loaders must not fall through */
    rtmod_path_free(&path);

    snprintf(ordered, sizeof(ordered), "%s:%s", b, a);
    assert(rtmod_path_init(&path, ordered, error, sizeof(error)) == 0);
    assert(rtmod_path_resolve(&path, name, 0, result, sizeof(result), error, sizeof(error)) == 0);
    assert(!strcmp(result, fb));
    assert(rtmod_path_resolve(&path, "missing_fixture", 0, result, sizeof(result), error, sizeof(error)) < 0);
    assert(strstr(error, a) && strstr(error, b));

    snprintf(outside, sizeof(outside), "/tmp/rtmod-path-outside-%ld%s", (long)getpid(), MODULE_EXT);
    touch_file(outside, "outside");
    snprintf(escaped, sizeof(escaped), "%s/rtmod_path_escape%s", a, MODULE_EXT);
    unlink(escaped); assert(symlink(outside, escaped) == 0);
    assert(rtmod_path_resolve(&path, "rtmod_path_escape", 0, result, sizeof(result), error, sizeof(error)) < 0);
    assert(strstr(error, "escapes trusted directory"));
    {
        char name[64];
        /* the module helper's check: trusted extra file yes, symlink escape
           no, and the protected names only from the standard directory */
        assert(rtmod_path_is_trusted_file(fb, name, sizeof(name)) && !strcmp(name, "rtmod_path_fixture"));
        assert(!rtmod_path_is_trusted_file(escaped, name, sizeof(name)));
        assert(!rtmod_path_is_trusted_file(outside, name, sizeof(name)));
        snprintf(result, sizeof(result), "%s/hal_lib%s", a, MODULE_EXT);
        touch_file(result, "shadow");
        assert(!rtmod_path_is_trusted_file(result, name, sizeof(name)));
        {
            char core[PATH_MAX], std_core[PATH_MAX];
            snprintf(std_core, sizeof(std_core), "%s/hal_lib%s", EMC2_RTLIB_DIR, MODULE_EXT);
            assert(rtmod_path_resolve(&path, "hal_lib", 1, core, sizeof(core), error, sizeof(error)) == 0);
            assert(!strcmp(core, std_core));
        }
        unlink(result);
    }
    rtmod_path_free(&path);

    unlink(fa); unlink(fb); unlink(escaped); unlink(outside);
    puts("rtmod-path: all tests passed");
    return 0;
}
