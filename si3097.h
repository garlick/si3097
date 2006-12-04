
/* Define IOCTL calls for the si3097 Camera Interface Card */

/* this is to be included by the driver and application alike */

#define SI3097_VERSION "1.0"

#ifndef __KERNEL_

#include <linux/types.h>
#include <linux/ioctl.h>

#endif

/* Sent to DMA_INIT to configure the DMA memory */

struct SI_DMA_CONFIG {
  int total;   /* total transfer len */
  int buflen;  /* length of buffer (bytes) */
  int timeout; /* jiffies to timeout of DMA_WAIT */
  int maxever; /* biggest value of 'total' that I would ever expect */
  unsigned int config; /* dma config mask */
};

/* status word of SI_DMA_CONFIG */

#define SI_DMA_CONFIG_WAKEUP_ONEND  0x01 
#define SI_DMA_CONFIG_WAKEUP_EACH   0x02

/* mask passed to verbose */

#define SI_VERBOSE_SERIAL   0x02
#define SI_VERBOSE_DMA      0x01

struct SI_DMA_STATUS {
  int transferred;     /* total bytes transferred this dma     */
  unsigned int status; /* DMA status register                  */
  int cur;             /* which sgl buffer is currently active */
  int next;            /* next sgl buffer that is done         */
};

/* status word of SI_DMA_STATUS */

#define SI_DMA_STATUS_DONE     0x10
#define SI_DMA_STATUS_ENABLE   0x01

// UART configuration 

struct SI_SERIAL_PARAM {
  int flags;
  int baud;
  int bits;
  int parity;
  int stopbits;
  int buffersize;
  int fifotrigger;
  int timeout;
};

/* flags field for configuring the uart */ 
#define SI_SERIAL_FLAGS_BLOCK        0x04 /* read/write block for done */

/* for SI_IOCTL_SETPOLL make poll (or select) wait on dma or the uart 
   but not both */

#define SI_SETPOLL_DMA   0
#define SI_SETPOLL_UART  1

#define SI_IOCTL_CODE_BASE      0x0
#define SI_MAGIC                'P'

//#define IOCTL_MSG( code ) _IOWR( SI_MAGIC, code, IOCTLDATA )

typedef enum _SI_DRIVER_MSGS
{
  MSG_SI_RESET = SI_IOCTL_CODE_BASE,
  MSG_SI_DMA_INIT,
  MSG_SI_DMA_START,
  MSG_SI_DMA_END,
  MSG_SI_DMA_STATUS,
  MSG_SI_DMA_NEXT,
  MSG_SI_DMA_ABORT,
  MSG_SI_SERIAL_IN_STATUS,
  MSG_SI_GET_SERIAL,
  MSG_SI_SET_SERIAL,
  MSG_SI_SERIAL_BREAK,
  MSG_SI_SERIAL_CLEAR,
  MSG_SI_SERIAL_OUT_STATUS,
  MSG_SI_VERBOSE,
  MSG_SI_SETPOLL,
  MSG_SI_FREEMEM,
} SI_DRIVER_MSGS;


// SI interface

#define SI_IOCTL_RESET             _IO(SI_MAGIC, MSG_SI_RESET)
#define SI_IOCTL_SERIAL_IN_STATUS  _IOR(SI_MAGIC, MSG_SI_SERIAL_IN_STATUS, int)
#define SI_IOCTL_GET_SERIAL        _IOR(SI_MAGIC, MSG_SI_GET_SERIAL, struct SI_SERIAL_PARAM)
#define SI_IOCTL_SET_SERIAL        _IOW(SI_MAGIC, MSG_SI_SET_SERIAL, struct SI_SERIAL_PARAM  )
#define SI_IOCTL_SERIAL_BREAK      _IOW(SI_MAGIC, MSG_SI_SERIAL_BREAK, int)
#define SI_IOCTL_SERIAL_CLEAR      _IO(SI_MAGIC, MSG_SI_SERIAL_CLEAR )
#define SI_IOCTL_SERIAL_OUT_STATUS _IOR(SI_MAGIC, MSG_SI_SERIAL_OUT_STATUS, int)
  
#define SI_IOCTL_DMA_INIT          _IOR(SI_MAGIC, MSG_SI_DMA_INIT, struct SI_DMA_CONFIG )
#define SI_IOCTL_DMA_START         _IOR(SI_MAGIC, MSG_SI_DMA_START, struct SI_DMA_STATUS )

#define SI_IOCTL_DMA_STATUS        _IOWR(SI_MAGIC, MSG_SI_DMA_STATUS, struct SI_DMA_STATUS )
#define SI_IOCTL_DMA_ABORT         _IOWR(SI_MAGIC, MSG_SI_DMA_ABORT, struct SI_DMA_STATUS )
#define SI_IOCTL_DMA_NEXT          _IOWR(SI_MAGIC, MSG_SI_DMA_NEXT, struct SI_DMA_STATUS )
#define SI_IOCTL_VERBOSE           _IOWR(SI_MAGIC, MSG_SI_VERBOSE, int )
#define SI_IOCTL_SETPOLL           _IOWR(SI_MAGIC, MSG_SI_SETPOLL, int )
#define SI_IOCTL_FREEMEM           _IO(SI_MAGIC, MSG_SI_FREEMEM )

