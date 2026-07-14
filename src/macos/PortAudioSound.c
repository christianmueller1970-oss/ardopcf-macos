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
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

char CaptureDevice[80] = "0";
char PlaybackDevice[80] = "0";

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
// M1 milestone: the audio backend is not yet implemented.  All audio
// functions are stubs that fail cleanly, so that the WAV decode path
// (-d) and the NOSOUND pseudo device work.  The real PortAudio
// implementation lands with the M2 milestone.

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

// Dummy stream state until the PortAudio backend is implemented;
// non-NULL means "open" to the code paths shared with the other platforms.
static int playstreamopen = 0;
static int recstreamopen = 0;

int CheckifLoaded()
{
	// Prevent CTRL/C from closing the TNC
	// (This causes problems if the TNC is started by LinBPQ)

	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	return TRUE;
}

int GetOutputDeviceCollection()
{
	ZF_LOGI("Playback Devices\n");
	ZF_LOGE("PortAudio backend not yet implemented on macOS (M1 stub)");
	WriteDevices = NULL;
	WriteDeviceCount = 0;
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
	ZF_LOGI("Capture Devices\n");
	ZF_LOGE("PortAudio backend not yet implemented on macOS (M1 stub)");
	ReadDevices = NULL;
	ReadDeviceCount = 0;
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
	if (ErrorMsg)
		sprintf (ErrorMsg, "PortAudio backend not yet implemented on macOS (M1 stub)");
	else
		ZF_LOGE("PortAudio backend not yet implemented on macOS (M1 stub)");
	return false;
}

int OpenSoundCapture(char * CaptureDevice, int m_sampleRate, char * ErrorMsg)
{
	if (ErrorMsg)
		sprintf (ErrorMsg, "PortAudio backend not yet implemented on macOS (M1 stub)");
	else
		ZF_LOGE("PortAudio backend not yet implemented on macOS (M1 stub)");
	return false;
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
	recstreamopen = 0;
	playstreamopen = 0;
	return 0;
}


int SoundCardWrite(short * input, unsigned int nSamples)
{
	if (!playstreamopen)
		return 0;

	return PackSamplesAndSend(input, nSamples);
}

int PackSamplesAndSend(short * input, int nSamples)
{
	// M1 stub: pretend the samples were sent.
	return nSamples;
}

int SoundCardRead(short * input, unsigned int nSamples)
{
	// M1 stub: no samples available.
	return 0;
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

	if (playstreamopen)
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
		while (Now < (pttOnTime + txlenMs))
		{
			usleep(2000);
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
		OpenSoundCapture(SavedCaptureDevice, SavedCaptureRate, strFault);
	StartCapture();

	if (WriteRxWav)
		// Start recording if not already recording, else to extend the recording time.
		StartRxWav();
	return;
}

VOID RadioPTT(int PTTState)
{
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
