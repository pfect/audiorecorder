CC=gcc

EXTRA_WARNINGS = -Wall 

GST_LIBS=`pkg-config --libs gstreamer-1.0`
GST_CFLAGS=`pkg-config --cflags gstreamer-1.0`

CFLAGS=-ggdb $(EXTRA_WARNINGS)

BINS=audiorecorder

audiorecorder:	audiorecorder.c log.c ini.c
	 $(CC) $+ $(CFLAGS) $(GST_CFLAGS) $(GST_LIBS) -o $@ -I.

clean:
	rm -rf $(BINS)
	rm *.opus

