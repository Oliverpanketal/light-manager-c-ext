/*
 ============================================================================
 Name        : lightmanager.c
 Author      : zwiebelchen <lars.cebu@gmail.com>
 Modified    : Norbert Richter <mail@norbert-richter.info>
 Version     : 1.2.0004
 Copyright   : GPL
 Description : main file which creates server socket and sends commands to
 LightManager pro via USB
 ============================================================================
 */

/*
	Revision history of lightmanager.c

	Legende:
	+ New/Add
	- Bugfix
	* Change


	1.02.0004
			First release using extensions

	1.02.0005
	        - TCP command handling revised (did not worked on windows telnet TCP sockets)
	        * -h parameter (FS20 housecode) syntax must now be a FS20 code (e.g. -h 14213444)
	        * SET CLOCK parameter changed to have the same format as for Linux date -s MMDDhhmm[[CC]YY][.ss]
	        + multiple cmds on command line -c parameter (e.g. -c "GET CLOCK; GET TEMP")

	1.02.0006
			+ Parameter -d implemented: Run as a real daemon
			* Outputs are now going to syslog

	1.02.0007
			+ WAIT command implemented

	1.02.0008
			* Output to syslog optional using -s (default stdout)
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <netdb.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <libusb-1.0/libusb.h>


/* ======================================================================== */
/* Defines */
/* ======================================================================== */

/* Program name and version */
#define VERSION				"1.2.0008"
#define PROGNAME			"Linux Lightmanager"

/* Some macros */
#define exit_if(expr) \
if(expr) { \
  debug(LOG_DEBUG, "exit_if() %s: %d: %s: Error %s", __FILE__, __LINE__, __PRETTY_FUNCTION__, strerror(errno)); \
  exit(1); \
}
#define return_if(expr, retvalue) \
if(expr) { \
  debug(LOG_DEBUG, "return_if() %s: %d: %s: Error %s", __FILE__, __LINE__, __PRETTY_FUNCTION__, strerror(errno)); \
  return(retvalue); \
}


#define LM_VENDOR_ID		0x16c0	/* jbmedia Light-Manager (Pro) USB vendor */
#define LM_PRODUCT_ID		0x0a32	/* jbmedia Light-Manager (Pro) USB product ID */

#define USB_MAX_RETRY		5		/* max number of retries on usb error */
#define USB_TIMEOUT			250		/* timeout in ms for usb transfer */
#define USB_WAIT_ON_ERROR	250		/* delay between unsuccessful usb retries */

#define INPUT_BUFFER_MAXLEN	1024	/* TCP commmand string buffer size */
#define MSG_BUFFER_MAXLEN	2048	/* TCP return message string buffer size */


/* command line parameter defaults */
#define DEF_DAEMON		false
#define DEF_DEBUG		false
#define DEF_SYSLOG		false
#define DEF_PORT		3456
#define DEF_HOUSECODE	0x0000


/* ======================================================================== */
/* Global vars */
/* ======================================================================== */
/* command line parameter variables */
bool fDaemon	= DEF_DAEMON;
bool fDebug		= DEF_DEBUG;
bool fsyslog	= DEF_SYSLOG;
unsigned int port = DEF_PORT;
unsigned int housecode = DEF_HOUSECODE;


fd_set socks;

pthread_mutex_t mutex_socks = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_usb   = PTHREAD_MUTEX_INITIALIZER;

libusb_device_handle *dev_handle;
libusb_context *usbContext;


/* ======================================================================== */
/* Prototypes */
/* ======================================================================== */
/* Non-ANSI stdlib functions */
int strnicmp(const char * cs,const char * ct,size_t count);
char *itoa(int value, char* result, int base);
char *ltrim(char *const s);
char *rtrim(char *const s);
char *trim(char *const s);

/* FS20 specific  */
int fs20toi(char *fs20, char **endptr);
const char *itofs20(char *buf, int code, char *separator);

/* USB Functions */
int usb_connect(void);
int usb_release(void);
int usb_send(libusb_device_handle* dev_handle, unsigned char* device_data, bool fexpectdata);

/* Helper Functions */
void debug(int priority, const char *format, ...);
void write_to_client(int socket_handle, const char *format, ...);
void client_cmd_help(int socket_handle);
int cmdcompare(const char * cs, const char * ct);
int handle_input(char* input, libusb_device_handle* dev_handle, int socket_handle);

/* TCP socket thread functions */
int tcp_server_init(int port);
int tcp_server_connect(int listen_sock);
int recbuffer(int s, void *buf, size_t len, int flags);
void *tcp_server_handle_client(void *arg);

/* Program helper functions */
void prog_version(void);
void copyright(void);
void usage(void);




/* ======================================================================== */
/* Non-ANSI stdlib functions */
/* ======================================================================== */
int strnicmp(const char * cs,const char * ct,size_t count)
{
	register signed char __res = 0;

	while (count) {
		if ((__res = toupper( *cs ) - toupper( *ct++ ) ) != 0 || !*cs++)
			break;
		count--;
	}

	return __res;
}


/** * C++ version 0.4 char* style "itoa":
	* Written by Luk�s Chmela
	* Released under GPLv3.
	*/
char * itoa(int value, char* result, int base)
{
	/* check that the base if valid */
	if (base < 2 || base > 36) { *result = '\0'; return result; }

	char* ptr = result, *ptr1 = result, tmp_char;
	int tmp_value;

	do {
		tmp_value = value;
		value /= base;
		*ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz" [35 + (tmp_value - value * base)];
	} while ( value );

	/* Apply negative sign */
	if (tmp_value < 0) *ptr++ = '-';
	*ptr-- = '\0';
	while(ptr1 < ptr) {
		tmp_char = *ptr;
		*ptr--= *ptr1;
		*ptr1++ = tmp_char;
	}
	return result;
}


/* Remove leading whitespaces */
char *ltrim(char *const s)
{
	size_t len;
	char *cur;

	if(s && *s) {
		len = strlen(s);
		cur = s;
		while(*cur && isspace(*cur)) {
			++cur, --len;
		}

		if(s != cur) {
			memmove(s, cur, len + 1);
		}
	}

	return s;
}

/* Remove trailing whitespaces */
char *rtrim(char *const s)
{
	size_t len;
	char *cur;

	if(s && *s) {
		len = strlen(s);
		cur = s + len - 1;

		while(cur != s && isspace(*cur)) {
			--cur, --len;
		}

		cur[isspace(*cur) ? 0 : 1] = '\0';
	}

	return s;
}

/* Remove leading and trailing whitespaces */
char *trim(char *const s)
{
	rtrim(s);
	ltrim(s);

	return s;
}
/* ======================================================================== */
/* FS20 specific  */
/* ======================================================================== */

/* convert FS20 code to int
   FS20 code format: xx.yy.... or xxyy...
   where xx and yy are number of addresscodes and subaddresses
   in the format 11..44
   returns: FS20 code as integer or -1 on error
   */
int fs20toi(char *fs20, char **endptr)
{
	int res = 0;

	/* length of string must be even */
	if ( strlen(fs20)%2 != 0 ) {
		return -1;
	}

	while( *fs20 && !isspace(*fs20) ) {
		int tmp;
		res <<= 4;
		tmp  = ((*fs20++ - '0')-1) * 4;
		tmp += ((*fs20++ - '0')-1);
		res += tmp;
	}
	if( endptr != NULL ){
		*endptr = fs20;
	}
	return res;
}


/* convert integer value to FS20 code
   FS20 code format: xxyy....
   where xx and yy are number of addresscodes and subaddresses
   in the format 11..44
   If separator is given (not NULL and not character is not '\0')
   it will be used each two digits
*/
const char *itofs20(char *buf, int code, char *separator)
{
	int strpos = 0;
	int shift = (code>0xff)?12:4;

	while( shift>=0 ) {
		if( ((code>>shift) & 0x0f) < 4 ) {
			/* Add leading 0 */
			buf[strpos++]='0';
			itoa(((code>>shift) & 0x0f), &buf[strpos++], 4);
		}
		else {
			itoa(((code>>shift) & 0x0f), &buf[strpos++], 4);
			strpos++;
		}
		if( separator!=NULL && *separator != '\0' ) {
			buf[strpos++]=(*separator - 1);
		}
		shift -= 4;
	}
	if( separator!=NULL && *separator != '\0' ) {
		buf[--strpos]='\0';
	}
	strpos = 0;
	while( buf[strpos] ) {
		buf[strpos++]++;
	}
	return buf;
}


/* ======================================================================== */
/* USB Functions */
/* ======================================================================== */
int usb_connect(void)
{
	int rc;

	/* USB connection */
	pthread_mutex_lock(&mutex_usb);
	usbContext = NULL;
	debug(LOG_DEBUG, "try to init libusb");
	rc = libusb_init(&usbContext);
	if (rc < 0) {
		debug(LOG_ERR, "libusb init error %i", rc);
		pthread_mutex_unlock(&mutex_usb);
		return EXIT_FAILURE;
	}
	debug(LOG_DEBUG, "libusb initialized");

	dev_handle = libusb_open_device_with_vid_pid(usbContext, LM_VENDOR_ID, LM_PRODUCT_ID); /* VendorID and ProductID in decimal */
	if (dev_handle == NULL ) {
		debug(LOG_DEBUG, "Cannot open device vendor %04x, product %04x", LM_VENDOR_ID, LM_PRODUCT_ID);
	}
	if (libusb_kernel_driver_active(dev_handle, 0) == 1) {
		debug(LOG_DEBUG, "Kernel driver active");
		if (libusb_detach_kernel_driver(dev_handle, 0) == 0) {
			debug(LOG_DEBUG, "Kernel driver detached!");
		} else {
			debug(LOG_DEBUG, "Kernel driver not detached!");
		}
	} else {
		debug(LOG_DEBUG, "Kernel driver not active");
	}

	rc = libusb_claim_interface(dev_handle, 0);
	if (rc < 0) {
		debug(LOG_ERR, "Error: Cannot claim interface\n");
		pthread_mutex_unlock(&mutex_usb);
		return EXIT_FAILURE;
	}
	pthread_mutex_unlock(&mutex_usb);
	return EXIT_SUCCESS;
}


int usb_release(void)
{
	int rc;

	pthread_mutex_lock(&mutex_usb);
	rc = libusb_release_interface(dev_handle, 0);
	if (rc != 0) {
		debug(LOG_ERR, "Cannot release interface\n");
		pthread_mutex_unlock(&mutex_usb);
		return EXIT_FAILURE;
	}
	libusb_close(dev_handle);
	libusb_exit(usbContext);
	pthread_mutex_unlock(&mutex_usb);
	return EXIT_SUCCESS;
}


int usb_send(libusb_device_handle* dev_handle, unsigned char* device_data, bool fexpectdata)
{
	int retry;
	int actual;
	int ret;
	int err = 0;

	pthread_mutex_lock(&mutex_usb);
	retry = USB_MAX_RETRY;
	ret = -1;
	while( ret!=0 && retry>0 ) {
		debug(LOG_DEBUG, "usb_send(0x01) (%02x %02x %02x %02x %02x %02x %02x %02x)", device_data[0], device_data[1], device_data[2], device_data[3], device_data[4], device_data[5], device_data[6], device_data[7] );
		ret = libusb_interrupt_transfer(dev_handle, (0x01 | LIBUSB_ENDPOINT_OUT), device_data, 8, &actual, USB_TIMEOUT);
		debug(LOG_DEBUG, "usb_send(0x01) transfered: %d, returns %d (%02x %02x %02x %02x %02x %02x %02x %02x)", actual, ret, device_data[0], device_data[1], device_data[2], device_data[3], device_data[4], device_data[5], device_data[6], device_data[7] );
		retry--;
		if( ret!=0 && retry>0 ) {
			usleep( USB_WAIT_ON_ERROR*1000L );
		}
	}
	if( ret!=0 && retry==0 ) {
		err = ret;
	}

	if( fexpectdata ) {
		retry = USB_MAX_RETRY;
		ret = -1;
		while( ret!=0 && retry>0 ) {
			debug(LOG_DEBUG, "usb_send(0x82) (%02x %02x %02x %02x %02x %02x %02x %02x)", device_data[0], device_data[1], device_data[2], device_data[3], device_data[4], device_data[5], device_data[6], device_data[7] );
			ret = libusb_interrupt_transfer(dev_handle, (0x82 | LIBUSB_ENDPOINT_IN), device_data, 8, &actual, USB_TIMEOUT);
			debug(LOG_DEBUG, "usb_send(0x82) transfered: %d, returns %d (%02x %02x %02x %02x %02x %02x %02x %02x)", actual, ret, device_data[0], device_data[1], device_data[2], device_data[3], device_data[4], device_data[5], device_data[6], device_data[7] );
			retry--;
			if( ret!=0 && retry>0 ) {
				usleep( USB_WAIT_ON_ERROR*1000L );
			}
		}
		if( ret!=0 && retry==0 ) {
			err = ret;
		}
	}

	pthread_mutex_unlock(&mutex_usb);

	return err;
}


/* ======================================================================== */
/* Helper Functions */
/* ======================================================================== */

void debug(int priority, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	if( priority == LOG_DEBUG ) {
		if( fDebug ) {
			if( fsyslog ) {
				vsyslog(priority, format, args);
			}
			else {
				vfprintf(stdout, format, args);
			}
		}
	}
	else {
		if( fsyslog ) {
			vsyslog(priority, format, args);
		}
		else {
			vfprintf(stdout, format, args);
		}
	}
	va_end(args);
}

cleanup(int sig)
{
	const char *reason;

	switch (sig ) {
		case SIGINT:
			reason = "SIGINT";
			break;
		case SIGKILL:
			reason = "SIGKILL";
			break;
		case SIGTERM:
			reason = "SIGTERM";
			break;
		default:
			reason = "unknown";
			break;
	}
	debug(LOG_INFO, "--- Terminate program %s %s (%s)", PROGNAME, VERSION, reason);
}

void sigfunc(int sig)
{
	if( (sig == SIGINT) ||
		(sig == SIGKILL) ||
		(sig == SIGTERM) )
	{
		cleanup(sig);
		debug(LOG_INFO, "exiting");
		exit (0);
	}
	return;
}


void write_to_client(int socket_handle, const char *format, ...)
{
	va_list args;
	char msg[MSG_BUFFER_MAXLEN];

	va_start (args, format);
	vsprintf (msg, format, args);
	if( socket_handle != 0 ) {
		pthread_mutex_lock(&mutex_socks);
		send(socket_handle, msg, strlen(msg), 0);
		pthread_mutex_unlock(&mutex_socks);
	}
	else {
		fputs(msg, stdout);
	}
	va_end (args);
}

void client_cmd_help(int socket_handle)
{
	write_to_client(socket_handle,
					 	"\r\n"
					 	"%s (%s) command list\r\n"
						"    FS20 addr cmd     Send a FS20 command where\r\n"
						"                        adr  FS20 address using the format ggss (1111-4444)\r\n"
						"                        cmd  Command ON|OFF|TOGGLE|UP|+|DOWN|-|<dim>\r\n"
						"                             where <dim> is the dim level\r\n"
						"                             * absolute values:   0 (min=off) to 16 (max))\r\n"
						"                             * percentage values: O\% to 100\%)\r\n"
						"    UNIROLL addr cmd  Send an Uniroll command where\r\n"
						"                        adr  Uniroll jalousie number (1-100)\r\n"
						"                        cmd  Command UP|+|DOWN|-|STOP\r\n"
						"    IT code addr cmd    Send an InterTechno command where\r\n"
						"                        code  InterTechno housecode (A-P)\r\n"
						"                        addr  InterTechno channel (1-16)\r\n"
						"                        cmd   Command ON|OFF|TOGGLE\r\n"
						"    SCENE scn         Activate scene <scn> (1-254)\r\n"
						"    GET CLOCK         Get the current device date and time\r\n"
						"    GET TEMP          Get the current device temperature sensor\r\n"
						"    SET CLOCK [time]  Set the device clock to system time or to <time>\r\n"
						"                      where time format is MMDDhhmm[[CC]YY][.ss]\r\n"
						"    WAIT ms           Wait for <ms> milliseconds\r\n"
						"    QUIT              Disconnect\r\n"
						"    EXIT              Disconnect and exit server programm\r\n"
					, PROGNAME, VERSION);
}


int cmdcompare(const char * cs, const char * ct)
{
	return strnicmp(cs, ct, strlen(ct));
}

/* 	handle command input either via TCP socket or by a given string.
	if socket_handle is 0, then results will be given via stdout
	otherwise it wilkl be sent back via TCP to the socket client
*/
int handle_input(char* input, libusb_device_handle* dev_handle, int socket_handle)
{
	static char usbcmd[8];
	char delimiter[] = " ,;\t\v\f";
	char *ptr;

	debug(LOG_DEBUG, "Handle input string '%s'", input);

	memset(usbcmd, 0, sizeof(usbcmd));

	ptr = strtok(input, delimiter);

	if( ptr != NULL ) {
		if (cmdcompare(input, "H") == 0 || cmdcompare(input, "?") == 0) {
			client_cmd_help(socket_handle);
			return 0;
		}
		/* FS20 devices */
		else if (cmdcompare(input, "FS20") == 0) {
			char *cp;
			int addr;
			int cmd = -1;

			/* next token: addr */
	 		ptr = strtok(NULL, delimiter);
	 		if( ptr!=NULL ) {
				int addr = fs20toi(ptr, &cp);
				if ( addr > 0 ) {
					/* next token: cmd */
			 		ptr = strtok(NULL, delimiter);
			 		if( ptr!=NULL ) {
						if (cmdcompare(ptr, "ON") == 0) {
							cmd = 0x11;
						} else if (cmdcompare(ptr, "OFF") == 0) {
							cmd = 0x00;
						} else if (cmdcompare(ptr, "TOGGLE") == 0) {
							cmd = 0x12;
						} else if (cmdcompare(ptr, "UP") == 0 || cmdcompare(ptr, "+") == 0 ) {
							cmd = 0x13;
						} else if (cmdcompare(ptr, "DOWN") == 0 || cmdcompare(ptr, "-") == 0 ) {
							cmd = 0x14;
						}
						/* dimming case */
						else {
							errno = 0;
							int dim_value = strtol(ptr, NULL, 10);
							if( *(ptr+strlen(ptr)-1)=='\%' ) {
								dim_value = (16 * dim_value) / 100;
							}
							if (errno != 0 || dim_value < 0 || dim_value > 16) {
								cmd = -2;
								write_to_client(socket_handle, "FS20: Wrong dim level (must be within 0-16 or 0\%-100\%)\r\n");
							}
							else {
								cmd = 0x01 * dim_value;
							}
						}
						if (cmd >= 0) {
							usbcmd[0] = 0x01;
							usbcmd[1] = (unsigned char) (housecode >> 8);   /* Housecode high byte */
							usbcmd[2] = (unsigned char) (housecode & 0xff); /* Housecode low byte */
							usbcmd[3] = addr;
							usbcmd[4] = cmd;
							usbcmd[6] = 0x03;
							if( usb_send(dev_handle, (unsigned char *)usbcmd, false) != 0 ) {
								write_to_client(socket_handle, "USB communication error\r\n");
							}
							else {
								write_to_client(socket_handle, "OK\r\n");
							}
						}
						else if (cmd == -1 ) {
							write_to_client(socket_handle, "FS20: unknown <cmd> parameter '%s'\r\n", ptr);
						}
					}
					else {
						write_to_client(socket_handle, "FS20: missing <cmd> parameter\r\n");
					}
				}
				else {
					write_to_client(socket_handle, "FS20 %s: wrong <addr> parameter\r\n", ptr);
				}
			}
			else {
				write_to_client(socket_handle, "FS20: missing <addr> parameter\r\n");
			}
	 	}
		/* Uniroll devices */
		else if (cmdcompare(input, "UNI") == 0) {
			int addr;
			int cmd = -1;

			/* next token: addr */
	 		ptr = strtok(NULL, delimiter);
	 		if( ptr!=NULL ) {
				errno = 0;
				int addr = strtol(ptr, NULL, 10);
				if (errno == 0 && addr >=1 && addr <= 16) {
					/* next token: cmd */
			 		ptr = strtok(NULL, delimiter);
			 		if( ptr!=NULL ) {
						if (cmdcompare(ptr, "STOP") == 0) {
							cmd = 0x02;
						} else if (cmdcompare(ptr, "UP") == 0 || cmdcompare(ptr, "+") == 0 ) {
							cmd = 0x01;
						} else if (cmdcompare(ptr, "DOWN") == 0 || cmdcompare(ptr, "-") == 0 ) {
							cmd = 0x04;
						}
						if (cmd >= 0) {
							/* 15 jj 74 cc 00 00 00 00 */
							usbcmd[0] = 0x15;
							usbcmd[1] = addr-1;
							usbcmd[2] = 0x74;
							usbcmd[3] = cmd;
							if( usb_send(dev_handle, (unsigned char *)usbcmd, false) != 0 ) {
								write_to_client(socket_handle, "USB communication error\r\n");
							}
							else {
								write_to_client(socket_handle, "OK\r\n");
							}
						}
						else {
							write_to_client(socket_handle, "UNIROLL: wrong <cmd> parameter '%s'\r\n", ptr);
						}
					}
					else {
						write_to_client(socket_handle, "UNIROLL: missing <cmd> parameter\r\n");
					}
				}
				else {
					write_to_client(socket_handle, "UNIROLL %s: wrong <addr> parameter\r\n", ptr);
				}
			}
			else {
				write_to_client(socket_handle, "UNIROLL: missing <addr> parameter\r\n");
			}
	 	}
		/* InterTechno devices */
		else if (cmdcompare(input, "IT") == 0 || cmdcompare(input, "InterTechno") == 0) {
			int code;
			int addr;
			int cmd = -1;

			/* next token: code */
	 		ptr = strtok(NULL, delimiter);
	 		if( ptr!=NULL ) {
	 			if( toupper(*ptr)>='A' && toupper(*ptr)<='Z' ) {
	 				code = toupper(*ptr) - 'A';
					/* next token: addr */
			 		ptr = strtok(NULL, delimiter);
			 		if( ptr!=NULL ) {
						errno = 0;
						int addr = strtol(ptr, NULL, 10);
						if (errno == 0 && addr >=1 && addr <= 16) {
							/* next token: cmd */
					 		ptr = strtok(NULL, delimiter);
					 		if( ptr!=NULL ) {
								if (cmdcompare(ptr, "ON") == 0) {
									cmd = 0x01;
								} else if (cmdcompare(ptr, "OFF") == 0 ) {
									cmd = 0x00;
								} else if (cmdcompare(ptr, "TOGGLE") == 0 ) {
									cmd = 0x02;
								}
								if (cmd >= 0) {
									usbcmd[0] = 0x05;
									usbcmd[1] = code * 0x10 + (addr - 1);
									usbcmd[2] = cmd;
									usbcmd[3] = 0x06;
									if( usb_send(dev_handle, (unsigned char *)usbcmd, false) != 0 ) {
										write_to_client(socket_handle, "USB communication error\r\n");
									}
									else {
										write_to_client(socket_handle, "OK\r\n");
									}
								}
								else {
									write_to_client(socket_handle, "InterTechno: wrong <cmd> parameter '%s'\r\n", ptr);
								}
							}
							else {
								write_to_client(socket_handle, "InterTechno: missing <cmd> parameter\r\n");
							}
						}
						else {
							write_to_client(socket_handle, "InterTechno: %s: <addr> parameter out of range (must be within 1 to 16)\r\n", ptr);
						}
					}
					else {
						write_to_client(socket_handle, "InterTechno: missing <addr> parameter\r\n");
					}
				}
				else {
					write_to_client(socket_handle, "InterTechno: <code> parameter out of range (must be within 'A' to 'P')\r\n");
				}
			}
			else {
				write_to_client(socket_handle, "InterTechno: missing <code> parameter\r\n");
			}
	 	}
	 	/* Scene commands */
		else if (cmdcompare(input, "SCENE") == 0) {
			long int scene;

	 		ptr = strtok(NULL, delimiter);
			if( ptr != NULL ) {
				scene = strtol(ptr, NULL, 10);
				if( scene >= 1 && scene<=254 ) {
					usbcmd[0] = 0x0f;
					usbcmd[1] = 0x01 * scene;
					if( usb_send(dev_handle, (unsigned char *)usbcmd, false) != 0 ) {
						write_to_client(socket_handle, "USB communication error\r\n");
					}
					else {
						write_to_client(socket_handle, "OK\r\n");
					}
				}
				else {
					write_to_client(socket_handle, "SCENE: parameter <s> out of range (must be within range 1-254)\r\n");
				}
			}
			else {
				write_to_client(socket_handle, "SCENE: missing parameter\r\n");
			}
	 	}
	 	/* Get commands */
		else if (cmdcompare(input, "GET") == 0) {
			/* next token GET device */
	 		ptr = strtok(NULL, delimiter);
	 		if( ptr!=NULL ) {
				if (cmdcompare(ptr, "CLOCK") == 0 ||
					cmdcompare(ptr, "TIME") == 0) {
					struct tm timeinfo;

					usbcmd[0] = 0x09;
					if( usb_send(dev_handle, (unsigned char *)usbcmd, true) != 0 ) {
						write_to_client(socket_handle, "USB communication error\r\n");
					}
					/* ss mm hh dd MM ww yy 00 */
					timeinfo.tm_sec  = usbcmd[0];
					timeinfo.tm_min  = usbcmd[1];
					timeinfo.tm_hour = usbcmd[2];
					timeinfo.tm_mday = usbcmd[3];
					timeinfo.tm_mon  = usbcmd[4]-1;
					timeinfo.tm_year = usbcmd[6] + 100;
					mktime ( &timeinfo );
					write_to_client(socket_handle, "%s\r", asctime(&timeinfo) );

				} else if (cmdcompare(ptr, "TEMP") == 0 ) {
					usbcmd[0] = 0x0c;
					if( usb_send(dev_handle, (unsigned char *)usbcmd, true) != 0 ) {
						write_to_client(socket_handle, "USB communication error\r\n");
					}
					else if( usbcmd[0]==0xfd ) {
						write_to_client(socket_handle, "%.1f degree Celsius\r\n", (float)usbcmd[1]/2);
					}
				}
				else {
					write_to_client(socket_handle, "GET: unknown parameter '%s'\r\n", ptr);
				}
			}
			else {
				write_to_client(socket_handle, "GET: missing parameter\r\n");
			}
	 	}
	 	/* Set commands */
		else if (cmdcompare(input, "SET") == 0) {
	 		ptr = strtok(NULL, delimiter);
	 		/* next token SET device */
	 		if( ptr!=NULL ) {
				if (cmdcompare(ptr, "CLOCK") == 0 ||
					cmdcompare(ptr, "TIME") == 0) {
					int cmd = 8;
				  	time_t now;
				  	struct tm * currenttime;
					struct tm timeinfo;

			        time(&now);
			        currenttime = localtime(&now);
			        memcpy(&timeinfo, currenttime, sizeof(timeinfo));

			        /* next token new time (optional) */
			 		ptr = strtok(NULL, delimiter);
			 		if( ptr!=NULL ) {
						switch( strlen(ptr) ) {
							case 8:		/* MMDDhhmm */
						        strptime(ptr, "%m%d%H%M", &timeinfo);
								break;
							case 10:	/* MMDDhhmmYY */
						        strptime(ptr, "%m%d%H%M%y", &timeinfo);
								break;
							case 11:	/* MMDDhhmm.ss */
						        strptime(ptr, "%m%d%H%M.%S", &timeinfo);
								break;
							case 12:	/* MMDDhhmmCCYY */
						        strptime(ptr, "%m%d%H%M%Y", &timeinfo);
								break;
							case 13:	/* MMDDhhmmYY.ss */
						        strptime(ptr, "%m%d%H%M%y.%S", &timeinfo);
								break;
							case 15:	/* MMDDhhmmCCYY.ss */
						        strptime(ptr, "%m%d%H%M%Y.%S", &timeinfo);
								break;
							default:
								write_to_client(socket_handle, "SET CLOCK: wrong time format (use MMDDhhmm[[CC]YY][.ss])\r\n");
								cmd = -1;
								break;
						}
			 		}
			 		if( cmd != -1 ) {
			 			int i;
						usbcmd[0] = cmd;
						usbcmd[1] = timeinfo.tm_sec;
						usbcmd[2] = timeinfo.tm_min;
						usbcmd[3] = timeinfo.tm_hour;
						usbcmd[4] = timeinfo.tm_mday;
						usbcmd[5] = timeinfo.tm_mon+1;
						usbcmd[6] = (timeinfo.tm_wday==0)?7:timeinfo.tm_wday;
						usbcmd[7] = timeinfo.tm_year-100;
						for(i=1; i<8;i++) {
							usbcmd[i] = ((usbcmd[i]/10)*0x10) + (usbcmd[i]%10);
						}
						usb_send(dev_handle, (unsigned char *)usbcmd, false);

						memset(usbcmd, 0, sizeof(usbcmd));
						usbcmd[2] = 0x0d;
						usb_send(dev_handle, (unsigned char *)usbcmd, false);

						memset(usbcmd, 0, sizeof(usbcmd));
						usbcmd[0] = 0x06;
						usbcmd[1] = 0x02;
						usbcmd[2] = 0x01;
						usbcmd[3] = 0x02;
						if( usb_send(dev_handle, (unsigned char *)usbcmd, false) != 0 ) {
							write_to_client(socket_handle, "USB communication error\r\n");
						}
						else {
							write_to_client(socket_handle, "OK\r\n");
						}
			 		}
			 	}
				else {
					write_to_client(socket_handle, "SET: unknown parameter '%s'\r\n", ptr);
				}
			}
			else {
				write_to_client(socket_handle, "SET: missing parameter\r\n");
			}
		}
	 	/* Control commands */
		else if (cmdcompare(input, "WAIT") == 0) {
			long int ms;

	 		ptr = strtok(NULL, delimiter);
			if( ptr != NULL ) {
				ms = strtol(ptr, NULL, 10);
				usleep(ms*1000L);
				write_to_client(socket_handle, "OK\r\n");
			}
			else {
				write_to_client(socket_handle, "WAIT: missing parameter\r\n");
			}
	 	}
		else if (cmdcompare(input, "QUIT") == 0 || cmdcompare(input, "Q") == 0) {
			return -1; //exit
		}
		else if (cmdcompare(input, "EXIT") == 0 || cmdcompare(input, "E") == 0) {
			return -2; //end
		}
		else {
			write_to_client(socket_handle, "\error - unknown command '%s'\r\n", ptr);
		}
	}

	return 0;

}


/* ======================================================================== */
/* TCP socket thread functions */
/* ======================================================================== */

int tcp_server_init(int port)
/* Server (listen) open port - only once
 * in port: TCP server port number
 * return: Socket filedescriptor
 */
{
	int listen_fd;
	int ret;
	struct sockaddr_in sock;
	int yes = 1;

	listen_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	exit_if(listen_fd < 0);

	/* prevent "Error Address already in use" error */
	ret = setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
	exit_if(ret < 0);

	memset((char *) &sock, 0, sizeof(sock));
	sock.sin_family = AF_INET;
	sock.sin_addr.s_addr = htonl(INADDR_ANY);
	sock.sin_port = htons(port);

	debug(LOG_DEBUG, "Server bind TCP socket");
	ret = bind(listen_fd, (struct sockaddr *) &sock, sizeof(sock));
	return_if(ret != 0, -1);

	debug(LOG_DEBUG, "Server listening");
	ret = listen(listen_fd, 5);
	return_if(ret < 0, -1);

	debug(LOG_INFO, "Server now listen on TCP port %d", port);

	return listen_fd;
}


int tcp_server_connect(int listen_sock)
/* Client TCP connection - for each client
 * in listen_sock: Socket main filedescriptor to get client connected
 * return: Client socket filedescriptor or error
 */
{
	int fd;
	struct sockaddr_in sock;
	socklen_t socklen;

	socklen = sizeof(sock);
	fd = accept(listen_sock, (struct sockaddr *) &sock, &socklen);
	return_if(fd < 0, -1);

	return fd;
}

int recbuffer(int s, void *buf, size_t len, int flags)
{
	int rc;
	int slen;
	char *str;

	memset(buf, 0, len);
	str = (char *)buf;
	slen = 0;
	while( (rc=recv(s, str, len, flags)) != -1) {
		slen += rc;
		if( rc>0 && *(str+rc-1)=='\r' || *(str+rc-1)=='\n' ) {
			return slen;
		}
		str +=rc;
	}
	return rc;
}

void *tcp_server_handle_client(void *arg)
/* Connected client thread
 * Handles input from clients
 * in arg: Client socket filedescriptor
 */
{
	int client_fd;
	char buf[INPUT_BUFFER_MAXLEN];
	int buflen;
	int rc;
	int wfd;

	client_fd = (int)arg;

	write_to_client(client_fd, "Welcome to %s (%s)\r\n"
							   ">", PROGNAME, VERSION);
	while(true) {
		memset(buf, 0, sizeof(buf));
		rc = recbuffer(client_fd, buf, sizeof(buf), 0);
		rc = handle_input(trim(buf), dev_handle, client_fd);
		if ( rc < 0 ) {
			debug(LOG_DEBUG, "Disconnect from client (handle %d)", client_fd);
			write_to_client(client_fd, "bye\r\n");
			/* End of TCP Connection */
			pthread_mutex_lock(&mutex_socks);
			FD_CLR(client_fd, &socks);      /* remove dead client_fd */
			pthread_mutex_unlock(&mutex_socks);
			close(client_fd);
			if( rc== -2 ) {
				rc = usb_release();
				exit(rc);
			}
			pthread_exit(NULL);
		}
		else {
			write_to_client(client_fd, ">");
		}
	}
	return NULL;
}




/* ======================================================================== */
/* Program helper functions */
/* ======================================================================== */

void prog_version(void)
{
	printf("%s (%s)\n", PROGNAME, VERSION);
}

void copyright(void)
{
	puts(	"License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n"
			"This is free software: you are free to change and redistribute it.\n"
			"There is NO WARRANTY, to the extent permitted by law.\n"
			"\n"
			"written by zwiebelchen <lars.cebu@gmail.com>\n"
			"modified by Norbert Richter <mail@norbert-richter.info>\n"
			"\n");
}

void usage(void)
{
	char buf[12];

	printf("\nUsage: lightmanager [OPTION]\n");
	printf("\n");
	printf("Options are:\n");
	printf("    -c cmd        Execute command <cmd> and exit (separate several commands by ';' or ',')\n");
	printf("    -d            Start as daemon (default %s)\n", DEF_DAEMON?"yes":"no");
	printf("    -g            Debug mode (default %s)\n", DEF_DEBUG?"yes":"no");
	printf("    -h housecode  Use <housecode> for sending FS20 data (default %s)\n", itofs20(buf, DEF_HOUSECODE, NULL));
	printf("    -p port       Listen on TCP <port> for command client (default %d)\n", DEF_PORT);
	printf("    -s            Redirect output to syslog instead of stdout (default)\n");
	printf("    -?            Prints this help and exit\n");
	printf("    -v            Prints version and exit\n");
}


int main(int argc, char * argv[]) {
	int listen_fd;
	int rc = 0;
	char cmdexec[MSG_BUFFER_MAXLEN];

	debug(LOG_INFO, "Starting %s (%s)", PROGNAME, VERSION);

	memset(cmdexec, 0, sizeof(cmdexec));
	while (true)
	{
		int result = getopt(argc, argv, "c:dgp:h:v?");
		if (result == -1) break; /* end of list */
		switch (result)
		{
			case ':': /* missing argument of a parameter */
				debug(LOG_ERR, "missing argument\n");
				return EXIT_FAILURE;
				break;
			case 'c':
				if( fDaemon ) {
					debug(LOG_WARNING, "Starting as daemon with parameter -c is not possible, disable daemon flag");
					fDaemon = false;
				}
				debug(LOG_INFO, "Execute command(s) '%s'", optarg);
				strncpy(cmdexec, optarg, sizeof(cmdexec));
				break;
			case 'd':
				if( *cmdexec ) {
					debug(LOG_WARNING, "Starting as daemon with parameter -c is not possible, disable daemon flag");
				}
				else {
					fDaemon = true;
					debug(LOG_INFO, "Starting as daemon");
				}
				break;
			case 'g':
				fDebug = true;
				debug(LOG_INFO, "Debug enabled");
				break;
			case 'h':
				{
					char buf[64];
					housecode = fs20toi(optarg, NULL);
					debug(LOG_INFO, "Using housecode %s (%0dd, 0x%04x, FS20=%s)", optarg, housecode, housecode, itofs20(buf, housecode, NULL));
				}
				break;
			case 'p':
				port = strtol(optarg, NULL, 10);
				debug(LOG_INFO, "Using TCP port %d for listening", port);
				break;
			case 's':
				fsyslog = true;
				debug(LOG_INFO, "Output to syslog");
				break;
			case '?': /* unknown parameter */
				prog_version();
				usage();
				return EXIT_SUCCESS;
			case 'v':
				prog_version();
				copyright();
				return EXIT_SUCCESS;
			default: /* unknown */
				break;
		}
	}
	while (optind < argc)
	{
		debug(LOG_WARNING, "Unknown parameter <%s>", argv[optind++]);
	}

	/* Starting as daemon if requested */
	if( fDaemon ) {
		pid_t pid, sid;

		/* Fork off the parent process */
		pid = fork();
		if (pid < 0) {
			// Log failure (use syslog if possible)
			debug(LOG_ERR, "Unable to fork the process");
			exit(EXIT_FAILURE);
		}
		// If we got a good PID, then we can exit the parent process.
		if (pid > 0) {
			exit(EXIT_SUCCESS);
		}
		/* Change the file mode mask */
		umask(0);

		/* Create a new SID for the child process */
		sid = setsid();
		if (sid < 0) {
			/* Log any failures here */
			syslog(LOG_ERR, "Unable to create a new SID for the child process");
			exit(EXIT_FAILURE);
		}
		pid = sid;
		/* Close out the standard file descriptors */
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}
	signal(SIGINT,sigfunc);
	signal(SIGKILL,sigfunc);
	signal(SIGTERM,sigfunc);


	rc = usb_connect();
	if( rc == EXIT_SUCCESS ) {

		/* If command line cmd is given, execute cmd and exit */
		if( *cmdexec ) {
			char delimiter[] = ",;";
			char *ptr;
			int rc = 0;
			char *token[100];
			int i;

			i = 0;
			token[i] = strtok(cmdexec, delimiter);
			while( token[i] ) {
				token[++i] = strtok(NULL, delimiter);
			}
			i = 0;
			while( rc>=0 && token[i]!=NULL ) {
				rc = handle_input(trim(token[i++]), dev_handle, 0);
			}
		}
		/* otherwise start TCP listing */
		else {
			/* open main TCP listening socket */
			listen_fd = tcp_server_init(port);
			debug(LOG_DEBUG, "Listening now on port %d (handle %d)", port, listen_fd);
			FD_ZERO(&socks);

			/* main loop */
			while (true) {
				int client_fd;
				void *arg;

				/* Check TCP server listen port (client connect) */
				client_fd = tcp_server_connect(listen_fd);
				debug(LOG_DEBUG, "Client connected (handle=%d)", client_fd);
				if (client_fd >= 0) {
					pthread_t	thread_id;

					pthread_mutex_lock(&mutex_socks);
					FD_SET(client_fd, &socks);
					pthread_mutex_unlock(&mutex_socks);

					/* start thread for client command handling */
					arg = (void *)client_fd;
					pthread_create(&thread_id, NULL, tcp_server_handle_client, arg);
				}
			}
			rc = usb_release();
		}
	}
	return rc;
}