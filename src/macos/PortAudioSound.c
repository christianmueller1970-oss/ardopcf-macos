//
// Audio interface Routine
//
// Passes audio samples to/from the sound interface
//
// As this is platform specific it also has the main() routine, which does
// platform specific initialisation before calling ardopmain()
//
// This is PortAudioSound.c for macOS (Darwin).
// It is derived from ALSASound.c (Linux); the ALSA calls are replaced
// with the PortAudio blocking API on top of CoreAudio.
// The Windows version is Waveout.c.
//
// Serial (CAT/RTS PTT) support is shared with Linux: src/linux/LinSerial.c
// is plain POSIX termios and is compiled unchanged on Darwin.

#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <portaudio.h>

#define HANDLE int

#include "common/ardopcommon.h"
#include "common/wav.h"
#include "rockliff/rrs.h"

int WriteLog(char * msg, int Log);
int _memicmp(unsigned char *a, unsigned char *b, int n);
int stricmp(const unsigned char * pStr1, const unsigned char *pStr2);
HANDLE OpenCOMPort(VOID * pPort, int speed, BOOL SetDTR, BOOL SetRTS, BOOL Quiet, int Stopbits);

VOID COMSetDTR(HANDLE fd);
VOID COMClearDTR(HANDLE fd);
VOID COMSetRTS(HANDLE fd);
VOID COMClearRTS(HANDLE fd);
VOID RadioPTT(int PTTState);
VOID SerialHostPoll();
VOID TCPHostPoll();
int CloseSoundCard();
int PackSamplesAndSend(short * input, int nSamples);
BOOL WriteCOMBlock(HANDLE fd, char * Block, int BytesToWrite);
VOID processargs(int argc, char * argv[]);
int wg_send_currentlevel(int cnum, unsigned char level);
int wg_send_pttled(int cnum, bool isOn);
int wg_send_pixels(int cnum, unsigned char *data, size_t datalen);
void WebguiPoll();

int add_noise(short *samples, unsigned int nSamples, short stddev);
short InputNoiseStdDev = 0;

extern BOOL blnDISCRepeating;

extern int SampleNo;
extern unsigned int pttOnTime;

BOOL UseLeftRX = TRUE;
BOOL UseRightRX = TRUE;

BOOL UseLeftTX = TRUE;
BOOL UseRightTX = TRUE;

extern BOOL WriteRxWav;
extern BOOL WriteTxWav;
extern BOOL FixTiming;
extern char DecodeWav[5][256];
extern int WavNow;  // Time since start of WAV file being decoded

extern struct sockaddr HamlibAddr;  // Dest for above
extern int useHamLib;


void Sleep(int mS)
{
	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
		usleep(mS * 1000);
	return;
}


// Windows and PortAudio work with signed samples +- 32767
short buffer[2][1200];  // Two Transfer/DMA buffers of 0.1 Sec
short inbuffer[2][1200];  // Two Transfer/DMA buffers of 0.1 Sec

BOOL Loopback = FALSE;

char CaptureDevice[80] = "DEFAULT";
char PlaybackDevice[80] = "DEFAULT";

char * CaptureDevices = CaptureDevice;
char * PlaybackDevices = CaptureDevice;

int InitSound();

int Ticks;

int LastNow;

extern int Number;  // Number waiting to be sent

long MaxAvail;

struct WavFile *rxwf = NULL;
struct WavFile *txwff = NULL;
// writing unfiltered tx audio to WAV disabled
// struct WavFile *txwfu = NULL;
#define RXWFTAILMS 10000;  // 10 seconds
unsigned int rxwf_EndNow = 0;

void extendRxwf()
{
	rxwf_EndNow = Now + RXWFTAILMS;
}

void StartRxWav()
{
	// Open a new WAV file if not already recording.
	// If already recording, then just extend the time before
	// recording will end.
	//
	// Wav files will use a filename that includes port, UTC date,
	// and UTC time, similar to log files but with added time to
	// the nearest second.  Like Log files, these Wav files will be
	// written to the Log directory if defined, else to the current
	// directory
	//
	// As currently implemented, the wav file written contains only
	// received audio.  Since nothing is written for the time while
	// transmitting, and thus not receiving, this recording is not
	// time continuous.  Thus, the filename indicates the time that
	// the recording was started, but the times of the received
	// transmissions, other than the first one, are not indicated.
	char rxwf_pathname[1024];
	int pnlen;

	if (rxwf != NULL)
	{
		// Already recording, so just extend recording time.
		extendRxwf();
		return;
	}
	struct tm * tm;
	time_t T;

	T = time(NULL);
	tm = gmtime(&T);

	struct timespec tp;
	int ss, hh, mm;
	clock_gettime(CLOCK_REALTIME, &tp);
	ss = tp.tv_sec % 86400;  // Seconds in a day
	hh = ss / 3600;
	mm = (ss - (hh * 3600)) / 60;
	ss = ss % 60;

	if (ardop_log_get_directory()[0])
		pnlen = snprintf(rxwf_pathname, sizeof(rxwf_pathname),
			"%s/ARDOP_rxaudio_%d_%04d%02d%02d_%02d%02d%02d.wav",
			ardop_log_get_directory(), port, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
			hh, mm, ss);
	else
		pnlen = snprintf(rxwf_pathname, sizeof(rxwf_pathname),
			"ARDOP_rxaudio_%d_%04d%02d%02d_%02d:%02d:%02d.wav",
			port, tm->tm_year +1900, tm->tm_mon+1, tm->tm_mday,
			hh, mm, ss);
	if (pnlen == -1 || pnlen > sizeof(rxwf_pathname)) {
		// Logpath too long likely to also prevent writing to log files.
		// So, print this error directly to console instead of using
		// WriteDebugLog.
		printf("Unable to write WAV file, invalid pathname. Logpath may be too long.\n");
		WriteRxWav = FALSE;
		return;
	}
	rxwf = OpenWavW(rxwf_pathname);
	extendRxwf();
}

// writing unfiltered tx audio to WAV disabled.  Only filtered
// tx audio will be written.  However, the code for unfiltered
// audio is left in place but commented out so that it can eaily
// be restored if desired.
void StartTxWav()
{
	// Open two new WAV files for filtered and unfiltered Tx audio.
	//
	// Wav files will use a filename that includes port, UTC date,
	// and UTC time, similar to log files but with added time to
	// the nearest second.  Like Log files, these Wav files will be
	// written to the Log directory if defined, else to the current
	// directory
	char txwff_pathname[1024];
	// char txwfu_pathname[1024];
	int pnflen;
	// int pnulen;

	if (txwff != NULL)  // || txwfu != NULL)
	{
		ZF_LOGW("WARNING: Trying to open Tx WAV file, but already open.");
		return;
	}
	struct tm * tm;
	time_t T;

	T = time(NULL);
	tm = gmtime(&T);

	struct timespec tp;
	int ss, hh, mm;
	clock_gettime(CLOCK_REALTIME, &tp);
	ss = tp.tv_sec % 86400;  // Seconds in a day
	hh = ss / 3600;
	mm = (ss - (hh * 3600)) / 60;
	ss = ss % 60;

	if (ardop_log_get_directory()[0])
	{
		pnflen = snprintf(txwff_pathname, sizeof(txwff_pathname),
			"%s/ARDOP_txfaudio_%d_%04d%02d%02d_%02d%02d%02d.wav",
			ardop_log_get_directory(), port, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
			hh, mm, ss);
	}
	else
	{
		pnflen = snprintf(txwff_pathname, sizeof(txwff_pathname),
			"ARDOP_txfaudio_%d_%04d%02d%02d_%02d:%02d:%02d.wav",
			port, tm->tm_year +1900, tm->tm_mon+1, tm->tm_mday,
			hh, mm, ss);
	}
	if (pnflen == -1 || pnflen > sizeof(txwff_pathname)) {
		// Logpath too long likely to also prevent writing to log files.
		// So, print this error directly to console instead of using
		// WriteDebugLog.
		printf("Unable to write WAV file, invalid pathname. Logpath may be too long.\n");
		WriteTxWav = FALSE;
		return;
	}
	txwff = OpenWavW(txwff_pathname);
	// txwfu = OpenWavW(txwfu_pathname);
}

void printtick(char * msg)
{
	ZF_LOGI("%s %i", msg, Now - LastNow);
	LastNow = Now;
}

struct timespec time_start;

unsigned int getTicks()
{
	struct timespec tp;

	// When decoding a WAV file, return WavNow, a measure of the offset
	// in ms from the start of the WAV file.
	if (DecodeWav[0][0])
		return WavNow;

	// Otherwise, return a measure of clock time (also measured in ms).
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (tp.tv_sec - time_start.tv_sec) * 1000 + (tp.tv_nsec - time_start.tv_nsec) / 1000000;
}

void PlatformSleep(int mS)
{
	TCPHostPoll();
	WebguiPoll();

	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
		Sleep(mS);

	if (PKTLEDTimer && Now > PKTLEDTimer)
	{
		PKTLEDTimer = 0;
		SetLED(PKTLED, 0);  // turn off packet rxed led
	}
}

/*
 * Lists common signal abbreviations
 *
 * This method is a portable version of glibc's sigabbrev_np().
 * It only supports a handful of signal names that ARDOP
 * currently catches and/or ignores. Unlike the glibc function,
 * the return value is always guaranteed to be non-NULL.
 */
const char* PlatformSignalAbbreviation(int signal) {
	switch (signal) {
		case SIGABRT:
			return "SIGABRT";
		case SIGINT:
			return "SIGINT";
		case SIGHUP:
			return "SIGHUP";
		case SIGPIPE:
			return "SIGPIPE";
		case SIGTERM:
			return "SIGTERM";
		default:
			return "Unknown";
	}
}

static void signal_handler_trigger_shutdown(int sig)
{
	blnClosing = TRUE;
	closedByPosixSignal = sig;
}

char * PortString = NULL;


int platform_main(int argc, char * argv[])
{
	struct sigaction act;
	// rslen_set[] must list all of the rslen values used.
	int rslen_set[] = {2, 4, 8, 16, 32, 36, 50, 64};
	init_rs(rslen_set, 8);

	char cmdstr[3000] = "";
	for (int i = 0; i < argc; i++) {
		if ((int)(sizeof(cmdstr) - strlen(cmdstr))
			<= snprintf(
				cmdstr + strlen(cmdstr),
				sizeof(cmdstr) - strlen(cmdstr),
				"%s ",
				argv[i])
		) {
			printf("ERROR: cmdstr[%ld] insufficient to hold fill command string for logging.\n", sizeof(cmdstr));
			break;
		}
	}

	processargs(argc, argv);

	setlinebuf(stdout);  // So we can redirect output to file and tail

	ZF_LOGI("%s Version %s (https://www.github.com/pflarue/ardop)", ProductName, ProductVersion);
	ZF_LOGI("Copyright (c) 2014-2024 Rick Muething, John Wiseman, Peter LaRue");
	ZF_LOGI(
		"See https://github.com/pflarue/ardop/blob/master/LICENSE for licence details including\n"
		"  information about authors of external libraries used and their licenses."
	);
	ZF_LOGD("Command line: %s", cmdstr);

	if (DecodeWav[0][0])
	{
		decode_wav();
		return (0);
	}

	if (HostPort[0])
		port = atoi(HostPort);

	if (CATPort[0])
	{
		char * Baud = strlop(CATPort, ':');
		if (Baud)
			CATBAUD = atoi(Baud);

		hCATDevice = OpenCOMPort(CATPort, CATBAUD, FALSE, FALSE, FALSE, 0);

	}

	if (PTTPort[0])
	{
		// Note: the Linux-only CM108 (/dev/hidraw) and Raspberry Pi GPIO
		// PTT variants are not available on macOS and are omitted here.

		char * Baud = strlop(PTTPort, ':');

		if (Baud)
			PTTBAUD = atoi(Baud);

		if (strcmp(CATPort, PTTPort) == 0)
		{
			hPTTDevice = hCATDevice;
		}
		else
		{
			if (Baud)
			{
				// Could be IPADDR:PORT or COMPORT:SPEED. See if first part is valid ip address

				struct sockaddr_in * destaddr = (struct sockaddr_in *)&HamlibAddr;

				destaddr->sin_family = AF_INET;
				destaddr->sin_addr.s_addr = inet_addr(PTTPort);
				destaddr->sin_port = htons(atoi(Baud));

				if (destaddr->sin_addr.s_addr != INADDR_NONE)
				{
					useHamLib = 1;
					ZF_LOGI("Using Hamlib at %s:%s for PTT", PTTPort, Baud);
					RadioControl = TRUE;
					PTTMode = PTTHAMLIB;
				}
				else
					PTTBAUD = atoi(Baud);
			}
			if (useHamLib == 0)
				hPTTDevice = OpenCOMPort(PTTPort, PTTBAUD, FALSE, FALSE, FALSE, 0);
		}
	}

	if (hCATDevice)
	{
		ZF_LOGI("CAT Control on port %s", CATPort);
		COMSetRTS(hPTTDevice);
		COMSetDTR(hPTTDevice);
		if (PTTOffCmdLen)
		{
			ZF_LOGI("PTT using CAT Port: %s", CATPort);
			RadioControl = TRUE;
		}
	}
	else
	{
		// Warn of -u and -k defined but no CAT Port

		if (PTTOffCmdLen)
		{
			ZF_LOGW("Warning PTT Off string defined but no CAT port");
		}
	}

	if (hPTTDevice)
	{
		ZF_LOGI("Using RTS on port %s for PTT", PTTPort);
		COMClearRTS(hPTTDevice);
		COMClearDTR(hPTTDevice);
		RadioControl = TRUE;
	}


	ZF_LOGI("ARDOPC listening on port %d", port);

	// Get Time Reference

	clock_gettime(CLOCK_MONOTONIC, &time_start);
	LastNow = getTicks();

	// Trap signals

	memset (&act, '\0', sizeof(act));

	act.sa_handler = &signal_handler_trigger_shutdown;
	if (sigaction(SIGINT, &act, NULL) < 0)
		perror ("SIGINT");

	act.sa_handler = &signal_handler_trigger_shutdown;
	if (sigaction(SIGTERM, &act, NULL) < 0)
		perror ("SIGTERM");

	act.sa_handler = SIG_IGN;

	if (sigaction(SIGHUP, &act, NULL) < 0)
		perror ("SIGHUP");

	if (sigaction(SIGPIPE, &act, NULL) < 0)
		perror ("SIGPIPE");

	ardopmain();

	return (0);
}

void txSleep(int mS)
{
	// called while waiting for next TX buffer or to delay response.
	// Run background processes

	TCPHostPoll();
	WebguiPoll();

	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
		Sleep(mS);

	if (PKTLEDTimer && Now > PKTLEDTimer)
	{
		PKTLEDTimer = 0;
		SetLED(PKTLED, 0);  // turn off packet rxed led
	}
}

// PortAudio Code
//
// The blocking PortAudio API maps directly onto the poll/blocking
// semantics the platform layer inherited from ALSA: SoundCardRead
// checks Pa_GetStreamReadAvailable before reading, SoundCardWrite
// waits for buffer space while running the background polls.
// All macOS devices are opened at 12000 Hz; paMacCore resamples
// to/from the device's native rate internally.
//
// Like the ALSA backend (SHARECAPTURE), only one direction is open at
// a time: the playback stream is opened when TX starts (StopCapture)
// and closed again at the end of TX (SoundFlush), which reopens the
// capture stream.

static PaStream * playStream = NULL;
static PaStream * recStream = NULL;

int m_playchannels = 1;
int m_recchannels = 1;

char SavedCaptureDevice[256];  // Saved so we can reopen
char SavedPlaybackDevice[256];

int Savedplaychannels = 1;

int SavedCaptureRate;
int SavedPlaybackRate;

char ** WriteDevices = NULL;
int WriteDeviceCount = 0;

char ** ReadDevices = NULL;
int ReadDeviceCount = 0;

static BOOL PaInitialized = FALSE;

static BOOL EnsurePaInitialized()
{
	PaError err;

	if (PaInitialized)
		return TRUE;

	err = Pa_Initialize();
	if (err != paNoError)
	{
		ZF_LOGE("Pa_Initialize failed: %s", Pa_GetErrorText(err));
		return FALSE;
	}
	PaInitialized = TRUE;
	return TRUE;
}

// Resolve a device argument to a PortAudio device index.  Accepted forms,
// tried in this order:
//   "DEFAULT"           the system default input/output device
//   a decimal number    PortAudio device index (as shown by the listing)
//   otherwise           exact device name, then unique case-insensitive
//                       substring of a device name
// Unlike the ALSA backend, the argument is never cut at the first space:
// CoreAudio device names regularly contain spaces ("USB Audio CODEC").
static PaDeviceIndex FindDevice(const char * spec, BOOL wantInput, char * ErrorMsg)
{
	int i;
	int count = Pa_GetDeviceCount();
	PaDeviceIndex match = paNoDevice;
	int matches = 0;
	BOOL allDigits = TRUE;

	if (stricmp((const unsigned char *)spec, (const unsigned char *)"DEFAULT") == 0)
	{
		PaDeviceIndex dev = wantInput ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
		if (dev == paNoDevice && ErrorMsg)
			sprintf(ErrorMsg, "no default %s device", wantInput ? "capture" : "playback");
		return dev;
	}

	for (i = 0; spec[i]; i++)
	{
		if (!isdigit((unsigned char)spec[i]))
		{
			allDigits = FALSE;
			break;
		}
	}

	if (allDigits && spec[0])
	{
		int idx = atoi(spec);
		if (idx >= 0 && idx < count)
			return (PaDeviceIndex)idx;
		if (ErrorMsg)
			sprintf(ErrorMsg, "audio device index %d out of range (0..%d)", idx, count - 1);
		return paNoDevice;
	}

	// Exact name match

	for (i = 0; i < count; i++)
	{
		const PaDeviceInfo * di = Pa_GetDeviceInfo(i);
		int channels = wantInput ? di->maxInputChannels : di->maxOutputChannels;

		if (channels > 0 && strcmp(di->name, spec) == 0)
			return (PaDeviceIndex)i;
	}

	// Unique case-insensitive substring match

	for (i = 0; i < count; i++)
	{
		const PaDeviceInfo * di = Pa_GetDeviceInfo(i);
		int channels = wantInput ? di->maxInputChannels : di->maxOutputChannels;
		size_t namelen = strlen(di->name);
		size_t speclen = strlen(spec);
		size_t off;

		if (channels <= 0 || speclen > namelen)
			continue;

		for (off = 0; off + speclen <= namelen; off++)
		{
			if (strncasecmp(di->name + off, spec, speclen) == 0)
			{
				if (match != (PaDeviceIndex)i)
				{
					match = (PaDeviceIndex)i;
					matches++;
				}
				break;
			}
		}
	}

	if (matches == 1)
		return match;

	if (ErrorMsg)
	{
		if (matches == 0)
			sprintf(ErrorMsg, "no %s device matches \"%s\"",
				wantInput ? "capture" : "playback", spec);
		else
			sprintf(ErrorMsg, "\"%s\" matches %d %s devices, be more specific",
				spec, matches, wantInput ? "capture" : "playback");
	}
	return paNoDevice;
}

int CheckifLoaded()
{
	// Prevent CTRL/C from closing the TNC
	// (This causes problems if the TNC is started by LinBPQ)

	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	return TRUE;
}

// Build a device list entry of the form "<index>: <name>" and log the
// device's capabilities the same way the ALSA backend does.
static char ** AddDeviceToList(char ** list, int * count, int index, const PaDeviceInfo * di)
{
	char NameString[300];

	snprintf(NameString, sizeof(NameString), "%d: %s", index, di->name);

	list = realloc(list, (*count + 1) * sizeof(char *));
	list[(*count)++] = strdup(NameString);
	return list;
}

int GetOutputDeviceCollection()
{
	int i, count;

	ZF_LOGI("Playback Devices\n");

	if (!EnsurePaInitialized())
		return 0;

	while (WriteDeviceCount)
		free(WriteDevices[--WriteDeviceCount]);
	free(WriteDevices);
	WriteDevices = NULL;

	count = Pa_GetDeviceCount();

	for (i = 0; i < count; i++)
	{
		const PaDeviceInfo * di = Pa_GetDeviceInfo(i);

		if (di->maxOutputChannels <= 0)
			continue;

		ZF_LOGI("  Device %d: `%s', %d channels, native rate %.0f Hz%s",
			i, di->name, di->maxOutputChannels, di->defaultSampleRate,
			i == Pa_GetDefaultOutputDevice() ? " (default)" : "");

		WriteDevices = AddDeviceToList(WriteDevices, &WriteDeviceCount, i, di);
	}

	return WriteDeviceCount;
}

int GetNextOutputDevice(char * dest, int max, int n)
{
	if (n >= WriteDeviceCount)
		return 0;

	strcpy(dest, WriteDevices[n]);
	return strlen(dest);
}


int GetInputDeviceCollection()
{
	int i, count;

	ZF_LOGI("Capture Devices\n");

	if (!EnsurePaInitialized())
		return 0;

	while (ReadDeviceCount)
		free(ReadDevices[--ReadDeviceCount]);
	free(ReadDevices);
	ReadDevices = NULL;

	count = Pa_GetDeviceCount();

	for (i = 0; i < count; i++)
	{
		const PaDeviceInfo * di = Pa_GetDeviceInfo(i);

		if (di->maxInputChannels <= 0)
			continue;

		ZF_LOGI("  Device %d: `%s', %d channels, native rate %.0f Hz%s",
			i, di->name, di->maxInputChannels, di->defaultSampleRate,
			i == Pa_GetDefaultInputDevice() ? " (default)" : "");

		ReadDevices = AddDeviceToList(ReadDevices, &ReadDeviceCount, i, di);
	}

	return ReadDeviceCount;
}

int GetNextInputDevice(char * dest, int max, int n)
{
	if (n >= ReadDeviceCount)
		return 0;

	strcpy(dest, ReadDevices[n]);
	return strlen(dest);
}

int OpenSoundPlayback(char * PlaybackDevice, int m_sampleRate, int channels, char * ErrorMsg)
{
	PaDeviceIndex dev;
	const PaDeviceInfo * di;
	PaStreamParameters params;
	PaError err;

	if (!EnsurePaInitialized())
		return false;

	if (playStream)
	{
		Pa_CloseStream(playStream);
		playStream = NULL;
	}

	dev = FindDevice(PlaybackDevice, FALSE, ErrorMsg);
	if (dev == paNoDevice)
	{
		if (!ErrorMsg)
			ZF_LOGE("cannot find playback audio device %s", PlaybackDevice);
		return false;
	}

	di = Pa_GetDeviceInfo(dev);

	memset(&params, 0, sizeof(params));
	params.device = dev;
	params.channelCount = channels;
	params.sampleFormat = paInt16;
	params.suggestedLatency = di->defaultHighOutputLatency;

	err = Pa_OpenStream(&playStream, NULL, &params, m_sampleRate,
		paFramesPerBufferUnspecified, paNoFlag, NULL, NULL);

	if (err != paNoError && channels == 1)
	{
		// Some devices refuse mono; retry in stereo like the ALSA backend
		channels = 2;
		params.channelCount = 2;
		err = Pa_OpenStream(&playStream, NULL, &params, m_sampleRate,
			paFramesPerBufferUnspecified, paNoFlag, NULL, NULL);
	}

	if (err != paNoError)
	{
		playStream = NULL;
		if (ErrorMsg)
			sprintf(ErrorMsg, "cannot open playback audio device %s (%s)",
				PlaybackDevice, Pa_GetErrorText(err));
		else
			ZF_LOGE("cannot open playback audio device %s (%s)",
				PlaybackDevice, Pa_GetErrorText(err));
		return false;
	}

	err = Pa_StartStream(playStream);
	if (err != paNoError)
	{
		Pa_CloseStream(playStream);
		playStream = NULL;
		if (ErrorMsg)
			sprintf(ErrorMsg, "cannot start playback stream (%s)", Pa_GetErrorText(err));
		else
			ZF_LOGE("cannot start playback stream (%s)", Pa_GetErrorText(err));
		return false;
	}

	Savedplaychannels = m_playchannels = channels;

	// The ring buffer of a freshly started blocking stream reports 0
	// writable frames until the CoreAudio callback has run, so the real
	// buffer size (used by the SoundFlush drain check) is tracked as
	// the largest value seen while writing.
	MaxAvail = Pa_GetStreamWriteAvailable(playStream);

	return true;
}

int OpenSoundCapture(char * CaptureDevice, int m_sampleRate, char * ErrorMsg)
{
	PaDeviceIndex dev;
	const PaDeviceInfo * di;
	PaStreamParameters params;
	PaError err;

	if (!EnsurePaInitialized())
		return false;

	if (recStream)
	{
		Pa_CloseStream(recStream);
		recStream = NULL;
	}

	dev = FindDevice(CaptureDevice, TRUE, ErrorMsg);
	if (dev == paNoDevice)
	{
		if (!ErrorMsg)
			ZF_LOGE("cannot find capture audio device %s", CaptureDevice);
		return false;
	}

	di = Pa_GetDeviceInfo(dev);

	m_recchannels = 1;

	if (UseLeftRX == 0 || UseRightRX == 0)
		m_recchannels = 2;  // L/R implies stereo

	memset(&params, 0, sizeof(params));
	params.device = dev;
	params.channelCount = m_recchannels;
	params.sampleFormat = paInt16;
	params.suggestedLatency = di->defaultHighInputLatency;

	err = Pa_OpenStream(&recStream, &params, NULL, m_sampleRate,
		paFramesPerBufferUnspecified, paNoFlag, NULL, NULL);

	if (err != paNoError && m_recchannels == 1)
	{
		// Some devices refuse mono; retry in stereo like the ALSA backend
		m_recchannels = 2;
		params.channelCount = 2;
		err = Pa_OpenStream(&recStream, &params, NULL, m_sampleRate,
			paFramesPerBufferUnspecified, paNoFlag, NULL, NULL);
	}

	if (err != paNoError)
	{
		recStream = NULL;
		if (ErrorMsg)
			sprintf(ErrorMsg, "cannot open capture audio device %s (%s)",
				CaptureDevice, Pa_GetErrorText(err));
		else
			ZF_LOGE("cannot open capture audio device %s (%s)",
				CaptureDevice, Pa_GetErrorText(err));
		return false;
	}

	err = Pa_StartStream(recStream);  // without this avail stays at 0
	if (err != paNoError)
	{
		Pa_CloseStream(recStream);
		recStream = NULL;
		if (ErrorMsg)
			sprintf(ErrorMsg, "cannot start capture stream (%s)", Pa_GetErrorText(err));
		else
			ZF_LOGE("cannot start capture stream (%s)", Pa_GetErrorText(err));
		return FALSE;
	}

	return TRUE;
}

int OpenSoundCard(char * CaptureDevice, char * PlaybackDevice, int c_sampleRate, int p_sampleRate, char * ErrorMsg)
{
	int Channels = 1;

	if (UseLeftRX == 1 && UseRightRX == 1)
	{
		ZF_LOGI("Using Both Channels of soundcard for RX");
	}
	else
	{
		if (UseLeftRX == 0)
			ZF_LOGI("Using Right Channel of soundcard for RX");
		if (UseRightRX == 0)
			ZF_LOGI("Using Left Channel of soundcard for RX");
	}

	if (UseLeftTX == 1 && UseRightTX == 1)
	{
		ZF_LOGI("Using Both Channels of soundcard for TX");
	}
	else
	{
		if (UseLeftTX == 0)
			ZF_LOGI("Using Right Channel of soundcard for TX");
		if (UseRightTX == 0)
			ZF_LOGI("Using Left Channel of soundcard for TX");
	}

	ZF_LOGI("Opening Playback Device %s Rate %d", PlaybackDevice, p_sampleRate);

	if (UseLeftTX == 0 || UseRightTX == 0)
		Channels = 2;  // L or R implies stereo

	strcpy(SavedPlaybackDevice, PlaybackDevice);  // Saved so we can reopen in error recovery
	SavedPlaybackRate = p_sampleRate;
	if (OpenSoundPlayback(PlaybackDevice, p_sampleRate, Channels, ErrorMsg))
	{
		// Close playback again until TX starts (see SHARECAPTURE in the
		// ALSA backend); StopCapture() reopens it before transmitting.

		if (playStream)
		{
			Pa_CloseStream(playStream);
			playStream = NULL;
		}

		ZF_LOGI("Opening Capture Device %s Rate %d", CaptureDevice, c_sampleRate);

		strcpy(SavedCaptureDevice, CaptureDevice);  // Saved so we can reopen in error recovery
		SavedCaptureRate = c_sampleRate;
		return OpenSoundCapture(CaptureDevice, c_sampleRate, ErrorMsg);
	}
	else
		return false;
}



int CloseSoundCard()
{
	if (recStream)
	{
		Pa_CloseStream(recStream);
		recStream = NULL;
	}

	if (playStream)
	{
		Pa_CloseStream(playStream);
		playStream = NULL;
	}
	return 0;
}


int SoundCardWrite(short * input, unsigned int nSamples)
{
	if (playStream == NULL)
		return 0;

	// Stop Capture (as with SHARECAPTURE in the ALSA backend: only one
	// direction is open while transmitting)

	if (recStream)
	{
		Pa_CloseStream(recStream);
		recStream = NULL;
	}

	return PackSamplesAndSend(input, nSamples);
}

int PackSamplesAndSend(short * input, int nSamples)
{
	short samples[256000];
	short * sampptr = samples;
	int n;
	int written = 0;
	PaError err;

	if (playStream == NULL)
		return 0;

	// Expand the mono samples to the stream's channel count.
	// (The ALSA backend tests UseLeftRX here; that looks like an
	// upstream typo - the TX flags are used on macOS instead.  With the
	// default of all flags TRUE the behaviour is identical.)

	if (m_playchannels == 1)
	{
		memcpy(samples, input, nSamples * sizeof(short));
	}
	else
	{
		for (n = 0; n < nSamples; n++)
		{
			if (UseLeftTX)
				*(sampptr++) = input[0];
			else
				*(sampptr++) = 0;

			if (UseRightTX)
				*(sampptr++) = input[0];
			else
				*(sampptr++) = 0;

			input ++;
		}
	}

	// PortAudio's blocking-mode ring buffer may be smaller than a
	// 0.1 s transfer buffer, so feed it in chunks as space becomes
	// available, running the background polls (host TCP, webgui)
	// in between.  A full ring is normal during TX - the modulator
	// produces samples faster than real time.

	while (written < nSamples)
	{
		long avail = Pa_GetStreamWriteAvailable(playStream);
		int chunk;

		if (avail < 0)
		{
			ZF_LOGE("Pa_GetStreamWriteAvailable error: %s",
				Pa_GetErrorText((PaError)avail));
			return written;
		}

		if (avail > MaxAvail)
			MaxAvail = avail;

		if (avail == 0)
		{
			txSleep(10);
			if (playStream == NULL)  // closed while waiting
				return written;
			continue;
		}

		chunk = (int)avail;
		if (chunk > nSamples - written)
			chunk = nSamples - written;

		err = Pa_WriteStream(playStream, samples + (long)written * m_playchannels, chunk);

		// An output underflow is possible after a buffer gap and is not
		// fatal; the samples have still been queued.

		if (err != paNoError && err != paOutputUnderflowed)
		{
			ZF_LOGE("Pa_WriteStream error: %s", Pa_GetErrorText(err));
			return written;
		}

		written += chunk;
	}

	return nSamples;
}

int SoundCardRead(short * input, unsigned int nSamples)
{
	short samples[65536];
	int n;
	int start;
	long avail;
	PaError err;

	if (recStream == NULL)
		return 0;

	avail = Pa_GetStreamReadAvailable(recStream);

	if (avail < 0)
	{
		// Stream error - reopen, like the ALSA error recovery
		ZF_LOGD("capture avail error %s, reopening ..", Pa_GetErrorText((PaError)avail));
		Pa_CloseStream(recStream);
		recStream = NULL;
		OpenSoundCapture(SavedCaptureDevice, SavedCaptureRate, NULL);
		if (recStream == NULL)
			return 0;
		avail = Pa_GetStreamReadAvailable(recStream);
	}

	if (avail < (long)nSamples)
		return 0;

	if (m_recchannels == 1)
	{
		err = Pa_ReadStream(recStream, input, nSamples);
	}
	else
	{
		err = Pa_ReadStream(recStream, samples, nSamples);

		if (err == paNoError || err == paInputOverflowed)
		{
			// Extract the wanted channel from the interleaved stereo data

			if (UseLeftRX)
				start = 0;
			else
				start = 1;

			for (n = 0; n < (int)nSamples; n++)
				input[n] = samples[n * 2 + start];
		}
	}

	// An input overflow means samples were lost before this read;
	// the data returned is still valid.

	if (err != paNoError && err != paInputOverflowed)
	{
		ZF_LOGE("RX Error %s", Pa_GetErrorText(err));
		Pa_CloseStream(recStream);
		recStream = NULL;
		OpenSoundCapture(SavedCaptureDevice, SavedCaptureRate, NULL);
		return 0;
	}

	return nSamples;
}




int PriorSize = 0;

int Index = 0;  // DMA Buffer being used 0 or 1
int inIndex = 0;  // DMA Buffer being used 0 or 1

BOOL DMARunning = FALSE;  // Used to start DMA on first write

short * SendtoCard(short * buf, int n)
{
	if (Loopback)
	{
		// Loop back   to decode for testing

		ProcessNewSamples(buf, 1200);  // signed
	}

	if (playStream)
		SoundCardWrite(&buffer[Index][0], n);
	if (txwff != NULL)
		WriteWav(&buffer[Index][0], n, txwff);

	return &buffer[Index][0];
}

short loopbuff[1200];  // Temp for testing - loop sent samples to decoder


int InitSound(BOOL Quiet)
{
	if (strcmp(PlaybackDevice, "NOSOUND") == 0)
		return (1);
	if (!EnsurePaInitialized())
		return 0;
	GetInputDeviceCollection();
	GetOutputDeviceCollection();
	return OpenSoundCard(CaptureDevice, PlaybackDevice, 12000, 12000, NULL);
}

int min = 0, max = 0, lastlevelreport = 0, lastlevelGUI = 0;
UCHAR CurrentLevel = 0;  // Peak from current samples


void PollReceivedSamples()
{
	if (strcmp(PlaybackDevice, "NOSOUND") == 0)
		return;

	// Process any captured samples
	// Ideally call at least every 100 mS, more than 200 will loose data

	if (SoundCardRead(&inbuffer[0][0], ReceiveSize))
	{
		// returns ReceiveSize or none

		short * ptr = &inbuffer[0][0];
		int i;

		if (add_noise(&inbuffer[0][0], ReceiveSize, InputNoiseStdDev) > 0) {
			max = 32767;
			min = -32768;
		} else {
			for (i = 0; i < ReceiveSize; i++)
			{
				if (*(ptr) < min)
					min = *ptr;
				else if (*(ptr) > max)
					max = *ptr;
				ptr++;
			}
		}

		CurrentLevel = ((max - min) * 75) /32768;  // Scale to 150 max
		wg_send_currentlevel(0, CurrentLevel);

		if ((Now - lastlevelGUI) > 2000)  // 2 Secs
		{
			if (WaterfallActive == 0 && SpectrumActive == 0)  // Don't need to send as included in Waterfall Line
				SendtoGUI('L', &CurrentLevel, 1);  // Signal Level

			lastlevelGUI = Now;

			if ((Now - lastlevelreport) > 10000)  // 10 Secs
			{
				lastlevelreport = Now;
				// Report input peaks to host and log if CONSOLELOG is ZF_LOG_DEBUG (2) or ZF_LOG_VERBOSE (1) or if close to clipping
				if (max >= 32000 || ardop_log_get_level_console() <= ZF_LOG_DEBUG)
				{
					char HostCmd[64] = "";
					snprintf(HostCmd, sizeof(HostCmd), "INPUTPEAKS %d %d", min, max);
					SendCommandToHostQuiet(HostCmd);
					ZF_LOGD("Input peaks = %d, %d", min, max);
					// A user with the default of CONSOLELOG = ZF_LOG_INFO will see this message if they are close to clipping
					if (ardop_log_get_level_console() > ZF_LOG_DEBUG)
					{
						ZF_LOGI(
							"Your input signal is probably clipping. If you see"
							" this message repeated in the next 20-30 seconds,"
							" Turn down your RX input until this message stops"
							" repeating."
						);
					}
				}
			}
			min = max = 0;  // Every 2 secs
		}

		if (rxwf != NULL)
		{
			// There is an open Wav file recording.
			// Either close it or write samples to it.
			if (rxwf_EndNow < Now)
			{
				CloseWav(rxwf);
				rxwf = NULL;
			}
			else
				WriteWav(&inbuffer[0][0], ReceiveSize, rxwf);
		}


		if (Capturing && Loopback == FALSE)
			ProcessNewSamples(&inbuffer[0][0], ReceiveSize);
	}
}

void StopCapture()
{
	Capturing = FALSE;

	if (strcmp(PlaybackDevice, "NOSOUND") == 0)
		return;

	// StopCapture is only called when we are about to transmit, so use
	// it to open the playback stream (see SHARECAPTURE in the ALSA
	// backend).  The capture stream is closed in SoundCardWrite.

	OpenSoundPlayback(SavedPlaybackDevice, SavedPlaybackRate, Savedplaychannels, NULL);
}

void StartCodec(char * strFault)
{
	strFault[0] = 0;
	OpenSoundCard(CaptureDevice, PlaybackDevice, 12000, 12000, strFault);
}

void StopCodec(char * strFault)
{
	strFault[0] = 0;
	CloseSoundCard();
}

void StartCapture()
{
	Capturing = TRUE;
	DiscardOldSamples();
	ClearAllMixedSamples();
	State = SearchingForLeader;
}

void CloseSound()
{
	CloseSoundCard();

	if (PaInitialized)
	{
		Pa_Terminate();
		PaInitialized = FALSE;
	}
}

VOID WriteSamples(short * buffer, int len)
{
	// Only used on Windows
}

unsigned short * SoundInit()
{
	Index = 0;
	return &buffer[0][0];
}

// Called at end of transmission

void SoundFlush()
{
	// Append Trailer then send remaining samples

	char strFault[100] = "";
	int txlenMs = 0;

	AddTrailer();  // add the trailer.

	if (Loopback)
		ProcessNewSamples(&buffer[Index][0], Number);

	SendtoCard(&buffer[Index][0], Number);

	// Wait for tx to complete

	// samples sent is is in SampleNo, time started in mS is in pttOnTime.
	// calculate time to stop

	txlenMs = SampleNo / 12 + 20;  // 12000 samples per sec. 20 mS TXTAIL

	ZF_LOGD("Tx Time %d Time till end = %d", txlenMs, (pttOnTime + txlenMs) - Now);

	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
	{
		while (Now < (pttOnTime + txlenMs))
		{
			usleep(2000);
		}

		// Also wait until PortAudio's ring buffer has fully drained plus
		// the reported output latency, so PTT is not released while the
		// tail of the transmission is still sounding.

		if (playStream)
		{
			int drainMs = 500;
			const PaStreamInfo * si = Pa_GetStreamInfo(playStream);

			while (drainMs > 0 && Pa_GetStreamWriteAvailable(playStream) < MaxAvail)
			{
				usleep(2000);
				drainMs -= 2;
			}

			if (si)
				usleep((useconds_t)(si->outputLatency * 1e6));
		}
	}

	SoundIsPlaying = FALSE;

	if (blnEnbARQRpt > 0 || blnDISCRepeating)  // Start Repeat Timer if frame should be repeated
		dttNextPlay = Now + intFrameRepeatInterval + extraDelay;

	KeyPTT(FALSE);  // Unkey the Transmitter
	if (txwff != NULL)
	{
		CloseWav(txwff);
		txwff = NULL;
	}

	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
	{
		// End of TX: close playback and go back to capture-only
		// (see SHARECAPTURE in the ALSA backend)

		if (playStream)
		{
			Pa_CloseStream(playStream);
			playStream = NULL;
		}

		OpenSoundCapture(SavedCaptureDevice, SavedCaptureRate, strFault);
	}
	StartCapture();

	if (WriteRxWav)
		// Start recording if not already recording, else to extend the recording time.
		StartRxWav();
	return;
}

// PTT via a Hamlib rigctld network connection ("-p <ip>:<port>").
// Upstream declares PTTHAMLIB but never acts on it; this makes it work
// by sending rigctld's set_ptt command over TCP.

static int hamlibSock = -1;

static void HamlibPTT(int PTTState)
{
	char cmd[8];
	int len;

	if (hamlibSock == -1)
	{
		hamlibSock = socket(AF_INET, SOCK_STREAM, 0);
		if (hamlibSock == -1)
			return;

		if (connect(hamlibSock, &HamlibAddr, sizeof(struct sockaddr_in)) == -1)
		{
			ZF_LOGE("Failed to connect to Hamlib rigctld for PTT, errno=%d", errno);
			close(hamlibSock);
			hamlibSock = -1;
			return;
		}
	}

	len = snprintf(cmd, sizeof(cmd), "T %d\n", PTTState ? 1 : 0);

	if (send(hamlibSock, cmd, len, 0) != len)
	{
		ZF_LOGE("Hamlib PTT send failed, errno=%d, will reconnect", errno);
		close(hamlibSock);
		hamlibSock = -1;
		return;
	}

	// rigctld replies "RPRT 0\n"; drain without blocking so the
	// socket buffer does not fill up over a long session.
	{
		char reply[64];
		recv(hamlibSock, reply, sizeof(reply), MSG_DONTWAIT);
	}
}

VOID RadioPTT(int PTTState)
{
	if (PTTMode & PTTHAMLIB)
		HamlibPTT(PTTState);

	if (PTTMode & PTTRTS)
		if (PTTState)
			COMSetRTS(hPTTDevice);
		else
			COMClearRTS(hPTTDevice);

	if (PTTMode & PTTDTR)
		if (PTTState)
			COMSetDTR(hPTTDevice);
		else
			COMClearDTR(hPTTDevice);

	if (PTTMode & PTTCI_V)
		if (PTTState)
			WriteCOMBlock(hCATDevice, PTTOnCmd, PTTOnCmdLen);
		else
			WriteCOMBlock(hCATDevice, PTTOffCmd, PTTOffCmdLen);

	// CM108 HID PTT (PTTCM108) is not supported on macOS.
}

// Function to send PTT TRUE or PTT FALSE commanad to Host or if local Radio control Keys radio PTT

const char BoolString[2][6] = {"FALSE", "TRUE"};

BOOL KeyPTT(BOOL blnPTT)
{
	// Returns TRUE if successful False otherwise

	if (blnLastPTT &&  !blnPTT)
		dttStartRTMeasure = Now;  // start a measurement on release of PTT.

	if (!RadioControl)
		if (blnPTT)
			SendCommandToHostQuiet("PTT TRUE");
		else
			SendCommandToHostQuiet("PTT FALSE");

	else
		RadioPTT(blnPTT);

	ZF_LOGD("[Main.KeyPTT]  PTT-%s", BoolString[blnPTT]);

	blnLastPTT = blnPTT;
	SetLED(0, blnPTT);
	wg_send_pttled(0, blnPTT);
	return TRUE;
}


int stricmp(const unsigned char * pStr1, const unsigned char *pStr2)
{
	unsigned char c1, c2;
	int  v;

	if (pStr1 == NULL)
	{
		if (pStr2)
			ZF_LOGW("stricmp called with NULL 1st param - 2nd %s ", pStr2);
		else
			ZF_LOGW("stricmp called with two NULL params");

		return 1;
	}


	do {
		c1 = *pStr1++;
		c2 = *pStr2++;
		// The casts are necessary when pStr1 is shorter & char is signed
		v = tolower(c1) - tolower(c2);
	} while ((v == 0) && (c1 != '\0') && (c2 != '\0') );

	return v;
}

char Leds[8]= {0};
unsigned int PKTLEDTimer = 0;

void SetLED(int LED, int State)
{
	// If GUI active send state

	Leds[LED] = State;
	SendtoGUI('D', Leds, 8);
}

void DrawTXMode(const char * Mode)
{
	unsigned char Msg[64];
	strcpy(Msg, Mode);
	SendtoGUI('T', Msg, strlen(Msg) + 1);  // TX Frame

}

void DrawTXFrame(const char * Frame)
{
	unsigned char Msg[64];
	strcpy(Msg, Frame);
	SendtoGUI('T', Msg, strlen(Msg) + 1);  // TX Frame
}

void DrawRXFrame(int State, const char * Frame)
{
	unsigned char Msg[64];

	Msg[0] = State;  // Pending/Good/Bad
	strcpy(&Msg[1], Frame);
	SendtoGUI('R', Msg, strlen(Frame) + 1);  // RX Frame
}
// mySetPixel() uses 3 bytes from Pixels per call.  So it must be 3 times the
// size of the larger of inPhases[0] or intToneMags/4. (intToneMags/4 is larger)
UCHAR Pixels[9108];
UCHAR * pixelPointer = Pixels;


// This data may be copied and pasted from the debug log file into the
// "Host Command" input box in the WebGui in developer mode to reproduce
// the constellation plot.
void LogConstellation() {
	char Msg[10000] = "CPLOT ";
	for (int i = 0; i < pixelPointer - Pixels; i++)
		snprintf(Msg + strlen(Msg), sizeof(Msg) - strlen(Msg), "%02X", Pixels[i]);
	ZF_LOGV("%s", Msg);
}


void mySetPixel(unsigned char x, unsigned char y, unsigned int Colour)
{
	// Used on Windows for constellation. Save points and send to GUI at end

	*(pixelPointer++) = x;
	*(pixelPointer++) = y;
	*(pixelPointer++) = Colour;
}
void clearDisplay()
{
	// Reset pixel pointer

	pixelPointer = Pixels;

}
void updateDisplay()
{
//	 SendtoGUI('C', Pixels, pixelPointer - Pixels);
}
void DrawAxes(int Qual, char * Mode)
{
	UCHAR Msg[80];
	SendtoGUI('C', Pixels, pixelPointer - Pixels);
	wg_send_pixels(0, Pixels, pixelPointer - Pixels);
	LogConstellation();
	pixelPointer = Pixels;

	sprintf(Msg, "%s Quality: %d", Mode, Qual);
	SendtoGUI('Q', Msg, strlen(Msg) + 1);
}
void DrawDecode(char * Decode)
{}
