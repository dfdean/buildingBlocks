################################################################################
# Copyright (c) 2005-2017 Dawson Dean
# 
# Permission is hereby granted, free of charge, to any person obtaining a 
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation 
# the rights to use, copy, modify, merge, publish, distribute, sublicense, 
# and/or sell copies of the Software, and to permit persons to whom the 
# Software is furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included 
# in all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
################################################################################
# 
# Common Makefile Includes
#
# This has standard defines that are used by various makefiles.
# Include this at the TOP of every makefile.
# This assumes a directory structure like this:
#
#   <build-root>/tools
#               /<project1>
#                    /Debug
#               /<project2>
#                    /Debug
#               /<project3>
#                    /Debug
#
# This file assumes the DD_BUILD_ROOT environment variable is defined.
# This is part of the build environment, and will be different for each
# installation.
################################################################################

DEBUG = 1



################################################################################
### Directory Paths

# It looks like nmake, MIDL, and a few other tools cannot handle
# quoted pathnames with spaces in them. So, the compiler binaries are
# in a special path that has no spaces in its name.
COMPILER_DIR=

OUTDIR=.\Debug

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 




################################################################################
### Compiler and Linker Flags



########################
# Compiler
#
# NOTE: Do NOT specify the path. Tools like Prefast use a different
# compiler executable and they work by changing the path environment
# variable to use theirs.
CPP=cl.exe

COMMON_CPP_FLAGS=/nologo \
                  /W3 \
                  /Gm \
                  /ZI \
                  /Od \
                  /EHsc \
                  /D "WIN32" \
                  /D "_WINDOWS" \
                  /D "_DEBUG" \
                  /D "_MBCS" \
                  /D WIN32 \
                  /D _WIN32_WINNT=0x0501 \
                  /D VC_EXTRALEAN \
                  /D _CRT_SECURE_NO_WARNINGS \
                  /D _MT=1 \
                  /D DD_DEBUG=1 \
                  /D INCLUDE_REGRESSION_TESTS=0 \
                  /D GOTO_DEBUGGER_ON_WARNINGS=1 \
                  /D CONSISTENCY_CHECKS=0 \
                  /Fo"$(OUTDIR)\\" \
                  /Fd"$(OUTDIR)\\" \
                  /FD \
                  /RTC1 \
                  $(EXTRA_CPP_FLAGS) \
                  /I$(MSVCDir)\ATLMFC\INCLUDE \
                  /I$(MSVCDir)\INCLUDE \
                  /I$(PLATFORM_SDK_PATH_INCLUDE) \
                  /I$(FrameworkSDKDir)\include \
                  /I$(DD_BUILD_ROOT)\buildingBlocks \
                  /I$(DD_BUILD_ROOT)\basicServer \
                  /I$(MSVCDir)\include \
                  /I$(MSVCDir)\atlmfc\include \
                  /I$(OUTDIR) \
                  /c

# NOTE: Compile with either MDd or MD. These use the dynamic library
# for the C runtime (MDd is debug, MD is release). The dynamic linking
# will require that something like msvcr71d.dll or msvcr71.dll exist somewhere
# on your system. That sucks, but those are redistributable, so an installer
# can install them in a customer machine. The alternative is to specify
# MTd or MT, which uses the static libraries libcmt. Besides larger
# executables, there can be problems if two COM objects or dlls in the
# same address space are both statically linked. So, to be safe I use the
# dynamic option.
#
!if "$(DEBUG)" == "1"
COMMON_CPP_FLAGS = /MTd $(COMMON_CPP_FLAGS)
!else
COMMON_CPP_FLAGS = /MT $(COMMON_CPP_FLAGS) 
!endif


########################
# Linker
LINK=link.exe
            
COMMON_LINK_FLAGS=/nologo \
            /MACHINE:IX86 \
            /DEBUG \
            /LIBPATH:$(MSVCDir)\lib \
            /LIBPATH:$(PLATFORM_SDK_PATH_LIB) \
            /LIBPATH:$(MSVCDir)\atlmfc\lib

COMMON_LINK_LIBRARIES=libcmtd.lib
## libcmtd.lib
			            

########################
# MIDL
MIDL=midl.exe 

COMMON_MIDL_FLAGS=/I$(PLATFORM_SDK_PATH_INCLUDE) \
                  /nologo \
                  /env win32 \
                  /ms_ext \
                  /c_ext \
                  /app_config \
                  /cpp_cmd $(CPP) \
                  /out $(OUTDIR)


########################
# Type Libraries
MTL=midl.exe
COMMON_MTL_FLAGS=/nologo /D "_DEBUG" /win32


########################
# RESOURCES
COMMON_RES_FLAGS=/I$(PLATFORM_SDK_PATH_INCLUDE) \
                  /I$(MSVCDir)\atlmfc\include \
                  /r


################################################################################
### File Rules

# Deleting .h files from the output directory is potentially dangerous.
# However, we MUST do this, to clean out the results of previous MIDL
# builds. Otherwise, the IDL is not generated, since it depends on the
# generated .h file to trigger that rule, and we cannot build.
DELETE_BUILD_OUTPUT_FILES = \
   @del /F /Q "$(OUTDIR)\*.obj" & \
   @del /F /Q "$(OUTDIR)\*.lib" & \
   @del /F /Q "$(OUTDIR)\*.pch" & \
   @del /F /Q "$(OUTDIR)\*.pdb" & \
   @del /F /Q "$(OUTDIR)\*.idb" & \
   @del /F /Q "$(OUTDIR)\*.dll" & \
   @del /F /Q "$(OUTDIR)\*.res" & \
   @del /F /Q "$(OUTDIR)\*_p.c" & \
   @del /F /Q "$(OUTDIR)\*_i.c" & \
   @del /F /Q "$(OUTDIR)\*.ilk" & \
   @del /F /Q "$(OUTDIR)\*.tlb" & \
   @del /F /Q "$(OUTDIR)\*.exp" & \
   @del /F /Q "$(OUTDIR)\*.exe" & \
   @del /F /Q "$(OUTDIR)\*.h" & \
   @del /F /Q "$(OUTDIR)\*.manifest" & \
   @del /F /Q "$(OUTDIR)\dlldata.c" & \
   $(_VC_MANIFEST_CLEAN)
   

   
################################################################################
### File Rules
#
# The syntax of rules is:
# {fromPath}.fromExtension{toPath}.toExtension:
#    commands
#
# Both paths are optional.
# The :: means this is a batch mode rule, which means it is invoked once for all
# files that trigger it. In other words, the compiler is run once, on all *.cpp
# files.
#
# $< is a variable that is expanded to the file that triggered the rule.
#
   
{$(OUTDIR)}.c{$(OUTDIR)}.obj::
   @$(CPP) $(COMMON_CPP_FLAGS) $< 

.c{$(OUTDIR)}.obj::
   @$(CPP) $(COMMON_CPP_FLAGS) $< 
   
.cpp{$(OUTDIR)}.obj::
   @$(CPP) $(COMMON_CPP_FLAGS) $< 

.cxx{$(OUTDIR)}.obj::
   @$(CPP) $(COMMON_CPP_FLAGS) $< 

.h{$(OUTDIR)}.idl::
   @$(MIDL) $(COMMON_MIDL_FLAGS) $< 


