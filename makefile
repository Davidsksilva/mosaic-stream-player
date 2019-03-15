CXXFLAGS := -O3
CXX = gcc

VIDEO := 
SOURCEDIR = source
BUILDIR = build
TARGET = player

SOURCE := $(wildcard $(SOURCEDIR)/*.c)

DEPS := $(SOURCE:.c=.d)

OBJS = $(SOURCE:.c=.o)

LIBS := -lSDL2 -lpthread -I /usr/local/include -L /usr/local/lib -lavformat -lavcodec -lswscale -lavutil -lavfilter -lswresample -lavdevice -lz -llzma -lva -lrt -lm 

.PHONY: clean all

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS)  $(LIBS) -o $(TARGET)
	make clean

$(SOURCEDIR)/%.o: $(SOURCEDIR)/%.c
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SOURCEDIR)/%.d: $(SOURCEDIR)/%.c 
	$(CXX) -M $(CXXFLAGS) $< > $@

clean:
	rm -f $(OBJS) $(DEPS)

-include $(DEPS)
