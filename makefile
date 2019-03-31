CXXFLAGS := -O3
CXX = g++

VIDEO := 
SOURCEDIR = source
BUILDIR = build
TARGET = player

SOURCE := $(wildcard $(SOURCEDIR)/*.cpp)

DEPS := $(SOURCE:.cpp=.d)

OBJS = $(SOURCE:.cpp=.o)

LIBS := -lSDL2 -lpthread -I /usr/local/include -L /usr/local/lib -lavformat -lavcodec -lswscale -lavutil -lavfilter -lswresample -lavdevice -lz -lrt -lm 

.PHONY: clean all

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS)  $(LIBS) -o $(TARGET)
	make clean

$(SOURCEDIR)/%.o: $(SOURCEDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SOURCEDIR)/%.d: $(SOURCEDIR)/%.cpp 
	$(CXX) -M $(CXXFLAGS) $< > $@

clean:
	rm -f $(OBJS) $(DEPS)

-include $(DEPS)
