CFLAGS	:=  -std=c++17 -O3 -DGC_MARK_SWEEP -I/usr/include/SDL2

CC = g++
SRC := ../src
OBJS := objmemory.o bitblt.o interpreter.o main_sdl.o

Smalltalk: $(OBJS)
	$(CC) -o $@ $^ -L/usr/local/lib -lSDL2main -lSDL2

main_sdl.o: ../main_sdl.cpp
	$(CC) $(CFLAGS) -I$(SRC) -c ../main_sdl.cpp -o main_sdl.o

%.o: $(SRC)/%.cpp
	$(CC) $(CFLAGS) -c $<  -o $@


clean:
	rm -f *.o Smalltalk
