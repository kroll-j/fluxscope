all:	libflux
	make -C libflux
	g++ -O2 -g src/main.cpp -Ilibflux/src -Llibflux/lib -lGL -lGLU -lSDL -lSDL_image -ljack -lflux-gl_sdl

