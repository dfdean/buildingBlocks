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
# Building Blocks Makefile
#
################################################################################

PROJECT_FILE_NAME_STEM = buildingBlocks

# Before including makeFileCommonWin32.mak, replace the compiler flags with these.
# The default is to use MSVCRTD.LIB, but MFC applications need libcmt.
# /MTd means link with libcmt
# /MDd means link with MSVCRTD.LIB debug lib
LIBC_COMPILER_FLAG=/MTd

!INCLUDE ..\buildingBlocks\makeFileCommonWin32.mak




################################################################################
### Build Targets

ALL : "$(OUTDIR)\buildingBlocks.lib"

CLEAN :
   $(DELETE_BUILD_OUTPUT_FILES)

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"





################################################################################
### Dependencies

LIB32_FLAGS=$(COMMON_LINK_FLAGS) /out:"$(OUTDIR)\buildingBlocks.lib" 

LIB32_OBJS= \
      "$(OUTDIR)\buildingBlocksManager.obj" \
      "$(OUTDIR)\osIndependantLayer.obj" \
      "$(OUTDIR)\stringLib.obj" \
      "$(OUTDIR)\config.obj" \
      "$(OUTDIR)\log.obj" \
      "$(OUTDIR)\debugging.obj" \
      "$(OUTDIR)\threads.obj" \
      "$(OUTDIR)\memAlloc.obj" \
      "$(OUTDIR)\refCount.obj" \
      "$(OUTDIR)\rbTree.obj" \
      "$(OUTDIR)\nameTable.obj" \
      "$(OUTDIR)\queue.obj" \
      "$(OUTDIR)\jobQueue.obj" \
      "$(OUTDIR)\fileUtils.obj" \
      "$(OUTDIR)\stringParse.obj" \
      "$(OUTDIR)\url.obj" \
      "$(OUTDIR)\blockIO.obj" \
      "$(OUTDIR)\memoryBlockIO.obj" \
      "$(OUTDIR)\fileBlockIO.obj" \
      "$(OUTDIR)\netBlockIO.obj" \
      "$(OUTDIR)\asyncIOStream.obj" \
      "$(OUTDIR)\polyHttpStream.obj" \
      "$(OUTDIR)\polyHttpStreamBasic.obj" \
      "$(OUTDIR)\polyXMLDoc.obj" \
      "$(OUTDIR)\polyXMLDocText.obj" \
      "$(OUTDIR)\serializedObject.obj"



"$(OUTDIR)\buildingBlocks.lib" : "$(OUTDIR)" $(DEF_FILE) $(LIB32_OBJS)
    @$(LINK) -lib $(LIB32_FLAGS) $(DEF_FLAGS) $(LIB32_OBJS)


# Create a dependency from object files onto header files.
# Unfortunately, I cannot use a wildcard in the target,
# so I have to make a separate target for each .c file.
# Moreover, I cannot easily specify which header files a target
# depends on without manually updating the makefile each time
# I add or remove an #include statement. Visual Studio does
# this; it reads the .cpp files and locates all #include files,
# and then generates a separate <proj>.dep file, which records these
# dependencies. This file is then included in the makefile.
# That's too risky to do manually, so instead I'm making a blanket 
# dependency on all header files.
"$(OUTDIR)\config.obj" : .\*.h
"$(OUTDIR)\buildingBlocksManager.obj" : .\*.h
"$(OUTDIR)\debugging.obj" : .\*.h
"$(OUTDIR)\fileBlockIO.obj" : .\*.h
"$(OUTDIR)\jobQueue.obj" : .\*.h
"$(OUTDIR)\threads.obj" : .\*.h
"$(OUTDIR)\log.obj" : .\*.h
"$(OUTDIR)\memAlloc.obj" : .\*.h
"$(OUTDIR)\fileUtils.obj" : .\*.h
"$(OUTDIR)\memoryBlockIO.obj" : .\*.h
"$(OUTDIR)\netBlockIO.obj" : .\*.h
"$(OUTDIR)\nameTable.obj" : .\*.h
"$(OUTDIR)\rbTree.obj" : .\*.h
"$(OUTDIR)\osIndependantLayer.obj" : .\*.h
"$(OUTDIR)\queue.obj" : .\*.h
"$(OUTDIR)\refCount.obj" : .\*.h
"$(OUTDIR)\stringLib.obj" : .\*.h
"$(OUTDIR)\stringParse.obj" : .\*.h
"$(OUTDIR)\url.obj" : .\*.h
"$(OUTDIR)\blockIO.obj" : .\*.h
"$(OUTDIR)\asyncIOStream.obj" : .\*.h
"$(OUTDIR)\polyXMLDoc.obj" : .\*.h
"$(OUTDIR)\polyXMLDocText.obj" : .\*.h
"$(OUTDIR)\polyHttpStream.obj" : .\*.h
"$(OUTDIR)\polyHttpStreamBasic.obj" : .\*.h
"$(OUTDIR)\serializedObject.obj" : .\*.cpp


## WARNING! Do NOT put a blank line above here. It will be interpreted as an empty rule
# for the last .obj file, so nmake will do nothing to create that last .obj file.





