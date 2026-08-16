/* web.c -- entry point for the WebAssembly build only.
   Built by web/build.sh; the native build (build.sh, Makefile) does not
   compile this file, and the whole thing compiles to nothing without
   __EMSCRIPTEN__ anyway. */
#include "nx88.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

int main(int argc, char **argv);

/* ★ The page does NOT start the emulator through Emscripten's callMain.
   With -sASYNCIFY, main() unwinds the moment the guest first reads the console
   and returns to JS long before the machine has stopped -- so callMain's return
   value says nothing, and the exit code and the "machine halted" moment are
   both lost.  Going through an ordinary exported function instead lets the page
   use ccall(..., {async: true}), which hands back a Promise that settles when
   main() has REALLY returned.

   Splitting the command line here rather than in JS is the other half of it:
   the page never has to marshal an argv into the heap, and the arguments it
   passes are the same words BOOT.md documents.  Splitting is on whitespace
   only -- there is no quoting -- which covers every flag the emulator has,
   none of which take an argument containing a space. */
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
