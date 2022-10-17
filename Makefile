all:
	gcc -o mux mux.c .libs/libfdk-aac.a .libs/libfdk-aac.so -lm -I.
	gcc -o demux demux.c .libs/libfdk-aac.a .libs/libfdk-aac.so -lm -I.
clean:
	rm -rf *.o *.mp4 mux demux