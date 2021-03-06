/*

Test application for the
Spectral Instruments 3097 Camera Interface

Copyright (C) 2006  Jeffrey R Hagen

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <stdarg.h>

#include "si3097.h"
#include "si_app.h"
#include "demux.h"
#include "lib.h"

/*
  low level testout of the si3097 driver
*/


void parse_commands( struct SI_CAMERA *c, char *buf );
void dma_test( struct SI_CAMERA *c, int cmd, int repeat );
void write_dma_data( void *ptr, int total );
void print_data_len( unsigned char *ptr, int buflen, int total );
void print_mem_changes( unsigned short *ptr, int nwords );
void expect_y( int fd );
void print_readout( struct SI_CAMERA *c );
void print_config( struct SI_CAMERA *c );
void print_status( struct SI_CAMERA *c );
void print_cfg( struct CFG_ENTRY *cfg, int val );
void print_dma_status( struct SI_CAMERA *c );
void stop_dma( struct SI_CAMERA *c );
void dma_unmap( struct SI_CAMERA *c );
void dma_mmap( struct SI_CAMERA *c );
void crash( struct SI_CAMERA *c );
void set_config( struct SI_CAMERA *c );
void print_help( void );
void camera_image( struct SI_CAMERA *c, int cmd );
void io_readout( struct SI_CAMERA *c );
void io_config( struct SI_CAMERA *a );
int change_cfg( struct SI_CAMERA *c, struct CFG_ENTRY *cfg,
                int value, int cmd );
int send_readout( struct SI_CAMERA *c );
void vmatest( struct SI_CAMERA *c );
void timeout_test( struct SI_CAMERA *c );
char *xstrdup (const char *s);
void usage ( void );
void die ( const char *fmt, ... );

const char *default_device = "/dev/sicamera0";
const char *default_setfile = NULL;
const char *default_dspfile = NULL;
const char *default_cfgfile = "Test.cfg";

static char *dspfile = NULL;
int verbose = 0;

#define OPTIONS "f:c:s:d:"
static const struct option longopts[] = {
  {"file",       required_argument,   0, 'f'},
  {"cfgfile",    required_argument,   0, 'c'},
  {"setfile",    required_argument,   0, 's'},
  {"dspfile",    required_argument,   0, 'd'},
  {0, 0, 0, 0},
};

int main(int argc, char *argv[] )
{
  struct SI_CAMERA *c;
  char *device = xstrdup (default_device);
  char *cfgfile = xstrdup (default_cfgfile);
  char *setfile = xstrdup (default_setfile);
  char buf[256];
  int ch;

  dspfile = xstrdup (default_dspfile);

  if (!(c = calloc(1, sizeof(*c))))
    die ("out of memory\n");

  while ((ch = getopt_long (argc, argv, OPTIONS, longopts, NULL)) != -1) {
    switch (ch) {
      case 'f': /* --device FILE */
        free (device);
        device = xstrdup (optarg);
        break;
      case 'c': /* --cfgfile FILE */
        free (cfgfile);
        cfgfile = xstrdup (optarg);
        break;
      case 's': /* --setfile FILE */
        free (setfile);
        setfile = xstrdup (optarg);
        break;
      case 'd': /* --dspfile FILE */
        free (dspfile);
        dspfile = xstrdup (optarg);
        break;
      case 'h':
      default:
        usage ();
    }
  }

  /* Load names for config and readout integer slots
   */
  if (si_load_camera_cfg( c, cfgfile ) < 0)
    die ("%s: %s\n", cfgfile, strerror (errno));

  /* Load default settings for camera
   */
  if (!setfile)
    die ("Please indicate camera settings file with --setfile FILE\n");
  if (si_setfile_readout( c, setfile ) < 0)
    die ("%s: %s\n", setfile, strerror (errno));

  /* Initiate communication with si3097 card.
   * Configure UART between card and camera.
   */
  if((c->fd = open( device, O_RDWR, 0 ))<0 )
    die ("%s: %s\n", device, strerror (errno));
#if 0
  if( ioctl(c->fd, SI_IOCTL_VERBOSE, &verbose) <0 )
    die ("SI_IOCTL_VERBOSE: %s\n", device, strerror (errno));
#endif
  si_init_com( c->fd, 57600, 0, 8, 1, 9000 );

  /* Store readout settings to camera.
   * Then load readout settings from camera.
   */
  if (send_readout( c ) < 0)
    die ("error sending readout params to camera: %s\n", strerror (errno));
  if (si_send_command(c->fd, 'H') < 0)     // H    - load readout
    die ("error sending H command: %s\n", strerror (errno));
  if (si_receive_n_ints( c->fd, 32, c->readout ) < 0)
    die ("error receiving readout params from camera: %s\n", strerror (errno));
  expect_y( c->fd );

  print_help();

  while(1) {
    printf("si3097> ");
    fflush(stdout);
    fgets( buf, 256, stdin);
    if( buf[0] == '?' ) {
      print_help();
      continue;
    }
    parse_commands( c, buf );
  }

  free (device);
  free (cfgfile);
}

void die (const char *fmt, ...)
{
    va_list ap;

    va_start (ap, fmt);
    vfprintf (stderr, fmt, ap);
    va_end (ap);
    exit (1);
}


void usage ( void )
{
  fprintf (stderr,
"Usage: test_app OPTIONS\n"
"    -f,--file=FILE      override default device file [%s]\n",
           default_device);
  exit (1);
}

char *xstrdup (const char *s)
{
  char *cpy = NULL;
  if (s) {
    if (!(cpy = strdup (s))) {
      fprintf (stderr, "out of memory\n");
      exit (1);
    }
  }
  return cpy;
}

void parse_commands( struct SI_CAMERA *c, char *buf )
{
  char *delim = " \t\n";
  char *s;
  static int repeat = 1;

  if( strncasecmp( buf, "sendfile", 8 ) == 0 ) {
    if (dspfile == NULL)
      printf ("no dspfile set\n");
    else
      si_sendfile( c->fd, 250, dspfile );
  } else if( buf[0] == 'A' ) {
    si_send_command_yn(c->fd, 'A');  // A  - Open Shutter  returns Y/N
  } else if( buf[0] == 'B' ) {
    si_send_command_yn(c->fd, 'B');  // B - Close Shutter returns Y/N
  } else if( buf[0] == 'I' ) {
    si_send_command(c->fd, 'I');     // I    - Get camera status
    si_receive_n_ints( c->fd, 16, c->status );
    print_status( c );
    expect_y( c->fd );
  } else if( buf[0] == 'J' ) {
    set_config(c);

  } else if( buf[0] == 'H' ) {
    si_send_command(c->fd, 'H');     // H    - load readout
    si_receive_n_ints( c->fd, 32, (int *)&c->readout );
    print_readout( c );
    expect_y( c->fd );

  } else if( buf[0] == 'L' ) {
    si_send_command(c->fd, 'L');     // L    - load config
    si_receive_n_ints( c->fd, 32, (int *)&c->config );
    print_config( c );
    expect_y( c->fd );

  } else if( buf[0] == 'S' ) {
    si_send_command(c->fd, 'S');     // S    - Cooler On, no response

  } else if( buf[0] == 'T' ) {
    si_send_command(c->fd, 'T');     // T    - Cooler Off, no response

  } else if( buf[0] == '0' ) {
    si_send_command(c->fd, '0');
    // Abort Readout, aborts an ongoing exposure (camera cannot
    // be stopped during actual readout)
  } else if( strncmp( buf, "stopdma", 7 )== 0 ) {
    stop_dma( c );
  } else if( strncmp( buf, "dma", 3 )== 0 ) {
    strtok( buf, delim );
    if( (s = strtok( NULL, delim )))
      dma_test( c, *s, repeat );
    else
      dma_test( c, 'C', repeat );

  } else if( strncmp( buf, "status", 6 )== 0 ) {
    bzero(&c->dma_status, sizeof(struct SI_DMA_STATUS));
    ioctl( c->fd, SI_IOCTL_DMA_STATUS, &c->dma_status );
    print_dma_status(c);
  } else if( strncmp( buf, "repeat", 6 )== 0 ) {
    strtok( buf, delim );
    if( (s = strtok( NULL, delim )))
      repeat = atoi(s);
    else
      repeat = 1;
  } else if( strncmp( buf, "verb", 4 )== 0 ) {
    switch (verbose) {
      case 0:
        verbose = SI_VERBOSE_SERIAL | SI_VERBOSE_DMA;
        break;
      case (SI_VERBOSE_SERIAL | SI_VERBOSE_DMA):
        verbose = 0;
        break;
    }
    if( ioctl(c->fd, SI_IOCTL_VERBOSE, &verbose) <0 ) {
      perror("verbose error");
    }
    printf("Verbose set to 0x%x\n", verbose);
  } else if( strncmp( buf, "crash", 5 )== 0 ) {
    crash(c);
  } else if( strncmp( buf, "image", 5 )== 0 ) {
    int cmd;
    strtok( buf, delim );
    if( (s = strtok( NULL, delim ) ))
       cmd = *s;
     else
       cmd = 'D';
    camera_image( c, cmd );
  } else if( strncmp( buf, "change_readout", 14 )== 0 ) {
    io_readout(c);
  } else if( strncmp( buf, "change_config", 13 )== 0 ) {
    io_config(c);
  } else if( strncmp( buf, "setfile_readout", 5 )== 0 ) {
    strtok( buf, delim );
    if( !(s = strtok( NULL, delim ) ))
      s = "800-299x1.set";
    si_setfile_readout( c,  s );
    send_readout( c );
    si_send_command(c->fd, 'H');     // H    - load readout
    si_receive_n_ints( c->fd, 32, (int *)&c->readout );
    print_readout( c );
    expect_y( c->fd );
  } else if( strncmp( buf, "send_readout", 5 )== 0 ) {
    send_readout( c );
  } else if( strncmp( buf, "quit", 4 )== 0 ) {
    exit(0);
  } else if( strncmp( buf, "vmatest", 7 )== 0 ) {
    vmatest( c );
  } else if( strncmp( buf, "timeout_test", 12 )== 0 ) {
    timeout_test( c );
  } else {
    print_help();
  }
}

/*
  F    - Send Readout Parameters
           send 32 unsigned 32 bit values following
           return Y/N byte

  G    - Send One Readout Parameters
           send one 32 bit offset
           send one 32 bit value
           returns Y/N byte


  J    - Send Configuration Parameters
           send 32 unsigned 32 bit values following
           returns Y/N byte

  K    - Send One Configuration Parameters
           send one 32 bit offset
           send one 32 bit value
           returns Y/N byte


  D    - Image Expose request regular image with shuttered exposure
           wants DMA active

  E    - Image Read, requests a dark image with closed shutter
           wants DMA active

  Z    - Image Tdi, requests a tdi image
           wants DMA active


  P    - Eeprom read, causes the EEPROM in the camera to be
         read into the camera's configuration buffer.

  M    - Eeprom write, causes the camera's configuration buffer
         to be copied to the camera's EEPROM.


*/

/*
  C    - Image Test, requests a test image
           wants DMA active

   first test of dma for new driver
*/

void dma_test( struct SI_CAMERA *c, int cmd, int repeat )
{
  int nbufs, loop, ret, rnd;
  unsigned char *data1, *data2;
  fd_set wait;
  int sel;

  data1 = ( unsigned char *)malloc(8000000);
  data2 = ( unsigned char *)malloc(8000000);

  if( cmd != 'C' && cmd != 'D' && cmd != 'E' && cmd != 'Z' ) {
    printf("dma test needs C, D, E, Z type ? for help\n");
    return;
  }
  printf("starting DMA test with %c loops %d\n", cmd, repeat );

  for( loop = 0; loop < repeat; loop++ ) {
    if( !c->dma_active ) {
      c->dma_config.maxever = 32*1024*1024;
      c->dma_config.total = 4000000*2;
      c->dma_config.buflen = 1024*1024; /* power of 2 makes it easy to mmap */
      c->dma_config.timeout = 5000;
//      c->dma_config.config = SI_DMA_CONFIG_WAKEUP_EACH;
      c->dma_config.config = SI_DMA_CONFIG_WAKEUP_ONEND;

      if( ioctl( c->fd, SI_IOCTL_DMA_INIT, &c->dma_config )<0 ){
        perror("dma init");
        return;
      }
     nbufs = c->dma_config.total / c->dma_config.buflen ;
     if( c->dma_config.total % c->dma_config.buflen )
       nbufs += 1;

      if(!(c->ptr = (unsigned short *)mmap( 0,
         c->dma_config.maxever,
         PROT_READ, MAP_SHARED, c->fd, 0))) {
        perror("mmap");
        return;
      }
      print_mem_changes( c->ptr, c->dma_config.total/sizeof(short) );

      c->dma_active = 1;
    }


  if( ioctl( c->fd, SI_IOCTL_DMA_START, &c->dma_status )<0 ){
    perror("dma start");
    return;
  }

  bzero(&c->dma_status, sizeof(struct SI_DMA_STATUS));
  si_send_command_yn(c->fd, cmd );

  /* random sleep to test driver */

  rnd = (int)(500000.0*(double)rand()/(double)RAND_MAX);
  usleep( rnd );

/* wait for DMA done */

/* test out driver poll function */

  FD_ZERO( &wait );
  FD_SET( c->fd, &wait );
  sel = select( c->fd+1, &wait, NULL, NULL, NULL );
  printf("wake up from select %d\n", sel );

  ret = ioctl( c->fd, SI_IOCTL_DMA_NEXT, &c->dma_status );

  if( ret < 0 ) {
    perror("ioctl dma_next");
  }
  if( c->dma_status.transferred != c->dma_config.total )
   printf("NEXT wakeup xfer %d bytes\n", c->dma_status.transferred );

  memcpy( data1, c->ptr, 8000000 );

  if( loop == 0 ) {
    memcpy( data2, c->ptr, 8000000 );
  } else if( memcmp( data1, data2, 8000000 ) != 0 ) {
      printf("memory mismatch %d\n", loop );
  }
  bzero(data1, 8000000);


  if( c->dma_status.status & SI_DMA_STATUS_DONE )
    printf("done %d\n", loop );
  else
    printf("timeout %d\n", loop );

  //print_dma_status(c);

  //print_data_len( c->ptr, c->dma_config.buflen, c->dma_config.total );
  //write_dma_data( c->ptr, c->dma_config.total );
  //print_mem_changes( c->ptr, c->dma_config.total/sizeof(short) );
  //  dma_unmap(c);
  }
  free(data1);
  free(data2);
}

/* look at data and guess how much data was transferred */

void write_dma_data( void *ptr, int total )
{
  FILE *fd;
  time_t tmm;
  char buf[256];

  time(&tmm);
  sprintf( buf, "dma_%ld.cam", tmm );

  if( !(fd = fopen( buf, "w+" ))) {
    printf("cant write dma.cam\n");
    return;
  }
  fwrite(ptr, total, 1, fd );
  fclose(fd);
  printf("wrote %d bytes to %s\n", total, buf );
}

void print_data_len( unsigned char *ptr, int buflen, int total )
{
  int nbufs, i, j, ix, ct;
  unsigned char targ;

  nbufs = total/ buflen;
  if( total & buflen )
   nbufs += 1;

  for( i=nbufs-1; i>=0; i-- ) {
    targ = (unsigned char)(i+1);
    for( j=buflen-1; j>=0 ; j-- ) {
      ix = i*buflen+j;
      if( ptr[ ix] != targ ) {
        printf( "total data guess %d, or 0x%x\n",  ix, ix );
        for( ct=ix-8; ct<ix+8; ct++ )
          printf( "0x%x 0x%x\n",  ct, ptr[ct] );
        return;
      }
    }
  }
  printf( "total data full %d\n",  total );
}

/*
   each buffer of the mmaped memory is written with the buffer number
   after it is allocated. This helps to tell what got written by the DMA.
   This printout shows that.
*/

void print_mem_changes( unsigned short *ptr, int nwords )
{
  int i, cr;
  unsigned short last;

  last = 0xcafe;
  cr = 0;
  for( i=0; i<nwords; i++ ) {
    if( ptr[i] != last ) {
      printf("0x%x:%x ", i*2, ptr[i]);
      cr++;
      if( cr>0 && (cr % 8) == 0 )
         printf("\n");
      last = ptr[i];
    }
  }
  if( (cr % 8) != 0 )
    printf("\n");

  printf("mem_changes %d nwords\n", nwords );
}


void expect_y( int fd )
{
  if( si_expect_yn(fd) != 1 )
    printf("ERROR expected yes got no from uart\n");
}


void print_readout( struct SI_CAMERA *c )
{
  int i;
  struct CFG_ENTRY *cfg;

  printf("Readout Parameters:\n");
  for( i=0; i< SI_READOUT_MAX; i++ ) {
    cfg = c->e_readout[i];
    if( cfg && cfg->name )
       print_cfg( cfg, c->readout[i] );
  }
}


void print_config( struct SI_CAMERA *c )
{
  int i;
  struct CFG_ENTRY *cfg;

  printf("Configuration Parameters:\n");
  for( i=0; i< SI_CONFIG_MAX; i++ ) {
     cfg = c->e_config[i];
     if( cfg && cfg->name )
       print_cfg( cfg, c->config[i] );
  }
}

void print_status( struct SI_CAMERA *c )
{
  struct CFG_ENTRY *cfg;
  int i;

  printf("Camera Status Words:\n" );
  for( i=0; i< SI_STATUS_MAX ; i++ ) {
    if( !(cfg = c->e_status[i]))
      break;
    print_cfg( cfg, c->status[i] );
  }
}


void print_cfg( struct CFG_ENTRY *cfg, int val )
{
  int ix;
  unsigned int mask;
  double value;
  char *units;

    switch( cfg->type ) {
      case CFG_TYPE_NOTUSED:
        break;
      case CFG_TYPE_INPUTD:
        if( cfg->u.iobox.units )
          units = cfg->u.iobox.units;
        else
          units = "";
        value = cfg->u.iobox.mult * val + cfg->u.iobox.offset;
        printf("%s %f %s val %d\n", cfg->name, value, units, val );
        break;
      case CFG_TYPE_DROPD:
        ix = val-cfg->u.drop.min;
        if( ix < cfg->u.drop.min || ix > cfg->u.drop.max )
          printf("%s val %d\n", cfg->name, val );
        else
          printf("%s %s val %d\n", cfg->name, cfg->u.drop.list[ix], val );
        break;
      case CFG_TYPE_BITF:
        mask = cfg->u.bitf.mask&val;
        ix = 0;
        while( !(mask & 1)) {
          ix++;
          mask = (mask>>1);
        }
        printf("%s %s val %d\n", cfg->name, cfg->u.bitf.list[ix], val );
        break;
    }
}

void print_dma_status( struct SI_CAMERA *c )
{
  printf("dma status\n");
  printf("transferred %d\n", c->dma_status.transferred );
  printf("status 0x%x\n",    c->dma_status.status );
  printf("cur %d\n",         c->dma_status.cur );
  printf("next %d\n",        c->dma_status.next );
}

void stop_dma( struct SI_CAMERA *c )
{

  if( ioctl( c->fd, SI_IOCTL_DMA_ABORT, &c->dma_status )<0 ) {
    perror("dma abort");
  }
}

void dma_unmap( struct SI_CAMERA *c )
{
  int nbufs;

  if( !c->dma_active )
    return;

  nbufs = c->dma_config.total / c->dma_config.buflen ;
  if( c->dma_config.total % c->dma_config.buflen )
    nbufs += 1;

  if(munmap( c->ptr, c->dma_config.buflen*nbufs ) )
    perror("failed to unmap");

  c->dma_active = 0;
}

void dma_mmap( struct SI_CAMERA *c )
{
  int nbufs;

  nbufs = c->dma_config.total / c->dma_config.buflen ;
  if( c->dma_config.total % c->dma_config.buflen )
    nbufs += 1;
  if(!(c->ptr = (unsigned short *)mmap( 0,
       c->dma_config.maxever,
       PROT_READ, MAP_SHARED, c->fd, 0))) {
      perror("mmap");
    } else
    c->dma_active = 1;
}

/* try to crash driver by failing to unmap */

void crash( struct SI_CAMERA *c )
{
  int repeat, ret;
  printf("starting crash\n");

  stop_dma( c );
  dma_unmap( c );
  repeat = 50000;
  while( repeat-- >0 ) {
    c->dma_config.maxever = 32*1024*1024;
    c->dma_config.total = 4096*4096*2;
    c->dma_config.buflen = 1024*1024; /* power of 2 makes it easy to mmap */
    c->dma_config.timeout = 5000;
//    c->dma_config.config = SI_DMA_CONFIG_WAKEUP_EACH;
    c->dma_config.config = SI_DMA_CONFIG_WAKEUP_ONEND;

    if( ioctl( c->fd, SI_IOCTL_DMA_INIT, &c->dma_config )<0 ){
      perror("dma init");
      continue;
    }
    dma_mmap(c);
    if( ioctl( c->fd, SI_IOCTL_DMA_START, &c->dma_status )<0 ){
      perror("dma start");
      continue;
    }
    si_send_command_yn(c->fd, 'C' );

    if( (ret = ioctl( c->fd, SI_IOCTL_DMA_NEXT, &c->dma_status ) )<0 )
      perror("dma next");

    print_mem_changes( c->ptr, c->dma_config.total/sizeof(short) );
    printf("trasferred %d total %d\n",
      c->dma_status.transferred, c->dma_config.total );
    stop_dma( c );
    dma_unmap( c );

    c->dma_config.maxever = 32*1024*1024;
    c->dma_config.total = 4096*2048*2;
    c->dma_config.buflen = 1024*1024; /* power of 2 makes it easy to mmap */
    c->dma_config.timeout = 1000;
//    c->dma_config.config = SI_DMA_CONFIG_WAKEUP_EACH;
    c->dma_config.config = SI_DMA_CONFIG_WAKEUP_ONEND;

    if( ioctl( c->fd, SI_IOCTL_DMA_INIT, &c->dma_config )<0 ){
      perror("dma init");
      continue;
    }
    dma_mmap(c);

    if( ioctl( c->fd, SI_IOCTL_DMA_START, &c->dma_status )<0 ){
      perror("dma start");
      continue;
    }
    si_send_command_yn(c->fd, 'C' );
    if( (ret = ioctl( c->fd, SI_IOCTL_DMA_NEXT, &c->dma_status ) )<0 )
      perror("dma next");

    print_mem_changes( c->ptr, c->dma_config.total/sizeof(short) );
    printf("trasferred %d total %d\n",
      c->dma_status.transferred, c->dma_config.total );
    stop_dma( c );
    dma_unmap( c );
  }
  printf("didnt crash\n");
}

int config_data[] = {
  850, //InstrumentModel
  100, //InstrumentSerialNumber
  0,   //HardwareRevision
  0,   //SerialRegisterPhasing
  1,   //SerialRegisterSplit
  2056,//SerialRegisterSize
  0,   //ParallelRegisterPhasing
  1,   //ParallelRegisterSplit
  2056,//ParallelRegisterSize
  20,  //ParallelShiftDelay
  3,   //NumberOfPorts
  20,  //ShutterCloseDelay
  0,   //CCDTemperatureOffset
  0,   //BackplateTemperatureOffset
  1732,//CCDTemperatureSetpoint
  0,   //DataU16Size
  0,   //MPPModeDisable
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
};

void set_config( struct SI_CAMERA *c )
{
  printf("sending default configuration parameters\n");
  memcpy( (int *)&c->config,  config_data, sizeof(int)*32 );
  si_send_command(c->fd, 'J'); // J    - Send Configuration Parameters
  si_send_n_ints( c->fd, 32, (int *)&c->config );
  expect_y( c->fd );
}

void print_help( void )
{
  printf("Comands\n" );
  printf(" A        -  Open Shutter returns Y/N\n");
  printf(" B        -  Close Shutter returns Y/N\n");
  printf(" I        -  Print camera status\n");
  printf(" H        -  Print readout parameters\n");
  printf(" L        -  Print camera config parameters\n");
  printf(" J        -  Send default camera config parameters\n");
  printf(" S        -  Cooler On\n");
  printf(" T        -  Cooler Off\n");
  printf(" O        -  Abort Readout\n");
  printf(" status   -  Print DMA status\n");
  printf(" sendfile -  Load dspfile into camera eeprom\n");
  printf("dma  'cmd'- configure dma for 8 million bytes, start the dma\n");
  printf("            send the 'cmd' command to start a test download,\n");
  printf("            wait for completion\n");
  printf("            cmd can be C test image\n");
  printf("                       D regular image with shuttered exposure\n");
  printf("                       E dark image with closed shutter\n");
  printf("                       Z tdi image\n");
  printf("stopdma  - issue stopdma for testing\n");
  printf("repeat   - number of times to do dma\n");
  printf("verbose  - toggle verbose flag\n");
  printf("change_readout  - changed readout params\n");
  printf("change_config  - changed config params\n");
  printf("image [cmd]  - take image based on readout params\n");
  printf("setfile_readout 'file'\n" );
  printf("send_readout\n");
}

void camera_image( struct SI_CAMERA *c, int cmd )
{
  int nbufs, ret;
  unsigned short *flip;
  int tot, serlen, parlen;

  if( cmd != 'C' && cmd != 'D' && cmd != 'E' && cmd != 'Z' ) {
    printf("camera_image needs C, D, E, Z type ? for help\n");
    return;
  }
  printf("starting camera_image %c\n", cmd );

  if( c->ptr )
    dma_unmap(c);

  serlen = c->readout[READOUT_SERLEN_IX];
  parlen = c->readout[READOUT_PARLEN_IX];

  tot = serlen*parlen*2*4; /* 2 bytes per short, 4 quadrants */
  c->dma_config.maxever = 32*1024*1024;
  c->dma_config.total = tot;
  c->dma_config.buflen = 1024*1024; /* power of 2 makes it easy to mmap */
  c->dma_config.timeout = 50000;
//  c->dma_config.config = SI_DMA_CONFIG_WAKEUP_EACH;
  c->dma_config.config = SI_DMA_CONFIG_WAKEUP_ONEND;

  if( ioctl( c->fd, SI_IOCTL_DMA_INIT, &c->dma_config )<0 ){
    perror("dma init");
    return;
  }
  nbufs = c->dma_config.total / c->dma_config.buflen ;
  if( c->dma_config.total % c->dma_config.buflen )
    nbufs += 1;

  if(!(c->ptr = (unsigned short *)mmap( 0, c->dma_config.maxever,
    PROT_READ, MAP_SHARED, c->fd, 0))) {
        perror("mmap");
        return;
  }
//      print_mem_changes( c->ptr, c->dma_config.total/sizeof(short) );

  c->dma_active = 1;

  if( ioctl( c->fd, SI_IOCTL_DMA_START, &c->dma_status )<0 ){
    perror("dma start");
    return;
  }

  bzero(&c->dma_status, sizeof(struct SI_DMA_STATUS));
  si_send_command_yn(c->fd, cmd );

/* wait for DMA done */

  printf("sent command %c, waiting\n", cmd );
  ret = ioctl( c->fd, SI_IOCTL_DMA_NEXT, &c->dma_status );

  if( ret < 0 ) {
    perror("ioctl dma_next");
  }
  if( c->dma_status.transferred != c->dma_config.total )
   printf("NEXT wakeup xfer %d bytes\n", c->dma_status.transferred );

  if( c->dma_status.status & SI_DMA_STATUS_DONE )
    printf("done\n" );
  else
    printf("timeout\n" );

  flip = (unsigned short *)malloc(4096*4096*2);
  bzero( flip, 4096*4096*2 );

  if( serlen < 2047 ) {
    si_camera_demux_gen( flip, c->ptr, 2048, serlen, parlen );
    write_dma_data( flip, 2048*2048*2 );
  } else {
    si_camera_demux_gen( flip, c->ptr, 4096, serlen, parlen );
    write_dma_data( flip, 4096*4096*2 );
  }
  free(flip);
}

void io_readout( struct SI_CAMERA *c )
{
  int i;
  struct CFG_ENTRY *cfg;

  printf("Changing Readout Parameters:\n");
  for( i=0; i< SI_READOUT_MAX; i++ ) {
    cfg = c->e_readout[i];
    if( cfg && cfg->name ) {
       change_cfg(c, cfg, c->readout[i], 'G' );
     }
  }
}

void io_config( struct SI_CAMERA *c )
{
  int i;
  struct CFG_ENTRY *cfg;

  printf("Changing Configuration Parameters:\n");
  for( i=0; i< SI_READOUT_MAX; i++ ) {
    cfg = c->e_config[i];
    if( cfg && cfg->name ) {
       change_cfg(c, cfg, c->config[i], 'K' );
     }
  }
}


int change_cfg( struct SI_CAMERA *c, struct CFG_ENTRY *cfg,
                int value, int cmd )
{
  char buf[256];
  int val, ex;
  int data[2];

  print_cfg( cfg, value );

  fprintf(stdout, "enter new val (ret to ignore): ");
  fflush(stdout);
  fgets( buf, 256, stdin);

  if( buf[0] == '\n' || buf[0] == '\r' )
    return value;

  val = atoi( buf );
  if( val == value ) {
    printf("no change, skipping\n");
    return value;
  }

  data[0] = cfg->index;
  data[1] = val;
  si_send_command(c->fd, cmd );
  si_send_n_ints( c->fd, 2, data );
  ex = si_expect_yn( c->fd );
  return ex;
}



int send_readout( struct SI_CAMERA *c )
{
  if (si_send_command(c->fd, 'F') < 0) // F    - Send Readout Parameters
    return -1;
  if (si_send_n_ints( c->fd, 32, (int *)&c->readout ) < 0)
    return -1;
  if (si_expect_yn( c->fd ) != 1)
    return -1;
  return 0;
}

void vmatest( struct SI_CAMERA *c )
{
  int nbufs, count;

  count = 1000;

  while(count-- > 0) {
    c->dma_config.maxever = 32*1024*1024;
    c->dma_config.total = ((count%2)?2:1)* 4000000*2;
    c->dma_config.buflen = 1024*1024; /* power of 2 makes it easy to mmap */
    c->dma_config.config = SI_DMA_CONFIG_WAKEUP_ONEND;
    if( ioctl( c->fd, SI_IOCTL_DMA_INIT, &c->dma_config )<0 ){
      perror("dma init");
      return;
    }
    nbufs = c->dma_config.total / c->dma_config.buflen ;
    if( c->dma_config.total % c->dma_config.buflen )
      nbufs += 1;


    if( !c->ptr ) {
      if(!(c->ptr = (unsigned short *)mmap( 0, c->dma_config.maxever,
        PROT_READ, MAP_PRIVATE, c->fd, 0))) {
          perror("mmap");
            return;
          }
    }
    print_mem_changes( c->ptr, c->dma_config.total/sizeof(short) );

    if(fork()==0) { /* child */
      sleep(2);
      exit(1);
    }
    sleep(1);
  }
}

void timeout_test( struct SI_CAMERA *c )
{
  int nbufs, count, serlen, parlen, tot, ret, repeat;

  repeat = 100;

  serlen = c->readout[READOUT_SERLEN_IX];
  parlen = c->readout[READOUT_PARLEN_IX];

  tot = serlen*parlen*2*4; /* 2 bytes per short, 4 quadrants */
  c->dma_config.maxever = 4096*4096*2;
  c->dma_config.total = tot;

  c->dma_config.buflen = 1024*1024; /* power of 2 makes it easy to mmap */
  c->dma_config.config = SI_DMA_CONFIG_WAKEUP_ONEND;
  c->dma_config.timeout = 10000;

  if( ioctl( c->fd, SI_IOCTL_DMA_INIT, &c->dma_config )<0 ){
    perror("dma init");
    return;
  }
  nbufs = c->dma_config.total / c->dma_config.buflen ;
  if( c->dma_config.total % c->dma_config.buflen )
    nbufs += 1;


  if( !c->ptr ) {
    if(!(c->ptr = (unsigned short *)mmap( 0, c->dma_config.maxever,
      PROT_READ, MAP_PRIVATE, c->fd, 0))) {
        perror("mmap");
          return;
        }
  }
  print_mem_changes( c->ptr, c->dma_config.total/sizeof(short) );

  while(repeat-- > 0) {
    bzero(&c->dma_status, sizeof(struct SI_DMA_STATUS));
    if( ioctl( c->fd, SI_IOCTL_DMA_START, &c->dma_status )<0 ){
      perror("dma start");
      continue;
    }

    si_send_command_yn(c->fd, 'D' );


    count = 0;
    do {
      ret = ioctl( c->fd, SI_IOCTL_DMA_NEXT, &c->dma_status );
      printf(" wake ret:%d stat 0x%x xfer %d tot %d\n",
        ret, c->dma_status.status,
        c->dma_status.transferred, c->dma_config.total);


      if( !c->dma_status.status &&
           (c->dma_status.transferred ==  c->dma_config.total))
      if( count++ > 7 )
        printf("yup\n");
    } while( ret != 0);
    printf(" done %d\n", count );
    sleep(1);
  }

}




