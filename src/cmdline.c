#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>

#include "cmdline.h"
#include "util.h"
#include "strbuf.h"

void pa_cmdline_help(const char *argv0) {
    const char *e;

    if ((e = strrchr(argv0, '/')))
        e++;
    else
        e = argv0;
    
    printf("%s [options]\n"
           "  -L MODULE  Load the specified plugin module with the specified argument\n"
           "  -F FILE    Run the specified script\n"
           "  -C         Open a command line on the running TTY\n"
           "  -D         Daemonize after loading the modules\n"
           "  -f         Dont quit when the startup fails\n"
           "  -v         Verbose startup\n"
           "  -h         Show this help\n", e);
}

struct pa_cmdline* pa_cmdline_parse(int argc, char * const argv []) {
    char c;
    struct pa_cmdline *cmdline = NULL;
    struct pa_strbuf *buf = NULL;
    assert(argc && argv);

    cmdline = malloc(sizeof(struct pa_cmdline));
    assert(cmdline);
    cmdline->daemonize = cmdline->help = cmdline->verbose = 0;
    cmdline->fail = 1;

    buf = pa_strbuf_new();
    assert(buf);
    
    while ((c = getopt(argc, argv, "L:F:CDhfv")) != -1) {
        switch (c) {
            case 'L':
                pa_strbuf_printf(buf, "load %s\n", optarg);
                break;
            case 'F':
                pa_strbuf_printf(buf, ".include %s\n", optarg);
                break;
            case 'C':
                pa_strbuf_puts(buf, "load module-cli\n");
                break;
            case 'D':
                cmdline->daemonize = 1;
                break;
            case 'h':
                cmdline->help = 1;
                break;
            case 'f':
                cmdline->fail = 0;
                break;
            case 'v':
                cmdline->verbose = 0;
                break;
            default:
                goto fail;
        }
    }

    cmdline->cli_commands = pa_strbuf_tostring_free(buf);
    return cmdline;
    
fail:
    if (cmdline)
        pa_cmdline_free(cmdline);
    if (buf)
        pa_strbuf_free(buf);
    return NULL;
}

void pa_cmdline_free(struct pa_cmdline *cmd) {
    assert(cmd);
    free(cmd->cli_commands);
    free(cmd);
}
