#*******************************************************************************
# Copyright (c) 2000, 2005 IBM Corporation and others.
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the Eclipse Public License v1.0
# which accompanies this distribution, and is available at 
# http://www.eclipse.org/legal/epl-v10.html
# 
# Contributors:
#     IBM Corporation - initial API and implementation
#     Kevin Cornell (Rational Software Corporation)
#*******************************************************************************
 
# Makefile for creating the Linux/Motif eclipse launcher program.

# This makefile expects the following environment variables set:
#
# PROGRAM_OUTPUT  - the filename of the output executable
# PROGRAM_LIBRARY - the filename of the output library
# DEFAULT_OS      - the default value of the "-os" switch
# DEFAULT_OS_ARCH - the default value of the "-arch" switch
# DEFAULT_WS      - the default value of the "-ws" switch
# X11_HOME	     - the full path to X11 header files
# MOTIF_HOME	 - the full path to Motif header files
# JAVA_JNI       - the full path to the java jni header files

ifeq ($(PROGRAM_OUTPUT),)
  PROGRAM_OUTPUT=eclipse
endif
ifeq ($(PROGRAM_LIBRARY),)
  PROGRAM_LIBRARY=eclipse_001.so
endif

# Define the object modules to be compiled and flags.
#OBJS = eclipse.o eclipseUtil.o eclipseJNI.o eclipseConfig.o eclipseMozilla.o eclipseMotif.o NgCommon.o NgImage.o NgImageData.o NgWinBMPFileFormat.o

MAIN_OBJS = eclipseMain.o
COMMON_OBJS = eclipseConfig.o eclipseCommon.o eclipseMotifCommon.o
DLL_OBJS	= eclipse.o eclipseMotif.o eclipseUtil.o eclipseJNI.o eclipseMozilla.o NgCommon.o NgImage.o NgImageData.o NgWinBMPFileFormat.o


EXEC = $(PROGRAM_OUTPUT)
DLL = $(PROGRAM_LIBRARY)
LIBS = -Xlinker -rpath -Xlinker . -L$(MOTIF_HOME)/lib -L$(X11_HOME)/lib -lXm -lXt -lX11 -lXinerama
LFLAGS = -shared -fpic -Wl,--export-dynamic 
CFLAGS = -g -s -Wall \
	-DLINUX \
	-DMOZILLA_FIX \
	-DDEFAULT_OS="\"$(DEFAULT_OS)\"" \
	-DDEFAULT_OS_ARCH="\"$(DEFAULT_OS_ARCH)\"" \
	-DDEFAULT_WS="\"$(DEFAULT_WS)\"" \
	-fPIC \
	-I./ \
	-I../ \
	-I$(MOTIF_HOME)/include \
	-I$(X11_HOME)/include \
	-I$(JAVA_JNI)

all: $(EXEC) $(DLL)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

eclipseMain.o: ../eclipseMain.c ../eclipseUnicode.h ../eclipseCommon.h  
	$(CC) $(CFLAGS) -c $< -o $@
	
eclipse.o: ../eclipse.c ../eclipseOS.h ../eclipseCommon.h ../eclipseJNI.h
	$(CC) $(CFLAGS) -c $< -o $@

eclipseCommon.o: ../eclipseCommon.c ../eclipseCommon.h ../eclipseUnicode.h 
	$(CC) $(CFLAGS) -c $< -o $@
	
eclipseUtil.o: ../eclipseUtil.c ../eclipseUtil.h ../eclipseOS.h
	$(CC) $(CFLAGS) -c $< -o $@

eclipseJNI.o: ../eclipseJNI.c ../eclipseCommon.h ../eclipseOS.h ../eclipseJNI.h
	$(CC) $(CFLAGS) -c $< -o $@
	
eclipseConfig.o: ../eclipseConfig.c ../eclipseConfig.h ../eclipseOS.h
	$(CC) $(CFLAGS) -c $< -o $@
	
eclipseMozilla.o: ../eclipseMozilla.c ../eclipseMozilla.h ../eclipseOS.h
	$(CC) $(CFLAGS) -c $< -o $@

$(EXEC): $(MAIN_OBJS) $(COMMON_OBJS)
	$(CC) -o $(EXEC) $(MAIN_OBJS) $(COMMON_OBJS) $(LIBS)

$(DLL): $(DLL_OBJS) $(COMMON_OBJS)
	$(CC) $(LFLAGS) -o $(DLL) $(DLL_OBJS) $(COMMON_OBJS) $(LIBS)
	
install: all
	cp $(EXEC) $(DLL) $(OUTPUT_DIR)
	rm -f $(EXEC) $(MAIN_OBJS) $(COMMON_OBJS) $(DLL_OBJS)

clean:
	rm -f $(EXEC) $(MAIN_OBJS) $(COMMON_OBJS) $(DLL_OBJS)
