/* Print the X11 CLIPBOARD selection as UTF-8 to stdout, like `xclip -o -selection clipboard`
 * (xclip and xsel are not installed where the acceptance runs). Used by
 * tests/acceptance/m3.2.sh to read what the app copied.
 *
 * usage: clip_read            (DISPLAY must name the Xvfb display; shot.sh exports it)
 * exit:  0 text printed (possibly empty)
 *        2 the clipboard has no owner (nothing was copied)
 *        3 the owner refused UTF8_STRING, sent INCR (over the request size), or did not
 *          answer within 5 s
 *        1 no display
 *
 * build: cc -O2 -o clip-read tests/acceptance/clip_read.c -lX11
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>

static int waitForNotify(Display* dpy, Window win, XEvent* out)
{
    const int fd = ConnectionNumber(dpy);
    struct timeval end;
    gettimeofday(&end, NULL);
    end.tv_sec += 5;
    for (;;) {
        while (XPending(dpy)) {
            XNextEvent(dpy, out);
            if (out->type == SelectionNotify && out->xselection.requestor == win)
                return 1;
        }
        struct timeval now, left;
        gettimeofday(&now, NULL);
        if (!timercmp(&now, &end, <))
            return 0;
        timersub(&end, &now, &left);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        select(fd + 1, &fds, NULL, NULL, &left);
    }
}

int main(void)
{
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "clip_read: cannot open display\n");
        return 1;
    }
    const Atom clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    const Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    const Atom incr = XInternAtom(dpy, "INCR", False);
    const Atom prop = XInternAtom(dpy, "CLIP_READ_RESULT", False);

    if (XGetSelectionOwner(dpy, clipboard) == None) {
        fprintf(stderr, "clip_read: the clipboard is empty (no owner)\n");
        return 2;
    }

    const Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
    XConvertSelection(dpy, clipboard, utf8, prop, win, CurrentTime);
    XFlush(dpy);

    XEvent ev;
    if (!waitForNotify(dpy, win, &ev)) {
        fprintf(stderr, "clip_read: the clipboard owner did not answer\n");
        return 3;
    }
    if (ev.xselection.property == None) {
        fprintf(stderr, "clip_read: the clipboard owner refused UTF8_STRING\n");
        return 3;
    }

    Atom type;
    int format;
    unsigned long count, after;
    unsigned char* data = NULL;
    if (XGetWindowProperty(dpy, win, prop, 0, 1L << 24, True, AnyPropertyType, &type, &format,
                           &count, &after, &data) != Success) {
        fprintf(stderr, "clip_read: cannot read the converted text\n");
        return 3;
    }
    if (type == incr || after != 0) {
        fprintf(stderr, "clip_read: text too large (INCR transfer not supported)\n");
        XFree(data);
        return 3;
    }
    if (data && format == 8)
        fwrite(data, 1, count, stdout);
    XFree(data);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
