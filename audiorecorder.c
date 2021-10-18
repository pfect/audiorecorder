#include <string.h>
#include <gst/gst.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "audiorecorder.h"
#include "ini.h"
#include "log.h"
#define INI_FILE "audiorecorder.ini"
#define FILEDIR_MAX_LEN	 200

char *target_directory = "";


/*  GST_DEBUG=3 \
	gst-launch-1.0 videotestsrc pattern=ball background-color=0x80808080 \
	num-buffers=100 "video/x-raw,framerate=5/1" ! \
	tee name=t \
	t. ! queue ! x264enc tune=zerolatency ! matroskamux ! filesink location=264.mkv \
	t. ! autovideosink

See also:
	* https://gstreamer.freedesktop.org/documentation/tutorials/basic/dynamic-pipelines.html?gi-language=c
	* https://coaxion.net/blog/2014/01/gstreamer-dynamic-pipelines/
	* https://github.com/sdroege/gst-snippets/blob/217ae015aaddfe3f7aa66ffc936ce93401fca04e/dynamic-filter.c
	* https://gstreamer.freedesktop.org/documentation/x264/index.html?gi-language=c#x264enc-page
	* https://bash.cyberciti.biz/guide/Sending_signal_to_Processes
	* https://oz9aec.net/software/gstreamer/pulseaudio-device-names
	* https://github.com/Igalia/aura/blob/master/src/pipeline.cpp
*/

static GMainLoop *loop;
static GstElement *pipeline, *src, *tee, *encoder, *muxer, *filesink, *audioconvert, *fakesink, *queue_record, *queue_fakesink;
static GstBus *bus;
static GstPad *teepad;
static gboolean recording = FALSE;
static gboolean unlinked = FALSE;

static gboolean
message_cb (GstBus * l_bus, GstMessage * message, gpointer user_data)
{
	GError *err = NULL;
	gchar *name, *debug = NULL;

	switch (GST_MESSAGE_TYPE (message)) {
		case GST_MESSAGE_ERROR:

			name = gst_object_get_path_string (message->src);
			gst_message_parse_error (message, &err, &debug);
			log_error("[%d] ERROR: from element %s: %s ", getpid(), name, err->message);
			if (debug != NULL)
				log_error("[%d] Debug info: %s ", getpid(),debug);

			g_error_free (err);
			g_free (debug);
			g_free (name);
			g_main_loop_quit (loop);
			break;
		case GST_MESSAGE_WARNING:
			name = gst_object_get_path_string (message->src);
			gst_message_parse_warning (message, &err, &debug);
			log_error("[%d] ERROR: from element %s: %s ", getpid(),name, err->message);
			if (debug != NULL)
				log_error("[%d] Debug info: %s ", getpid(),debug);

			g_error_free (err);
			g_free (debug);
			g_free (name);
			break;
		case GST_MESSAGE_EOS:
			log_info("[%d] Got EOS", getpid());
			g_main_loop_quit (loop);
			gst_element_set_state (pipeline, GST_STATE_NULL);
			g_main_loop_unref (loop);
			gst_object_unref (pipeline);
			exit(0);
			break;
		default:
			break;
	}

	return TRUE;
}

static GstPadProbeReturn unlink_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
	log_info("[%d] Unlinking", getpid());
	GstPad *sinkpad;
	sinkpad = gst_element_get_static_pad (queue_record, "sink");
	gst_pad_unlink (teepad, sinkpad);
	gst_object_unref (sinkpad);
	gst_element_send_event(encoder, gst_event_new_eos()); 
	// sleep(1);
	gst_bin_remove(GST_BIN (pipeline), queue_record);
	gst_bin_remove(GST_BIN (pipeline), encoder);
	gst_bin_remove(GST_BIN (pipeline), muxer);
	gst_bin_remove(GST_BIN (pipeline), filesink);
	gst_element_set_state(queue_record, GST_STATE_NULL);
	gst_element_set_state(encoder, GST_STATE_NULL);
	gst_element_set_state(muxer, GST_STATE_NULL);
	gst_element_set_state(filesink, GST_STATE_NULL);
	gst_object_unref(queue_record);
	gst_object_unref(encoder);
	gst_object_unref(muxer);
	gst_object_unref(filesink);
	gst_element_release_request_pad (tee, teepad);
	gst_object_unref (teepad);
	log_info("[%d] Unlinked", getpid());
	unlinked = TRUE; 
	return GST_PAD_PROBE_REMOVE;
}

void stopRecording(void) {
	log_info("[%d] Record stop", getpid());  
	gst_pad_add_probe(teepad, GST_PAD_PROBE_TYPE_IDLE, unlink_cb, NULL, (GDestroyNotify) g_free);
	recording = FALSE;
}

char *time_stamp(){
	char *timestamp = (char *)malloc(sizeof(char) * 18);
	memset(timestamp,0,18);
	time_t ltime;
	ltime=time(NULL);
	struct tm *tm;
	tm=localtime(&ltime);
	sprintf(timestamp,"%04d%02d%02d_%02d%02d%02d", tm->tm_year+1900, tm->tm_mon, 
		tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
	return timestamp;
}

void startRecording(void) {
	log_info("[%d] Record start", getpid()); 
	GstPad *sinkpad;
	GstPadTemplate *templ;
	templ = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(tee), "src_%u");
	teepad = gst_element_request_pad(tee, templ, NULL, NULL);
	queue_record = gst_element_factory_make("queue", "queue_record");
	encoder = gst_element_factory_make("opusenc", NULL);
	g_object_set (G_OBJECT ( encoder ), "bitrate", 16000, NULL); 	// 6000 - 128000
	g_object_set (G_OBJECT ( encoder ), "audio-type", 2048, NULL);	// 2048 = voice, 2049 = Generic
	g_object_set (G_OBJECT ( encoder ), "bandwidth", 1103, NULL);	// 1101 narrowband 1102 medium band fullband (1105) 
	muxer = gst_element_factory_make("oggmux", NULL);
	filesink = gst_element_factory_make("filesink", NULL);
	char *file_name = (char*) malloc(255 * sizeof(char));
	sprintf(file_name, "%s.opus", time_stamp() );
	log_info("[%d] Recording to file: %s", getpid(),file_name);
	g_object_set(filesink, "location", file_name, NULL);
	free(file_name);
	gst_bin_add_many(GST_BIN(pipeline), gst_object_ref(queue_record), gst_object_ref(encoder), gst_object_ref(muxer), gst_object_ref(filesink), NULL);
	gst_element_link_many(queue_record, encoder, muxer, filesink, NULL);
	gst_element_sync_state_with_parent(queue_record);
	gst_element_sync_state_with_parent(encoder);
	gst_element_sync_state_with_parent(muxer);
	gst_element_sync_state_with_parent(filesink);
	sinkpad = gst_element_get_static_pad(queue_record, "sink");
	gst_pad_link(teepad, sinkpad);
	gst_object_unref(sinkpad);
	recording = TRUE;
}

void sigintHandler(int sig) {
	if ( sig == 18 )
	{
		log_info("[%d] Received: SIGCONT, continuing new file.", getpid());	
		if (recording) {
			/* Get current filename */
			char *sinkfilename; 
			g_object_get(filesink, "location", &sinkfilename, NULL);
			log_info("[%d] Produced file: %s", getpid(), sinkfilename);	
			stopRecording();
			while ( unlinked == FALSE );
			unlinked=FALSE;
			/* (atomic) move file to output directory */
			char *targetfileanddirectory = (char *)malloc(sizeof(char) * FILEDIR_MAX_LEN); 
			memset(targetfileanddirectory,0,FILEDIR_MAX_LEN);
			sprintf(targetfileanddirectory,"%s/%s",target_directory,sinkfilename);
			log_info("[%d] Moving to target directory: %s", getpid(),targetfileanddirectory);	
			rename(sinkfilename, targetfileanddirectory);
			/* Start new recording */
			startRecording();
			signal(SIGCONT, sigintHandler);
		}
		else
		{
			startRecording();
		}
	}
}

int main(int argc, char *argv[])
{
	ini_t *config = ini_load(INI_FILE);
	char *audiosrc;
	log_info("[%d] audiorecorder ", getpid());
	ini_sget(config, "audiorecorder", "targetdirectory", NULL, &target_directory);
	ini_sget(config, "audiorecorder", "audiosource", NULL, &audiosrc);
	log_trace("[%d] target directory (%s) ",getpid(),target_directory);
	log_trace("[%d] audio source (%s) ",getpid(),audiosrc);
	log_info("[%d] SIGCONT will cut file ( kill -18 [pid] ) ", getpid());
	
	/* Create directories if they do not exist */
	struct stat st = {0};
	if (stat(target_directory, &st) == -1) {
		log_trace("[%d] Creating directory (%s) ",getpid(),target_directory);
		mkdir(target_directory, 0755);
	}
	
	signal(SIGCONT, sigintHandler); 
	gst_init (&argc, &argv);
	pipeline = gst_pipeline_new(NULL);
	src = gst_element_factory_make(audiosrc, NULL); 
	// g_object_set(src, "do-timestamp", TRUE, NULL);
	tee = gst_element_factory_make("tee", NULL);
	audioconvert = gst_element_factory_make("audioconvert", NULL);	
	queue_fakesink = gst_element_factory_make("queue", "queue_fakesink");
	fakesink = gst_element_factory_make("fakesink", NULL);
	if (!pipeline || !src  || !tee ||  !audioconvert || !fakesink || !queue_fakesink) {
		log_error("[%d] Failed to create elements", getpid()); 
		return -1;
	}
	gst_bin_add_many(GST_BIN(pipeline), src, tee, queue_fakesink, audioconvert, fakesink, NULL);
	if (!gst_element_link_many(src, tee, NULL) || !gst_element_link_many(tee, queue_fakesink,audioconvert,fakesink, NULL)) {
		log_error("[%d] Failed to link elements", getpid());
		return -2;
	}
	startRecording();
	loop = g_main_loop_new(NULL, FALSE);
	bus = gst_pipeline_get_bus(GST_PIPELINE (pipeline));
	gst_bus_add_signal_watch(bus);
	g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(message_cb), NULL);
	gst_object_unref(GST_OBJECT(bus));
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
	log_info("[%d] Starting", getpid());
	g_main_loop_run(loop);
	return 0;
}
