/*****************************************************************************\
 *  Copyright (c) 2017 Jim Garlick All rights reserved.
 *
 *  This file is part of si3097.
 *  For details, see https://github.com/garlick/si3097.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  sbig-util is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>
#include <stdarg.h>

#include "si3097.h"

const char *device = "/dev/si3097a";

void die (const char *fmt, ...)
{
    va_list ap;

    va_start (ap, fmt);
    vfprintf (stderr, fmt, ap);
    va_end (ap);
    exit (1);
}

/* Open the device, enable maximum driver printk verbosity, configure UART,
 * and clear serial buffer.  Return open file descriptor.
 * Exit with error message on stderr if anything goes wrong.
 */
int initialize (const char *path)
{
    int verbose = SI_VERBOSE_SERIAL | SI_VERBOSE_DMA;
    struct SI_SERIAL_PARAM serial;
    int fd;

    if ((fd = open (device, O_RDWR)) < 0)
        die ("%s: %s\n", device, strerror (errno));

    if (ioctl (fd, SI_IOCTL_VERBOSE, &verbose) < 0)
        die ("ioctl SI_IOCTL_VERBOSE: %s\n", strerror (errno));

    memset (&serial, 0, sizeof (serial));
    serial.baud = 57600;
    serial.parity = 0; // none
    serial.bits = 8;
    serial.stopbits = 1;
    serial.buffersize = 4096;
    serial.flags = SI_SERIAL_FLAGS_BLOCK;
    serial.timeout = 500; // ms
    serial.fifotrigger = 8;
    if (ioctl (fd, SI_IOCTL_SET_SERIAL, &serial) < 0)
        die ("ioctl SI_IOCTL_SET_SERIAL: %s\n", strerror (errno));

    if (ioctl (fd, SI_IOCTL_SERIAL_CLEAR, &serial) < 0)
        die ("ioctl SI_IOCTL_SERIAL_CLEAR: %s\n", strerror (errno));

    return fd;
}

/* Send single character command, then read it when it is echoed back.
 * Exit with error message on stderr if anything goes wrong.
 */
void send_command (int fd, char cmd)
{
    int rc;
    char c;

    rc = write (fd, &cmd, 1);
    if (rc < 0)
        die ("write[%c]: %s\n", cmd, strerror (errno));
    if (rc != 1)
        die ("write[%c]: unexected return value %d\n", cmd, rc);
    rc = read (fd, &c, 1);
    if (rc < 0)
        die ("read[%c]: %s\n", cmd, strerror (errno));
    if (rc == 0)
        die ("read[%c]: echo timed out\n", cmd);
    if (c != cmd)
        die ("read[%c]: echo garbled: %c\n", cmd, c);
}

/* Receive 'Y' character as acknowledgement to command.
 * 'N' is a negative acknowledgement.
 * Exit with error message on stderr if anything goes wrong, including NAK.
 */
void recv_acknak (int fd)
{
    int rc;
    char c;

    rc = read (fd, &c, 1);
    if (rc < 0)
        die ("read: %s\n", strerror (errno));
    if (rc == 0)
        die ("read: command terminator timed out\n");
    if (c != 'Y')
        die ("read: expected 'Y' got %c\n", c);
}

/* Send 'cmd', then receive 'count' uint32_t's, then get ACK/NAK
 * Exit with error message on stderr if anything goes wrong.
 * Display the raw block of integers.
 */
void dump_from_camera (int fd, char cmd, int count)
{
    int i, rc;
    int len = (count * sizeof (uint32_t));
    uint32_t *buf;

    if (!(buf = malloc (len)))
        die ("out of memory");

    send_command (fd, cmd);
    rc = read (fd, buf, len);
    if (rc < 0)
        die ("read: %s\n", strerror (errno));
    if (rc < len)
        die ("read: timed out reading %d 32-bit integers\n", count);
    recv_acknak (fd);

    for (i = 0; i < count; i++)
        printf ("[%.02d] = %u\n", i, ntohl (buf[i]));

    free (buf);
}

void usage (void)
{
        fprintf (stderr,
"Usage: si-dump CMD\n"
"    L - configuration parameters\n"
"    H - readout parameters\n"
"    I - camera status\n");
        exit (1);
}

int main (int argc, char **argv)
{
    int fd;

    if (argc != 2)
        usage ();

    fd = initialize (device);
    switch (argv[1][0]) {
        case 'L':
            printf ("Configuration parameters:\n");
            dump_from_camera (fd, 'L', 32);
            break;
        case 'H':
            printf ("Readout Parameters:\n");
            dump_from_camera (fd, 'H', 32);
            break;
        case 'I':
            printf ("Camera status:\n");
            dump_from_camera (fd, 'I', 16);
            break;
        default:
            usage ();
    }
    close (fd);

    exit (0);
}

/*
 *  vi:tabstop=4 shiftwidth=4 expandtab
 */
