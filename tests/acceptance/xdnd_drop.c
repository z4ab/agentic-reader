// Drops files onto the X window at a screen position, the way a file manager
// does (XDND version 5, text/uri-list, action copy). Test helper for
// tests/acceptance/m2.5.sh, which builds it with: cc -o xdnd-drop xdnd_drop.c -lX11
//
// usage: xdnd-drop <x> <y> <file>...
//
// Exit status: 0 after the target sends XdndFinished, or 1 s after it read the
// file list (a target may show a modal box before it finishes the drop), 2 on
// bad usage, 3 when no XDND-aware window is at (x, y), 4 when the target
// refuses the drop, 5 on a timeout (5 s).
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

static Display* dpy;
static Atom aAware, aEnter, aPosition, aStatus, aDrop, aLeave, aFinished;
static Atom aSelection, aActionCopy, aUriList, aTargets;

static long xdndVersion(Window w)
{
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char* data = NULL;
    long version = -1;
    if (XGetWindowProperty(dpy, w, aAware, 0, 1, False, AnyPropertyType, &type, &format,
                           &count, &after, &data) == Success
        && data && count == 1 && format == 32)
        version = (long)((unsigned long*)data)[0];
    if (data)
        XFree(data);
    return version;
}

// The outermost XDND-aware window under (x, y), walking down from the root.
static Window findTarget(int x, int y, long* version)
{
    Window root = DefaultRootWindow(dpy);
    Window w = root;
    for (;;) {
        Window child = None;
        int cx, cy;
        if (!XTranslateCoordinates(dpy, root, w, x, y, &cx, &cy, &child))
            return None;
        if (child == None)
            return None;
        *version = xdndVersion(child);
        if (*version >= 0)
            return child;
        w = child;
    }
}

static void sendClient(Window target, Atom type, long l0, long l1, long l2, long l3, long l4)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.display = dpy;
    ev.xclient.window = target;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = l0;
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    ev.xclient.data.l[3] = l3;
    ev.xclient.data.l[4] = l4;
    XSendEvent(dpy, target, False, NoEventMask, &ev);
    XFlush(dpy);
}

// A server timestamp, from a zero-length property append on our own window.
static Time serverTime(Window w)
{
    unsigned char dummy = 0;
    XEvent ev;
    XSelectInput(dpy, w, PropertyChangeMask);
    XChangeProperty(dpy, w, XA_WM_NAME, XA_STRING, 8, PropModeAppend, &dummy, 0);
    XWindowEvent(dpy, w, PropertyChangeMask, &ev);
    return ev.xproperty.time;
}

static void appendUri(char* out, size_t size, const char* path)
{
    char full[PATH_MAX];
    if (!realpath(path, full))
        snprintf(full, sizeof full, "%s", path);
    size_t n = strlen(out);
    n += (size_t)snprintf(out + n, size - n, "file://");
    for (const unsigned char* p = (const unsigned char*)full; *p && n + 4 < size; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')
            || strchr("/-._~", *p))
            out[n++] = (char)*p;
        else
            n += (size_t)snprintf(out + n, size - n, "%%%02X", *p);
    }
    out[n] = '\0';
    snprintf(out + n, size - n, "\r\n");
}

// Returns 1 when the file list was sent.
static int answerSelection(const XSelectionRequestEvent* req, const char* uris)
{
    int served = 0;
    XEvent reply;
    memset(&reply, 0, sizeof reply);
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = dpy;
    reply.xselection.requestor = req->requestor;
    reply.xselection.selection = req->selection;
    reply.xselection.target = req->target;
    reply.xselection.time = req->time;
    reply.xselection.property = None;
    Atom property = req->property != None ? req->property : req->target;
    if (req->target == aUriList) {
        XChangeProperty(dpy, req->requestor, property, aUriList, 8, PropModeReplace,
                        (const unsigned char*)uris, (int)strlen(uris));
        reply.xselection.property = property;
        served = 1;
    } else if (req->target == aTargets) {
        Atom targets[2] = {aTargets, aUriList};
        XChangeProperty(dpy, req->requestor, property, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char*)targets, 2);
        reply.xselection.property = property;
    }
    XSendEvent(dpy, req->requestor, False, NoEventMask, &reply);
    XFlush(dpy);
    return served;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: xdnd-drop <x> <y> <file>...\n");
        return 2;
    }
    const int x = atoi(argv[1]);
    const int y = atoi(argv[2]);
    static char uris[65536];
    uris[0] = '\0';
    for (int i = 3; i < argc; ++i)
        appendUri(uris, sizeof uris, argv[i]);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xdnd-drop: cannot open display\n");
        return 2;
    }
    aAware = XInternAtom(dpy, "XdndAware", False);
    aEnter = XInternAtom(dpy, "XdndEnter", False);
    aPosition = XInternAtom(dpy, "XdndPosition", False);
    aStatus = XInternAtom(dpy, "XdndStatus", False);
    aDrop = XInternAtom(dpy, "XdndDrop", False);
    aLeave = XInternAtom(dpy, "XdndLeave", False);
    aFinished = XInternAtom(dpy, "XdndFinished", False);
    aSelection = XInternAtom(dpy, "XdndSelection", False);
    aActionCopy = XInternAtom(dpy, "XdndActionCopy", False);
    aUriList = XInternAtom(dpy, "text/uri-list", False);
    aTargets = XInternAtom(dpy, "TARGETS", False);

    long version = -1;
    const Window target = findTarget(x, y, &version);
    if (target == None) {
        fprintf(stderr, "xdnd-drop: no XDND-aware window at %d,%d\n", x, y);
        return 3;
    }
    if (version > 5)
        version = 5;

    const Window source = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, 0);
    const Time now = serverTime(source);
    XSetSelectionOwner(dpy, aSelection, source, now);

    XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, x, y);
    sendClient(target, aEnter, (long)source, version << 24, (long)aUriList, None, None);
    sendClient(target, aPosition, (long)source, 0, ((long)x << 16) | (y & 0xffff), (long)now,
               (long)aActionCopy);

    int dropped = 0;
    int served = 0;
    struct timeval end, tv;
    gettimeofday(&end, NULL);
    end.tv_sec += 5;
    const int fd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == SelectionRequest) {
                if (answerSelection(&ev.xselectionrequest, uris) && !served) {
                    served = 1;
                    gettimeofday(&end, NULL);
                    end.tv_sec += 1;
                }
            } else if (ev.type == ClientMessage && ev.xclient.message_type == aStatus) {
                if (dropped)
                    continue;
                if (!(ev.xclient.data.l[1] & 1)) {
                    sendClient(target, aLeave, (long)source, 0, 0, 0, 0);
                    fprintf(stderr, "xdnd-drop: target refused the drop\n");
                    return 4;
                }
                sendClient(target, aDrop, (long)source, 0, (long)now, 0, 0);
                dropped = 1;
            } else if (ev.type == ClientMessage && ev.xclient.message_type == aFinished) {
                XCloseDisplay(dpy);
                return 0;
            }
        }
        gettimeofday(&tv, NULL);
        if (timercmp(&tv, &end, >)) {
            if (served) {
                XCloseDisplay(dpy);
                return 0;
            }
            fprintf(stderr, "xdnd-drop: timed out (%s)\n", dropped ? "after drop" : "no status");
            return 5;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval wait = {0, 50000};
        select(fd + 1, &fds, NULL, NULL, &wait);
    }
}
