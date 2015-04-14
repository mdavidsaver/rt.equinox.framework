/*******************************************************************************
 * Copyright (c) 2000, 2011 IBM Corporation and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at 
 * http://www.eclipse.org/legal/epl-v10.html
 * 
 * Contributors:
 *     IBM Corporation - initial API and implementation
 *     Kevin Cornell (Rational Software Corporation)
 *     Tom Tromey (Red Hat, Inc.)
 *******************************************************************************/

#include "eclipseMozilla.h"
#include "eclipseCommon.h"
#include "eclipseOS.h"
#include "eclipseUtil.h"
#include "eclipseGtk.h"

#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#ifdef SOLARIS
#include <sys/filio.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <semaphore.h>
#include <fcntl.h>

#ifdef HPUX
#define SEM_FAILED (void *)-1
#endif

/* Global Variables */
char*  defaultVM     = "java";
char*  vmLibrary 	 = "libjvm.so";
char*  shippedVMDir  = "jre/bin/";

/* Define the special arguments for the various Java VMs. */
static char*  argVM_JAVA[]        = { NULL };

/* Define local variables . */
static long			splashHandle = 0;
static GtkWidget*   shellHandle = 0;

static sem_t* mutex;
static Atom appWindowAtom, launcherWindowAtom;
static _TCHAR** openFilePath = NULL; /* the files we want to open */
static int openFileTimeout = 60; 	 /* number of seconds to wait before timeout */
static int windowPropertySet = 0;	 /* set to 1 on success */

static struct sigaction quitAction;
static struct sigaction intAction;

/* Local functions */
static void catch_signal(int sig) {
	//catch signals, free the lock, reinstall the original
	//signal handlers and reraise the signal.
	sem_post(mutex);
	sem_close(mutex);
	sigaction(SIGINT, &intAction, NULL);
	sigaction(SIGQUIT, &intAction, NULL);
	raise(sig);
}

/* Create a "SWT_Window_" + APP_NAME string with optional suffix.
 * Caller should free the memory when finished */
static char * createSWTWindowString(char * suffix, int semaphore) {
#ifdef SOLARIS
	/* solaris requires semaphore names to start with '/' */
	char * prefix = semaphore != 0 ? _T_ECLIPSE("/SWT_Window_") : _T_ECLIPSE("SWT_Window_");
#else
	char * prefix = _T_ECLIPSE("SWT_Window_");
#endif
	
	char * result = malloc((_tcslen(prefix) + _tcslen(getOfficialName()) + (suffix != NULL ? _tcslen(suffix) : 0) + 1) * sizeof(char));
	if (suffix != NULL)
		_stprintf(result, _T_ECLIPSE("%s%s%s"), prefix, getOfficialName(), suffix);
	else
		_stprintf(result, _T_ECLIPSE("%s%s"), prefix, getOfficialName());
	return result;
}

static int setAppWindowProperty() {
	Window appWindow;
	Atom propAtom;
	_TCHAR *propVal;

	//Look for the SWT window. If it's there, set a property on it.
	appWindow = gtk.XGetSelectionOwner(gtk_GDK_DISPLAY, appWindowAtom);
	if (appWindow) {
		propAtom = gtk.XInternAtom(gtk_GDK_DISPLAY, "org.eclipse.swt.filePath.message", FALSE);
		//append a colon delimiter in case more than one file gets appended to the app windows property.
		propVal = concatPaths(openFilePath, _T_ECLIPSE(':'));
		gtk.XChangeProperty(gtk_GDK_DISPLAY, appWindow, propAtom, propAtom, 8, PropModeAppend, (unsigned char *)propVal, _tcslen(propVal));
		gtk.XSync(gtk_GDK_DISPLAY, False);
		free(propVal);
		windowPropertySet = 1;
		return 1;
	}
	return 0;
}

/* timer callback function to call setAppWindowProperty */
static gboolean setAppWindowTimerProc(gpointer data) {
	//try to set the app window property. If unsuccessful return true to reschedule the timer.
	openFileTimeout--;
	return !setAppWindowProperty() && openFileTimeout > 0;
}

int createLauncherWindow() {
	Window window, launcherWindow;
	/* XGrabServer prevents other X clients from interrupting between
	 * testing for an existing selection owner (XGetSelectionOwner) and
	 * and setting a new selection owner.
	 * Note: This is heavyweight X server wide mutual exclusion, use with care.
	 */
	gtk.XGrabServer(gtk_GDK_DISPLAY);
	//check if a launcher window exists. If none exists, we know we are the first and we should be launching the app.
	window = gtk.XGetSelectionOwner(gtk_GDK_DISPLAY, launcherWindowAtom);
	if (window == 0) {
		//create a launcher window that other processes can find.
		launcherWindow = gtk.XCreateWindow(gtk_GDK_DISPLAY, gtk.XRootWindow(gtk_GDK_DISPLAY, gtk.XDefaultScreen(gtk_GDK_DISPLAY)), -10, -10, 1,
				1, 0, 0, InputOnly, CopyFromParent, (unsigned long) 0, (XSetWindowAttributes *) NULL);
		if(launcherWindow==0) {
			gtk.XUngrabServer(gtk_GDK_DISPLAY);
			return 1;
		}
		gtk.XSetSelectionOwner(gtk_GDK_DISPLAY, launcherWindowAtom, launcherWindow, CurrentTime);
		gtk.XUngrabServer(gtk_GDK_DISPLAY);
		// final check to see that everything worked.  This should only fail
		// if another client isn't playing nice (not using XGrabServer).
		if(gtk.XGetSelectionOwner(gtk_GDK_DISPLAY, launcherWindowAtom)!=launcherWindow)
			return 1;
		//add a timeout to set the property on the apps window once the app is launched.
		gtk.g_timeout_add(1000, setAppWindowTimerProc, 0);
		return 0;
	}
	gtk.XUngrabServer(gtk_GDK_DISPLAY);
	return 1;
}

/* This call checks for the presence of another instance of this product
 * and, if found, sends a message with a file name string to be opened.
 * Implemented using the X11 Selection IPC mechanism (aka the X clipboard).
 *
 * See Selections in chapter #4 of
 * http://www.x.org/releases/current/doc/libX11/libX11/libX11.html
 * Also chapter #2 of
 * http://www.x.org/releases/current/doc/xorg-docs/icccm/icccm.html
 */
int reuseWorkbench(_TCHAR** filePath, int timeout) {
	char *appName, *launcherName;
	int result = 0;

	if (initWindowSystem(&initialArgc, initialArgv, 1) != 0)
		return -1;

	openFileTimeout = timeout;
	openFilePath = filePath;
	
	//App name is defined in SWT (Display.java) as well. Values must be consistent.
	appName = createSWTWindowString(NULL, 0);
	appWindowAtom = gtk.XInternAtom(gtk_GDK_DISPLAY, appName, FALSE);
	free(appName);

	//check if app is already running. Just set property if it is.
	if (setAppWindowProperty() > 0)
		return 1;

	/* app is not running, create a launcher window to act as a mutex so we don't need to keep the semaphore locked */
	launcherName = createSWTWindowString(_T_ECLIPSE("_Launcher"), 1);
	launcherWindowAtom = gtk.XInternAtom(gtk_GDK_DISPLAY, launcherName, FALSE);
	result = createLauncherWindow();
	free(launcherName);

	if (result == 1) {
		//The app is already being launched in another process.  Set the property on that app window and exit
		while (openFileTimeout > 0) {
			if (setAppWindowProperty() > 0)
				return 1; //success
			else {
				openFileTimeout--;
				sleep(1);
			}
		}
		//timed out trying to set the app property
		result = 0;
	}
	return result;
}

/* Create and Display the Splash Window */
int showSplash( const char* featureImage )
{
	GtkWidget *image;
	GdkPixbuf *pixbuf;

	if (splashHandle != 0)
		return 0; /* already showing splash */
	if (featureImage == NULL)
		return -1;
	
	if (initialArgv == NULL)
		initialArgc = 0;
	
	if( initWindowSystem(&initialArgc, initialArgv, 1) != 0)
		return -1;
	
	shellHandle = gtk.gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk.gtk_window_set_decorated((GtkWindow*)(shellHandle), FALSE);
	gtk.gtk_window_set_type_hint((GtkWindow*)(shellHandle), 4 /*GDK_WINDOW_TYPE_HINT_SPLASHSCREEN*/);
	gtk.g_signal_connect_data((gpointer)shellHandle, "destroy", (GtkSignalFunc)(gtk.gtk_widget_destroyed), &shellHandle, NULL, 0);
		
	pixbuf = gtk.gdk_pixbuf_new_from_file(featureImage, NULL);
	image = gtk.gtk_image_new_from_pixbuf(pixbuf);
	if (pixbuf) {
		gtk.g_object_unref(pixbuf);
	}
	gtk.gtk_container_add((GtkContainer*)(shellHandle), image);
	
	if (getOfficialName() != NULL)
		gtk.gtk_window_set_title((GtkWindow*)(shellHandle), getOfficialName());
	gtk.gtk_window_set_position((GtkWindow*)(shellHandle), GTK_WIN_POS_CENTER);
	gtk.gtk_window_resize((GtkWindow*)(shellHandle), gtk.gdk_pixbuf_get_width(pixbuf), gtk.gdk_pixbuf_get_height(pixbuf));
	gtk.gtk_widget_show_all((GtkWidget*)(shellHandle));
	splashHandle = (long)shellHandle;
	dispatchMessages();
	return 0;
}

void dispatchMessages() {
	if (gtk.g_main_context_iteration != 0)
		while(gtk.g_main_context_iteration(0,0) != 0) {}
}

jlong getSplashHandle() {
	return splashHandle;
}

void takeDownSplash() {
	if(shellHandle != 0) {
		gtk.gtk_widget_destroy(shellHandle);
		dispatchMessages();
		splashHandle = 0;
		shellHandle = NULL;
	}
}

/* Get the window system specific VM arguments */
char** getArgVM( char* vm ) 
{
    char** result;

/*    if (isJ9VM( vm )) 
        return argVM_J9;*/
    
    /* Use the default arguments for a standard Java VM */
    result = argVM_JAVA;
    return result;
}

JavaResults* launchJavaVM( char* args[] )
{
	JavaResults* jvmResults = NULL;
  	pid_t   jvmProcess, finishedProcess = 0;
  	int     exitCode;
  	
#ifdef MOZILLA_FIX
	fixEnvForMozilla();
#endif /* MOZILLA_FIX */
	
	jvmProcess = fork();
  	if (jvmProcess == 0) 
    {
    	/* Child process ... start the JVM */
      	execv(args[0], args);

      	/* The JVM would not start ... return error code to parent process. */
      	/* TODO, how to distinguish this as a launch problem to the other process? */
      	_exit(errno);
    }

  	jvmResults = malloc(sizeof(JavaResults));
  	memset(jvmResults, 0, sizeof(JavaResults));
  	
	/* If the JVM is still running, wait for it to terminate. */
	if (jvmProcess != 0)
	{
		/* When attempting a file open, we need to spin the event loop
		 * for setAppWindowTimerProc to run.  When that succeeds or times out, 
		 * we can stop the event loop and just wait on the child process.
		 */
		if (openFilePath != NULL) {
			struct timespec sleepTime;
			sleepTime.tv_sec = 0;
			sleepTime.tv_nsec = 5e+8; // 500 milliseconds
			
			while(openFileTimeout > 0 && !windowPropertySet && (finishedProcess = waitpid(jvmProcess, &exitCode, WNOHANG)) == 0) {
				dispatchMessages();
				nanosleep(&sleepTime, NULL);
			}
		}
		if (finishedProcess == 0)
			waitpid(jvmProcess, &exitCode, 0);
      	if (WIFEXITED(exitCode))
      		/* TODO, this should really be a runResult if we could distinguish the launch problem above */
			jvmResults->launchResult = WEXITSTATUS(exitCode);
    }

	return jvmResults;
}
