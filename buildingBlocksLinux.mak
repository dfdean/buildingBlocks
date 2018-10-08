#############################################################################
# Makefile for building BuildingBlocks Library
#############################################################################

#############################################################################
# Compiler, tools and options
#   -c will generate a library, not a final executable.
#   -g will include debug information.
#   -pg will include gprof profiling.
#
CC = g++
CFLAGS = -c -Wall -W -g \
   -fno-strength-reduce -static \
   -D"LINUX" \
   -DLINUX \
   -D"_DEBUG" \
   -DDD_DEBUG=1 \
   -DINCLUDE_REGRESSION_TESTS=1 \
   -DGOTO_DEBUGGER_ON_WARNINGS=1 \
   -DCONSISTENCY_CHECKS=1 
   
INCPATH = -I$(QTDIR)/include

LINK = ar
LFLAGS = rcs
LIBS = 

OUTPUT_DIR = obj



#############################################################################
# Files

HEADERS =

SOURCES = buildingBlocksManager.cpp \
   osIndependantLayer.cpp \
   stringLib.cpp \
   config.cpp \
   log.cpp \
   debugging.cpp \
   memAlloc.cpp \
   refCount.cpp \
   threads.cpp \
   fileUtils.cpp \
   queue.cpp \
   jobQueue.cpp \
   stringParse.cpp \
   rbTree.cpp \
   nameTable.cpp \
   url.cpp \
   blockIO.cpp \
   memoryBlockIO.cpp \
   fileBlockIO.cpp \
   netBlockIO.cpp \
   asyncIOStream.cpp \
   polyHttpStream.cpp \
   polyHttpStreamBasic.cpp \
   polyXMLDoc.cpp \
   polyXMLDocText.cpp \
   serializedObject.cpp



OBJECTS = \
   $(OUTPUT_DIR)/buildingBlocksManager.o \
   $(OUTPUT_DIR)/osIndependantLayer.o \
   $(OUTPUT_DIR)/stringLib.o \
   $(OUTPUT_DIR)/config.o \
   $(OUTPUT_DIR)/log.o \
   $(OUTPUT_DIR)/debugging.o \
   $(OUTPUT_DIR)/memAlloc.o \
   $(OUTPUT_DIR)/refCount.o \
   $(OUTPUT_DIR)/threads.o \
   $(OUTPUT_DIR)/fileUtils.o \
   $(OUTPUT_DIR)/queue.o \
   $(OUTPUT_DIR)/jobQueue.o \
   $(OUTPUT_DIR)/stringParse.o \
   $(OUTPUT_DIR)/rbTree.o \
   $(OUTPUT_DIR)/nameTable.o \
   $(OUTPUT_DIR)/url.o \
   $(OUTPUT_DIR)/blockIO.o \
   $(OUTPUT_DIR)/memoryBlockIO.o \
   $(OUTPUT_DIR)/fileBlockIO.o \
   $(OUTPUT_DIR)/netBlockIO.o \
   $(OUTPUT_DIR)/asyncIOStream.o \
   $(OUTPUT_DIR)/polyHTTPStream.o \
   $(OUTPUT_DIR)/polyHTTPStreamBasic.o \
   $(OUTPUT_DIR)/polyXMLDoc.o \
   $(OUTPUT_DIR)/polyXMLDocText.o \
   $(OUTPUT_DIR)/serializedObject.o


TARGET = $(OUTPUT_DIR)/libbuildingBlocks.a


#############################################################################
# Implicit rules

.SUFFIXES: .cpp .c

$(OUTPUT_DIR)/%.o:
	$(CC) -c $(CFLAGS) $(INCPATH) -o $@ $<


#############################################################################
# Build rules

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(LINK) $(LFLAGS) $(TARGET) $(OBJECTS)

clean:
	-rm -f $(OBJECTS) $(TARGET)
	-rm -f ~/core


#############################################################################
# Compile
$(OUTPUT_DIR)/osIndependantLayer.o: osIndependantLayer.cpp osIndependantLayer.h
$(OUTPUT_DIR)/buildingBlocksManager.o: buildingBlocksManager.cpp osIndependantLayer.h
$(OUTPUT_DIR)/stringLib.o: stringLib.cpp osIndependantLayer.h stringLib.h
$(OUTPUT_DIR)/config.o: config.cpp osIndependantLayer.h stringLib.h config.h
$(OUTPUT_DIR)/log.o: log.cpp log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/debugging.o: debugging.cpp debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/memAlloc.o: memAlloc.cpp memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/refCount.o: refCount.cpp refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/threads.o: threads.cpp threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/fileUtils.o: fileUtils.cpp fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/queue.o: queue.cpp queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/jobQueue.o: jobQueue.cpp jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/stringParse.o: stringParse.cpp stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/rbTree.o: rbTree.cpp rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/nameTable.o: nameTable.cpp nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/url.o: url.cpp url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/blockIO.o: blockIO.cpp blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/memoryBlockIO.o: memoryBlockIO.cpp blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/fileBlockIO.o: fileBlockIO.cpp blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/netBlockIO.o: netBlockIO.cpp blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/asyncIOStream.o: asyncIOStream.cpp asyncIOStream.h blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/polyHTTPStream.o: polyHTTPStream.cpp polyHTTPStream.h asyncIOStream.h blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/polyHTTPStreamBasic.o: polyHTTPStreamBasic.cpp polyHTTPStream.h asyncIOStream.h blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/polyXMLDoc.o: polyXMLDoc.cpp polyXMLDoc.h polyHTTPStream.h asyncIOStream.h blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/polyXMLDocText.o: polyXMLDocText.cpp polyXMLDoc.h polyHTTPStream.h asyncIOStream.h blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h
$(OUTPUT_DIR)/serializedObject.o: serializedObject.cpp serializedObject.h polyXMLDoc.h polyHTTPStream.h asyncIOStream.h blockIO.h url.h nameTable.h rbTree.h stringParse.h jobQueue.h queue.h fileUtils.h threads.h refCount.h memAlloc.h debugging.h log.h config.h stringLib.h osIndependantLayer.h

