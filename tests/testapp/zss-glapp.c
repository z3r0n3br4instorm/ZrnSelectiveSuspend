// SPDX-License-Identifier: GPL-2.0-only
/*
 * zss-glapp: a small OpenGL (GLX) program for the tests. It draws a triangle
 * that turns, and can change its swap interval while running, as games do
 * when they apply their vertical-sync setting after the first frames; that
 * makes Zink build a new swapchain with another present mode.
 *
 * Usage: zss-glapp [--frames N] [--interval-at FRAME=INTERVAL]... [--flip-every N]
 * --flip-every N changes the interval between 0 and 1 every N frames.
 * Prints "frame N" every ten frames, flushed.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*swap_interval_fn)(Display *, GLXDrawable, int);

int main(int argc, char **argv)
{
    static int attrs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
    int frames = 300, at[8], interval[8], nat = 0, flip = 0;
    Display *dpy = XOpenDisplay(NULL);
    XSetWindowAttributes swa;
    swap_interval_fn swap_interval;
    XVisualInfo *vi;
    GLXContext ctx;
    Window win;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flip-every") && i + 1 < argc)
            flip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval-at") && i + 1 < argc && nat < 8 &&
                 sscanf(argv[++i], "%d=%d", &at[nat], &interval[nat]) == 2)
            nat++;
    }
    if (!dpy || !(vi = glXChooseVisual(dpy, DefaultScreen(dpy), attrs))) {
        fprintf(stderr, "zss-glapp: no display or no visual\n");
        return 2;
    }
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, 320, 240, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, "zss-glapp");
    XMapWindow(dpy, win);
    ctx = glXCreateContext(dpy, vi, NULL, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "zss-glapp: no OpenGL context\n");
        return 2;
    }
    swap_interval = (swap_interval_fn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    printf("renderer %s\n", (const char *)glGetString(GL_RENDERER));
    fflush(stdout);
    for (int f = 1; f <= frames; f++) {
        for (int i = 0; i < nat; i++)
            if (at[i] == f && swap_interval) {
                swap_interval(dpy, win, interval[i]);
                printf("interval %d\n", interval[i]);
                fflush(stdout);
            }
        if (flip && f % flip == 0 && swap_interval)
            swap_interval(dpy, win, (f / flip) & 1);
        glClearColor(0.1f, 0.1f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glLoadIdentity();
        glRotatef((float)f, 0, 0, 1);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(-0.6f, -0.5f);
        glColor3f(0, 1, 0); glVertex2f(0.6f, -0.5f);
        glColor3f(0, 0, 1); glVertex2f(0.0f, 0.7f);
        glEnd();
        glXSwapBuffers(dpy, win);
        if (f % 10 == 0) {
            printf("frame %d\n", f);
            fflush(stdout);
        }
    }
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
