/* web.c -- entry point for the WebAssembly build only.
   Built by web/build.sh; the native build (build.sh, Makefile) does not
   compile this file, and the whole thing compiles to nothing without
   __EMSCRIPTEN__ anyway. */
#include "nx88.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

int main(int argc, char **argv);

/* ★ The page does NOT start the emulator through Emscripten's callMain: under
   -sASYNCIFY, main() unwinds at the guest's first console read and returns to
   JS long before the machine stops, losing the exit code and the "halted"
   moment.  An ordinary exported function lets the page use
   ccall(..., {async: true}), whose Promise settles when main() really returns.

   Splitting the command line here spares the page marshalling an argv into the
   heap, and takes the same words BOOT.md documents.  Whitespace only, no
   quoting: no flag takes an argument containing a space. */
#define WEB_MAXARGS 64
static char web_cmd[1024];      /* argv points INTO this; must outlive the run,
                                   and must not be ccall's temporary stack copy,
                                   which asyncify would unwind out from under it */

EMSCRIPTEN_KEEPALIVE int nx_web_run(const char *cmdline)
{
    snprintf(web_cmd, sizeof web_cmd, "%s", cmdline ? cmdline : "");

    char *argv[WEB_MAXARGS];
    int argc = 0;
    argv[argc++] = (char *)"nx88";
    for (char *p = web_cmd; *p && argc < WEB_MAXARGS; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = 0;
    }
    return main(argc, argv);
}
#endif  /* __EMSCRIPTEN__ */
