/*  
 * awesome.c - awesome main functions
 * 
 * Copyright © 2007 Julien Danjou <julien@danjou.info> 
 * 
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version. 
 * 
 * This program is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 * GNU General Public License for more details. 
 * 
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. 
 * 
 */ 

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/select.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h> 
#include <X11/extensions/Xrandr.h> 

#include "awesome.h"
#include "event.h"
#include "layout.h"
#include "tag.h"
#include "screen.h"
#include "util.h"
#include "statusbar.h"

Client *clients = NULL;
Client *sel = NULL;
Client *stack = NULL;
DC *dc;

static int (*xerrorxlib) (Display *, XErrorEvent *);
static Bool readin = True, running = True;

/** Cleanup everything on quit
 * \param disp Display ref
 * \param drawcontext Drawcontext ref
 * \param awesomeconf awesome config
 */
static void
cleanup(Display *disp, DC *drawcontext, awesome_config *awesomeconf)
{
    int screen, i;

    close(STDIN_FILENO);
    
    while(stack)
    {
        unban(stack);
        unmanage(stack, drawcontext, NormalState, awesomeconf);
    }

    for(screen = 0; screen < ScreenCount(disp); screen++)
    {
        if(drawcontext[screen].font.set)
            XFreeFontSet(disp, drawcontext[screen].font.set);
        else
            XFreeFont(disp, drawcontext[screen].font.xfont);

            XUngrabKey(disp, AnyKey, AnyModifier, RootWindow(disp, screen));

        XFreePixmap(disp, awesomeconf[screen].statusbar.drawable);
        XFreeGC(disp, drawcontext[screen].gc);
        XDestroyWindow(disp, awesomeconf[screen].statusbar.window);
        XFreeCursor(disp, drawcontext[screen].cursor[CurNormal]);
        XFreeCursor(disp, drawcontext[screen].cursor[CurResize]);
        XFreeCursor(disp, drawcontext[screen].cursor[CurMove]);

        for(i = 0; i < awesomeconf[screen].ntags; i++)
            p_delete(&awesomeconf[screen].tags[i].name);
        for(i = 0; i < awesomeconf[screen].nkeys; i++)
            p_delete(&awesomeconf[screen].keys[i].arg);
        for(i = 0; i < awesomeconf[screen].nlayouts; i++)
            p_delete(&awesomeconf[screen].layouts[i].symbol);
        for(i = 0; i < awesomeconf[screen].nrules; i++)
        {
            p_delete(&awesomeconf[screen].rules[i].prop);
            p_delete(&awesomeconf[screen].rules[i].tags);
        }
        p_delete(&awesomeconf[screen].tags);
        p_delete(&awesomeconf[screen].layouts);
        p_delete(&awesomeconf[screen].rules);
        p_delete(&awesomeconf[screen].keys);
    }
    XSetInputFocus(disp, PointerRoot, RevertToPointerRoot, CurrentTime);
    XSync(disp, False);
    p_delete(&awesomeconf);
    p_delete(&dc);
}

/** Get a window state (WM_STATE)
 * \param disp Display ref
 * \param w Client window
 * \return state
 */
static long
getstate(Display *disp, Window w)
{
    int format, status;
    long result = -1;
    unsigned char *p = NULL;
    unsigned long n, extra;
    Atom real;
    status = XGetWindowProperty(disp, w, XInternAtom(disp, "WM_STATE", False),
                                0L, 2L, False, XInternAtom(disp, "WM_STATE", False),
                                &real, &format, &n, &extra, (unsigned char **) &p);
    if(status != Success)
        return -1;
    if(n != 0)
        result = *p;
    XFree(p);
    return result;
}

/** Scan X to find windows to manage
 * \param disp Display ref
 * \param screen Screen number
 * \param drawcontext Drawcontext ref
 * \param awesomeconf awesome config
 */
static void
scan(Display *disp, int screen, DC *drawcontext, awesome_config *awesomeconf)
{
    unsigned int i, num;
    Window *wins, d1, d2;
    XWindowAttributes wa;

    wins = NULL;
    if(XQueryTree(disp, RootWindow(disp, screen), &d1, &d2, &wins, &num))
    {
        for(i = 0; i < num; i++)
        {
            if(!XGetWindowAttributes(disp, wins[i], &wa)
               || wa.override_redirect
               || XGetTransientForHint(disp, wins[i], &d1))
                continue;
                if(wa.map_state == IsViewable || getstate(disp, wins[i]) == IconicState)
                    manage(disp, drawcontext, wins[i], &wa, awesomeconf);
        }
        /* now the transients */
        for(i = 0; i < num; i++)
        {
            if(!XGetWindowAttributes(disp, wins[i], &wa))
                continue;
            if(XGetTransientForHint(disp, wins[i], &d1)
               && (wa.map_state == IsViewable || getstate(disp, wins[i]) == IconicState))
                manage(disp, drawcontext, wins[i], &wa, awesomeconf);
        }
    }
    if(wins)
        XFree(wins);
}

/** Setup everything before running
 * \param disp Display ref
 * \param screen Screen number
 * \param awesomeconf awesome config ref
 * \todo clean things...
 */
static void
setup(Display *disp, int screen, DC *drawcontext, awesome_config *awesomeconf)
{
    XSetWindowAttributes wa;

    /* init cursors */
    drawcontext->cursor[CurNormal] = XCreateFontCursor(disp, XC_left_ptr);
    drawcontext->cursor[CurResize] = XCreateFontCursor(disp, XC_sizing);
    drawcontext->cursor[CurMove] = XCreateFontCursor(disp, XC_fleur);

    /* select for events */
    wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask
        | EnterWindowMask | LeaveWindowMask | StructureNotifyMask;
    wa.cursor = drawcontext->cursor[CurNormal];

    XChangeWindowAttributes(disp, RootWindow(disp, screen), CWEventMask | CWCursor, &wa);

    XSelectInput(disp, RootWindow(disp, screen), wa.event_mask);

    grabkeys(disp, screen, awesomeconf);

    compileregs(awesomeconf->rules, awesomeconf->nrules);

    /* bar */
    drawcontext->h = awesomeconf->statusbar.height = drawcontext->font.height + 2;
    initstatusbar(disp, screen, drawcontext, &awesomeconf->statusbar);
    drawcontext->gc = XCreateGC(disp, RootWindow(disp, screen), 0, 0);
    XSetLineAttributes(disp, drawcontext->gc, 1, LineSolid, CapButt, JoinMiter);

    if(!drawcontext->font.set)
        XSetFont(disp, drawcontext->gc, drawcontext->font.xfont->fid);

    loadawesomeprops(disp, awesomeconf);
}

/** Startup Error handler to check if another window manager
 * is already running.
 * \param disp Display ref
 * \param ee Error event
 */
static int __attribute__ ((noreturn))
xerrorstart(Display * disp __attribute__ ((unused)), XErrorEvent * ee __attribute__ ((unused)))
{
    eprint("awesome: another window manager is already running\n");
}

/** Quit awesome
 * \param disp Display ref
 * \param screen Screen number
 * \param drawcontext Drawcontext ref
 * \param awesomeconf awesome config
 * \param arg nothing
 * \ingroup ui_callback
 */
void
uicb_quit(Display *disp __attribute__ ((unused)),
          DC *drawcontext __attribute__ ((unused)),
          awesome_config *awesomeconf __attribute__((unused)),
          const char *arg __attribute__ ((unused)))
{
    readin = running = False;
}

/* There's no way to check accesses to destroyed windows, thus those cases are
 * ignored (especially on UnmapNotify's).  Other types of errors call Xlibs
 * default error handler, which may call exit.
 */
int
xerror(Display * edpy, XErrorEvent * ee)
{
    if(ee->error_code == BadWindow
       || (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
       || (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable)
       || (ee->request_code == X_PolyFillRectangle
           && ee->error_code == BadDrawable)
       || (ee->request_code == X_PolySegment && ee->error_code == BadDrawable)
       || (ee->request_code == X_ConfigureWindow
           && ee->error_code == BadMatch) || (ee->request_code == X_GrabKey
                                              && ee->error_code == BadAccess)
       || (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
        return 0;
    fprintf(stderr, "awesome: fatal error: request code=%d, error code=%d\n",
            ee->request_code, ee->error_code);

    return xerrorxlib(edpy, ee);        /* may call exit */
}

/** Hello, this is main
 * \param argc who knows
 * \param argv who knows
 * \return EXIT_SUCCESS I hope
 */
typedef void event_handler (XEvent *, awesome_config *); 
int
main(int argc, char *argv[])
{
    char *p;
    int r, xfd, e_dummy;
    fd_set rd;
    XEvent ev;
    Display * dpy;
    awesome_config *awesomeconf;
    int shape_event, randr_event_base;
    int screen;
    enum { NetSupported, NetWMName, NetLast };   /* EWMH atoms */ 
    Atom netatom[NetLast];
    event_handler **handler;

    if(argc == 2 && !a_strcmp("-v", argv[1]))
    {
        printf("awesome-" VERSION " © 2007 Julien Danjou\n");
        return EXIT_SUCCESS;
    }
    else if(argc != 1)
        eprint("usage: awesome [-v]\n");

    /* Tag won't be printed otherwised */
    setlocale(LC_CTYPE, "");

    if(!(dpy = XOpenDisplay(NULL)))
        eprint("awesome: cannot open display\n");

    xfd = ConnectionNumber(dpy);

    XSetErrorHandler(xerrorstart);
    for(screen = 0; screen < ScreenCount(dpy); screen++)
        /* this causes an error if some other window manager is running */
        XSelectInput(dpy, RootWindow(dpy, screen), SubstructureRedirectMask);

    /* need to XSync to validate errorhandler */
    XSync(dpy, False);
    XSetErrorHandler(NULL);
    xerrorxlib = XSetErrorHandler(xerror);
    XSync(dpy, False);

    netatom[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
    netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);

    /* allocate stuff */
    dc = p_new(DC, ScreenCount(dpy));
    awesomeconf = p_new(awesome_config, ScreenCount(dpy));

    for(screen = 0; screen < ScreenCount(dpy); screen++)
    {
        parse_config(dpy, screen, &dc[screen], &awesomeconf[screen]);
        setup(dpy, screen, &dc[screen], &awesomeconf[screen]);
        XChangeProperty(dpy, RootWindow(dpy, screen), netatom[NetSupported],
                        XA_ATOM, 32, PropModeReplace, (unsigned char *) netatom, NetLast);
        drawstatusbar(dpy, &dc[screen], &awesomeconf[screen]);
    }

    handler = p_new(event_handler *, LASTEvent);
    handler[ButtonPress] = handle_event_buttonpress;
    handler[ConfigureRequest] = handle_event_configurerequest;
    handler[ConfigureNotify] = handle_event_configurenotify;
    handler[DestroyNotify] = handle_event_destroynotify;
    handler[EnterNotify] = handle_event_enternotify;
    handler[LeaveNotify] = handle_event_leavenotify;
    handler[Expose] = handle_event_expose;
    handler[KeyPress] = handle_event_keypress;
    handler[MappingNotify] = handle_event_mappingnotify;
    handler[MapRequest] = handle_event_maprequest;
    handler[PropertyNotify] = handle_event_propertynotify;
    handler[UnmapNotify] = handle_event_unmapnotify;

    /* check for shape extension */
    if((awesomeconf[0].have_shape = XShapeQueryExtension(dpy, &shape_event, &e_dummy)))
    {
        p_realloc(&handler, shape_event + 1);
        handler[shape_event] = handle_event_shape;
    }

    /* check for randr extension */
    if((awesomeconf[0].have_randr = XRRQueryExtension(dpy, &randr_event_base, &e_dummy)))
    {
        p_realloc(&handler, randr_event_base + RRScreenChangeNotify + 1);
        handler[randr_event_base + RRScreenChangeNotify] = handle_event_randr_screen_change_notify;
    }

    for(screen = 0; screen < ScreenCount(dpy); screen++)
    {
        awesomeconf[screen].have_shape = awesomeconf[0].have_shape;
        awesomeconf[screen].have_randr = awesomeconf[0].have_randr;
        scan(dpy, screen, &dc[screen], &awesomeconf[screen]);
    }

    XSync(dpy, False);

    /* main event loop, also reads status text from stdin */
    while(running)
    {
        FD_ZERO(&rd);
        if(readin)
            FD_SET(STDIN_FILENO, &rd);
        FD_SET(xfd, &rd);
        if(select(xfd + 1, &rd, NULL, NULL, NULL) == -1)
        {
            if(errno == EINTR)
                continue;
            eprint("select failed\n");
        }
        if(FD_ISSET(STDIN_FILENO, &rd))
        {
            switch (r = read(STDIN_FILENO, awesomeconf[0].statustext, sizeof(awesomeconf[0].statustext) - 1))
            {
            case -1:
                strncpy(awesomeconf[0].statustext, strerror(errno), sizeof(awesomeconf[0].statustext) - 1);
                awesomeconf[0].statustext[sizeof(awesomeconf[0].statustext) - 1] = '\0';
                readin = False;
                break;
            case 0:
                strncpy(awesomeconf[0].statustext, "EOF", 4);
                readin = False;
                break;
            default:
                for(awesomeconf[0].statustext[r] = '\0', p = awesomeconf[0].statustext + a_strlen(awesomeconf[0].statustext) - 1;
                    p >= awesomeconf[0].statustext && *p == '\n'; *p-- = '\0');
                for(; p >= awesomeconf[0].statustext && *p != '\n'; --p);
                if(p > awesomeconf[0].statustext)
                    strncpy(awesomeconf[0].statustext, p + 1, sizeof(awesomeconf[0].statustext));
            }
            drawstatusbar(dpy, &dc[0], &awesomeconf[0]);
        }

        while(XPending(dpy))
        {
            XNextEvent(dpy, &ev);
            if(handler[ev.type])
                handler[ev.type](&ev, awesomeconf);       /* call handler */
        }
    }
    cleanup(dpy, dc, awesomeconf);
    XCloseDisplay(dpy);

    return EXIT_SUCCESS;
}
